// rtgl_heathaze.h
// dhewm3-owned, app-side heat-haze / screen-space refraction custom pass.
//
// The per-frame POD and its magic/version constants live here (moved from
// RTGL1.h in stage 3). It is game-specific (Doom 3 heatHazeWithMask.vfp
// screen-space quads); RTGL1.dll no longer knows this layout.

#ifndef __D3_RTGL_HEATHAZE_H__
#define __D3_RTGL_HEATHAZE_H__

#ifndef RG_USE_SURFACE_WIN32
#define RG_USE_SURFACE_WIN32
#endif
#include "RTGL1/RTGL1.h"   // RGAPI_CALL, uint32_t, RgDrawFrameCustomPassParams

// ---------------------------------------------------------------------------
// Per-frame payload packed by rtgl_heathaze.cpp (from rtgl_geometry.cpp quads).
// magic/version guard the packer <-> drawer contract.
// ---------------------------------------------------------------------------
#define RG_CUSTOM_PASS_HEATHAZE_MAGIC      0x48485A31u /* "HHZ1" */
#define RG_CUSTOM_PASS_HEATHAZE_VERSION    1u
#define RG_CUSTOM_PASS_HEATHAZE_MAX_QUADS 64

typedef struct RgCustomPassHeatHazeQuad
{
    /* Screen-space rectangle in normalized [0,1] coords, top-left origin. */
    float rectX;
    float rectY;
    float rectW;
    float rectH;
    /* Per-quad animation phase (from the material's scroll registers). */
    float phase;
    /* Distortion magnitude for this quad. */
    float strength;
    /* Edge feather width in normalized units (mask). */
    float maskFeather;
    float reserved;
} RgCustomPassHeatHazeQuad;

typedef struct RgCustomPassHeatHazeFrame
{
    uint32_t magic;
    uint32_t version;
    uint32_t quadCount;
    float    time;          /* animation time in seconds */
    float    ior;           /* index of refraction (rt_refr_index) */
    float    waveSpeed;     /* wave animation speed */
    float    waveStrength;  /* normal-perturbation strength */
    float    targetWidth;   /* render target width (validation only) */
    float    targetHeight;  /* render target height (validation only) */
    RgCustomPassHeatHazeQuad quads[ RG_CUSTOM_PASS_HEATHAZE_MAX_QUADS ];
} RgCustomPassHeatHazeFrame;

// ---------------------------------------------------------------------------
// App-side custom pass. Only two entry points are shared with rtgl_main.cpp.
// All Vulkan handles are passed as void* so this header stays free of Vulkan
// types; rtgl_heathaze.cpp casts them back to VkDevice/VkCommandBuffer/etc.
//
// RTGL_HeatHaze_CustomPass matches PFN_rgDrawFrameCustomPassCallback (in
// RTGL1.h) parameter-for-parameter; it is assigned to
// RgDrawFrameCustomPassParams.pfnCallback.
// ---------------------------------------------------------------------------
void RGAPI_CALL RTGL_HeatHaze_CustomPass(
    void*    pCmd,
    uint32_t frameIndex,
    void*    pColorTargetView,
    void*    pDepthTargetView,
    void*    pCurrentRenderView,
    uint32_t targetWidth,
    uint32_t targetHeight,
    void*    pDevice,
    void*    pPhysicalDevice,
    void*    pColorImage,
    uint32_t colorFormat,
    void*    pUserData );

void RTGL_HeatHaze_Shutdown( void );

#endif // __D3_RTGL_HEATHAZE_H__
