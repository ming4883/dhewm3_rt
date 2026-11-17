#include "sys/platform.h"
#include "framework/Common.h"

#ifdef D3_RTGL

#include "renderer/rtgl/rtgl_internal.h"

// ===========================================================================
// P4 - Sky and illumination params + Path A rasterized 6-face skybox cube
//
// Doom 3 skyboxes (e.g. textures/skies/commoutside in mars_city1, marssky,
// hellsky, desert) are BSP brushes with `forceOpaque`, `noshadows`, and stages
// carrying `cameraCubeMap env/<name>` + `texgen skybox` / `texgen wobblesky`.
//
// Path A (engine-side, no RTGL1.dll changes):
// 1. Sky brush surfaces are classified as RG_MESH_PRIMITIVE_SKY_VISIBILITY |
//    RG_MESH_PRIMITIVE_NO_SHADOW (non-translucent) so they enter the BLAS in
//    slot PV_WORLD_2 (INSTANCE_CUSTOM_INDEX_FLAG_SKY). Primary and reflection
//    rays that hit a sky brush resolve to `storeSky()` while geometry behind
//    the sky brush remains occluded and no shadow is cast by the sky ceiling.
// 2. When a sky surface is observed during RTGL_SubmitCommands, we record its
//    active skybox stages (cubemap base name, evaluated stage color, blend
//    mode, and wobblesky 3x3 rotation matrix).
// 3. RTGL_SubmitSkyGeometry() uploads a 6-face cube around skyViewerPosition
//    (0,0,0) with RG_MESH_PRIMITIVE_SKY, binding the 6 cameraCubeMap faces
//    (_forward, _back, _left, _right, _up, _down rotated to match GL cube
//    axes +X, -X, +Y, -Y, +Z, -Z), and RTGL_BuildSkyParams switches skyType
//    to RG_SKY_TYPE_RASTERIZED_GEOMETRY. RTGL1 then rasterizes the 6 faces
//    into both the primary albedo buffer (DrawSkyToAlbedo) and the environment
//    cubemap (DrawSkyToCubemap) for indirect GI bounces and reflections.
// ===========================================================================

#define RTGL_MAX_SKY_LAYERS 4

struct rtglSkyLayer_t {
    idStr               cubeName;
    cubeFiles_t         cubeType;
    RgColor4DPacked32   color;
    bool                translucent;
    bool                additive;
    float               rot[3][3];  // row-major 3x3 inverse wobble transform
};

static rtglSkyLayer_t   s_skyLayers[ RTGL_MAX_SKY_LAYERS ];
static int              s_numSkyLayers = 0;
static bool             s_hasRasterSky = false;

void RTGL_SkyBeginFrame( void ) {
    s_numSkyLayers = 0;
    s_hasRasterSky = false;
}

void RTGL_SkyShutdown( void ) {
    s_numSkyLayers = 0;
    s_hasRasterSky = false;
}

static void RTGL_BuildWobbleRotation( const idMaterial *mat, const float *regs,
                                      float floatTime, float outRot[3][3] ) {
    // Identity default
    memset( outRot, 0, sizeof( float ) * 9 );
    outRot[0][0] = 1.0f;
    outRot[1][1] = 1.0f;
    outRot[2][2] = 1.0f;

    if ( !mat || !regs || mat->Texgen() != TG_WOBBLESKY_CUBE ) {
        return;
    }
    const int *parms = mat->GetTexGenRegisters();
    if ( !parms ) {
        return;
    }

    float wobbleDegrees = regs[ parms[0] ] * idMath::PI / 180.0f;
    float wobbleSpeed   = regs[ parms[1] ] * 2.0f * idMath::PI / 60.0f;
    float rotateSpeed   = regs[ parms[2] ] * 2.0f * idMath::PI / 60.0f;

    float a = floatTime * wobbleSpeed;
    float s = sinf( a ) * sinf( wobbleDegrees );
    float c = cosf( a ) * sinf( wobbleDegrees );
    float z = cosf( wobbleDegrees );

    idVec3 axis[3];
    axis[2][0] = c;
    axis[2][1] = s;
    axis[2][2] = z;

    axis[1][0] = -sinf( a * 2.0f ) * sinf( wobbleDegrees );
    axis[1][2] = -s * sinf( wobbleDegrees );
    float y2 = 1.0f - ( axis[1][0] * axis[1][0] + axis[1][2] * axis[1][2] );
    axis[1][1] = ( y2 > 0.0f ) ? sqrtf( y2 ) : 0.0f;

    axis[1] -= ( axis[2] * axis[1] ) * axis[2];
    axis[1].Normalize();
    axis[0].Cross( axis[1], axis[2] );

    s = sinf( rotateSpeed * floatTime );
    c = cosf( rotateSpeed * floatTime );

    // In R_WobbleskyTexGen (tr_light.cpp:269-298), texCoord = M * dir where
    // M's rows are r0, r1, r2 below. Rotating the skybox geometry by M^T
    // makes world ray `dir` hit the exact cubemap texel `M * dir`.
    float r0[3] = {
        axis[0][0] * c + axis[1][0] * s,
        axis[0][1] * c + axis[1][1] * s,
        axis[0][2] * c + axis[1][2] * s
    };
    float r1[3] = {
        axis[1][0] * c - axis[0][0] * s,
        axis[1][1] * c - axis[0][1] * s,
        axis[1][2] * c - axis[0][2] * s
    };
    float r2[3] = {
        axis[2][0],
        axis[2][1],
        axis[2][2]
    };

    // Transpose M so outRot = M^T
    outRot[0][0] = r0[0]; outRot[0][1] = r1[0]; outRot[0][2] = r2[0];
    outRot[1][0] = r0[1]; outRot[1][1] = r1[1]; outRot[1][2] = r2[1];
    outRot[2][0] = r0[2]; outRot[2][1] = r1[2]; outRot[2][2] = r2[2];
}

