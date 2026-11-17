#include "sys/platform.h"
#include "framework/Common.h"
#include "idlib/Str.h"
#include <math.h>

#ifdef D3_RTGL

#include "renderer/rtgl/rtgl_internal.h"

// ===========================================================================
// P4 - Lights
//
// Walk the frame's viewLight_t chains (viewDef->viewLights) and translate each
// DOOM 3 light into an RTGL1 RgLightInfo + a pNext-linked RgLightXXXEXT:
//
//   parms.pointLight                      -> RgLightSphericalEXT
//   projected light (has target/frustum)  -> RgLightSpotEXT   (cookie ~ spot)
//   parms.parallel                        -> RgLightDirectionalEXT  (enabled by default; rt_light_skipParallel 1 to skip)
//   lightShader fog / blend               -> skipped (no RTGL1 equivalent)
//   noShadows                             -> kept; RTGL1 shadow rays are physical
//   rt_sun_enable                         -> one extra directional light (view-INDEPENDENT global sun)
//
// PARALLEL / DIRECTIONAL LIGHTS (2026-11-13 fix, mars_city1 light pop):
//   RTGL1 RgLightDirectionalEXT is a SINGLE, UNBOUNDED, GLOBAL sun for the whole
//   frame (LightManager allows only one; RaygenCommon.h samples it at EVERY
//   surface via globalUniform.directionalLightExists, a per-frame bool).
//   Doom 3 "parallel" lights, however, are bounded to a finite light volume
//   (lightRadius / start / end) and only appear in viewDef->viewLights when that
//   volume intersects the view frustum (tr_local.h: a viewLight "should never
//   exist if its volume does not intersect the view frustum"; R_Remove-
//   UnecessaryViewLights also drops lights with no visible interactions).
//   => Deriving the global sun from the frustum-culled viewLights makes
//      directionalLightExists flip 0<->1 when a bounded parallel light crosses
//      the frustum boundary on a tiny camera rotation, flooding / darkening the
//      ENTIRE scene at once. mars_city1 start has exactly two such lights
//      (light_5262 @ 313 -1216 147, light_5351 @ 313 -1472 147), both tiny
//      (radius "32 48 32") and "nodiffuse 1 nospecular 1 falloff 0" so they
//      light no surfaces in the original game -> rotating the camera caused no
//      brightness change there, but imported as a global sun it pops.
//   Mitigation (11-13): rt_light_skipParallel=1 skipped imported p.parallel
//   lights; directionalLightExists was then driven ONLY by rt_sun_enable
//   (injected view-independently every frame -> no pop).
//   2026-11-14: support RESTORED (rt_light_skipParallel default back to 0).
//   If the pop returns, set rt_light_skipParallel 1 or use rt_sun_enable instead.
//
// COORDINATE / SCALE CONTRACT (important):
//   P2 submits geometry in RAW DOOM engine units, and RTGL1 does NOT scale
//   runtime vertices by worldScale (worldScale is only used by the GLTF
//   import/export path). Therefore every light position/direction/radius here
//   is also in RAW DOOM UNITS so geometry and lights line up. worldScale
//   (0.0254) only affects the physical meaning of the intensity unit (lumen)
//   and denoiser depth thresholds; the visual match is tuned through
//   rt_light_intensity / importedLightIntensityScale*, not by rescaling coords.
//
// DOOM 3 lights are artistic / non-physical. There is no exact physical
// intensity. rt_light_intensity is the single tuning knob; the value is a
// luminous-flux-like scalar. Stable uniqueID = lightDef->index (stable for the
// life of the light def) so A-SVGF / ReSTIR temporal accumulation does not
// break when lights move or flicker.
// ===========================================================================

static idList<uint64_t> s_lightIDs;   // per-frame duplicate detection
static bool s_directionalUploaded = false; // RTGL1 accepts only ONE directional light per frame

void RTGL_LightBeginFrame( void ) {
    s_lightIDs.Clear();
    s_directionalUploaded = false;
}

// Extract the light's color from its lightShader's first active stage,
// mirroring backEnd.lightColor = regs[stage->color.registers[0..2]].
// Falls back to the light's shaderParms (the DOOM-wide light color parms).
static RgColor4DPacked32 RTGL_LightColor( const viewLight_t *vLight ) {
    float r = 1.0f, g = 1.0f, b = 1.0f;

    const idMaterial *mat = vLight->lightShader;
    const float *regs = vLight->shaderRegisters;
    if ( mat && regs && mat->GetNumStages() > 0 ) {
        int numRegs = mat->GetNumRegisters();
        const shaderStage_t *stage = mat->GetStage( 0 );
        if ( stage && stage->conditionRegister >= 0 && stage->conditionRegister < numRegs &&
             regs[ stage->conditionRegister ] != 0.0f ) {
            int r0 = stage->color.registers[0];
            int r1 = stage->color.registers[1];
            int r2 = stage->color.registers[2];
            if ( r0 >= 0 && r0 < numRegs ) { r = regs[ r0 ]; }
            if ( r1 >= 0 && r1 < numRegs ) { g = regs[ r1 ]; }
            if ( r2 >= 0 && r2 < numRegs ) { b = regs[ r2 ]; }
        }
    } else if ( vLight->lightDef ) {
        const renderLight_t &parms = vLight->lightDef->parms;
        r = parms.shaderParms[SHADERPARM_RED];
        g = parms.shaderParms[SHADERPARM_GREEN];
        b = parms.shaderParms[SHADERPARM_BLUE];
    }

    // Guard against NaN / absurd values and clamp to a sane linear range.
    if ( r < 0.0f ) r = 0.0f; if ( r > 4.0f ) r = 4.0f;
    if ( g < 0.0f ) g = 0.0f; if ( g > 4.0f ) g = 4.0f;
    if ( b < 0.0f ) b = 0.0f; if ( b > 4.0f ) b = 4.0f;

    // RTGL1 premultiplies color * intensity. Return the raw (possibly >1)
    // color so the hue AND the original per-light brightness both survive;
    // intensity is applied separately (and is size-driven, see below).
    // Do NOT multiply by any derived colorBrightness here: that would
    // double-count the color energy already carried in r/g/b.
    return RTGL_PackColorF( r, g, b, 1.0f );
}

