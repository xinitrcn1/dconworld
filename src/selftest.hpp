#pragma once

#include <cstdint>

#include "math.hpp"
#include "gamestate.hpp"
#include "world.hpp"

namespace selftest {
    // Scripted movement scenarios in synthetic rooms; returns process exit code
    int run();
    // Random input soak test of the slugcat in every loaded room
    int fuzz(sys::game_state& gs, uint32_t ticks_per_room);
    // A sensible place to drop the slugcat in a room (feet position)
    rw::vec2 find_spawn(world::room_view const& room, rw::vec2 near);
}
