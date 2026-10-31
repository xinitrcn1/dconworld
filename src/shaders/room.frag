// The level shader for room camera images: vanilla's LevelColor (the
// ripple, heat and melt variants left out). Its colours come from the
// room palette (a layer of the palette texture array) and the global
// effectcolors.png (vanilla copies them into the palette's columns 30, 31).
//
// Vanilla draws the level once over the sprites, making it transparent
// wherever a sprite was drawn on depths 5+, and the sprites cast shadows
// on sunlit depths 5+ along the room's lightAngle (the GrabPass of what was
// drawn before). Here it is drawn in passes:
//   0: depths 5+ and the sky, behind the sprites, with the clouds' shadow;
//   1: depths 0..4, over the sprites;
//   2: after the sprites, over the pass 0 pixels no sprite covers: only
//      those a sprite shades from the sun, in their shaded colour. The
//      sprites are read from the sprite mask (binding 4: the frame's alpha
//      grabbed after them, render.cpp)
#version 450
#extension GL_GOOGLE_include_directive : require
#include "level_common.glsl"

// render.cpp's room_uniforms
layout(set = 0, binding = 0, std140) uniform room_uniforms {
    mat4 u_proj;
    vec2 u_screen;        // framebuffer pixels
    vec2 u_view_pos;      // the view's bottom left, room pixels
    vec2 u_cam_origin;    // this camera image's bottom left, room pixels
    vec2 u_light_dir;     // room.lightAngle
    ivec2 u_effect_colors; // EffectColorA, EffectColorB (-1: none)
    int u_index;          // palette layer
    int u_camera_index;   // camera image layer
    float u_zoom;
    float u_night_time;   // [0, 1] towards the rain palette
    float u_grime;        // roomSettings.Grime
    float u_rain;         // vanilla's _RAIN clock
    float u_light;        // vanilla's _light
    float u_clouds_speed; // vanilla's _cloudsSpeed
    int u_pass;
    float u_swarm;        // vanilla's _SwarmRoom
};
layout(set = 0, binding = 1) uniform sampler2DArray u_level;   // the room's camera images
layout(set = 0, binding = 2) uniform sampler2DArray u_palette;
layout(set = 0, binding = 3) uniform sampler2D u_effects;      // effectcolors.png
layout(set = 0, binding = 4) uniform sampler2D u_mask;         // the sprite mask (pass 2)

layout(location = 0) out vec4 o_frag_color;
layout(location = 0) in vec2 v_texcoord;

// palette texel (30 + col, y) as vanilla's ApplyEffectColorsToPaletteTexture
// leaves it for effect colour `e` (1: A, 2: B): rows y 5/4 (A) and 3/2 (B)
vec3 effect_texel(int e, int col, bool far, bool rain) {
    int fx = e == 1 ? u_effect_colors.x : u_effect_colors.y;
    if (fx < 0)
        return level_pal(u_palette, u_index, 30 + col, (e == 1 ? 5 : 3) - (far ? 1 : 0), rain);
    // effectcolors.png, top row first: rows 0/1 the palette's near/far, 2/3 the rain's
    return texelFetch(u_effects, ivec2(fx * 2 + col, (rain ? 2 : 0) + (far ? 1 : 0)), 0).rgb;
}

vec3 level_color(bool rain, ivec3 t, level_px p, float shadow, vec2 uv) {
    int red = p.depth;
    int column = p.floor_dark ? red : 0;
    vec3 fog = level_pal(u_palette, u_index, 1, 7, rain);
    vec3 c = mix(level_pal(u_palette, u_index, column, p.palette_color + 3, rain),
                 level_pal(u_palette, u_index, column, p.palette_color, rain), shadow);
    // grime
    float rbcol = sin((u_rain + level_tex_noise(uv * 2.0) * 4.0 + float(red) / 12.0) * 3.14 * 2.0) * 0.5 + 0.5;
    if (p.green >= 4)
        c = mix(c, level_pal(u_palette, u_index, int(5.5 + rbcol * 25.0), 6, rain), 0.2 * u_grime);
    if (p.prop) {
        // coloured props: their colour from the image's top row
        vec3 prop = texelFetch(u_level, ivec3(255 - t.b, 0, u_camera_index), 0).rgb;
        if (p.palette_color == 2)
            prop = mix(prop, vec3(1.0), 0.2 - shadow * 0.1);
        prop = mix(prop, fog, float(red) / 60.0);
        c = mix(mix(c, prop, 0.7), c * prop * 1.5, mix(0.9, 0.3 + 0.4 * shadow, clamp((float(red) - 3.5) * 0.3, 0.0, 1.0)));
    } else if (p.green > 0 && p.green < 3) {
        vec3 near = mix(effect_texel(p.green, 0, false, rain), effect_texel(p.green, 1, false, rain), shadow);
        vec3 far = mix(effect_texel(p.green, 0, true, rain), effect_texel(p.green, 1, true, rain), shadow);
        c = mix(c, mix(near, far, float(red) / 30.0), float(t.b) / 255.0);
    } else if (p.green == 3) {
        c = mix(c, vec3(1.0), float(t.b) / 255.0 * u_swarm);
    }
    float fog_amount = level_fog_amount(u_palette, u_index, rain);
    float fog_k = red < 10 ? mix(p.floor_dark ? 1.0 : 0.0, 1.0, 0.5) : 1.0;
    return mix(c, fog, clamp(float(red) * fog_k * fog_amount / 30.0, 0.0, 1.0));
}

void main() {
    ivec3 t = ivec3(texture(u_level, vec3(v_texcoord, float(u_camera_index))).rgb * 255.0 + 0.5);
    level_px p = level_decode(t);
    bool foreground = !p.sky && t.r > 0 && p.depth < 5;
    if (u_pass == 1 ? !foreground : foreground)
        discard;
    if (p.sky) {
        if (u_pass == 2)
            discard;
        // the sky colour is the frame's clear colour (and the backgrounds')
        o_frag_color = vec4(0.0);
        return;
    }
    // Unity's uv: y from the bottom
    vec2 uv = vec2(v_texcoord.x, 1.0 - v_texcoord.y);
    float shadow = p.sunlit ? level_cloud_shadow(uv, t.r, u_rain, u_clouds_speed, u_light) : 1.0;
    if (u_pass == 2) {
        if (shadow >= 1.0)
            discard;
        vec2 room = u_cam_origin + uv * level_size;
        // (vanilla's level is transparent there: the sprite shows)
        if (level_sprite_at(u_mask, (room - u_view_pos) * u_zoom / u_screen))
            discard;
        if (!level_sprite_shadow(u_mask, uv, p.depth, u_light_dir, u_cam_origin, u_view_pos, u_zoom, u_screen))
            discard;
        shadow = 1.0;
    }
    vec3 day = level_color(false, t, p, shadow, uv);
    vec3 night = level_color(true, t, p, shadow, uv);
    o_frag_color = vec4(mix(day, night, u_night_time), 1.0);
}
