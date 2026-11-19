#include "sys/platform.h"
#include "framework/Common.h"
#include "idlib/Str.h"

#ifdef D3_RTGL

#include "renderer/rtgl/rtgl_internal.h"

// ===========================================================================
// P-UI (Plan B, 2026-11-14) - in-game 2D GUI / console / HUD composition
//
// Why this file exists
// --------------------
// In the RTGL path R_IssueRenderCommands() takes an early return (RenderSystem
// .cpp) after RTGL_SubmitFrameCommands() and skips the entire GL back end
// (RB_ExecuteBackEndCommands). All of Doom 3's 2D rendering (console, HUD,
// menus, PDA) goes through idGuiModel -> DrawStretchPic -> a 2D viewDef
// (EmitFullScreen) -> GL 2D stretchPic commands. Those commands were simply
// discarded, so the in-game UI was invisible even though input and the
// separate Win32 console (win_viewlog) worked.
//
// The fix: walk the SAME command chain and re-emit the 2D views' drawSurfs as
// RTGL1 *swapchained* primitives (RgMeshPrimitiveSwapchainedEXT). RTGL1
// rasterizes those directly into the swapchain image after the path-traced
// frame (VulkanDevice.cpp DrawToSwapchain, LOAD_OP_LOAD so the 3D image is
// preserved), with alpha blending and no depth test. This is exactly the
// 2D/HUD overlay mechanism (see RTGL rasterizer / HUD composite).
//
// IMPORTANT: this is PORT-SIDE ONLY. The swapchain rasterizer pipelines and
// shaders (RsRasterizer.vert.spv / RsSwapchain.frag.spv) already ship inside
// the deployed RTGL1.dll + aligned spv set, so no DLL rebuild / no shader
// regen is needed -> the DLL/spv stay aligned (avoids the historical
// "all-black" layout-mismatch trap).
//
// Coordinate / color conventions
// ------------------------------
//  * Doom3 2D GUI is authored in a fixed 640x480 virtual screen
//    (SCREEN_WIDTH/HEIGHT, RenderSystem.h), origin top-left, +Y down.
//  * We emit vertex positions ALREADY in Vulkan clip space and pass an
//    identity pViewProjection (RsRasterizer.vert: gl_Position = viewProj *
//    position; mesh==NULL -> instance transform identity). Vulkan NDC:
//    x=-1 left..+1 right, y=-1 TOP..+1 bottom (positive-height viewport).
//    So: ndcX = 2*x/640 - 1, ndcY = 2*y/480 - 1. rt_ui_flipY inverts Y if
//    the runtime shows the UI upside down (visual check is left to the user;
//    we only compile here, per the standing "compile, don't launch" rule).
//  * Per-surface color comes from the material's evaluated shaderRegisters,
//    exactly like the GL backend (draw_common.cpp:1108-1111):
//    color[k] = regs[ stage0->color.registers[k] ]. We put it in
//    RgMeshPrimitiveInfo::color (the push-constant packedColor) and leave the
//    per-vertex color white, so the final fragment color is
//    unpack(packedColor) * white * texture.
//  * A NULL pTextureName -> RTGL1's 1x1 white texture -> solid color quads
//    (used by "_white" console bars / fills).
//  * RG_MESH_PRIMITIVE_TRANSLUCENT -> SRC_ALPHA / ONE_MINUS_SRC_ALPHA blend,
//    no depth test (SWAPCHAIN raster type), painter's order = submit order.
//
// Limitations (P-UI first pass, for later refinement):
//  * Only the first material stage is used (RTGL1 draws one texture per
//    swapchained primitive; multi-stage GUI materials are approximated).
//  * Offscreen 2D views (screenshot / remote render) are also drawn, since
//    this port has no separate render target for them.
//  * Font texture alpha / blend mode is taken as-is from the material.
//
// In-world interactive GUIs (terminals/keypads/screens) are NOT pure 2D
// views: they are injected into the 3D view as SS_GUI quads. They are
// handled by RTGL_SubmitWorldGuis() below, which by default submits them
// to the WORLD raster channel so they depth-test against the scene (the
// earlier swapchain approach drew them through characters/geometry).
// ===========================================================================