void RTGL_ObserveSkySurface( const drawSurf_t *surf, float floatTime ) {
    if ( s_numSkyLayers > 0 || !surf || !surf->material ) {
        return;
    }
    const idMaterial *mat = surf->material;
    const float *regs = surf->shaderRegisters;

    float baseRot[3][3];
    RTGL_BuildWobbleRotation( mat, regs, floatTime, baseRot );

    for ( int i = 0; i < mat->GetNumStages() && s_numSkyLayers < RTGL_MAX_SKY_LAYERS; i++ ) {
        const shaderStage_t *st = mat->GetStage( i );
        if ( !st || !st->texture.image ) { continue; }
        if ( regs && st->conditionRegister >= 0 && regs[ st->conditionRegister ] == 0.0f ) {
            continue;
        }
        const idImage *img = st->texture.image;
        if ( img->type != TT_CUBIC || img->imgName.Length() <= 0 ) {
            continue;
        }

        // Verify at least face 0 can be loaded & provided (e.g. env/cloudy_forward.tga)
        const char *face0 = RTGL_ProvideSkyFaceTexture( img->imgName.c_str(), img->cubeFiles, 0 );
        if ( !face0 ) {
            continue;
        }

        float cr = 1.0f, cg = 1.0f, cb = 1.0f, ca = 1.0f;
        if ( regs ) {
            cr = regs[ st->color.registers[0] ];
            cg = regs[ st->color.registers[1] ];
            cb = regs[ st->color.registers[2] ];
            ca = regs[ st->color.registers[3] ];
            if ( cr < 0.0f ) { cr = 0.0f; } else if ( cr > 1.0f ) { cr = 1.0f; }
            if ( cg < 0.0f ) { cg = 0.0f; } else if ( cg > 1.0f ) { cg = 1.0f; }
            if ( cb < 0.0f ) { cb = 0.0f; } else if ( cb > 1.0f ) { cb = 1.0f; }
            if ( ca < 0.0f ) { ca = 0.0f; } else if ( ca > 1.0f ) { ca = 1.0f; }
        }

        int srcB = st->drawStateBits & GLS_SRCBLEND_BITS;
        int dstB = st->drawStateBits & GLS_DSTBLEND_BITS;
        bool isFirstLayer = ( s_numSkyLayers == 0 );
        bool translucent = false;
        bool additive = false;
        if ( !isFirstLayer ) {
            if ( dstB == GLS_DSTBLEND_ONE ) {
                translucent = true;
                additive = true;
                ca = 1.0f;
            } else if ( dstB == GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA || srcB == GLS_SRCBLEND_SRC_ALPHA ) {
                translucent = true;
                additive = false;
            }
        } else {
            ca = 1.0f; // base sky layer is always opaque onto the sky framebuffer
        }

        rtglSkyLayer_t &layer = s_skyLayers[ s_numSkyLayers++ ];
        layer.cubeName = img->imgName;
        layer.cubeType = img->cubeFiles;
        layer.color = RTGL_PackColorF( cr, cg, cb, ca );
        layer.translucent = translucent;
        layer.additive = additive;
        memcpy( layer.rot, baseRot, sizeof( baseRot ) );
    }
}

