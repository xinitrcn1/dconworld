#pragma once

// One 40Hz tick of the whole world. Every room of every region is loaded and
// simulated all the time (no AbstractRoom/AbstractCreature layer): the rain
// cycles, players, creatures (and the ones in pipes and dens), items, rot,
// water, sand, the rooms' rain and gates of all the rooms, in
// parallel, one task per room (TBB). A room's update only reads and writes
// what is in it; what reaches beyond (objects created or deleted, anything
// moving to another room) is queued on the room and applied after, serially,
// room by room in order, so the outcome doesn't depend on the thread count.
// Shortcut vessels and the Watcher's warps, which carry things between rooms,
// run serially around the parallel phase. Graphics modules, particles,
// sounds and music only run for the rooms a camera looks at (the players'
// rooms and game_state::camera_rooms).

#include <cstdint>
#include <span>

#include "gamestate.hpp"
#include "player_types.hpp"

namespace simulation {
    // `inputs[i]` drives the i-th player in `players`
    void tick(sys::game_state& state, std::span<dcon::player_id const> players, std::span<::player::input_package const> inputs);
    // room the player is in (its body's room)
    dcon::room_id player_room(sys::game_state const& state, dcon::player_id p) noexcept;
    // Room.Loaded's first visit side effects (items, scavenger loot, rot,
    // warp points, sand) for every room that hasn't had them yet: all of
    // them at startup, deterministically, each room rolling with its own
    // generator seeded from game_state::random and its name; then, once,
    // the rain cycles (rain.hpp) and the world files' creatures in their
    // dens (spawns.hpp, unless game_state::spawn_dens is off). Set the
    // campaign (story_slugcat) and the seed first. tick() calls it too, for
    // rooms added later (tests)
    void populate(sys::game_state& state);
    // where the ticks' time went, in ms (summed since start; the bench resets it)
    struct tick_times {
        double serial_before = 0.; // vessels, the viewed rooms, the room index
        double rooms = 0.;         // every room's update, in parallel
        double serial_after = 0.;  // transactions, outputs, warps, particles
        uint32_t ticks = 0;
    };
    tick_times& times() noexcept;
    // a hash of every body chunk's position and velocity (determinism checks)
    uint64_t hash(sys::game_state const& state);
}
