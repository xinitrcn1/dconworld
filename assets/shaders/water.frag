// The body of the water, below its front edge: the scene behind is bent by
// a slowly churning current, lit by drifting caustics, and loses the light
// the deeper it is, sinking into the palette's deep water colour.
//
// Everything is built from hashed value noise, domain warped and drifting,
// so the pattern never visibly repeats; the shimmer and caustics weaken with
// depth but never fade out, down to the floor of the deepest pools.

in vec2 v_local;          // x: room x, y: depth below the surface (pixels)
out vec4 frag_color;

uniform sampler2D u_scene;
uniform vec2 u_screen;
uniform float u_time;     // seconds
uniform vec3 u_shallow;   // RoomPalette.waterColor1
uniform vec3 u_deep;      // RoomPalette.waterColor2
uniform vec3 u_shine;     // RoomPalette.waterShineColor
uniform float u_zoom;

// integer hash (no visible lattice, unlike the sin() hashes)
float hash13(vec3 p) {
    uvec3 q = uvec3(ivec3(floor(p))) * uvec3(1597334673u, 3812015801u, 2798796415u);
    uint n = (q.x ^ q.y ^ q.z) * 1597334673u;
    n ^= n >> 15;
    n *= 2246822519u;
    n ^= n >> 13;
    return float(n) * (1.0 / 4294967295.0);
}

// value noise over (x, y, time): the time axis makes it evolve instead of scrolling
float vnoise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    vec3 u = f * f * (3.0 - 2.0 * f);
    float n000 = hash13(i);
    float n100 = hash13(i + vec3(1.0, 0.0, 0.0));
    float n010 = hash13(i + vec3(0.0, 1.0, 0.0));
    float n110 = hash13(i + vec3(1.0, 1.0, 0.0));
    float n001 = hash13(i + vec3(0.0, 0.0, 1.0));
    float n101 = hash13(i + vec3(1.0, 0.0, 1.0));
    float n011 = hash13(i + vec3(0.0, 1.0, 1.0));
    float n111 = hash13(i + vec3(1.0, 1.0, 1.0));
    return mix(mix(mix(n000, n100, u.x), mix(n010, n110, u.x), u.y),
               mix(mix(n001, n101, u.x), mix(n011, n111, u.x), u.y), u.z);
}

float fbm(vec3 p) {
    float v = 0.0;
    float a = 0.5;
    // rotate each octave so their grids never line up
    mat2 rot = mat2(0.8, 0.6, -0.6, 0.8);
    for (int i = 0; i < 4; ++i) {
        v += a * vnoise(p);
        p.xy = rot * p.xy * 2.03 + vec2(11.7, 3.1);
        p.z *= 1.37;
        a *= 0.5;
    }
    return v;
}

void main() {
    float depth = max(v_local.y, 0.0);
    vec2 world = vec2(v_local.x, -depth);
    float t = u_time;

    // a slow current warping the noise domain
    vec2 q = world * 0.012;
    vec2 warp = vec2(fbm(vec3(q, t * 0.11)), fbm(vec3(q + vec2(5.2, 1.3), t * 0.13))) - 0.5;
    vec2 p = q + warp * 1.6;

    // refraction: strong under the surface, settling to a gentle shimmer at depth
    float near = exp(-depth / 160.0);
    float strength = mix(0.35, 1.0, near);
    vec2 bend = vec2(fbm(vec3(p * 2.0, t * 0.35)), fbm(vec3(p * 2.0 + vec2(9.1, 4.7), t * 0.33))) - 0.5;
    vec2 offset = bend * 7.0 * strength * u_zoom;
    vec2 uv = clamp((gl_FragCoord.xy + offset) / u_screen, vec2(0.001), vec2(0.999));
    vec3 scene = texture(u_scene, uv).rgb;

    // less light the deeper: the sun only reaches so far
    float murk = 1.0 - exp(-depth / 650.0);
    float light = mix(0.8, 0.52, murk);
    vec3 tint = mix(u_shallow, u_deep, murk);
    vec3 col = scene * light * mix(vec3(1.0), tint * 1.5, 0.45);
    col = mix(col, tint * light, 0.15 + 0.3 * murk);

    // caustics: bright creases of warped noise, dimmer but present all the way down
    float c = fbm(vec3(p * 4.5, t * 0.5));
    float crease = 1.0 - abs(c * 2.0 - 1.0);
    float caustic = pow(crease, 9.0) * mix(0.45, 1.0, near);
    col += u_shine * caustic * 0.35 * light;

    float alpha = mix(0.62, 0.86, murk);
    frag_color = vec4(col, alpha);
}
