#ifndef __D3_RTGL_STATS_H__
#define __D3_RTGL_STATS_H__

// Per-frame RTGL1 submission counters. Reset in RTGL_BeginFrame(),
// accumulated by the P2 geometry walker and P4 light walker,
// reported by rt_dbg_stats.
struct rtglFrameStats_t {
    int frameIndex;
    int meshes;              // RgMeshInfo submissions
    int primitives;          // RgMeshPrimitiveInfo submissions (drawSurfs)
    int vertices;            // RgPrimitiveVertex uploaded
    int indices;             // indices uploaded
    int staticPrimitives;    // world / static surfaces
    int dynamicPrimitives;   // entity / dynamic surfaces
    int skippedSurfs;        // drawSurfs intentionally not submitted
    int skippedNoGeo;        // geo / material == NULL
    int skippedNoVerts;      // numVerts / numIndexes <= 0
    int drawSurfsSeen;       // total drawSurfs across all RC_DRAW_VIEW commands
    int texturesProvided;    // P3: rgProvideOriginalTexture calls (unique, this session)
    int idCollisions;        // P2.5: duplicate uniqueObjectID seen this frame
    int uiSurfs;             // P-UI: GUI drawSurfs seen in 2D views
    int uiPrims;             // P-UI: swapchained overlay primitives uploaded
    int uiVerts;             // P-UI: overlay vertices uploaded
    int worldGuiSurfs;       // P-UI: in-world (3D-view) GUI drawSurfs seen
    int worldGuiPrims;       // P-UI: in-world GUI swapchained primitives uploaded
    int worldGuiVerts;       // P-UI: in-world GUI vertices uploaded

    // P4
    int lightsSeen;          // viewLights on all RC_DRAW_VIEW views
    int lightsUploaded;      // rgUploadLight calls that succeeded
    int sphereLights;        // RgLightSphericalEXT
    int spotLights;          // RgLightSpotEXT
    int directionalLights;   // RgLightDirectionalEXT (incl. rt_sun)
    int skippedLights;       // fog/blend/noShadow/budget/duplicate/failed
    int skippedFog;          // fog lights
    int skippedBlend;        // blend lights
    int skippedNoShadow;     // noShadow lights (kept by default; counted if skipped)
    int lightDuplicates;     // same uniqueID seen twice this frame
    int skippedDir;         // extra directional lights skipped (RTGL1 allows only one)
    int failedLightUploads;  // rgUploadLight returned an error

    void Clear( int frame ) {
        frameIndex = frame;
        meshes = 0;
        primitives = 0;
        vertices = 0;
        indices = 0;
        staticPrimitives = 0;
        dynamicPrimitives = 0;
        skippedSurfs = 0;
        skippedNoGeo = 0;
        skippedNoVerts = 0;
        drawSurfsSeen = 0;
        texturesProvided = 0;
        idCollisions = 0;
        uiSurfs = 0;
        uiPrims = 0;
        uiVerts = 0;
        worldGuiSurfs = 0;
        worldGuiPrims = 0;
        worldGuiVerts = 0;
        lightsSeen = 0;
        lightsUploaded = 0;
        sphereLights = 0;
        spotLights = 0;
        directionalLights = 0;
        skippedLights = 0;
        skippedFog = 0;
        skippedBlend = 0;
        skippedNoShadow = 0;
        lightDuplicates = 0;
        skippedDir = 0;
        failedLightUploads = 0;
    }
};

#endif // __D3_RTGL_STATS_H__
