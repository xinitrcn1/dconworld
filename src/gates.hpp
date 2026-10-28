#pragma once

// RegionGate: the three door airlock of gate rooms. Doors are tile columns
// 14+9k (two wide), rows 8..16, solid while a door is closed. Karma locks
// are not enforced yet (karma isn't tracked), every gate opens.

#include <cstdint>

#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace gates {
    inline constexpr int32_t door_first_x = 14;
    inline constexpr int32_t door_spacing = 9;
    inline constexpr int32_t door_bottom_y = 8;
    inline constexpr int32_t door_top_y = 16;

    // RegionGate.Update + Door.Update for the gate in `room`, if any
    void update_room(sys::game_state& state, dcon::room_id room);
    // the gate of a gate room (invalid for other rooms)
    dcon::gate_id gate_of(sys::game_state const& state, dcon::room_id room) noexcept;
}
