// CustomDecal mesh: room space positions, the decal image's coordinates
layout (location = 0) in vec2 l_position;
layout (location = 2) in vec2 l_uv;

out vec2 v_room;
out vec2 v_uv;

uniform mat4 u_proj;

void main() {
    gl_Position = u_proj * vec4(l_position, 0.0, 1.0);
    v_room = l_position;
    v_uv = l_uv;
}
