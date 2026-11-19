#include "sys/platform.h"
#include "framework/Common.h"
#include "framework/Session.h"
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
static bool                      s_dumpSurfsThisFrame = false;

static idList<rtglDeferredPostSurf_t> g_deferredPostSurfs;

// Per-frame payload handed to RTGL1 through
// RgDrawFrameCustomPassParams.pUserData. Filled by RTGL_BuildHeatHazePayload()
// from the intercepted heat-haze quads; read by RTGL_EndFrame(). Kept as a
// plain POD so the DLL can reinterpret it without any ABI change.
static RgCustomPassHeatHazeFrame g_heatHazePayload = {};

RgCustomPassHeatHazeFrame * RTGL_GetHeatHazePayload( void ) {
    return &g_heatHazePayload;
}

// DIAG: decode RgMeshPrimitiveFlags into a compact readable string so a full
// surface dump can immediately reveal SKY_VISIBILITY / SKY / WATER / GLASS etc.
static void RTGL_FormatPrimFlags( RgMeshPrimitiveFlags f, char *out, int outSize ) {
    struct FlagName { RgMeshPrimitiveFlags bit; const char *name; };
    static const FlagName kFlags[] = {
        { RG_MESH_PRIMITIVE_ALPHA_TESTED,           "ALPHA_TESTED" },
        { RG_MESH_PRIMITIVE_TRANSLUCENT,            "TRANSLUCENT" },
        { RG_MESH_PRIMITIVE_SKY,                    "SKY" },
        { RG_MESH_PRIMITIVE_MIRROR,                 "MIRROR" },
        { RG_MESH_PRIMITIVE_GLASS,                  "GLASS" },
        { RG_MESH_PRIMITIVE_WATER,                  "WATER" },
        { RG_MESH_PRIMITIVE_DONT_GENERATE_NORMALS,  "DONT_GEN_NRM" },
        { RG_MESH_PRIMITIVE_FORCE_EXACT_NORMALS,    "EXACT_NRM" },
        { RG_MESH_PRIMITIVE_MIRROR_IF_SMOOTH,       "MIRROR_IF_SMOOTH" },
        { RG_MESH_PRIMITIVE_GLASS_IF_SMOOTH,        "GLASS_IF_SMOOTH" },
        { RG_MESH_PRIMITIVE_IGNORE_REFRACT_AFTER,   "IGNORE_REFRACT_AFTER" },
        { RG_MESH_PRIMITIVE_ACID,                   "ACID" },
        { RG_MESH_PRIMITIVE_THIN_MEDIA,             "THIN_MEDIA" },
        { RG_MESH_PRIMITIVE_SKY_VISIBILITY,         "SKY_VISIBILITY" },
        { RG_MESH_PRIMITIVE_DECAL,                  "DECAL" },
        { RG_MESH_PRIMITIVE_EXPORT_INVERT_NORMALS,  "INVERT_NRM" },
        { RG_MESH_PRIMITIVE_NO_SHADOW,              "NO_SHADOW" },
        { RG_MESH_PRIMITIVE_NO_MOTION_VECTORS,      "NO_MOTION" },
    };
    if ( outSize <= 0 ) { return; }
    out[0] = 0;
    int len = 0;
    for ( int i = 0; i < (int)( sizeof( kFlags ) / sizeof( kFlags[0] ) ); i++ ) {
        if ( ( f & kFlags[i].bit ) == 0 ) { continue; }
        int n = idStr::snPrintf( out + len, outSize - len, "%s%s",
                                 len > 0 ? "|" : "", kFlags[i].name );
        if ( n < 0 || n >= outSize - len ) { break; }
        len += n;
    }
    if ( len == 0 ) {
        idStr::snPrintf( out, outSize, "NONE" );
    }
}

void RTGL_ClearDeferredPostSurfs( void ) {
    g_deferredPostSurfs.SetNum( 0, false );
}

bool RTGL_HasDeferredPostSurfs( void ) {
    return g_deferredPostSurfs.Num() > 0;
}

int RTGL_GetNumDeferredPostSurfs( void ) {
    return g_deferredPostSurfs.Num();
}

const rtglDeferredPostSurf_t * RTGL_GetDeferredPostSurf( int index ) {
    if ( index >= 0 && index < g_deferredPostSurfs.Num() ) {
        return &g_deferredPostSurfs[index];
    }
    return NULL;
}

static void RTGL_CollectDeferredPostSurf( const drawSurf_t *surf, const float *sregs, int ordinal ) {
    if ( !surf || !surf->material || !surf->geo ) {
        return;
    }
    rtglDeferredPostSurf_t defSurf;
    defSurf.material = surf->material;
    defSurf.geo = surf->geo;
    defSurf.space = surf->space;
    defSurf.sort = surf->sort;
    defSurf.numRegisters = 0;
    if ( sregs && surf->material ) {
        int nReg = surf->material->GetNumRegisters();
        if ( nReg > MAX_RTGL_DEFERRED_REGISTERS ) nReg = MAX_RTGL_DEFERRED_REGISTERS;
        defSurf.numRegisters = nReg;
        for ( int r = 0; r < nReg; r++ ) {
            defSurf.registers[r] = sregs[r];
        }
    }
    const char *nrm = RTGL_NormalMapName( surf->material );
    const char *msk = RTGL_HeatHazeMaskName( surf->material );
    defSurf.normalMapName = nrm ? nrm : "";
    defSurf.maskName = msk ? msk : "";
    defSurf.scroll[0] = 0.0f;
    defSurf.scroll[1] = 0.0f;
    defSurf.deformMag = 1.0f;

    for ( int i = 0; i < surf->material->GetNumStages(); i++ ) {
        const shaderStage_t *st = surf->material->GetStage( i );
        if ( st && st->newStage && sregs ) {
            if ( st->newStage->numVertexParms > 0 ) {
                defSurf.scroll[0] = sregs[ st->newStage->vertexParms[0][0] ];
                defSurf.scroll[1] = sregs[ st->newStage->vertexParms[0][1] ];
            }
            if ( st->newStage->numVertexParms > 1 ) {
                defSurf.deformMag = sregs[ st->newStage->vertexParms[1][0] ];
            }
            break;
        }
    }

    g_deferredPostSurfs.Append( defSurf );
}

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
    RTGL_ClearDeferredPostSurfs();
    memset( &g_heatHazePayload, 0, sizeof( g_heatHazePayload ) );
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

