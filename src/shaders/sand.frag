// The Watcher's sand: the colour model of vanilla's SlopedTerrainSurface
// shader on rwcpp's 2D dunes. Its colours come from the room's terrain
// palette (sunlit and ambient rows, picked by lightness), lit by the sun
// along the room's lightAngle (dimmed with the clouds and the coming rain
// like the level, shaded by the clouds and the sprites' shadows, and only
// where the level behind it is sunlit: vanilla's level editor lightmap),
// fogged into the distance, and lit by the room's LightSources. Drawn once
// per camera image it overlaps, after the sprites: sprites stay in front of
// it, nearer level geometry hides it.
//
// Mud (MudPit, u_mud.a 1): the same light on the mud pits, their colour the
// region's mudColor shaded towards the palette's black (MudPit.ApplyPalette)
// instead of the terrain palette's, no grain, and in front of the sprites
// that sank into it (it isn't kept behind them like the sand).
//
// The dunes: v_uv.x is vanilla's clr.r, below 0 the pixels down the front
// face, 0..1 across the top going back; v_uv.y the depth (0..1 of the 30
// layers); v_color.rg the surface normal's x and y.
//
// The far dunes (rwcpp's, v_color.a under 1: 1 - a is how far they've sunk
// into the haze): bands of sand behind the sand's back edge, each sliding
// with the view at its own slower rate (render.cpp's draw_sand), behind all
// of the level (their depth is past the 30 layers). v_uv.x is their own x
// (their texture slides with them), v_uv.y their depth. Lit like the top,
// without the LightSources or the sprites' shadows, sinking into the fog
// and the sky.
//
// The texture (rwcpp's: vanilla's _SandNormals isn't available): wind
// ripples across the top, crowding towards the back and meandering along
// the dune, wavy strata and streaks down the front face, soft clumps and a
// fine grain everywhere, all as tilts of the normal and nudges of the
// lightness the terrain palette is read at (so its colours stay the
// palette's). TerrainWaves scales the ripples.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "level_common.glsl"

layout(location = 0) in vec2 v_room;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec4 v_color;
layout(location = 0) out vec4 o_color;

// render.cpp's sand_uniforms
layout(set = 0, binding = 0, std140) uniform sand_uniforms {
    mat4 u_proj;
    vec2 u_screen;
    vec2 u_view_pos;
    vec2 u_cam_origin;
    vec2 u_light_dir;     // room.lightAngle
    float u_zoom;
    float u_night_time;
    float u_rain;         // _RAIN
    float u_light;        // _light
    float u_clouds_speed;
    int u_index;          // palette layer
    int u_camera_index;
    int u_later_count;
    float u_terrain_light; // _terrainParams
    float u_terrain_grain;
    float u_edge_radius;
    float u_sky_fade;
    int u_light_count;
    int u_terrain_rows;   // the terrain palette's rows (0: no terrain palette)
    float u_water_level;  // room pixels
    float u_waves;        // TerrainWaves
    vec4 u_mud;           // rgb: the mud's colour, a: 1 drawing mud
    vec4 u_mud_black;     // rgb: the palette's black
    vec4 u_later[16];
    vec4 u_light_color[16];  // rgb: LightSource.color, a: its alpha
    vec4 u_light_params[16]; // x, y, radius
};
layout(set = 0, binding = 1) uniform sampler2DArray u_level;
layout(set = 0, binding = 2) uniform sampler2DArray u_palette;
layout(set = 0, binding = 3) uniform sampler2D u_terrain; // the terrain palette, then its rain variant below it
layout(set = 0, binding = 4) uniform sampler2D u_mask;

bool shown_by_later_camera(vec2 room) {
    for (int i = 0; i < u_later_count; ++i) {
        vec4 o4 = u_later[i / 2];
        vec2 o = (i % 2) == 0 ? o4.xy : o4.zw;
        vec2 d = room - o;
        if (d.x >= 0.0 && d.y >= 0.0 && d.x < level_size.x && d.y < level_size.y)
            return true;
    }
    return false;
}