static idList<RgPrimitiveVertex> s_uiVerts;
static idList<uint32_t>          s_uiIndices;

// Identity matrix, valid for the whole call (rgUploadMeshPrimitive copies the
// 16 floats into the collector; see RasterizedDataCollector.cpp:286).
static const float kUI_IdentityVP[16] = {
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f,
};

static void RTGL_UIVertex( RgPrimitiveVertex *dst, const idDrawVert *src,
                           float sx, float sy, bool flipY ) {
    float ny = src->xyz.y * sy - 1.0f;      // Vulkan: y=0 (top) -> -1
    if ( flipY ) { ny = -ny; }
    dst->position[0] = src->xyz.x * sx - 1.0f;
    dst->position[1] = ny;
    dst->position[2] = 0.0f;
    dst->normalPacked = 0;                  // unused by the rasterizer VS
    dst->texCoord[0]  = src->st.x;
    dst->texCoord[1]  = src->st.y;
    dst->color        = 0xFFFFFFFF;         // color lives in prim.color
    dst->_pad0        = 0;
}

struct rtglGuiStageDraw_t {
    const char        *textureName;
    RgColor4DPacked32  color;
    float              emissive;
    bool               hasTexMat;
    float              texMat[2][3];
};

static void RTGL_ApplyGuiTexMat( RgPrimitiveVertex *verts, const idDrawVert *srcVerts,
                                 int numVerts, const rtglGuiStageDraw_t &st ) {
    if ( !st.hasTexMat ) {
        for ( int k = 0; k < numVerts; k++ ) {
            verts[k].texCoord[0] = srcVerts[k].st.x;
            verts[k].texCoord[1] = srcVerts[k].st.y;
        }
        return;
    }
    for ( int k = 0; k < numVerts; k++ ) {
        float u = srcVerts[k].st.x;
        float v = srcVerts[k].st.y;
        verts[k].texCoord[0] = u * st.texMat[0][0] + v * st.texMat[0][1] + st.texMat[0][2];
        verts[k].texCoord[1] = u * st.texMat[1][0] + v * st.texMat[1][1] + st.texMat[1][2];
    }
}

