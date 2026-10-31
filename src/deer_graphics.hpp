#pragma once

// Port of DeerGraphics: the antlers (DeerGraphics.Antlers, also climbed by
// a rider), the five body chunks, the danglers hanging off the body and the
// antlers (Dangler), the four legs (two jointed sprites and a mesh each)
// and the eyes.

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace deer_graphics {
    // the DeerGraphics constructor's rolls (Random.InitState(ID.RandomSeed)):
    // the antlers, the chunks' turns, the danglers' layout
    void create(sys::game_state& state, dcon::creature_id c);
    // GraphicsModule.Reset when a camera looks at the creature's room again
    void reset_positions(sys::game_state& state, dcon::creature_id c);
    // the graphics module's Update, once per tick after the creature's
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // DrawSprites (ApplyPalette from `pal`); a TriangleMesh.MakeLongMesh's
    // vertices go to `strip`
    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip);
}
