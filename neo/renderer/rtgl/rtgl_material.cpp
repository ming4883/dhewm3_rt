#include "sys/platform.h"
#include "framework/Common.h"
#include "idlib/Str.h"
#include "idlib/containers/List.h"
#include "renderer/tr_local.h"
#include "renderer/Image.h"
#include "renderer/Material.h"

#ifdef D3_RTGL

#include "renderer/rtgl/rtgl_internal.h"

// ===========================================================================
// P3 - Materials and textures
//
//  * rgProvideOriginalTexture() feeds an idImage's RGBA pixels to RTGL1 under
//    a stable name (the image's own imgName). RgMeshPrimitiveInfo::pTextureName
//    references that same name, so the classic albedo shows up in the path
//    tracer. Each name is provided once per session (cached in the registry).
//  * A small idMaterial -> RTGL1 classifier maps coverage / alpha-test /
//    additive blend / no-shadow onto RgMeshPrimitiveFlags + emissive.
//
//  DECAL TRANSPARENCY (2026-11-12 fix):
//  Doom 3 decal textures (scorch / grime / splat) are 24-bit TGA with NO alpha
//  channel (Image_files.cpp LoadTGA writes alpha=255 for pixel_size 24). Their
//  "transparent" area is encoded in the RGB through MULTIPLY blends, not alpha:
//    * blend filter / "gl_dst_color,gl_zero"  -> result = src*dst, so WHITE is
//      the identity (transparent) and dark texels are the mark. e.g. scorch.tga
//      is a black scorch on a pure white background.
//    * "gl_zero,gl_one_minus_src_color"       -> result = (1-src)*dst, so BLACK
//      is the identity (transparent) and bright texels are the mark.
//  RTGL1's RtAlphaTest.rahit discards a texel based on color.a, which for these
//  textures is always 1.0 -> the "transparent" area was drawn as an opaque
//  white quad. Marking the primitive ALPHA_TESTED is NOT enough; we BAKE a
//  coverage alpha from the RGB before rgProvideOriginalTexture:
//    * white-transparent (mode 1): alpha = 255 - maxRGB
//    * black-transparent (mode 2): alpha = maxRGB  AND invert RGB to 255-rgb
//      so the surviving mark is dark (both multiply blends darken the dest).
//  The baked image is provided under a DERIVED name ("<orig>@rtgl_dN") so it
//  never clobbers the un-baked texture used by other (opaque) materials.
//
// Scope note: diffuse albedo + tangent-space NORMAL MAP (Plan B, 2026-11-14)
// are now wired (in-memory, via RgOriginalTextureNormalMapEXT pNext).
// Multi-stage specular layering, PBR overrides, per-stage scroll/deform UVs
// and fog are still deliberately left for P3 iteration / P7.
// ===========================================================================

static idList<idStr>  s_provided;       // names already handed to rgProvideOriginalTexture
static idList<idStr*> s_derivedNames;   // stable storage for baked-texture keys
static bool           s_regInit = false;

void RTGL_MaterialBeginFrame( void ) {
    // Nothing per-frame yet: original textures are persistent until shutdown.
    // Kept as a hook for animated/dynamic image re-upload in a later pass.
}

void RTGL_MaterialShutdown( void ) {
    if ( rg.rgMarkOriginalTextureAsDeleted ) {
        for ( int i = 0; i < s_provided.Num(); i++ ) {
            rg.rgMarkOriginalTextureAsDeleted( s_provided[i].c_str() );
        }
    }
    for ( int i = 0; i < s_derivedNames.Num(); i++ ) {
        delete s_derivedNames[i];
    }
    s_derivedNames.Clear();
    s_provided.Clear();
    s_regInit = false;
}

static bool RTGL_WasProvided( const char *name ) {
    for ( int i = 0; i < s_provided.Num(); i++ ) {
        if ( s_provided[i] == name ) {
            return true;
        }
    }
    return false;
}

