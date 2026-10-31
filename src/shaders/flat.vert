#version 450
// The flat shader: coloured (optionally textured) triangles, in room pixels
// or screen pixels depending on u_proj
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_uv;

layout(set = 0, binding = 0, std140) uniform flat_uniforms {
    mat4 u_proj;
    int u_textured;
};

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;

void main() {
    v_color = a_color;
    v_uv = a_uv;
    gl_Position = u_proj * vec4(a_pos, 0.0, 1.0);
}
