#pragma once

// PuffBall (and its PuffBallSkin and SporeCloud bursts): a light weapon
// item with a stalk of spores, thrown like a rock; it bursts into a cloud
// of spores when it hits something (PuffBall.Explode), which stuns the
// insects about and calls a rain deer over (SporePuffVisionObscurer). A
// deer eats the free ones it finds (deer.cpp). items.cpp keeps it like its
// other weapons and calls in here for what is its own; its own state is
// the puff_ball_data object (puff_ball_types.hpp).

#include <cstdint>
#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace puff_ball {
    // Weapon.exitThrownModeSpeed
    inline constexpr float exit_thrown_mode_speed = 15.f;

    // the PuffBall constructor's body and its rolls (from the ID), for an
    // item just made by items.cpp
    void init(sys::game_state& state, dcon::item_id it);
    // PuffBall.PlaceInRoom (down to the floor) + NewRoom (the stalk)
    void place(sys::game_state& state, dcon::item_id it);
    // gone (items::destroy): its data object
    void destroyed(sys::game_state& state, dcon::item_id it);
    // PuffBall.Update before Weapon.Update; true when it bursts (it was
    // thrown and hit the terrain)
    bool update(sys::game_state& state, dcon::item_id it, world::room_view const& room);
    // PuffBall.Thrown: a puff of spores in the throw direction
    void thrown(sys::game_state& state, dcon::item_id it, rw::ivec2 throw_dir);
    // PuffBall.Explode at `at` (items.cpp destroys the item after)
    void explode(sys::game_state& state, world::room_view const& room, dcon::item_id it, rw::vec2 at);
    // Deer.Act's eating: PuffBall.beingEaten starts, and it shrinks away
    void start_being_eaten(sys::game_state& state, dcon::item_id it);

    // PuffBall.DrawSprites (ApplyPalette: `texel_11_4` is the palette's
    // texture.GetPixel(11, 4))
    void build(sys::game_state const& state, dcon::item_id it, float alpha, rw::vec3 black, rw::vec3 texel_11_4, std::vector<sprites::draw_cmd>& out);
}
