#pragma once

// Port of BigSpiderGraphics: the body (a TriangleMesh along a bezier from
// the head to the tail end), the head (Circle20), the eight legs
// (CentipedeLegA, SpiderLeg{n}A, SpiderLeg{n}B / CentipedeLegB: three
// segments through Custom.InverseKinematic, gripping the terrain with
// Limb.FindGrip, holding on to what the legs grabbed), the mandibles
// (SpiderLeg0A and a stretched SpiderLeg0B) and the hair (strands of
// LizardScaleA1, the spitter's stiffer and redder), in vanilla sprite
// order. The JaggedSquare shader of the body mesh isn't ported: it is
// drawn solid.

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace big_spider_graphics {
    // the BigSpiderGraphics constructor's rolls from the ID's seed (the
    // hair's layout, leg and body thickness, the dead leg), at creation
    void init(sys::game_state& state, dcon::creature_id c);
    // GraphicsModule.Reset: every part back at the chunk it hangs from
    void reset_positions(sys::game_state& state, dcon::creature_id c);
    // BigSpiderGraphics.Update, once per tick after the creature's
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // DrawSprites (ApplyPalette from `pal`); the body mesh's vertices go to
    // `strip`
    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip);
}
