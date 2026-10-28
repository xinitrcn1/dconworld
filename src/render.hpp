#pragma once

// Debug view: draws room geometry and every player body in the room with
// OpenGL 3.3 (GLEW) in an SDL3 window. Tiles are culled to the view and the
// world is batched into a single vertex buffer upload and draw call; the HUD
// text uses anthiel's VGA font.

#include <cstdint>
#include <string>
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

    struct state {
        SDL_Window* window = nullptr;
        SDL_GLContextState* context = nullptr;
        int width = 0;
        int height = 0;
        bool show_cameras = true;
        bool show_hud = true;
        uint32_t program = 0;
        int32_t u_proj = -1;
        int32_t u_textured = -1;
        uint32_t font_texture = 0;
        int font_width = 1;
        int font_height = 1;
        uint32_t vao = 0;
        uint32_t vbo = 0;
        std::vector<vertex> batch;
    };

    // `hidden` creates an invisible window (headless screenshots)
    bool init(state& r, int width, int height, char const* title, bool hidden);
    // Loads anthiel's VGA font (vgafont.png) for the HUD from the filesystem
    bool load_font(state& r, layered_fs::filesystem const& fs);
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
