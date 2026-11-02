// The Watcher's DynamicLevelElement meshes that FloatingDebris' "Rot Islands"
// (RotFloater) are made of: vanilla's DynamicLevelBlob and DynamicLevelWire
// shaders (_DynamicLevelElementFunctions.cginc's Blob and Wire). Vanilla
// writes them into the depth texture and the level texture, so they take
// the level's colours; here each fragment works out the same level pixel
// (pack_red's light, depth and facing, effect colour B as the green flag
// with the glow in blue) and colours it the way room.frag colours the camera
// images. It is hidden where the camera image is nearer than it.
// Differences: the normals come from the mesh's own depth (screen space
// derivatives) instead of the depth texture, there is no SelfShadow
// marching, and SampleSunlight is whether the level pixel behind is sunlit
// or sky. The two meshes are told apart by the uv: a wire's v is shifted
// up by 2 (room_decor.cpp).
#version 450
#extension GL_GOOGLE_include_directive : require
#include "level_common.glsl"

layout(location = 0) in vec2 v_room;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec4 v_color;
layout(location = 0) out vec4 o_color;

// room_decor.cpp's dynamic_level_uniforms
layout(set = 0, binding = 0, std140) uniform dynamic_level_uniforms {
    mat4 u_proj;
    vec2 u_screen;
    vec2 u_view_pos;
    vec2 u_cam_origin;     // the camera image's bottom left, room pixels
    vec2 u_light_dir;      // room.lightAngle (vanilla's _lightDirAndPixelSize.xy)
    ivec2 u_effect_colors; // EffectColorA, EffectColorB (-1: none)
    int u_index;           // palette layer
    int u_camera_index;
    float u_zoom;
    float u_night_time;
    float u_rain;          // vanilla's _RAIN
    float u_light;         // vanilla's _light
    float u_clouds_speed;
    int u_later_count;     // camera images drawn after this one
    vec4 u_later[16];      // their bottom lefts, two per vec4
};
layout(set = 0, binding = 1) uniform sampler2DArray u_level;
layout(set = 0, binding = 2) uniform sampler2DArray u_palette;
layout(set = 0, binding = 3) uniform sampler2D u_effects;

const float depth_mult = 0.766; // _DepthMult

