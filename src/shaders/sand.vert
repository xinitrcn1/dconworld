#version 450
// Room space geometry for the level-lit effects (sand.frag, sandstorm.frag,
// spores.frag): the position, the vertex colour and the coordinates each
// shader reads its own way
layout (location = 0) in vec2 l_position;
layout (location = 1) in vec4 l_color;
layout (location = 2) in vec2 l_uv;

layout(location = 0) out vec2 v_room;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out vec4 v_color;

// (the start of each fragment shader's block)
layout(set = 0, binding = 0, std140) uniform effect_uniforms {
    mat4 u_proj;
};

void main() {
    gl_Position = u_proj * vec4(l_position, 0.0, 1.0);
    v_room = l_position;
    v_uv = l_uv;
    v_color = l_color;
}
