#pragma once

// An NPC slugcat's brain state, stored in the data container (the
// slugcat_npc object, see npc.hpp)

#include <cstdint>

#include "dcon_generated_ids.hpp"
#include "math.hpp"

namespace npc {
    // what its legs are doing: plain steps along the path field, or one of
    // the moves that takes a few ticks of input (jumping up to a ledge, a
    // running jump or a charge pounce over a gap)
    enum class move : uint8_t {
        walk,
        // standing up, then jumping towards the ledge and holding on
        rise,
        // a running jump: back from the edge for a run up, the run up
        back_up,
        run,
        // a pounce: down on fours facing the gap, holding jump until it is
        // charged
        crouch,
        charge,
        // in the air after either jump (climbing up a ledge it grabbed)
        flight,
        // off a pole, holding up to grab the other one
        swing,
        // up onto a pole's tip, to jump from it
        tip,
        // wall jumps from side to side up a narrow shaft
        wall,
        // from a pole's tip into a wall, off it back to a pole
        bounce,
        // a crawl-turn vault (the community's "perfect CTV"): down on fours
        // facing the way, back for 5 ticks cancelled with up at the second,
        // then down, the way and jump at once: far higher than a jump
        vault,
    };

    // what it is after
    enum class goal : uint8_t {
        // in its shelter at the start, waiting to set off
        start,
        // on its way to the target shelter
        travel,
        // running from a threat
        flee,
        // in the target shelter until the rain is over
        rest,
    };

    inline constexpr uint32_t max_blocked = 4;
    inline constexpr uint32_t max_no_guess = 4;

    struct nav_state {
        goal what = goal::start;
        move how = move::walk;
        // ticks in the current move, in the current goal
        int32_t move_timer = 0;
        int32_t goal_timer = 0;
        // the move's direction, the tile it aims for and the one it started
        // from (a jump's edge), whether it holds jump in the air
        int8_t dir = 0;
        rw::ivec2 aim{-1, -1};
        rw::ivec2 from{-1, -1};
        uint8_t hold_jump = 0;
        // the step it took last (debugging)
        rw::ivec2 last_step{-1, -1};
        uint8_t last_kind = 0;
        // the moves that failed lately (from, to), skipped while in the room
        rw::ivec2 blocked[4][2] = {{{-1, -1}, {-1, -1}}, {{-1, -1}, {-1, -1}}, {{-1, -1}, {-1, -1}}, {{-1, -1}, {-1, -1}}};
        uint8_t blocked_next = 0;
        // wall jumps: the wall it heads for, ticks before the next jump
        int32_t wall_side = 0;
        int32_t cool = 0;
        // the room it was in last tick (its index + 1, 0 none): arriving
        // somewhere moves its trip on
        uint16_t room = 0;
        uint16_t room_before = 0;
        // how often in a row it went back into the room before (back and
        // forth between two: a way that leads nowhere either side)
        uint8_t bounces = 0;
        // no progress along the path: the best distance to the goal so far
        // and the ticks since it got better
        rw::ivec2 goal_tile{-1, -1};
        uint16_t best = 0xFFFF;
        int32_t stuck = 0;
        // failed tries at the current trip (a new target after a few), the
        // target it gave up on last (its index + 1, 0 none)
        uint8_t fails = 0;
        uint16_t given_up = 0;
        // the trip was guessed from the rooms' connections (no walkable way
        // known): where the path field doesn't reach, it tries anyway
        uint8_t guessed = 0;
        // into a new room on a guessed trip: look for a walkable way again
        uint8_t rethink = 0;
        // the rooms a guessed way through failed in lately (index + 1, 0
        // none): guessed through no more, the walkable way there only
        uint16_t no_guess[4] = {0, 0, 0, 0};
        uint8_t no_guess_next = 0;
        // the creature it runs from, where it last saw it, and the ticks
        // since (it gives up after a while out of sight)
        dcon::creature_id threat;
        rw::vec2 threat_pos{0.f, 0.f};
        int32_t threat_timer = 0;
        // the pipe it flees into (x < 0 none)
        rw::ivec2 flee_tile{-1, -1};
        // a pack leader waiting for the others at a pipe
        int32_t wait = 0;
        // ticks standing in a warp waiting for it to set off
        int32_t warp_wait = 0;
        // A spear step (only where no other way on is known): a spear thrown
        // from `spear_from` (standing there) towards `spear_dir` sticks in the
        // wall and lends `spear_beam` (and the tile behind it) a beam to climb
        // on. 0 none, 1 under way (fetching a spear, going there, throwing);
        // the room it last looked for one in (index + 1), throws tried
        // crawl-turn vaults failed in this room, the one that failed twice
        // (not tried again here)
        uint8_t vault_fails = 0;
        rw::ivec2 vault_bad[2] = {{-1, -1}, {-1, -1}};
        uint8_t spear_phase = 0;
        int8_t spear_dir = 0;
        rw::ivec2 spear_from{-1, -1};
        rw::ivec2 spear_beam{-1, -1};
        uint16_t spear_looked = 0;
        uint8_t spear_throws = 0;
        int32_t spear_wait = 0;
        // chased back through a pipe: how many times in a row from the same
        // room (its index + 1), the ticks it hides before trying again (more
        // each time: a creature camping by the pipe moves on in the end)
        uint8_t chased = 0;
        uint16_t chased_from = 0;
        int32_t hide = 0;
        // ticks spent running from threats lately, the ticks it ignores them
        // (it had enough of running: in it goes, whatever is there)
        int32_t flee_ticks = 0;
        int32_t brave = 0;
        // Spearmaster: ticks it has been fighting the current threat
        int32_t fight_ticks = 0;
        // ticks before it picks something up or throws again
        uint8_t arm_cool = 0;
        // it made for the nearest shelter with the rain close (once a cycle)
        uint8_t hurried = 0;
        // its target shelter's region's finished cycles when it got there
        // (a new cycle sends it off again)
        uint32_t cycles = 0;
        // the failsafe against the rain: the share of the cycle (percent,
        // rolled per pack, its leader's counts) by which it must be half way
        // to its target, else it goes back to the shelter it started in; the
        // rooms the way to the target took when it was picked, the cycle
        // (+ 1) it last checked in
        // Spearmaster: the cycle (+ 1) it last had its two spears in a
        // shelter
        uint32_t spear_cycle = 0;
        uint16_t spear_timer = 0;
        uint8_t turn_back = 50;
        uint8_t route_hops = 0;
        uint32_t turn_cycle = 0;
    };
}
