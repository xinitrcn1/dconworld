// The whole screen while a warp goes off: the view winds into the warp,
// rings race inwards and it darkens to the far side; past the midpoint the
// destination unwinds out of its own warp.

#version 450
#extension GL_GOOGLE_include_directive : require
#include "warp_common.glsl"

layout(location = 0) in vec2 v_local;
layout(location = 0) out vec4 frag_color;

// render.cpp's warp_screen_uniforms
layout(set = 0, binding = 0, std140) uniform warp_screen_uniforms {
    mat4 u_proj;
    vec2 u_screen;
    vec2 u_center;         // the warp on screen, pixels
    float u_progress;      // 0..1 over the transition
    float u_time;
    float u_variant;
    // WarpPoint effect knobs, each 0..1
    float u_vignette;
    float u_darkness;
    float u_spiral_gap;
    float u_swirl;
    float u_lensing;
    float u_noise;
    float u_twist;
    float u_agitation;
};
layout(set = 0, binding = 1) uniform sampler2D u_scene;

void main() {
    float t = clamp(u_progress, 0.0, 1.0);
    // 0 -> 1 at the midpoint -> 0
    float bell = 1.0 - abs(t - 0.5) * 2.0;
    float winding = t < 0.5 ? 1.0 : -1.0;
    float aspect = u_screen.x / u_screen.y;
    vec2 p = (warp_frag_coord(u_screen) - u_center) / u_screen.y;
    float r = length(p);

    // swirl, strongest at the warp, spiral bands spaced by the gap knob
    float bands = sin(r * mix(12.0, 48.0, u_spiral_gap) - u_time * mix(1.0, 6.0, u_agitation));
    float angle = winding * bell * bell * (u_swirl * 5.0 * exp(-r * 2.5) + u_twist * 0.35 * bands * exp(-r));
    // lensing pulls everything towards the warp
    float scale = 1.0 - u_lensing * 0.7 * bell * exp(-r * 2.0);
    vec2 q = warp_rotate(p, angle) * scale;
    // rings rushing in
    float rings = sin(r * 55.0 + u_time * 10.0 + bell * 20.0) * 0.5 + 0.5;
    q += normalize(p + 1e-5) * rings * 0.006 * bell;
    // agitation noise
    vec2 jitter = vec2(warp_noise(p * 9.0 + u_time * 2.0), warp_noise(p * 9.0 - u_time * 2.0)) - 0.5;
    q += jitter * u_noise * 0.03 * bell;

    vec2 uv = (u_center + q * u_screen.y) / u_screen;
    uv = abs(uv);
    uv = 1.0 - abs(1.0 - uv);
    vec3 col = texture(u_scene, uv).rgb;

    vec3 far = warp_far_side(u_variant, warp_fbm(q * 4.0 + u_time * 0.2));
    // drained to the far side's colours, darkening towards the middle of the warp
    col = mix(col, warp_luma(col) * far * 1.5, warp_step(0.1, 0.8, bell) * 0.8);
    float vignette = warp_step(0.15, 0.9 * aspect, r) * u_vignette;
    float dark = clamp(u_darkness * 0.8 * pow(bell, 1.5) + vignette * bell, 0.0, 0.97);
    col *= 1.0 - dark;
    col += far * rings * bell * 0.08;
    // the crossing itself: the far side fills the screen
    float crossing = warp_step(0.82, 1.0, bell);
    col = mix(col, far * 0.15, crossing);
    frag_color = vec4(col, 1.0);
}
