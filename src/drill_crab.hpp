#pragma once

// Drill crabs (the Watcher's DrillCrab.cs + DrillCrabAI + DrillCrabPather +
// DrillCrabState; the tower crab, TowerCrab, is the same class with longer
// legs and is the `tower` variant here): a shelled walker of two chunks (the
// body, mainBodyChunk 0, and the drill under it, chunk 1) carried at
// idealHeight over the ground on six jointed legs (creature_parts,
// part_kind::drill_crab_leg; their segments in drill_crab_data). The legs
// step in pairs, each scanning the terrain under where the crab is heading
// (nextStepPos) for a foothold and reaching for it (two bone inverse
// kinematics per joint); with fewer than two legs on the ground gravity comes
// back and it falls. It follows its path (pathing.hpp, its own accessibility
// rule on top) toward a move target idealHeight above the floor.
//
// Its AI only fears: a threat (or a creature it is uncomfortable with) in
// sight makes it flee, and close by it hides in its shell (legs and eyes
// drawn in); a threat that keeps at it (a spear, ThreatTracker) makes it
// spin its drill (spinOutTimer): bouncing off the terrain in sparks and
// smoke, and ramming whatever its drill touches (Violence, blunt). A
// subordinate crab facing a more dominant one kneels. The rain sends it home.
//
// Shots: only the body (chunk 0) takes damage, and only from behind
// (IsDamageBlocked: within 70 degrees of the drill's direction); the drill
// and the shell's front deflect. A leg hit hurts the leg (DrillCrabState
// legHealths: the part's health) and a dead leg goes limp. Stuns under 30
// ticks don't take (Stun override).
//
// No tile is ever written: the drill bounces off the terrain, it doesn't dig.

#include <cstdint>
#include <string>

#include "creature.hpp"
#include "drill_crab_types.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace drill_crab {
    // CreatureTemplate.meatPoints (no meat eating is ported)
    inline constexpr int32_t meat_points = 6;

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos, variant v);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // DrillCrab.Update (its AI's and its legs' with it)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // whether it collides with `t` at all
    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t);
    // DrillCrab.Collide: its spinning drill (chunk 1) rams `t`'s chunk `j`
    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j);
    // DrillCrab.Violence + Creature.Violence: a body chunk, or a leg
    // (`part.tentacle`: the leg, prev_segment/distance_to_next along it)
    void violence(sys::game_state& state, dcon::creature_id c, creature::hit_part const& part, rw::vec2 momentum, float damage, float stun_bonus,
        creature::damage_type type);
    // it holds nothing: nothing to let go of
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // after Creature.Die (nothing of its own)
    void died(sys::game_state& state, dcon::creature_id c);
    // Creature.SpitOutOfShortCut + DrillCrab.PlaceInRoom: out of a pipe in
    // `room` at `at`, its legs reset to the ground under it
    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, rw::vec2 at);
    // a line for the log
    std::string describe(sys::game_state const& state, dcon::creature_id c);
    // where the tests' spawns may put one (--fuzz, --bench): open air over a floor
    bool spawn_spot(world::room_view const& room, rw::ivec2 tile);

    // SharedPhysics.TraceProjectileAgainstBodyChunks' appendage half for the
    // drill crabs of `room` (IHaveAppendages: the legs, from the body chunk
    // through their segments): the first leg the segment from `from` to `to`
    // passes within a leg's thickness of, `at` how far along (0..1)
    bool trace_legs(sys::game_state const& state, dcon::room_id room, rw::vec2 from, rw::vec2 to, dcon::creature_id thrown_by, dcon::creature_id& hit,
        creature::hit_part& part, float& at);

    // What the graphics module reads of it
    variant variant_of(sys::game_state const& state, dcon::creature_id c) noexcept;
    // the leg's segment count (5, the tower crab's 6) and Segment.prevDist
    uint32_t leg_segments(variant v) noexcept;
    float segment_length(sys::game_state const& state, dcon::creature_id c, uint32_t leg, uint32_t seg) noexcept;
    // DrillCrab.Hiding
    bool hiding(sys::game_state const& state, dcon::creature_id c) noexcept;
}
