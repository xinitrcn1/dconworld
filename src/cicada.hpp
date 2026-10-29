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

#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace cicada {
    // Cicada constructor: `gender` true is a CicadaA (white), false a CicadaB (dark)
    dcon::cicada_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos, bool gender);
    void destroy(sys::game_state& state, dcon::cicada_id c);
    // Cicada.Update (+ CicadaGraphics.Update) for every cicada in the room
    void update_room(sys::game_state& state, world::room_view const& room);
    // Room object collisions between cicadas and slugcats (pushing apart,
    // Cicada.Collide's bump attacks and tentacle clinging) and between cicadas
    void collide(sys::game_state& state, world::room_view const& room);
    // Weapon.Update's projectile trace against cicadas: the cicada the
    // segment hits (radius `rad`), and which chunk
    bool trace(sys::game_state const& state, dcon::room_id room, rw::vec2 from, rw::vec2 to, float rad, dcon::cicada_id& hit, uint32_t& chunk);
    // Creature.Violence (+ Cicada.Stun)
    void violence(sys::game_state& state, dcon::cicada_id c, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus);

    dcon::physical_object_id body(sys::game_state const& state, dcon::cicada_id c) noexcept;
    // bodyChunks[i]: 0 the head (mainBodyChunk), 1 the body
    dcon::body_chunk_id chunk(sys::game_state const& state, dcon::cicada_id c, uint32_t i) noexcept;
    bool consious(sys::game_state const& state, dcon::cicada_id c) noexcept;
    // Cicada.Charging
    bool charging(sys::game_state const& state, dcon::cicada_id c) noexcept;

    // The player side: a slugcat carrying a cicada in both hands (Player
    // grasps[0], a HeavyCarry TwoHands object)
    // the cicada `p` holds, if any
    dcon::cicada_id held_by(sys::game_state const& state, dcon::player_id p) noexcept;
    // Cicada.LiftPlayerPower
    float lift_player_power(sys::game_state const& state, dcon::cicada_id c) noexcept;
    void set_currently_lifting_player(sys::game_state& state, dcon::cicada_id c, bool lifting) noexcept;
    // Player.PickupCandidate over the cicadas (Player.CanIPickThisUp,
    // Grabability TwoHands): the best one and its score, lower is better
    dcon::cicada_id pickup_candidate(sys::game_state const& state, dcon::player_id p, world::room_view const& room, float& score);
    // Player.SlugcatGrab + Creature.Grab: held by `p`
    void grab(sys::game_state& state, dcon::player_id p, dcon::cicada_id c);
    // Creature.ReleaseGrasp from the player's side
    void release(sys::game_state& state, dcon::cicada_id c);
    // releases the cicada `p` holds and any cicada clinging to `p`
    void release_player(sys::game_state& state, dcon::player_id p);
    // Player.GraphicsModuleUpdated's HeavyCarry pull: the held cicada and
    // the slugcat kept 30 apart, by mass
    void carry_update(sys::game_state& state, dcon::player_id p);
    // the held cicada travels through a shortcut with `p`: moved to `room`,
    // and (when it comes out) placed on its main chunk
    void follow_player(sys::game_state& state, dcon::player_id p, dcon::room_id room, bool spit_out);
}
