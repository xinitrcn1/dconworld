#pragma once

// Jetfish (JetFish.cs + JetFishAI): two chunks swimming head first, a jet of
// water out of the tentacles when the tail end breaks the surface (JetFish.
// Swim), flopping and hopping about out of the water. It never drowns nor
// suffocates (vanilla has neither for it). JetFishAI, simplified: it swims
// about its room (random spots in sight, now and then a far one: the
// exploreCoordinate), flees what it is afraid of (ThreatTracker), hunts what
// it eats or antagonizes (PreyTracker): it rams it (Violence over 12 of
// speed), pushes what is under it down while its attackCounter runs and
// then backs off, grabs small or dead prey in its mouth and takes it home
// (ReturnPrey), and goes home before the rain (RainTracker, dens.hpp). The
// slugcat is SocialDependent: a fish that doesn't like it (SocialMemory)
// antagonizes it near the surface, and holding a fish makes it like the
// slugcat less.
//
// A slugcat picks one up in both hands (Grabability TwoHands while
// `grabable`: out of the water, near the surface, or bumped gently) and
// rides it: the fish swims where the slugcat's input points
// (GrabbedByPlayer), the slugcat's own swimming is off (Player's DeepSwim
// and SurfaceSwim with a conscious jetfish in grasps[0]) and the HeavyCarry
// pull drags it along. No Remix (MMF): the old speeds, no personality.

#include <cstdint>
#include <string>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace jetfish {
    using creature::body;
    using creature::chunk;
    using creature::consious;

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // JetFish.Update (its AI's with it)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // whether it collides with `t` at all (not what holds it, nor its prey)
    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t);
    // JetFish.Collide: its chunk `i` touched `t`'s chunk `j`
    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j);
    // JetFish.Stun (half the time it lets go) + Creature.Violence
    void violence(sys::game_state& state, dcon::creature_id c, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus, creature::damage_type type);
    // it lets go of `t`
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // JetFish.Die, after Creature.Die: it sinks, the jet stops
    void died(sys::game_state& state, dcon::creature_id c);
    // JetFish.SpitOutOfShortCut: out of a pipe in `room` at `at`, head first
    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, rw::vec2 at);
    // a line for the log
    std::string describe(sys::game_state const& state, dcon::creature_id c);
    // where the tests' spawns may put one (--fuzz, --bench): under water
    bool spawn_spot(world::room_view const& room, rw::ivec2 tile);

    // JetFish.iVars and albino, for the graphics
    individual_variations const& ivars(sys::game_state const& state, dcon::creature_id c) noexcept;

    // The player side: a slugcat holding a jetfish in both hands (Player
    // grasps[0], a HeavyCarry TwoHands creature) and riding it
    // the jetfish `p` holds, if any
    dcon::creature_id held_by(sys::game_state const& state, dcon::player_id p) noexcept;
    // it holds a conscious one: the fish swims for it (Player's DeepSwim and
    // SurfaceSwim skip their own strokes, waterFriction 1)
    bool riding(sys::game_state const& state, dcon::player_id p) noexcept;
    // Player.PickupCandidate over the jetfish (Grabability TwoHands while
    // JetFish.grabable): the best one and its score, lower is better
    dcon::creature_id pickup_candidate(sys::game_state const& state, dcon::player_id p, world::room_view const& room, float& score);
    // Player.SlugcatGrab + Creature.Grab: held by `p` by its chunk 0
    void grab(sys::game_state& state, dcon::player_id p, dcon::creature_id c);
    // Creature.ReleaseGrasp from the player's side
    void release(sys::game_state& state, dcon::creature_id c);
    // lets go of the jetfish `p` holds
    void release_player(sys::game_state& state, dcon::player_id p);
    // Player.GraphicsModuleUpdated's HeavyCarry pull: the held fish and the
    // slugcat kept 5 + its head's radius apart, by mass
    void carry_update(sys::game_state& state, dcon::player_id p);
    // the held fish travels through a shortcut with `p`: moved to `room`,
    // and (when it comes out) placed on its main chunk
    void follow_player(sys::game_state& state, dcon::player_id p, dcon::room_id room, bool spit_out);
}
