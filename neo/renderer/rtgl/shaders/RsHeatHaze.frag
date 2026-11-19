// RsHeatHaze.frag
// App-side (dhewm3) screen-space refraction / heat-haze. Samples the copied
// color target with a procedural wave-perturbed UV and blends it over the
// current frame using an edge-feathered mask. The color target is a separate
// copy of the composited frame (see rtgl_heathaze.cpp), so there is no
// read/write feedback loop.

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

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uCurrentRender;

void main()
{
    // Local quad coords in [0,1]; center at 0.5.
    vec2 q = vUV;

    // Build an animated pseudo-normal from layered sine waves. This mimics the
    // tangent-space normal map used by Doom 3's heatHazeWithMask.vfp without
    // needing to bind the original texture here.
    float t       = pc.anim.x * pc.anim.z + pc.quad.x;
    float strength = pc.quad.y * pc.anim.w;
    float w1 = sin( ( q.y * 14.0 + t * 1.7 ) ) * 0.5 + 0.5;
    float w2 = sin( ( q.x * 21.0 - t * 2.3 ) + q.y * 9.0 ) * 0.5 + 0.5;
    vec2  perturb = ( vec2( w1, w2 ) - 0.5 ) * 2.0;

    // Base refraction displacement scaled by (ior - 1); hot-air wobble is
    // subtle (ior ~ 1.06), glass-like distortion larger.
    float refr = max( pc.anim.y - 1.0, 0.0 );
    vec2 disp = perturb * ( 0.004 + refr * 0.02 ) * ( 0.5 + 0.5 * strength );

    // Sample the undisturbed frame at the perturbed screen UV.
    // rect is top-left origin (y down), which matches Vulkan's texture
    // coordinate convention (t=0 = first framebuffer row = top), so the
    // copied scratch image needs no vertical flip here.
    vec2 screenUV = pc.rect.xy + q * pc.rect.zw + disp;
    vec3 refracted = texture( uCurrentRender, clamp( screenUV, 0.0, 1.0 ) ).rgb;

    // Edge feather mask so the quad blends smoothly into the world. Bright
    // interior = active distortion, dark corners/edges = no distortion.
    vec2 d = min( q, 1.0 - q );
    float edge = clamp( min( d.x, d.y ) / max( pc.quad.z, 1e-3 ), 0.0, 1.0 );
    edge = smoothstep( 0.0, 1.0, edge );
    float mask = edge * ( 0.6 + 0.4 * w1 );

    // Blend the refracted sample over the (already drawn) frame. Since the
    // render pass uses LOAD_OP_LOAD on the same attachment and this pipeline is
    // alpha-blended, dst is preserved where mask is low.
    outColor = vec4( refracted, mask );
}