// HLSL's smoothstep (also with edge0 > edge1, which GLSL leaves undefined)
float hsmooth(float a, float b, float x) {
    float t = clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

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

// room.frag's effect_texel
vec3 effect_texel(int e, int col, bool far, bool rain) {
    int fx = e == 1 ? u_effect_colors.x : u_effect_colors.y;
    if (fx < 0)
        return level_pal(u_palette, u_index, 30 + col, (e == 1 ? 5 : 3) - (far ? 1 : 0), rain);
    return texelFetch(u_effects, ivec2(fx * 2 + col, (rain ? 2 : 0) + (far ? 1 : 0)), 0).rgb;
}

// room.frag's level_color for the pixel the shaders write: no floor
// darkening flag (notFloorDark), green 2 (effect colour B) mixed by `b`
vec3 element_color(bool rain, int depth, int facing, float shadow, float b) {
    vec3 fog = level_pal(u_palette, u_index, 1, 7, rain);
    vec3 c = mix(level_pal(u_palette, u_index, depth, facing + 3, rain),
                 level_pal(u_palette, u_index, depth, facing, rain), shadow);
    vec3 near = mix(effect_texel(2, 0, false, rain), effect_texel(2, 1, false, rain), shadow);
    vec3 far = mix(effect_texel(2, 0, true, rain), effect_texel(2, 1, true, rain), shadow);
    c = mix(c, mix(near, far, float(depth) / 30.0), b);
    float fog_amount = level_fog_amount(u_palette, u_index, rain);
    return mix(c, fog, clamp(float(depth) * fog_amount / 30.0, 0.0, 1.0));
}

// _DynamicLevelElementFunctions.cginc's Blob: the depth written, 0 outside
float blob_depth(vec4 clr, vec2 uv) {
    float wave = sin(u_rain * (3.0 + 9.0 * clr.z) + clr.y * 3.14159) * 0.5 + 0.5;
    vec2 cuv = abs(uv * 2.0 - 1.0);
    float noise = level_tex_noise(uv * 0.25 + vec2(clr.y * 3.0, clr.z * 7.0));
    noise = clamp(hsmooth(0.1 + wave * 0.4, 0.0, abs(noise - 0.5)), 0.0, 1.0);
    float noise1 = level_tex_noise(uv * 0.2 + vec2(clr.y * 5.0 + wave * 0.05, clr.z * 17.0));
    noise1 = hsmooth(0.25, 1.0, noise1);
    float sphere = 1.0 - pow(clamp(length(cuv) + 0.15 + noise1 * 0.4 - (1.0 - wave) * 0.15, 0.0, 1.0), 2.0);
    float blob = sphere - noise * 0.2 * (1.0 - wave);
    if (blob <= 0.0)
        return 0.0;
    return (clr.x * 0.9 + blob * 0.1) * depth_mult;
}

// _DynamicLevelElementFunctions.cginc's Wire
float wire_depth(vec4 clr, vec2 uv) {
    uv.x += clr.z * 33.0;
    float wire = (-uv.y * uv.y + uv.y) * 3.0;
    float direction = clr.y * 2.0 - 1.0;
    float wave = sin(uv.x * 4.0 + u_rain * sign(direction) * (8.0 + 10.0 * clr.z) + clr.z * 33.0) * clr.z * 0.15;
    float n1 = level_tex_noise(uv * vec2(4.0, 0.076) + vec2(-wave * 0.3, clr.w * 11.0)) * 2.0 - 1.0;
    float n = level_tex_noise(uv * vec2(4.0, 0.076) + vec2(wire * direction * 0.2 - wave * 0.6, clr.w * 11.0)) * 2.0 - 1.0;
    wire = max(wire, wire + wave);
    if (wire - n1 * 0.75 <= 0.0)
        return 0.0;
    wire = clamp(wire - n * 0.51, 0.0, 1.0);
    return (clr.x * 0.9 + wire * 0.1) * depth_mult;
}

void main() {
    bool is_wire = v_uv.y > 1.5;
    vec2 uv = is_wire ? v_uv - vec2(0.0, 2.0) : v_uv;
    vec4 clr = v_color;
    float d = is_wire ? wire_depth(clr, uv) : blob_depth(clr, uv);

    // getNormals: the depth's slope a room pixel to the right and up (the
    // mesh's own; vanilla reads the depth texture), clamped
    float clmp = is_wire ? 0.0125 : 0.003;
    float rx = dFdxFine(v_room.x);
    float ry = dFdyFine(v_room.y);
    float ddx = dFdxFine(d);
    float ddy = dFdyFine(d);
    float gx = abs(rx) > 1e-5 ? ddx / rx : 0.0;
    float gy = abs(ry) > 1e-5 ? ddy / ry : 0.0;
    vec3 normals = normalize(vec3(clamp(gx, -clmp, clmp), clamp(gy, -clmp, clmp), (d - clr.x * 0.9 * depth_mult) * 0.1));

    if (d <= 0.0)
        discard;
    // the level pixel behind, y from the bottom
    vec2 tc = (floor(v_room) + 0.5 - u_cam_origin) / level_size;
    if (tc.x < 0.0 || tc.y < 0.0 || tc.x > 1.0 || tc.y > 1.0)
        discard;
    if (shown_by_later_camera(v_room))
        discard;
    ivec2 px = clamp(ivec2(floor(tc * level_size)), ivec2(0), ivec2(level_size) - 1);
    ivec3 t = ivec3(texelFetch(u_level, ivec3(px.x, int(level_size.y) - 1 - px.y, u_camera_index), 0).rgb * 255.0 + 0.5);
    level_px p = level_decode(t);
    int depth = min(int(clamp(1.0 - d, 0.0, 1.0) * 30.0), 29);
    // ClipByDepth: the level is in front
    if (!p.sky && p.depth < depth)
        discard;

    vec3 ld = normalize(vec3(u_light_dir.x, -u_light_dir.y, 8.0));
    float light2 = dot(normals, vec3(0.0, -1.0, 0.5));
    float light;
    float facing;
    float b;
    if (is_wire) {
        float direction = clr.y * 2.0 - 1.0;
        float wave = sin((uv.x + clr.z * 33.0) * 4.0 + u_rain * sign(direction) * (8.0 + 10.0 * clr.z) + clr.z * 33.0) * clr.z * 0.15;
        wave = hsmooth(0.0, 0.1, wave);
        float bumps = (d - clr.x * 0.9 * depth_mult) * 10.0;
        light = step(0.2 + abs(direction * 0.5), clamp(dot(normals, ld), 0.0, 1.0));
        float highlight = step(0.27, light2);
        float midlight = step(-0.3, light2);
        facing = max(highlight * 2.0, midlight);
        b = hsmooth(0.4, 1.0, bumps) * wave * 0.5;
    } else {
        float wave = sin(u_rain * (3.0 + 9.0 * clr.z) + clr.y * 3.14159) * 0.5 + 0.5;
        float noise = level_tex_noise(uv * 0.25 + vec2(clr.y * 3.0, clr.z * 7.0));
        float glow = clamp(hsmooth(0.0, 0.4, abs(noise - 0.5)), 0.0, 1.0);
        glow = glow * (1.0 + sin(uv.x * 10.0 + wave * (3.0 + clr.z) + clr.z + u_rain) * 0.5);
        float tendons = clamp(hsmooth(0.09, 0.0, abs(noise - 0.5)), 0.0, 1.0) * (1.0 - wave);
        light = step(0.1, dot(normals, ld));
        float midlight = step(-0.3 + tendons, light2);
        float highlight = step(0.22 + tendons * 0.5, light2);
        facing = max(highlight * 2.0, midlight);
        b = glow * (1.0 - wave) * 0.25;
    }
    // (SelfShadow isn't marched; SampleSunlight: the level behind is sunlit or sky)
    float shadow_flag = (p.sky || p.sunlit) ? 0.0 : 1.0;
    bool sunlit = clamp(light - shadow_flag, 0.0, 1.0) > 0.5;
    int pc = clamp(int(facing), 0, 2);
    // room.frag: a sunlit pixel takes the clouds' shadow, a shaded one is in shade
    float shadow = sunlit ? level_cloud_shadow(tc, depth + 90, u_rain, u_clouds_speed, u_light) : 1.0;
    vec3 day = element_color(false, depth, pc, shadow, clamp(b, 0.0, 1.0));
    vec3 night = element_color(true, depth, pc, shadow, clamp(b, 0.0, 1.0));
    o_color = vec4(mix(day, night, u_night_time), 1.0);
}