// Mirror of draw_common.cpp:1017-1124 (RB_T_RenderShaderPasses).
// Evaluates conditionRegister, blend mode (blend add -> ADDITIVE via
// emissive > FLT_EPSILON; blend blend -> standard SRC_ALPHA/ONE_MINUS_SRC_ALPHA;
// maskcolor / GL_DST_ALPHA skipped), stage color, and 2x3 texture matrix.
static bool RTGL_EvalGuiStage( const drawSurf_t *surf, int stageIndex,
                               rtglGuiStageDraw_t *out ) {
    const idMaterial *mat = surf ? surf->material : NULL;
    if ( !mat || stageIndex < 0 || stageIndex >= mat->GetNumStages() ) {
        return false;
    }
    const shaderStage_t *stage = mat->GetStage( stageIndex );
    if ( !stage ) { return false; }

    const float *regs = surf->shaderRegisters;
    if ( regs && !regs[ stage->conditionRegister ] ) {
        return false;
    }
    if ( stage->lighting != SL_AMBIENT ) {
        return false;
    }
    if ( ( stage->drawStateBits & ( GLS_REDMASK | GLS_GREENMASK | GLS_BLUEMASK ) ) ==
         ( GLS_REDMASK | GLS_GREENMASK | GLS_BLUEMASK ) ) {
        return false; // maskcolor-only stage
    }

    int srcBlend = stage->drawStateBits & GLS_SRCBLEND_BITS;
    int dstBlend = stage->drawStateBits & GLS_DSTBLEND_BITS;
    if ( srcBlend == GLS_SRCBLEND_DST_ALPHA ||
         srcBlend == GLS_SRCBLEND_ONE_MINUS_DST_ALPHA ||
         dstBlend == GLS_DSTBLEND_DST_ALPHA ||
         dstBlend == GLS_DSTBLEND_ONE_MINUS_DST_ALPHA ) {
        return false; // relies on prior maskcolor pass into dest alpha
    }

    float cr = 1.0f, cg = 1.0f, cb = 1.0f, ca = 1.0f;
    if ( regs ) {
        cr = regs[ stage->color.registers[0] ];
        cg = regs[ stage->color.registers[1] ];
        cb = regs[ stage->color.registers[2] ];
        ca = regs[ stage->color.registers[3] ];
    }

    int bakeMode = 0;
    float emissive = 0.0f;
    if ( dstBlend == GLS_DSTBLEND_ONE ) {
        // Additive stage (e.g. gui/addhighlight, gui/test/gui_scanlines,
        // gui/outerglowbar, gui/addlogo). Without ADDITIVE blend, these
        // full-screen 24-bit black-background TGAs (alpha=1) overwrite the
        // entire door/computer screen with opaque black.
        if ( cr <= 0.0f && cg <= 0.0f && cb <= 0.0f ) {
            return false;
        }
        if ( srcBlend == GLS_SRCBLEND_ONE ) {
            ca = 1.0f;
        } else if ( ca <= 0.0f ) {
            return false;
        }
        emissive = 1e-4f; // > FLT_EPSILON -> PipelineStateFlagBits::ADDITIVE
    } else if ( srcBlend == GLS_SRCBLEND_DST_COLOR && dstBlend == GLS_DSTBLEND_ZERO ) {
        bakeMode = 1;
        ca = 1.0f;
    } else if ( srcBlend == GLS_SRCBLEND_ZERO && dstBlend == GLS_DSTBLEND_ONE_MINUS_SRC_COLOR ) {
        bakeMode = 2;
        ca = 1.0f;
    } else if ( srcBlend == GLS_SRCBLEND_ONE && dstBlend == GLS_DSTBLEND_ZERO ) {
        ca = 1.0f;
    } else {
        if ( ca <= 0.0f ) {
            return false;
        }
    }

    out->textureName = RTGL_GuiStageTexture( stage, bakeMode );
    out->color       = RTGL_PackColorF( cr, cg, cb, ca );
    out->emissive    = emissive;
    out->hasTexMat   = ( stage->texture.hasMatrix && regs != NULL );
    if ( out->hasTexMat ) {
        out->texMat[0][0] = regs[ stage->texture.matrix[0][0] ];
        out->texMat[0][1] = regs[ stage->texture.matrix[0][1] ];
        out->texMat[0][2] = regs[ stage->texture.matrix[0][2] ];
        out->texMat[1][0] = regs[ stage->texture.matrix[1][0] ];
        out->texMat[1][1] = regs[ stage->texture.matrix[1][1] ];
        out->texMat[1][2] = regs[ stage->texture.matrix[1][2] ];
    }
    return true;
}

