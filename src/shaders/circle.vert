#version 450
// The circle pipeline (circle.vert/frag): a disc as one quad, pulled out of
// the storage buffer by gl_VertexIndex (no vertex input), six vertices per
// disc making its two triangles; the fragment shader keeps what is inside
// the outline
struct circle {
    vec2 pos;       // centre, room pixels
    float rad;      // radius, room pixels
    uint rgba;      // 0xAABBGGRR
    float rotation; // degrees, clockwise from up (Custom.DegToVec)
    float jag;      // 0: round, else how deep the rim's dents go (a fraction of the radius)
    uint seed;      // the dents' rolls
    float pad;
};
layout(set = 0, binding = 5, std430) readonly buffer circles_buffer {
    circle circles[];
};
layout(set = 0, binding = 0, std140) uniform circle_uniforms {
    mat4 u_proj;
    uint u_first; // the draw's first circle
};

layout(location = 0) out vec2 v_local; // room pixels from the centre
layout(location = 1) flat out vec4 v_color;
layout(location = 2) flat out vec3 v_shape; // radius, rotation, jag
layout(location = 3) flat out uint v_seed;

const vec2 corners[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

void main() {
    uint id = uint(gl_VertexIndex);
    circle c = circles[u_first + id / 6u];
    // (a jagged rim never reaches past the radius)
    v_local = corners[id % 6u] * c.rad;
    v_color = unpackUnorm4x8(c.rgba);
    v_shape = vec3(c.rad, c.rotation, c.jag);
    v_seed = c.seed;
    gl_Position = u_proj * vec4(c.pos + v_local, 0.0, 1.0);
}
