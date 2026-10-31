#pragma once

// Monster kelps (TentaclePlant.cs, TentaclePlantAI.cs): rooted at a den,
// one long tentacle lashing out at what moves near it and pulling it in.

#include <cstdint>
#include <string>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace tentacle_plant {
    using creature::body;
    using creature::chunk;
    using creature::consious;

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // TentaclePlant.Update (its AI's with it)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // whether it collides with `t` at all
    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t);
    // TentaclePlant.Collide: its chunk `i` touched `t`'s chunk `j`
    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j);
    // TentaclePlant.Violence + Creature.Violence
    void violence(sys::game_state& state, dcon::creature_id c, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus, creature::damage_type type);
    // it lets go of `t`
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // TentaclePlant.Die, after Creature.Die
    void died(sys::game_state& state, dcon::creature_id c);
    // out of a pipe in `room` at `at`
    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, rw::vec2 at);
    // a line for the log
    std::string describe(sys::game_state const& state, dcon::creature_id c);
    // SharedPhysics' projectile trace against the room's kelp tentacles (an
    // appendage): the plant whose tentacle the segment crosses first, and
    // how far along the segment (0 to 1)
    bool trace_tentacle(sys::game_state const& state, dcon::room_id room, rw::vec2 from, rw::vec2 to, dcon::creature_id& hit, float& f);
    // TentaclePlant.Rad: the tentacle's radius at `f` along it (0 the root)
    float rad(float f) noexcept;
    // where the tests' spawns may put one (--fuzz, --bench)
    bool spawn_spot(world::room_view const& room, rw::ivec2 tile);
}
