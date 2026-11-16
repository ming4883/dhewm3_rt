#ifndef __D3_RTGL_INTERNAL_H__
#define __D3_RTGL_INTERNAL_H__

#ifdef D3_RTGL

#include "sys/platform.h"
#include "framework/Common.h"
#include "framework/CVarSystem.h"
#include "idlib/Str.h"
#include "idlib/containers/List.h"
#include <string.h>

#ifndef RG_USE_SURFACE_WIN32
#define RG_USE_SURFACE_WIN32
#endif
#include "libs/rtgl/RTGL1.h"

#include "renderer/tr_local.h"
#include "renderer/Image.h"
#include "renderer/Material.h"
#include "renderer/rtgl/rtgl_stats.h"
#include "renderer/rtgl/rtgl_cvars.h"

// ---------------------------------------------------------------------------
// Shared state, defined in rtgl_main.cpp
// ---------------------------------------------------------------------------
extern RgInterface        rg;                  // RTGL1 function table (valid while active)
extern rtglFrameStats_t   g_rtglStats;         // P2.5 per-frame counters
extern int                g_rtglFrameIndex;    // monotonically increasing frame no.
extern int                g_rtglWidth;         // current framebuffer width (from BeginFrame)
extern int                g_rtglHeight;        // current framebuffer height (from BeginFrame)
extern bool               g_rtglCameraUploaded;// set once a camera was uploaded this frame

bool RTGL_FrameActive( void );                 // instance active + rgStartFrame done

// helpers (rtgl_main.cpp)
RgFloat3D          RTGL_V3( float x, float y, float z );
RgExtent2D         RTGL_Ext( uint32_t w, uint32_t h );
const char *       RTGL_ResultString( RgResult r );
RgNormalPacked32   RTGL_PackNormal( float x, float y, float z );
RgColor4DPacked32  RTGL_PackColorF( float r, float g, float b, float a );
RgColor4DPacked32  RTGL_PackColorB( uint8_t r, uint8_t g, uint8_t b, uint8_t a );
RgColor4DPacked32  RTGL_White( void );
void               RTGL_IdentityTransform( RgTransform *t );
// Doom column-major modelMatrix[16] -> RTGL1 row-major 3x4. Everything stays
// in raw Doom units (no metric scaling); worldScale only affects P4 light
// intensity semantics, NOT runtime vertex scaling.
void               RTGL_ModelMatrixToTransform( const float modelMatrix[16], RgTransform *t );

// ---------------------------------------------------------------------------
// P2 geometry / camera (rtgl_geometry.cpp)
// ---------------------------------------------------------------------------
void               RTGL_SubmitBeginFrame( void );   // reset scratch / per-frame registries
void               RTGL_SubmitCommands( const void *cmdHead ); // walk RC_DRAW_VIEW, upload camera + geometry
void               RTGL_ReportStats( void );  // throttled per-frame stats (call AFTER SubmitLights)
RgColor4DPacked32  RTGL_DebugColor( int mode, uint64_t objectID, int primIndex,
                                    int entityIndex, const char *matName, bool isStatic );

// ---------------------------------------------------------------------------
// P3 materials / textures (rtgl_material.cpp)
// ---------------------------------------------------------------------------
void         RTGL_MaterialBeginFrame( void );
void         RTGL_MaterialShutdown( void );
const char * RTGL_PrimaryTexture( const idMaterial *mat, const float *shaderRegisters = NULL );  // stable name, lazily provided
const char * RTGL_GuiTexture( const idMaterial *mat );      // P-UI: GUI/2D texture name, or NULL (= RTGL1 white)
const char * RTGL_GuiStageTexture( const shaderStage_t *stage, int bakeMode );
const char * RTGL_LightTexture( const idImage *img );
const char * RTGL_ProvideSkyFaceTexture( const char *cubeBaseName, cubeFiles_t cubeType, int side );
bool         RTGL_IsSkySurface( const idMaterial *mat );
bool         RTGL_IsDecal( const idMaterial *mat );  // multiply-blend decal detection (by blend bits, not sort)
void         RTGL_ClassifyMaterial( const idMaterial *mat,
                                    RgMeshPrimitiveFlags *outFlags, float *outEmissive );
void         RTGL_GetMaterialPBR( const idMaterial *mat,
                                  float *outRoughness, float *outMetallic );

// ---------------------------------------------------------------------------
// P4 lights / sky (rtgl_light.cpp / rtgl_sky.cpp)
// ---------------------------------------------------------------------------
void         RTGL_LightBeginFrame( void );                    // reset per-frame light-id registry
void         RTGL_SubmitLights( const void *cmdHead );       // walk viewLights, rgUploadLight
void         RTGL_SubmitUI( const void *cmdHead );           // P-UI: 2D GUI views -> swapchained overlay primitives
void         RTGL_SubmitWorldGuis( const void *cmdHead );    // P-UI: 3D-view SS_GUI quads -> world-space swapchain overlays
void         RTGL_SkyBeginFrame( void );
void         RTGL_ObserveSkySurface( const drawSurf_t *surf, float floatTime );
void         RTGL_SubmitSkyGeometry( void );
void         RTGL_BuildSkyParams( RgDrawFrameSkyParams *sky );
void         RTGL_BuildIlluminationParams( RgDrawFrameIlluminationParams *illum );

#endif // D3_RTGL
#endif // __D3_RTGL_INTERNAL_H__
