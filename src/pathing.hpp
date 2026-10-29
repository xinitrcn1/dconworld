#pragma once

// On demand pathfinding. Vanilla bakes per creature-template connectivity
// maps and runs an incremental A* per creature (PathFinder, AImap). Here a
// creature asks for a distance field to its goal tile instead: one reverse
// Dijkstra over the room's tiles, kept in sys::game_state::path_fields (a
// cache, not game state: it can always be rebuilt). Fields are keyed by (room, goal tile, movement kind) and shared by
// every creature heading to the same tile (a pack hunting the slugcat builds
// one field per tile the slugcat stands on), recycled least recently used,
// and rebuilt when the room's geometry_version changes (gate doors, spear
// beams). Following a field is a lookup of the cheapest neighbour.

#include <cstdint>
#include <optional>
#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "pathing_types.hpp"
#include "world.hpp"

namespace pathing {
    inline constexpr uint16_t unreachable = 0xFFFF;
    // a field: its index in sys::game_state::path_fields
    using field_id = uint32_t;
    // TileConnectionResistance, in tenths: standard step, open diagonal, drop
    inline constexpr uint16_t step_cost = 10;
    inline constexpr uint16_t diagonal_cost = 30;
    inline constexpr uint16_t drop_cost = 50;

    access tile_access(world::room_view const& room, rw::ivec2 t) noexcept;
    // Whether the tile is one the movement kind can stand on
    bool accessible(world::room_view const& room, rw::ivec2 t, movement const& m) noexcept;
    // AImap.getTerrainProximity, capped: tiles to the nearest solid tile
    int32_t terrain_proximity(world::room_view const& room, rw::ivec2 t, int32_t cap = 4) noexcept;
    // AItile.narrowSpace: solid on both sides along an axis or a diagonal
    bool narrow_space(world::room_view const& room, rw::ivec2 t) noexcept;
    // AItile.floorAltitude: tiles up from the closest floor below (a Floor
    // tile, or a solid one with open space over it)
    int32_t floor_altitude(world::room_view const& room, rw::ivec2 t) noexcept;
    // AImap.getTerrainProximity as AImapper's TerrainProximityMapper fills
    // it: four direction steps to the nearest solid tile (0 solid, 1 beside
    // one), uncapped, -1 outside the room. Kept per room in
    // sys::game_state::terrain_proximity_cache
    int32_t ai_terrain_proximity(sys::game_state& state, dcon::room_id room, rw::ivec2 t);
    // the whole map (row major), for callers asking about many tiles at a
    // time; valid until another room's map is built
    std::vector<uint16_t> const& ai_terrain_proximity_map(sys::game_state& state, dcon::room_id room);

    // The field to `goal`, built if no fresh one exists. An inaccessible goal
    // (a slugcat in mid air) is approached through the accessible tiles near it
    field_id field_to(sys::game_state& state, dcon::room_id room, rw::ivec2 goal, movement const& m);
    // travel cost from `t` to the field's goal (unreachable out of the room)
    uint16_t distance(sys::game_state const& state, field_id f, rw::ivec2 t) noexcept;
    // The neighbour of `from` to step to, none when there is no way on
    std::optional<rw::ivec2> next_step(sys::game_state const& state, field_id f, rw::ivec2 from) noexcept;

    // fields built since start (statistics, tests)
    uint32_t builds() noexcept;
}
