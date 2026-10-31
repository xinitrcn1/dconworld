#pragma once

// Rain deers (Deer.cs, DeerAI, DeerTentacle on Tentacle, DeerAbstractAI's
// sporePos). Six chunks: the head (mainBodyChunk 0), four body chunks and
// the antlers (5), carried by four legs (DeerTentacle, creature_parts with
// a deer_leg each) gripping the floors below. It wanders its room on its
// tall legs, lies down to rest, goes for the puff balls lying in its room
// and eats them, walks to a puff ball's spore cloud (a summons, from other
// rooms too), and goes home when hurt or when the rain comes. A slugcat
// holding up at the antlers grabs them and climbs about them
// (Deer.PlayerInAntlers) while the deer walks; player.cpp asks `ridden`,
// `jump_off` and `rider_hand`. The dominance games between deer, kneeling
// and Remix's deer behaviour aren't ported (docs/divergences/deer.md).

#include <cstdint>
#include <string>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace deer {
    using creature::body;
    using creature::chunk;
    using creature::consious;

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // Deer.Update (its AI's with it)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // whether it collides with `t` at all
    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t);
    // Deer.Collide: its chunk `i` touched `t`'s chunk `j`
    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j);
    // Deer.Violence + Creature.Violence
    void violence(sys::game_state& state, dcon::creature_id c, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus, creature::damage_type type);
    // it lets go of `t`
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // Deer.Die, after Creature.Die
    void died(sys::game_state& state, dcon::creature_id c);
    // out of a pipe in `room` at `at`
    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, rw::vec2 at);
    // a line for the log
    std::string describe(sys::game_state const& state, dcon::creature_id c);
    // where the tests' spawns may put one (--fuzz, --bench)
    bool spawn_spot(world::room_view const& room, rw::ivec2 tile);

    // the slugcat's Player.playerInAntlers: the deer whose antlers it hangs
    // on (none)
    dcon::creature_id ridden(sys::game_state const& state, dcon::player_id p);
    // Player.Jump's AntlerClimb branch: the slugcat jumps off the antlers
    // (`num`: the jump's strength factor), pushing them back
    void jump_off(sys::game_state& state, dcon::player_id p, float num);
    // SlugcatHand.EngageInMovement in AntlerClimb: where hand `limb` grabs,
    // the antlers' velocity, whether the slugcat dangles; false off the antlers
    bool rider_hand(sys::game_state const& state, dcon::player_id p, uint32_t limb, rw::vec2& pos, rw::vec2& antler_vel, bool& dangle);
    // SporePuffVisionObscurer.AttractADeer: a puff ball burst at `pos`
    // calls a deer of the room (or, sometimes, one of the region's other
    // rooms) over (a transaction)
    void attract_to_spores(sys::game_state& state, dcon::room_id room, rw::vec2 pos);
    // DeerAI.goToPuffBall's puff ball, if any (the graphics look at it)
    dcon::item_id going_to_puff_ball(sys::game_state const& state, dcon::creature_id c);
    // DeerGraphics.Antlers.TransformToHeadRotat of antler point `k` (the
    // flattened index) on `side` (DeerGraphics.CurrentFaceDir: `face_dir`)
    rw::vec2 antler_point_pos(sys::game_state const& state, dcon::creature_id c, uint32_t k, float side, rw::vec2 head, rw::vec2 antler_pos, float face_dir);
    // DeerGraphics.CurrentFaceDir
    float face_dir(sys::game_state const& state, dcon::creature_id c, float alpha);
}
