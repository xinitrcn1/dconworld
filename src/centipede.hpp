#pragma once

// Centipede (Centipede.cs, CentipedeAI): a chain of body chunks that crawls
// along any surface, either end leading, grabs prey with its ends and shocks
// it once both ends hold on. Sizes 0..1 like Centipede.GenerateSize: big ones
// outweigh the slugcat, hunt it and kill it with a shock; small ones are
// afraid of it and only stun it.
//
// The AI is CentipedeAI's utility comparison over a simplified tracker (sight
// of the slugcat, prey/threat by total mass, injury) and its paths come from
// pathing.hpp's shared, on demand distance fields.

#include <cstdint>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace centipede {
    using creature::body;
    using creature::chunk;
    using creature::chunk_count;
    using creature::total_mass;

    // `size` < 0 rolls one like Centipede.GenerateSize
    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos, float size = -1.f);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // Centipede.Update
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // Centipede.Collide: its chunk `i` touched the slugcat's chunk `j`
    // (pushed apart already)
    void collide_player(sys::game_state& state, dcon::creature_id c, uint32_t i, dcon::player_id p, uint32_t j);
    // Creature.Violence (+ Centipede.Violence)
    void violence(sys::game_state& state, dcon::creature_id c, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus);

    // Centipede.HeadIndex
    uint32_t head_index(sys::game_state const& state, dcon::creature_id c) noexcept;
    // lets go of `p` if it holds it
    void release_player(sys::game_state& state, dcon::creature_id c, dcon::player_id p);
}