void RTGL_SubmitSkyGeometry( void ) {
    if ( s_numSkyLayers <= 0 || !rg.rgUploadMeshPrimitive ) {
        return;
    }

    // Half-size of the skybox cube around skyViewerPosition (0,0,0).
    // RTGL1's cameraNear is ~3 and cameraFar is 10000; 512 sits comfortably
    // inside the frustum for both DrawSkyToCubemap and DrawSkyToAlbedo.
    const float R = 512.0f;

    // For each cubemap face (0:+X, 1:-X, 2:+Y, 3:-Y, 4:+Z, 5:-Z), the 4
    // corner directions corresponding to UV (0,0), (1,0), (1,1), (0,1) under
    // OpenGL's cubemap convention (after R_LoadCubeImages's camera-to-cube
    // rotations):
    //   +X: sc = -z, tc = -y  -> u=0: z=+R, u=1: z=-R; v=0: y=+R, v=1: y=-R
    //   -X: sc = +z, tc = -y  -> u=0: z=-R, u=1: z=+R; v=0: y=+R, v=1: y=-R
    //   +Y: sc = +x, tc = +z  -> u=0: x=-R, u=1: x=+R; v=0: z=-R, v=1: z=+R
    //   -Y: sc = -x, tc = +z  -> u=0: x=+R, u=1: x=-R; v=0: z=-R, v=1: z=+R
    //   +Z: sc = +x, tc = -y  -> u=0: x=-R, u=1: x=+R; v=0: y=+R, v=1: y=-R
    //   -Z: sc = +x, tc = +y  -> u=0: x=-R, u=1: x=+R; v=0: y=-R, v=1: y=+R
    static const float kFaceCorners[6][4][3] = {
        // 0: +X
        { { +R, +R, +R }, { +R, +R, -R }, { +R, -R, -R }, { +R, -R, +R } },
        // 1: -X
        { { -R, +R, -R }, { -R, +R, +R }, { -R, -R, +R }, { -R, -R, -R } },
        // 2: +Y
        { { -R, +R, -R }, { +R, +R, -R }, { +R, +R, +R }, { -R, +R, +R } },
        // 3: -Y
        { { +R, -R, -R }, { -R, -R, -R }, { -R, -R, +R }, { +R, -R, +R } },
        // 4: +Z
        { { -R, +R, +R }, { +R, +R, +R }, { +R, -R, +R }, { -R, -R, +R } },
        // 5: -Z
        { { -R, -R, -R }, { +R, -R, -R }, { +R, +R, -R }, { -R, +R, -R } },
    };
    static const float kFaceUVs[4][2] = {
        { 0.0f, 0.0f },
        { 1.0f, 0.0f },
        { 1.0f, 1.0f },
        { 0.0f, 1.0f },
    };
    static const uint32_t kQuadIndices[6] = { 0, 1, 2, 0, 2, 3 };

    RgColor4DPacked32 white = RTGL_White();
    RgNormalPacked32  upNrm = RTGL_PackNormal( 0.0f, 0.0f, 1.0f );

    for ( int l = 0; l < s_numSkyLayers; l++ ) {
        const rtglSkyLayer_t &layer = s_skyLayers[l];
        // Pull inner overlay layers slightly closer so depth-LEQUAL always passes
        float scale = 1.0f - 0.01f * (float)l;

        RgMeshInfo mesh = {};
        mesh.sType = RG_STRUCTURE_TYPE_MESH_INFO;
        mesh.pNext = NULL;
        mesh.flags = 0;
        mesh.uniqueObjectID = 0x534B59424F580000ULL | (uint64_t)l;
        mesh.pMeshName = "rtgl_skybox";
        memset( &mesh.transform, 0, sizeof( mesh.transform ) );
        for ( int r = 0; r < 3; r++ ) {
            for ( int c = 0; c < 3; c++ ) {
                mesh.transform.matrix[r][c] = layer.rot[r][c] * scale;
            }
        }
        mesh.isExportable = 0;
        mesh.animationTime = 0.0f;
        mesh.localLightsIntensity = 1.0f;

        for ( int side = 0; side < 6; side++ ) {
            const char *texName = RTGL_ProvideSkyFaceTexture( layer.cubeName.c_str(), layer.cubeType, side );
            if ( !texName ) {
                continue;
            }

            RgPrimitiveVertex verts[4];
            memset( verts, 0, sizeof( verts ) );
            for ( int k = 0; k < 4; k++ ) {
                verts[k].position[0] = kFaceCorners[side][k][0];
                verts[k].position[1] = kFaceCorners[side][k][1];
                verts[k].position[2] = kFaceCorners[side][k][2];
                verts[k].normalPacked = upNrm;
                verts[k].texCoord[0] = kFaceUVs[k][0];
                verts[k].texCoord[1] = kFaceUVs[k][1];
                verts[k].color = white;
            }

            RgMeshPrimitiveFlags pflags = RG_MESH_PRIMITIVE_SKY | RG_MESH_PRIMITIVE_NO_SHADOW;
            float emissive = 0.0f;
            if ( layer.translucent ) {
                pflags |= RG_MESH_PRIMITIVE_TRANSLUCENT;
                if ( layer.additive ) {
                    emissive = 1.0f;
                }
            }

            RgMeshPrimitiveInfo prim = {};
            prim.sType = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_INFO;
            prim.pNext = NULL;
            prim.flags = pflags;
            prim.primitiveIndexInMesh = (uint32_t)side;
            prim.pVertices = verts;
            prim.vertexCount = 4;
            prim.pIndices = kQuadIndices;
            prim.indexCount = 6;
            prim.pTextureName = texName;
            prim.textureFrame = 0;
            prim.color = layer.color;
            prim.emissive = emissive;
            prim.classicLight = 1.0f;

            RgResult res = rg.rgUploadMeshPrimitive( &mesh, &prim );
            if ( res == RG_RESULT_SUCCESS || res == RG_RESULT_SUCCESS_FOUND_MESH ) {
                s_hasRasterSky = true;
            }
        }
    }
}