static void RTGL_SubmitSurf( const drawSurf_t *surf, int ordinal,
                             const idVec3 &camPos ) {
    g_rtglStats.drawSurfsSeen++;

    if ( !surf || !surf->geo || !surf->material ) {
        g_rtglStats.skippedSurfs++; g_rtglStats.skippedNoGeo++; return;
    }

    if ( s_dumpSurfsThisFrame ) {
        const char *filter = rt_dbg_dumpMatFilter.GetString();
        const char *mName = surf->material ? surf->material->GetName() : "<null>";
        bool match = true;
        if ( filter && filter[0] != '\0' ) {
            match = ( idStr::FindText( mName, filter, false ) >= 0 );
        } else {
            // DIAG: FULL dump (no default filter). The grey-board source is a
            // sky/ray-miss region whose brush is MC_OPAQUE + sort=0 + DFRM_NONE,
            // which the old default filter excluded. Dump EVERY surface so the
            // culprit can be identified by its classification tags below.
            match = true;
        }
        if ( match ) {
            const viewEntity_t *sp = surf->space;
            const char *modelName = ( sp && sp->entityDef && sp->entityDef->parms.hModel )
                                    ? sp->entityDef->parms.hModel->Name() : "world";
            const bool dSky  = RTGL_IsSkySurface( surf->material );
            const bool dHaze = RTGL_IsHeatHazeOrPostProcess( surf->material );
            const bool dPost = ( surf->material->GetSort() >= SS_POST_PROCESS );
            common->Printf( "[RTGL_SURF_DUMP] #%03d model='%s' mat='%s' matPtr=0x%p cov=%d sort=%.1f deform=%d stages=%d verts=%d idx=%d [sky=%d haze=%d post=%d]\n",
                            ordinal, modelName, mName, (void*)surf->material,
                            (int)surf->material->Coverage(), surf->material->GetSort(),
                            (int)surf->material->Deform(), surf->material->GetNumStages(),
                            surf->geo ? surf->geo->numVerts : 0, surf->geo ? surf->geo->numIndexes : 0,
                            dSky ? 1 : 0, dHaze ? 1 : 0, dPost ? 1 : 0 );
            for ( int s = 0; s < surf->material->GetNumStages(); s++ ) {
                const shaderStage_t *st = surf->material->GetStage( s );
                if ( !st ) continue;
                const char *imgName = ( st->texture.image ) ? st->texture.image->imgName.c_str() : "<none>";
                common->Printf( "    stage %d: light=%d srcB=0x%x dstB=0x%x tex='%s'\n",
                                s, st->lighting, ( st->drawStateBits & GLS_SRCBLEND_BITS ),
                                ( st->drawStateBits & GLS_DSTBLEND_BITS ), imgName );
                if ( st->newStage ) {
                    for ( int fp = 0; fp < st->newStage->numFragmentProgramImages; fp++ ) {
                        const idImage *fpi = st->newStage->fragmentProgramImages[fp];
                        common->Printf( "        fragmentMap %d: '%s'\n", fp, fpi ? fpi->imgName.c_str() : "<null>" );
                    }
                }
            }
        }
    }
    const srfTriangles_t *tri = surf->geo;
    if ( tri->numVerts <= 0 || tri->numIndexes <= 0 || !tri->verts || !tri->indexes ) {
        g_rtglStats.skippedSurfs++; g_rtglStats.skippedNoVerts++; return;
    }
    const float *sregs = surf->shaderRegisters;
    if ( !sregs && surf->material ) {
        sregs = surf->material->ConstantRegisters();
    }
    float sort = surf->material->GetSort();
    // Post-process / heat-haze surfaces carry NO diffuse texture (stage 0
    // tex='<none>'); if uploaded to RTGL1 world rasterizer or BLAS they fall
    // back to the white 1x1 texture and become a solid grey quad (the classic
    // func_static_52844 / textures/sfx/vp1 artifact). Drop them UNCONDITIONALLY.
    // Gate on the engine semantic (sort >= SS_POST_PROCESS) as a hard floor in
    // addition to the shader-semantic detector, so no single detector can leak.
    const bool isPostProcess = ( sort >= SS_POST_PROCESS );
    const bool isHeatHaze = RTGL_IsHeatHazeOrPostProcess( surf->material );
    if ( isPostProcess || isHeatHaze ) {
        if ( s_dumpSurfsThisFrame ) {
            common->Printf( "[RTGL_HAZE_DROP] #%03d mat='%s' sort=%.1f deform=%d "
                            "byPostProcess=%d byDetector=%d deferred=%d -> NOT uploaded\n",
                            ordinal, surf->material->GetName(), sort,
                            (int)surf->material->Deform(),
                            isPostProcess ? 1 : 0, isHeatHaze ? 1 : 0,
                            rt_raster_distortion.GetBool() ? 1 : 0 );
        }
        if ( rt_raster_distortion.GetBool() ) {
            RTGL_CollectDeferredPostSurf( surf, sregs, ordinal );
        }
        g_rtglStats.skippedSurfs++;
        return;
    }
    if ( sort < 0.0f || sort >= SS_POST_PROCESS ) {
        g_rtglStats.skippedSurfs++; return;
    }
    // Carrier surfaces such as textures/common/entityGui have 0 shader stages
    // (IsDrawn() is true only because entityGui != 0) and sort == SS_MEDIUM.
    // In the GL backend they execute 0 stage passes; uploading them here with
    // texName == NULL would draw an opaque white rectangle over the GUI panel.
    if ( surf->material->GetNumStages() <= 0 ) {
        g_rtglStats.skippedSurfs++; return;
    }

    // Drop 2004 legacy polygonal flashlight beam quads (`models/items/flashlight/beam1`
    // and `beam2`). In Doom 3 these were flat billboard quads designed for two-pass
    // destination-alpha masking with dust.tga. In RTGL1 path tracing they render as
    // giant opaque/glowing white sheets covering half the screen, while real 3D
    // spotlight volumetrics and cookie lighting replace them.
    if ( idStr::FindText( surf->material->GetName(), "flashlight/beam", false ) >= 0 ) {
        g_rtglStats.skippedSurfs++;
        return;
    }

    // When refraction is disabled via cvar, drop heat haze / post-process surfaces
    // to prevent fallback to untextured gray quads.
    if ( !rt_refr_enable.GetBool() && RTGL_IsHeatHazeOrPostProcess( surf->material ) ) {
        g_rtglStats.skippedSurfs++;
        return;
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
    int ni = tri->numIndexes;
    s_verts.SetNum( nv );
    memset( s_verts.Ptr(), 0, nv * sizeof( RgPrimitiveVertex ) );
    // --- Align uploaded vertex normals to RTGL1's geometric hemisphere ----
    // HitInfo.inl flips h.normal / exactNormal together to face the viewer and
    // then calls sanitizeNormal(); BOTH assume the smoothed vertex normal agrees
    // with the geometric face normal cross(e1,e2). Doom3 uses clockwise winding
    // and inward-facing vertex normals (see R_ReverseTriangles in tr_trisurf.cpp,
    // which negates verts[].normal when flipping a two-sided surface), so
    // idDrawVert.normal is systematically OPPOSITE to RTGL1's cross(e1,e2).
    // FORCE_EXACT_NORMALS ignored the uploaded normal (correct lighting); once we
    // enabled smoothed vertex normals (DONT_GENERATE_NORMALS), they landed in the
    // wrong hemisphere -> sanitizeNormal bent them into arbitrary directions
    // (the whole normal field looked "rotated") and dot(n,light)<0 -> black.
    //
    // Do NOT hardcode a negation: derive the sign from the SAME positions we are
    // about to upload, so this is correct regardless of winding convention. Sum
    // dot(faceNormal, sum(vertexNormals)) over all triangles; if negative, flip
    // every uploaded normal into the geometric hemisphere. This keeps smoothness
    // (interpolated vertex normals) while guaranteeing agreement with cross().
    // Computed in object space: RTGL1 applies the same orthonormal instance
    // rotation to both positions and normals, which preserves the dot sign.
    float align = 0.0f;
    for ( int t = 0; t + 2 < ni; t += 3 ) {
        const idDrawVert &A = tri->verts[ tri->indexes[ t + 0 ] ];
        const idDrawVert &B = tri->verts[ tri->indexes[ t + 1 ] ];
        const idDrawVert &C = tri->verts[ tri->indexes[ t + 2 ] ];
        float e1x = B.xyz.x - A.xyz.x, e1y = B.xyz.y - A.xyz.y, e1z = B.xyz.z - A.xyz.z;
        float e2x = C.xyz.x - A.xyz.x, e2y = C.xyz.y - A.xyz.y, e2z = C.xyz.z - A.xyz.z;
        float gx = e1y * e2z - e1z * e2y;
        float gy = e1z * e2x - e1x * e2z;
        float gz = e1x * e2y - e1y * e2x;
        float vx = A.normal.x + B.normal.x + C.normal.x;
        float vy = A.normal.y + B.normal.y + C.normal.y;
        float vz = A.normal.z + B.normal.z + C.normal.z;
        align += gx * vx + gy * vy + gz * vz;
    }
    const float nsign = ( align < 0.0f ) ? -1.0f : 1.0f;

    // (2026-11-14 fix) Coplanar iris quads: when RTGL_ClassifyMaterial
    // routes an eyeball material to the WORLD raster channel (depth-tested),
    // nudge each vertex a fraction of a Doom unit toward the camera so the
    // iris wins LESS_OR_EQUAL against the eyeball mesh it sits on. Vertices
    // are uploaded in OBJECT space with mesh.transform = modelMatrix (the
    // proven P2 path), so convert the world-space camera-ward push into
    // object space via A^T (modelMatrix 3x3 is an orthonormal rotation for
    // Doom entities). Only active when rt_eye_worldRaster is on.
    const bool  isEyeRaster = rt_eye_worldRaster.GetBool() &&
                              surf->material->Deform() == DFRM_EYEBALL;
    const bool  isLitDecalCutout = !isEyeRaster &&
                                   RTGL_IsDecal( surf->material ) &&
                                   surf->material->ReceivesLighting();
    const bool  isDecalRaster = !isEyeRaster &&
                                !isLitDecalCutout &&
                                ( rt_decal_mode.GetInteger() == 1 ) &&
                                RTGL_IsDecal( surf->material );
    const float eyePush = isEyeRaster ? rt_eye_depthPush.GetFloat()
                        : ( isDecalRaster ? 0.15f : ( isLitDecalCutout ? 0.05f : 0.0f ) );
    const float *emm = surf->space ? surf->space->modelMatrix : NULL;

    // sregs already declared and evaluated at top of RTGL_SubmitSurf

    // Locate the primary active stage so we can apply its texture matrix,
    // conditionRegister, stage color registers, and vertexColor mode (needed
    // by translucent effects: deform flare, deform sprite, smoke/dust/beam
    // particles, and DFRM_EYEBALL iris materials).
    const shaderStage_t *texStage = NULL;
    bool anyActiveStage = false;
    for ( int s = 0; s < surf->material->GetNumStages(); s++ ) {
        const shaderStage_t *st = surf->material->GetStage( s );
        if ( !st ) { continue; }
        if ( sregs && st->conditionRegister >= 0 && sregs[ st->conditionRegister ] == 0.0f ) {
            continue;
        }
        anyActiveStage = true;
    }
    if ( !anyActiveStage ) {
        g_rtglStats.skippedSurfs++;
        return;
    }
    const int maskColorBits = ( GLS_REDMASK | GLS_GREENMASK | GLS_BLUEMASK );
    for ( int pass = 0; pass < 3 && !texStage; pass++ ) {
        for ( int s = 0; s < surf->material->GetNumStages(); s++ ) {
            const shaderStage_t *st = surf->material->GetStage( s );
            if ( !st || !st->texture.image || st->texture.image->type != TT_2D ) { continue; }
            if ( st->lighting == SL_BUMP ) { continue; }
            if ( ( st->drawStateBits & maskColorBits ) == maskColorBits ) { continue; }
            if ( sregs && st->conditionRegister >= 0 && sregs[ st->conditionRegister ] == 0.0f ) {
                continue;
            }
            if ( ( pass == 0 && st->lighting == SL_DIFFUSE ) ||
                 ( pass == 1 && st->lighting == SL_AMBIENT ) ||
                 ( pass == 2 && st->lighting == SL_SPECULAR ) ) {
                texStage = st;
                break;
            }
        }
    }
    const bool hasTexMat = ( texStage != NULL && texStage->texture.hasMatrix && sregs != NULL );

    // Evaluate stage color & vertexColor mode for pure ambient/translucent
    // effects (flares, sprites, particles/smoke). Standard lit diffuse/bump
    // surfaces use interaction lighting and keep neutral white vertex/prim color.
    const bool isSkySurf = RTGL_IsSkySurface( surf->material );
    const bool isEffectSurf =
        !isEyeRaster &&
        !isSkySurf &&
        !isHeatHaze &&
        ( isDecalRaster ||
          isLitDecalCutout ||
          surf->material->Coverage() == MC_TRANSLUCENT ||
          ( surf->material->Deform() != DFRM_NONE && surf->material->Deform() != DFRM_EYEBALL ) ||
          ( !surf->material->ReceivesLighting() ) );

    stageVertexColor_t vcMode = SVC_IGNORE;
    bool isOneOneAdd = false;
    RgColor4DPacked32 stageColorPacked = RTGL_White();
    if ( isEffectSurf && texStage ) {
        vcMode = texStage->vertexColor;
        int srcB = texStage->drawStateBits & GLS_SRCBLEND_BITS;
        int dstB = texStage->drawStateBits & GLS_DSTBLEND_BITS;
        isOneOneAdd = ( srcB == GLS_SRCBLEND_ONE && dstB == GLS_DSTBLEND_ONE &&
                        !texStage->hasAlphaTest );
        float cr = 1.0f, cg = 1.0f, cb = 1.0f, ca = 1.0f;
        if ( sregs ) {
            cr = sregs[ texStage->color.registers[0] ];
            cg = sregs[ texStage->color.registers[1] ];
            cb = sregs[ texStage->color.registers[2] ];
            ca = sregs[ texStage->color.registers[3] ];
            if ( cr < 0.0f ) { cr = 0.0f; } else if ( cr > 1.0f ) { cr = 1.0f; }
            if ( cg < 0.0f ) { cg = 0.0f; } else if ( cg > 1.0f ) { cg = 1.0f; }
            if ( cb < 0.0f ) { cb = 0.0f; } else if ( cb > 1.0f ) { cb = 1.0f; }
            if ( ca < 0.0f ) { ca = 0.0f; } else if ( ca > 1.0f ) { ca = 1.0f; }
        }
        if ( isLitDecalCutout ) {
            ca = 1.0f; // Keep alpha=1.0 so RtAlphaTest.rahit uses baked texel alpha
        }
        // Scale light flare / glare additive surfaces by rt_flare_opacity so
        // RTGL1's post-raster auto-exposure does not blow them out into opaque
        // white polygons. RTGL1's ADDITIVE pipeline blends as SRC_ALPHA, ONE,
        // so scaling `ca` directly controls the additive opacity.
        const bool isFlareSurf =
            ( surf->material->Deform() == DFRM_FLARE ) ||
            ( texStage->texture.image &&
              idStr::Icmp( texStage->texture.image->imgName.c_str(), "_quadratic" ) == 0 ) ||
            ( idStr::FindText( surf->material->GetName(), "flare", false ) >= 0 );
        if ( isFlareSurf ) {
            float flareOp = rt_flare_opacity.GetFloat();
            if ( flareOp < 0.0f ) { flareOp = 0.0f; } else if ( flareOp > 1.0f ) { flareOp = 1.0f; }
            ca *= flareOp;
        }
        // Attenuate oversized ambient dust / smoke clouds (`textures/particles/dust`)
        // so RTGL1's post-raster auto-exposure (`outColor *= avgLuminance`) does not
        // over-brighten them into thick grey sheets in dark corridors.
        if ( idStr::Icmp( surf->material->GetName(), "textures/particles/dust" ) == 0 ) {
            ca *= 0.20f;
        }
        // Skip effect stage if its evaluated contribution is completely zero
        if ( ca <= 0.0f || ( isOneOneAdd && cr <= 0.0f && cg <= 0.0f && cb <= 0.0f ) ) {
            g_rtglStats.skippedSurfs++;
            return;
        }
        stageColorPacked = RTGL_PackColorF( cr, cg, cb, ca );
    }

    RgColor4DPacked32 vcolor = RTGL_White();
    for ( int i = 0; i < nv; i++ ) {
        const idDrawVert &src = tri->verts[i];
        RgPrimitiveVertex &dst = s_verts[i];
        dst.position[0]  = src.xyz.x;
        dst.position[1]  = src.xyz.y;
        dst.position[2]  = src.xyz.z;
        if ( eyePush > 0.0f ) {
            float ax = src.xyz.x, ay = src.xyz.y, az = src.xyz.z;
            float wx = ax, wy = ay, wz = az;
            if ( emm ) {
                wx = emm[0]*ax + emm[4]*ay + emm[8]*az  + emm[12];
                wy = emm[1]*ax + emm[5]*ay + emm[9]*az  + emm[13];
                wz = emm[2]*ax + emm[6]*ay + emm[10]*az + emm[14];
            }
            idVec3 toCam( camPos.x - wx, camPos.y - wy, camPos.z - wz );
            float dist = toCam.Length();
            if ( dist > 1e-4f ) {
                float s = eyePush / dist;
                float pdx = toCam.x * s, pdy = toCam.y * s, pdz = toCam.z * s;
                if ( emm ) {
                    dst.position[0] = ax + (emm[0]*pdx + emm[1]*pdy + emm[2]*pdz);
                    dst.position[1] = ay + (emm[4]*pdx + emm[5]*pdy + emm[6]*pdz);
                    dst.position[2] = az + (emm[8]*pdx + emm[9]*pdy + emm[10]*pdz);
                } else {
                    dst.position[0] = ax + pdx;
                    dst.position[1] = ay + pdy;
                    dst.position[2] = az + pdz;
                }
            }
        }
        // Upload in entity/object space (nsign aligns hemisphere; see above).
        // RTGL1 getTriangle() (VertexData.inl:526-531) transforms decoded normals
        // to world space with the instance matrix (w=0), just like positions --
        // do NOT pre-rotate on the CPU (double transform -> black characters).
        dst.normalPacked = RTGL_PackNormal( nsign * src.normal.x,
                                           nsign * src.normal.y,
                                           nsign * src.normal.z );
        float u = src.st.x;
        float v = src.st.y;
        if ( hasTexMat ) {
            const int (&tm)[2][3] = texStage->texture.matrix;
            float nu = u * sregs[ tm[0][0] ] + v * sregs[ tm[0][1] ] + sregs[ tm[0][2] ];
            float nv_uv = u * sregs[ tm[1][0] ] + v * sregs[ tm[1][1] ] + sregs[ tm[1][2] ];
            u = nu;
            v = nv_uv;
        }
        if ( isEyeRaster ) {
            if ( u < 0.01f ) { u = 0.01f; } else if ( u > 0.99f ) { u = 0.99f; }
            if ( v < 0.01f ) { v = 0.01f; } else if ( v > 0.99f ) { v = 0.99f; }
        }
        dst.texCoord[0]  = u;
        dst.texCoord[1]  = v;
        if ( vcMode == SVC_MODULATE ) {
            dst.color = RTGL_PackColorB( src.color[0], src.color[1], src.color[2], src.color[3] );
        } else if ( vcMode == SVC_INVERSE_MODULATE ) {
            dst.color = RTGL_PackColorB( (uint8_t)( 255 - src.color[0] ),
                                         (uint8_t)( 255 - src.color[1] ),
                                         (uint8_t)( 255 - src.color[2] ), src.color[3] );
        } else {
            dst.color = vcolor;
        }
        dst._pad0        = 0;
    }

    // --- glIndex_t (int) -> uint32_t ---
    s_indices.SetNum( ni );
    for ( int i = 0; i < ni; i++ ) {
        s_indices[i] = (uint32_t)tri->indexes[i];
    }

    // --- P3 material classification + primary albedo ---
    RgMeshPrimitiveFlags primFlags = 0;
    float emissive = 0.0f;
    RTGL_ClassifyMaterial( surf->material, &primFlags, &emissive );
    // P2 fix (2026-11-14): use SMOOTH per-vertex normals from idDrawVert.
    // RTGL1 defaults to GENERATE_NORMALS (overwrites uploaded vertex normals
    // with face normals in CmVertexPreprocess), and FORCE_EXACT_NORMALS makes
    // HitInfo.inl use cross(edge1,edge2) directly -> both give flat shading.
    // Setting DONT_GENERATE_NORMALS preserves the uploaded (already-smoothed)
    // normals; omitting FORCE_EXACT_NORMALS selects the interpolated vertex
    // normal in HitInfo.inl:319-322 (RTGL1 transforms them to world space).
    primFlags |= RG_MESH_PRIMITIVE_DONT_GENERATE_NORMALS;
    const char *texName = RTGL_PrimaryTexture( surf->material, sregs );

    // For opaque/alpha-tested surfaces with a `blend add` glow stage, evaluate
    // the additive stage's conditionRegister and RGB registers against `sregs`:
    //   1) If the stage is disabled or its RGB is 0 (e.g. switched-off light
    //      fixture or unpowered panel), or it has no 2D `_add` mask texture,
    //      force `emissive = 0.0f` so HitInfo.inl never falls back to making the
    //      entire diffuse surface (`h.albedo * tr.emissiveMult`) glow.
    //   2) Otherwise scale `emissive` by the stage's peak RGB channel, while
    //      keeping `stageColorPacked` (diffuse albedo) untouched.
    if ( !isEffectSurf && emissive > 0.0f ) {
        float activeGlow = 0.0f;
        const int numRegs = surf->material->GetNumRegisters();
        for ( int s = 0; s < surf->material->GetNumStages(); s++ ) {
            const shaderStage_t *st = surf->material->GetStage( s );
            if ( !st || st->lighting != SL_AMBIENT || st->hasAlphaTest ) { continue; }
            if ( sregs && st->conditionRegister >= 0 && st->conditionRegister < numRegs &&
                 sregs[ st->conditionRegister ] == 0.0f ) {
                continue;
            }
            int dstB = st->drawStateBits & GLS_DSTBLEND_BITS;
            int srcB = st->drawStateBits & GLS_SRCBLEND_BITS;
            if ( dstB == GLS_DSTBLEND_ONE && srcB != GLS_SRCBLEND_ZERO &&
                 st->texture.image && st->texture.image->type == TT_2D &&
                 st->texture.image->imgName.Length() > 0 &&
                 st->texture.image->imgName[0] != '_' ) {
                float cr = 1.0f, cg = 1.0f, cb = 1.0f;
                if ( sregs ) {
                    int r0 = st->color.registers[0];
                    int r1 = st->color.registers[1];
                    int r2 = st->color.registers[2];
                    if ( r0 >= 0 && r0 < numRegs ) { cr = sregs[r0]; }
                    if ( r1 >= 0 && r1 < numRegs ) { cg = sregs[r1]; }
                    if ( r2 >= 0 && r2 < numRegs ) { cb = sregs[r2]; }
                }
                float maxC = cr;
                if ( cg > maxC ) { maxC = cg; }
                if ( cb > maxC ) { maxC = cb; }
                if ( maxC > activeGlow ) { activeGlow = maxC; }
            }
        }
        if ( activeGlow <= 0.0f ) {
            emissive = 0.0f;
        } else {
            if ( activeGlow > 1.0f ) { activeGlow = 1.0f; }
            emissive *= activeGlow;
        }
    }

    int colorMode = rt_dbg_colorMode.GetInteger();
    RgColor4DPacked32 primColor = ( colorMode != 0 )
        ? RTGL_DebugColor( colorMode, objectID, ordinal, entityIndex,
                           surf->material->GetName(), isStatic )
        : stageColorPacked;

    // --- mesh (one per drawSurf) ---
    RgMeshInfo mesh = {};
    mesh.sType = RG_STRUCTURE_TYPE_MESH_INFO;
    mesh.pNext = NULL;
    mesh.flags = 0;
    if ( space && space->weaponDepthHack ) {
        mesh.flags |= RG_MESH_FIRST_PERSON;
    }
    mesh.uniqueObjectID = objectID;
    mesh.pMeshName = NULL;
    RTGL_IdentityTransform( &mesh.transform );
    if ( space ) {
        RTGL_ModelMatrixToTransform( space->modelMatrix, &mesh.transform );
    }
    mesh.isExportable = 0;
    mesh.animationTime = 0.0f;
    mesh.localLightsIntensity = 1.0f;

    float pbrRoughness = 1.0f;
    float pbrMetallic  = 0.0f;
    RTGL_GetMaterialPBR( surf->material, &pbrRoughness, &pbrMetallic );

    RgMeshPrimitivePBREXT pbrExt = {};
    pbrExt.sType = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_PBR_EXT;
    pbrExt.pNext = NULL;
    pbrExt.metallicDefault  = pbrMetallic;
    pbrExt.roughnessDefault = pbrRoughness;

    RgMeshPrimitiveInfo prim = {};
    prim.sType = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_INFO;
    prim.pNext = &pbrExt;
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

    // Dump-time outcome marker: for the effect/deform surfaces that the dump
    // filter selects, print that they ACTUALLY reached the upload (so a grey
    // quad can be attributed unambiguously). Pairs with [RTGL_HAZE_DROP].
    if ( s_dumpSurfsThisFrame ) {
        char flagStr[256];
        RTGL_FormatPrimFlags( primFlags, flagStr, sizeof( flagStr ) );
        common->Printf( "[RTGL_SURF_UPLOAD] #%03d mat='%s' matPtr=0x%p sort=%.1f deform=%d "
                        "tex='%s' flags=0x%x [%s] objID=0x%llx -> uploaded\n",
                        ordinal, surf->material->GetName(), (void*)surf->material, sort,
                        (int)surf->material->Deform(),
                        texName ? texName : "<none>", (int)primFlags, flagStr,
                        (unsigned long long)mesh.uniqueObjectID );
    }

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
    common->Printf( "[RTGL] frame %d | ui surfs=%d prims=%d verts=%d | "
        "worldGui(mode=%d) surfs=%d prims=%d verts=%d\n",
        g_rtglFrameIndex, g_rtglStats.uiSurfs, g_rtglStats.uiPrims,
        g_rtglStats.uiVerts, rt_ui_worldGuisMode.GetInteger(),
        g_rtglStats.worldGuiSurfs, g_rtglStats.worldGuiPrims,
        g_rtglStats.worldGuiVerts );
}

// Project one local-space vertex to normalized [0,1] screen coordinates using
// the view's modelViewMatrix (local->eye) and projectionMatrix (eye->clip).
// Returns false if the vertex is behind the camera (w <= 0).
static bool RTGL_ProjectVertexToScreen( const idVec3 &v,
                                        const float mv[16], const float proj[16],
                                        float &outX, float &outY ) {
    // eye = modelViewMatrix * (v, 1)   (column-major OpenGL layout)
    float ex = mv[0]*v.x + mv[4]*v.y + mv[8]*v.z  + mv[12];
    float ey = mv[1]*v.x + mv[5]*v.y + mv[9]*v.z  + mv[13];
    float ez = mv[2]*v.x + mv[6]*v.y + mv[10]*v.z + mv[14];
    float ew = mv[3]*v.x + mv[7]*v.y + mv[11]*v.z + mv[15];
    // clip = projectionMatrix * eye
    float cx = proj[0]*ex + proj[4]*ey + proj[8]*ez  + proj[12]*ew;
    float cy = proj[1]*ex + proj[5]*ey + proj[9]*ez  + proj[13]*ew;
    float cw = proj[3]*ex + proj[7]*ey + proj[11]*ez + proj[15]*ew;
    if ( cw <= 0.0001f ) { return false; }
    float ndcX = cx / cw;   // [-1,1]
    float ndcY = cy / cw;   // [-1,1], +Y up
    outX = ( ndcX + 1.0f ) * 0.5f;        // [0,1] left->right
    outY = 1.0f - ( ndcY + 1.0f ) * 0.5f; // [0,1] top->bottom
    return true;
}

// Build the RgCustomPassHeatHazeFrame POD from the heat-haze quads that were
// intercepted this frame. Each quad becomes a screen-space rectangle (the AABB
// of its projected vertices). Called once per frame at the end of
// RTGL_SubmitCommands(), while camView->projectionMatrix is still valid.
static void RTGL_BuildHeatHazePayload( const viewDef_t *camView ) {
    RgCustomPassHeatHazeFrame &f = g_heatHazePayload;
    memset( &f, 0, sizeof( f ) );
    f.magic   = RG_CUSTOM_PASS_HEATHAZE_MAGIC;
    f.version = RG_CUSTOM_PASS_HEATHAZE_VERSION;
    f.time    = (float)( g_rtglFrameIndex ) * ( 1.0f / 60.0f );
    f.ior           = rt_refr_index.GetFloat();
    f.waveSpeed     = rt_refr_waveSpeed.GetFloat();
    f.waveStrength  = rt_refr_waveStrength.GetFloat();
    f.targetWidth   = (float)( g_rtglWidth  > 0 ? g_rtglWidth  : 640 );
    f.targetHeight  = (float)( g_rtglHeight > 0 ? g_rtglHeight : 480 );

    if ( !camView ) { return; }
    const float *proj = camView->projectionMatrix;

    int n = g_deferredPostSurfs.Num();
    for ( int i = 0; i < n && f.quadCount < RG_CUSTOM_PASS_HEATHAZE_MAX_QUADS; i++ ) {
        const rtglDeferredPostSurf_t &ds = g_deferredPostSurfs[i];
        if ( !ds.geo || !ds.space || !ds.geo->verts || ds.geo->numVerts <= 0 ) { continue; }
        const float *mv = ds.space->modelViewMatrix;

        float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
        int visible = 0;
        for ( int k = 0; k < ds.geo->numVerts; k++ ) {
            float sx, sy;
            if ( !RTGL_ProjectVertexToScreen( ds.geo->verts[k].xyz, mv, proj, sx, sy ) ) { continue; }
            if ( sx < minX ) minX = sx;  if ( sx > maxX ) maxX = sx;
            if ( sy < minY ) minY = sy;  if ( sy > maxY ) maxY = sy;
            visible++;
        }
        if ( visible == 0 ) { continue; }

        RgCustomPassHeatHazeQuad &q = f.quads[ f.quadCount++ ];
        q.rectX = minX;
        q.rectY = minY;
        q.rectW = maxX - minX;
        q.rectH = maxY - minY;
        q.phase = ds.scroll[0] + ds.scroll[1];
        // deformMag is the vertexParm[1] wave magnitude; clamp to a sane range.
        q.strength = ds.deformMag;
        if ( q.strength <= 0.0f || q.strength > 8.0f ) { q.strength = 1.0f; }
        q.maskFeather = 0.12f;
        q.reserved = 0.0f;
    }
}

void RTGL_SubmitCommands( const void *cmdHead ) {
    if ( s_submittedThisFrame || !RTGL_FrameActive() ) {
        return;
    }
    const emptyCommand_t *cmd = (const emptyCommand_t *)cmdHead;
    if ( !cmd ) {
        return;
    }

    int dumpN = rt_dbg_dumpSurfs.GetInteger();
    if ( dumpN == 1 ) {
        s_dumpSurfsThisFrame = true;
        rt_dbg_dumpSurfs.SetInteger( 0 ); // one-shot dump
    } else if ( dumpN > 1 ) {
        s_dumpSurfsThisFrame = ( g_rtglFrameIndex % dumpN == 0 );
    } else {
        s_dumpSurfsThisFrame = false;
    }

    // Camera: first non-subview RC_DRAW_VIEW.
    const viewDef_t *camView = NULL;
    for ( const emptyCommand_t *c = cmd; c; c = (const emptyCommand_t *)c->next ) {
        if ( c->commandId == RC_DRAW_VIEW ) {
            const drawSurfsCommand_t *dv = (const drawSurfsCommand_t *)c;
            // P-UI: 2D/GUI views (viewEntitys==NULL) are pure overlays and
            // carry no usable camera; only a real 3D view may supply one.
            if ( dv->viewDef && dv->viewDef->viewEntitys &&
                 !dv->viewDef->isSubview && !dv->viewDef->isMirror ) {
                camView = dv->viewDef;
                break;
            }
        }
    }
    if ( camView ) {
        RTGL_UploadCamera( camView );
        g_rtglCameraUploaded = true;
    }

    // Dump header: one line per one-shot dump, so every [RTGL_SURF_DUMP]
    // / [RTGL_HAZE_DROP] / [RTGL_SURF_UPLOAD] below can be tied to an exact
    // level + frame + camera pose. Heat-haze quads are `deform sprite`
    // billboards (always face the camera), so without the camera pose a dump
    // cannot be correlated with a screenshot / viewpoint.
    if ( s_dumpSurfsThisFrame ) {
        // The authoritative CURRENT map name is idSessionLocal::currentMapName,
        // built by Session.cpp:1584-1596 from mapSpawnData.serverInfo (the dict
        // actually used to load the world), exposed via session->GetCurrentMapName().
        // Do NOT use the global "si_map" cvar here: it is CVAR_ARCHIVE with a
        // hardcoded default "game/mp/d3dm1" (SysCvar.cpp:71) and means "map to
        // be played NEXT"; +map / console map switches never update it, so it
        // stays stale and mislabels the dump (e.g. showing d3dm1 while the
        // player is actually in game/mc_underground). Print si_map separately
        // as a reference so the two are never confused again.
        const char *curMap = session ? session->GetCurrentMapName() : NULL;
        if ( !curMap || !curMap[0] ) { curMap = "<unknown>"; }
        const char *siMap = cvarSystem->GetCVarString( "si_map" );
        if ( !siMap || !siMap[0] ) { siMap = "<unset>"; }
        if ( camView ) {
            const renderView_t &rv = camView->renderView;
            const idVec3 &fwd = rv.viewaxis[0];
            common->Printf( "[RTGL_DUMP_HEADER] map='%s' si_map='%s' frame=%d "
                            "camPos=(%.1f, %.1f, %.1f) "
                            "camFwd=(%.3f, %.3f, %.3f) fovY=%.1f\n",
                            curMap, siMap, g_rtglFrameIndex,
                            rv.vieworg.x, rv.vieworg.y, rv.vieworg.z,
                            fwd.x, fwd.y, fwd.z, rv.fov_y );
        } else {
            common->Printf( "[RTGL_DUMP_HEADER] map='%s' si_map='%s' frame=%d "
                            "camPos=<none> camFwd=<none> fovY=<none>\n",
                            curMap, siMap, g_rtglFrameIndex );
        }
    }

    // Geometry from every RC_DRAW_VIEW.
    int budget = rt_dbg_maxSurfs.GetInteger();
    int ordinal = 0;
    for ( const emptyCommand_t *c = cmd; c; c = (const emptyCommand_t *)c->next ) {
        if ( c->commandId != RC_DRAW_VIEW ) { continue; }
        const drawSurfsCommand_t *dv = (const drawSurfsCommand_t *)c;
        const viewDef_t *v = dv->viewDef;
        if ( !v ) { continue; }
        // P-UI: 2D/GUI views are handled by RTGL_SubmitUI (swapchain overlays),
        // not by the world-geometry path (RTGL_SubmitSurf drops sort<0 anyway).
        if ( !v->viewEntitys ) { continue; }
        if ( rt_dbg_skipSubViews.GetBool() &&
             ( v->isSubview || v->isMirror || v->isXraySubview ) ) {
            continue;
        }
        for ( int i = 0; i < v->numDrawSurfs; i++ ) {
            if ( budget > 0 && g_rtglStats.primitives >= budget ) { break; }
            const drawSurf_t *ds = v->drawSurfs[i];
            if ( ds && ds->material && RTGL_IsSkySurface( ds->material ) ) {
                RTGL_ObserveSkySurface( ds, v->floatTime );
            }
            RTGL_SubmitSurf( ds, ordinal++, v->renderView.vieworg );
        }
    }

    // Pack the intercepted heat-haze quads into the custom-pass payload while
    // the primary view (camera + projectionMatrix) is still in scope.
    RTGL_BuildHeatHazePayload( camView );

    s_submittedThisFrame = true;
    // NOTE: RTGL_ReportStats() is now called by RTGL_SubmitFrameCommands()
    // after RTGL_SubmitLights(), so P4 light counters are included.
}

#endif // D3_RTGL
