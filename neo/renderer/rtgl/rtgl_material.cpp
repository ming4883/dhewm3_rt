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
// Scope note: first-pass link-through for high-frequency diffuse materials;
// multi-stage bump/specular layering, PBR overrides, scroll/deform and fog are
// deliberately left for P3 iteration / P7.
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

// Gate the bake on rt_decal_mode: modes 1 (alpha-tested cutout) and 3 (DECAL
// raster overlay) bake; mode 0 (legacy) and 2 (skip; handled in SubmitSurf)
// do not bake.
static int RTGL_DecalBakeMode( const idMaterial *mat ) {
    int dm = rt_decal_mode.GetInteger();
    if ( dm != 1 && dm != 3 ) { return 0; }
    return RTGL_DecalBlendType( mat );
}

// Load `loadName`'s RGBA, optionally bake a coverage alpha from RGB, and hand
// it to RTGL1 under `provideName`. Names are cached (s_provided) so each key is
// uploaded once per session.
static void RTGL_ProvideTexture( const char *loadName, const char *provideName, int bakeMode ) {
    if ( !loadName || !loadName[0] || !provideName || !provideName[0] ) { return; }
    if ( RTGL_WasProvided( provideName ) ) { return; }
    if ( !rg.rgProvideOriginalTexture ) { return; }

    byte *pic = NULL;
    int width = 0, height = 0;
    ID_TIME_T timestamp = 0;
    textureDepth_t depth = TD_DEFAULT;

    // This is the exact call idImage::ActuallyLoadImage() uses for imgName,
    // so it handles both plain files and image-program names.
    R_LoadImageProgram( loadName, &pic, &width, &height, &timestamp, &depth );

    if ( !pic || width <= 0 || height <= 0 ) {
        if ( pic ) { R_StaticFree( pic ); }
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

    RgOriginalTextureInfo info = {};
    info.sType = RG_STRUCTURE_TYPE_ORIGINAL_TEXTURE_INFO;
    info.pNext = NULL;
    info.pTextureName = provideName;
    info.pPixels = pic;
    info.size = RTGL_Ext( (uint32_t)width, (uint32_t)height );
    info.filter = RG_SAMPLER_FILTER_LINEAR;
    info.addressModeU = RG_SAMPLER_ADDRESS_MODE_REPEAT;
    info.addressModeV = RG_SAMPLER_ADDRESS_MODE_REPEAT;

    RgResult r = rg.rgProvideOriginalTexture( &info );
    R_StaticFree( pic );

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
const char * RTGL_PrimaryTexture( const idMaterial *mat ) {
    if ( !mat || mat->GetNumStages() <= 0 ) { return NULL; }

    const idImage *chosen = NULL;
    // Prefer a diffuse stage, then bump, then any stage carrying an image.
    for ( int pass = 0; pass < 3 && !chosen; pass++ ) {
        for ( int i = 0; i < mat->GetNumStages(); i++ ) {
            const shaderStage_t *stage = mat->GetStage( i );
            if ( !stage || !stage->texture.image ) { continue; }
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
    int bake = RTGL_DecalBakeMode( mat );
    if ( bake != 0 ) {
        const char *derived = RTGL_DerivedName( name, bake );
        RTGL_ProvideTexture( name, derived, bake );
        return derived;
    }
    RTGL_ProvideTexture( name, name, 0 );
    return name;
}

void RTGL_ClassifyMaterial( const idMaterial *mat,
                            RgMeshPrimitiveFlags *outFlags, float *outEmissive ) {
    RgMeshPrimitiveFlags flags = 0;
    float emissive = 0.0f;

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
            int dst = stage->drawStateBits & GLS_DSTBLEND_BITS;
            // Only "blend add" (dst=ONE) is genuine emissive glow. "blend filter"
            // and the inverted-multiply blends are DARKENING overlays, not glow;
            // treating them as emissive makes their identity texels glow.
            if ( dst == GLS_DSTBLEND_ONE ) {
                if ( rt_emissive_enable.GetBool() ) {
                    emissive = 1.0f * rt_emissive_mult.GetFloat();
                } else {
                    emissive = 0.0f;
                }
                flags |= RG_MESH_PRIMITIVE_NO_SHADOW;
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