// Return a stable pointer to "<base>@rtgl_d<mode>". The strings live in their
// own heap idStr objects (s_derivedNames stores POINTERS, so list growth never
// moves them) and stay valid until RTGL_MaterialShutdown().
static const char * RTGL_DerivedName( const char *base, int mode ) {
    char buf[ 1024 ];
    idStr::snPrintf( buf, sizeof( buf ), "%s@rtgl_d%d", base, mode );
    for ( int i = 0; i < s_derivedNames.Num(); i++ ) {
        if ( *s_derivedNames[i] == buf ) {
            return s_derivedNames[i]->c_str();
        }
    }
    idStr *s = new idStr( buf );
    s_derivedNames.Append( s );
    return s->c_str();
}

// Return the image-program name of the material's bump/normal-map stage, or
// NULL when normal maps are disabled or there is no 2D bump stage.
// A Doom3 "bumpmap" (and heightmap(...)/addnormals(...) image programs) is
// already resolved to a tangent-space normal map by R_LoadImageProgram
// (R_HeightmapToNormalMap): RGBA8 with RGB = XYZ normal, A = height -- the
// exact layout RTGL1's "_n" slot samples in HitInfo.inl.
static const char * RTGL_NormalMapName( const idMaterial *mat ) {
    if ( !rt_normalmap_enable.GetBool() ) { return NULL; }
    const shaderStage_t *bump = mat ? mat->GetBumpStage() : NULL;
    if ( !bump || !bump->texture.image ) { return NULL; }
    const idImage *img = bump->texture.image;
    if ( img->type != TT_2D || img->imgName.Length() <= 0 ) { return NULL; }
    return img->imgName.c_str();
}

// Classify a material by its BLEND drawStateBits. Returns the "bake mode":
//   0 = not a multiply-blend decal (no alpha bake)
//   1 = multiply / filter          (WHITE = transparent)
//   2 = inverted multiply          (BLACK = transparent)
// Detection is by blend bits, NOT by sort==SS_DECAL, because many decals use
// "sort 1" (a numeric sort) and would be missed by SS_DECAL alone.
static int RTGL_DecalBlendType( const idMaterial *mat ) {
    if ( !mat ) { return 0; }
    for ( int i = 0; i < mat->GetNumStages(); i++ ) {
        const shaderStage_t *stage = mat->GetStage( i );
        if ( !stage ) { continue; }
        int src = stage->drawStateBits & GLS_SRCBLEND_BITS;
        int dst = stage->drawStateBits & GLS_DSTBLEND_BITS;
        if ( src == GLS_SRCBLEND_DST_COLOR && dst == GLS_DSTBLEND_ZERO ) {
            return 1;   // filter / modulate / gl_dst_color,gl_zero
        }
        if ( src == GLS_SRCBLEND_ZERO && dst == GLS_DSTBLEND_ONE_MINUS_SRC_COLOR ) {
            return 2;   // gl_zero,gl_one_minus_src_color
        }
    }
    return 0;
}

bool RTGL_IsDecal( const idMaterial *mat ) {
    return RTGL_DecalBlendType( mat ) != 0;
}

bool RTGL_IsSkySurface( const idMaterial *mat ) {
    if ( !mat ) { return false; }
    if ( mat->IsPortalSky() || mat->GetSort() == SS_PORTAL_SKY ) { return true; }
    texgen_t tg = mat->Texgen();
    return ( tg == TG_SKYBOX_CUBE || tg == TG_WOBBLESKY_CUBE );
}

