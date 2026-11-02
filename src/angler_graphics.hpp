#pragma once

// Port of AnglerGraphics: the body (a long mesh from the tail cone to the
// head), the tail cone, the jaw and head (Circle20s on their bottom edge),
// the cheek, the rudder fin, the seven spikes, the bottom and top teeth
// (curved long meshes), the eye, the lure's arm (four "pixel" sprites and
// the bulb at its tip) and the propeller blades, in vanilla sprite order,
// all in the palette's black but the eye and the lure; the lights (the
// LightSource, and the FlatLightBehindTerrain and LightSource sprites of
// the Water container at the lure) are `light_of`'s. The lure itself
// (lightPos) moves in the simulation (angler.cpp).

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace angler_graphics {
    // GraphicsModule.Reset when a camera looks at the creature's room again
    void reset_positions(sys::game_state& state, dcon::creature_id c);
    // the graphics module's Update, once per tick after the creature's
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // DrawSprites (ApplyPalette from `pal`); the meshes' vertices go to `strip`
    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip);

    // Angler.lightSource (LightSource, affectedByPaletteDarkness 0.5, set
    // from AnglerGraphics.Update) and the lure's two light sprites (the
    // Water container's FlatLightBehindTerrain and LightSource Futile_White
    // sprites at the bulb), in the lure's colour. False while the lights
    // are out (flickered down to nothing)
    struct light {
        rw::vec2 source_pos{0.f, 0.f};
        float source_rad = 0.f;
        float source_alpha = 0.f;
        rw::vec3 color{1.f, 1.f, 1.f};
        rw::vec2 tip{0.f, 0.f};
        // the sprites' half sizes (Futile_White is 16 pixels)
        float flat_half = 0.f;
        float glow_half = 0.f;
    };
    bool light_of(sys::game_state const& state, dcon::creature_id c, float alpha, light& out);
}
