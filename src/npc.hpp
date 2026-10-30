#pragma once

// NPC slugcats: slugcats without a controller, driven by a brain of our own
// rather than vanilla's SlugNPCAI. A slugcat is a creature like any other
// (creature.hpp), and an NPC is a `player` (Player.cs' state) whose input is
// made up every tick by `think` instead of read from a controller; its brain
// is its `slugcat_npc` object (the player's `npc`).
//
// They start in shelters (a group of 1 to 8 in a share of the world's
// shelters, `spawn_world`), any slugcat (Downpour's and the Watcher's too,
// the Watcher at times a Void Weaver) in random colours. Each one heads for
// a target shelter, any one it can get to, in its region or in another one
// it can reach through gates and Watcher warps: a world level route over
// the room exits (`build_routes` works out, once, which exits and warps of
// every room a slugcat can walk between), followed room by room with the
// creatures' trips (migrate.hpp's `trip` and `travel_tile`: the pipes, the
// gates' airlocks, the warps). In a room it follows a slugcat path field
// (pathing.hpp's movement::slugcat: walking, crawling, climbing poles,
// swimming, dropping off ledges, jumping up to a ledge at most 3 tiles up,
// and charge pounces over gaps, no other tech) with plain controller input.
// It doesn't fight: it runs from whatever would hunt it (into a pipe when it
// can), and a pack (like the Yellow lizards') shares what each of them
// sees, goes where its leader goes and waits for the others at the pipes.
// In the target shelter it stays until the rain is over, then picks
// another one (lying down where slugcats sleep there, Spearmaster pulling
// two needles out of its tail when a cycle starts); with the rain close it makes for the nearest shelter, and a
// pack too slow on its way (less than half way there by 50 to 75% of the
// cycle, rolled per pack) goes back to the shelter it started in.

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "player_types.hpp"
#include "world.hpp"

namespace npc {
    // the share of shelters that get a group at world load (`--npcs`)
    inline constexpr float default_shelter_chance = 0.25f;

    inline dcon::slugcat_npc_id of(sys::game_state const& state, dcon::player_id p) noexcept { return state.world.player_get_npc(p); }
    // a slugcat driven by a brain rather than a controller
    inline bool is_npc(sys::game_state const& state, dcon::player_id p) noexcept { return bool(of(state, p)); }

    // An NPC slugcat with its feet at `feet`: a random slugcat in random
    // colours, rolled with `rng`; in `leader`'s pack if given (none: alone,
    // the player itself: a new pack's leader)
    dcon::player_id create(sys::game_state& state, dcon::room_id room, rw::vec2 feet, rw::rng& rng, dcon::slugcat_npc_id leader = {},
        player::slugcat_name name = player::slugcat_name::count);
    // a group of `n` around `at`, some of them a pack (at least two)
    void spawn_group(sys::game_state& state, dcon::room_id room, rw::vec2 at, uint32_t n, rw::rng& rng);
    // world load: a group of 1 to 8 in `chance` of the shelters (rolled
    // with each shelter's generator); how many were made
    uint32_t spawn_world(sys::game_state& state, float chance);
    // removes one, every one
    void destroy(sys::game_state& state, dcon::player_id p);
    void destroy_all(sys::game_state& state);

    // which exits and warps of every room a slugcat can walk between
    // (serial, every room in parallel; again after rooms are added)
    void build_routes(sys::game_state& state);
    // the shelters a slugcat could get to from where `p` stands (tests, the
    // HUD); the first is the closest
    std::vector<dcon::room_id> reachable_shelters(sys::game_state& state, dcon::player_id p);

    // where slugcats wake up and sleep in a shelter (ShelterDoor's
    // playerSpawnPos): NPCs spawn there and lie down there when they arrive
    std::optional<rw::ivec2> shelter_spot(sys::game_state const& state, dcon::room_id shelter);

    // The pipe (or warp) `p` would leave its room by on its way to its
    // target from where it stands now (planned afresh; `next`: the room it
    // leads to), none without a way on
    std::optional<rw::ivec2> planned_exit(sys::game_state& state, dcon::player_id p, dcon::room_id& next);
    // the brain goes: from now on a controller drives `p`
    void take_over(sys::game_state& state, dcon::player_id p);

