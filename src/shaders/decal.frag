// A decal painted onto the level: only over the level pixels whose depth is
// within the decal's range, fogged with the room's palette like the level
// itself; out of the sun it sinks into the surface, fainter. Drawn once per room camera image it overlaps, split like the level
// into the pass behind the sprites (depths 5+) and the one in front.

#version 450

layout(location = 0) in vec2 v_room;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in float v_opacity;
layout(location = 0) out vec4 o_color;

// render.cpp's decal_uniforms
layout(set = 0, binding = 0, std140) uniform decal_uniforms {
    mat4 u_proj;
    vec2 u_cam_origin;             // the camera image's bottom left, room pixels
    float u_from;                  // depth range, 0..1 of the 30 depths
    float u_to;
    int u_index;                   // palette layer
    int u_camera_index;
    int u_pass;
};
layout(set = 0, binding = 1) uniform sampler2DArray u_level;    // the room's camera images
layout(set = 0, binding = 2) uniform sampler2DArray u_palette;
layout(set = 0, binding = 3) uniform sampler2D u_decal;

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
    // the level's own colour under the paint (its depth and facing in the palette)
    bool sunlit = level.r > 90;
    int facing = ((level.r - 1) / 30) % 3;
    vec3 shade = texelFetch(u_palette, ivec3(depth, abs(facing - 7), u_index), 0).rgb;
    // in the shade the paint is mostly multiplied into the surface and much
    // fainter; in the sun it shows its own colour
    vec3 col = sunlit ? c.rgb : mix(c.rgb * shade * 1.6, shade, 0.35);
    float a = c.a * v_opacity * (sunlit ? 1.0 : 0.25 + 0.25 * v_opacity);
    col *= mix(1.0, 0.8, d);
    col = mix(col, fog, (1.0 - fog_amount) * d);
    o_color = vec4(col, a);
}
