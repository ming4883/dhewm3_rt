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
// mechanism gzdoom-rt uses for its 2D/HUD layer (rt_main.cpp:4655 etc.).
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

// Mirror of draw_common.cpp:1108-1111 (RB_T_RenderShaderPasses old-style
// stages). shaderRegisters are evaluated by R_AddDrawSurf from the GUI
// surface's shaderParms (SetColor), so this yields the exact GL color.
static RgColor4DPacked32 RTGL_UIColor( const drawSurf_t *surf ) {
    const idMaterial *mat = surf->material;
    const float *regs = surf->shaderRegisters;
    if ( mat && mat->GetNumStages() > 0 && regs ) {
        const shaderStage_t *stage = mat->GetStage( 0 );
        if ( stage ) {
            return RTGL_PackColorF( regs[ stage->color.registers[0] ],
                                    regs[ stage->color.registers[1] ],
                                    regs[ stage->color.registers[2] ],
                                    regs[ stage->color.registers[3] ] );
        }
    }
    return RTGL_White();
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

            RgMeshPrimitiveInfo prim = {};
            prim.sType                = RG_STRUCTURE_TYPE_MESH_PRIMITIVE_INFO;
            prim.pNext                = &sw;
            prim.flags                = RG_MESH_PRIMITIVE_TRANSLUCENT;
            prim.primitiveIndexInMesh = 0;
            prim.pVertices            = s_uiVerts.Ptr();
            prim.vertexCount          = (uint32_t)tri->numVerts;
            prim.pIndices             = s_uiIndices.Ptr();
            prim.indexCount           = (uint32_t)tri->numIndexes;
            prim.pTextureName         = RTGL_GuiTexture( surf->material );
            prim.textureFrame         = 0;
            prim.color                = RTGL_UIColor( surf );
            prim.emissive             = 0.0f;
            prim.classicLight         = 1.0f;

            // mesh == NULL is the documented swapchained path (gzdoom-rt does
            // the same); VulkanDevice.cpp routes it to Rasterizer::Upload with
            // an identity transform, bypassing uploadPrimitive_Core's assert.
            RgResult r = rg.rgUploadMeshPrimitive( NULL, &prim );
            if ( r == RG_RESULT_SUCCESS || r == RG_RESULT_SUCCESS_FOUND_MESH ) {
                g_rtglStats.uiPrims++;
                g_rtglStats.uiVerts += tri->numVerts;
            }
        }
    }
}

#endif // D3_RTGL
