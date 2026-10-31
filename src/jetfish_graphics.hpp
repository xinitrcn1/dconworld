#pragma once

// Port of JetFishGraphics: the body sprite (JetFish0..8 by the z rotation),
// the eyes, the whiskers and the two tentacles (long meshes) and the
// flippers, in vanilla sprite order; the tentacles wave with the swim and
// fan out into the jet, the flippers paddle.

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace jetfish_graphics {
    // GraphicsModule.Reset when a camera looks at the creature's room again
    // (and the constructor's whisker layout, from iVars.whiskerSeed)
    void reset_positions(sys::game_state& state, dcon::creature_id c);
    // the graphics module's Update, once per tick after the creature's
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // DrawSprites (ApplyPalette from `pal`); a TriangleMesh.MakeLongMesh's
    // vertices go to `strip`
    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip);
}
