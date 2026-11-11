#include "sys/platform.h"
#include "framework/Common.h"
#include "idlib/Str.h"

#ifdef D3_RTGL

#include "renderer/rtgl/rtgl_internal.h"

// ===========================================================================
// P2 - Geometry submission (+ camera) and P2.5 debug instrumentation
//
// Injection: R_IssueRenderCommands() (RenderSystem.cpp) calls
// RTGL_SubmitCommands( frameData->cmdHead ) in RT mode before the command
// chain is cleared. We walk RC_DRAW_VIEW commands: the first non-subview view
// supplies the camera, every view contributes viewDef->drawSurfs as RTGL1
// mesh primitives.
//
// Units: geometry is submitted in RAW Doom engine units. RTGL1 does NOT scale
// runtime vertices by worldScale (GLTF-only), so P4 lights use the same raw
// units. worldScale documents the physical unit for intensity semantics.
//
// ID stability: static world surfaces derive uniqueObjectID from their
// srfTriangles_t pointer (stable for the map's life). Dynamic entities derive
// it from renderWorld entityDef->index plus a per-frame ordinal. A fully
// stable per-surface ID for animated MD5 models and the first-person weapon
// symmetric-history-rejection are P5/P6 refinements (see milestone plan).
// ===========================================================================

static idList<RgPrimitiveVertex> s_verts;
static idList<uint32_t>          s_indices;
static idList<uint64_t>          s_objectIDs;       // P2.5 rt_dbg_validateIDs
static bool                      s_submittedThisFrame = false;

static uint32_t RTGL_HashPtr( const void *p ) {
    uint64_t v = (uint64_t)(uintptr_t)p;
    v ^= v >> 33; v *= 0xff51afd7ed558ccdULL;
    v ^= v >> 33; v *= 0xc4ceb9fe1a85ec53ULL;
    v ^= v >> 33;
    return (uint32_t)v;
}

static uint32_t RTGL_HashStr( const char *s ) {
    uint32_t h = 2166136261u;
    if ( s ) { for ( ; *s; s++ ) { h ^= (uint8_t)(*s); h *= 16777619u; } }
    return h;
}

RgColor4DPacked32 RTGL_DebugColor( int mode, uint64_t objectID, int ordinal,
                                    int entityIndex, const char *matName, bool isStatic ) {
    uint32_t h;
    switch ( mode ) {
        case 1:  h = (uint32_t)( objectID ^ (objectID >> 32) ); break;
        case 2:  return isStatic ? RTGL_PackColorF( 0.20f, 0.90f, 0.25f, 1.0f )
                                 : RTGL_PackColorF( 0.95f, 0.25f, 0.20f, 1.0f );
        case 3:  h = (uint32_t)ordinal * 2654435761u; break;
        case 4:  h = (uint32_t)(entityIndex + 1) * 2654435761u; break;
        case 5:  h = RTGL_HashStr( matName ); break;
        default: return RTGL_White();
    }
    float r = (float)( (h      ) & 0xFF ) / 255.0f * 0.8f + 0.2f;
    float g = (float)( (h >>  8) & 0xFF ) / 255.0f * 0.8f + 0.2f;
    float b = (float)( (h >> 16) & 0xFF ) / 255.0f * 0.8f + 0.2f;
    return RTGL_PackColorF( r, g, b, 1.0f );
}

void RTGL_SubmitBeginFrame( void ) {
    s_verts.Clear();
    s_indices.Clear();
    s_objectIDs.Clear();
    s_submittedThisFrame = false;
}

static void RTGL_UploadCamera( const viewDef_t *view ) {
    if ( !rg.rgUploadCamera ) {
        return;
    }
    const renderView_t &rv = view->renderView;

    RgCameraInfo cam = {};
    cam.sType = RG_STRUCTURE_TYPE_CAMERA_INFO;
    cam.pNext = NULL;
    cam.flags = 0;
    cam.position = RTGL_V3( rv.vieworg.x, rv.vieworg.y, rv.vieworg.z );
    // RTGL1 builds forward = cross( up, right ). For Doom's right-handed
    // viewaxis (axis[0]=forward, axis[1]=left, axis[2]=up),
    // cross( axis[2], -axis[1] ) = axis[0]. So up = axis[2], right = -axis[1].
    cam.up    = RTGL_V3(  rv.viewaxis[2].x, rv.viewaxis[2].y, rv.viewaxis[2].z );
    cam.right = RTGL_V3( -rv.viewaxis[1].x, -rv.viewaxis[1].y, -rv.viewaxis[1].z );
    float fovY = rv.fov_y;
    if ( fovY <= 0.0f ) { fovY = 74.0f; }
    cam.fovYRadians = fovY * ( 3.14159265358979323846f / 180.0f );
    float w = ( g_rtglWidth  > 0 ) ? (float)g_rtglWidth  : 640.0f;
    float h = ( g_rtglHeight > 0 ) ? (float)g_rtglHeight : 480.0f;
    cam.aspect = w / h;
    cam.cameraNear = r_znear.GetFloat();
    if ( cam.cameraNear <= 0.0f ) { cam.cameraNear = 3.0f; }
    cam.cameraFar = 10000.0f;
    cam.pView = NULL;

    RgResult r = rg.rgUploadCamera( &cam );
    if ( r != RG_RESULT_SUCCESS ) {
        common->Warning( "[RTGL] rgUploadCamera failed (%s)", RTGL_ResultString( r ) );
    }
}

