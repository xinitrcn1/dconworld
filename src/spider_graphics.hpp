#pragma once

// Port of SpiderGraphics: the body ("SpiderBody", turned along bodyDir) and
// eight two-part legs ("SpiderLeg0A".."SpiderLeg3B", bent at the knee by
// inverse kinematics) whose Limbs find grips on the terrain as it walks,
// the front pair holding on to the spider ahead in a chain (or the prey),
// the back pair to the one behind; all in the palette's black, drawn in
// vanilla's sprite order (the body, then side 0's upper and lower leg
// sprites, then side 1's).

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace spider_graphics {
    // the SpiderGraphics constructor (rolled from the spider: the same
    // whenever it is made)
    void reset(sys::game_state& state, dcon::creature_id c);
    // GraphicsModule.Reset when a camera looks at the creature's room again
    void reset_positions(sys::game_state& state, dcon::creature_id c);
    // SpiderGraphics.Update, once per tick after the spider's update
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // DrawSprites (ApplyPalette from `pal`: everything blackColor)
    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip);
}
