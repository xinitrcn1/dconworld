#version 450
// Warp effects (warp_tear.frag, warp_screen.frag) and the water body
// (water.frag): a quad in room space, a_uv carrying its local coordinates
// (-1..1 across the tear)
layout (location = 0) in vec2 l_position;
layout (location = 2) in vec2 l_local;

layout(location = 0) out vec2 v_local;

// (the start of every fragment shader's block)
layout(set = 0, binding = 0, std140) uniform warp_uniforms {
    mat4 u_proj;
};

void main() {
    gl_Position = u_proj * vec4(l_position, 0.0, 1.0);
    v_local = l_local;
}
