#pragma once

// Stowaways (Downpour's StowawayBug.cs, StowawayBugAI.cs, StowawayBugState):
// a mouth hanging from the ceiling (its first chunk pinned at its home, the
// second, the maw, held below it), a fringe of feelers dangling down
// (seven to eleven strings of points under gravity, longer the more open
// room there is below) and three heads: limp Tentacles curled up in the
// body until it fires them. It doesn't see (visualRadius 0): it learns of
// creatures when they brush its feelers' tips or are caught. Hunting, it
// spits a head at its prey's predicted position (tip velocity 45); a head
// that touches a creature stings it (stun) and, if it eats that, holds on
// and reels it up as the head retracts; held at the maw, the prey is bitten
// (4 damage) and, dead, swallowed (pulled into the mouth over 80 ticks,
// destroyed; a slugcat is eaten). It then digests, asleep. Most cycles a
// stowaway pointing down sleeps through (StowawayBugState.AwakeThisCycle:
// one cycle in 32 or so, rolled again each new cycle here); a hit wakes it.
//
// Not a den creature: a world file's spawn comes out of its den's pipe and
// stays at the hole (SpitOutOfShortCut). Spawned in a room (the tests,
// --creature), it hangs from the ceiling above the spot, facing down (a
// stand-in for the Stowaway placed object, which isn't read).

#include <cstdint>
#include <string>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "stowaway_types.hpp"
#include "world.hpp"

namespace stowaway {
    using creature::body;
    using creature::chunk;
    using creature::consious;

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // StowawayBug.Update (its AI's with it) and the part of
    // StowawayBugGraphics.Update the simulation reads (mouthOpen, biting,
    // digestPrey)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // whether it collides with `t` at all (its maw chunk, only while shut;
    // not what it holds or eats)
    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t);
    // StowawayBug.Collide: nothing of its own
    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j);
    // Creature.Violence (HealthState) with StowawayBug.Stun
    void violence(sys::game_state& state, dcon::creature_id c, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus, creature::damage_type type);
    // StowawayBug.Stun (asleep, a stun wakes it, hidden, a third as long; it
    // lets go of everything)
    void stun(sys::game_state& state, dcon::creature_id c, int32_t st);
    // it lets go of `t` (and stops eating it)
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // StowawayBug.Die, after Creature.Die (nothing of its own)
    void died(sys::game_state& state, dcon::creature_id c);
    // SpitOutOfShortCut: out of a pipe in `room` at `at`, settling at the
    // hole facing out of it
    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, rw::vec2 at);
    // a line for the log
    std::string describe(sys::game_state const& state, dcon::creature_id c);
    // where the tests' spawns may put one (--fuzz, --bench): open air under
    // a ceiling, room below
    bool spawn_spot(world::room_view const& room, rw::ivec2 tile);

    // what the graphics module reads
    // StowawayBug.AttachPos(timeStacker)
    rw::vec2 attach_pos(sys::game_state const& state, dcon::creature_id c, float alpha);
    // StowawayBug.Rad (the heads' rope graphics' thickness)
    float rad(float f);
}
