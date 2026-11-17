#include "sys/platform.h"
#include "framework/Common.h"
#include "framework/FileSystem.h"
#include "idlib/Str.h"
#include "idlib/containers/List.h"
#include "idlib/containers/HashTable.h"
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

// Return the 2D additive glow mask (`blend add`, e.g. `*_add.tga`) for an
// opaque/alpha-tested lit material when `rt_emissive_enable` is on and the
// stage evaluates to non-zero RGB in `shaderRegisters`. Binding this mask to
// RTGL1's `TEXTURE_EMISSIVE_INDEX` ("_e") ensures only the small indicator LEDs
// or light strips glow instead of the entire diffuse surface (`h.albedo`).
static const char * RTGL_EmissiveMapName( const idMaterial *mat, const float *shaderRegisters ) {
    if ( !rt_emissive_enable.GetBool() || !mat ) { return NULL; }
    materialCoverage_t cov = mat->Coverage();
    const bool isPureEffect =
        ( cov == MC_TRANSLUCENT ) ||
        ( mat->Deform() != DFRM_NONE && mat->Deform() != DFRM_EYEBALL ) ||
        ( !mat->ReceivesLighting() );
    if ( isPureEffect ) { return NULL; }

    const int numRegs = mat->GetNumRegisters();
    for ( int i = 0; i < mat->GetNumStages(); i++ ) {
        const shaderStage_t *stage = mat->GetStage( i );
        if ( !stage || stage->lighting != SL_AMBIENT ) { continue; }
        if ( stage->hasAlphaTest ) { continue; }
        int src = stage->drawStateBits & GLS_SRCBLEND_BITS;
        int dst = stage->drawStateBits & GLS_DSTBLEND_BITS;
        if ( dst != GLS_DSTBLEND_ONE || src == GLS_SRCBLEND_ZERO ) { continue; }
        if ( shaderRegisters && stage->conditionRegister >= 0 &&
             stage->conditionRegister < numRegs &&
             shaderRegisters[ stage->conditionRegister ] == 0.0f ) {
            continue;
        }
        if ( shaderRegisters ) {
            int r0 = stage->color.registers[0];
            int r1 = stage->color.registers[1];
            int r2 = stage->color.registers[2];
            float cr = ( r0 >= 0 && r0 < numRegs ) ? shaderRegisters[r0] : 1.0f;
            float cg = ( r1 >= 0 && r1 < numRegs ) ? shaderRegisters[r1] : 1.0f;
            float cb = ( r2 >= 0 && r2 < numRegs ) ? shaderRegisters[r2] : 1.0f;
            if ( cr <= 0.0f && cg <= 0.0f && cb <= 0.0f ) {
                continue;
            }
        }
        const idImage *img = stage->texture.image;
        if ( img && img->type == TT_2D && img->imgName.Length() > 0 && img->imgName[0] != '_' ) {
            return img->imgName.c_str();
        }
    }
    return NULL;
}