void RTGL_SubmitUI( const void *cmdHead ) {
    if ( !rt_ui_enable.GetBool() ) { return; }
    if ( !RTGL_FrameActive() ) { return; }
    if ( !rg.rgUploadMeshPrimitive ) { return; }
    const emptyCommand_t *cmd = (const emptyCommand_t *)cmdHead;
    if ( !cmd ) { return; }

    const bool flipY = rt_ui_flipY.GetBool();
    // Doom3 GUI virtual screen is a fixed 640x480 (SCREEN_WIDTH/HEIGHT in
    // RenderSystem.h). Use literals so this file has no extra include deps.
    const float sx = 2.0f / 640.0f;
    const float sy = 2.0f / 480.0f;

    for ( const emptyCommand_t *c = cmd; c; c = (const emptyCommand_t *)c->next ) {
        if ( c->commandId != RC_DRAW_VIEW ) { continue; }
        const drawSurfsCommand_t *dv = (const drawSurfsCommand_t *)c;
        const viewDef_t *v = dv->viewDef;
        if ( !v ) { continue; }
        // 2D / GUI view: no viewEntitys (same test the GL backend uses at
        // tr_backend.cpp:666 to count a "2D" view). 3D views have entitys and
        // are handled by RTGL_SubmitCommands (world geometry) instead.
        if ( v->viewEntitys ) { continue; }

        for ( int i = 0; i < v->numDrawSurfs; i++ ) {
            const drawSurf_t *surf = v->drawSurfs[i];
            g_rtglStats.uiSurfs++;
            if ( !surf || !surf->geo || !surf->material ) { continue; }
            const srfTriangles_t *tri = surf->geo;
            if ( tri->numVerts <= 0 || tri->numIndexes <= 0 ||
                 !tri->verts || !tri->indexes ) { continue; }

            s_uiVerts.SetNum( tri->numVerts );
            for ( int k = 0; k < tri->numVerts; k++ ) {
                RTGL_UIVertex( &s_uiVerts[k], &tri->verts[k], sx, sy, flipY );
            }
            s_uiIndices.SetNum( tri->numIndexes );
            for ( int k = 0; k < tri->numIndexes; k++ ) {
                s_uiIndices[k] = (uint32_t)tri->indexes[k];
            }

            RgMeshPrimitiveSwapchainedEXT sw = {};
            sw.sType           = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_SWAPCHAINED_EXT;
            sw.pNext           = NULL;
            sw.flags           = 0;
            sw.pViewport       = NULL;             // full swapchain viewport
            sw.pView           = NULL;
            sw.pProjection     = NULL;
            sw.pViewProjection = kUI_IdentityVP;  // positions are already NDC

            const int nStages = surf->material->GetNumStages();
            for ( int stIdx = 0; stIdx < nStages; stIdx++ ) {
                rtglGuiStageDraw_t st;
                if ( !RTGL_EvalGuiStage( surf, stIdx, &st ) ) { continue; }
                RTGL_ApplyGuiTexMat( s_uiVerts.Ptr(), tri->verts, tri->numVerts, st );

                RgMeshPrimitiveInfo prim = {};
                prim.sType                = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_INFO;
                prim.pNext                = &sw;
                prim.flags                = RG_MESH_PRIMITIVE_TRANSLUCENT;
                prim.primitiveIndexInMesh = (uint32_t)stIdx;
                prim.pVertices            = s_uiVerts.Ptr();
                prim.vertexCount          = (uint32_t)tri->numVerts;
                prim.pIndices             = s_uiIndices.Ptr();
                prim.indexCount           = (uint32_t)tri->numIndexes;
                prim.pTextureName         = st.textureName;
                prim.textureFrame         = 0;
                prim.color                = st.color;
                prim.emissive             = st.emissive;
                prim.classicLight         = 1.0f;

                RgResult r = rg.rgUploadMeshPrimitive( NULL, &prim );
                if ( r == RG_RESULT_SUCCESS || r == RG_RESULT_SUCCESS_FOUND_MESH ) {
                    g_rtglStats.uiPrims++;
                    g_rtglStats.uiVerts += tri->numVerts;
                }
            }
        }
    }
}