// Bake a light stage's 2D texture matrix into the world-space S/T projection
// planes (mirrors RB_BakeTextureMatrixIntoTexgen / DUDE's RB_RHI_BakeLightTextureMatrix).
static void RTGL_BakeLightTextureMatrix( float proj[4][4], const float texMat[16] ) {
    float genMatrix[16], fin[16];
    genMatrix[0] = proj[0][0]; genMatrix[4] = proj[0][1]; genMatrix[8]  = proj[0][2]; genMatrix[12] = proj[0][3];
    genMatrix[1] = proj[1][0]; genMatrix[5] = proj[1][1]; genMatrix[9]  = proj[1][2]; genMatrix[13] = proj[1][3];
    genMatrix[2] = 0.0f;       genMatrix[6] = 0.0f;       genMatrix[10] = 0.0f;       genMatrix[14] = 0.0f;
    genMatrix[3] = proj[2][0]; genMatrix[7] = proj[2][1]; genMatrix[11] = proj[2][2]; genMatrix[15] = proj[2][3];
    myGlMultMatrix( genMatrix, texMat, fin );
    proj[0][0] = fin[0]; proj[0][1] = fin[4]; proj[0][2] = fin[8]; proj[0][3] = fin[12];
    proj[1][0] = fin[1]; proj[1][1] = fin[5]; proj[1][2] = fin[9]; proj[1][3] = fin[13];
}

// Fill RgLightBoundsAndCookieEXT from Doom 3's viewLight_t (lightProject planes +
// 2D cookie & falloff textures, ported from DUDE's RB_RHI_SceneLightRow).
static bool RTGL_BuildLightBoundsAndCookie( const viewLight_t *vLight,
                                            RgLightBoundsAndCookieEXT *outExt ) {
    memset( outExt, 0, sizeof( *outExt ) );
    if ( !rt_light_useBoundsAndCookie.GetBool() || !vLight || !vLight->lightDef ) {
        return false;
    }
    const idRenderLightLocal *ld = vLight->lightDef;
    if ( ld->parms.parallel ) {
        return false;
    }

    outExt->sType = RG_STRUCTURE_TYPE_LIGHT_BOUNDS_AND_COOKIE_EXT;
    outExt->pNext = NULL;
    outExt->flags = 1u | ( ld->parms.pointLight ? 0u : 2u );

    for ( int r = 0; r < 4; r++ ) {
        outExt->lightProject[r][0] = vLight->lightProject[r][0];
        outExt->lightProject[r][1] = vLight->lightProject[r][1];
        outExt->lightProject[r][2] = vLight->lightProject[r][2];
        outExt->lightProject[r][3] = vLight->lightProject[r][3];
    }

    const idMaterial *ls = vLight->lightShader;
    const float *cregs = vLight->shaderRegisters;
    if ( ls ) {
        const shaderStage_t *liveStage = NULL;
        for ( int s = 0; s < ls->GetNumStages(); s++ ) {
            const shaderStage_t *st = ls->GetStage( s );
            if ( !st ) { continue; }
            if ( cregs && st->conditionRegister >= 0 && cregs[ st->conditionRegister ] == 0.0f ) {
                continue;
            }
            liveStage = st;
            break;
        }
        if ( liveStage && liveStage->texture.image && liveStage->texture.image->type == TT_2D &&
             liveStage->texture.cinematic == NULL ) {
            outExt->pCookieTextureName = RTGL_LightTexture( liveStage->texture.image );
            const textureRepeat_t wrap = liveStage->texture.image->repeat;
            const bool zeroClamped = ( wrap == TR_CLAMP_TO_ZERO ||
                                       wrap == TR_CLAMP_TO_ZERO_ALPHA ||
                                       wrap == TR_CLAMP_TO_BORDER );
            if ( outExt->pCookieTextureName && liveStage->texture.hasMatrix && cregs && zeroClamped ) {
                float texMat[16];
                RB_GetShaderTextureMatrix( cregs, &liveStage->texture, texMat );
                RTGL_BakeLightTextureMatrix( outExt->lightProject, texMat );
            }
        }
    }

    const idImage *falloffImg = vLight->falloffImage ? vLight->falloffImage : ld->falloffImage;
    if ( falloffImg && falloffImg->type == TT_2D ) {
        outExt->pFalloffTextureName = RTGL_LightTexture( falloffImg );
    }

    return true;
}

static bool RTGL_SeenLightID( uint64_t id ) {
    for ( int i = 0; i < s_lightIDs.Num(); i++ ) {
        if ( s_lightIDs[i] == id ) { return true; }
    }
    s_lightIDs.Append( id );
    return false;
}