// Load and upload one face (side 0..5 = +X, -X, +Y, -Y, +Z, -Z) of a Doom 3
// cubemap (`cameraCubeMap` CF_CAMERA or `cubeMap` CF_NATIVE) as a 2D clamped
// RTGL1 texture for Path A rasterized skybox geometry.
const char * RTGL_ProvideSkyFaceTexture( const char *cubeBaseName, cubeFiles_t cubeType, int side ) {
    if ( !cubeBaseName || !cubeBaseName[0] || side < 0 || side >= 6 ) { return NULL; }
    if ( !rg.rgProvideOriginalTexture ) { return NULL; }

    static const char *cameraSides[6] = {
        "_forward.tga", "_back.tga", "_left.tga", "_right.tga", "_up.tga", "_down.tga"
    };
    static const char *axisSides[6] = {
        "_px.tga", "_nx.tga", "_py.tga", "_ny.tga", "_pz.tga", "_nz.tga"
    };
    const char *suffix = ( cubeType == CF_CAMERA ) ? cameraSides[side] : axisSides[side];

    char loadName[ 1024 ];
    idStr::snPrintf( loadName, sizeof( loadName ), "%s%s", cubeBaseName, suffix );

    const char *provideName = RTGL_DerivedName( loadName, 10 + side );
    if ( RTGL_WasProvided( provideName ) ) {
        return provideName;
    }

    byte *pic = NULL;
    int width = 0, height = 0;
    ID_TIME_T timestamp = 0;
    textureDepth_t depth = TD_DEFAULT;
    R_LoadImageProgram( loadName, &pic, &width, &height, &timestamp, &depth );
    if ( !pic || width <= 0 || height <= 0 || timestamp == FILE_NOT_FOUND_TIMESTAMP ) {
        if ( pic ) { R_StaticFree( pic ); }
        return NULL;
    }

    // Apply the exact same camera -> native cubemap orientation transforms as
    // R_LoadCubeImages (Image_files.cpp:978-1002) so each face matches GL cube
    // coordinates (+X, -X, +Y, -Y, +Z, -Z).
    if ( cubeType == CF_CAMERA && width == height ) {
        switch ( side ) {
            case 0: // forward (+X)
                R_RotatePic( pic, width );
                break;
            case 1: // back (-X)
                R_RotatePic( pic, width );
                R_HorizontalFlip( pic, width, height );
                R_VerticalFlip( pic, width, height );
                break;
            case 2: // left (+Y)
                R_VerticalFlip( pic, width, height );
                break;
            case 3: // right (-Y)
                R_HorizontalFlip( pic, width, height );
                break;
            case 4: // up (+Z)
                R_RotatePic( pic, width );
                break;
            case 5: // down (-Z)
                R_RotatePic( pic, width );
                break;
        }
    }

    RgOriginalTextureInfo info = {};
    info.sType = RG_STRUCTURE_TYPE_ORIGINAL_TEXTURE_INFO;
    info.pNext = NULL;
    info.pTextureName = provideName;
    info.pPixels = pic;
    info.size = RTGL_Ext( (uint32_t)width, (uint32_t)height );
    info.filter = RG_SAMPLER_FILTER_LINEAR;
    info.addressModeU = RG_SAMPLER_ADDRESS_MODE_CLAMP;
    info.addressModeV = RG_SAMPLER_ADDRESS_MODE_CLAMP;

    RgResult r = rg.rgProvideOriginalTexture( &info );
    R_StaticFree( pic );

    if ( r == RG_RESULT_SUCCESS || r == RG_RESULT_SUCCESS_FOUND_TEXTURE ) {
        s_provided.Append( idStr( provideName ) );
        g_rtglStats.texturesProvided++;
        return provideName;
    }
    return NULL;
}

// Eyeball/iris materials (DFRM_EYEBALL: translucent, blend filter, coplanar
// with the eyeball mesh) are handled by a dedicated WORLD-raster path, gated
// by rt_eye_worldRaster. Kept in one predicate so the classifier, the texture
// bake and the geometry push all agree on the exact same set of surfaces.
static bool RTGL_IsEyeRasterMaterial( const idMaterial *mat ) {
    return mat && rt_eye_worldRaster.GetBool() &&
           mat->Deform() == DFRM_EYEBALL &&
           RTGL_DecalBlendType( mat ) == 1;
}