    // Serial, before the rooms update: the packs (a dead leader's place
    // goes to the next one), the leaders' targets for the others
    void update_world(sys::game_state& state);
    // This tick's input of an NPC slugcat out in `room` (its room's update)
    player::input_package think(sys::game_state& state, dcon::player_id p, world::room_view const& room);

    std::string describe(sys::game_state const& state, dcon::player_id p);

    // Tracing and debugging, from the command line (main.cpp's --npc-*
    // options; see docs/SlugAI.md). Never read by the simulation itself,
    // only by the reports below and selftest.cpp's NPC runs
    struct debug_options {
        // --npc-trace <room|*>[,every]: the NPC's state every `every` ticks
        // while it is in that room (--npc-trip, the selftest scenarios)
        std::string trace_room;
        int32_t trace_every = 10;
        // --npc-route <room,room,...>: whether each room of the list can be
        // walked from the pipe from the room before to the one to the next
        std::vector<std::string> route;
        // --npc-maps: with --npc-route, a map of every room that can't
        // (f reachable from the way in, o can reach the way out, B both)
        bool maps = false;
        // --npc-room <room>: dump the room (poles, pipes, walkable tiles);
        // --npc-goal <bit>: mark what reaches that exit (or warp); --npc-box
        // <x0,y0,x1,y1>: list the walkable tiles there with their distance
        std::string room;
        int32_t goal = -1;
        bool box = false;
        rw::ivec2 box_lo{0, 0};
        rw::ivec2 box_hi{0, 0};
        // --npc-edges <room,x0,y0,x1,y1>: the jumps leaving those tiles
        std::string edges_room;
        rw::ivec2 edges_lo{0, 0};
        rw::ivec2 edges_hi{0, 0};
        // --npc-dead-ends: the exits a slugcat can walk to no other exit
        // from; --npc-unreachable: why the first NPCs without a shelter
        // have none; --npc-list: every NPC at the end of --npc-soak
        bool dead_ends = false;
        bool unreachable = false;
        bool list = false;
        // --npc-guess: --npc-trip sets off without a walkable way known
        // (the guessed trips of an NPC that knows none)
        bool guess = false;
        // --npc-measure (with --selftest): the movement measurement in its
        // current form instead of the tests
        bool measure = false;
        // --npc-exp <room,x,y,mode,dir[,turn]>: a scripted jump off the pole
        // at x climbed to y (mode 0 off its side, 1 from its tip, 2 into a
        // wall and off it, 3 off, back to the wall behind and off it, 4 from
        // the tip into a wall and back, 5 off the side into a wall and back)
        std::string experiment;
    };
    debug_options& debug() noexcept;
    std::string debug_routes(sys::game_state& state, dcon::player_id p);
    std::string debug_dead_ends(sys::game_state& state);
    std::string debug_reach(sys::game_state& state, dcon::player_id p);
    std::string debug_route(sys::game_state& state, std::vector<std::string> const& names);
    std::string debug_room(sys::game_state& state, dcon::room_id room, rw::ivec2 mark);

    // what happened since start (reports, tests; never read by the simulation)
    struct counters {
        std::atomic<uint32_t> spawned{0};
        std::atomic<uint32_t> arrived{0};
        std::atomic<uint32_t> regions_crossed{0};
        std::atomic<uint32_t> trips{0};
        std::atomic<uint32_t> no_way{0};
        std::atomic<uint32_t> stuck{0};
        std::atomic<uint32_t> rises{0};
        std::atomic<uint32_t> leaps{0};
        std::atomic<uint32_t> pounces{0};
        std::atomic<uint32_t> flights{0};
        std::atomic<uint32_t> hurried{0};
        std::atomic<uint32_t> turned_back{0};
        std::atomic<uint32_t> picked_up{0};
        std::atomic<uint32_t> thrown{0};
        std::atomic<uint32_t> braved{0};
        std::atomic<uint32_t> retargeted{0};
        std::atomic<uint32_t> diverted{0};
        std::atomic<uint32_t> spear_steps{0};
        std::atomic<uint32_t> gave_up{0};
    };
    counters& stats() noexcept;
    std::string stats_line(sys::game_state const& state);
}