// ===========================================================================
// Debug: (C) per-light console dump + (A) in-scene light visualization
//
// rt_dbg_dumpLights is an interval in frames (0 = off). Each light gets one
// line on common->Printf -- exactly what the dedicated Win32 console
// (win_viewlog 1) shows. (C) enhancement: lines are buffered and printed
// SORTED by distance to the main camera, and each line gains "d=" (distance)
// plus the light's world-space AABB, so the list can be correlated with what
// is on screen. pos/radius are RAW Doom units; I is rt_light_intensity;
// RTGL1 encodes radiance as I/(pi*radius^2). Skipped-light counts live in the
// [RTGL] stats line (rt_dbg_stats). Throttle uses g_rtglFrameIndex.
//
// (A) rt_dbg_lightViz: for every uploaded sphere/spot light, submit an
// emissive sphere (mode 1) or a 3-ring wireframe (mode 2) centred at the light
// with radius == the ACTUAL radius uploaded to RTGL1 (i.e. after
// rt_light_radiusScale). It is real in-scene geometry via rgUploadMeshPrimitive
// (the same proven path as P2 geometry), so it is occluded by walls, follows
// the camera and shows the true extent. Tagged NO_SHADOW so it does not perturb
// the lighting being inspected. Directional lights have no finite radius or
// meaningful position (z ~ 100000), so they are not drawn.
//
// All of it is gated by cvars that default to 0/off: zero effect on normal
// rendering. NOTE: emissive/tonemapping appearance is not verified here (the
// project's convention is compile-only validation); check visually at runtime.
// ===========================================================================
static bool s_dumpLightsThisFrame = false;

struct RTGLDumpEntry {
    float              dist;
    uint64_t           uid;
    char               kind[16];
    idVec3             pos;
    float              radius;
    float              intensity;
    RgColor4DPacked32  color;
    bool               hasDir;
    idVec3             dir;
    float              aInDeg, aOutDeg;
    char               result[16];
};
static idList<RTGLDumpEntry> s_dumpEntries;
static idVec3                s_dumpCamPos;

static void RTGL_DumpLight( uint64_t uid, const char *kind,
                             const idVec3 &pos, float radius, float intensity,
                             RgColor4DPacked32 color, bool hasDir,
                             const idVec3 &dir, float aInDeg, float aOutDeg,
                             const char *result ) {
    if ( !s_dumpLightsThisFrame ) { return; }
    RTGLDumpEntry e;
    memset( &e, 0, sizeof( e ) );
    e.dist      = ( pos - s_dumpCamPos ).Length();
    e.uid       = uid;
    idStr::snPrintf( e.kind,   sizeof( e.kind ),   "%s", kind   ? kind   : "" );
    idStr::snPrintf( e.result, sizeof( e.result ), "%s", result ? result : "" );
    e.pos = pos; e.radius = radius; e.intensity = intensity; e.color = color;
    e.hasDir = hasDir; e.dir = dir; e.aInDeg = aInDeg; e.aOutDeg = aOutDeg;
    s_dumpEntries.Append( e );
}

// Insertion sort (nearest first) + print. Dump N is small; C++98, no STL.
static void RTGL_DumpFlush( void ) {
    RTGLDumpEntry *a = s_dumpEntries.Ptr();
    int n = s_dumpEntries.Num();
    for ( int i = 1; i < n; i++ ) {
        RTGLDumpEntry key = a[i];
        int j = i - 1;
        while ( j >= 0 && a[j].dist > key.dist ) { a[j + 1] = a[j]; j--; }
        a[j + 1] = key;
    }
    for ( int i = 0; i < n; i++ ) {
        const RTGLDumpEntry &e = a[i];
        char aabb[ 96 ];
        if ( e.radius > 0.0f ) {
            idStr::snPrintf( aabb, sizeof( aabb ),
                "aabb=(%7.1f %7.1f %7.1f)..(%7.1f %7.1f %7.1f)",
                e.pos.x - e.radius, e.pos.y - e.radius, e.pos.z - e.radius,
                e.pos.x + e.radius, e.pos.y + e.radius, e.pos.z + e.radius );
        } else {
            idStr::snPrintf( aabb, sizeof( aabb ), "aabb=point/infinite" );
        }
        if ( e.hasDir ) {
            common->Printf(
                "[RTGL L] f=%d #%d uid=%llu %s d=%8.1f pos=(%8.1f %8.1f %8.1f) "
                "r=%8.2f I=%9.1f col=#%08X dir=(%6.2f %6.2f %6.2f) "
                "coneDeg=(%5.1f %5.1f) %s %s\n",
                g_rtglFrameIndex, i, (unsigned long long)e.uid, e.kind, e.dist,
                e.pos.x, e.pos.y, e.pos.z, e.radius, e.intensity, (unsigned int)e.color,
                e.dir.x, e.dir.y, e.dir.z, e.aInDeg, e.aOutDeg, aabb, e.result );
        } else {
            common->Printf(
                "[RTGL L] f=%d #%d uid=%llu %s d=%8.1f pos=(%8.1f %8.1f %8.1f) "
                "r=%8.2f I=%9.1f col=#%08X %s %s\n",
                g_rtglFrameIndex, i, (unsigned long long)e.uid, e.kind, e.dist,
                e.pos.x, e.pos.y, e.pos.z, e.radius, e.intensity, (unsigned int)e.color,
                aabb, e.result );
        }
    }
}

