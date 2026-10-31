// The Watcher's Sandstorm: vanilla's Sandstorm shader. Three layers of
// blowing sand scrolled by the wind (a light grainy top layer, darker slower
// clouds, puffs over the open sky), the level and the sprites turned to
// silhouettes as it thickens; where the room is open to the wind (the
// surface mask, binding 3) the SurfaceSandstorm effect blows even before the
// storm. Premultiplied alpha (Blend One OneMinusSrcAlpha), over the level
// like vanilla's Water container. vanilla's noise and clouds textures are
// hashed value noise. Drawn once per camera image under the view, a pixel
// only from the camera image the level shows there.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "level_common.glsl"

layout(location = 0) in vec2 v_room;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec4 v_color;
layout(location = 0) out vec4 o_color;

// render.cpp's sandstorm_uniforms
layout(set = 0, binding = 0, std140) uniform sandstorm_uniforms {
    mat4 u_proj;
    vec2 u_screen;
    vec2 u_view_pos;
    vec2 u_cam_origin;
    vec2 u_scroll;       // _sandstormParams.xy: the wind's scroll
    vec4 u_dust;         // _sandstormColor
    vec2 u_mask_size;    // the surface mask, tiles
    float u_zoom;
    float u_night_time;
    float u_rain;        // _RAIN
    float u_intensity;   // GlobalIntensity (the sprite's alpha)
    float u_surface;     // SurfaceIntensity (_sandstormParams.z)
    int u_index;
    int u_camera_index;
    int u_later_count;
    float u_pad0, u_pad1;
    vec4 u_later[16];
};
layout(set = 0, binding = 1) uniform sampler2DArray u_level;
layout(set = 0, binding = 2) uniform sampler2DArray u_palette;
layout(set = 0, binding = 3) uniform sampler2D u_surface_mask;
layout(set = 0, binding = 4) uniform sampler2D u_mask;

bool shown_by_later_camera(vec2 room) {
    for (int i = 0; i < u_later_count; ++i) {
        vec4 o4 = u_later[i / 2];
        vec2 o = (i % 2) == 0 ? o4.xy : o4.zw;
        vec2 d = room - o;
        if (d.x >= 0.0 && d.y >= 0.0 && d.x < level_size.x && d.y < level_size.y)
            return true;
    }
    return false;
}

// tex2D(_NoiseTex, uv) and tex2D(_CloudsTex, uv), both repeating over 1 unit
float noise_tex(vec2 uv) {
    return level_noise(uv, 9.0) * 0.6 + level_noise(uv + vec2(3.7, 1.9), 23.0) * 0.4;
}
float clouds_tex(vec2 uv) {
    float n = level_noise(uv + vec2(11.3, 7.1), 4.0) * 0.55 + level_noise(uv + vec2(2.9, 5.3), 11.0) * 0.3 + level_noise(uv, 27.0) * 0.15;
    return clamp((n - 0.35) * 2.2, 0.0, 1.0);
}
float sample_noise(vec2 uv) {
    return noise_tex(uv * 0.4) * noise_tex(uv * 1.5) * 1.5;
}

void main() {
    vec2 world = v_room;
    vec2 tc = (floor(world) + 0.5 - u_cam_origin) / level_size;
    if (tc.x < 0.0 || tc.y < 0.0 || tc.x > 1.0 || tc.y > 1.0)
        discard;
    if (shown_by_later_camera(world))
        discard;
    ivec3 t = ivec3(texture(u_level, vec3(tc.x, 1.0 - tc.y, float(u_camera_index))).rgb * 255.0 + 0.5);
    float level_depth = t == ivec3(255) ? 1.0 : float(max(t.r - 1, 0) % 30) / 30.0;
    vec2 frame_uv = (world - u_view_pos) * u_zoom / u_screen;
    if (level_depth > 6.0 / 30.0 && level_sprite_at(u_mask, frame_uv))
        level_depth = 6.0 / 30.0;

    vec4 dust = u_dust;
    vec4 black = vec4(mix(level_pal(u_palette, u_index, 0, 0, false), level_pal(u_palette, u_index, 0, 0, true), u_night_time), 1.0);

    float distortion = sample_noise(vec2(world.x / 10000.0, u_rain * 0.5)) * 0.5;
    distortion += sample_noise(vec2(world.y / 8000.0 + 0.5, u_rain * 0.35)) * 0.25;
    vec2 scroll = u_scroll;
    float intensity = u_intensity;

    // the surface sandstorm, where the room is open to the wind
    if (u_surface > 0.0) {
        float mask = texture(u_surface_mask, world / 20.0 / u_mask_size).a;
        mask = max(mask, texture(u_surface_mask, (world + vec2(0.0, distortion * 200.0 - 50.0)) / 20.0 / u_mask_size).a);
        mask = pow(min(mask * 1.5, 1.0), 0.8);
        mask *= mix(0.2, 1.0, min(level_depth * 2.0, 1.0));
        intensity = mix(intensity, 1.0, u_surface * mask);
    }
    distortion = mix(0.5, distortion, intensity);

    // top layer: light soft noise with a bit of grain
    vec2 a_coord = world + vec2(0.0, distortion * 500.0) - scroll;
    float layer_a = sample_noise(a_coord / 256.0 * vec2(0.05, 0.3));
    vec2 grain = vec2(level_hash(floor(a_coord)), level_hash(floor(a_coord) + vec2(7919.0, 104729.0)));
    layer_a *= mix(grain.x, 1.0, layer_a * 0.9);

    // bottom layer: darker clouds that move slower
    vec2 b_coord = world + vec2(0.0, distortion * 400.0 + 100.0) - scroll * 0.7;
    float layer_b = clouds_tex(b_coord / 256.0 * vec2(0.05, 0.3));
    b_coord = world + vec2(0.0, distortion * 50.0) - scroll * 0.5;
    layer_b = max(layer_b, clouds_tex(b_coord / 256.0 * vec2(0.05, 0.3) + 0.5));

    // background layer: puffs of sand in the sky
    vec2 c_coord = world - scroll * 0.2;
    float layer_c = clouds_tex(c_coord / 256.0 * vec2(0.2, 0.6));
    c_coord = world - scroll * 0.18;
    layer_c = max(layer_c, clouds_tex(c_coord / 256.0 * vec2(0.15, 0.3) + 0.5));
    layer_c *= clamp(noise_tex(c_coord / 256.0 * vec2(0.06, 0.15)) * 4.0 - 1.0, 0.0, 1.0);
    layer_c *= level_depth >= 1.0 ? 1.0 : 0.0;

    layer_a = (pow(layer_a, 0.8) * 0.25 + 0.75) * intensity * intensity;
    layer_b = sqrt(max(layer_b - max(1.0 - 2.0 * intensity, 0.0), 0.0));
    layer_b *= intensity;
    layer_c *= min(max(u_intensity, u_surface) * 3.0, 1.0);

    // the screen's height, from the bottom (the sprite's uv)
    float screen_y = clamp(frame_uv.y, 0.0, 1.0);
    vec4 col = mix(dust, black, 0.5 * (1.0 - screen_y) + grain.y * 0.02) * layer_a;
    col += mix(dust, black, 0.6) * layer_b * (1.0 - col.a);
    col += black * layer_c * 0.1 * (1.0 - col.a);
    // the terrain and the objects turned to silhouettes
    col += mix(dust, black, pow(clamp(1.0 - level_depth, 0.0, 1.0), 1.25)) * 0.5 * intensity * intensity * (1.0 - col.a);
    if (col.a <= 0.0)
        discard;
    o_color = col;
}
