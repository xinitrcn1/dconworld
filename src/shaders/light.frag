// LightSource: vanilla's LightSource shader (FlatLight for the flat ones).
// The light falls on the level by depth: strongest on the near layers,
// shadowed where nearer geometry stands between it and a deeper surface
// (the level sampled back towards the light's centre), a highlight on the
// edges facing it. Sprites over depths 5+ aren't lit, and they cast their
// shadows away from the light (the sprite mask, binding 4). Drawn once per
// camera image it overlaps, a pixel only from the camera image the level
// shows there. Alpha blended over the level, like vanilla's
// ForegroundLights container.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "level_common.glsl"

layout(location = 0) in vec2 v_room;
layout(location = 1) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

// render.cpp's light_uniforms
layout(set = 0, binding = 0, std140) uniform light_uniforms {
    mat4 u_proj;
    vec2 u_screen;
    vec2 u_view_pos;
    vec2 u_cam_origin;     // this camera image's bottom left, room pixels
    vec2 u_center;         // the light, room pixels
    vec4 u_color;          // rgb: White's colour; a: the sprite's alpha before colorAlpha
    ivec2 u_effect_colors;
    int u_index;           // palette layer
    int u_camera_index;
    float u_zoom;
    float u_night_time;
    int u_color_type;      // 0 Environment, 1 White, 2 EffectColor1, 3 EffectColor2
    int u_flat;
    vec2 u_env_origin;     // the camera image under the light (Environment's colour)
    int u_env_camera;
    int u_later_count;     // camera images drawn after this one
    vec4 u_later[16];      // their bottom lefts, two per vec4
};
layout(set = 0, binding = 1) uniform sampler2DArray u_level;
layout(set = 0, binding = 2) uniform sampler2DArray u_palette;
layout(set = 0, binding = 3) uniform sampler2D u_effects;
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

// the level at `tc` (this camera image's uv, y from the bottom)
ivec3 level_at(vec2 tc) {
    return ivec3(texture(u_level, vec3(tc.x, 1.0 - tc.y, float(u_camera_index))).rgb * 255.0 + 0.5);
}
// fmod(round(red) - 1, 30) / 30, the sky 1
float depth_at(vec2 tc) {
    ivec3 t = level_at(tc);
    if (t == ivec3(255))
        return 1.0;
    return float(max(t.r - 1, 0) % 30) / 30.0;
}

// RoomCamera.PixelColorAtCoordinate at the light
vec3 environment_color(bool rain) {
    vec2 tc = (floor(u_center - u_env_origin) + 0.5) / level_size;
    ivec3 t = ivec3(texture(u_level, vec3(tc.x, 1.0 - tc.y, float(u_env_camera))).rgb * 255.0 + 0.5);
    if (t == ivec3(255))
        return level_pal(u_palette, u_index, 0, 7, rain);
    int num = t.r;
    float shade = 0.0;
    if (num > 90)
        num -= 90;
    else
        shade = 1.0;
    int facing = num / 30;
    int depth = max(num - 1, 0) % 30;
    vec3 c = mix(level_pal(u_palette, u_index, depth, facing + 3, rain), level_pal(u_palette, u_index, depth, facing, rain), shade);
    return mix(c, level_pal(u_palette, u_index, 1, 7, rain), float(depth) * (1.0 - level_pal(u_palette, u_index, 9, 7, rain).r) / 30.0);
}

// LightSource.ApplyPalette: palette.texture.GetPixel(30, 5 - effectColor * 2)
vec3 effect_color(bool rain) {
    int fx = u_color_type == 2 ? u_effect_colors.x : u_effect_colors.y;
    if (fx < 0)
        return level_pal(u_palette, u_index, 30, u_color_type == 2 ? 5 : 3, rain);
    return texelFetch(u_effects, ivec2(fx * 2, rain ? 2 : 0), 0).rgb;
}