// --- (A) light gizmo geometry ---------------------------------------------
// Append one triangle (three fresh verts; no index sharing needed at this size).
static void RTGL_VizTri( idList<RgPrimitiveVertex> &verts, idList<uint32_t> &idx,
                         const idVec3 &a, const idVec3 &b, const idVec3 &c,
                         const idVec3 &center, RgColor4DPacked32 col ) {
    uint32_t base = (uint32_t)verts.Num();
    const idVec3 *p[3] = { &a, &b, &c };
    for ( int i = 0; i < 3; i++ ) {
        RgPrimitiveVertex v;
        memset( &v, 0, sizeof( v ) );
        v.position[0] = p[i]->x;
        v.position[1] = p[i]->y;
        v.position[2] = p[i]->z;
        idVec3 n = *p[i] - center;
        n.Normalize();
        v.normalPacked = RTGL_PackNormal( n.x, n.y, n.z );
        v.color = col;
        verts.Append( v );
        idx.Append( base + (uint32_t)i );
    }
}

// Solid UV sphere.
static void RTGL_VizBuildSolid( idList<RgPrimitiveVertex> &verts,
                                idList<uint32_t> &idx,
                                const idVec3 &c, float r, RgColor4DPacked32 col ) {
    const int LAT = 8, LON = 12;
    const float PI = 3.14159265358979f;
    idVec3 g[LAT + 1][LON + 1];
    for ( int la = 0; la <= LAT; la++ ) {
        float phi = (float)la / (float)LAT * PI;
        for ( int lo = 0; lo <= LON; lo++ ) {
            float th = (float)lo / (float)LON * 2.0f * PI;
            idVec3 d( sinf(phi) * cosf(th), sinf(phi) * sinf(th), cosf(phi) );
            g[la][lo] = c + d * r;
        }
    }
    for ( int la = 0; la < LAT; la++ ) {
        for ( int lo = 0; lo < LON; lo++ ) {
            RTGL_VizTri( verts, idx, g[la][lo],     g[la+1][lo],   g[la+1][lo+1], c, col );
            RTGL_VizTri( verts, idx, g[la][lo],     g[la+1][lo+1], g[la][lo+1],   c, col );
        }
    }
}

// Wireframe: three orthogonal great-circle rings as thin radial ribbons.
static void RTGL_VizBuildWire( idList<RgPrimitiveVertex> &verts,
                               idList<uint32_t> &idx,
                               const idVec3 &c, float r, RgColor4DPacked32 col ) {
    const int K = 24;
    const float t = r * 0.04f;   // ribbon half-thickness
    const float PI = 3.14159265358979f;
    idVec3 U[3] = { idVec3(1,0,0), idVec3(1,0,0), idVec3(0,1,0) };
    idVec3 V[3] = { idVec3(0,1,0), idVec3(0,0,1), idVec3(0,0,1) };
    for ( int ring = 0; ring < 3; ring++ ) {
        for ( int k = 0; k < K; k++ ) {
            float a0 = (float)k     / (float)K * 2.0f * PI;
            float a1 = (float)(k+1) / (float)K * 2.0f * PI;
            idVec3 d0 = U[ring] * cosf(a0) + V[ring] * sinf(a0);
            idVec3 d1 = U[ring] * cosf(a1) + V[ring] * sinf(a1);
            idVec3 p0 = c + d0 * r, p1 = c + d1 * r;
            idVec3 q0 = p0 - d0 * t, q1 = p0 + d0 * t;
            idVec3 q2 = p1 + d1 * t, q3 = p1 - d1 * t;
            RTGL_VizTri( verts, idx, q0, q1, q2, c, col );
            RTGL_VizTri( verts, idx, q0, q2, q3, c, col );
        }
    }
}

static void RTGL_EmitLightViz( const idVec3 &pos, float radius,
                               RgColor4DPacked32 color, int mode, uint64_t uid ) {
    if ( !rg.rgUploadMeshPrimitive || radius <= 0.0f ) { return; }
    static idList<RgPrimitiveVertex> verts;
    static idList<uint32_t>          idx;
    verts.Clear();
    idx.Clear();
    if ( mode == 2 ) { RTGL_VizBuildWire(  verts, idx, pos, radius, color ); }
    else             { RTGL_VizBuildSolid( verts, idx, pos, radius, color ); }
    if ( idx.Num() == 0 ) { return; }

    RgMeshInfo mesh = {};
    mesh.sType = RG_STRUCTURE_TYPE_MESH_INFO;
    mesh.pNext = NULL;
    mesh.flags = 0;
    // High bit keeps these far away from P2 geometry uniqueObjectIDs.
    mesh.uniqueObjectID = 0x4000000000000000ULL | uid;
    mesh.pMeshName = NULL;
    RTGL_IdentityTransform( &mesh.transform );
    mesh.isExportable = 0;
    mesh.animationTime = 0.0f;
    mesh.localLightsIntensity = 1.0f;

    RgMeshPrimitiveInfo prim = {};
    prim.sType = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_INFO;
    prim.pNext = NULL;
    prim.flags = RG_MESH_PRIMITIVE_NO_SHADOW; // don't perturb the lighting under inspection
    prim.primitiveIndexInMesh = 0;
    prim.pVertices = verts.Ptr();
    prim.vertexCount = (uint32_t)verts.Num();
    prim.pIndices = idx.Ptr();
    prim.indexCount = (uint32_t)idx.Num();
    prim.pTextureName = NULL;
    prim.textureFrame = 0;
    prim.color = color;
    prim.emissive = 4.0f;
    prim.classicLight = 1.0f;

    rg.rgUploadMeshPrimitive( &mesh, &prim );
}

