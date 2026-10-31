#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

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
    // NPC slugcats in the world's shelters (`gs.npc_shelter_chance`), `ticks`
    // ticks of the whole world: how they get on (shelters reached, regions
    // crossed, stuck, dead) and what they cost a tick
    int npc_soak(sys::game_state& gs, uint32_t ticks);
    // One NPC slugcat from shelter `from` to shelter `to`, alone in the world
    // (no creatures, the rain timers frozen): it passes when it rests in
    // `to` within `ticks`, fails when it dies, finds no way or gets stuck.
    // `name`: the slugcat (count: random); `seed` its rolls
    int npc_trip(sys::game_state& gs, std::string const& from, std::string const& to, uint32_t ticks, player::slugcat_name name, uint32_t seed);
    // `ticks` ticks of the whole world, once on one thread and once on every
    // thread: ms per tick, and whether both end in the same world (`load`
    // makes a fresh one). `mode`: "none" (no creatures), "each" (a creature
    // of each kind in every room), "dens" (the world files' population in
    // their dens, coming out), empty for all three
    int bench(std::function<std::unique_ptr<sys::game_state>()> const& load, uint32_t ticks, std::string const& mode = {});
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
        dcon::creature_id tube_worm;
        // the kinds ported after it, by type
        dcon::creature_id later[creature::type_count];
        // (a leech: the school's first)
        dcon::creature_id& of(creature::type t) noexcept;
    };
    // one creature of each kind in the room, placed with rolls seeded by the
    // room's index (the fuzz's and the bench's world)
    world_creatures spawn_creatures(sys::game_state& gs, dcon::room_id rid);
    // A sensible place to drop the slugcat in a room (feet position)
    rw::vec2 find_spawn(world::room_view const& room, rw::vec2 near);
}