vec3 pal(int x, int y) {
    return mix(level_pal(u_palette, u_index, x, y, false), level_pal(u_palette, u_index, x, y, true), u_night_time);
}

// the terrain palette at `x` (0..1, filtered) and `row_up` texels up from
// its bottom edge (Unity's v times its height)
vec3 terrain_texel(float x, float row_up) {
    float h = float(u_terrain_rows);
    float v = (h - row_up) / (2.0 * h);
    vec3 a = texture(u_terrain, vec2(x, v)).rgb;
    vec3 b = texture(u_terrain, vec2(x, v + 0.5)).rgb;
    return mix(a, b, u_night_time);
}

// PaletteSurfaceColor
vec3 surface_color(float lightness, float depth, float sunlit, level_px behind) {
    if (u_mud.a > 0.5) {
        // the mud in its colour, the shade going towards the palette's black
        vec3 shade = mix(u_mud_black.rgb, u_mud.rgb, 0.45);
        vec3 lit = u_mud.rgb * mix(1.0, 1.25, sunlit);
        return mix(shade, lit, clamp(lightness, 0.0, 1.0));
    }
    if (u_terrain_rows < 3) {
        // (no terrain palette: the level palette's lit and shaded colours)
        int d = clamp(int(depth * 30.0), 0, 29);
        vec3 shade = pal(d, 0);
        vec3 lit = pal(d, 3);
        return mix(shade, lit, clamp(lightness, 0.0, 1.0) * mix(0.6, 1.0, sunlit));
    }
    float layers = floor((float(u_terrain_rows) - 1.0) / 2.0);
    float depth_coord = (layers - 1.0) * clamp(depth, 0.0, 1.0);
    return terrain_texel(clamp(lightness, 0.0, 1.0), 1.5 + depth_coord + layers * sunlit);
}

vec3 terrain_misc(int x) {
    if (u_mud.a > 0.5)
        return x == 1 ? vec3(1.0) : vec3(0.0);
    if (u_terrain_rows < 3)
        return x == 1 ? vec3(1.0) : vec3(0.0);
    return terrain_texel((float(x) + 0.5) / 32.0, 0.5);
}

float white_noise(vec2 cell, float salt) {
    return level_hash(floor(cell) + vec2(salt * 1013.0, salt * 557.0));
}

// smooth value noise in [0, 1], one cell per unit
float vn(vec2 p) {
    return level_noise(p, 1.0);
}