// Build + upload a single light. C++98: zero-init structs then fill fields.
static void RTGL_UploadOneLight( const viewLight_t *vLight, const idVec3 &camPos ) {
    const idRenderLightLocal *ldef = vLight->lightDef;
    if ( !ldef ) { g_rtglStats.skippedLights++; return; }
    const renderLight_t &p = ldef->parms;
    const idMaterial *mat = vLight->lightShader;

    // --- skip categories with no RTGL1 equivalent ---
    if ( mat && mat->IsFogLight() && rt_light_skipFog.GetBool() ) {
        g_rtglStats.skippedLights++; g_rtglStats.skippedFog++; return;
    }
    if ( mat && mat->IsBlendLight() && rt_light_skipBlend.GetBool() ) {
        g_rtglStats.skippedLights++; g_rtglStats.skippedBlend++; return;
    }
    if ( mat && rt_light_skipSkyLights.GetBool() ) {
        // Skip fake ambient sky lights (e.g. lights/squarelight1sky). In original
        // Doom 3, mappers placed huge point lights with `lights/*sky*` shaders
        // to approximate outdoor sky fill without an environment cubemap or GI.
        // In RTGL1, physical sky illumination is already evaluated from the skybox
        // cubemap; uploading these fake omni lights double-counts sky energy and
        // causes violent lighting pops when portal/frustum culling toggles them
        // on and off near windows (such as light_4874 outside the mars_city1 kitchen).
        if ( idStr::FindText( mat->GetName(), "sky", false ) >= 0 ) {
            g_rtglStats.skippedLights++; return;
        }
    }
    if ( p.noShadows && rt_light_skipNoShadows.GetBool() ) {
        g_rtglStats.skippedLights++; g_rtglStats.skippedNoShadow++; return;
    }
    // Mirror Doom 3 engine culling (R_RemoveUnecessaryViewLights): if a light
    // has no visible surface interactions in this view, do not upload it.
    // In original Doom 3, lights only illuminate surfaces that have explicit
    // interactions generated; without interactions, a light affects 0 surfaces.
    // In RTGL1 path tracing, an uploaded spherical light is omnidirectional and
    // unconstrained by Doom 3 interaction lists, so distant sky/volume lights
    // (e.g. light_5318 at z=5728, radius=5000) that only have portal contact
    // would bleed onto indoor geometry if not culled here.
    if ( rt_light_skipNoInteractions.GetBool() ) {
        if ( !vLight->localInteractions && !vLight->globalInteractions && !vLight->translucentInteractions ) {
            g_rtglStats.skippedLights++;
            return;
        }
    }

    // --- stable unique id ---
    uint64_t uid = 1ULL + (uint64_t)(uint32_t)( ldef->index * 2 + 1 );
    if ( RTGL_SeenLightID( uid ) ) {
        g_rtglStats.lightDuplicates++;
        g_rtglStats.skippedLights++;
        return;
    }

    // --- budget ---
    int maxLights = rt_light_maxLights.GetInteger();
    if ( maxLights > 0 && g_rtglStats.lightsUploaded >= maxLights ) {
        g_rtglStats.skippedLights++; return;
    }

    RgColor4DPacked32 color = RTGL_LightColor( vLight );
    float intensity = rt_light_intensity.GetFloat();
    float dirIntensity = rt_light_dirIntensity.GetFloat();
    if ( dirIntensity < 0.0f ) { dirIntensity = 0.0f; }
    float radiusScale = rt_light_radiusScale.GetFloat();
    if ( radiusScale <= 0.0f ) { radiusScale = 1.0f; }

    // Origin in raw DOOM units.
    // For point lights, Doom 3 mappers often set `light_center` (up to cientos
    // of units) solely to shift the N.L bump-map highlight or stencil-shadow
    // apex, while the light's actual 3D attenuation volume (falloff & XY cookie)
    // ALWAYS stays centered on `p.origin`! On `noshadows 1` lights (e.g. Hangar
    // upper blue lights `light_4933..5261` with `origin.z=664` and
    // `light_center=(-84,0,124)`), `globalLightOrigin` pushes the emitter 150
    // units behind the angled ceiling fixture (`z=788` behind `z=729..746`) where
    // RTGL1's physical shadow rays are 100% occluded by the ceiling. Even on
    // shadowed fill lights (e.g. `light_5253` in Hangar with `origin.z=-172` and
    // `light_center=(32,320,868)`), an extreme `light_center` moves the sphere
    // light 868 units away from the volume center. Clamp `lightCenter` to at
    // most 25% of `lightRadius` (and use `p.origin` directly when `p.noShadows`
    // is set) so the physical emitter stays inside the room and matches Doom 3's
    // attenuation box center.
    idVec3 pos = vLight->globalLightOrigin;
    if ( p.pointLight && !p.parallel ) {
        if ( p.noShadows ) {
            pos = p.origin;
        } else {
            idVec3 ofs = vLight->globalLightOrigin - p.origin;
            float maxOfs = p.lightRadius.Length() * 0.25f;
            if ( ofs.Length() > maxOfs && maxOfs > 0.0f ) {
                ofs.Normalize();
                pos = p.origin + ofs * maxOfs;
            }
        }
    } else if ( !p.pointLight ) {
        // Projected/spot lights (e.g. player's flashlight) always emit from p.origin
        pos = p.origin;
    }

    // Compute raw Doom-unit radius for filtering and bounds checking
    float rawRadius = p.lightRadius.Length();
    if ( !p.pointLight && rawRadius < 1.0f ) {
        rawRadius = ( p.end - p.start ).Length();
    }
    if ( rawRadius < 1.0f ) {
        rawRadius = 256.0f;
    }

    // Size-driven intensity scaling (EXPERIMENT, OFF by default 2026-11-13).
    // Originally added to make intensity grow with light extent, but RTGL1
    // surface irradiance ~= color*intensity/d^2 does NOT depend on emitter
    // radius: intensity is the ONLY scene-brightness knob. With refRadius=256,
    // mars_city1's visible small lamps (extent 70..200) got (E/256)^2 < 1 and
    // their intensity fell from 200 to ~15..113 -> the scene could not be lit.
    // Kept as an OFF-by-default knob (rt_light_sizeExponent=0 => no-op). If
    // re-enabled, use a SMALL refRadius and raise the base intensity, otherwise
    // lamps are dimmed. The physically-correct size fix is clamping the emitter
    // RADIUS (penumbra only), not scaling energy.
    // NOTE: color is NOT folded in here -- RTGL1 already does color*intensity
    // and RTGL_LightColor returns the raw, brightness-carrying RGB.
    if ( !p.parallel ) {
        float sizeExponent = rt_light_sizeExponent.GetFloat();
        float refRadius = rt_light_refRadius.GetFloat();
        if ( refRadius < 1.0f ) { refRadius = 1.0f; }
        if ( sizeExponent != 0.0f && rawRadius > 0.0f ) {
            intensity *= powf( rawRadius / refRadius, sizeExponent );
            if ( intensity < 0.0f ) { intensity = 0.0f; }
        }
    }

    // Step 1 / Plan B: Skip massive sky-dome / level-spanning filler lights.
    // In original Doom 3, lights with radius > 2000 are fake sky/ambient lights
    // that were strictly bounded by portals/surfaces. In RTGL1, an uploaded
    // omni light has unattenuated physical GI that floods the entire map.
    float maxRadius = rt_light_maxRadius.GetFloat();
    if ( !p.parallel && maxRadius > 0.0f && rawRadius > maxRadius ) {
        g_rtglStats.skippedLights++;
        RTGL_DumpLight( uid, "SKIP_MAX_RAD", pos, rawRadius * radiusScale, intensity, color, false, pos, 0.0f, 0.0f, "skipped" );
        return;
    }

    // Step 1 / Plan A: Spatial bounding box vertical culling.
    // In Doom 3, point light falloff is strictly zero outside its bounding box.
    // If the entire light volume is far above the camera (by > 256 units),
    // in Doom 3 no surface near the viewer can receive direct illumination.
    // In RTGL1, omni lights bleed infinitely downward through open sky/portals.
    if ( rt_light_cullFrustumBounds.GetBool() && p.pointLight && ldef->frustumTris ) {
        float bottomZ = ldef->frustumTris->bounds[0].z;
        if ( camPos.z < bottomZ && ( bottomZ - camPos.z ) > 256.0f ) {
            g_rtglStats.skippedLights++;
            RTGL_DumpLight( uid, "SKIP_BOUNDS", pos, rawRadius * radiusScale, intensity, color, false, pos, 0.0f, 0.0f, "skipped" );
            return;
        }
    }

    RgLightInfo base;
    memset( &base, 0, sizeof( base ) );
    base.sType = RG_STRUCTURE_TYPE_LIGHT_INFO;
    base.pNext = NULL;
    base.uniqueID = uid;
    base.isExportable = 0;

    RgResult r = RG_RESULT_SUCCESS;
    float vizRadius = 0.0f;   // (A) the TRUE radius uploaded to RTGL1

    if ( p.parallel ) {
        // Default ON: a bounded, frustum-culled Doom 3 parallel light must NOT
        // become the unbounded RTGL1 global sun (see header block: whole-scene
        // bright/dark pop on small camera rotations). rt_sun_enable is the
        // view-independent way to get a global sun.
        // Skip tiny local parallel lights (e.g. mars_city1 `light_5262` and
        // `light_5351` with `light_radius = 32 48 32`, `rawRadius = 66`): in
        // Doom 3 they only touch a tiny 64-unit doorway trim box, whereas in
        // RTGL1 `RgLightDirectionalEXT` becomes an UNBOUNDED global sun for the
        // entire map that hijacks 50% of ReSTIR DI reservoirs (`initCombinedReservoir`)
        // and pops the whole Hangar bright/dark when crossing the view frustum.
        // Only allow map-scale outdoor parallel suns (`rawRadius >= 500`).
        if ( rt_light_skipParallel.GetBool() || rawRadius < 500.0f ) {
            g_rtglStats.skippedLights++;
            g_rtglStats.skippedDir++;
            RTGL_DumpLight( uid, "SKIP_PARALLEL", pos, 0.0f, 0.0f, color, false, pos, 0.0f, 0.0f, "skipped" );
            return;
        }

        // RTGL1 LightManager allows exactly one directional light per frame;
        // extra parallel lights are skipped to avoid a per-frame debug::Error.
        if ( s_directionalUploaded ) {
            g_rtglStats.skippedLights++;
            g_rtglStats.skippedDir++;
            RTGL_DumpLight( uid, "SKIP_DIR_LIMIT", pos, 0.0f, intensity, color, false, pos, 0.0f, 0.0f, "skipped" );
            return;
        }
        s_directionalUploaded = true;
        // Parallel (infinite) light: RgLightDirectionalEXT.
        // lightCenter gives the direction TO the light at infinity; the light
        // travels opposite to that. Fallback: -Z if degenerate.
        idVec3 dir = p.lightCenter;
        if ( dir.Length() < 0.001f ) { dir.Set( 0.0f, 0.0f, -1.0f ); }
        dir.Normalize();
        dir = -dir; // from the light toward the scene

        RgLightDirectionalEXT d;
        memset( &d, 0, sizeof( d ) );
        d.sType = RG_STRUCTURE_TYPE_LIGHT_DIRECTIONAL_EXT;
        d.pNext = NULL;
        d.color = color;
        d.intensity = dirIntensity;   // FIX(P4): dedicated lux knob, not the 200 lumen sphere/spot scalar
        d.direction = RTGL_V3( dir.x, dir.y, dir.z );
        d.angularDiameterDegrees = 0.5f;
        base.pNext = &d;
        r = rg.rgUploadLight( &base );
        if ( r == RG_RESULT_SUCCESS ) { g_rtglStats.directionalLights++; }
        RTGL_DumpLight( uid, "DIR", pos, 0.0f, dirIntensity, color, true, dir, 0.0f, 0.0f, r == RG_RESULT_SUCCESS ? "ok" : "UPLOAD_FAIL" );

    } else if ( !p.pointLight ) {
        // Projected / cookie light -> RgLightSpotEXT.
        // The exact world-space projection direction is given by vLight->lightProject[2].Normal()
        // (which is already transformed by the light's world orientation matrix).
        idVec3 dir = vLight->lightProject[2].Normal();
        if ( dir.Length() < 0.001f ) {
            dir = p.axis * p.target;
        }
        if ( dir.Length() < 0.001f ) { dir.Set( 0.0f, 0.0f, -1.0f ); }
        dir.Normalize();

        // Offset the emission point slightly forward along the projection direction (+12 Doom units)
        // so it clears the flashlight lens and weapon barrel geometry completely.
        idVec3 emitterPos = pos + dir * 12.0f;

        // CRITICAL: RTGL1's GLSL decodeAsSpotLight() has `l.radius = 0.05` HARDCODED.
        // In EncodeAsSpotLight(), intensity is divided by (PI * radius * radius).
        // If we passed radius = 32.0f, intensity was divided by 32^2 while the shader
        // evaluated solid angle with 0.05^2, resulting in a 409,600x energy loss!
        // We MUST pass s.radius = 0.05f to match GZDoom-RT and preserve full physical irradiance.
        const float spotRadius = 0.05f;
        vizRadius = spotRadius;

        float outerDeg = rt_light_spotAngle.GetFloat();
        if ( outerDeg <= 0.0f ) { outerDeg = 45.0f; }
        float outer = outerDeg * ( 3.14159265358979323846f / 180.0f );

        RgLightBoundsAndCookieEXT bcExt;
        const bool hasBC = RTGL_BuildLightBoundsAndCookie( vLight, &bcExt );

        RgLightSpotEXT s;
        memset( &s, 0, sizeof( s ) );
        s.sType = RG_STRUCTURE_TYPE_LIGHT_SPOT_EXT;
        s.pNext = hasBC ? &bcExt : NULL;
        s.color = color;
        s.intensity = intensity;
        s.position = RTGL_V3( emitterPos.x, emitterPos.y, emitterPos.z );
        s.direction = RTGL_V3( dir.x, dir.y, dir.z );
        s.radius = spotRadius;
        s.angleOuter = outer;
        s.angleInner = outer * 0.6f; // soft penumbra
        base.pNext = &s;
        r = rg.rgUploadLight( &base );
        if ( r == RG_RESULT_SUCCESS ) { g_rtglStats.spotLights++; }
        RTGL_DumpLight( uid, "SPOT", emitterPos, spotRadius, intensity, color, true, dir, outerDeg * 0.6f, outerDeg, r == RG_RESULT_SUCCESS ? "ok" : "UPLOAD_FAIL" );

    } else {
        // Omni point light -> RgLightSphericalEXT.
        float radius = rawRadius * radiusScale;
        vizRadius = radius;

        RgLightBoundsAndCookieEXT bcExt;
        const bool hasBC = RTGL_BuildLightBoundsAndCookie( vLight, &bcExt );

        RgLightSphericalEXT sph;
        memset( &sph, 0, sizeof( sph ) );
        sph.sType = RG_STRUCTURE_TYPE_LIGHT_SPHERICAL_EXT;
        sph.pNext = hasBC ? &bcExt : NULL;
        sph.color = color;
        sph.intensity = intensity;
        sph.position = RTGL_V3( pos.x, pos.y, pos.z );
        sph.radius = radius;
        base.pNext = &sph;
        r = rg.rgUploadLight( &base );
        if ( r == RG_RESULT_SUCCESS ) { g_rtglStats.sphereLights++; }
        idVec3 noDir( 0.0f, 0.0f, 0.0f );
        RTGL_DumpLight( uid, "SPHERE", pos, radius, intensity, color, false, noDir, 0.0f, 0.0f, r == RG_RESULT_SUCCESS ? "ok" : "UPLOAD_FAIL" );
    }

    // (A) Debug viz: show the light's true position + uploaded radius. Only
    // sphere/spot lights have a finite radius; directional lights are skipped.
    int vizMode = rt_dbg_lightViz.GetInteger();
    if ( vizMode > 0 && vizRadius > 0.0f ) {
        RTGL_EmitLightViz( pos, vizRadius, color, vizMode, uid );
    }

    if ( r == RG_RESULT_SUCCESS ) {
        g_rtglStats.lightsUploaded++;
    } else {
        g_rtglStats.failedLightUploads++;
        g_rtglStats.skippedLights++;
        common->Warning( "[RTGL P4] rgUploadLight(uid=%llu) failed (%s)",
                         (unsigned long long)uid, RTGL_ResultString( r ) );
    }
}

