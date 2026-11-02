// The body of the water, below its front edge: the scene behind is bent by
// a slowly churning current, lit by drifting caustics, and loses the light
// the deeper it is, sinking into the palette's deep water colour.
//
// Everything is built from hashed value noise, domain warped and drifting,
// so the pattern never visibly repeats; the shimmer and caustics weaken with
// depth but never fade out, down to the floor of the deepest pools.
//
// The caustics follow the level behind the water, not the weather: where the
// camera image is shaded (its red channel's sun bit off) they sink by as much
// as the palette darkens that pixel (its shade row against its lit row), so
// the room's shadows obscure them. Neither the sun nor the coming rain
// (`_light`) reaches them: the water looks the same through the cycle.

#version 450
#extension GL_GOOGLE_include_directive : require
#include "warp_common.glsl"
#include "level_common.glsl"

layout(location = 0) in vec2 v_local;          // x: room x, y: depth below the surface (pixels)
layout(location = 0) out vec4 frag_color;

// render.cpp's water_uniforms
layout(set = 0, binding = 0, std140) uniform water_uniforms {
    mat4 u_proj;
    vec3 u_shallow;   // RoomPalette.waterColor1
    float u_time;     // seconds
    vec3 u_deep;      // RoomPalette.waterColor2
    float u_zoom;
    vec3 u_shine;     // RoomPalette.waterShineColor
    float u_lit;      // 1 - the palette's darkness: the caustics' strength
    vec2 u_screen;
    vec2 u_view_pos;  // the view's bottom left, room pixels
    int u_index;      // palette layer
    int u_cam_count;  // camera images (0: no level, the caustics everywhere)
    float u_night_time;
    float u_pad;
    vec4 u_cams[16];  // their bottom lefts, two per vec4
};
layout(set = 0, binding = 1) uniform sampler2D u_scene;
layout(set = 0, binding = 2) uniform sampler2DArray u_level;   // the room's camera images
layout(set = 0, binding = 3) uniform sampler2DArray u_palette;

// integer hash (no visible lattice, unlike the sin() hashes)
float hash13(vec3 p) {
    uvec3 q = uvec3(ivec3(floor(p))) * uvec3(1597334673u, 3812015801u, 2798796415u);
    uint n = (q.x ^ q.y ^ q.z) * 1597334673u;
    n ^= n >> 15;
    n *= 2246822519u;
    n ^= n >> 13;
    return float(n) * (1.0 / 4294967295.0);
}

// value noise over (x, y, time): the time axis makes it evolve instead of scrolling
float vnoise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    vec3 u = f * f * (3.0 - 2.0 * f);
    float n000 = hash13(i);
    float n100 = hash13(i + vec3(1.0, 0.0, 0.0));
    float n010 = hash13(i + vec3(0.0, 1.0, 0.0));
    float n110 = hash13(i + vec3(1.0, 1.0, 0.0));
    float n001 = hash13(i + vec3(0.0, 0.0, 1.0));
    float n101 = hash13(i + vec3(1.0, 0.0, 1.0));
    float n011 = hash13(i + vec3(0.0, 1.0, 1.0));
    float n111 = hash13(i + vec3(1.0, 1.0, 1.0));
    return mix(mix(mix(n000, n100, u.x), mix(n010, n110, u.x), u.y),
               mix(mix(n001, n101, u.x), mix(n011, n111, u.x), u.y), u.z);
}

float fbm(vec3 p) {
    float v = 0.0;
    float a = 0.5;
    // rotate each octave so their grids never line up
    mat2 rot = mat2(0.8, 0.6, -0.6, 0.8);
    for (int i = 0; i < 4; ++i) {
        v += a * vnoise(p);
        p.xy = rot * p.xy * 2.03 + vec2(11.7, 3.1);
        p.z *= 1.37;
        a *= 0.5;
    }
    return v;
}

float luma(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

// how much of the caustics the level behind lets through: 1 where it is
// sunlit (or open sky), else its shaded colour's brightness against its lit
// one (the palette's shade rows), out of the camera image the level shows
// there (the last one covering it)
float level_light(vec2 room) {
    int cam = -1;
    vec2 origin = vec2(0.0);
    for (int i = 0; i < u_cam_count; ++i) {
        vec4 o4 = u_cams[i / 2];
        vec2 o = (i % 2) == 0 ? o4.xy : o4.zw;
        vec2 d = room - o;
        if (d.x >= 0.0 && d.y >= 0.0 && d.x < level_size.x && d.y < level_size.y) {
            cam = i;
            origin = o;
        }
    }
    if (cam < 0)
        return 1.0;
    vec2 tc = (floor(room - origin) + 0.5) / level_size;
    ivec3 t = ivec3(texture(u_level, vec3(tc.x, 1.0 - tc.y, float(cam))).rgb * 255.0 + 0.5);
    level_px p = level_decode(t);
    if (p.sky || p.sunlit)
        return 1.0;
    int column = p.floor_dark ? p.depth : 0;
    vec3 lit = mix(level_pal(u_palette, u_index, column, p.palette_color + 3, false),
                   level_pal(u_palette, u_index, column, p.palette_color + 3, true), u_night_time);
    vec3 shade = mix(level_pal(u_palette, u_index, column, p.palette_color, false),
                     level_pal(u_palette, u_index, column, p.palette_color, true), u_night_time);
    float k = clamp(luma(shade) / max(luma(lit), 1e-3), 0.0, 1.0);
    // (a shade as bright as the light still reads as shade)
    return k * 0.8;
}

void main() {
    float depth = max(v_local.y, 0.0);
    vec2 world = vec2(v_local.x, -depth);
    float t = u_time;

    // a slow current warping the noise domain
    vec2 q = world * 0.012;
    vec2 warp = vec2(fbm(vec3(q, t * 0.11)), fbm(vec3(q + vec2(5.2, 1.3), t * 0.13))) - 0.5;
    vec2 p = q + warp * 1.6;

    // refraction: strong under the surface, settling to a gentle shimmer at depth
    float near = exp(-depth / 160.0);
    float strength = mix(0.35, 1.0, near);
    vec2 bend = vec2(fbm(vec3(p * 2.0, t * 0.35)), fbm(vec3(p * 2.0 + vec2(9.1, 4.7), t * 0.33))) - 0.5;
    vec2 offset = bend * 5.0 * strength * u_zoom;
    vec2 uv = clamp((warp_frag_coord(u_screen) + offset) / u_screen, vec2(0.001), vec2(0.999));
    vec3 scene = texture(u_scene, uv).rgb;

    // less light the deeper: the sun only reaches so far
    float murk = 1.0 - exp(-depth / 650.0);
    float light = mix(0.8, 0.52, murk);
    vec3 tint = mix(u_shallow, u_deep, murk);
    vec3 col = scene * light * mix(vec3(1.0), tint * 1.5, 0.45);
    col = mix(col, tint * light, 0.15 + 0.3 * murk);

    // caustics: bright creases of warped noise, dimmer but present all the way down
    float c = fbm(vec3(p * 4.5, t * 0.5));
    float crease = 1.0 - abs(c * 2.0 - 1.0);
    float caustic = pow(crease, 11.0) * mix(0.45, 1.0, near);
    vec2 room = u_view_pos + warp_frag_coord(u_screen) / u_zoom;
    col += u_shine * caustic * 0.2 * light * u_lit * level_light(room);

    float alpha = mix(0.62, 0.86, murk);
    frag_color = vec4(col, alpha);
}
