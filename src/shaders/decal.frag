// CustomDecal: vanilla's Decal shader. The decal paints the level pixels
// whose depth is within its range, blended into the terrain's colour (a mix,
// a screen in the sun and a multiply); in the shade it is much fainter.
// Sprites cast their sun shadows on it and hide it on depths 5+ (the sprite
// mask, binding 4). Drawn once per room camera image it overlaps, after the
// sprites; a pixel is painted only from the camera image the level shows
// there (the last one covering it), or overlapping cameras would paint it
// over and over.

#version 450
#extension GL_GOOGLE_include_directive : require
#include "level_common.glsl"

layout(location = 0) in vec2 v_room;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec4 v_color; // r: the smudge, a: the opacity
layout(location = 0) out vec4 o_color;

// render.cpp's decal_uniforms
layout(set = 0, binding = 0, std140) uniform decal_uniforms {
    mat4 u_proj;
    vec2 u_screen;
    vec2 u_view_pos;
    vec2 u_cam_origin;   // the camera image's bottom left, room pixels
    vec2 u_light_dir;
    int u_index;         // palette layer
    int u_camera_index;
    float u_zoom;
    float u_night_time;
    float u_rain;
    float u_light;
    float u_clouds_speed;
    int u_later_count;   // camera images drawn after this one
    vec4 u_later[16];    // their bottom lefts, two per vec4
    float u_from;        // depth range, 0..1 of the 30 depths
    float u_to;
};
layout(set = 0, binding = 1) uniform sampler2DArray u_level;
layout(set = 0, binding = 2) uniform sampler2DArray u_palette;
layout(set = 0, binding = 3) uniform sampler2D u_decal;
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

vec3 terrain_color(level_px p, float shadow, bool rain) {
    vec3 c = mix(level_pal(u_palette, u_index, p.depth, p.palette_color + 3, rain),
                 level_pal(u_palette, u_index, p.depth, p.palette_color, rain), shadow);
    return mix(c, level_pal(u_palette, u_index, 1, 7, rain), clamp(float(p.depth) * level_fog_amount(u_palette, u_index, rain) / 30.0, 0.0, 1.0));
}

void main() {
    // the level pixel under it, y from the bottom
    vec2 tc = (floor(v_room) + 0.5 - u_cam_origin) / level_size;
    if (tc.x < 0.0 || tc.y < 0.0 || tc.x > 1.0 || tc.y > 1.0)
        discard;
    if (shown_by_later_camera(v_room))
        discard;
    ivec3 t = ivec3(texture(u_level, vec3(tc.x, 1.0 - tc.y, float(u_camera_index))).rgb * 255.0 + 0.5);
    level_px p = level_decode(t);
    if (p.sky)
        discard;
    float shadow = p.sunlit ? level_cloud_shadow(tc, t.r, u_rain, u_clouds_speed, u_light) : 1.0;
    float d = float(p.depth) / 30.0;
    if (d < u_from || d > u_to)
        discard;
    if (shadow != 1.0 && p.depth >= 5
        && level_sprite_shadow(u_mask, tc, p.depth, u_light_dir, u_cam_origin, u_view_pos, u_zoom, u_screen))
        shadow = 1.0;
    if (p.depth >= 5 && level_sprite_at(u_mask, (v_room - u_view_pos) * u_zoom / u_screen))
        discard;
    vec3 terrain = mix(terrain_color(p, shadow, false), terrain_color(p, shadow, true), u_night_time);

    // the image shrinks into the deeper layers and drips with the smudge
    vec2 g = (v_uv - 0.5) * mix(1.0, 1.5, d) + 0.5;
    float h = mix(floor(tc.x * 700.0) / 700.0, tc.x, 0.5);
    float smudge = v_color.r;
    float drip = level_noise(vec2(h * 4.0, tc.y * 0.01 + float(p.depth) / 100.0), 64.0);
    g.y += pow(max(0.0, drip - 0.5) * 2.0, 3.0 - smudge) * 0.3 * smudge;
    // (Unity's uv, y from the bottom; the image's top row first)
    vec4 c = texture(u_decal, vec2(g.x, 1.0 - g.y));

    vec3 light_col = mix(terrain, c.rgb, 0.5);
    vec3 screen = 1.0 - (1.0 - terrain) * (1.0 - c.rgb);
    light_col = mix(light_col, screen, (1.0 - shadow) * (p.palette_color == 2 ? 0.9 : 0.25));
    vec3 col = mix(light_col, terrain * c.rgb, mix(0.2, 0.3, shadow));
    float opacity = v_color.a;
    float a = c.a * mix(1.0, 0.25 + 0.25 * opacity, shadow) * opacity;
    if (a <= 0.0)
        discard;
    o_color = vec4(col, a);
}
