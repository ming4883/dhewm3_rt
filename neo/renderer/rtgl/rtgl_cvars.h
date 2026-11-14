#ifndef __D3_RTGL_CVARS_H__
#define __D3_RTGL_CVARS_H__

// All RTGL cvars are declared here, defined in rtgl_main.cpp.
// NONE carry CVAR_ARCHIVE: rt_* / r_rtgl_* values must never be persisted
// into the shared DoomConfig.cfg / dhewm.cfg (cross-backend pollution).
// They are CVAR_RENDERER only; pin them via +exec rtgl-debug.cfg if needed.

extern idCVar r_rtgl_enable;        // 0 = stock OpenGL/ARB2, 1 = RTGL1
extern idCVar r_rtgl_overridePath;  // runtime folder (RTGL1.dll, shaders, RTGL1.json)

// P2.5 debug / visualization (non-ARCHIVE on purpose)
extern idCVar rt_dbg_colorMode;     // 0 normal, 1 objectID hash, 2 static/dyn, 3 primIdx, 4 entity, 5 material hash
extern idCVar rt_dbg_validateIDs;   // report duplicate uniqueObjectID per frame
extern idCVar rt_dbg_stats;         // throttled per-frame submission stats
extern idCVar rt_dbg_skipSubViews;  // skip mirror/camera/xray subviews
extern idCVar rt_dbg_maxSurfs;      // per-frame surface budget (0 = unlimited)
extern idCVar rt_dbg_bounds;        // reserved: rasterized AABB/normal overlay (P5)
extern idCVar rt_dbg_dumpLights;  // per-light list every N frames (0 = off); view with win_viewlog 1
extern idCVar rt_dbg_printRepeat; // suppress repeated RTGL1 log messages (print 1st, then every N; 0 = off)
extern idCVar rt_dbg_lightViz; // 0 off, 1 solid sphere, 2 wireframe (A: in-scene light gizmos at true radius)
extern idCVar rt_decal_mode;         // multiply-blend decals: 0 legacy, 1 baked-alpha cutout, 2 skip, 3 DECAL overlay
extern idCVar rt_emissive_enable;      // 1 = enable mesh light emission from blend-add surfaces (default 1, 2026-11-14 restored)
extern idCVar rt_emissive_mult;        // multiplier on surface emission when enabled (default 0.1)
extern idCVar rt_normalmap_enable;   // 1 = bind Doom3 bump stages as in-memory normal maps (Plan B, default 1)
extern idCVar rt_ui_enable;          // 1 = compose in-game 2D GUI (console/menu/HUD) as RTGL1 swapchain overlays (default 1)
extern idCVar rt_ui_flipY;           // 1 = vertically flip the 2D overlay (if console/HUD renders upside down)

// P4 lights / sky / illumination (non-ARCHIVE on purpose)
extern idCVar rt_light_enable;          // master switch for viewLight -> RgLight upload
extern idCVar rt_light_intensity;       // global scale on imported (sphere/spot) light intensity
extern idCVar rt_light_dirIntensity; // dedicated illuminance (lux) for parallel/directional lights
extern idCVar rt_light_skipParallel; // default 0: import bounded p.parallel lights (2026-11-14 restored); 1 = skip (avoids global-sun view pop)
extern idCVar rt_light_radiusScale;     // scale on spherical/spot light radius
extern idCVar rt_light_sizeExponent;    // auto intensity *= (extent/refRadius)^sizeExponent; 0 = flat legacy (default 2)
extern idCVar rt_light_refRadius;       // extent normalization for sizeExponent (default 256)
extern idCVar rt_light_maxLights;       // budget cap (< 4096), 0 = unlimited
extern idCVar rt_light_spotAngle;       // outer cone angle (deg) for projected/cookie lights
extern idCVar rt_light_skipFog;         // skip fog lights (no direct RgLight equivalent)
extern idCVar rt_light_skipBlend;       // skip blend lights (subtractive, no RTGL1 equivalent)
extern idCVar rt_light_skipNoShadows;   // skip noShadow lights (or keep as unshadowed = all RT lights are shadowed)
extern idCVar rt_light_skipNoInteractions; // 1 = skip lights with no visible interactions (local/global/translucentInteractions)
extern idCVar rt_light_maxRadius;          // skip lights with raw radius > maxRadius (0 = unlimited, default 2000)
extern idCVar rt_light_cullFrustumBounds;  // 1 = skip point lights whose volume bottom is far above camera view (default 1)
extern idCVar rt_sun_enable;            // inject one directional "sun" (debug / ambient fill)
extern idCVar rt_sun_intensity;         // sun illuminance (lux-like)
extern idCVar rt_sun_dirX;              // sun direction (from sun toward scene)
extern idCVar rt_sun_dirY;
extern idCVar rt_sun_dirZ;
extern idCVar rt_sky_colorR;            // sky backdrop + sky-light color (linear 0..1)
extern idCVar rt_sky_colorG;
extern idCVar rt_sky_colorB;
extern idCVar rt_sky_brightness;        // skyColorMultiplier (visible backdrop)
extern idCVar rt_sky_light;             // skyLightMultiplier (indirect/ambient fill)
extern idCVar rt_sky_lightScale;        // live skyLightScale
extern idCVar rt_bounce_enableSecond;   // enableSecondBounceForIndirect
extern idCVar rt_bounce_maxShadows;     // maxBounceShadows (shadow rays)
extern idCVar rt_gi_sensitivity;        // directDiffuseSensitivityToChange

#endif // __D3_RTGL_CVARS_H__