// Gate the bake on rt_decal_mode: modes 1 (alpha-tested cutout) and 3 (DECAL
// raster overlay) bake; mode 0 (legacy) and 2 (skip; handled in SubmitSurf)
// do not bake.
static int RTGL_DecalBakeMode( const idMaterial *mat ) {
    // Iris path ALWAYS needs the "white = transparent" baked alpha so the
    // WORLD raster alpha-test can discard the white sclera, independent of
    // the user's global rt_decal_mode.
    if ( RTGL_IsEyeRasterMaterial( mat ) ) { return 1; }
    int dm = rt_decal_mode.GetInteger();
    if ( dm != 1 && dm != 3 ) { return 0; }
    return RTGL_DecalBlendType( mat );
}

// Load `loadName`'s RGBA, optionally bake a coverage alpha from RGB, and hand
// it to RTGL1 under `provideName`. Names are cached (s_provided) so each key is
// uploaded once per session.
static void RTGL_ProvideTexture( const char *loadName, const char *provideName, int bakeMode, const char *normalLoadName, bool clamp = false ) {
    if ( !loadName || !loadName[0] || !provideName || !provideName[0] ) { return; }
    if ( RTGL_WasProvided( provideName ) ) { return; }
    if ( !rg.rgProvideOriginalTexture ) { return; }

    byte *pic = NULL;
    int width = 0, height = 0;
    ID_TIME_T timestamp = 0;
    textureDepth_t depth = TD_DEFAULT;

    // Synthesize Doom 3's builtin "_quadratic" falloff image (see R_QuadraticImage
    // in Image_init.cpp: 32x4 quadratic falloff along X, used by deform flare).
    if ( idStr::Icmp( loadName, "_quadratic" ) == 0 ) {
        width = 32;
        height = 4;
        pic = (byte *)R_StaticAlloc( width * height * 4 );
        for ( int x = 0; x < width; x++ ) {
            float d = idMath::Fabs( (float)x - ( width * 0.5f - 0.5f ) );
            d = ( d - 0.5f ) / ( width * 0.5f );
            d = 1.0f - d;
            d = d * d;
            int b = (int)( d * 255.0f );
            if ( b <= 0 ) { b = 0; } else if ( b > 255 ) { b = 255; }
            for ( int y = 0; y < height; y++ ) {
                byte *px = pic + ( y * width + x ) * 4;
                px[0] = px[1] = px[2] = (byte)b;
                px[3] = 255;
            }
        }
        clamp = true;
    } else if ( loadName[0] == '_' ) {
        return; // other procedural internal images (_white, _flat, etc.)
    } else {
        // This is the exact call idImage::ActuallyLoadImage() uses for imgName,
        // so it handles both plain files and image-program names.
        R_LoadImageProgram( loadName, &pic, &width, &height, &timestamp, &depth );
    }

    if ( !pic || width <= 0 || height <= 0 ) {
        if ( pic ) { R_StaticFree( pic ); }
        s_provided.Append( idStr( provideName ) );
        return;
    }

    if ( bakeMode != 0 ) {
        const int n = width * height;
        for ( int i = 0; i < n; i++ ) {
            byte *p = pic + i * 4;
            int mx = p[0];
            if ( p[1] > mx ) { mx = p[1]; }
            if ( p[2] > mx ) { mx = p[2]; }
            if ( bakeMode == 1 ) {
                // white = transparent: coverage = darkness
                p[3] = (byte)( 255 - mx );
            } else {
                // black = transparent: coverage = brightness; also darken the
                // surviving mark (both multiply blends darken the destination)
                p[3] = (byte)mx;
                p[0] = (byte)( 255 - p[0] );
                p[1] = (byte)( 255 - p[1] );
                p[2] = (byte)( 255 - p[2] );
            }
        }
    }

    // Optionally load the tangent-space normal map and chain it as the
    // "_n" companion of this material (Plan B, 2026-11-14). The pNext EXT
    // makes RTGL1 bind these pixels to the normal slot without any disk
    // "<material>_n" asset. Pixels only need to outlive this call.
    byte *nrmPic = NULL;
    int nrmW = 0, nrmH = 0;
    ID_TIME_T nrmTimestamp = 0;
    textureDepth_t nrmDepth = TD_DEFAULT;
    if ( normalLoadName && normalLoadName[0] ) {
        R_LoadImageProgram( normalLoadName, &nrmPic, &nrmW, &nrmH, &nrmTimestamp, &nrmDepth );
        if ( !nrmPic || nrmW <= 0 || nrmH <= 0 ) {
            if ( nrmPic ) { R_StaticFree( nrmPic ); }
            nrmPic = NULL;
        }
    }

    RgOriginalTextureNormalMapEXT nrm = {};
    if ( nrmPic ) {
        nrm.sType = RG_STRUCTURE_TYPE_ORIGINAL_TEXTURE_NORMAL_MAP_EXT;
        nrm.pNext = NULL;
        nrm.pPixels = nrmPic;
        nrm.size = RTGL_Ext( (uint32_t)nrmW, (uint32_t)nrmH );
    }

    RgOriginalTextureInfo info = {};
    info.sType = RG_STRUCTURE_TYPE_ORIGINAL_TEXTURE_INFO;
    info.pNext = nrmPic ? &nrm : NULL;
    info.pTextureName = provideName;
    info.pPixels = pic;
    info.size = RTGL_Ext( (uint32_t)width, (uint32_t)height );
    info.filter = RG_SAMPLER_FILTER_LINEAR;
    info.addressModeU = clamp ? RG_SAMPLER_ADDRESS_MODE_CLAMP : RG_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeV = clamp ? RG_SAMPLER_ADDRESS_MODE_CLAMP : RG_SAMPLER_ADDRESS_MODE_REPEAT;

    RgResult r = rg.rgProvideOriginalTexture( &info );
    R_StaticFree( pic );
    if ( nrmPic ) { R_StaticFree( nrmPic ); }

    if ( r == RG_RESULT_SUCCESS || r == RG_RESULT_SUCCESS_FOUND_TEXTURE ) {
        s_provided.Append( idStr( provideName ) );
        g_rtglStats.texturesProvided++;
    } else {
        common->Warning( "[RTGL] rgProvideOriginalTexture(%s) failed (%s)",
                            provideName, RTGL_ResultString( r ) );
    }
}

