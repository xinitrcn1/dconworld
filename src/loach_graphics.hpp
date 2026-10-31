#pragma once

// Port of LoachGraphics: the mouth tentacles (LoachTentacleGraphic, a
// RopeGraphic(15) along each tentacle), the danglers under the body
// (Dangler), the shoulders over the legs, the body (eleven circles along
// the chunks), the spots (GrowthA/B, in front of or behind the body as it
// turns), the six legs (two jointed deerLeg sprites and a mesh each, as
// the rain deer's) and the two peeper eyes, in vanilla sprite order.

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace loach_graphics {
    // the LoachGraphics constructor's rolls (Random.InitState(ID.RandomSeed)):
    // the danglers' layout, the spots
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
