#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "math.hpp"
#include "gamestate.hpp"
#include "world.hpp"
#include "lizard_types.hpp"
#include "daddy_types.hpp"
#include "centipede_types.hpp"
#include "leech_types.hpp"

namespace selftest {
    // Scripted movement scenarios in synthetic rooms; returns process exit code
    int run();
    // Random input soak test: a creature of each kind in every room (the
    // centipede and leech variants in turn), then
    // slugcats in every room (a batch of rooms at a time, the whole world
    // ticking), then every room exit pipe
    int fuzz(sys::game_state& gs, uint32_t ticks_per_room);
    // `ticks` ticks of the whole world with a creature of each kind in every
    // room, once on one thread and once on every thread: ms per tick, and
    // whether both end in the same world (`load` makes a fresh one)
    int bench(std::function<std::unique_ptr<sys::game_state>()> const& load, uint32_t ticks);
    // the creatures spawn_creatures put in a room (invalid: no spot found)
    struct world_creatures {
        dcon::creature_id centipede;
        centipede::variant centipede_variant{};
        dcon::creature_id dropbug;
        dcon::creature_id cicada;
        dcon::creature_id lizard;
        lizard::breed breed{};
        dcon::creature_id daddy;
        daddy::variant variant{};
        // a little school in the rooms with water (a jungle leech's on dry land)
        static constexpr uint32_t school = 3;
        dcon::creature_id leeches[school];
        leech::variant leech_variant{};
        // (a leech: the school's first)
        dcon::creature_id& of(creature::type t) noexcept;
    };
    // one creature of each kind in the room, placed with rolls seeded by the
    // room's index (the fuzz's and the bench's world)
    world_creatures spawn_creatures(sys::game_state& gs, dcon::room_id rid);
    // A sensible place to drop the slugcat in a room (feet position)
    rw::vec2 find_spawn(world::room_view const& room, rw::vec2 near);
}
