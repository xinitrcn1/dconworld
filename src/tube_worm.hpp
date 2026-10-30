#pragma once

// Grapple worms (TubeWorm.cs + TubeWormAI): two chunks with a tongue at
// each end. A tongue shoots out, sticks to the terrain (or a creature) and
// pulls its end there on a Rope that wraps the tile corners; the worm
// inches along the rope between its two tongues and flops from grip to
// grip, grappling up to spots it can't walk to (FindInaccessibleGrapplingPos).
// TubeWormAI only picks random destinations in its room and follows its
// StandardPather (floors and corridors) there; it doesn't flee the rain.
//
// A slugcat picks one up with one hand (Grabability OneHand, one at a
// time): held, the front tongue shoots from the slugcat and pulls it, and
// the slugcat's jump in the air shoots it at what ProperAutoAim finds (or
// lets go): the slugcat hangs from it and swings (Player's GrapplingSwing).
// No Remix (MMF): no old tongue option, the jump in a vine lets go of it.

#include <cstdint>
#include <string>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace tube_worm {
    using creature::body;
    using creature::chunk;
    using creature::consious;

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // TubeWorm.Update (its AI's with it)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // whether it collides with a creature or slugcat (not the one holding it)
    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t);
    // TubeWorm.Collide: it wakes up
    void collide(sys::game_state& state, dcon::creature_id c);
    // TubeWorm.Violence (each tongue lets go half the time) + Creature.Violence
    void violence(sys::game_state& state, dcon::creature_id c, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus);
    // its tongues let go of `t`
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // TubeWorm.Die, after Creature.Die: the slugcat holding it lets go, the
    // tongues let go
    void died(sys::game_state& state, dcon::creature_id c);
    // TubeWorm.SpitOutOfShortCut / NewRoom: the tongues back in, at `at`
    void place_in_room(sys::game_state& state, dcon::creature_id c, rw::vec2 at);

    // the tongues (0 the front one) and what TubeWormGraphics reads of them
    tongue const& tongue_of(sys::game_state const& state, dcon::creature_id c, uint32_t i) noexcept;
    // TubeWorm.totalRope, RopeStretchFac
    float total_rope(sys::game_state const& state, dcon::creature_id c) noexcept;
    float rope_stretch_fac(sys::game_state const& state, dcon::creature_id c) noexcept;
    // the tongues' modes, for the log
    std::string describe_tongues(sys::game_state const& state, dcon::creature_id c);

    // The player side: a grapple worm in a slugcat's hand (Grabability OneHand)
    // the worm `p` holds in `hand`, if any
    dcon::creature_id held_in_hand(sys::game_state const& state, dcon::player_id p, uint32_t hand) noexcept;
    // Player.tubeWorm: the one it holds in either hand
    dcon::creature_id held_by(sys::game_state const& state, dcon::player_id p) noexcept;
    // Player.PickupCandidate over the grapple worms (none while it holds
    // one: CantGrab): the best one and its score, lower is better
    dcon::creature_id pickup_candidate(sys::game_state const& state, dcon::player_id p, world::room_view const& room, float& score);
    // Player.SlugcatGrab + Creature.Grab: held by `p` in `hand`, by its chunk 0
    void grab(sys::game_state& state, dcon::player_id p, dcon::creature_id c, uint32_t hand);
    // Creature.ReleaseGrasp from the player's side
    void release(sys::game_state& state, dcon::creature_id c);
    // lets go of whatever grapple worm `p` holds
    void release_player(sys::game_state& state, dcon::player_id p);
    // Player.GraphicsModuleUpdated: the held chunk in the hand
    void carry_update(sys::game_state& state, dcon::player_id p, uint32_t hand, rw::vec2 hand_pos);
    // the worms `p` holds travel through a shortcut or a warp with it:
    // moved to `room`, and (when it comes out) put in its hands
    void follow_player(sys::game_state& state, dcon::player_id p, dcon::room_id room, bool spit_out);
    // TubeWorm.JumpButton: the slugcat pressed jump holding `c`; false when
    // the worm takes the jump (its tongue shoots out or lets go)
    bool jump_button(sys::game_state& state, dcon::creature_id c, dcon::player_id p);
    // its front tongue holds on (Player's GrapplingSwing), and where
    // (Tongue.AttachedPos)
    bool swinging(sys::game_state const& state, dcon::creature_id c) noexcept;
    rw::vec2 attached_pos(sys::game_state const& state, dcon::creature_id c) noexcept;
}