void main() {
    float cd = distance(v_uv, vec2(0.5));
    if (cd > 0.5)
        discard; // (vanilla's CircularSprite)
    vec2 tc = (floor(v_room) + 0.5 - u_cam_origin) / level_size;
    if (tc.x < 0.0 || tc.y < 0.0 || tc.x > 1.0 || tc.y > 1.0)
        discard;
    if (shown_by_later_camera(v_room))
        discard;

    // LightSource.color's setter: the colour at full strength, its
    // brightness (colorAlpha) moved into the alpha
    vec3 rgb = u_color.rgb;
    if (u_color_type == 0)
        rgb = mix(environment_color(false), environment_color(true), u_night_time);
    else if (u_color_type >= 2)
        rgb = mix(effect_color(false), effect_color(true), u_night_time);
    float color_alpha = max(rgb.r, max(rgb.g, rgb.b));
    if (color_alpha <= 0.0)
        discard;
    rgb /= color_alpha;
    float alpha = u_color.a * color_alpha;

    if (u_flat != 0) {
        float c = pow(1.0 - pow(cd * 2.0, 2.0), 3.5);
        o_color = vec4(rgb, c * alpha);
        return;
    }

    ivec3 t = level_at(tc);
    int palette_color = (t.r % 90) / 30;
    if (t.g >= 16)
        palette_color = 3;
    float dist = t == ivec3(255) ? 1.0 : float(max(t.r - 1, 0) % 30) / 30.0;

    vec2 dir = normalize(v_uv - vec2(0.5) + vec2(1e-6, 0.0));
    float k = pow(cd, 1.25);
    vec2 shadow_pos = tc - dir * k * dist * dist * 0.3;
    vec2 highlight_pos = tc - dir * 0.003 * pow(cd * 2.0, 0.25);
    // (in screen units in vanilla, the screen about the camera image's size)
    vec2 old_shadow_room = v_room - dir * k * pow(dist, 1.5) * 0.3 * level_size;

    float shadow_dist = depth_at(shadow_pos);
    float highlight_dist = depth_at(highlight_pos);

    // sprites over the deeper layers aren't lit
    if (dist > 5.0 / 30.0 && level_sprite_at(u_mask, (v_room - u_view_pos) * u_zoom / u_screen))
        discard;
    // and they shade the layers behind them
    if (shadow_dist > 5.0 / 30.0 && level_sprite_at(u_mask, (old_shadow_room - u_view_pos) * u_zoom / u_screen))
        shadow_dist = 6.0 / 30.0;

    float shadow = dist - shadow_dist - (palette_color == 1 ? 0.0 : 2.0 / 30.0);
    shadow = pow(clamp(shadow, 0.0, 1.0), mix(1.0 - dist, 0.5, 0.5));

    float highlight = 0.0;
    if (highlight_dist > dist + 0.05)
        highlight = sin(cd * 3.14 * 2.0);

    if (palette_color == 0) {
        vec2 sd2_pos = tc - dir * 0.01 * cd;
        ivec3 s2 = level_at(sd2_pos);
        float sd2 = float(max(s2.r - 1, 0) % 30) / 30.0;
        if (sd2 < dist && sd2 > dist - 0.1)
            shadow = mix(shadow, 1.0, pow(cd * 2.0, 2.5 - 4.0 * cd));
    }

    float d = dist;
    if (dist < 0.2)
        dist = pow(1.0 - dist * 5.0, 0.35);
    else
        dist = clamp((dist - 0.2) * 1.3, 0.0, 1.0);
    dist = 1.0 - dist;
    dist *= pow(pow(1.0 - pow(cd * 2.0, 2.0), 3.5), mix(0.5, 3.5, d));

    dist = clamp(mix(dist, 0.0, shadow) - shadow * 0.3, 0.0, 1.0);
    if (palette_color == 0)
        dist *= 0.8;
    else if (palette_color == 2)
        dist = pow(dist, 0.8);
    else if (palette_color == 3) {
        dist *= 0.2;
        highlight = 0.0;
    }
    float a = dist * alpha * (0.65 + highlight * 0.35);
    if (a <= 0.0)
        discard;
    o_color = vec4(rgb, a);
}
