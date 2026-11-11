#include "sys/platform.h"
#include "framework/Common.h"

#ifdef D3_RTGL

#include "renderer/rtgl/rtgl_internal.h"

// ===========================================================================
// P4 - Sky and illumination params
//
// Build the two pNext-linked RgDrawFrame* structs that RTGL1 consumes in
// rgDrawFrame. RTGL1's sky is a ray-miss procedural sample, NOT a light
// source: skyColorMultiplier controls the visible backdrop, while
// skyLightMultiplier / skyLightScale drive indirect-bounce / ambient fill.
//
// CUBEMAP sky (Doom 3 skybox texgen) is dead code in this RTGL1 fork and
// needs a real cubemap asset; for P4 we use RG_SKY_TYPE_COLOR with a tunable
// backdrop + ambient fill so that GI/RT shadows actually have light to work
// with. Wiring a real skybox cubemap is deferred to the sky-asset iteration.
//
// The caller owns the storage; these structs are linked into the pNext chain:
//   RgDrawFrameInfo -> RgDrawFrameSkyParams -> RgDrawFrameIlluminationParams
// (order is arbitrary to RTGL1, it scans the chain by sType).
// ===========================================================================

void RTGL_BuildSkyParams( RgDrawFrameSkyParams *sky ) {
    memset( sky, 0, sizeof( *sky ) );
    sky->sType = RG_STRUCTURE_TYPE_DRAW_FRAME_SKY_PARAMS;
    sky->pNext = NULL;
    sky->skyType = RG_SKY_TYPE_COLOR;
    sky->skyColorDefault = RTGL_V3( rt_sky_colorR.GetFloat(),
                                     rt_sky_colorG.GetFloat(),
                                     rt_sky_colorB.GetFloat() );
    sky->skyColorMultiplier = rt_sky_brightness.GetFloat();
    sky->skyColorSaturation = 1.0f;
    sky->skyLightMultiplier = rt_sky_light.GetFloat();
    sky->skyLightSaturation = 1.0f;
    sky->skyColorScale = 1.0f;
    sky->skyLightScale = rt_sky_lightScale.GetFloat();
    sky->skyViewerPosition = RTGL_V3( 0.0f, 0.0f, 0.0f );
    sky->pSkyCubemapTextureName = NULL;
    // skyCubemapRotationTransform zero => RTGL1 uses identity (per header).
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
