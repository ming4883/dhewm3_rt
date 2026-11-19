// RsHeatHaze.vert
// App-side (dhewm3) custom-pass vertex shader for deferred heat-haze /
// screen-space refraction quads. Self-contained (no ShaderCommonGLSL.h) so the
// SPIR-V is compiled by dhewm3's own CMake rule into rtgl-runtime/shaders/ and
// loaded at runtime by rtgl_heathaze.cpp.
// Draw: 4 vertices, TRIANGLE_STRIP, no vertex buffer; quad rect via push const.

#version 460

struct HeatHaze_PushConst
{
    vec4  rect;        // x, y, w, h in normalized [0,1] screen coords
    vec4  anim;        // time, ior, waveSpeed, waveStrength
    vec4  quad;        // phase, strength, maskFeather, aspect
};

layout(push_constant) uniform HeatHaze_PushConst_Block
{
    HeatHaze_PushConst pc;
};

layout(location = 0) out vec2 vUV;

void main()
{
    int idx = gl_VertexIndex;
    vec2 corner = vec2( float( idx & 1 ), float( ( idx >> 1 ) & 1 ) );

    vec2 screen = pc.rect.xy + corner * pc.rect.zw;   // top-left origin, y down
    // Vulkan NDC (positive-height viewport): x=-1 left..+1 right,
    // y=-1 TOP..+1 bottom. rect.y is top-left (0 = top of frame), so the
    // correct mapping is ndcY = screen.y*2 - 1. The previous
    // "1.0 - screen.y*2.0" (GL-style, y=+1 at top) mirrored the quad
    // vertically about the screen center and made the haze up-side down.
    vec2 ndc    = vec2( screen.x * 2.0 - 1.0,
                        screen.y * 2.0 - 1.0 );

    gl_Position = vec4( ndc, 0.0, 1.0 );
    vUV = corner;
}
