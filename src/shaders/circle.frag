#version 450
// A disc, hard edged like the flat triangles: round, or with `jag` its
// rim a polygon of 18 corners at radius * (1 - jag + jag * roll), the rolls
// an xorshift32 from the seed, corner k at Custom.DegToVec(rotation +
// k * 20): the fan of triangles the daddy long legs' JaggedCircle bulbs were
layout(location = 0) in vec2 v_local;
layout(location = 1) flat in vec4 v_color;
layout(location = 2) flat in vec3 v_shape;
layout(location = 3) flat in uint v_seed;

layout(location = 0) out vec4 frag_color;

const int corners = 18;

float cross2(vec2 a, vec2 b) {
    return a.x * b.y - a.y * b.x;
}

// Custom.DegToVec
vec2 deg_to_vec(float deg) {
    float a = radians(deg);
    return vec2(sin(a), cos(a));
}

void main() {
    float rad = v_shape.x, rotation = v_shape.y, jag = v_shape.z;
    if (jag <= 0.0) {
        if (dot(v_local, v_local) > rad * rad)
            discard;
    } else {
        // the corner before this point's angle, counted clockwise from up
        float ang = mod(degrees(atan(v_local.x, v_local.y)) - rotation, 360.0);
        int k = clamp(int(floor(ang * float(corners) / 360.0)), 0, corners - 1);
        uint h = v_seed | 1u;
        float r0 = 0.0, rk = 0.0, rk1 = 0.0;
        for (int i = 0; i <= k + 1 && i < corners; ++i) {
            h ^= h << 13;
            h ^= h >> 17;
            h ^= h << 5;
            float r = rad * (1.0 - jag + jag * float(h & 0xFFFFu) / 65535.0);
            if (i == 0)
                r0 = r;
            if (i == k)
                rk = r;
            if (i == k + 1)
                rk1 = r;
        }
        if (k == corners - 1)
            rk1 = r0;
        vec2 a = deg_to_vec(rotation + float(k) * 360.0 / float(corners)) * rk;
        vec2 b = deg_to_vec(rotation + float(k + 1) * 360.0 / float(corners)) * rk1;
        // in the fan's triangle (centre, a, b): on the centre's side of ab
        if (cross2(b - a, v_local - a) * cross2(b - a, -a) < 0.0)
            discard;
    }
    frag_color = v_color;
}
