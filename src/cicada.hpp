#pragma once

// Squidcada (Cicada.cs, CicadaAI): a two chunk flying creature, white
// (CicadaA) or dark (CicadaB). It flutters about the room, lands on walls,
// ceilings and poles to sit, circles with other squidcadas, and pesters a
// slugcat coming too close: bump attacks (charging into it) and clinging to
// it with its tentacles. A slugcat can grab one with both hands and get
// lifted by it, slowing its fall and boosting its jumps until the
// squidcada tires.
//
// The AI is CicadaAI's utility comparison (antagonizing the slugcat as
// prey, fear of an armed one, being stuck) over a simplified tracker (sight
// of the slugcat and of the other squidcadas); its paths come from
// pathing.hpp's shared, on demand distance fields with CicadaPather's
// preference for open air.

#include <cstdint>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace cicada {
    // bodyChunks[i]: 0 the head (mainBodyChunk), 1 the body
    using creature::body;
    using creature::chunk;
    using creature::consious;

    // Cicada constructor: `gender` true is a CicadaA (white), false a CicadaB (dark)
    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos, bool gender);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // Cicada.Update
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // Room object collisions: whether it collides with a creature or
    // slugcat (not the one holding it), Cicada.Collide's bump attacks and
    // tentacle clinging (its chunk `i` touched the other's `j`), bumping
    // into another cicada while charging
    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t);
    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j);
    void collide_kin(sys::game_state& state, dcon::creature_id c, uint32_t i, dcon::creature_id other, uint32_t j);
    // Creature.Violence (+ Cicada.Stun)
    void violence(sys::game_state& state, dcon::creature_id c, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus);
    // Cicada.Charging
    bool charging(sys::game_state const& state, dcon::creature_id c) noexcept;

    // The player side: a slugcat carrying a cicada in both hands (Player
    // grasps[0], a HeavyCarry TwoHands object)
    // the cicada `p` holds, if any
    dcon::creature_id held_by(sys::game_state const& state, dcon::player_id p) noexcept;
    // Cicada.LiftPlayerPower
    float lift_player_power(sys::game_state const& state, dcon::creature_id c) noexcept;
    void set_currently_lifting_player(sys::game_state& state, dcon::creature_id c, bool lifting) noexcept;
    // Player.PickupCandidate over the cicadas (Player.CanIPickThisUp,
    // Grabability TwoHands): the best one and its score, lower is better
    dcon::creature_id pickup_candidate(sys::game_state const& state, dcon::player_id p, world::room_view const& room, float& score);
    // Player.SlugcatGrab + Creature.Grab: held by `p`
    void grab(sys::game_state& state, dcon::player_id p, dcon::creature_id c);
    // Creature.ReleaseGrasp from the player's side
    void release(sys::game_state& state, dcon::creature_id c);
    // lets go of `t`: held by it, clinging to it
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // releases the cicada `p` holds and any cicada clinging to `p`
    void release_player(sys::game_state& state, dcon::player_id p);
    // Player.GraphicsModuleUpdated's HeavyCarry pull: the held cicada and
    // the slugcat kept 30 apart, by mass
    void carry_update(sys::game_state& state, dcon::player_id p);
    // the held cicada travels through a shortcut with `p`: moved to `room`,
    // and (when it comes out) placed on its main chunk
    void follow_player(sys::game_state& state, dcon::player_id p, dcon::room_id room, bool spit_out);
}
