// Shared by the level shaders (room.frag, decal.frag, light.frag): the
// camera image's encoding, the palette lookups and the noise.
//
// The camera image (1400x800, top row first) encodes per pixel: R = 1..90
// shaded / 91..180 sunlit, each three facings (palette colours) of 30
// depths; G = flags (16: no floor darkening, 8: coloured prop, 4: grime,
// 1/2: effect colour A/B, 3: swarm); B = the effect amount or the prop's
// colour column. White is open sky.
//
// Palettes are the 32x16 files, top row first: rows 0..7 the palette, rows
// 8..15 its rain variant. Vanilla's 8 row paletteTexture (y from the bottom)
// is row 7 - y of either half.

const vec2 level_size = vec2(1400.0, 800.0);

// vanilla's _NoiseTex is replaced by hashed value noise
uint level_hash_u(uvec2 p) {
    uint h = p.x * 0x8da6b343u ^ p.y * 0xd8163841u;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    h *= 0x297a2d39u;
    h ^= h >> 15;
    return h;
}
float level_hash(vec2 cell) {
    return float(level_hash_u(uvec2(ivec2(cell) + ivec2(1 << 20))) & 0xFFFFFFu) / 16777215.0;
}
// smooth value noise in [0, 1], `cells` cells per unit, tiling every 4096 units
float level_noise(vec2 p, float cells) {
    p = mod(p, 4096.0) * cells;
    vec2 i = floor(p);
    vec2 f = p - i;
    f = f * f * (3.0 - 2.0 * f);
    float a = level_hash(i);
    float b = level_hash(i + vec2(1.0, 0.0));
    float c = level_hash(i + vec2(0.0, 1.0));
    float d = level_hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
// what vanilla's tex2D(_NoiseTex, uv).x is used as: a soft cloudy field
float level_tex_noise(vec2 uv) {
    return level_noise(uv, 6.0) * 0.65 + level_noise(uv + vec2(17.3, 5.1), 19.0) * 0.35;
}

// the level's red channel as vanilla reads it
struct level_px {
    bool sky;
    bool sunlit;        // red > 90
    int palette_color;  // 0..2, the facing
    int depth;          // 0..29
    int green;          // flags, 16 and 8 taken out
    bool floor_dark;    // !(green & 16): notFloorDark
    bool prop;          // coloured prop (green & 8)
};

level_px level_decode(ivec3 t) {
    level_px p;
    p.sky = t == ivec3(255);
    int red = t.r;
    p.sunlit = red > 90;
    if (p.sunlit)
        red -= 90;
    p.palette_color = clamp(max(red - 1, 0) / 30, 0, 2);
    p.depth = max(red - 1, 0) % 30;
    int g = t.g;
    p.floor_dark = true;
    if (g >= 16) {
        p.floor_dark = false;
        g -= 16;
    }
    p.prop = g >= 8;
    if (p.prop)
        g -= 8;
    p.green = g;
    return p;
}

// vanilla paletteTexture.GetPixel(x, y) (y 0..7 from the bottom) of the
// palette or its rain half
vec3 level_pal(sampler2DArray pal, int layer, int x, int y, bool rain) {
    return texelFetch(pal, ivec3(clamp(x, 0, 31), (7 - y) + (rain ? 8 : 0), layer), 0).rgb;
}

// vanilla's _fogAmount (RoomCamera.ApplyPalette)
float level_fog_amount(sampler2DArray pal, int layer, bool rain) {
    vec3 p = level_pal(pal, layer, 9, 7, rain);
    if (p.r == 0.0 && p.g == 0.0 && p.b > 0.0)
        return 1.0 + p.b;
    return 1.0 - p.r;
}

// vanilla's cloud shadow over the sunlit pixels: 0 lit, 1 shaded. `uv`
// is the camera image's, y from the bottom (Unity's)
float level_cloud_shadow(vec2 uv, int raw_red, float rain_clock, float clouds_speed, float light) {
    float r30 = float(raw_red % 30);
    float s = level_tex_noise(vec2(uv.x * 0.5 + rain_clock * 0.1 * clouds_speed - 0.003 * r30,
                                   1.0 - uv.y * 0.5 + rain_clock * 0.2 * clouds_speed - 0.003 * r30));
    s = 0.5 + sin(fract(s + rain_clock * 0.1 * clouds_speed - uv.y) * 3.14 * 2.0) * 0.5;
    return clamp((s - 0.5) * 6.0 + 0.5 - light * 4.0, 0.0, 1.0);
}

// is a sprite at this point of the frame? (the target's alpha, grabbed
// after the sprites; render.cpp's sprite mask)
bool level_sprite_at(sampler2D mask, vec2 frame_uv) {
    if (frame_uv.x < 0.0 || frame_uv.y < 0.0 || frame_uv.x > 1.0 || frame_uv.y > 1.0)
        return false;
    return texture(mask, frame_uv).a > 0.5 / 255.0;
}

// LevelColor/Decal's sprite shadows: is the sun at this sunlit pixel of
// `depth` (>= 5) blocked by a sprite? `cam_uv` is the pixel in its camera
// image (y from the bottom), the camera at `cam_origin` (room pixels); the
// frame shows the room from `view_pos` at `zoom` over `screen` pixels.
// The sun comes along room.lightAngle, a pixel per depth
bool level_sprite_shadow(sampler2D mask, vec2 cam_uv, int depth, vec2 light_dir, vec2 cam_origin, vec2 view_pos, float zoom, vec2 screen) {
    float k = float(depth - 5);
    // FixEdgeShadowStretch (the screen as the camera image, y from the top)
    vec2 sp = vec2(cam_uv.x, 1.0 - cam_uv.y);
    vec4 fix = vec4(clamp(sp / 0.3, 0.0, 1.0), clamp(sp / 0.1, 0.0, 1.0));
    vec2 g = vec2(cam_uv.x - light_dir.x * k / level_size.x * fix.x,
                  cam_uv.y + light_dir.y * k / level_size.y * fix.y);
    g = mix(g, (g - vec2(0.5, 0.3)) * (1.0 + k / 460.0) + vec2(0.5, 0.3), fix.zw);
    vec2 room = cam_origin + g * level_size;
    return level_sprite_at(mask, (room - view_pos) * zoom / screen);
}
