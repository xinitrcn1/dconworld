// LightSource: vanilla's LightSource shader (FlatLight for the flat ones,
// UnderWaterLight for the second sprite a LightSource has in a room with
// water). The light falls on the level by depth: strongest on the near
// layers, shadowed where nearer geometry stands between it and a deeper
// surface (the level sampled back towards the light's centre), a highlight
// on the edges facing it. Sprites over depths 5+ aren't lit, and they cast
// their shadows away from the light (the sprite mask, binding 4). Drawn
// once per camera image it overlaps, a pixel only from the camera image the
// level shows there. Alpha blended, like vanilla's ForegroundLights and
// Water containers.
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
    vec2 u_env_pos;        // the LightSource's position (Environment's colour)
    vec4 u_color;          // rgb: the colour; a: the sprite's alpha
    vec4 u_element_rect;   // the element in the atlas (binding 3), uv; z 0: plain white
    vec4 u_element_trim;   // its packed part in its source rect, 0..1 of the source
    int u_index;           // palette layer
    int u_camera_index;
    float u_zoom;
    float u_night_time;
    int u_color_mode;      // 0 Environment, 1 LightSource.color (normalised), 2 the sprite's colour as is
    int u_kind;            // 0 LightSource, 1 FlatLight, 2 UnderWaterLight
    vec2 u_env_origin;     // the camera image under the light (Environment's colour)
    int u_env_camera;
    int u_later_count;     // camera images drawn after this one
    float u_rain;          // vanilla's _RAIN
    float u_pad0, u_pad1, u_pad2;
    vec4 u_later[16];      // their bottom lefts, two per vec4
};
layout(set = 0, binding = 1) uniform sampler2DArray u_level;
layout(set = 0, binding = 2) uniform sampler2DArray u_palette;
layout(set = 0, binding = 3) uniform sampler2D u_element;
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
    vec2 tc = (floor(u_env_pos - u_env_origin) + 0.5) / level_size;
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

// tex2D(_MainTex, uv).x: the sprite's element (Futile_White, or
// RedlightLight's LightMask0 out of the atlas)
float element_at(vec2 uv) {
    if (u_element_rect.z <= 0.0)
        return 1.0;
    // (Unity's uv from the bottom, the atlas from the top)
    vec2 src = vec2(uv.x, 1.0 - uv.y);
    vec2 local = (src - u_element_trim.xy) / u_element_trim.zw;
    if (local.x < 0.0 || local.y < 0.0 || local.x > 1.0 || local.y > 1.0)
        return 0.0;
    return texture(u_element, u_element_rect.xy + local * u_element_rect.zw).r;
}

// UnderWaterLight's alpha before the sprite's
float under_water(vec2 tc) {
    vec2 uv = v_uv;
    float rbcol = sin((u_rain + level_tex_noise(tc * 1.2) * 3.0) * 3.14 * 2.0) * 0.5 + 0.5;
    vec2 distortion = vec2(mix(-0.002, 0.002, rbcol) * mix(1.0, 20.0, pow(uv.y, 200.0)), -0.02 * pow(uv.y, 8.0));
    distortion = floor(distortion * level_size) / level_size;
    float dist = depth_at(tc + distortion);
    if (dist > 6.0 / 30.0 && level_sprite_at(u_mask, (v_room - u_view_pos) * u_zoom / u_screen))
        dist = 6.0 / 30.0;
    vec2 dir = normalize(uv - vec2(0.5) + vec2(1e-6, 0.0));
    float cd = clamp(distance(uv, vec2(0.5)) * 2.0, 0.0, 1.0);
    tc += cd * dir * 0.02;
    float d = dist;
    if (dist < 0.2)
        dist = pow(1.0 - dist * 5.0, 0.35);
    else
        dist = clamp((dist - 0.2) * 1.3, 0.0, 1.0);
    dist = 1.0 - dist;
    dist *= pow(1.0 - pow(min(cd + d * 0.5, 1.0), 2.0), 3.5);
    float s1 = u_rain * 6.0 + level_tex_noise(vec2(d / 10.0 + mix(tc.x, 0.5, d / 3.0) * 2.1, u_rain * 0.1 + d / 5.0 + mix(tc.y, 0.5, d / 3.0) * 2.1)) * 7.0;
    float col = sin(s1 * 3.14 * 2.0) * 0.5 + 0.5;
    tc += dir * 0.15 * pow(1.0 - cd, 1.7);
    float s2 = u_rain * 2.7 + level_tex_noise(vec2(d / 7.0 + mix(tc.x, 0.5, d / 5.0) * 1.3, u_rain * -0.21 + d / 8.0 + mix(tc.y, 0.5, d / 6.0) * 1.3)) * 6.33;
    float col2 = sin(s2 * 3.14 * 2.0) * 0.5 + 0.5;
    if (pow(max(col, col2), 47.0) >= 0.8)
        dist = pow(dist, 0.6 + 0.4 * cd);
    dist *= mix(1.0, max(col, col2), pow(cd, 1.5));
    return dist * mix(0.75, 0.5, cd);
}

void main() {
    float cd = distance(v_uv, vec2(0.5));
    // (vanilla's CircularSprite; UnderWaterLight is a square sprite)
    if (cd > 0.5 && u_kind != 2)
        discard;
    vec2 tc = (floor(v_room) + 0.5 - u_cam_origin) / level_size;
    if (tc.x < 0.0 || tc.y < 0.0 || tc.x > 1.0 || tc.y > 1.0)
        discard;
    if (shown_by_later_camera(v_room))
        discard;

    // LightSource.color's setter: the colour at full strength, its
    // brightness (colorAlpha) moved into the alpha
    vec3 rgb = u_color.rgb;
    if (u_color_mode == 0)
        rgb = mix(environment_color(false), environment_color(true), u_night_time);
    float alpha = u_color.a;
    if (u_color_mode != 2) {
        float color_alpha = max(rgb.r, max(rgb.g, rgb.b));
        if (color_alpha <= 0.0)
            discard;
        rgb /= color_alpha;
        alpha *= color_alpha;
    }

    if (u_kind == 1) {
        float c = pow(1.0 - pow(min(cd * 2.0, 1.0), 2.0), 3.5);
        o_color = vec4(rgb, c * alpha * element_at(v_uv));
        return;
    }
    if (u_kind == 2) {
        float a = under_water(tc) * alpha;
        if (a <= 0.0)
            discard;
        o_color = vec4(rgb, a);
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
    dist *= element_at(v_uv);
    float a = dist * alpha * (0.65 + highlight * 0.35);
    if (a <= 0.0)
        discard;
    o_color = vec4(rgb, a);
}
