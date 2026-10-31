#pragma once

// Pole plants (PoleMimic.cs, no AI): a stalk rooted in a den's pipe,
// standing along the tiles in front of it still and straight like a pole
// (a Tentacle whose chunks are pulled onto every second tile of the line),
// climbable by the slugcat (an IClimbableVine: rot.hpp's vines take it).
// What it eats sticks to its chunks when it touches them; that wakes it: the
// stalk lets go of its spot, its tip hunts the tastiest stuck chunk, grabs
// what it touches and pulls it to the root and into the den. Hurt (or
// dead) it withdraws into its den. A pole plant made away from a pipe
// (`--creature PoleMimic,x,y`, the tests) grows out of the terrain closest
// to where it was made, and is gone when it withdraws.

#include <cstdint>
#include <string>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace pole_mimic {
    using creature::body;
    using creature::chunk;
    using creature::consious;

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // PoleMimic.Update (it has no AI)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // whether it collides with `t` at all
    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t);
    // PoleMimic.Collide: its chunk `i` touched `t`'s chunk `j`
    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j);
    // PoleMimic.Violence + Creature.Violence + PoleMimic.Stun/HitByWeapon
    void violence(sys::game_state& state, dcon::creature_id c, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus, creature::damage_type type);
    // ...on a chunk or on the stalk (an appendage hit: ApplyForceOnAppendage)
    void violence(sys::game_state& state, dcon::creature_id c, creature::hit_part const& part, rw::vec2 momentum, float damage, float stun_bonus,
        creature::damage_type type);
    // it lets go of `t`
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // PoleMimic.Die, after Creature.Die
    void died(sys::game_state& state, dcon::creature_id c);
    // out of a pipe in `room` at `at`: the first time (the world's spawn
    // leaving its den) placed as a pole (Initiate), later SpitOutOfShortCut
    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, rw::vec2 at);
    // a line for the log
    std::string describe(sys::game_state const& state, dcon::creature_id c);
    // where the tests' spawns may put one (--fuzz, --bench): a floor tile
    // with four free tiles above
    bool spawn_spot(world::room_view const& room, rw::ivec2 tile);

    // IClimbableVine (rot::vines): TotalPositions, Pos, Push (Rad is 2, Mass
    // 1), CurrentlyClimbable, BeingClimbedOn
    uint32_t vine_positions(sys::game_state const& state, dcon::creature_id c);
    rw::vec2 vine_pos(sys::game_state const& state, dcon::creature_id c, uint32_t i);
    void vine_push(sys::game_state& state, dcon::creature_id c, uint32_t i, rw::vec2 movement);
    bool currently_climbable(sys::game_state const& state, dcon::creature_id c);
    void being_climbed_on(sys::game_state& state, dcon::creature_id c, dcon::creature_id by);
    inline constexpr float vine_rad = 2.f;
    inline constexpr float vine_mass = 1.f;

    // the room's pole plant stalks a projectile from `from` to `to` crosses
    // (PoleMimic's appendage: the root, then its tentacle chunks): the
    // first one, the segment and how far along the projectile's way (`at`)
    bool trace_stalk(sys::game_state const& state, dcon::room_id room, rw::vec2 from, rw::vec2 to, dcon::creature_id& hit, creature::hit_part& part, float& at);
}
