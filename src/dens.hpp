#pragma once

// Dens and creatures travelling through pipes.
//
// A creature lives in a den (AbstractCreature.spawnDen: a room and its
// exitAndDenIndex node, the creature's `home_room`/`home_den`). When its AI
// wants to go home (EscapeRain as the rain approaches, ReturnPrey with a
// slugcat in its jaws, Injured) it asks `home_tile` for the pipe entrance to
// head for: its den's in its home room, else the room exit on the way
// there (a world level route over the rooms' exits, `build_route`, not the
// level's baked pathing). Close to it, the creature squeezes in
// (Creature.enteringShortCut: every chunk pulled into the hole, its Update
// skipped), and travels the pipe as a vessel like the slugcat
// (ShortcutHandler), its body in no room meanwhile. A room exit leads into
// the connected room's matching pipe and out of it; a den keeps the
// creature (AbstractRoom.entitiesInDens: `away` 2, not simulated) when it
// wants to stay (AbstractCreature.OpportunityToEnterDen: the rain is near,
// it is hurt, it carries prey, which dies), else it turns back.
// AbstractCreature.InDenUpdate counts `remain_in_den` down while the cycle
// allows and lets it out through the den's pipe; when the rain is over and
// its region's next cycle starts, the ones waiting it out get a fresh
// count, like a new cycle's spawns.
//
// The vessels and the dens update with the room holding them (in
// parallel); a creature coming out of a pipe, going into one or changing
// rooms is a transaction (`game_state::defer`).

#include <atomic>
#include <cstdint>
#include <optional>

#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace dens {
    // the spawn data's tags (AbstractCreature.setCustomFlags), creature.spawn_flags
    enum spawn_flag : uint8_t {
        flag_night = 1 << 0,          // {Night}: out only at night
        flag_ignore_cycle = 1 << 1,   // {Ignorecycle}: the rain doesn't bother it
        flag_pre_cycle = 1 << 2,      // {PreCycle}: out only in a precycle (none here)
        flag_tentacle_immune = 1 << 3,
        flag_lava_safe = 1 << 4,
        flag_winter = 1 << 5,
        flag_alternate_form = 1 << 6, // superSizeMe
        flag_void_sea = 1 << 7,
    };
    enum class where : uint8_t { room, shortcut, den };
    inline constexpr uint8_t no_den = 0xFF;

    // the den a creature lives in (and the route home from anywhere)
    void set_home(sys::game_state& state, dcon::creature_id c, dcon::room_id room, uint8_t den);
    // the room to room route towards `home` (serial: before the rooms update)
    void build_route(sys::game_state& state, dcon::room_id home);
    // the pipe entrance of a room's den node, of the exit leading to `next`
    std::optional<rw::ivec2> den_entrance(sys::game_state const& state, dcon::room_id room, uint8_t den);
    // the pipe entrance of the room exit node `node` of `room`
    std::optional<rw::ivec2> exit_entrance(sys::game_state const& state, dcon::room_id room, uint8_t node);
    // the room an exit of `from` leads to, if its pipe can be travelled
    // both ways; gates and shelters only when `gates` (migrate.hpp's
    // trips through a gate; creatures keep out of shelters)
    dcon::room_id exit_target(sys::game_state const& state, dcon::room_exit_id e, dcon::room_id from, bool gates = false);
    // puts a creature (out in a room, at its den) straight into its den
    // (world load: AbstractRoom.MoveEntityToDen) to come out in `remain` ticks
    void put_in_den(sys::game_state& state, dcon::creature_id c, int32_t remain);

    // The AI's DenFinder.GetDenPosition and the abstract AI's route: the pipe
    // entrance to head for on the way home from `room` (its den's, a room
    // exit's, or the closest den of the room for a creature without one);
    // marks the creature as going home this tick
    std::optional<rw::ivec2> home_tile(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // RainTracker.Utility for the creature in `room`, by its home region's
    // cycle (where it is if it has no den): 0 for a night creature or one
    // ignoring the cycle
    float rain_utility(sys::game_state const& state, dcon::creature_id c, dcon::room_id room) noexcept;
    // a creature moved from room `from` to `to` (a pipe, a warp): counts
    // the region it changed (the fuzz's)
    void changed_rooms(sys::game_state& state, dcon::creature_id c, dcon::room_id from, dcon::room_id to, bool warp);
    // AbstractCreature.WantToStayInDenUntilEndOfCycle
    bool want_to_stay(sys::game_state const& state, dcon::creature_id c, dcon::room_id room);

    // A creature's turn in its room's update. `before`: Creature.Update while
    // enteringShortCut, true when it squeezes into a pipe (the kind's update
    // is skipped). `after`: a creature going home close to its pipe starts
    // squeezing in
    bool before_update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    void after_update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // the vessels in the room's pipes and the creatures in its dens
    void update_room(sys::game_state& state, world::room_view const& room);
    // a region's (group's) new cycle started: the creatures waiting in its
    // dens for the rain to pass get a fresh count to come out
    void cycle_restarted(sys::game_state& state, dcon::region_id leader);

    inline where where_is(sys::game_state const& state, dcon::creature_id c) noexcept { return where(state.world.creature_get_away(c)); }

    // what happened since start (reports, the fuzz; never read by the simulation)
    struct counters {
        std::atomic<uint32_t> into_pipes{0};
        std::atomic<uint32_t> into_dens{0};
        std::atomic<uint32_t> out_of_dens{0};
        std::atomic<uint32_t> room_changes{0};
        std::atomic<uint32_t> prey_taken{0};
        std::atomic<uint32_t> creatures_eaten{0};
        // creatures that went to another region through a gate, a warp
        std::atomic<uint32_t> gated_regions{0};
        std::atomic<uint32_t> warped_regions{0};
        // trips planned (migrate.hpp), to another region
        std::atomic<uint32_t> trips{0};
        std::atomic<uint32_t> cross_trips{0};
        std::atomic<uint32_t> warp_trips{0};
        // creatures a warp carried on their own (not pulled along with the
        // Watcher)
        std::atomic<uint32_t> warp_carried{0};
    };
    counters& stats() noexcept;
}
