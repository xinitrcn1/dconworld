#pragma once

// Moth grubs (the Watcher's MothGrub.cs + MothGrubAI.cs): the big moths'
// young, a plump five chunk larva (a head, chunk 0, on a body of four
// joined chunks) lying about where it was put. It has no legs to move by:
// vanilla's MothGrub.Act follows its path only to switch GoThroughFloors
// and to slip into a pipe it lies at (MovementConnection ShortCut), so a
// grub out of its den stays at the den's mouth and, as the rain comes
// (RainTracker: EscapeRain), slips back in (dens.hpp). Its AI watches the
// creatures around it (CreatureLooker, not the small ones) and turns its
// head to the one it looks at; now and then, on its back on the ground,
// it rolls itself upright (selfRightState). It fears what the relationship
// table says it fears (ThreatTracker: Flee) without any way to run.
//
// For the slugcat: harmless, and always grabbable (Player.Grabability
// Drag, alive or dead): picked up by its closest chunk into grasps[0] (a
// HeavyCarry creature, like the jetfish's TwoHands), dragged along at a
// distance of 5 + the chunk's radius and dropped when it gets more than
// twice that and 30 px away; carried through pipes with the slugcat. Hits
// on its head do double damage and stun (MothGrub.Violence); a HealthState
// creature with baseDamageResistance 2.3.

#include <cstdint>
#include <string>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace moth_grub {
    using creature::body;
    using creature::chunk;
    using creature::consious;

    // CreatureTemplate.meatPoints (what a carnivore would get out of it;
    // no meat eating is ported)
    inline constexpr int32_t meat_points = 5;

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // MothGrub.Update (its AI's with it)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // whether it collides with `t` at all (not what holds it)
    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t);
    // MothGrub has no Collide of its own
    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j);
    // MothGrub.Violence (double on the head) + Creature.Violence
    void violence(sys::game_state& state, dcon::creature_id c, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus, creature::damage_type type);
    // it holds nothing (no grasps): nothing to let go of
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // MothGrub.Die, after Creature.Die (nothing of its own)
    void died(sys::game_state& state, dcon::creature_id c);
    // Creature.SpitOutOfShortCut: out of a pipe in `room` at `at`
    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, rw::vec2 at);
    // a line for the log
    std::string describe(sys::game_state const& state, dcon::creature_id c);
    // where the tests' spawns may put one (--fuzz, --bench): open air over
    // a floor
    bool spawn_spot(world::room_view const& room, rw::ivec2 tile);

    // The player side: a slugcat dragging a grub (Player grasps[0],
    // Grabability Drag, HeavyCarry)
    // the grub `p` holds, if any
    dcon::creature_id held_by(sys::game_state const& state, dcon::player_id p) noexcept;
    // Player.PickupCandidate over the grubs (CanIPickThisUp's closest
    // chunk): the best one and its score, lower is better
    dcon::creature_id pickup_candidate(sys::game_state const& state, dcon::player_id p, world::room_view const& room, float& score);
    // Player.SlugcatGrab + Creature.Grab: held by `p` by its chunk closest
    // to the slugcat
    void grab(sys::game_state& state, dcon::player_id p, dcon::creature_id c);
    // Creature.ReleaseGrasp from the player's side
    void release(sys::game_state& state, dcon::creature_id c);
    // lets go of the grub `p` holds
    void release_player(sys::game_state& state, dcon::player_id p);
    // Player.GrabUpdate's HeavyCarry pull: the held chunk and the slugcat
    // kept 5 + its radius apart, by mass; a Drag object too far is let go of
    void carry_update(sys::game_state& state, dcon::player_id p);
    // the held grub travels through a shortcut with `p`: moved to `room`,
    // and (when it comes out) placed on the slugcat's main chunk
    void follow_player(sys::game_state& state, dcon::player_id p, dcon::room_id room, bool spit_out);
}
