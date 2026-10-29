// Shared helpers of the warp shaders (prepended to warp_tear.frag and
// warp_screen.frag): hashed value noise and a few octaves of it

// smoothstep that also takes falling (edge0 > edge1) and empty ranges,
// which GLSL's own leaves undefined
float warp_step(float e0, float e1, float x) {
    float w = e1 - e0;
    if (abs(w) < 1e-6)
        return x < e0 ? 0.0 : 1.0;
    float t = clamp((x - e0) / w, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

float warp_hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float warp_noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = warp_hash(i);
    float b = warp_hash(i + vec2(1.0, 0.0));
    float c = warp_hash(i + vec2(0.0, 1.0));
    float d = warp_hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float warp_fbm(vec2 p) {
    float v = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 5; ++i) {
        v += amp * warp_noise(p);
        p = p * 2.03 + vec2(17.1, 9.4);
        amp *= 0.5;
    }
    return v;
}

// thin bright lines where the noise crosses its middle: branching veins
float warp_ridges(vec2 p) {
    float n = warp_fbm(p);
    return 1.0 - abs(n * 2.0 - 1.0);
}

vec2 warp_rotate(vec2 p, float a) {
    float c = cos(a);
    float s = sin(a);
    return vec2(c * p.x - s * p.y, s * p.x + c * p.y);
}

float warp_luma(vec3 c) {
    return max(c.r, max(c.g, c.b));
}

// palette of the far side: 0 normal, 1 ripple layer, 2 bad warp, 3 outer rim
vec3 warp_far_side(float variant, float n) {
    vec3 a = vec3(0.04, 0.10, 0.42);
    vec3 b = vec3(0.30, 0.12, 0.62);
    if (variant > 0.5 && variant < 1.5) {
        a = vec3(0.30, 0.26, 0.80);
        b = vec3(0.95, 0.70, 0.25);
    } else if (variant > 1.5 && variant < 2.5) {
        a = vec3(0.16, 0.02, 0.28);
        b = vec3(0.55, 0.05, 0.20);
    } else if (variant > 2.5) {
        a = vec3(0.18, 0.22, 0.20);
        b = vec3(0.05, 0.35, 0.18);
    }
    return mix(a, b, warp_step(0.35, 0.75, n));
}
