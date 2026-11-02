// The BackgroundScene shaders of vanilla's AboveCloudsView
// (background_scene.cpp), one mode each:
//   0 Background: the illustration times the sprite's colour;
//   1 BackgroundAdditive (Blend SrcAlpha One);
//   2 Background on the "pixel" element: a plain colour;
//   3 Cloud: the close clouds (clouds1..3), drifting with _RAIN, their
//     density (red) and shade (green) mixed with _CloudsTex, coloured from
//     the palette's sky colour towards _AboveCloudsAtmosphereColor by depth;
//   4 CloudDistant: the distant and flying clouds, the same without the
//     depth offsets;
//   5 DistantBkgObject: buildings faded into the atmosphere colour.
// The scene is drawn before the level, so vanilla's BackgroundClipVanilla
// (only where the level is sky and nothing was drawn) is the level drawing
// over it; the Watcher's ripple clip isn't ported. Colours go out
// premultiplied (Blend One OneMinusSrcAlpha, the target's alpha kept), the
// additive mode with alpha 0. vanilla's _NoiseTex is hashed value noise
// (level_common.glsl); a missing cloud illustration or cloudsTexture falls
// back on noise too (u_flags).
#version 450
#extension GL_GOOGLE_include_directive : require
#include "level_common.glsl"

layout(location = 0) in vec2 v_room;
layout(location = 1) in vec2 v_uv;   // the sprite's, Unity's (v from the bottom)
layout(location = 2) in vec4 v_color;
layout(location = 0) out vec4 o_color;

// background_scene.cpp's bkg_uniforms
layout(set = 0, binding = 0, std140) uniform background_uniforms {
    mat4 u_proj;
    vec4 u_clr;          // the sprite's colour (vanilla's vertex colour, the shaders' inputs)
    vec4 u_atmosphere;   // _AboveCloudsAtmosphereColor
    vec4 u_multiply;     // _MultiplyColor
    vec2 u_cam_pos;      // vanilla's camPos, room pixels
    vec2 u_vscreen;      // vanilla's screen size
    float u_rain;        // _RAIN
    float u_wind_dir;    // _windDir
    float u_night_time;  // towards the rain palette
    int u_mode;
    int u_index;         // palette layer
    int u_flags;         // 1: procedural _MainTex, 2: procedural _CloudsTex
    float u_pad0, u_pad1;
};
layout(set = 0, binding = 1) uniform sampler2D u_main;       // the element's illustration
layout(set = 0, binding = 2) uniform sampler2DArray u_palette;
layout(set = 0, binding = 3) uniform sampler2D u_clouds;     // cloudsTexture

// tex2D with Unity's uv (the textures' rows go top to bottom)
vec4 main_tex(vec2 uv) {
    if ((u_flags & 1) != 0) {
        // no clouds illustration: density thinning upwards, a cloudy shade
        float n = level_noise(vec2(uv.x * 6.0, uv.y * 1.5), 1.0) * 0.6 + level_noise(vec2(uv.x * 17.0, uv.y * 4.0) + vec2(5.3, 1.7), 1.0) * 0.4;
        float y = fract(uv.y);
        float dp = clamp(n * 1.2 - y * 0.9 + 0.35, 0.0, 1.0);
        float sh = clamp(0.4 + (n - 0.5) + (1.0 - y) * 0.2, 0.0, 1.0);
        return vec4(dp, sh, 0.0, 1.0);
    }
    return texture(u_main, vec2(uv.x, 1.0 - uv.y));
}
// tex2D(_CloudsTex, uv).x
float clouds_tex(vec2 uv) {
    if ((u_flags & 2) != 0) {
        float n = level_noise(uv + vec2(11.3, 7.1), 4.0) * 0.55 + level_noise(uv + vec2(2.9, 5.3), 11.0) * 0.3 + level_noise(uv, 27.0) * 0.15;
        return clamp((n - 0.35) * 2.2, 0.0, 1.0);
    }
    return texture(u_clouds, vec2(uv.x, 1.0 - uv.y)).x;
}
// tex2D(_NoiseTex, uv).x
float noise_tex(vec2 uv) {
    return level_tex_noise(uv);
}
// tex2D(_PalTex, half2(0.5/32.0, 7.5/8.0)): the sky colour
vec3 sky_color() {
    return mix(level_pal(u_palette, u_index, 0, 7, false), level_pal(u_palette, u_index, 0, 7, true), u_night_time);
}
// the shaders' "if (b > 0.5) a = 2ab; else a = 1 - 2(1-a)(1-b)"
float blend_layers(float a, float b) {
    return b > 0.5 ? 2.0 * a * b : 1.0 - 2.0 * (1.0 - a) * (1.0 - b);
}
// HLSL round (the values rounded here are whole thirds / quarters apart)
float hround(float x) {
    return sign(x) * floor(abs(x) + 0.5);
}

