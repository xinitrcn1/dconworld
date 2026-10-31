#pragma once

// Port of GarbageWormGraphics: the tentacle as one long mesh (MakeLongMesh
// over the tentacle's chunks) waving with a sine along it and swelling
// where it swallows, the "WormHead" on its tip and the two "WormEye"s (one
// behind the body, one in front, swapped by the way the head faces), white,
// red when it shows it is angry. Drawn back in, the body is pulled down
// into the hole.

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace garbage_worm_graphics {
    // GraphicsModule.Reset when a camera looks at the creature's room again
    void reset_positions(sys::game_state& state, dcon::creature_id c);
    // the graphics module's Update, once per tick after the creature's
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // DrawSprites (ApplyPalette from `pal`); the mesh's vertices go to `strip`
    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip);
}