// Pick the primary albedo image for a material and ensure it is provided.
// Returns a stable pointer or NULL (= albedo white).
const char * RTGL_PrimaryTexture( const idMaterial *mat, const float *shaderRegisters ) {
    if ( !mat || mat->GetNumStages() <= 0 ) { return NULL; }

    const idImage *chosen = NULL;
    // Prefer a diffuse stage, then bump, then any stage carrying an image.
    for ( int pass = 0; pass < 3 && !chosen; pass++ ) {
        for ( int i = 0; i < mat->GetNumStages(); i++ ) {
            const shaderStage_t *stage = mat->GetStage( i );
            if ( !stage || !stage->texture.image ) { continue; }
            if ( shaderRegisters && stage->conditionRegister >= 0 &&
                 shaderRegisters[ stage->conditionRegister ] == 0.0f ) {
                continue;
            }
            bool ok = false;
            if ( pass == 0 )      { ok = ( stage->lighting == SL_DIFFUSE ); }
            else if ( pass == 1 ) { ok = ( stage->lighting == SL_BUMP ); }
            else                  { ok = true; }
            if ( ok && stage->texture.image->type == TT_2D ) {
                chosen = stage->texture.image;
                break;
            }
        }
    }

    if ( !chosen || chosen->imgName.Length() <= 0 ) { return NULL; }
    const char *name = chosen->imgName.c_str();
    if ( name[0] == '_' && idStr::Icmp( name, "_quadratic" ) != 0 ) { return NULL; }

    const char *nrmName = RTGL_NormalMapName( mat );
    const bool clamp = RTGL_IsEyeRasterMaterial( mat ) || ( chosen->repeat != TR_REPEAT );
    int bake = RTGL_DecalBakeMode( mat );
    if ( bake != 0 ) {
        const char *derived = RTGL_DerivedName( name, bake );
        RTGL_ProvideTexture( name, derived, bake, nrmName, clamp );
        return derived;
    }
    RTGL_ProvideTexture( name, name, 0, nrmName, clamp );
    return name;
}

