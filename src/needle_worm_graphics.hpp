#pragma once

// Port of NeedleWormGraphics: the snout (with the adult's fang), the long
// body and its highlight as TriangleMesh long meshes, two eyes, four wings
// (CentipedeWing CustomFSprites), the little legs (JetFishFlipper3) and the
// adult's wing lumps (JetFishEyeB), coloured from the creature's ID.

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace needle_worm_graphics {
    // NeedleWormGraphics constructor: the looks rolled from the ID's seed
    void init(sys::game_state& state, dcon::creature_id c);
    // GraphicsModule.Reset when a camera looks at the creature's room again
    void reset_positions(sys::game_state& state, dcon::creature_id c);
    // the graphics module's Update, once per tick after the creature's
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // DrawSprites (ApplyPalette from `pal`); a TriangleMesh.MakeLongMesh's
    // vertices go to `strip`
    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip);
}