static void RTGL_SubmitSurf( const drawSurf_t *surf, int ordinal ) {
    g_rtglStats.drawSurfsSeen++;

    if ( !surf || !surf->geo || !surf->material ) {
        g_rtglStats.skippedSurfs++; g_rtglStats.skippedNoGeo++; return;
    }
    const srfTriangles_t *tri = surf->geo;
    if ( tri->numVerts <= 0 || tri->numIndexes <= 0 || !tri->verts || !tri->indexes ) {
        g_rtglStats.skippedSurfs++; g_rtglStats.skippedNoVerts++; return;
    }
    float sort = surf->material->GetSort();
    if ( sort < 0.0f || sort >= 100.0f ) {  // SS_SUBVIEW/SS_GUI (<0), SS_POST_PROCESS (=100)
        g_rtglStats.skippedSurfs++; return;
    }

    // P3: decal skip mode (rt_decal_mode == 2): guaranteed removal of the
    // transparent/bright decal quads, as a clean fallback / comparison.
    if ( rt_decal_mode.GetInteger() == 2 && RTGL_IsDecal( surf->material ) ) {
        g_rtglStats.skippedSurfs++;
        return;
    }

    // --- stable object id ---
    const viewEntity_t *space = surf->space;
    bool isStatic = ( space == NULL ) || ( space->entityDef == NULL );
    int  entityIndex = isStatic ? -1 : space->entityDef->index;
    uint64_t objectID;
    if ( isStatic ) {
        objectID = 1ULL + (uint64_t)RTGL_HashPtr( tri );
    } else {
        objectID = 0x100000000ULL + ( (uint64_t)( entityIndex + 1 ) << 32 ) + (uint64_t)ordinal;
    }

    // P2.5: uniqueness check across this frame
    if ( rt_dbg_validateIDs.GetBool() ) {
        for ( int k = 0; k < s_objectIDs.Num(); k++ ) {
            if ( s_objectIDs[k] == objectID ) { g_rtglStats.idCollisions++; break; }
        }
        s_objectIDs.Append( objectID );
    }

    // --- idDrawVert -> RgPrimitiveVertex ---
    int nv = tri->numVerts;
    s_verts.SetNum( nv );
    memset( s_verts.Ptr(), 0, nv * sizeof( RgPrimitiveVertex ) );
    RgColor4DPacked32 vcolor = RTGL_White();
    for ( int i = 0; i < nv; i++ ) {
        const idDrawVert &src = tri->verts[i];
        RgPrimitiveVertex &dst = s_verts[i];
        dst.position[0]  = src.xyz.x;
        dst.position[1]  = src.xyz.y;
        dst.position[2]  = src.xyz.z;
        dst.normalPacked = RTGL_PackNormal( src.normal.x, src.normal.y, src.normal.z );
        dst.texCoord[0]  = src.st.x;
        dst.texCoord[1]  = src.st.y;
        dst.color        = vcolor;
        dst._pad0        = 0;
    }

    // --- glIndex_t (int) -> uint32_t ---
    int ni = tri->numIndexes;
    s_indices.SetNum( ni );
    for ( int i = 0; i < ni; i++ ) {
        s_indices[i] = (uint32_t)tri->indexes[i];
    }

    // --- P3 material classification + primary albedo ---
    RgMeshPrimitiveFlags primFlags = 0;
    float emissive = 0.0f;
    RTGL_ClassifyMaterial( surf->material, &primFlags, &emissive );
    primFlags |= RG_MESH_PRIMITIVE_FORCE_EXACT_NORMALS;
    const char *texName = RTGL_PrimaryTexture( surf->material );

    int colorMode = rt_dbg_colorMode.GetInteger();
    RgColor4DPacked32 primColor = ( colorMode != 0 )
        ? RTGL_DebugColor( colorMode, objectID, ordinal, entityIndex,
                           surf->material->GetName(), isStatic )
        : RTGL_White();

    // --- mesh (one per drawSurf) ---
    RgMeshInfo mesh = {};
    mesh.sType = RG_STRUCTURE_TYPE_MESH_INFO;
    mesh.pNext = NULL;
    mesh.flags = 0;
    mesh.uniqueObjectID = objectID;
    mesh.pMeshName = NULL;
    RTGL_IdentityTransform( &mesh.transform );
    if ( space ) {
        RTGL_ModelMatrixToTransform( space->modelMatrix, &mesh.transform );
    }
    mesh.isExportable = 0;
    mesh.animationTime = 0.0f;
    mesh.localLightsIntensity = 1.0f;

    RgMeshPrimitiveInfo prim = {};
    prim.sType = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_INFO;
    prim.pNext = NULL;
    prim.flags = primFlags;
    prim.primitiveIndexInMesh = 0;
    prim.pVertices = s_verts.Ptr();
    prim.vertexCount = (uint32_t)nv;
    prim.pIndices = s_indices.Ptr();
    prim.indexCount = (uint32_t)ni;
    prim.pTextureName = texName;
    prim.textureFrame = 0;
    prim.color = primColor;
    prim.emissive = emissive;
    prim.classicLight = 1.0f;

    RgResult r = rg.rgUploadMeshPrimitive( &mesh, &prim );
    if ( r == RG_RESULT_SUCCESS || r == RG_RESULT_SUCCESS_FOUND_MESH ) {
        g_rtglStats.meshes++;
        g_rtglStats.primitives++;
        g_rtglStats.vertices += nv;
        g_rtglStats.indices += ni;
        if ( isStatic ) { g_rtglStats.staticPrimitives++; }
        else            { g_rtglStats.dynamicPrimitives++; }
    } else {
        g_rtglStats.skippedSurfs++;
    }
}

