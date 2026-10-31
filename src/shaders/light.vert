#version 450
// A LightSource: a quad around it in room space, the circle's coordinates
// (0..1, the centre at 0.5) in l_uv
layout (location = 0) in vec2 l_position;
layout (location = 2) in vec2 l_uv;

layout(location = 0) out vec2 v_room;
layout(location = 1) out vec2 v_uv;

// (the start of light.frag's block)
layout(set = 0, binding = 0, std140) uniform light_uniforms {
    mat4 u_proj;
};

void main() {
    gl_Position = u_proj * vec4(l_position, 0.0, 1.0);
    v_room = l_position;
    v_uv = l_uv;
}
