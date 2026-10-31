#pragma once

// The Watcher's loach (Loach.cs, LoachAI, LoachLeg and LoachTentacle on
// Tentacle, LoachState's interest). A huge grazer: six chunks, the head
// (chunk 0, mainBodyChunk) to the hanging sac at the back (4, 5), carried
// on six legs (LoachLeg, a rain deer's DeerTentacle with a third pair)
// gripping the floors below, with five mouth tentacles (LoachTentacle) that
// sweep the ground under the head. Both are creature_parts with a
// loach_limb each.
//
// What it means for the slugcat: the plain Loach ignores it, but a mouth
// tentacle whose tip touches a creature it has seen, slowly enough, grabs
// it (Touch: it becomes the loach's prey, LoachState.SetNewTarget) and
// recedes to the head, pulling it in; any body chunk touching what a
// tentacle holds starts swallowing it (Loach.Collide -> eatObjects): dead
// halfway through, gone (PermaDie, Destroy) when done. A thrown weapon that
// hits it makes the thrower its prey too (Loach.Violence, through
// `weapon_hit`). While its interest lasts it hunts the prey (and now and
// then lunges: StartLurch), its tentacles reaching for it within their
// 150 px. The RotLoach eats slugcats outright and reaches 250 px.
// It flees what it fears, goes home before the rain and travels the
// rooms' pipes on trips (migrate.hpp). docs/DIVERGENCES.md lists what's
// left out.

#include <cstdint>
#include <string>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace loach {
    using creature::body;
    using creature::chunk;
    using creature::consious;

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos, variant v = variant::loach);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // Loach.Update (its AI's and its limbs' with it)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // whether it collides with `t` at all (not what it is swallowing,
    // not another loach)
    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t);
    // Loach.Collide: its chunk `i` touched `t`'s chunk `j`
    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j);
    // Loach.Violence + Creature.Violence
    void violence(sys::game_state& state, dcon::creature_id c, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus, creature::damage_type type);
    // Loach.Violence's weapon branch: a weapon `thrower` threw hit it (before
    // the violence); the thrower may become its prey
    void weapon_hit(sys::game_state& state, dcon::creature_id c, dcon::creature_id thrower);
    // it lets go of `t` (its tentacles, what it swallows, its prey)
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // Loach.Die, after Creature.Die
    void died(sys::game_state& state, dcon::creature_id c);
    // out of a pipe in `room` at `at` (NewRoom: the legs find the floor)
    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, rw::vec2 at);
    // a line for the log
    std::string describe(sys::game_state const& state, dcon::creature_id c);
    // where the tests' spawns may put one (--fuzz, --bench)
    bool spawn_spot(world::room_view const& room, rw::ivec2 tile);

    // for the graphics: Loach.Rotted, MostDigestedEatObject, the AI's
    // focus creature (null), the HeadDir
    bool rotted(sys::game_state const& state, dcon::creature_id c) noexcept;
    float most_digested(sys::game_state const& state, dcon::creature_id c) noexcept;
    dcon::creature_id focus(sys::game_state const& state, dcon::creature_id c) noexcept;
    // the slugcat `t` is in its tentacles or being swallowed
    bool holds(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) noexcept;
    bool eating(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) noexcept;
}
