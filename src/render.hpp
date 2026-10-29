#pragma once

// Renders rooms with anthiel's level shader (camera images coloured through
// a palette texture array and effectcolors.png), slugcats with DressMySlugcat
// style sprites, an optional collision geometry overlay and the VGA font HUD.
// OpenGL 3.3 (GLEW) in an SDL3 window.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

struct SDL_Window;
struct SDL_GLContextState;

namespace render {
    struct camera {
        rw::vec2 pos{0.f, 0.f}; // bottom left corner of the view, room pixels
        float zoom = 1.f;
    };

    struct vertex {
        float x, y;
        uint32_t rgba; // little endian 0xAABBGGRR
        float u, v;
    };

    // Rain World constants: camera images and palettes
    inline constexpr int camera_width = 1400;
    inline constexpr int camera_height = 800;
    inline constexpr int palette_width = 32;
    inline constexpr int palette_height = 16;
    inline constexpr int max_lights = 16;

    struct atlas_frame {
        float x = 0.f, y = 0.f, w = 0.f, h = 0.f; // pixels, top left origin
    };

    struct room_shader_uniforms {
        int32_t proj = -1, palette = -1, camera_index = -1, night_time = -1, fog_factor = -1;
        int32_t effect_colors = -1, grime = -1, rooftop_blend = -1, timer = -1;
        int32_t lighting_data = -1, lighting_pos = -1, replace_a = -1, replace_b = -1;
        int32_t pass = -1;
    };

    struct state {
        SDL_Window* window = nullptr;
        SDL_GLContextState* context = nullptr;
        int width = 0;
        int height = 0;
        bool show_cameras = true;
        bool show_hud = true;
        bool show_geometry = false;
        float night_time = 0.f;
        uint32_t program = 0;
        int32_t u_proj = -1;
        int32_t u_textured = -1;
        uint32_t font_texture = 0;
        int font_width = 1;
        int font_height = 1;
        // room rendering (anthiel's level shader)
        uint32_t room_program = 0;
        room_shader_uniforms room_uniforms;
        uint32_t palette_texarray = 0;
        std::vector<uint8_t> palette_pixels; // CPU copy, for the sky colour
        uint32_t effects_texture = 0;
        uint32_t room_texture = 0; // camera images of room_texture_room
        uint32_t room_texture_layers = 0;
        dcon::room_id room_texture_room;
        // slugcat sprites (DressMySlugcat style atlas)
        uint32_t slugcat_texture = 0;
        int slugcat_width = 1;
        int slugcat_height = 1;
        std::unordered_map<std::string, atlas_frame> slugcat_frames;
        // warp effects (assets/shaders/warp_*): the tears and the full screen
        // transition, both reading a copy of the frame drawn so far
        uint32_t warp_tear_program = 0;
        uint32_t warp_screen_program = 0;
        uint32_t scene_texture = 0;
        // decals (decals/<name>.png, loaded on first use) and the water body
        uint32_t decal_program = 0;
        uint32_t water_program = 0;
        struct decal_texture {
            uint32_t texture = 0; // 0: missing
        };
        std::unordered_map<std::string, decal_texture> decal_textures;
        int scene_width = 0;
        int scene_height = 0;
        uint32_t vao = 0;
        uint32_t vbo = 0;
        std::vector<vertex> batch;
    };

    // `hidden` creates an invisible window (headless screenshots)
    bool init(state& r, int width, int height, char const* title, bool hidden);
    // HUD font, room shader, palettes and the slugcat atlas (after world::load)
    void load_assets(state& r, sys::game_state const& s);
    // Makes the camera images of `rid` resident (drops the previous room's)
    void use_room(state& r, sys::game_state const& s, dcon::room_id rid);
    void shutdown(state& r);

    // `alpha` interpolates chunk positions between the last two ticks
    void draw_frame(state& r, sys::game_state const& s, world::room_view const& room, float alpha, camera const& cam, std::string const& hud);
    // Multi line text panel in the top left corner, drawn with the VGA font
    void draw_hud(state& r, std::string const& hud);
    void present(state& r);
    void set_title(state& r, std::string const& title);
    // Saves the current back buffer as a 24 bit BMP
    bool save_screenshot(state& r, char const* path);

    rw::vec2 screen_to_room(state const& r, camera const& cam, float sx, float sy);
}
