#version 450
// The rope pipeline (rope.vert/frag): ropes drawn from their points alone,
// pulled out of the storage buffer by gl_VertexIndex, no vertex input. The
// points of the ropes drawn together lie one after the other, each rope
// between two padding points (half width < 0): the point before its first
// and the one after its last, there for the ends' directions. Segment k of
// the draw runs from point first + k + 1 to first + k + 2, with the points
// on either side as its neighbours; one with a padding end (a rope's
// outside, or between two ropes) comes out degenerate. A segment is
// u_steps quads along it, each two triangles (6 vertices); u goes along the
// segment, the side across it.
//
// The curve: the cubic Bezier between the segment's ends with handles a
// third of its length along the directions through the neighbours (like
// RopeGraphic's Bezier between its claimed segments), or the straight
// line; width and colour go linearly between the ends. The ends' sides come
// from those same directions, which the neighbouring segments share, so
// the segments meet without gaps
struct rope_point {
    vec2 pos;
    float rad; // half width, room pixels (< 0: padding)
    uint rgba; // 0xAABBGGRR
};
layout(set = 0, binding = 4, std430) readonly buffer rope_points {
    rope_point points[];
};
layout(set = 0, binding = 0, std140) uniform rope_uniforms {
    mat4 u_proj;
    uint u_first;   // the draw's first point
    uint u_steps;   // quads per segment
    float u_smooth; // 0: straight segments, 1: Bezier curves through the points
};

layout(location = 0) out vec4 v_color;

vec2 dir_or(vec2 a, vec2 b, vec2 fallback) {
    vec2 d = b - a;
    float l = length(d);
    return l > 1e-5 ? d / l : fallback;
}

// a quad's corners: (steps along, side), triangles (0, 1, 2) and (0, 2, 3)
const vec2 corners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));

void main() {
    uint steps = max(u_steps, 1u);
    uint id = uint(gl_VertexIndex);
    uint seg = id / (steps * 6u);
    uint within = id % (steps * 6u);
    vec2 corner = corners[within % 6u];
    float u = (float(within / 6u) + corner.x) / float(steps);
    float side = corner.y * 2.0 - 1.0;

    uint base = u_first + seg;
    rope_point q0 = points[base], q1 = points[base + 1u], q2 = points[base + 2u], q3 = points[base + 3u];
    if (q1.rad < 0.0 || q2.rad < 0.0) {
        // (a padding end: nothing, every corner the same point)
        v_color = vec4(0.0);
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    vec2 p0 = q0.pos, p1 = q1.pos, p2 = q2.pos, p3 = q3.pos;
    float d = distance(p1, p2);
    vec2 chord = dir_or(p1, p2, dir_or(p0, p3, vec2(0.0, 1.0)));
    vec2 t1 = dir_or(p0, p2, chord);
    vec2 t2 = dir_or(p1, p3, chord);
    vec2 pos, dir;
    if (u_smooth > 0.5) {
        vec2 b1 = p1 + t1 * (d / 3.0);
        vec2 b2 = p2 - t2 * (d / 3.0);
        float s = 1.0 - u;
        pos = s * s * s * p1 + 3.0 * s * s * u * b1 + 3.0 * s * u * u * b2 + u * u * u * p2;
        vec2 tangent = 3.0 * s * s * (b1 - p1) + 6.0 * s * u * (b2 - b1) + 3.0 * u * u * (p2 - b2);
        float tl = length(tangent);
        dir = tl > 1e-5 ? tangent / tl : dir_or(vec2(0.0), mix(t1, t2, u), chord);
    } else {
        pos = mix(p1, p2, u);
        dir = dir_or(vec2(0.0), mix(t1, t2, u), chord);
    }
    // the ends exactly as the neighbouring segments have them
    if (u <= 0.0) {
        pos = p1;
        dir = t1;
    } else if (u >= 1.0) {
        pos = p2;
        dir = t2;
    }
    pos += vec2(-dir.y, dir.x) * mix(q1.rad, q2.rad, u) * side;
    v_color = mix(unpackUnorm4x8(q1.rgba), unpackUnorm4x8(q2.rgba), u);
    gl_Position = u_proj * vec4(pos, 0.0, 1.0);
}
