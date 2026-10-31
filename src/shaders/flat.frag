#version 450
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;

layout(set = 0, binding = 0, std140) uniform flat_uniforms {
    mat4 u_proj;
    int u_textured;
};
layout(set = 0, binding = 1) uniform sampler2D u_texture;

layout(location = 0) out vec4 frag_color;

void main() {
    frag_color = u_textured != 0 ? v_color * texture(u_texture, v_uv) : v_color;
}
