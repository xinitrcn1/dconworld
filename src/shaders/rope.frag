#version 450
// The rope's colour, flat (no texture)
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 frag_color;

void main() {
    frag_color = v_color;
}