// P2.5 + P4: throttled per-frame submission stats. Called from
// RTGL_SubmitFrameCommands() AFTER RTGL_SubmitLights() so light counters are
// included. (Not static: exposed via rtgl_internal.h.)
void RTGL_ReportStats( void ) {
    int interval = rt_dbg_stats.GetInteger();
    if ( interval <= 0 ) { return; }
    if ( ( g_rtglFrameIndex % interval ) != 0 ) { return; }
    common->Printf(
        "[RTGL] frame %d | geo drawSurfs=%d meshes=%d prims=%d verts=%d idx=%d "
        "static=%d dynamic=%d tex=%d skipSurf=%d(noGeo=%d noVerts=%d) idColl=%d | "
        "lights seen=%d up=%d (sph=%d spot=%d dir=%d) skip=%d(fog=%d blend=%d noSh=%d dup=%d fail=%d)\n",
        g_rtglFrameIndex, g_rtglStats.drawSurfsSeen, g_rtglStats.meshes,
        g_rtglStats.primitives, g_rtglStats.vertices, g_rtglStats.indices,
        g_rtglStats.staticPrimitives, g_rtglStats.dynamicPrimitives,
        g_rtglStats.texturesProvided, g_rtglStats.skippedSurfs,
        g_rtglStats.skippedNoGeo, g_rtglStats.skippedNoVerts,
        g_rtglStats.idCollisions,
        g_rtglStats.lightsSeen, g_rtglStats.lightsUploaded,
        g_rtglStats.sphereLights, g_rtglStats.spotLights,
        g_rtglStats.directionalLights, g_rtglStats.skippedLights,
        g_rtglStats.skippedFog, g_rtglStats.skippedBlend,
        g_rtglStats.skippedNoShadow, g_rtglStats.lightDuplicates,
        g_rtglStats.failedLightUploads );
}

void RTGL_SubmitCommands( const void *cmdHead ) {
    if ( s_submittedThisFrame || !RTGL_FrameActive() ) {
        return;
    }
    const emptyCommand_t *cmd = (const emptyCommand_t *)cmdHead;
    if ( !cmd ) {
        return;
    }

    // Camera: first non-subview RC_DRAW_VIEW.
    const viewDef_t *camView = NULL;
    for ( const emptyCommand_t *c = cmd; c; c = (const emptyCommand_t *)c->next ) {
        if ( c->commandId == RC_DRAW_VIEW ) {
            const drawSurfsCommand_t *dv = (const drawSurfsCommand_t *)c;
            if ( dv->viewDef && !dv->viewDef->isSubview && !dv->viewDef->isMirror ) {
                camView = dv->viewDef;
                break;
            }
        }
    }
    if ( camView ) {
        RTGL_UploadCamera( camView );
        g_rtglCameraUploaded = true;
    }

    // Geometry from every RC_DRAW_VIEW.
    int budget = rt_dbg_maxSurfs.GetInteger();
    int ordinal = 0;
    for ( const emptyCommand_t *c = cmd; c; c = (const emptyCommand_t *)c->next ) {
        if ( c->commandId != RC_DRAW_VIEW ) { continue; }
        const drawSurfsCommand_t *dv = (const drawSurfsCommand_t *)c;
        const viewDef_t *v = dv->viewDef;
        if ( !v ) { continue; }
        if ( rt_dbg_skipSubViews.GetBool() &&
             ( v->isSubview || v->isMirror || v->isXraySubview ) ) {
            continue;
        }
        for ( int i = 0; i < v->numDrawSurfs; i++ ) {
            if ( budget > 0 && g_rtglStats.primitives >= budget ) { break; }
            RTGL_SubmitSurf( v->drawSurfs[i], ordinal++ );
        }
    }

    s_submittedThisFrame = true;
    // NOTE: RTGL_ReportStats() is now called by RTGL_SubmitFrameCommands()
    // after RTGL_SubmitLights(), so P4 light counters are included.
}

#endif // D3_RTGL
