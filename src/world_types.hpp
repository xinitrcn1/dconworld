#pragma once

// Plain types stored in the data container (see dcon_generated.txt)

#include <cstdint>

namespace world {
    // World.preProcessingGeneration: connectivity maps older than this are stale
    inline constexpr int32_t preprocessing_generation = 20;

    // Room.Tile.TerrainType
    enum class terrain : uint8_t {
        air,
        solid,
        slope,
        floor,
        shortcut_entrance,
    };

    // Room.Tile boolean attributes
    enum tile_flag : uint8_t {
        tile_vertical_beam = 1 << 0,
        tile_horizontal_beam = 1 << 1,
        tile_wall_behind = 1 << 2,
        tile_hive = 1 << 3,
        tile_waterfall = 1 << 4,
        tile_garbage_hole = 1 << 5,
        tile_worm_grass = 1 << 6,
    };

    // Room.SlopeDirection
    enum class slope_direction : uint8_t {
        up_left,
        up_right,
        down_left,
        down_right,
        broken,
    };

    // ShortcutData.Type
    enum class shortcut_type : uint8_t {
        normal,
        room_exit,
        creature_hole,
        npc_transportation,
        region_transportation,
        dead_end,
    };

    enum class item_type : uint8_t {
        rock,
        spear,
    };

    // Weapon.Mode
    enum class weapon_mode : uint8_t {
        free,
        thrown,
        carried,
        stuck_in_wall,
    };

    // RegionGate.Mode
    enum class gate_mode : uint8_t {
        middle_closed,
        closing_air_lock,
        waiting,
        opening_middle,
        middle_open,
        closing_middle,
        opening_side,
        closed,
        broken,
    };

    // RoomSettings.DangerType
    enum class danger_type : uint8_t {
        rain,
        flood,
        flood_and_rain,
        none,
        unknown,
    };

    // One entry of AbstractRoomNode.connectivity[template, node]
    struct node_connectivity {
        int16_t distance = -1;
        int16_t second = -1;
    };

    // By-value copy of a Room.Tile, assembled from the tile properties
    struct tile {
        terrain t = terrain::air;
        uint8_t flags = 0;
        uint8_t shortcut = 0; // Room.Tile.shortCut
        uint8_t water = 0;    // Room.Tile.waterInt (0 none, 1 deep, 2 surface)

        constexpr bool solid() const noexcept { return t == terrain::solid; }
        constexpr bool vertical_beam() const noexcept { return (flags & tile_vertical_beam) != 0; }
        constexpr bool horizontal_beam() const noexcept { return (flags & tile_horizontal_beam) != 0; }
        constexpr bool any_beam() const noexcept { return (flags & (tile_vertical_beam | tile_horizontal_beam)) != 0; }
    };
}
