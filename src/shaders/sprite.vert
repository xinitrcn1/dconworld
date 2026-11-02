#version 450
// The sprite pipeline (sprite.vert/frag): Futile sprites, free quads and
// triangles, and the Watcher's halo, one instance each (gpu::sprite_instance,
// five uvec4s, floats as their bits) pulled out of the storage buffer by
// gl_VertexIndex, six vertices making its two triangles. What the CPU used to
// expand into six batch vertices (and a sin and cos per sprite) the GPU does
// here, the same corners: FSprite.UpdateLocalVertices placed and turned
// clockwise about the sprite's position
layout(set = 0, binding = 5, std430) readonly buffer sprite_words {
    uvec4 words[];
};
layout(set = 0, binding = 0, std140) uniform sprite_uniforms {
    mat4 u_proj;
    uint u_first; // the draw's first instance, in uvec4s
    int u_textured;
};

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out vec2 v_local;       // the halo: room pixels from its centre
layout(location = 3) flat out uint v_mode;
layout(location = 4) flat out vec4 v_halo;   // strength, n11, n12

// mode 0: the corners in bl, br, tr, bl, tr, tl order (0 tl, 1 tr, 2 br, 3 bl)
const uint sprite_corner[6] = uint[6](3u, 2u, 1u, 3u, 1u, 0u);
// mode 1: triangles 0 1 2, 0 2 3
const uint quad_corner[6] = uint[6](0u, 1u, 2u, 0u, 2u, 3u);
const vec2 halo_corner[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
const float halo_outer = 50.0;

void main() {
    uint id = uint(gl_VertexIndex);
    uint base = u_first + (id / 6u) * 5u;
    uint k = id % 6u;
    vec4 p0 = uintBitsToFloat(words[base]);
    vec4 p1 = uintBitsToFloat(words[base + 1u]);
    vec4 uv = uintBitsToFloat(words[base + 2u]);
    uvec4 rgba = words[base + 3u];
    uint mode = words[base + 4u].x;
    v_mode = mode;
    v_halo = vec4(0.0);
    v_local = vec2(0.0);
    vec2 pos;
    uint c;
    if (mode == 2u) {
        v_local = halo_corner[k] * halo_outer;
        pos = p0.xy + v_local;
        v_halo = vec4(p0.z, p0.w, p1.x, 0.0);
        v_color = unpackUnorm4x8(rgba.x);
        v_uv = vec2(0.0);
        gl_Position = u_proj * vec4(pos, 0.0, 1.0);
        return;
    }
    if (mode == 0u) {
        c = sprite_corner[k];
        // corner c's local position: tl (x0, y1), tr (x1, y1), br (x1, y0), bl (x0, y0)
        float lx = (c == 0u || c == 3u) ? p1.x : p1.z;
        float ly = (c == 0u || c == 1u) ? p1.w : p1.y;
        float rad = radians(p0.z);
        float cs = cos(rad), sn = sin(rad);
        pos = p0.xy + vec2(lx * cs + ly * sn, -lx * sn + ly * cs);
    } else {
        c = quad_corner[k];
        pos = c == 0u ? p0.xy : c == 1u ? p0.zw : c == 2u ? p1.xy : p1.zw;
    }
    v_uv = vec2((c == 0u || c == 3u) ? uv.x : uv.z, (c == 0u || c == 1u) ? uv.y : uv.w);
    v_color = unpackUnorm4x8(c == 0u ? rgba.x : c == 1u ? rgba.y : c == 2u ? rgba.z : rgba.w);
    gl_Position = u_proj * vec4(pos, 0.0, 1.0);
}
