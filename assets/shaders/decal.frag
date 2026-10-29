// A decal painted onto the level: only over the level pixels whose depth is
// within the decal's range, fogged with the room's palette like the level
// itself. Drawn once per room camera image it overlaps, split like the level
// into the pass behind the sprites (depths 5+) and the one in front.

in vec2 v_room;
in vec2 v_uv;
out vec4 o_color;

uniform sampler2DArray u_level;    // the room's camera images
uniform sampler2DArray u_palette;
uniform sampler2D u_decal;
uniform int u_index;               // palette layer
uniform int u_camera_index;
uniform vec2 u_cam_origin;         // the camera image's bottom left, room pixels
uniform float u_from;              // depth range, 0..1 of the 30 depths
uniform float u_to;
uniform int u_pass;

void main() {
    vec2 luv = vec2((v_room.x - u_cam_origin.x) / 1400.0, 1.0 - (v_room.y - u_cam_origin.y) / 800.0);
    if (luv.x < 0.0 || luv.y < 0.0 || luv.x > 1.0 || luv.y > 1.0)
        discard;
    ivec3 level = ivec3(texture(u_level, vec3(luv, float(u_camera_index))).rgb * 255.0 + 0.5);
    // nothing to paint on where the level is open sky
    if (level == ivec3(255) || level.r == 0)
        discard;
    int depth = (level.r - 1) % 30;
    float d = float(depth) / 30.0;
    if (d < u_from - 0.001 || d > u_to + 0.001)
        discard;
    bool front = depth < 5;
    if ((u_pass == 0) == front)
        discard;
    vec4 c = texture(u_decal, vec2(v_uv.x, 1.0 - v_uv.y));
    if (c.a <= 0.003)
        discard;
    vec3 fog = texelFetch(u_palette, ivec3(1, 0, u_index), 0).rgb;
    float fog_amount = texelFetch(u_palette, ivec3(9, 0, u_index), 0).r;
    // deeper paint sinks into the haze, and a little into the shade
    vec3 col = c.rgb * mix(1.0, 0.75, d);
    col = mix(col, fog, (1.0 - fog_amount) * d);
    o_color = vec4(col, c.a);
}