// Optional debug / ambient-fill directional "sun" (rt_sun_enable).
static void RTGL_UploadSun( void ) {
    float inten = rt_sun_intensity.GetFloat();
    if ( inten <= 0.0f ) { return; }
    idVec3 dir( rt_sun_dirX.GetFloat(), rt_sun_dirY.GetFloat(), rt_sun_dirZ.GetFloat() );
    if ( dir.Length() < 0.001f ) { dir.Set( 0.0f, 0.0f, -1.0f ); }
    dir.Normalize();

    RgLightDirectionalEXT d;
    memset( &d, 0, sizeof( d ) );
    d.sType = RG_STRUCTURE_TYPE_LIGHT_DIRECTIONAL_EXT;
    d.pNext = NULL;
    d.color = RTGL_PackColorF( 1.0f, 0.98f, 0.92f, 1.0f );
    d.intensity = inten;
    d.direction = RTGL_V3( dir.x, dir.y, dir.z );
    d.angularDiameterDegrees = 0.5f;

    RgLightInfo base;
    memset( &base, 0, sizeof( base ) );
    base.sType = RG_STRUCTURE_TYPE_LIGHT_INFO;
    base.pNext = &d;
    base.uniqueID = 0xFFFFFFFFFFFFFFFFULL; // reserved: never collides with lightDef->index
    base.isExportable = 0;

    RgResult r = rg.rgUploadLight( &base );
    if ( r == RG_RESULT_SUCCESS ) {
        g_rtglStats.lightsUploaded++;
        g_rtglStats.directionalLights++;
    } else {
        g_rtglStats.failedLightUploads++;
    }
    if ( s_dumpLightsThisFrame ) { common->Printf( "[RTGL L] f=%d uid=reserved SUN dir=(%6.2f %6.2f %6.2f) I=%9.1f %s\n", g_rtglFrameIndex, dir.x, dir.y, dir.z, inten, r == RG_RESULT_SUCCESS ? "ok" : "UPLOAD_FAIL" ); }
}

