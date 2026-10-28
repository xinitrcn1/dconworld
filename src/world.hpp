#pragma once

// Static world data: regions, rooms and their geometry, loaded once from the
// layered filesystem into the data container.

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "config.h"
#include "gamestate.hpp"
#include "math.hpp"
#include "parsers.hpp"
#include "world_types.hpp"

namespace world {
    // By-value copy of a ShortcutData
    struct shortcut_data {
        rw::ivec2 start{0, 0};
        rw::ivec2 dest{0, 0};
        uint16_t length = 0;
        shortcut_type type = shortcut_type::dead_end;
        dcon::room_exit_id dest_exit;
    };

    // Lightweight view of one realised room, mirrors the tile helpers of
    // Room.cs. The tile pointers address the room's contiguous tile block in
    // the data container (or temporary buffers while a room is being parsed).
    struct room_view {
        sys::game_state const* s = nullptr;
        dcon::room_id id;
        int32_t width = 0;
        int32_t height = 0;
        terrain const* terrains = nullptr;
        uint8_t const* flags = nullptr;
        uint8_t const* shortcuts = nullptr;
        uint8_t const* waters = nullptr;
        bool water = false;
        int32_t default_water_level = -1;
        float float_water_level = std::numeric_limits<float>::lowest();
        float gravity = 1.f;

        float pixel_width() const noexcept { return float(width) * RW_TILE_SIZE; }
        float pixel_height() const noexcept { return float(height) * RW_TILE_SIZE; }

        static rw::ivec2 get_tile_position(rw::vec2 pos) noexcept {
            return rw::ivec2{int32_t((pos.x + 20.f) / 20.f) - 1, int32_t((pos.y + 20.f) / 20.f) - 1};
        }
        static rw::vec2 middle_of_tile(int32_t x, int32_t y) noexcept {
            return rw::vec2{10.f + float(x) * 20.f, 10.f + float(y) * 20.f};
        }
        static rw::vec2 middle_of_tile(rw::ivec2 p) noexcept { return middle_of_tile(p.x, p.y); }
        static rw::vec2 middle_of_tile(rw::vec2 p) noexcept { return middle_of_tile(get_tile_position(p)); }

        bool inside(int32_t x, int32_t y) const noexcept {
            return x >= 0 && y >= 0 && x < width && y < height;
        }

        // Room.GetTile: out of bounds clamps to the border, with shortcut
        // entrances turned solid and shortcut paths removed
        tile get_tile(int32_t x, int32_t y) const noexcept {
            if (inside(x, y)) {
                size_t const i = size_t(y) * size_t(width) + size_t(x);
                return tile{terrains[i], flags[i], shortcuts[i], waters[i]};
            }
            int32_t const cx = rw::custom::int_clamp(x, 0, width - 1);
            int32_t const cy = rw::custom::int_clamp(y, 0, height - 1);
            size_t const i = size_t(cy) * size_t(width) + size_t(cx);
            tile t{terrains[i], flags[i], shortcuts[i], waters[i]};
            if (t.t == terrain::shortcut_entrance)
                t.t = terrain::solid;
            t.shortcut = 0;
            return t;
        }
        tile get_tile(rw::ivec2 p) const noexcept { return get_tile(p.x, p.y); }
        tile get_tile(rw::vec2 p) const noexcept { return get_tile(get_tile_position(p)); }
        terrain get_terrain(int32_t x, int32_t y) const noexcept { return get_tile(x, y).t; }

        float get_float_water_level(rw::vec2) const noexcept { return float_water_level; }
        bool point_submerged(rw::vec2 pos) const noexcept { return pos.y < float_water_level; }

        slope_direction identify_slope(int32_t x, int32_t y) const noexcept;
        slope_direction identify_slope(rw::ivec2 p) const noexcept { return identify_slope(p.x, p.y); }
        slope_direction identify_slope(rw::vec2 p) const noexcept { return identify_slope(get_tile_position(p)); }
        bool visual_contact(rw::vec2 a, rw::vec2 b) const noexcept;
        rw::ivec2 shortcut_entrance_hole_direction(rw::ivec2 pos) const noexcept;
        shortcut_data get_shortcut_data(rw::ivec2 pos) const noexcept;
    };

    room_view get_room_view(sys::game_state const& s, dcon::room_id rid) noexcept;

    // Parses level file contents (the "<room>.txt" file) and adds the room to
    // the data container. Returns an invalid id (errors reported) on failure.
    dcon::room_id add_room(sys::game_state& s, std::string_view name, dcon::region_id region, std::string_view contents, parsers::error_handler& err);

    // Loads every region and room reachable from world/regions.txt; room files
    // are parsed in parallel (TBB) and committed to the container in order
    void load(sys::game_state& s, parsers::error_handler& err);
}
