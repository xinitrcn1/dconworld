#version 450
// CustomDecal mesh: room space positions, the decal image's coordinates
// and the opacity blended between its corners
layout (location = 0) in vec2 l_position;
layout (location = 1) in vec4 l_color; // a: opacity
layout (location = 2) in vec2 l_uv;

layout(location = 0) out vec2 v_room;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out float v_opacity;

// (the start of decal.frag's block)
layout(set = 0, binding = 0, std140) uniform decal_uniforms {
    mat4 u_proj;
};

void main() {
    gl_Position = u_proj * vec4(l_position, 0.0, 1.0);
    v_room = l_position;
    v_uv = l_uv;
    v_opacity = l_color.a;
}
