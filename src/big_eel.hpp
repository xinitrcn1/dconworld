#pragma once

// Leviathans (BigEel.cs + BigEelAI + BigEelPather, BigEelAbstractAI's room
// to room roaming): a huge swimmer, twenty chunks from the head (chunk 0,
// mainBodyChunk) to the tail, 10 to 60 px wide, swimming in deep water with
// a sideways wave. It hunts what it eats under the surface (PreyTracker,
// relationships faded out above the water level and for small bodies) and
// charges its jaws (jawCharge) when the prey is within 200 px: at 0.35
// the jaws snap (JawsSnap), and everything in the bite area (a box 120 px
// wide and 100 px deep in front of the head) is caught: the creatures die,
// the chunks are held in the jaws (clampedObjects, their connections
// slack: Crush) and pulled in as the charge runs on to 1, when it
// swallows (Swallow: what it caught is gone; a slugcat is eaten).
//
// What it means for the slugcat: a slugcat swimming deep near one is
// prey; caught in the jaws it dies at once, is dragged into the mouth and
// eaten. The slugcat's state is only written from its own room's update
// (the leviathan is in the same room), like the loach's and the monster
// kelp's eating.
//
// It lives off screen at sea (the world files spawn it OFFSCREEN, see
// `spawn_offscreen`): it swims into a room with a sea exit (deep water
// open at a side of the room) over that side, stays a while, and swims out
// over a side again to roam to a neighbouring sea room (BigEelAbstractAI:
// every 1200 ticks it may pick one) or away from the rain (EscapeRain),
// waiting at sea until the rain is over. docs/DIVERGENCES.md lists what
// is left out.

#include <cstdint>
#include <string>

#include "big_eel_types.hpp"
#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace spawns {
    struct spawn;
    struct report;
}

namespace big_eel {
    using creature::body;
    using creature::chunk;
    using creature::consious;

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // BigEel.Update (its AI's with it)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // whether it collides with `t` at all (not what is in its jaws, not
    // while off at sea)
    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t);
    // BigEel.Collide: nothing of its own
    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j);
    // BigEel.Stun + Creature.Violence (baseDamageResistance and
    // baseStunResistance 1000)
    void violence(sys::game_state& state, dcon::creature_id c, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus, creature::damage_type type);
    // lets go of `t` (out of its jaws)
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // after Creature.Die (no Remix: nothing of its own)
    void died(sys::game_state& state, dcon::creature_id c);
    // out of a pipe in `room` at `at`
    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, rw::vec2 at);
    // a line for the log
    std::string describe(sys::game_state const& state, dcon::creature_id c);
    // where the tests' spawns may put one (--fuzz, --bench): deep water 4
    // tiles off the terrain in a room with a sea exit
    bool spawn_spot(world::room_view const& room, rw::ivec2 tile);

    // the world file's OFFSCREEN spawns (`0-Leviathan-2`): each made off at
    // sea beside a room of `region` with a sea exit, to swim in after 100 to
    // 1000 ticks; counted in `r` like spawns::spawn_in_den
    uint32_t spawn_offscreen(sys::game_state& state, dcon::region_id region, spawns::spawn const& s, spawns::report& r);
    // off at sea: not out in its room (not drawn, nothing touches it)
    bool hidden(sys::game_state const& state, dcon::creature_id c) noexcept;
    // a side of the room (-1 left, 1 right) open to the sea: the rows
    // [lo, hi] of deep water whose border tiles are open (false: none)
    // (borders.hpp's sea exits on that edge, 8 rows at least)
    bool sea_exit(sys::game_state const& state, world::room_view const& room, int32_t side, int32_t& lo, int32_t& hi);

    // for the graphics: the IndividualVariations, the payload
    individual_variations const& ivars(sys::game_state const& state, dcon::creature_id c) noexcept;
    creature::big_eel_payload const& state_of(sys::game_state const& state, dcon::creature_id c) noexcept;
    // AbstractCreature.IsVoided ({Voidsea}: the gold void creature)
    bool voided(sys::game_state const& state, dcon::creature_id c) noexcept;
}