// Classify a material by its BLEND drawStateBits / translucent-diffuse decal
// pattern. Returns the decal blend type:
//   0 = not a decal
//   1 = multiply / filter          (WHITE = transparent, darkens background)
//   2 = inverted multiply          (BLACK = transparent, darkens background)
//   4 = translucent diffusemap     (BLACK = transparent, ADDITIVE light-interaction
//                                   in original Doom 3: sentry_spot, floor_stripe,
//                                   alphabet4, military_logo, mixom1, marsec, etc.)
// Detection is by blend/stage properties, NOT by sort==SS_DECAL alone, because
// many decals use "sort 1" or default translucent sort (SS_MEDIUM).
static int RTGL_DecalBlendType( const idMaterial *mat ) {
    if ( !mat ) { return 0; }
    bool hasDiffuse = false;
    bool hasBump = false;
    bool hasAlphaTest = false;
    bool hasCubemap = false;
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
        if ( stage->lighting == SL_DIFFUSE ) { hasDiffuse = true; }
        // Ignore the implicit `map _flat` bump stage automatically synthesized
        // by idMaterial::AddImplicitStages() (Material.cpp:1721) for every
        // diffuse-only material; only treat explicit non-procedural normal maps
        // as `hasBump`.
        if ( stage->lighting == SL_BUMP && stage->texture.image &&
             stage->texture.image->imgName.Length() > 0 &&
             stage->texture.image->imgName[0] != '_' ) {
            hasBump = true;
        }
        if ( stage->hasAlphaTest )           { hasAlphaTest = true; }
        if ( stage->texture.image && stage->texture.image->type == TT_CUBIC ) {
            hasCubemap = true;
        }
    }
    // In Doom 3's ARB2 backend, a `translucent` material with `blend diffusemap`
    // skips RB_T_FillDepthBuffer (never writes black) and is drawn in
    // vLight->translucentInteractions with additive `GL_ONE, GL_ONE` blending,
    // so black (0,0,0) in its 24-bit TGA is 100% transparent.
    if ( mat->Coverage() == MC_TRANSLUCENT && hasDiffuse &&
         !hasBump && !hasAlphaTest && !hasCubemap &&
         mat->GetSurfaceType() != SURFTYPE_GLASS ) {
        return 4;
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

// Gate the bake on rt_decal_mode:
//   bakeMode 1 = white-transparent, preserve RGB (eyeball iris alpha-cutout)
//   bakeMode 2 = black-transparent, invert RGB + alpha = maxRGB (splat decals)
//   bakeMode 3 = white-transparent, unmix white background from RGB (eyeball iris)
//   bakeMode 4 = black-transparent, preserve RGB + alpha = maxRGB (translucent
//                diffusemap decals: sentry_spot, floor_stripe, alphabet4, etc.)
//   bakeMode 5 = white-transparent multiply decal (`blend filter` / `gl_dst_color,gl_zero`
//                such as `skid`, `scorch`, `stain01`, `hair`): attenuates unmixed
//                RGB by `(p[c] - mn) / 8` so unlit WORLD-raster alpha blending
//                darkens dark floors instead of painting glowing orange/yellow marks!
static int RTGL_DecalBakeMode( const idMaterial *mat ) {
    if ( RTGL_IsEyeRasterMaterial( mat ) ) { return 3; }
    int dm = rt_decal_mode.GetInteger();
    if ( dm != 1 && dm != 3 ) { return 0; }
    int bt = RTGL_DecalBlendType( mat );
    if ( bt == 1 ) { return 5; }
    return bt;
}

// Load `loadName`'s RGBA, optionally bake a coverage alpha from RGB, and hand
// it to RTGL1 under `provideName`. Names are cached (s_provided) so each key is
// uploaded once per session.
static void RTGL_ProvideTexture( const char *loadName, const char *provideName, int bakeMode, const char *normalLoadName, bool clamp = false, const char *emissiveLoadName = NULL ) {
    if ( !loadName || !loadName[0] || !provideName || !provideName[0] ) { return; }
    if ( RTGL_WasProvided( provideName ) ) { return; }
    if ( !rg.rgProvideOriginalTexture ) { return; }

    byte *pic = NULL;
    int width = 0, height = 0;
    ID_TIME_T timestamp = 0;
    textureDepth_t depth = TD_DEFAULT;

    // Synthesize Doom 3's builtin "_quadratic" falloff image (used by `deform flare`).
    // R_FlareDeform assigns outer ring UVs along all 4 edges: (0,0.5), (1,0.5),
    // (0.5,0), and (0.5,1). Original GL relied on GL_CLAMP_TO_BORDER (black=0)
    // to fade the top/bottom edges of its 32x4 texture, whereas RTGL1's CLAMP uses
    // VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE. Build a 32x32 2D quadratic falloff
    // with an explicit 0 border on all 4 sides so every outer flare vertex fades
    // smoothly to 0 under CLAMP_TO_EDGE.
    if ( idStr::Icmp( loadName, "_quadratic" ) == 0 ) {
        width = 32;
        height = 32;
        pic = (byte *)R_StaticAlloc( width * height * 4 );
        for ( int y = 0; y < height; y++ ) {
            float dy = idMath::Fabs( (float)y - ( height * 0.5f - 0.5f ) );
            dy = ( dy - 0.5f ) / ( height * 0.5f - 1.0f );
            if ( dy < 0.0f ) { dy = 0.0f; } else if ( dy > 1.0f ) { dy = 1.0f; }
            float fy = ( 1.0f - dy ) * ( 1.0f - dy );
            for ( int x = 0; x < width; x++ ) {
                float dx = idMath::Fabs( (float)x - ( width * 0.5f - 0.5f ) );
                dx = ( dx - 0.5f ) / ( width * 0.5f - 1.0f );
                if ( dx < 0.0f ) { dx = 0.0f; } else if ( dx > 1.0f ) { dx = 1.0f; }
                float fx = ( 1.0f - dx ) * ( 1.0f - dx );
                int b = (int)( fx * fy * 255.0f + 0.5f );
                if ( x == 0 || x == width - 1 || y == 0 || y == height - 1 || b < 0 ) {
                    b = 0;
                } else if ( b > 255 ) {
                    b = 255;
                }
                byte *px = pic + ( y * width + x ) * 4;
                px[0] = px[1] = px[2] = (byte)b;
                px[3] = (byte)b;
            }
        }
        clamp = true;
    } else if ( idStr::Icmp( loadName, "_noFalloff" ) == 0 ) {
        // Synthesize Doom 3's builtin "_noFalloff" ramp (64x16 white interior with
        // 0 at x=0 and x=63, used by `lights/defaultProjectedLight`).
        width = 64;
        height = 16;
        pic = (byte *)R_StaticAlloc( width * height * 4 );
        memset( pic, 0, width * height * 4 );
        for ( int y = 0; y < height; y++ ) {
            for ( int x = 1; x < width - 1; x++ ) {
                byte *px = pic + ( y * width + x ) * 4;
                px[0] = px[1] = px[2] = px[3] = 255;
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
            int mn = p[0];
            if ( p[1] < mn ) { mn = p[1]; }
            if ( p[2] < mn ) { mn = p[2]; }
            if ( bakeMode == 1 ) {
                // Eye iris cutout: white = transparent, preserve iris RGB
                p[3] = (byte)( 255 - mx );
            } else if ( bakeMode == 3 ) {
                // Eyeball iris (`DFRM_EYEBALL` + `blend filter` over a bright white
                // sclera where dst ~= 1.0): unmix the white sclera component `mn`
                // so `src_rgb * a + dst * (1 - a)` reproduces `dst * (tex/255)`
                // across pupil, mid-iris, limbal ring, and sclera.
                int cov = ( ( 255 - mn ) * (int)p[3] + 127 ) / 255;
                p[3] = (byte)cov;
                p[0] = (byte)( p[0] - mn );
                p[1] = (byte)( p[1] - mn );
                p[2] = (byte)( p[2] - mn );
            } else if ( bakeMode == 5 ) {
                // Multiply / `blend filter` decals (`skid`, `scorch`, `stain01`,
                // `stainwallred`, `hair`, `glasses2_fx`):
                // Original Doom 3 computes `dst * tex_rgb`, which is proportional
                // to the floor's lighting `dst`. In RTGL1's unlit WORLD rasterizer,
                // standard alpha blend `src_rgb * a + dst * (1 - a)` adds an
                // UNLIT `src_rgb * a` term that gets amplified by post-raster
                // auto-exposure! With `p[c] - mn`, a slightly warm grey tire skid
                // (`R=180, G=150, B=130` -> `unmixed = (50, 20, 0)`) turns into a
                // glowing 100%-saturated bright orange track on dark hangar floors.
                // Scaling the unmixed chroma down by 1/8 (`>> 3`) keeps subtle dark
                // tinting (e.g. dark dried blood/mud) while ensuring multiply decals
                // primarily darken (`dst * (1 - a)`) rather than emit unlit orange.
                int cov = ( ( 255 - mn ) * (int)p[3] + 127 ) / 255;
                p[3] = (byte)cov;
                p[0] = (byte)( ( p[0] - mn ) >> 3 );
                p[1] = (byte)( ( p[1] - mn ) >> 3 );
                p[2] = (byte)( ( p[2] - mn ) >> 3 );
            } else if ( bakeMode == 4 ) {
                // Translucent diffusemap decals (sentry_spot, floor_stripe,
                // alphabet4, alphabet6, military_logo, corp_logo1, uacosticker,
                // mixom1, arrow1, marsec): 24-bit RGB on a pure black background
                // where black is transparent, but the surviving painted mark IS
                // a lit `diffusemap` surface (receives path-traced lighting and
                // shadows in the ray-traced BLAS via RtAlphaTest.rahit).
                // Keep original diffuse RGB untouched so gray/orange/yellow paint
                // isn't blown out to unlit white, and bake a sharp cutout alpha
                // so RtAlphaTest discards the black background (mx < ~22).
                int cov = mx * 6;
                if ( cov > 255 ) { cov = 255; }
                cov = ( cov * (int)p[3] + 127 ) / 255;
                p[3] = (byte)cov;
            } else {
                // black = transparent (`gl_zero, gl_one_minus_src_color`, e.g.
                // `footprint01`, `dsplat2`, `duffysplat`):
                // Original Doom 3 computes `dst * (1 - src_rgb)`. Attenuate the
                // unmixed chroma by 1/6 so blood/bootprints look dark crimson/brown
                // on shadowed floors instead of glowing bright red.
                p[3] = (byte)mx;
                p[0] = (byte)( ( mx - p[0] ) / 6 );
                p[1] = (byte)( ( mx - p[1] ) / 6 );
                p[2] = (byte)( ( mx - p[2] ) / 6 );
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

    byte *emisPic = NULL;
    int emisW = 0, emisH = 0;
    ID_TIME_T emisTimestamp = 0;
    textureDepth_t emisDepth = TD_DEFAULT;
    if ( emissiveLoadName && emissiveLoadName[0] ) {
        R_LoadImageProgram( emissiveLoadName, &emisPic, &emisW, &emisH, &emisTimestamp, &emisDepth );
        if ( !emisPic || emisW <= 0 || emisH <= 0 ) {
            if ( emisPic ) { R_StaticFree( emisPic ); }
            emisPic = NULL;
        }
    }

    RgOriginalTextureEmissiveMapEXT emisExt = {};
    if ( emisPic ) {
        emisExt.sType = RG_STRUCTURE_TYPE_ORIGINAL_TEXTURE_EMISSIVE_MAP_EXT;
        emisExt.pNext = NULL;
        emisExt.pPixels = emisPic;
        emisExt.size = RTGL_Ext( (uint32_t)emisW, (uint32_t)emisH );
    }

    RgOriginalTextureNormalMapEXT nrm = {};
    if ( nrmPic ) {
        nrm.sType = RG_STRUCTURE_TYPE_ORIGINAL_TEXTURE_NORMAL_MAP_EXT;
        nrm.pNext = emisPic ? (void *)&emisExt : NULL;
        nrm.pPixels = nrmPic;
        nrm.size = RTGL_Ext( (uint32_t)nrmW, (uint32_t)nrmH );
    }

    RgOriginalTextureInfo info = {};
    info.sType = RG_STRUCTURE_TYPE_ORIGINAL_TEXTURE_INFO;
    info.pNext = nrmPic ? (void *)&nrm : ( emisPic ? (void *)&emisExt : NULL );
    info.pTextureName = provideName;
    info.pPixels = pic;
    info.size = RTGL_Ext( (uint32_t)width, (uint32_t)height );
    info.filter = RG_SAMPLER_FILTER_LINEAR;
    info.addressModeU = clamp ? RG_SAMPLER_ADDRESS_MODE_CLAMP : RG_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeV = clamp ? RG_SAMPLER_ADDRESS_MODE_CLAMP : RG_SAMPLER_ADDRESS_MODE_REPEAT;

    RgResult r = rg.rgProvideOriginalTexture( &info );
    R_StaticFree( pic );
    if ( nrmPic ) { R_StaticFree( nrmPic ); }
    if ( emisPic ) { R_StaticFree( emisPic ); }

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
    // Prefer SL_DIFFUSE, then SL_AMBIENT (excluding maskcolor stages), then
    // SL_SPECULAR. NEVER pick SL_BUMP as the primary albedo texture: tangent-
    // space normal maps (e.g. security/gog_local.tga, zsgogs2_local.tga,
    // hazmat/visor_local.tga) encode XYZ normals in RGB and paint goggles/visors
    // bright blue/purple/green if bound to the albedo slot.
    const int maskColorBits = ( GLS_REDMASK | GLS_GREENMASK | GLS_BLUEMASK );
    for ( int pass = 0; pass < 3 && !chosen; pass++ ) {
        for ( int i = 0; i < mat->GetNumStages(); i++ ) {
            const shaderStage_t *stage = mat->GetStage( i );
            if ( !stage || !stage->texture.image ) { continue; }
            if ( stage->lighting == SL_BUMP ) { continue; }
            if ( ( stage->drawStateBits & maskColorBits ) == maskColorBits ) { continue; }
            if ( shaderRegisters && stage->conditionRegister >= 0 &&
                 shaderRegisters[ stage->conditionRegister ] == 0.0f ) {
                continue;
            }
            bool ok = false;
            if ( pass == 0 )      { ok = ( stage->lighting == SL_DIFFUSE ); }
            else if ( pass == 1 ) { ok = ( stage->lighting == SL_AMBIENT ); }
            else                  { ok = ( stage->lighting == SL_SPECULAR ); }
            if ( ok && stage->texture.image->type == TT_2D ) {
                chosen = stage->texture.image;
                break;
            }
        }
    }

    if ( !chosen || chosen->imgName.Length() <= 0 ) { return NULL; }
    const char *name = chosen->imgName.c_str();
    if ( name[0] == '_' && idStr::Icmp( name, "_quadratic" ) != 0 ) { return NULL; }

    const char *nrmName  = RTGL_NormalMapName( mat );
    const char *emisName = RTGL_EmissiveMapName( mat, shaderRegisters );
    const bool clamp = RTGL_IsEyeRasterMaterial( mat ) || ( chosen->repeat != TR_REPEAT );
    int bake = RTGL_DecalBakeMode( mat );
    if ( emisName != NULL ) {
        const char *derived = RTGL_DerivedName( name, 20 + bake );
        RTGL_ProvideTexture( name, derived, bake, nrmName, clamp, emisName );
        return derived;
    }
    if ( bake != 0 ) {
        const char *derived = RTGL_DerivedName( name, bake );
        RTGL_ProvideTexture( name, derived, bake, nrmName, clamp, NULL );
        return derived;
    }
    RTGL_ProvideTexture( name, name, 0, nrmName, clamp, NULL );
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

// Provide a 2D light projection cookie or falloff texture.
const char * RTGL_LightTexture( const idImage *img ) {
    if ( !img || img->type != TT_2D || img->imgName.Length() <= 0 ) { return NULL; }
    const char *name = img->imgName.c_str();
    if ( name[0] == '_' && idStr::Icmp( name, "_noFalloff" ) != 0 && idStr::Icmp( name, "_quadratic" ) != 0 ) {
        return NULL;
    }
    const bool clamp = ( img->repeat != TR_REPEAT );
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
        // Do NOT set RG_MESH_PRIMITIVE_ALPHA_TESTED here: in Doom 3's eye
        // textures (bet.tga, green.tga, brown.tga, blue.tga), the mid-iris body
        // has RGB ~ 142..184 (alpha = 255 - RGB = 71..113 < 128), while the
        // pupil (RGB ~ 0, a = 254) and outer limbal ring (RGB ~ 98..112, a = 143)
        // have alpha >= 128. Enabling ALPHA_TESTED (threshold 0.5) discards the
        // entire mid-iris and exposes a bright white ring of sclera between the
        // pupil and outer limbal ring. Pure TRANSLUCENT with bakeMode 3 smoothly
        // modulates the lit eyeball underneath across the full iris.
        *outFlags = RG_MESH_PRIMITIVE_TRANSLUCENT |
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
            // Ignore non-2D cubemap reflection stages (e.g. `env/bland` on
            // security/gog, hazmat/visor) and alpha-tested cutouts (`security/gog`
            // stage 0 has `blend add` + `alphaTest 0.5` for the dark goggle frame)
            // when deciding whether to force the WORLD raster ADDITIVE pipeline:
            // RTGL1's ToPipelineState() gives ADDITIVE precedence over ALPHA_TESTED,
            // which would skip alphaTest discard in RsWorld.inl.
            const bool is2DStage = ( stage->texture.image != NULL &&
                                     stage->texture.image->type == TT_2D );
            if ( is2DStage && dst == GLS_DSTBLEND_ONE && src != GLS_SRCBLEND_ZERO &&
                 !stage->hasAlphaTest && !( flags & RG_MESH_PRIMITIVE_ALPHA_TESTED ) ) {
                const bool isPureEffect =
                    ( cov == MC_TRANSLUCENT ) ||
                    ( mat->Deform() != DFRM_NONE && mat->Deform() != DFRM_EYEBALL ) ||
                    ( !mat->ReceivesLighting() );
                if ( isPureEffect ) {
                    flags |= RG_MESH_PRIMITIVE_TRANSLUCENT;
                    flags |= RG_MESH_PRIMITIVE_NO_SHADOW;
                    // Pure additive effects (flares, sprites, smoke) only need a tiny
                    // epsilon emissive to select PipelineStateFlagBits::ADDITIVE in
                    // RTGL1's rasterizer. Giving them full surface emissive (rt_emissive_mult)
                    // writes huge values to outScreenEmission and blows them out into
                    // opaque white blocks via bloom / tonemapping.
                    emissive = 1e-4f;
                } else {
                    if ( rt_emissive_enable.GetBool() ) {
                        emissive = 1.0f * rt_emissive_mult.GetFloat();
                    } else {
                        emissive = 0.0f;
                    }
                    if ( idStr::Cmpn( mat->GetName(), "textures/base_light/", 20 ) == 0 ) {
                        flags |= RG_MESH_PRIMITIVE_NO_SHADOW;
                    }
                }
            }
        }

        if ( !mat->SurfaceCastsShadow() ) {
            flags |= RG_MESH_PRIMITIVE_NO_SHADOW;
        }
    }

    // Decal overlays (Solution 1): route multiply-blend decals (skid, stain,
    // scorch, blood splat, scientist/zombie hair cards) to the WORLD raster
    // channel (RG_MESH_PRIMITIVE_TRANSLUCENT | RG_MESH_PRIMITIVE_NO_SHADOW,
    // WITHOUT ALPHA_TESTED). This keeps them out of the ray-traced BLAS so:
    //   1) Primary rays hit the underlying floor/wall geometry and preserve its
    //      tangent-space normal map, path-traced lighting, and specular;
    //   2) The unmixed dark-alpha texture (bakeMode 3 / 2) smoothly darkens the
    //      lit floor and eliminates white/grey fringes on decals and hair.
    if ( RTGL_IsDecal( mat ) ) {
        int dm = rt_decal_mode.GetInteger();
        int bt = RTGL_DecalBlendType( mat );
        if ( dm != 0 ) {
            emissive = 0.0f;                        // decals never glow
            flags |= RG_MESH_PRIMITIVE_NO_SHADOW;
            if ( bt == 4 ) {
                // Lit diffusemap decals (uacosticker, corp_logo1, military_logo,
                // alphabet4/6, sentry_spot, floor_stripe): route to the ray-traced
                // BLAS as ALPHA_TESTED cutouts (NOT TRANSLUCENT). In WORLD raster
                // they would be drawn unlit (`outColor = tex * color`); in the BLAS
                // RtAlphaTest.rahit discards their black background while survivors
                // receive full path-traced direct/indirect lighting and shadows.
                flags &= ~RG_MESH_PRIMITIVE_TRANSLUCENT;
                flags |= RG_MESH_PRIMITIVE_ALPHA_TESTED;
            } else {
                // Multiply darkening decals (bt == 1 or 2: skid, stain, scorch,
                // blood splat, hair cards): route to WORLD raster alpha blend so
                // they darken the already-lit floor without replacing its normal map.
                flags &= ~RG_MESH_PRIMITIVE_ALPHA_TESTED;
                if ( dm == 1 ) {
                    flags |= RG_MESH_PRIMITIVE_TRANSLUCENT;
                } else if ( dm == 3 ) {
                    flags |= RG_MESH_PRIMITIVE_DECAL;
                    flags |= RG_MESH_PRIMITIVE_TRANSLUCENT;
                }
            }
        }
    }

    *outFlags = flags;
    *outEmissive = emissive;
}


// ===========================================================================
// Per-material PBR (roughness / metallic) table + fallback classifier
// (ported from DUDE's MaterialPbr.cpp & pbr/pbr_materials.cfg).
//
// Why this is critical for RTGL1 specular visibility:
//   1) When RgMeshPrimitivePBREXT is omitted, RTGL1's ASManager.cpp sets
//      roughnessDefault = 1.0 and metallicDefault = 0.0 on every surface.
//   2) In CmSVGFAtrous.comp:304, RTGL1 blends `filteredSpec` into `indirect`
//      via `smoothstep(0.5, 0.75, roughness)` (FAKE_ROUGH_SPECULAR_THRESHOLD = 0.5).
//      At roughness = 1.0, smoothstep = 1.0 -> 100% of direct/indirect specular
//      is discarded and replaced by diffuse indirect!
//   3) Even before that replacement, `metallic = 0.0` sets `ro_s = 0.04`, which
//      scales the demodulated `unfilteredSpecular` down by 25x in the final image
//      (whereas the Unfiltered Specular debug view shows the raw demodulated buffer).
// ===========================================================================
enum rtglPbrCategory_t {
    RTGL_PBR_CAT_NONE = 0,
    RTGL_PBR_CAT_SOLIDMETAL,
    RTGL_PBR_CAT_HOLLOWMETAL,
    RTGL_PBR_CAT_MONSTERMETAL,
    RTGL_PBR_CAT_RUST,
    RTGL_PBR_CAT_CONCRETE,
    RTGL_PBR_CAT_CERAMIC,
    RTGL_PBR_CAT_ROCK,
    RTGL_PBR_CAT_DIRT,
    RTGL_PBR_CAT_SAND,
    RTGL_PBR_CAT_GRAVEL,
    RTGL_PBR_CAT_SKIN,
    RTGL_PBR_CAT_EYES,
    RTGL_PBR_CAT_FLESH,
    RTGL_PBR_CAT_WOOD,
    RTGL_PBR_CAT_CLOTH,
    RTGL_PBR_CAT_CARDBOARD,
    RTGL_PBR_CAT_ARMOR,
    RTGL_PBR_CAT_ELECTRONICS,
    RTGL_PBR_CAT_PLASTIC,
    RTGL_PBR_CAT_RUBBER,
    RTGL_PBR_CAT_GLASS,
    RTGL_PBR_CAT_BPGLASS,
    RTGL_PBR_CAT_WATER,
    RTGL_PBR_CAT_OIL,
    RTGL_PBR_CAT_GELATINOUSFLUID,
    RTGL_PBR_CAT_COUNT
};

struct rtglPbrCatDefault_t {
    int         cat;
    const char *token;
    float       metallic;
    float       roughness;
};

static rtglPbrCatDefault_t s_pbrCatDefaults[ RTGL_PBR_CAT_COUNT ] = {
    { RTGL_PBR_CAT_NONE,            "none",            0.00f, 0.45f },
    { RTGL_PBR_CAT_SOLIDMETAL,      "solidmetal",      0.50f, 0.42f },
    { RTGL_PBR_CAT_HOLLOWMETAL,     "hollowmetal",     0.32f, 0.42f },
    { RTGL_PBR_CAT_MONSTERMETAL,    "monstermetal",    0.54f, 0.44f },
    { RTGL_PBR_CAT_RUST,            "rust",            0.54f, 0.46f },
    { RTGL_PBR_CAT_CONCRETE,        "concrete",        0.00f, 0.90f },
    { RTGL_PBR_CAT_CERAMIC,         "ceramic",         0.08f, 0.35f },
    { RTGL_PBR_CAT_ROCK,            "rock",            0.00f, 0.90f },
    { RTGL_PBR_CAT_DIRT,            "dirt",            0.00f, 0.90f },
    { RTGL_PBR_CAT_SAND,            "sand",            0.00f, 0.90f },
    { RTGL_PBR_CAT_GRAVEL,          "gravel",          0.00f, 0.90f },
    { RTGL_PBR_CAT_SKIN,            "skin",            0.00f, 0.42f },
    { RTGL_PBR_CAT_EYES,            "eyes",            0.00f, 0.15f },
    { RTGL_PBR_CAT_FLESH,           "flesh",           0.00f, 0.42f },
    { RTGL_PBR_CAT_WOOD,            "wood",            0.00f, 0.75f },
    { RTGL_PBR_CAT_CLOTH,           "cloth",           0.00f, 0.85f },
    { RTGL_PBR_CAT_CARDBOARD,       "cardboard",       0.00f, 0.85f },
    { RTGL_PBR_CAT_ARMOR,           "armor",           0.15f, 0.45f },
    { RTGL_PBR_CAT_ELECTRONICS,     "electronics",     0.25f, 0.40f },
    { RTGL_PBR_CAT_PLASTIC,         "plastic",         0.00f, 0.42f },
    { RTGL_PBR_CAT_RUBBER,          "rubber",          0.00f, 0.70f },
    { RTGL_PBR_CAT_GLASS,           "glass",           0.00f, 0.05f },
    { RTGL_PBR_CAT_BPGLASS,         "bpglass",         0.00f, 0.05f },
    { RTGL_PBR_CAT_WATER,           "water",           0.00f, 0.15f },
    { RTGL_PBR_CAT_OIL,             "oil",             0.00f, 0.10f },
    { RTGL_PBR_CAT_GELATINOUSFLUID, "gelatinousfluid", 0.00f, 0.20f },
};

struct rtglPbrEntry_t {
    float metallic;   // -1 = inherit category
    float roughness;  // -1 = inherit category
    int   category;
};

static idHashTable<rtglPbrEntry_t> s_pbrTable;
static bool                        s_pbrTableLoaded = false;

static float RTGL_PbrParseNum( const char *tok ) {
    if ( !tok || !tok[0] || tok[0] == '*' || tok[0] == '-' || idStr::Icmp( tok, "auto" ) == 0 ) {
        return -1.0f;
    }
    return (float)atof( tok );
}

static int RTGL_PbrCategoryForName( const char *cat ) {
    if ( !cat || !cat[0] || cat[0] == '*' || cat[0] == '-' ) {
        return RTGL_PBR_CAT_NONE;
    }
    for ( int i = 1; i < RTGL_PBR_CAT_COUNT; i++ ) {
        if ( idStr::Icmp( cat, s_pbrCatDefaults[i].token ) == 0 ) {
            return i;
        }
    }
    if ( idStr::Icmp( cat, "metal" ) == 0 )         { return RTGL_PBR_CAT_SOLIDMETAL; }
    if ( idStr::Icmp( cat, "metal_painted" ) == 0 ) { return RTGL_PBR_CAT_HOLLOWMETAL; }
    if ( idStr::Icmp( cat, "ceramic_sheen" ) == 0 ) { return RTGL_PBR_CAT_CERAMIC; }
    if ( idStr::Icmp( cat, "metal_rust" ) == 0 )    { return RTGL_PBR_CAT_RUST; }
    if ( idStr::Icmp( cat, "stone" ) == 0 )         { return RTGL_PBR_CAT_CONCRETE; }
    if ( idStr::Icmp( cat, "liquid" ) == 0 )        { return RTGL_PBR_CAT_WATER; }
    return RTGL_PBR_CAT_NONE;
}

static void RTGL_PbrParseTableBuffer( const char *buf, const char *label ) {
    int count = 0;
    const char *p = buf;
    while ( *p ) {
        const char *lineStart = p;
        while ( *p && *p != '\n' && *p != '\r' ) { p++; }
        idStr line( lineStart, 0, (int)( p - lineStart ) );
        while ( *p == '\n' || *p == '\r' ) { p++; }
        int c = line.Find( '#' );
        if ( c >= 0 ) { line = line.Left( c ); }

        char kw[16] = "", cname[64] = "";
        float cm = 0.0f, cr = 0.0f, cw = 0.0f, ce = 0.0f;
        if ( sscanf( line.c_str(), "%15s %63s %f %f %f %f", kw, cname, &cm, &cr, &cw, &ce ) >= 4 &&
             idStr::Icmp( kw, "@cat" ) == 0 ) {
            int cat = RTGL_PbrCategoryForName( cname );
            if ( cat > RTGL_PBR_CAT_NONE && cat < RTGL_PBR_CAT_COUNT ) {
                s_pbrCatDefaults[cat].metallic  = idMath::ClampFloat( 0.0f, 1.0f, cm );
                s_pbrCatDefaults[cat].roughness = idMath::ClampFloat( 0.03f, 1.0f, cr );
            }
            continue;
        }

        char name[256] = "", mTok[32] = "", rTok[32] = "", cTok[32] = "";
        int fields = sscanf( line.c_str(), "%255s %31s %31s %31s", name, mTok, rTok, cTok );
        if ( fields >= 3 ) {
            rtglPbrEntry_t e;
            float m = RTGL_PbrParseNum( mTok );
            float r = RTGL_PbrParseNum( rTok );
            e.metallic  = ( m < 0.0f ) ? -1.0f : idMath::ClampFloat( 0.0f, 1.0f, m );
            e.roughness = ( r < 0.0f ) ? -1.0f : idMath::ClampFloat( 0.03f, 1.0f, r );
            e.category  = RTGL_PbrCategoryForName( cTok );
            idStr key( name );
            key.ToLower();
            s_pbrTable.Set( key, e );
            count++;
        }
    }
    common->Printf( "[RTGL] PBR material table: %d entries loaded from %s\n", count, label );
}

static void RTGL_EnsurePbrTableLoaded( void ) {
    if ( s_pbrTableLoaded ) { return; }
    s_pbrTableLoaded = true;
    s_pbrTable.Clear();

    const char *paths[] = { "pbr/pbr_materials.cfg", "pbr/pbr_overrides.cfg" };
    for ( int i = 0; i < 2; i++ ) {
        char *buf = NULL;
        int len = fileSystem->ReadFile( paths[i], (void **)&buf, NULL );
        if ( len > 0 && buf ) {
            RTGL_PbrParseTableBuffer( buf, paths[i] );
            fileSystem->FreeFile( buf );
        }
    }
}

void RTGL_GetMaterialPBR( const idMaterial *mat,
                          float *outRoughness, float *outMetallic ) {
    if ( !rt_pbr_enable.GetBool() || !mat ) {
        *outRoughness = 1.0f;
        *outMetallic  = 0.0f;
        return;
    }

    RTGL_EnsurePbrTableLoaded();

    // Check if the material has an active SL_SPECULAR stage.
    bool hasSpecularStage = false;
    for ( int i = 0; i < mat->GetNumStages(); i++ ) {
        const shaderStage_t *st = mat->GetStage( i );
        if ( st && st->lighting == SL_SPECULAR && st->texture.image ) {
            hasSpecularStage = true;
            break;
        }
    }

    // Decals and unlit surfaces without specular should stay matte dielectric.
    if ( !hasSpecularStage && ( RTGL_IsDecal( mat ) || !mat->ReceivesLighting() ) ) {
        *outRoughness = 0.90f;
        *outMetallic  = 0.0f;
        return;
    }

    float metallic  = -1.0f;
    float roughness = -1.0f;

    idStr key( mat->GetName() );
    key.ToLower();

    rtglPbrEntry_t *entry = NULL;
    if ( s_pbrTable.Get( key, &entry ) && entry ) {
        int cat = entry->category;
        if ( cat > RTGL_PBR_CAT_NONE && cat < RTGL_PBR_CAT_COUNT ) {
            metallic  = s_pbrCatDefaults[cat].metallic;
            roughness = s_pbrCatDefaults[cat].roughness;
        }
        if ( entry->metallic  >= 0.0f ) { metallic  = entry->metallic; }
        if ( entry->roughness >= 0.0f ) { roughness = entry->roughness; }
    }

    // Heuristic fallback if not in pbr_materials.cfg (or if both were '*' with category 'none')
    if ( metallic < 0.0f || roughness < 0.0f ) {
        surfTypes_t st = mat->GetSurfaceType();
        if ( st == SURFTYPE_METAL || st == SURFTYPE_RICOCHET ) {
            metallic  = 0.50f;
            roughness = 0.38f;
        } else if ( st == SURFTYPE_GLASS ) {
            metallic  = 0.00f;
            roughness = 0.05f;
        } else if ( st == SURFTYPE_FLESH ) {
            metallic  = 0.00f;
            roughness = 0.42f;
        } else if ( st == SURFTYPE_PLASTIC ) {
            metallic  = 0.00f;
            roughness = 0.40f;
        } else if ( st == SURFTYPE_STONE ) {
            metallic  = 0.00f;
            roughness = hasSpecularStage ? 0.48f : 0.90f;
        } else if ( key.Find( "base_floor/" ) >= 0 || key.Find( "base_trim/" ) >= 0 ||
                    key.Find( "base_door/" ) >= 0  || key.Find( "base_wall/" ) >= 0 ||
                    key.Find( "base_light/" ) >= 0 || key.Find( "mcity/" ) >= 0 ||
                    key.Find( "alphalabs/" ) >= 0  || key.Find( "enpro/" ) >= 0 ||
                    key.Find( "delta/" ) >= 0      || key.Find( "commoutside/" ) >= 0 ||
                    key.Find( "recycling/" ) >= 0  || key.Find( "monorail/" ) >= 0 ||
                    key.Find( "weapons/" ) >= 0 ) {
            metallic  = hasSpecularStage ? 0.35f : 0.15f;
            roughness = hasSpecularStage ? 0.40f : 0.60f;
        } else if ( key.Find( "characters/" ) >= 0 || key.Find( "monsters/" ) >= 0 ) {
            metallic  = 0.05f;
            roughness = hasSpecularStage ? 0.42f : 0.65f;
        } else {
            metallic  = hasSpecularStage ? 0.20f : 0.00f;
            roughness = hasSpecularStage ? 0.45f : 0.85f;
        }
    }

    // Crucial for RTGL1 (`CmSVGFAtrous.comp:304` + `RtRaygenIndirect.inl:92`):
    // RTGL1's `FAKE_ROUGH_SPECULAR_THRESHOLD` is `0.50` (`0.50..0.75` smoothstep).
    // Any surface with an explicit Doom 3 `specularmap` (`SL_SPECULAR`) was
    // authored to show specular highlights! If a category (e.g. `rust` at 0.49
    // or `armor` at 0.50) puts roughness right at or above 0.50, RTGL1 stops
    // tracing GGX specular bounce rays (`useDiffuse(r) = r >= 0.5`) and blends
    // `filteredSpec` into `indirect`. Clamping specular-mapped materials to
    // `<= 0.46` guarantees they stay in the true GGX specular path, and raising
    // their minimum metallic to `>= 0.25` compensates for RTGL1's `0.04` F0
    // demodulation (`ro_s = mix(0.04, albedo, metallic)`).
    if ( hasSpecularStage ) {
        if ( roughness > 0.46f ) { roughness = 0.46f; }
        if ( metallic  < 0.25f ) { metallic  = 0.25f; }
    }

    roughness = idMath::ClampFloat( 0.03f, 1.0f, roughness * rt_pbr_roughnessScale.GetFloat() );
    metallic  = idMath::ClampFloat( 0.00f, 1.0f, metallic  * rt_pbr_metallicScale.GetFloat() );

    *outRoughness = roughness;
    *outMetallic  = metallic;
}

#endif // D3_RTGL