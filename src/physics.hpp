#pragma once

// Port of Rain World's object physics: BodyChunk (point masses with tile
// collision) and PhysicalObject.BodyChunkConnection (distance constraints).
// All state lives in the data container: physical_object, body_chunk (owned
// through chunk_ownership) and the chunk_connection relationship.

#include <cstdint>

#include "gamestate.hpp"
#include "math.hpp"
#include "physics_types.hpp"
#include "world.hpp"

namespace physics {
    // Called from inside the chunk update, exactly where vanilla calls
    // PhysicalObject.TerrainImpact (the callee may modify any chunk state)
    using terrain_impact_fn = void (*)(void* user, dcon::physical_object_id object, uint32_t local_chunk, rw::ivec2 direction, float speed, bool first_contact);

    struct object_desc {
        float gravity = 0.9f;
        float air_friction = 0.999f;
        float water_friction = 0.96f;
        float buoyancy = 0.95f;
        float surface_friction = 0.5f;
        float bounce = 0.1f;
        float impact_threshold = 1.f;
    };

    dcon::physical_object_id create_object(sys::game_state& state, dcon::room_id room, object_desc const& desc);
    dcon::body_chunk_id add_chunk(sys::game_state& state, dcon::physical_object_id object, rw::vec2 pos, float rad, float mass);
    dcon::chunk_connection_id add_connection(sys::game_state& state, dcon::physical_object_id object, dcon::body_chunk_id chunk1,
        dcon::body_chunk_id chunk2, float distance, connection_type type, float elasticity, float weight_symmetry);
    // removes the object with its chunks and connections
    void delete_object(sys::game_state& state, dcon::physical_object_id object);
    // BodyChunk with the given index within the object (bodyChunks[i])
    dcon::body_chunk_id chunk(sys::game_state const& state, dcon::physical_object_id object, uint32_t local) noexcept;

    inline float gravity(sys::game_state const& state, dcon::physical_object_id o, world::room_view const& room) noexcept {
        return state.world.physical_object_get_g(o) * room.gravity;
    }
    float terrain_rad(sys::game_state const& state, dcon::body_chunk_id c) noexcept;
    float submersion(sys::game_state const& state, world::room_view const& room, dcon::body_chunk_id c) noexcept;
    // Average submersion of all chunks (PhysicalObject.Submersion)
    float object_submersion(sys::game_state const& state, world::room_view const& room, dcon::physical_object_id object) noexcept;
    // PhysicalObject.IsTileSolid
    bool is_tile_solid(sys::game_state const& state, world::room_view const& room, dcon::body_chunk_id c, int32_t rx, int32_t ry) noexcept;
    void set_go_through_floors(sys::game_state& state, dcon::physical_object_id object, bool v) noexcept;
    void hard_set_position(sys::game_state& state, dcon::body_chunk_id c, rw::vec2 p) noexcept;

    // BodyChunk.Update
    void update_chunk(sys::game_state& state, world::room_view const& room, dcon::body_chunk_id c, terrain_impact_fn on_impact, void* user);
    // BodyChunkConnection.Update
    void update_connection(sys::game_state& state, dcon::chunk_connection_id k);
    // The physics half of PhysicalObject.Update: all chunks, then connections
    void update_object(sys::game_state& state, world::room_view const& room, dcon::physical_object_id object, terrain_impact_fn on_impact, void* user);
}
