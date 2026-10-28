#pragma once

// One 40Hz tick of everything that runs: players (or their shortcut
// vessels), then items and gates of every room a player is in.

#include <span>

#include "gamestate.hpp"
#include "player_types.hpp"

namespace simulation {
    // `inputs[i]` drives the i-th player in `players`
    void tick(sys::game_state& state, std::span<dcon::player_id const> players, std::span<::player::input_package const> inputs);
    // room the player is in (its body's room)
    dcon::room_id player_room(sys::game_state const& state, dcon::player_id p) noexcept;
    // Room.Loaded side effects of a room becoming active (item spawns)
    void activate_room(sys::game_state& state, dcon::room_id room);
}
