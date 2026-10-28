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

#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace centipede {
    // `size` < 0 rolls one like Centipede.GenerateSize
    dcon::centipede_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos, float size = -1.f);
    void destroy(sys::game_state& state, dcon::centipede_id c);
    // Centipede.Update for every centipede in the room
    void update_room(sys::game_state& state, world::room_view const& room);
    // Room object collisions between centipedes and slugcats (pushing apart,
    // Centipede.Collide grabbing)
    void collide(sys::game_state& state, world::room_view const& room);
    // Weapon.Update's projectile trace against centipedes: the centipede the
    // segment hits (radius `rad`), and which chunk
    bool trace(sys::game_state const& state, dcon::room_id room, rw::vec2 from, rw::vec2 to, float rad, dcon::centipede_id& hit, uint32_t& chunk);
    // Creature.Violence (+ Centipede.Violence)
    void violence(sys::game_state& state, dcon::centipede_id c, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus);

    dcon::physical_object_id body(sys::game_state const& state, dcon::centipede_id c) noexcept;
    uint32_t chunk_count(sys::game_state const& state, dcon::centipede_id c) noexcept;
    dcon::body_chunk_id chunk(sys::game_state const& state, dcon::centipede_id c, uint32_t i) noexcept;
    uint32_t head_index(sys::game_state const& state, dcon::centipede_id c) noexcept;
    float total_mass(sys::game_state const& state, dcon::centipede_id c) noexcept;
    // releases whatever the centipede holds, and any centipede holding `p`
    void release_player(sys::game_state& state, dcon::player_id p);
}