// ===========================================================================
// P-UI / world GUIs - depth-tested against the scene (2026-11-14 fix #2)
//
// In-world interactive GUI surfaces (terminals, keypads, security monitors,
// PDA panels) reach us as SS_GUI (sort -2) drawSurfs inside the 3D view
// (tr_light.cpp:1339 R_RenderGuiSurf -> EmitToCurrentView -> EmitSurface).
// gui->Redraw() rebuilds these quads every frame -> the "animated" content.
//
// History:
//  * first pass: submitted them as SWAPCHAIN overlays (mesh==NULL +
//    RgMeshPrimitiveSwapchainedEXT). That drew them, but the SWAPCHAIN raster
//    pass has NO depth attachment: RasterizedDataCollector.cpp ToPipelineState
//    only adds DEPTH_TEST when rasterType != SWAPCHAIN. Result: the GUI drew on
//    top of everything -> a screen floating through a character's back.
//
// This fix (mode 1, default): submit them through the WORLD raster channel so
// they are depth-tested against the ray-traced scene:
//   * RasterPass::CreateWorldRenderPass depth attachment LOADs the depth that
//     DepthCopying::Process(false) copies from FB_IMAGE_INDEX_DEPTH_NDC (the
//     real primary-hit depth written by RaygenPrimary.inl:129/442). The port
//     runs full ray tracing (RG_EndFrame -> rgDrawFrame, no disable* flags),
//     so that depth is genuine.
//   * ToPipelineState gives WORLD primitives DEPTH_TEST (LESS_OR_EQUAL),
//     pipeline depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL.
//   * DrawToFinalImage runs AFTER denoiser->Denoise (VulkanDevice.cpp:738/750),
//     so the crisp GUI is not blurred; illumVolumeEnable=0 so RsWorld.inl
//     only multiplies by max(1, avgLuminance) - no darkening.
//
// Geometry convention (mirrors the proven P2 world path in rtgl_geometry.cpp):
// vertices are transformed to WORLD space by the surface's guiSpace->
// modelMatrix (Doom column-major), mesh.transform = identity, and NO
// SwapchainedEXT chain. RG_MESH_PRIMITIVE_TRANSLUCENT makes IsRasterized()
// route it to GeometryRasterType::WORLD; RTGL1 projects with its own
// cameraInfo view/proj - the SAME camera used for the ray-traced walls, so the
// GUI lands exactly on its panel and its depth is comparable to the scene.
//
// Coplanar z-fighting: Doom walls are zero-thickness plane sheets and the GUI
// quad sits exactly on the panel surface. We push each vertex a tiny fraction
// of the way toward the camera (rt_ui_worldGuisDepthBias, default 0.002):
//     p += (camPos - p) * bias
// Because the offset is along the view ray, the perspective-projected screen
// position is UNCHANGED while NDC depth is pulled slightly closer, so the GUI
// wins the LESS_OR_EQUAL test against the wall without visibly floating.
//
// PORT-SIDE ONLY: reuses the already-deployed WORLD rasterizer + shaders; no
// DLL/spv rebuild, so they stay aligned (no "all-black" layout-mismatch trap).
// rt_ui_worldGuisMode 0 restores the old swapchain (no depth) path as a
// fallback if runtime validation shows a problem (we compile but don't run).
// ===========================================================================

static idList<RgPrimitiveVertex> s_wgVerts;
static idList<uint32_t>          s_wgIndices;

// Doom column-major 4x4 transform of an in-place vec4 (m[col*4+row]).
static void RTGL_Transform4( const float m[16], float v[4] ) {
    float o[4];
    for ( int r = 0; r < 4; r++ ) {
        o[r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r] * v[3];
    }
    v[0] = o[0]; v[1] = o[1]; v[2] = o[2]; v[3] = o[3];
}

