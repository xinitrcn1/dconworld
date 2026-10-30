#pragma once

// On demand pathfinding. Vanilla bakes per creature-template connectivity
// maps and runs an incremental A* per creature (PathFinder, AImap). Here a
// creature asks for a distance field to its goal tile instead: one reverse
// Dijkstra over the room's tiles, kept in the room's
// sys::room_runtime::path_fields (a cache, not game state: it can always be
// rebuilt). Every room has its own cache, touched only by that room's update,
// so rooms path in parallel. Fields are keyed by (goal tile, movement kind)
// and shared by every creature of the room heading to the same tile (a pack
// hunting the slugcat builds one field per tile the slugcat stands on),
// recycled least recently used, dropped after a while unused, and rebuilt
// when the room's geometry_version changes (gate doors, spear beams).
// Following a field is a lookup of the cheapest neighbour.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "pathing_types.hpp"
#include "world.hpp"

namespace pathing {
    inline constexpr uint16_t unreachable = 0xFFFF;
    // a field: its room's index (above the low 8 bits) and its slot in the
    // room's cache. Valid for the rest of the room's update
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
    // sys::room_runtime::proximity
    int32_t ai_terrain_proximity(sys::game_state& state, dcon::room_id room, rw::ivec2 t);
    // the whole map (row major), for callers asking about many tiles at a
    // time; valid until the room's geometry changes
    std::vector<uint16_t> const& ai_terrain_proximity_map(sys::game_state& state, dcon::room_id room);

    // The field to `goal`, built if no fresh one exists. An inaccessible goal
    // (a slugcat in mid air) is approached through the accessible tiles near it
    field_id field_to(sys::game_state& state, dcon::room_id room, rw::ivec2 goal, movement const& m);
    // forgets the room's fields nobody asked for in a while (before the
    // room's update: field ids stay valid through it)
    void trim(sys::game_state& state, dcon::room_id room);
    // travel cost from `t` to the field's goal (unreachable out of the room)
    uint16_t distance(sys::game_state const& state, field_id f, rw::ivec2 t) noexcept;
    // The neighbour of `from` to step to, none when there is no way on
    std::optional<rw::ivec2> next_step(sys::game_state const& state, field_id f, rw::ivec2 from) noexcept;

    // The NPC slugcat's walking (movement::slugcat). A tile it can stand
    // on (open, over solid ground, a slope or a platform)
    bool standable(world::room_view const& room, rw::ivec2 t) noexcept;
    // a tile its path may go through (standing, a pole, a corridor, water)
    bool slugcat_walkable(sys::game_state& state, world::room_view const& room, rw::ivec2 t);
    // the next move along a slugcat field from `from`: the tile it leads
    // to, how, and its side (none at the goal or with no way on)
    struct step {
        rw::ivec2 tile;
        step_kind kind = step_kind::walk;
        int8_t dir = 0;
    };
    // (`skip`: moves (from, to) it shouldn't take)
    std::optional<step> slugcat_step(sys::game_state& state, field_id f, rw::ivec2 from, std::function<bool(rw::ivec2, rw::ivec2)> const& skip = nullptr);
    std::string debug_jumps(sys::game_state& state, world::room_view const& room, rw::ivec2 from);
    // the tiles a slugcat at `from` can get to (row major, 1 reachable)
    std::vector<uint8_t> slugcat_reach_from(sys::game_state& state, world::room_view const& room, rw::ivec2 from);

    // fields built since start (statistics, tests)
    uint32_t builds() noexcept;
}
