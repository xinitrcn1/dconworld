#pragma once

// Port of LeechGraphics: a tail of GenericBodyParts trailing the leech's
// chunk, wiggling as it swims and humping along the ground, and DrawSprites'
// one TriangleMesh (Futile_White: a flat colour, the same with or without
// the game's atlases), red, a sea leech's blue or a jungle leech's green
// fading into the palette's black.

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace leech_graphics {
    // the LeechGraphics constructor (and GraphicsModule.Reset)
    void reset(sys::game_state& state, dcon::creature_id c);
    // GraphicsModule.Reset when a camera looks at the creature's room again
    void reset_positions(sys::game_state& state, dcon::creature_id c);
    // LeechGraphics.Update, once per tick after the leech's update
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // LeechGraphics.DrawSprites; `black` is the palette's blackColor
    void build(sys::game_state const& state, dcon::creature_id c, float alpha, rw::vec3 black, std::vector<sprites::draw_cmd>& out);
}
