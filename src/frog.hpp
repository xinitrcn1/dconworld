#pragma once

// Frogs (the Watcher's Frog.cs + FrogAI.cs + FrogState.cs): a small two
// chunk hopper (the head, chunk 0, and the belly, chunk 1) walking the
// floors and poles of its room in little hops, sitting between them and
// croaking now and then. It hunts slugcats, scavengers, lizards, vultures
// and egg bugs (the relationship table: Eats), following its prey; it never
// pounces on its own (Frog.Attack is never called in vanilla): what it hits
// while hopping along its path (Frog.ThrowUpdate's trace while `jumping`),
// or what a slugcat throws it at, it latches on to (Frog.Attach): it holds
// the creature's chunk (grasps[0]), stops colliding with anything and sucks
// blood (SuckFood): from a slugcat its food pips, one every 40 to 80
// seconds (in a shelter it waits), until the slugcat has none left or the
// frog is full, when it bites hard (a long stun, the slugcat exhausted) and
// hops off as soon as the slugcat comes to; from any other creature a small
// bite every 11 to 27 seconds. Each feed swells its belly (FrogState.feedSize,
// up to 5); from 3 on it is a "blood bank": afraid of slugcats, scavengers
// and vultures, too heavy to latch on to anything, slower jumps, and the
// other frogs of the room follow it around (FrogAI.PackLeader). A latched
// frog can't be hurt (Frog.Violence) nor picked up; stunned it lets go.
//
// Its template has no HealthState (FrogState): baseDamageResistance 0 makes
// any damage a kill and no hit stuns it (Creature.Violence's divisions by 0).
//
// The slugcat side: a frog is picked up one handed (Player.Grabability
// OneHand, CantGrab while latched on); held, it latches on to whatever
// comes close to it (the slugcat holding it excepted: ImmuneToLatch); a
// thrown frog (not a blood bank) flies like a weapon (Frog.Thrown) and
// latches on to what it hits; a blood bank is tossed like any creature, and
// it is the only frog a slugcat can eat (IPlayerEdible: 3 bites, 1 pip).

#include <cstdint>
#include <string>

#include "creature.hpp"
#include "frog_types.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace frog {
    using creature::body;
    using creature::chunk;
    using creature::consious;

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // Frog.Update (its AI's with it)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // whether it collides with `t` at all (Frog.setCollisions: not while
    // latched on; not what holds it)
    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t);
    // Frog.Collide: a frog thrown (throwLatch) latches on to what it touches
    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j);
    // Frog.Violence (nothing while latched on) + Creature.Violence without a
    // HealthState: any damage kills it
    void violence(sys::game_state& state, dcon::creature_id c, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus, creature::damage_type type);
    // it lets go of `t` if it is latched on to it (Frog.Detach)
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // Frog.Die: Detach, after Creature.Die
    void died(sys::game_state& state, dcon::creature_id c);
    // Frog.SpitOutOfShortCut: out of a pipe in `room` at `at`
    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, rw::vec2 at);
    // a line for the log
    std::string describe(sys::game_state const& state, dcon::creature_id c);
    // where the tests' spawns may put one (--fuzz, --bench): open air over
    // a floor, dry
    bool spawn_spot(world::room_view const& room, rw::ivec2 tile);

    // what the graphics and the others read
    // the creature it is latched on to (Frog.attachedObject), none
    dcon::creature_id attached_to(sys::game_state const& state, dcon::creature_id c) noexcept;
    // Frog.bloodBank (FrogState.feedSize >= 3), bloodBankBloat
    bool blood_bank(sys::game_state const& state, dcon::creature_id c) noexcept;
    float blood_bank_bloat(sys::game_state const& state, dcon::creature_id c) noexcept;
    // Frog.Croaking
    float croaking(sys::game_state const& state, dcon::creature_id c) noexcept;
    // Frog.DigIn
    float dig_in(sys::game_state const& state, dcon::creature_id c) noexcept;

    //
    // The slugcat side (items.cpp's one handed grab, throw and eat loop and
    // shortcuts.cpp's carry through pipes call these the way they call
    // barnacle.hpp's)
    //
    // the frog in `p`'s hand `hand`, in either hand
    dcon::creature_id held_in_hand(sys::game_state const& state, dcon::player_id p, uint32_t hand) noexcept;
    dcon::creature_id held_by(sys::game_state const& state, dcon::player_id p) noexcept;
    // Player.PickupCandidate over the frogs (Grabability OneHand, CantGrab
    // latched on): the best one and its score, lower is better
    dcon::creature_id pickup_candidate(sys::game_state const& state, dcon::player_id p, world::room_view const& room, float& score);
    // Player.SlugcatGrab: into the hand
    void grab(sys::game_state& state, dcon::player_id p, dcon::creature_id c, uint32_t hand);
    // Creature.ReleaseGrasp from the slugcat's side
    void release(sys::game_state& state, dcon::creature_id c);
    void release_player(sys::game_state& state, dcon::player_id p);
    // the held frog at the hand's position; Player.Update's grasp loop: a
    // held frog latches on (throwLatch) to anything but its holder
    // (ImmuneToLatch)
    void carry_update(sys::game_state& state, dcon::player_id p, uint32_t hand, rw::vec2 hand_pos);
    // the held frog travels a shortcut with `p`
    void follow_player(sys::game_state& state, dcon::player_id p, dcon::room_id room, bool spit_out);
    // Player.ThrowObject for a frog: true when it is thrown as a projectile
    // (not a blood bank: let go of here, Frog.Thrown with throwLatch and
    // jumpStun 40); false for a blood bank, which the caller tosses like
    // any creature (TossObject) after calling `tossed`
    // `throw_dir`: Player.ThrowDirection's IntVector2, `force`: Lerp(1.25,
    // 1.75, Adrenaline)
    bool thrown(sys::game_state& state, dcon::player_id p, dcon::creature_id c, rw::ivec2 throw_dir, float force);
    // a blood bank tossed: ImmuneToLatch, throwLatch, jumpStun 40
    void tossed(sys::game_state& state, dcon::player_id p, dcon::creature_id c);
    // IPlayerEdible.Edible (a blood bank only)
    bool edible(sys::game_state const& state, dcon::creature_id c) noexcept;
    // Frog.BitByPlayer: a bite (it dies of the first); true when it is all
    // eaten (ObjectEaten: the caller destroys it)
    bool bit_by_player(sys::game_state& state, dcon::creature_id c, dcon::player_id p);
}