vec4 cloud(vec2 scr_pos) {
    vec4 clr = u_clr;
    vec2 sample_coord = vec2(clr.y + v_uv.x - 0.025 * u_rain * u_wind_dir * (1.0 - clr.x), v_uv.y);
    sample_coord.y = min(1.0 - 0.01, sample_coord.y);

    // (vanilla's half2(0.5, 2/3): the integer division makes it (0.5, 0))
    vec2 dp_offst = vec2(0.5, 0.0) - scr_pos;
    dp_offst.y /= 2.0;

    float h2 = 0.5 - sin((u_wind_dir * 0.07 * u_rain + noise_tex(vec2(sample_coord.x * 1.5, sample_coord.y * 0.75 + clr.y)) * 2.0) * 3.14 * 2.0) * 0.5;
    float dp = main_tex(sample_coord + dp_offst * 0.05 * h2 + vec2(0.0, mix(-1.0, 1.0, h2) * 0.01)).x;
    dp -= 0.1 * h2;
    dp = pow(max(dp - 0.15, 0.0), mix(0.2, 0.35, h2 * h2));
    dp = min(1.0, dp + max(0.0, ((1.0 - v_uv.y) - 0.9) * 15.0));

    float clds = clouds_tex(vec2(sample_coord.x * 5.0 * (1.0 / clr.z), sample_coord.y * 1.5 + u_rain * u_wind_dir * 0.025) + dp_offst * 0.11 * dp);
    float clds2 = clouds_tex(vec2(sample_coord.x * 8.0 * (1.0 / clr.z), sample_coord.y * 2.5 + u_rain * u_wind_dir * 0.018) * mix(2.0, 1.0, sin(clr.x * 3.14))
                             + dp_offst * mix(0.1, 0.2, clds) * dp);
    clds = blend_layers(clds, clds2);
    dp = blend_layers(dp, clds);

    vec4 tex_col = main_tex(sample_coord + dp_offst * 0.15 * dp);
    float noise = noise_tex(vec2((sample_coord.x + dp_offst.x * 0.5 * dp) * 3.0, (sample_coord.y + dp_offst.y * 0.5 * dp) * 1.5));
    noise = 0.5 + sin((u_wind_dir * 0.0025 * u_rain + noise * 2.0) * 3.14 * 2.0) * 0.5;

    float col = pow(tex_col.y, mix(1.4, 0.7, noise)) * clamp((dp - 0.3) * 6.0, 0.5, 1.0);
    col = blend_layers(col, clds);
    col = clamp(col * 1.4, 0.0, 1.0);
    col = mix(col, 0.5, clamp(((1.0 - v_uv.y) - mix(0.8, 0.5, noise)) * mix(5.0, 2.0, noise), 0.0, 1.0));

    vec3 rgb = mix(pow(sky_color(), vec3(mix(1.6, 0.4, hround(col * 4.0) / 4.0))), u_atmosphere.rgb, clr.x);
    clds = mix(clds, 1.0, col);
    float a = hround((pow(clamp(dp, 0.0, 1.0), mix(1.2, 0.05, clds)) * 1.25 - (1.0 - clds) * 0.2 + noise * 0.05) * 3.0) / 3.0;
    return vec4(rgb, a);
}

vec4 cloud_distant() {
    vec4 clr = u_clr;
    vec2 sample_coord = vec2(clr.y + v_uv.x - 0.025 * u_rain * u_wind_dir * (1.0 - clr.x), v_uv.y);
    sample_coord.y = min(1.0 - 0.01, sample_coord.y);

    vec4 main_col = main_tex(sample_coord);
    float dp = main_col.x;
    float clds = clouds_tex(vec2(sample_coord.x * 5.0 * (1.0 / clr.z), sample_coord.y * 1.5 + u_rain * u_wind_dir * 0.025));
    dp = blend_layers(dp, clds);

    float col = main_col.y * clamp((dp - 0.3) * 6.0, 0.5, 1.0);
    vec3 rgb = mix(pow(sky_color(), vec3(mix(1.6, 0.4, hround(col * 4.0) / 4.0))), u_atmosphere.rgb, clr.x);
    float a = (hround((pow(clamp(dp, 0.0, 1.0), mix(1.2, 0.05, clds)) * 1.25 - (1.0 - clds) * 0.2) * 3.0) / 3.0) * clr.w;
    return vec4(rgb, a);
}

void main() {
    // ComputeScreenPos: the point on vanilla's screen, 0..1 from the bottom left
    vec2 scr_pos = (v_room - u_cam_pos) / u_vscreen;
    vec4 c;
    if (u_mode == 0 || u_mode == 1) {
        c = main_tex(v_uv) * u_clr;
    } else if (u_mode == 2) {
        c = u_clr;
    } else if (u_mode == 3) {
        c = cloud(scr_pos);
    } else if (u_mode == 4) {
        c = cloud_distant();
    } else {
        vec4 t = main_tex(v_uv);
        c = vec4(mix(t.rgb, u_atmosphere.rgb, u_clr.x), t.a) * u_multiply;
    }
    c = clamp(c, 0.0, 1.0);
    if (c.a <= 0.0)
        discard;
    o_color = u_mode == 1 ? vec4(c.rgb * c.a, 0.0) : vec4(c.rgb * c.a, c.a);
}