void RTGL_BuildSkyParams( RgDrawFrameSkyParams *sky ) {
    memset( sky, 0, sizeof( *sky ) );
    sky->sType = RG_STRUCTURE_TYPE_DRAW_FRAME_SKY_PARAMS;
    sky->pNext = NULL;

    if ( !s_hasRasterSky ) {
        // Pure indoor area with no sky surface in active areas: shut off sky
        // lighting completely (pure black {0,0,0}, 0.0x multiplier).
        sky->skyType = RG_SKY_TYPE_COLOR;
        sky->skyColorDefault = RTGL_V3( 0.0f, 0.0f, 0.0f );
        sky->skyColorMultiplier = 0.0f;
        sky->skyColorSaturation = 0.0f;
        sky->skyLightMultiplier = 0.0f;
        sky->skyLightSaturation = 0.0f;
        sky->skyColorScale = 0.0f;
        sky->skyLightScale = 0.0f;
    } else {
        sky->skyType = RG_SKY_TYPE_RASTERIZED_GEOMETRY;
        sky->skyColorDefault = RTGL_V3( rt_sky_colorR.GetFloat(),
                                         rt_sky_colorG.GetFloat(),
                                         rt_sky_colorB.GetFloat() );
        float colorMult = rt_sky_brightness.GetFloat() * 2.0f;
        sky->skyColorMultiplier = colorMult;
        sky->skyColorSaturation = 1.0f;
        sky->skyLightMultiplier = rt_sky_light.GetFloat();
        sky->skyLightSaturation = 1.0f;
        sky->skyColorScale = 1.0f;
        sky->skyLightScale = rt_sky_lightScale.GetFloat();
    }

    sky->skyViewerPosition = RTGL_V3( 0.0f, 0.0f, 0.0f );
    sky->pSkyCubemapTextureName = NULL;
}

void RTGL_BuildIlluminationParams( RgDrawFrameIlluminationParams *illum ) {
    memset( illum, 0, sizeof( *illum ) );
    illum->sType = RG_STRUCTURE_TYPE_DRAW_FRAME_ILLUMINATION_PARAMS;
    illum->pNext = NULL;
    illum->maxBounceShadows = (uint32_t)rt_bounce_maxShadows.GetInteger();
    if ( illum->maxBounceShadows > 4 ) { illum->maxBounceShadows = 4; }
    illum->enableSecondBounceForIndirect = rt_bounce_enableSecond.GetBool() ? 1 : 0;
    illum->cellWorldSize = 1.0f;
    illum->directDiffuseSensitivityToChange = rt_gi_sensitivity.GetFloat();
    illum->indirectDiffuseSensitivityToChange = 0.2f;
    illum->specularSensitivityToChange = 0.5f;
    illum->polygonalLightSpotlightFactor = 2.0f;
    illum->lightUniqueIdIgnoreFirstPersonViewerShadows = NULL;
}

#endif // D3_RTGL
