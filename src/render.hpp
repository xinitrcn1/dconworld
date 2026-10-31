#pragma once

// Renders rooms with anthiel's level shader (camera images coloured through
// a palette texture array and effectcolors.png), slugcats with DressMySlugcat
// style sprites, an optional collision geometry overlay and the VGA font HUD.
// Vulkan 1.3 (through gpu.hpp) in an SDL3 window.

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "gamestate.hpp"
#include "gpu.hpp"
#include "math.hpp"
#include "world.hpp"

struct SDL_Window;

namespace render {
    struct camera {
        rw::vec2 pos{0.f, 0.f}; // bottom left corner of the view, room pixels
        float zoom = 1.f;
    };

    using vertex = gpu::vertex;

    // Rain World constants: camera images and palettes
    inline constexpr int camera_width = 1400;
    inline constexpr int camera_height = 800;
    inline constexpr int palette_width = 32;
    inline constexpr int palette_height = 16;
    inline constexpr int max_lights = 16;

    struct atlas_frame {
        float x = 0.f, y = 0.f, w = 0.f, h = 0.f; // pixels, top left origin
    };
    // a Futile element of the game's own atlases (TexturePacker sheets):
    // where it sits in game_texture and how it was trimmed from its source
    struct game_frame {
        float x = 0.f, y = 0.f, w = 0.f, h = 0.f; // in game_texture, pixels, top left origin
        bool rotated = false;                     // packed turned 90 degrees clockwise
        float trim_x = 0.f, trim_y = 0.f;         // the packed part's offset in the source
        float source_w = 0.f, source_h = 0.f;     // the element's size
    };

    struct state {
        SDL_Window* window = nullptr;
        gpu::context* gpu = nullptr;
        int width = 0;
        int height = 0;
        bool show_cameras = true;
        bool show_hud = true;
        bool show_geometry = false;
        float night_time = 0.f;
        // gpu::texture ids, 0 when not loaded
        uint32_t font_texture = 0;
        int font_width = 1;
        int font_height = 1;
        // room rendering (anthiel's level shader)
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
        // the game's own atlases (rainWorld, rainworldmsc...), read from the
        // Unity data at `unity_dir` at startup; empty without it
        std::string unity_dir;
        uint32_t game_texture = 0;
        int game_width = 1;
        int game_height = 1;
        std::unordered_map<std::string, game_frame> game_frames;
        // decals (decals/<name>.png, loaded on first use)
        struct decal_texture {
            uint32_t texture = 0; // 0: missing
        };
        std::unordered_map<std::string, decal_texture> decal_textures;
        std::vector<vertex> batch;
    };

    // `hidden` creates an invisible window (headless screenshots)
    bool init(state& r, int width, int height, char const* title, bool hidden);
    // the element's frame in the game's atlases, if they were loaded
    game_frame const* find_game_frame(state const& r, std::string_view element);
    // HUD font, palettes, the slugcat atlas and, if `r.unity_dir`
    // is set, the game's own atlases (after world::load)
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
    // Saves the frame drawn so far as a 24 bit BMP
    bool save_screenshot(state& r, char const* path);

    rw::vec2 screen_to_room(state const& r, camera const& cam, float sx, float sy);
}
