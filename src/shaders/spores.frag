// Vanilla's Spores shader (SandPuffSpawner.SandPuff's sprite): a dithered
// cloud of specks, thinning out towards its edge and as its alpha drops
// (the vertex alpha). Its colour is SandPuff.ApplyPalette's: the terrain
// palette's light dust where the level is sunlit (RoomCamera.LitAtCoordinate,
// here per pixel out of the camera image the level shows), its dark dust
// where it isn't, by the palette's darkness over the open sky. _NoiseTex is
// hashed value noise.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "level_common.glsl"

layout(location = 0) in vec2 v_room;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec4 v_color;
layout(location = 0) out vec4 o_color;

// render.cpp's spores_uniforms
layout(set = 0, binding = 0, std140) uniform spores_uniforms {
    mat4 u_proj;
    vec4 u_light_dust;
    vec4 u_dark_dust;
    float u_rain;      // _RAIN
    int u_cam_count;   // camera images (0: no level, the sky's choice everywhere)
    int u_sky_lit;     // the palette's darkness under 0.5
    float u_pad;
    vec4 u_cams[16];   // their bottom lefts, two per vec4
};
layout(set = 0, binding = 1) uniform sampler2DArray u_level;

bool lit_at(vec2 room) {
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
        return u_sky_lit != 0;
    vec2 tc = (floor(room - origin) + 0.5) / level_size;
    ivec3 t = ivec3(texture(u_level, vec3(tc.x, 1.0 - tc.y, float(cam))).rgb * 255.0 + 0.5);
    if (t == ivec3(255))
        return u_sky_lit != 0;
    return t.r > 90;
}

void main() {
    // (the screen's pixels in vanilla: the room's here, so the specks stay put)
    vec2 tc = vec2(floor(v_room.x), floor(v_room.y + u_rain * 153.2)) / level_size;
    tc.y += 0.04;
    float dist = clamp(1.0 - distance(v_uv, vec2(0.5)) * 2.0, 0.0, 1.0);
    float h = sin((1.77 * u_rain + level_tex_noise(vec2(tc.x * 5.2, u_rain * -0.1 + tc.y * 2.6)) * 3.0) * 3.14 * 2.0) * 0.5 + 0.5;
    h *= sin((3.5 * u_rain + level_tex_noise(vec2(tc.x * 12.2, u_rain * -0.25 + tc.y * 6.6)) * 3.0) * 3.14 * 2.0) * 0.5 + 0.5;
    h *= 0.5 + 0.5 * sin((level_tex_noise(v_uv) + u_rain) * 6.28 * 3.0);
    float a = v_color.a;
    h = mix(h * dist, mix(h, 1.0, mix(0.3, 0.8, a)), dist);
    float rnd = fract(sin(tc.x * 12.98232 + tc.y - level_tex_noise(tc)) * 43758.5453);
    h -= rnd * mix(0.7, 0.3, a);
    if (h * a < 0.35)
        discard;
    vec3 c = lit_at(v_room) ? u_light_dust.rgb : u_dark_dust.rgb;
    o_color = vec4(h * a > 0.5 ? c * 0.7 : c, 1.0);
}
