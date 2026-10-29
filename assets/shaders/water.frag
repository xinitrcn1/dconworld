// The body of the water, below its front edge: the scene behind is bent by
// the surface's waves and shimmer, lit by moving caustic bands near the top,
// and sinks into the palette's deep water colour further down.

in vec2 v_local;          // x: room x, y: depth below the surface (pixels)
out vec4 frag_color;

uniform sampler2D u_scene;
uniform vec2 u_screen;
uniform float u_time;     // seconds
uniform vec3 u_shallow;   // RoomPalette.waterColor1
uniform vec3 u_deep;      // RoomPalette.waterColor2
uniform vec3 u_shine;     // RoomPalette.waterShineColor
uniform float u_zoom;

float wh(vec2 p) {
    p = fract(p * vec2(234.34, 435.345));
    p += dot(p, p + 34.23);
    return fract(p.x * p.y);
}
float wnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(wh(i), wh(i + vec2(1.0, 0.0)), u.x), mix(wh(i + vec2(0.0, 1.0)), wh(i + vec2(1.0, 1.0)), u.x), u.y);
}

void main() {
    float depth = max(v_local.y, 0.0);
    float x = v_local.x;
    float t = u_time;
    // refraction: the bending is strongest just under the surface and settles deeper down
    float near = exp(-depth / 140.0);
    vec2 wobble = vec2(
        sin(x * 0.045 + t * 1.7 + depth * 0.03) + 0.6 * sin(x * 0.11 - t * 2.3),
        cos(x * 0.05 - t * 1.3 + depth * 0.02));
    wobble += (vec2(wnoise(vec2(x, depth) * 0.04 + t * 0.4), wnoise(vec2(depth, x) * 0.04 - t * 0.35)) - 0.5) * 2.0;
    vec2 offset = wobble * (1.0 + 3.0 * near) * u_zoom;
    vec2 uv = (gl_FragCoord.xy + offset) / u_screen;
    uv = clamp(uv, vec2(0.001), vec2(0.999));
    vec3 scene = texture(u_scene, uv).rgb;

    // the deeper, the more of the water's own colour
    float murk = 1.0 - exp(-depth / 260.0);
    vec3 tint = mix(u_shallow, u_deep, murk);
    vec3 col = mix(scene * mix(vec3(1.0), tint * 1.4, 0.45), tint, 0.25 + 0.55 * murk);

    // caustics: two crossing sets of bands, sharpened, fading with depth
    float c1 = sin(x * 0.06 + depth * 0.025 + t * 1.1 + 2.0 * wnoise(vec2(x * 0.02, t * 0.3)));
    float c2 = sin(x * 0.041 - depth * 0.033 - t * 0.8);
    float caustic = pow(max(0.0, 1.0 - abs(c1 + c2) * 0.6), 6.0) * near;
    col += u_shine * caustic * 0.35;

    float alpha = mix(0.55, 0.93, murk);
    frag_color = vec4(col, alpha);
}
