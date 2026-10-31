#version 450
// Room camera image ("<room>_<n>.png", 1400x800) placed in room space.
// From anthiel's nv_shaders/room.vert, projected with the room camera matrix.
layout (location = 0) in vec2 l_position; // room pixels
layout (location = 2) in vec2 l_texcoord; // shares the debug vertex layout

layout(location = 0) out vec2 v_texcoord;

// (the start of room.frag's block)
layout(set = 0, binding = 0, std140) uniform room_uniforms {
    mat4 u_proj;
};

void main() {
    gl_Position = u_proj * vec4(l_position, 0.0, 1.0);
    v_texcoord = l_texcoord;
}