void main() {
    vec2 room = v_room;
    vec2 tc = (floor(room) + 0.5 - u_cam_origin) / level_size;
    if (tc.x < 0.0 || tc.y < 0.0 || tc.x > 1.0 || tc.y > 1.0)
        discard;
    if (shown_by_later_camera(room))
        discard;
    // sprites stay in front of the sand (the mud covers what sank into it)
    vec2 frame_uv = (room - u_view_pos) * u_zoom / u_screen;
    if (u_mud.a < 0.5 && level_sprite_at(u_mask, frame_uv))
        discard;
    float e = v_uv.x;
    float dpth = v_uv.y;
    // the far dunes: their own x for the texture, read as the top's middle
    float far = u_mud.a < 0.5 ? 1.0 - v_color.a : 0.0;
    bool is_far = far > 0.002;
    vec2 pattern = is_far ? vec2(v_uv.x, room.y) : room;
    if (is_far)
        e = 0.5;
    // the level in front of it hides it
    ivec3 t = ivec3(texture(u_level, vec3(tc.x, 1.0 - tc.y, float(u_camera_index))).rgb * 255.0 + 0.5);
    level_px behind = level_decode(t);
    if (!behind.sky && t.r > 0 && float(behind.depth) / 30.0 < dpth)
        discard;

    float edge = max(u_edge_radius, 1.0);
    float noise = white_noise(pattern, 1.0);
    float noise2 = white_noise(pattern, 2.0);
    float fade = clamp(1.0 - e / -edge, 0.0, 1.0);

    // the normal: the dune's, turned towards the viewer down the front face
    vec2 n2 = v_color.rg * 2.0 - 1.0;
    vec3 normal = normalize(vec3(n2, sqrt(max(0.0, 1.0 - dot(n2, n2)))));
    if (e < 0.0)
        normal = normalize(mix(normal, vec3(0.0, 0.0, 1.0), min(-e / edge, 1.0)));
    vec3 mesh_normal = normal;
    vec3 tangent = normalize(vec3(normal.y, -normal.x, 0.0) + vec3(1e-4, 0.0, 0.0));
    vec3 bitangent = cross(normal, tangent);
    float grain_factor = clamp(dot(normalize(vec2(-u_light_dir.x, u_light_dir.y) + vec2(1e-6)), normal.xy) * (1.0 - abs(normal.z) * 0.5), 0.0, 1.0);
    grain_factor = pow(max((grain_factor - 1.0) / max(1.0 - u_terrain_light, 1e-3) + 1.0, 0.0), 0.2) * 0.8 + 0.2;
    normal = normalize(normal + (tangent * pow(noise2 * 2.0 - 1.0, 3.0) + bitangent * pow(noise * 2.0 - 1.0, 3.0)) * u_terrain_grain * grain_factor * 0.5 * (1.0 - far));

    // the sand's texture: a tilt along the bitangent (into the screen on the
    // top, up on the front face) and a nudge of the lightness
    float tone = 0.0;
    if (u_mud.a < 0.5) {
        float waves = 0.3 + 0.7 * clamp(u_waves, 0.0, 1.0);
        float bump = 0.0;
        if (is_far) {
            // the far dunes: only long streaks along them (lines across
            // would crawl, their y isn't their own)
            bump = (vn(vec2(pattern.x * 0.02, 5.3)) - 0.5) * 0.25 * waves;
        } else if (e >= 0.0) {
            // ripples: lines along the dune, meandering, the gentle windward
            // side then the steep lee, crowding and fading towards the back
            float t01 = clamp(e, 0.0, 1.0);
            float meander = (vn(vec2(pattern.x * 0.006, 3.7)) - 0.5) * 3.0 + sin(pattern.x * 0.021) * 0.35;
            float phase = (t01 + 0.8 * t01 * t01) * 16.0 + meander;
            float r = fract(phase);
            float slope = r < 0.72 ? 1.0 / 0.72 : -1.0 / 0.28;
            slope *= smoothstep(0.0, 0.08, r) * (1.0 - smoothstep(0.92, 1.0, r));
            bump = slope * 0.16 * waves * (0.6 + 0.4 * vn(pattern * 0.05)) * (1.0 - 0.6 * t01);
            tone = (r < 0.72 ? 0.02 : -0.03) * waves * (1.0 - t01);
        } else {
            // the front face: wavy strata and fine streaks of settled sand
            float y = pattern.y + (vn(vec2(pattern.x * 0.01, 1.3)) - 0.5) * 18.0;
            float st = vn(vec2(pattern.x * 0.004, y * 0.22));
            float sk = vn(vec2(pattern.x * 0.03 + y * 0.05, y * 0.6));
            bump = ((st - 0.5) * 0.35 + (sk - 0.5) * 0.15) * waves;
            tone = (st - 0.5) * 0.08;
        }
        normal = normalize(normal + bitangent * bump);
        // soft clumps and single grains
        float grain = clamp(u_terrain_grain, 0.0, 1.0) * (1.0 - far);
        tone += ((vn(pattern / 7.0) - 0.5) * 0.06 + (vn(pattern / 2.3) - 0.5) * 0.04) * (1.0 - far);
        tone += (noise - 0.5) * 0.06 * grain;
    }

    vec3 light_dir = normalize(vec3(-u_light_dir.x, u_light_dir.y, 2.0));
    float pixels_above_water = room.y - u_water_level + 100.0;
    float wetness = clamp((50.0 - pixels_above_water) / 100.0, 0.0, 1.0);

    // sunlight
    float sunlight = max(dot(light_dir, normal), 0.0);
    sunlight = sqrt(max((sunlight - 1.0) / max(1.0 - u_terrain_light, 1e-3) + 1.0, 0.0));
    // (the level editor's lightmap: where the level behind it is sunlit or open sky)
    sunlight *= (behind.sky || behind.sunlit) ? 1.0 : 0.0;
    // ObjectShadows: the clouds, the sprites
    int layer = int(dpth * 30.0 - 1.0);
    vec2 cam_uv = (room - u_view_pos) / level_size;
    float shadow = level_cloud_shadow(cam_uv, max(layer, 0), u_rain, u_clouds_speed, u_light);
    if (shadow != 1.0 && layer >= 5 && !is_far && level_sprite_shadow(u_mask, cam_uv, layer, u_light_dir, u_view_pos, u_view_pos, u_zoom, u_screen))
        shadow = 1.0;
    sunlight *= 1.0 - shadow;
    sunlight *= pow(fade, 3.0);
    sunlight *= 1.0 - wetness;

    float ambient = (max(dot(normal, mesh_normal), 0.0) + max(normal.z * normal.z, 0.0) * 0.25) * fade;
    float lightness = clamp(sunlight * 0.75 + ambient * 0.25 + tone, 0.0, 1.0);
    vec3 sun_color = surface_color(lightness, dpth, 1.0, behind);
    vec3 ambient_color = surface_color(lightness, dpth, 0.0, behind);

    vec2 scr = frame_uv - vec2(0.5);
    vec3 view_dir = normalize(vec3(scr * u_screen, -2000.0));
    float specular = pow(clamp(dot(light_dir, reflect(view_dir, normal)), 0.0, 1.0), 7.0);
    vec3 sky = pal(0, 7);
    vec3 fog = pal(1, 7);
    vec3 col = mix(ambient_color, sun_color, sunlight);
    // light filtering through water
    col = mix(col, pal(5, 7), clamp(-pixels_above_water / 10.0, 0.0, 1.0) * clamp(e, 0.0, 0.1));
    col += terrain_misc(0) * fade * clamp(noise2 * noise * 2.0 - 1.0, 0.0, 1.0) * u_terrain_grain * (specular * sunlight + ambient * 0.05);
    float fog_amount = level_fog_amount(u_palette, u_index, false);
    if (is_far) {
        // the far dunes sink into the haze, the furthest the most
        col = mix(col, mix(fog, sky, clamp(u_sky_fade, 0.0, 1.0) * 0.6), far);
    } else {
        col = mix(col, fog, clamp(e, 0.0, 1.0) * fog_amount);
        // (half of vanilla's sky fade: the far dunes go on behind the back edge)
        col = mix(col, sky, clamp(e * 3.0 - 2.0, 0.0, 1.0) * u_sky_fade * (u_mud.a > 0.5 ? 1.0 : 0.5));
    }

    // the point LightSources
    vec3 tint = terrain_misc(1);
    vec3 added = vec3(0.0);
    for (int j = 0; j < (is_far ? 0 : u_light_count); ++j) {
        vec3 incident = vec3(room, dpth) - vec3(u_light_params[j].xy, 7.0);
        float dist = max(length(incident), 1e-3);
        incident /= dist;
        float diffuse = 0.8 * max(dot(-incident, normal), 0.0);
        float spec = min(pow(clamp(dot(-incident, reflect(view_dir, normal)), 0.0, 1.0), 20.0) * 100.0, 10.0)
            * mix(0.2, clamp(noise2 * noise * 2.0 - 1.0, 0.0, 1.0), u_terrain_grain);
        float c = pow(1.0 - min(pow(dist / u_light_params[j].z, 2.0), 1.0), 3.5);
        float amount = (0.8 + diffuse) * max(c, 0.0) + spec / dist;
        amount *= clamp(1.0 + e / edge * 2.0, 0.0, 1.0);
        added += u_light_color[j].rgb * u_light_color[j].a * amount;
    }
    col += added * tint * clamp(1.0 - e, 0.0, 1.0);
    // the mud is wet: a soft shine where the sun catches it
    if (u_mud.a > 0.5)
        col += vec3(0.35) * specular * sunlight * fade;
    o_color = vec4(col, 1.0);
}
