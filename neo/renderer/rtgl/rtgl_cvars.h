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
extern idCVar rt_dbg_maxSurfs;
extern idCVar rt_dbg_dumpSurfs;
extern idCVar rt_dbg_dumpMatFilter;      // per-frame surface budget (0 = unlimited)
extern idCVar rt_dbg_bounds;        // reserved: rasterized AABB/normal overlay (P5)
extern idCVar rt_dbg_dumpLights;  // per-light list every N frames (0 = off); view with win_viewlog 1
extern idCVar rt_dbg_printRepeat; // suppress repeated RTGL1 log messages (print 1st, then every N; 0 = off)
extern idCVar rt_dbg_lightViz; // 0 off, 1 solid sphere, 2 wireframe (A: in-scene light gizmos at true radius)
extern idCVar rt_refr_enable;
extern idCVar rt_refr_index;
extern idCVar rt_refr_depth;
extern idCVar rt_refr_waveSpeed;
extern idCVar rt_refr_waveStrength;
extern idCVar rt_raster_distortion;   // 1 = defer heat haze and air distortion for custom raster pass
extern idCVar rt_dbg_disableRaster;   // DIAG: set RgDrawFrameInfo.disableRasterization (kill RTGL1 rasterizer)
extern idCVar rt_dbg_resetHistory;    // DIAG: set RgDrawFrameInfo.resetHistory (drop temporal accumulation)
extern idCVar rt_dbg_disableRTGeom;   // DIAG: set RgDrawFrameInfo.disableRayTracedGeometry (kill BLAS/RT geometry)
extern idCVar rt_dbg_forceSkyColor;   // DIAG: force RG_SKY_TYPE_COLOR + magenta skyColorDefault (verify grey board == ray-miss background)
extern idCVar rt_decal_mode;         // multiply-blend decals: 0 legacy, 1 baked-alpha cutout, 2 skip, 3 DECAL overlay
extern idCVar rt_emissive_enable;      // 1 = enable mesh light emission from blend-add surfaces (default 1, 2026-11-14 restored)
extern idCVar rt_emissive_mult;        // multiplier on surface emission when enabled (default 0.1)
extern idCVar rt_normalmap_enable;   // 1 = bind Doom3 bump stages as in-memory normal maps (Plan B, default 1)
extern idCVar rt_ui_enable;          // 1 = compose in-game 2D GUI (console/menu/HUD) as RTGL1 swapchain overlays (default 1)
extern idCVar rt_ui_flipY;           // 1 = vertically flip the 2D overlay (if console/HUD renders upside down)
extern idCVar rt_ui_worldGuis;       // 1 = draw in-world interactive GUI surfaces (terminals/keypads/screens)
extern idCVar rt_ui_worldGuisFlipY;  // 1 = vertically flip in-world GUI overlay (safety net for Y convention)
extern idCVar rt_ui_worldGuisMode;   // 1 = WORLD raster (depth-tested vs scene); 0 = old swapchain (no depth)
extern idCVar rt_ui_worldGuisDepthBias; // pull GUI slightly toward camera to win depth test on coplanar wall
extern idCVar rt_eye_worldRaster;    // 1 = route eyeball/iris materials through WORLD raster (alpha-tested, depth-tested) instead of ray-traced BLAS
extern idCVar rt_eye_depthPush;      // Doom-unit camera-ward push for coplanar iris quads (rt_eye_worldRaster only)
extern idCVar rt_flare_opacity;      // opacity scale (0..1) for light flare / glare additive surfaces (default 0.15)
extern idCVar rt_pbr_enable;         // 1 = assign per-material PBR roughness/metallic from pbr_materials.cfg + heuristics
extern idCVar rt_pbr_roughnessScale; // global multiplier on per-material PBR roughness (default 1.0)
extern idCVar rt_pbr_metallicScale;  // global multiplier on per-material PBR metallic (default 1.0)

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
extern idCVar rt_light_skipSkyLights;   // skip fake ambient sky point lights (lights/*sky*, e.g. light_4874 outside kitchen)
extern idCVar rt_light_skipNoInteractions; // 0 = keep lights without visible surface interactions (avoids view-edge popping)
extern idCVar rt_light_maxRadius;          // skip lights with raw radius > maxRadius (default 3500, culls sky domes >4800)
extern idCVar rt_light_cullFrustumBounds;  // 0 = off, let GPU OBB clipping handle light volume bounds (default 0)
extern idCVar rt_light_useBoundsAndCookie; // 1 = pass Doom 3 OBB/frustum lightProject + cookie/falloff textures to RTGL1
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
