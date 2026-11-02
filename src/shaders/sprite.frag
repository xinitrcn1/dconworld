#version 450
// Sprites and quads: their colour times the atlas, as the flat shader does
// (mode 3: the ColoredSprite2 shader's palette lookup, see below).
// The Watcher's halo (WeaverGlow): no geometry but its quad, the glow worked
// out per pixel. It was a 64 sided ring of 256 triangles: alpha 0 at 10px, 1
// at 25px, 0 at 50px, linearly, times eight soft lobes around it
// (0.55 + ping^(2 + n12) * (0.45 + 0.2 n11), ping going 0..1..0 over each
// eighth) times its strength
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec2 v_local;
layout(location = 3) flat in uint v_mode;
layout(location = 4) flat in vec4 v_halo;
layout(location = 5) flat in uvec4 v_pal_near;
layout(location = 6) flat in uvec4 v_pal_far;

layout(set = 0, binding = 0, std140) uniform sprite_uniforms {
    mat4 u_proj;
    uint u_first;
    int u_textured;
};
layout(set = 0, binding = 1) uniform sampler2D u_texture;

layout(location = 0) out vec4 frag_color;

const float tau = 6.2831855;

void main() {
    if (v_mode == 2u) {
        float r = length(v_local);
        float radial = r < 25.0 ? (r - 10.0) / 15.0 : (50.0 - r) / 25.0;
        if (radial <= 0.0)
            discard;
        // the ring went round clockwise from +x (angle -a at step a)
        float a = atan(-v_local.y, v_local.x);
        if (a < 0.0)
            a += tau;
        float u = a / tau * 8.0;
        float pp = u - 2.0 * floor(u / 2.0);
        float ping = pp > 1.0 ? 2.0 - pp : pp;
        float band = clamp(0.55 + pow(ping, 2.0 + v_halo.z) * (0.45 + v_halo.y * 0.2), 0.0, 1.0);
        frag_color = vec4(v_color.rgb, v_color.a * band * v_halo.x * radial);
        return;
    }
    if (v_mode == 3u) {
        // ColoredSprite2: the texel's blue picks the palette row
        // (floor(0.5 + 2 b)), its red pushes the depth towards 1 (the two
        // depths' colours mixed), its alpha is the sprite's coverage
        vec4 t = texture(u_texture, v_uv);
        uint row = t.b < 0.25 ? 0u : (t.b < 0.75 ? 1u : 2u);
        vec3 near_col = unpackUnorm4x8(row == 0u ? v_pal_near.x : (row == 1u ? v_pal_near.y : v_pal_near.z)).rgb;
        vec3 far_col = unpackUnorm4x8(row == 0u ? v_pal_far.y : (row == 1u ? v_pal_far.z : v_pal_far.w)).rgb;
        frag_color = vec4(mix(near_col, far_col, t.r), t.a);
        return;
    }
    frag_color = u_textured != 0 ? v_color * texture(u_texture, v_uv) : v_color;
}
