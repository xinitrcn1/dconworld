#pragma once

// Miros birds (MirosBird, a simpler MirosBirdAI, no flocks): a tall body
// walking on two long legs, a long neck and a beak that snaps at what it
// eats. See miros.cpp.

#include <cstdint>
#include <string>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace miros {
    using creature::body;
    using creature::chunk;
    using creature::consious;

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // MirosBird.Update (its AI's with it)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // whether it collides with `t` at all
    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t);
    // MirosBird.Collide: its chunk `i` touched `t`'s chunk `j`
    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j);
    // MirosBird.Violence + Creature.Violence
    void violence(sys::game_state& state, dcon::creature_id c, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus, creature::damage_type type);
    // it lets go of `t`
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // MirosBird.Die, after Creature.Die
    void died(sys::game_state& state, dcon::creature_id c);
    // out of a pipe in `room` at `at`
    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, rw::vec2 at);
    // a line for the log
    std::string describe(sys::game_state const& state, dcon::creature_id c);
    // where the tests' spawns may put one (--fuzz, --bench)
    bool spawn_spot(world::room_view const& room, rw::ivec2 tile);
}