// Old (mode 0) swapchain path: GUI vertices baked to NDC, drawn with no
// depth test. Kept only as a runtime fallback; see the mode-1 path below.
static RgResult RTGL_SubmitWorldGui_Swapchain( const drawSurf_t *surf,
                                               const srfTriangles_t *tri,
                                               const viewDef_t *v,
                                               bool flipY ) {
    static const float kIdentityVP[16] = {
        1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,0,1,
    };
    const float *mv   = surf->space->modelViewMatrix;
    const float *proj = v->projectionMatrix;
    s_wgVerts.SetNum( tri->numVerts );
    for ( int k = 0; k < tri->numVerts; k++ ) {
        const idDrawVert *src = &tri->verts[k];
        float p[4] = { src->xyz.x, src->xyz.y, src->xyz.z, 1.0f };
        RTGL_Transform4( mv, p );
        RTGL_Transform4( proj, p );
        if ( p[3] <= 1e-4f ) { return RG_RESULT_WRONG_FUNCTION_ARGUMENT; }
        float invw = 1.0f / p[3];
        float ny = -( p[1] * invw );
        if ( flipY ) { ny = -ny; }
        RgPrimitiveVertex *dst = &s_wgVerts[k];
        dst->position[0] = p[0] * invw;
        dst->position[1] = ny;
        dst->position[2] = 0.0f;
        dst->normalPacked = 0;
        dst->texCoord[0]  = src->st.x;
        dst->texCoord[1]  = src->st.y;
        dst->color        = 0xFFFFFFFF;
        dst->_pad0        = 0;
    }
    s_wgIndices.SetNum( tri->numIndexes );
    for ( int k = 0; k < tri->numIndexes; k++ ) { s_wgIndices[k] = (uint32_t)tri->indexes[k]; }

    RgMeshPrimitiveSwapchainedEXT sw = {};
    sw.sType           = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_SWAPCHAINED_EXT;
    sw.pViewProjection = kIdentityVP;

    RgResult lastRes = RG_RESULT_SUCCESS;
    const int nStages = surf->material->GetNumStages();
    for ( int stIdx = 0; stIdx < nStages; stIdx++ ) {
        rtglGuiStageDraw_t st;
        if ( !RTGL_EvalGuiStage( surf, stIdx, &st ) ) { continue; }
        RTGL_ApplyGuiTexMat( s_wgVerts.Ptr(), tri->verts, tri->numVerts, st );

        RgMeshPrimitiveInfo prim = {};
        prim.sType                = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_INFO;
        prim.pNext                = &sw;
        prim.flags                = RG_MESH_PRIMITIVE_TRANSLUCENT;
        prim.primitiveIndexInMesh = (uint32_t)stIdx;
        prim.pVertices            = s_wgVerts.Ptr();
        prim.vertexCount          = (uint32_t)tri->numVerts;
        prim.pIndices             = s_wgIndices.Ptr();
        prim.indexCount           = (uint32_t)tri->numIndexes;
        prim.pTextureName         = st.textureName;
        prim.color                = st.color;
        prim.emissive             = st.emissive;
        prim.classicLight         = 1.0f;
        lastRes = rg.rgUploadMeshPrimitive( NULL, &prim );
    }
    return lastRes;
}

// New (mode 1) WORLD path: world-space vertices + depth test against scene.
static RgResult RTGL_SubmitWorldGui_World( const drawSurf_t *surf,
                                           const srfTriangles_t *tri,
                                           const viewDef_t *v,
                                           float bias ) {
    const float *mm = surf->space->modelMatrix;       // GUI local -> world
    const idVec3 &cam = v->renderView.vieworg;         // camera world position
    s_wgVerts.SetNum( tri->numVerts );
    for ( int k = 0; k < tri->numVerts; k++ ) {
        const idDrawVert *src = &tri->verts[k];
        float p[4] = { src->xyz.x, src->xyz.y, src->xyz.z, 1.0f };
        RTGL_Transform4( mm, p );                     // -> world space
        if ( bias > 0.0f ) {
            p[0] += ( cam.x - p[0] ) * bias;
            p[1] += ( cam.y - p[1] ) * bias;
            p[2] += ( cam.z - p[2] ) * bias;
        }
        RgPrimitiveVertex *dst = &s_wgVerts[k];
        dst->position[0] = p[0];
        dst->position[1] = p[1];
        dst->position[2] = p[2];
        dst->normalPacked = 0;                        // rasterizer ignores
        dst->texCoord[0]  = src->st.x;
        dst->texCoord[1]  = src->st.y;
        dst->color        = 0xFFFFFFFF;               // color in prim.color
        dst->_pad0        = 0;
    }
    s_wgIndices.SetNum( tri->numIndexes );
    for ( int k = 0; k < tri->numIndexes; k++ ) { s_wgIndices[k] = (uint32_t)tri->indexes[k]; }

    static uint64_t s_worldGuiId = 0;
    RgResult lastRes = RG_RESULT_SUCCESS;
    const int nStages = surf->material->GetNumStages();
    for ( int stIdx = 0; stIdx < nStages; stIdx++ ) {
        rtglGuiStageDraw_t st;
        if ( !RTGL_EvalGuiStage( surf, stIdx, &st ) ) { continue; }
        RTGL_ApplyGuiTexMat( s_wgVerts.Ptr(), tri->verts, tri->numVerts, st );

        RgMeshInfo mesh = {};
        mesh.sType = RG_STRUCTURE_TYPE_MESH_INFO;
        mesh.pNext = NULL;
        mesh.flags = 0;
        mesh.uniqueObjectID = 0x200000000ULL + ( ++s_worldGuiId );
        mesh.pMeshName = NULL;
        RTGL_IdentityTransform( &mesh.transform );   // verts already in world
        mesh.isExportable = 0;
        mesh.animationTime = 0.0f;
        mesh.localLightsIntensity = 1.0f;

        RgMeshPrimitiveInfo prim = {};
        prim.sType                = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_INFO;
        prim.pNext                = NULL;
        prim.flags                = RG_MESH_PRIMITIVE_TRANSLUCENT;
        prim.primitiveIndexInMesh = (uint32_t)stIdx;
        prim.pVertices            = s_wgVerts.Ptr();
        prim.vertexCount          = (uint32_t)tri->numVerts;
        prim.pIndices             = s_wgIndices.Ptr();
        prim.indexCount           = (uint32_t)tri->numIndexes;
        prim.pTextureName         = st.textureName;
        prim.textureFrame         = 0;
        prim.color                = st.color;
        prim.emissive             = st.emissive;
        prim.classicLight         = 1.0f;
        lastRes = rg.rgUploadMeshPrimitive( &mesh, &prim );
    }
    return lastRes;
}

