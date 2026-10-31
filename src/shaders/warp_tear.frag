// A warp point's tear: the scene behind it bends around a dark core, a
// wave rolls outwards every couple of seconds, and veins of the far side
// crack through further the wider the tear is open.

#version 450
#extension GL_GOOGLE_include_directive : require
#include "warp_common.glsl"

layout(location = 0) in vec2 v_local;
layout(location = 0) out vec4 frag_color;

// render.cpp's warp_tear_uniforms
layout(set = 0, binding = 0, std140) uniform warp_tear_uniforms {
    mat4 u_proj;
    vec2 u_screen;           // framebuffer size in pixels
    float u_radius_px;       // the quad's radius on screen
    float u_open;            // WarpTear.openAnimation (0..1)
    float u_fade;            // WarpTear.fadeAnim (0..1)
    float u_time;            // seconds
    float u_seed;
    float u_variant;         // see warp_far_side
    float u_ghost;           // a warp of the other layer: faint, no core
};
layout(set = 0, binding = 1) uniform sampler2D u_scene;   // a copy of the frame drawn so far

void main() {
    float d = length(v_local);
    float fade = warp_step(0.0, 1.0, u_fade);
    if (d >= 1.0 || fade <= 0.0)
        discard;
    vec2 dir = d > 0.0001 ? v_local / d : vec2(0.0, 1.0);
    float open = warp_step(0.0, 1.0, u_open) * fade;
    float t = u_time;

    // the distortion wave, rolling out from the middle
    float cycle = fract(t / 2.1 + u_seed);
    float wave_r = cycle * 0.75;
    float wave = exp(-pow((d - wave_r) / 0.035, 2.0)) * (1.0 - cycle) * (0.35 + 0.65 * open);

    // noise in polar space, slowly turning
    vec2 polar = vec2(atan(v_local.y, v_local.x) * 1.6, d * 7.0);
    float churn = warp_fbm(v_local * 5.0 + vec2(t * 0.07, -t * 0.05) + u_seed * 13.0);
    float veins = warp_ridges(polar + vec2(u_seed * 7.0, -t * 0.12));
    veins = pow(veins, 10.0 - 4.0 * open);
    float reach = mix(0.04, 0.32, open) * (0.8 + 0.4 * churn);
    float vein_mask = veins * warp_step(reach, reach * 0.3, d) * (1.0 - u_ghost);

    // the core: a hole into the far side, its edge wobbling
    float core_r = 0.11 * open * (0.85 + 0.3 * churn);
    float core = core_r > 0.001 ? warp_step(core_r, core_r * 0.55, d) * (1.0 - u_ghost) : 0.0;
    float rim = exp(-pow((d - core_r) / 0.012, 2.0)) * open * (1.0 - u_ghost);

    // bend the scene: pulled towards the core, pushed by the wave, swirled near the middle
    float lens = warp_step(0.45, 0.0, d) * open;
    float swirl = lens * lens * (1.2 + 0.4 * sin(t * 0.9 + u_seed));
    vec2 bent = warp_rotate(v_local, swirl) * (1.0 - 0.18 * lens);
    bent += dir * (wave * 0.025 + vein_mask * 0.012);
    vec2 offset_px = (bent - v_local) * u_radius_px;
    vec2 uv = (warp_frag_coord(u_screen) + offset_px) / u_screen;
    uv = abs(uv);
    uv = 1.0 - abs(1.0 - uv);
    vec3 scene = texture(u_scene, uv).rgb;

    vec3 far = warp_far_side(u_variant, churn);
    vec3 far_lit = far * (0.6 + 0.8 * warp_noise(v_local * 40.0 + t * 0.3));
    vec3 col = scene;
    // around the tear the world loses its colour to the far side's
    float dim = warp_step(0.3, 0.02, d) * (0.08 + 0.4 * open);
    col = mix(col, warp_luma(scene) * far * 1.6, dim);
    col += wave * far * 0.6;
    col = mix(col, max(far_lit, warp_luma(scene) * far), vein_mask);
    col = mix(col, far * 0.25, core);
    col += rim * mix(vec3(0.6, 0.8, 1.0), far * 2.0, 0.4);

    float alpha = warp_step(1.0, 0.7, d) * fade;
    if (u_ghost > 0.5)
        alpha *= 0.45;
    frag_color = vec4(col, alpha);
}
