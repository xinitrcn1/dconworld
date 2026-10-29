#pragma once

// Simplified port of PlayerGraphics: the head, legs and tail body parts and
// the sprite selection of DrawSprites (body, hips, tail mesh, head, legs,
// face), using vanilla element names so DressMySlugcat style atlases drop in.
// Arms/hands (Limb IK), breathing, look targets and class specific extras
// are not ported yet.

#include <cstdint>
#include <string_view>
#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace player_graphics {
    using sprites::sprite;
    using sprites::draw_cmd;
    // the Watcher's Void Weaver decorations (longer tail, fluff, halo, gold eyes)
    void set_weaver(sys::game_state& state, dcon::player_id p, bool on);
    bool weaver(sys::game_state const& state, dcon::player_id p);

    // Puts every body part back on the body (PlayerGraphics.Reset)
    void reset(sys::game_state& state, dcon::player_id p);
    // PlayerGraphics.Update, once per tick after the player update
    void update(sys::game_state& state, dcon::player_id p, world::room_view const& room);
    // PlayerGraphics.ThrowObject: the throwing hand follows the item a moment
    void throw_object(sys::game_state& state, dcon::player_id p, uint32_t hand, dcon::item_id item);
    // where SlugcatHand `hand` is (carried items sit there)
    rw::vec2 hand_position(sys::game_state const& state, dcon::player_id p, uint32_t hand);
    // PlayerGraphics.DrawSprites; `alpha` interpolates between ticks
    void build(sys::game_state const& state, dcon::player_id p, float alpha, std::vector<draw_cmd>& out);
}