void RTGL_SubmitWorldGuis( const void *cmdHead ) {
    if ( !rt_ui_enable.GetBool() || !rt_ui_worldGuis.GetBool() ) { return; }
    if ( !RTGL_FrameActive() ) { return; }
    if ( !rg.rgUploadMeshPrimitive ) { return; }
    const emptyCommand_t *cmd = (const emptyCommand_t *)cmdHead;
    if ( !cmd ) { return; }

    const int   mode  = rt_ui_worldGuisMode.GetInteger();  // 0=swapchain, 1=world
    const bool  flipY = rt_ui_worldGuisFlipY.GetBool();
    const float bias  = rt_ui_worldGuisDepthBias.GetFloat();

    for ( const emptyCommand_t *c = cmd; c; c = (const emptyCommand_t *)c->next ) {
        if ( c->commandId != RC_DRAW_VIEW ) { continue; }
        const drawSurfsCommand_t *dv = (const drawSurfsCommand_t *)c;
        const viewDef_t *v = dv->viewDef;
        if ( !v ) { continue; }
        if ( !v->viewEntitys ) { continue; }          // 3D views only
        if ( v->isSubview || v->isMirror || v->isXraySubview ) { continue; }

        for ( int i = 0; i < v->numDrawSurfs; i++ ) {
            const drawSurf_t *surf = v->drawSurfs[i];
            if ( !surf || !surf->geo || !surf->material ) { continue; }
            if ( surf->material->GetSort() >= 0.0f ) { continue; } // SS_GUI only
            if ( !surf->space ) { continue; }
            const srfTriangles_t *tri = surf->geo;
            if ( tri->numVerts <= 0 || tri->numIndexes <= 0 ||
                 !tri->verts || !tri->indexes ) { continue; }

            g_rtglStats.worldGuiSurfs++;

            RgResult r = ( mode == 0 )
                ? RTGL_SubmitWorldGui_Swapchain( surf, tri, v, flipY )
                : RTGL_SubmitWorldGui_World( surf, tri, v, bias );

            if ( r == RG_RESULT_SUCCESS || r == RG_RESULT_SUCCESS_FOUND_MESH ) {
                g_rtglStats.worldGuiPrims++;
                g_rtglStats.worldGuiVerts += tri->numVerts;
            }
        }
    }
}

#endif // D3_RTGL
