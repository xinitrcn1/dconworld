#pragma once

// Yeeks (Downpour's Yeek.cs + YeekAI + YeekState): a small fluffy hopper of
// two chunks (body and head), shy and harmless. It gets about by hops
// (Yeek.Hop, Jump: a kick of the body chunk towards the next tile of its
// path, higher and farther the more frightened it is), climbs poles and
// crawls corridors. YeekAI, simplified: it picks idle spots near the other
// yeeks of its squad (IdlePosScore: close to the ones likely to lead, far
// from what it fears) and sits there until bored; bored of the room it
// leaves (MakeCreatureLeaveRoom); everything else is a threat (Afraid): it
// flees (ThreatTracker.FleeTo, jumping away from what comes within 80),
// and when something it fears surprises it close by, the whole squad jumps
// away in panic and leaves the room (SurprisedFearReaction). A thrown
// weapon flying at it scares it off too (FlyingWeapon). It goes home before
// the rain (RainTracker, dens.hpp). Lizards, dropwigs and vultures eat it,
// scavengers attack it (relationships.cpp).
//
// The slugcat side: any slugcat picks one up in both hands (Grabability
// TwoHands, HeavyCarry; live ones aren't stunned by it,
// IsCreatureImmuneToPlayerGrabStun). Held, it weighs nearly nothing
// (SetPlayerHoldingBodyMass) and hops now and then, or when the slugcat
// jumps, lifting the slugcat with it (controlledJumpVelocity): a jump boost.
// A dead one is meat for the carnivores (Hunter, Artificer, Gourmand,
// Inv: Player.CanEatMeat; meatPoints 2, a pip a bite, Gourmand a half).

#include <cstdint>
#include <string>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"
#include "yeek_types.hpp"

namespace yeek {
    using creature::body;
    using creature::chunk;
    using creature::consious;

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // Yeek.Update (its AI's with it)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // whether it collides with `t` at all (not what holds it)
    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t);
    // Yeek.Collide: its chunk `i` touched `t`'s chunk `j` (it only grabs
    // fruit: nothing with a creature)
    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j);
    // Creature.Violence (HealthState)
    void violence(sys::game_state& state, dcon::creature_id c, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus, creature::damage_type type);
    // it lets go of `t` (it never holds a creature)
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // after Creature.Die
    void died(sys::game_state& state, dcon::creature_id c);
    // Yeek.SpitOutOfShortCut + YeekAI.NewRoom: out of a pipe in `room` at `at`
    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, rw::vec2 at);
    // a line for the log
    std::string describe(sys::game_state const& state, dcon::creature_id c);
    // where the tests' spawns may put one (--fuzz, --bench): on the ground
    bool spawn_spot(world::room_view const& room, rw::ivec2 tile);
    // Yeek.FlyingWeapon (Weapon.INotifyOfFlyingWeapons): a thrown weapon at
    // `pos` (it jumps away and leaves the room)
    void flying_weapon(sys::game_state& state, dcon::creature_id c, rw::vec2 pos);

    // for the graphics: Yeek.OnGround, GetClimbingMode, GetTunnelMode,
    // GroupLeaderPotential, the personality and the looks
    bool on_ground(sys::game_state const& state, dcon::creature_id c, world::room_view const& room) noexcept;
    bool climbing_mode(sys::game_state const& state, dcon::creature_id c) noexcept;
    bool tunnel_mode(sys::game_state const& state, dcon::creature_id c) noexcept;
    personality const& personality_of(sys::game_state const& state, dcon::creature_id c) noexcept;
    looks const& looks_of(sys::game_state const& state, dcon::creature_id c) noexcept;
    // YeekAI.focusCreature (none)
    dcon::creature_id focus(sys::game_state const& state, dcon::creature_id c) noexcept;

    //
    // The player side: a slugcat holding a yeek in both hands (Player
    // grasps[0], a HeavyCarry TwoHands creature, like the jetfish)
    //
    // the yeek `p` holds, if any
    dcon::creature_id held_by(sys::game_state const& state, dcon::player_id p) noexcept;
    // Player.PickupCandidate over the yeeks (Grabability TwoHands, dead or
    // alive): the best one and its score, lower is better
    dcon::creature_id pickup_candidate(sys::game_state const& state, dcon::player_id p, world::room_view const& room, float& score);
    // Player.SlugcatGrab + Creature.Grab: held by `p` by its chunk 0
    void grab(sys::game_state& state, dcon::player_id p, dcon::creature_id c);
    // Creature.ReleaseGrasp from the player's side
    void release(sys::game_state& state, dcon::creature_id c);
    // lets go of the yeek `p` holds
    void release_player(sys::game_state& state, dcon::player_id p);
    // Player.GraphicsModuleUpdated's HeavyCarry pull: the held yeek and the
    // slugcat kept 5 + its chunk's radius apart, by mass
    void carry_update(sys::game_state& state, dcon::player_id p);
    // the held yeek travels through a shortcut with `p`: moved to `room`,
    // and (when it comes out) placed on its main chunk
    void follow_player(sys::game_state& state, dcon::player_id p, dcon::room_id room, bool spit_out);

    // Player.GrabUpdate's meat eating of a held dead yeek (CanEatMeat:
    // Hunter, Artificer, Gourmand, Inv; Template.meatPoints 2, EatMeatUpdate
    // with the yeek's own count of the eating, Player.eatMeat)
    enum class meat : uint8_t {
        // not eating: GrabUpdate goes on as usual
        none,
        // pick up held down over the meat: GrabUpdate returns
        eating,
        // done (all eaten or full, or let go of after a while): the caller
        // tosses it and lets go (TossObject, ReleaseGrasp, wantToPickUp 0)
        done,
    };
    // `full`: the stomach is full (FoodInStomach >= MaxFoodInStomach);
    // `quarters`: the food a bite gave (4 a pip, the Gourmand's 2)
    meat eat_meat_update(sys::game_state& state, dcon::player_id p, bool full, int32_t& quarters);
}
