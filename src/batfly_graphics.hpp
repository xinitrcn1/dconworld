#pragma once

// Port of FlyGraphics: the body ("FlyBody", aimed from the dangling lower
// body to the chunk), two wings ("FlyWing", anchored at their base,
// flapping with the fly's flap, folded towards the steering side, gone one
// per bite) and the eyes ("FlyEyes"), in that order. The body and the
// wings take the palette's black (ApplyPalette); the eyes stay white.

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace batfly_graphics {
    // GraphicsModule.Reset: the lower body on the chunk
    void reset_positions(sys::game_state& state, dcon::creature_id c);
    // FlyGraphics.Update, once per tick after the fly's (viewed rooms only)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // DrawSprites (ApplyPalette from `pal`)
    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip);
}