// Main P4 entry: iterate all RC_DRAW_VIEW views, upload each viewLights chain.
// Different views (main / subview) can reference the same lightDef; the
// per-frame duplicate set prevents double-upload.
void RTGL_SubmitLights( const void *cmdHead ) {
    if ( !RTGL_FrameActive() || !rg.rgUploadLight ) { return; }
    if ( !rt_light_enable.GetBool() && !rt_sun_enable.GetBool() ) { return; }

    const emptyCommand_t *cmd = (const emptyCommand_t *)cmdHead;
    if ( !cmd ) { return; }

    int dumpInterval = rt_dbg_dumpLights.GetInteger();
    s_dumpLightsThisFrame = ( dumpInterval > 0 ) && ( ( g_rtglFrameIndex % dumpInterval ) == 0 );

    // (C) Find the main camera position so the per-light dump can report
    // distance-to-camera and be sorted nearest-first. Same pick rule as P2
    // geometry: first non-subview, non-mirror RC_DRAW_VIEW.
    s_dumpCamPos.Set( 0.0f, 0.0f, 0.0f );
    for ( const emptyCommand_t *c = cmd; c; c = (const emptyCommand_t *)c->next ) {
        if ( c->commandId == RC_DRAW_VIEW ) {
            const drawSurfsCommand_t *dv = (const drawSurfsCommand_t *)c;
            if ( dv->viewDef && !dv->viewDef->isSubview && !dv->viewDef->isMirror ) {
                s_dumpCamPos = dv->viewDef->renderView.vieworg;
                break;
            }
        }
    }
    s_dumpEntries.Clear();

    if ( s_dumpLightsThisFrame ) { common->Printf( "[RTGL L] === frame %d light list (rt_dbg_dumpLights=%d cam=(%8.1f %8.1f %8.1f), nearest first) ===\n", g_rtglFrameIndex, dumpInterval, s_dumpCamPos.x, s_dumpCamPos.y, s_dumpCamPos.z ); }
    if ( s_dumpLightsThisFrame ) { common->Printf( "[RTGL L] note: pos/radius RAW Doom units; I=rt_light_intensity; d=dist to cam; aabb=world light bound; RTGL1 radiance=I/(pi*radius*radius)\n" ); }

    for ( const emptyCommand_t *c = cmd; c; c = (const emptyCommand_t *)c->next ) {
        if ( c->commandId != RC_DRAW_VIEW ) { continue; }
        const drawSurfsCommand_t *dv = (const drawSurfsCommand_t *)c;
        const viewDef_t *v = dv->viewDef;
        if ( !v ) { continue; }
        if ( rt_dbg_skipSubViews.GetBool() &&
             ( v->isSubview || v->isMirror || v->isXraySubview ) ) {
            continue;
        }
        if ( !rt_light_enable.GetBool() ) { break; } // sun-only: still go to sun below
        for ( const viewLight_t *vl = v->viewLights; vl; vl = vl->next ) {
            g_rtglStats.lightsSeen++;
            RTGL_UploadOneLight( vl, v->renderView.vieworg );
        }
    }

    if ( rt_sun_enable.GetBool() ) {
        RTGL_UploadSun();
    }
    if ( s_dumpLightsThisFrame ) { RTGL_DumpFlush(); }
    if ( s_dumpLightsThisFrame ) { common->Printf( "[RTGL L] === end frame %d: seen=%d up=%d skip=%d (sph=%d spot=%d dir=%d) ===\n", g_rtglFrameIndex, g_rtglStats.lightsSeen, g_rtglStats.lightsUploaded, g_rtglStats.skippedLights, g_rtglStats.sphereLights, g_rtglStats.spotLights, g_rtglStats.directionalLights ); s_dumpLightsThisFrame = false; }
}

#endif // D3_RTGL
