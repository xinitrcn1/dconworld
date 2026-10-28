// Level colouring for room camera images, from anthiel's nv_shaders/room.frag.
// The camera image encodes per pixel: R = depth/facing/lighting, G = effect
// colour and flags, B = effect amount. Colours come from the room palette
// (a layer of the palette texture array) and the global effectcolors.png.
uniform sampler2DArray u_texture0; //per-room texture (an array of)
uniform sampler2DArray u_texture1; //palette texture
uniform sampler2D u_texture2; //global effectscolors.png
uniform int u_index; //what palette # in array
uniform int u_camera_index; //# camera in the 2d texture array
uniform float u_night_time; //[0,1] night time t
uniform float u_fog_factor; //[0,1] fog factor t
uniform ivec2 u_camera_view; //# camera for the camera positions
uniform ivec2 u_effect_colors; //[0,7] EffectColorA + EffectColorB
uniform float u_grime_factor; //[0,1] amount of grime
uniform float u_rooftop_blend; //[0,1] alpha of skybox
uniform float u_timer;
// Lighting (we don't prebake lets just go in raw)
uniform vec4 u_lighting_data[MAX_LIGHTS]; //vec3(rgb), a=intensity
uniform vec2 u_lighting_pos[MAX_LIGHTS]; //(x,y,z) position (euclidean distance)
// Used by regionkit regions
uniform vec4 u_replace_effect_color_a;
uniform vec4 u_replace_effect_color_b;

out vec4 o_frag_color;
in vec2 v_texcoord;

int floor_div(int a, int b) {
    return int(floor(float(a) / float(b)));
}

vec3 get_replaced_colour(ivec2 index, vec4 c) {
    bool unlit = (index.y % 2) > 0;
    return unlit ? c.rgb : c.rgb;
}

vec4 get_room_color(bool rain, ivec3 index, vec4 light_color) {
    bool grime = (((index.g >> 2) & 1) != 0);
    bool decal = (((index.g >> 3) & 1) != 0);
    bool unlit = (((index.g >> 4) & 1) != 0);
    bool lit = (index.r > 90 && !rain) && !unlit;

    int facing = floor_div(index.r - 1, 30) % 3;
    int depth = !unlit ? ((index.r - 1) % 30) : 0;

    vec3 fog_color = texelFetch(u_texture1, ivec3(1, 0, u_index), 0).rgb;
    vec3 shadow_color = texelFetch(u_texture1, ivec3(2, 0, u_index), 0).rgb;
    float fog_amount = texelFetch(u_texture1, ivec3(9, 0, u_index), 0).r;
    vec3 shortcut_1_color = texelFetch(u_texture1, ivec3(10, 0, u_index), 0).rgb;
    vec3 shortcut_2_color = texelFetch(u_texture1, ivec3(11, 0, u_index), 0).rgb;
    vec3 shortcut_3_color = texelFetch(u_texture1, ivec3(12, 0, u_index), 0).rgb;

    if (decal && index.b == 0) {
        return vec4(((index.g & 0x01) != 0) ? shortcut_1_color : shortcut_1_color, 1.f);
    }

    {
        int variant_row = depth < 10 ? 0 : 1;
        int variant_column = lit ? 0 : 1;

        // scenery - if you invert the `abs(x - y)` the glsl compiler bugs out for some reason
        int scenery_palette_row_a = abs((facing + (lit ? 3 : 0)) - (rain ? 15 : 7));
        vec3 scenery_color_a = texelFetch(u_texture1, ivec3(depth, scenery_palette_row_a, u_index), 0).rgb;
        int scenery_palette_row_b = abs(facing - (rain ? 15 : 7));
        vec3 scenery_color_b = texelFetch(u_texture1, ivec3(depth, scenery_palette_row_b, u_index), 0).rgb;
        vec3 scenery_color = mix(scenery_color_b, scenery_color_a * light_color.rgb, light_color.a);

        // blending
        vec3 blend_color_rgb;
        switch (index.g & 3) {
        case 0: {
            blend_color_rgb = scenery_color;
            break;
        }
        case 1: { //EffectColorA
            int palette_row = (rain ? 2 : 0) + variant_row;
            int palette_column = u_effect_colors[0] * 2 + variant_column;
            if (u_replace_effect_color_a != vec4(0.f)) { // regionkit+
                blend_color_rgb = get_replaced_colour(ivec2(palette_column, palette_row), u_replace_effect_color_a);
            } else {
                blend_color_rgb = texelFetch(u_texture2, ivec2(palette_column, palette_row), 0).rgb;
            }
            break;
        }
        case 2: { //EffectColorB
            int palette_row = (rain ? 2 : 0) + variant_row;
            int palette_column = u_effect_colors[1] * 2 + variant_column;
            if (u_replace_effect_color_b != vec4(0.f)) { // regionkit+
                blend_color_rgb = get_replaced_colour(ivec2(palette_column, palette_row), u_replace_effect_color_b);
            } else {
                blend_color_rgb = texelFetch(u_texture2, ivec2(palette_column, palette_row), 0).rgb;
            }
            break;
        }
        case 3:
            blend_color_rgb = vec3(1.f);
            break;
        }

        vec3 out_color_rgb;
        if (decal) {
            // pipes handled above
            out_color_rgb = texelFetch(u_texture0, ivec3(255 - index.b, 0, u_camera_index), 0).rgb;
            out_color_rgb = mix(scenery_color, out_color_rgb, 0.25f - 0.25f * float(depth) / 30.f);
        } else {
            out_color_rgb = mix(scenery_color, blend_color_rgb, float(index.b) / 255.f);
        }
        if (grime) {
            int grime_seed = int(sin((v_texcoord.x + u_timer)) * 128.f + cos((v_texcoord.y + u_timer)) * 128.f) % 30;
            vec3 grime_color = texelFetch(u_texture2, ivec2(grime_seed, 1), 0).rgb;
            out_color_rgb = mix(out_color_rgb, grime_color, u_grime_factor * (float(index.r) / 255.f));
        }
        out_color_rgb = mix(out_color_rgb, fog_color, (1.f - fog_amount) * (float(depth) / 30.f));
        return vec4(out_color_rgb, 1.f);
    }
}

void main() {
    vec3 room_color = texture(u_texture0, vec3(v_texcoord.xy, float(u_camera_index))).rgb;
    ivec3 index = ivec3(room_color * 255.0f);

    vec4 light_color = vec4(0.f);
    for (int i = 0; i < MAX_LIGHTS; ++i) {
        float light_i = u_lighting_data[i].w;
        float light_d = 1.f - distance(v_texcoord, u_lighting_pos[i].xy);
        float light_sublayer = float((index.r - 1) % 30) / 30.f;
        float amount = light_d * light_i - light_sublayer * light_i;
        light_color = mix(light_color, vec4(u_lighting_data[i].rgb, 1.f), amount);
    }
    light_color.a = mix(light_color.a, 1.f, u_rooftop_blend);

    vec4 day_color = get_room_color(false, index, light_color);
    vec4 night_color = get_room_color(true, index, light_color);
    if (index == ivec3(255, 255, 255)) {
        // sky layer
        o_frag_color = vec4(0.f);
    } else {
        // normal layer
        o_frag_color = mix(day_color, night_color, u_night_time);
    }
}