// Provide the 2D texture for a single GUI / HUD shader stage.
const char * RTGL_GuiStageTexture( const shaderStage_t *stage, int bakeMode ) {
    if ( !stage || !stage->texture.image ) { return NULL; }
    const idImage *img = stage->texture.image;
    if ( img->type != TT_2D || img->imgName.Length() <= 0 ) { return NULL; }
    const char *name = img->imgName.c_str();
    if ( name[0] == '_' ) { return NULL; } // _white / _default -> 1x1 white
    const bool clamp = ( img->repeat != TR_REPEAT );
    if ( bakeMode != 0 ) {
        const char *derived = RTGL_DerivedName( name, bakeMode );
        RTGL_ProvideTexture( name, derived, bakeMode, NULL, clamp );
        return derived;
    }
    RTGL_ProvideTexture( name, name, 0, NULL, clamp );
    return name;
}

const char * RTGL_GuiTexture( const idMaterial *mat ) {
    if ( !mat ) { return NULL; }
    for ( int i = 0; i < mat->GetNumStages(); i++ ) {
        const char *t = RTGL_GuiStageTexture( mat->GetStage( i ), 0 );
        if ( t ) { return t; }
    }
    return NULL;
}

void RTGL_ClassifyMaterial( const idMaterial *mat,
                            RgMeshPrimitiveFlags *outFlags, float *outEmissive ) {
    RgMeshPrimitiveFlags flags = 0;
    float emissive = 0.0f;

    // (2026-11-14 fix) Iris / eyeball surfaces: route through the WORLD
    // raster channel instead of the ray-traced BLAS. As a coplanar
    // translucent decal they lose the primary hit to the eyeball/head
    // geometry and show all-white eyes. WORLD raster gives them:
    //   TRANSLUCENT  -> IsRasterized() == true (WORLD, not BLAS)
    //   ALPHA_TESTED -> RsWorld.inl discards the baked-white sclera (a=0)
    //   NO_SHADOW    -> no shadow casting
    // The matching baked-alpha texture is forced in RTGL_DecalBakeMode
    // above, and the coplanar depth push is applied in RTGL_SubmitSurf.
    if ( RTGL_IsEyeRasterMaterial( mat ) ) {
        *outFlags = RG_MESH_PRIMITIVE_TRANSLUCENT |
                    RG_MESH_PRIMITIVE_ALPHA_TESTED |
                    RG_MESH_PRIMITIVE_NO_SHADOW;
        *outEmissive = 0.0f;
        return;
    }

    // Path A skybox portal brushes (textures/skies/* with TG_SKYBOX_CUBE /
    // TG_WOBBLESKY_CUBE or portalSky): place them into the ray-traced BLAS as
    // SKY_VISIBILITY (PV_WORLD_2 -> INSTANCE_CUSTOM_INDEX_FLAG_SKY) and
    // NO_SHADOW, WITHOUT RG_MESH_PRIMITIVE_TRANSLUCENT. When a primary or
    // reflection ray hits this brush, doesPayloadContainHitInfo() returns
    // false -> storeSky() samples the rasterized 6-face skybox cube while
    // still occluding any geometry behind the sky brush.
    if ( RTGL_IsSkySurface( mat ) ) {
        *outFlags = RG_MESH_PRIMITIVE_SKY_VISIBILITY |
                    RG_MESH_PRIMITIVE_NO_SHADOW;
        *outEmissive = 0.0f;
        return;
    }

    if ( mat ) {
        materialCoverage_t cov = mat->Coverage();

        if ( cov == MC_TRANSLUCENT ) {
            flags |= RG_MESH_PRIMITIVE_TRANSLUCENT;
            flags |= RG_MESH_PRIMITIVE_NO_SHADOW;
        } else if ( cov == MC_PERFORATED ) {
            flags |= RG_MESH_PRIMITIVE_ALPHA_TESTED;
        }

        for ( int i = 0; i < mat->GetNumStages(); i++ ) {
            const shaderStage_t *stage = mat->GetStage( i );
            if ( !stage ) { continue; }
            if ( stage->hasAlphaTest && !( flags & RG_MESH_PRIMITIVE_ALPHA_TESTED ) ) {
                flags |= RG_MESH_PRIMITIVE_ALPHA_TESTED;
            }
            int src = stage->drawStateBits & GLS_SRCBLEND_BITS;
            int dst = stage->drawStateBits & GLS_DSTBLEND_BITS;
            // Only "blend add" (dst=ONE) is genuine emissive glow / additive blend.
            // Pure ambient/translucent additive effects (flares, sprites, additive
            // smoke/beam particles, which have no diffuse/bump interaction stages)
            // always go through RTGL1's WORLD rasterizer. RTGL1's ToPipelineState()
            // only selects PipelineStateFlagBits::ADDITIVE (SRC_ALPHA, ONE) when
            // TRANSLUCENT is set AND emissive > FLT_EPSILON; with emissive == 0 it
            // falls back to ordinary alpha blend and draws black-background /
            // alpha=1 flare & smoke textures as opaque blocks. Give pure additive
            // effects a tiny positive emissive (1e-4f > FLT_EPSILON) so they use
            // the additive raster pipeline without triggering bloom blowout.
            if ( dst == GLS_DSTBLEND_ONE && src != GLS_SRCBLEND_ZERO ) {
                const bool isPureEffect =
                    ( cov == MC_TRANSLUCENT ) ||
                    ( mat->Deform() != DFRM_NONE && mat->Deform() != DFRM_EYEBALL ) ||
                    ( !mat->ReceivesLighting() );
                if ( isPureEffect ) {
                    flags |= RG_MESH_PRIMITIVE_TRANSLUCENT;
                    flags |= RG_MESH_PRIMITIVE_NO_SHADOW;
                    if ( rt_emissive_enable.GetBool() ) {
                        emissive = 1.0f * rt_emissive_mult.GetFloat();
                    } else if ( emissive <= 0.0f ) {
                        emissive = 1e-4f;
                    }
                } else {
                    if ( rt_emissive_enable.GetBool() ) {
                        emissive = 1.0f * rt_emissive_mult.GetFloat();
                    } else {
                        emissive = 0.0f;
                    }
                    flags |= RG_MESH_PRIMITIVE_NO_SHADOW;
                }
            }
        }

        if ( !mat->SurfaceCastsShadow() ) {
            flags |= RG_MESH_PRIMITIVE_NO_SHADOW;
        }
    }

    // Decal overlays: the baked coverage alpha (see RTGL_ProvideTexture) lets
    // RTGL1's any-hit shader discard the transparent texels. RTGL_IsDecal is
    // blend-based, so it also catches "sort 1" decals (scorch / schmutz).
    if ( RTGL_IsDecal( mat ) ) {
        int dm = rt_decal_mode.GetInteger();
        if ( dm != 0 ) {
            emissive = 0.0f;                        // darkening overlays never glow
            flags |= RG_MESH_PRIMITIVE_NO_SHADOW;
            if ( dm == 1 ) {
                flags &= ~RG_MESH_PRIMITIVE_TRANSLUCENT;  // cutout, not refraction
                flags |= RG_MESH_PRIMITIVE_ALPHA_TESTED;
            } else if ( dm == 3 ) {
                flags |= RG_MESH_PRIMITIVE_DECAL;
                flags |= RG_MESH_PRIMITIVE_TRANSLUCENT;
            }
        }
    }

    *outFlags = flags;
    *outEmissive = emissive;
}

#endif // D3_RTGL