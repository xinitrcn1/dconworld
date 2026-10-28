#pragma once

// The whole game. Every object (regions, rooms, tiles, cameras, shortcuts,
// physical objects, body chunks, players...) lives in `world`, the data
// container. Whatever doesn't fit the SoA model (global constants, strings,
// lookup maps, the filesystem) lives next to it in the game_state.
//
// The data container stores its arrays inline (tens of MB), so a game_state
// must be heap allocated (see sys::make_game_state).

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "dcon_generated.hpp"
#include "layered_fs.hpp"

namespace sys {
    struct game_state {
        dcon::data_container world;
        layered_fs::filesystem fs;

        // global constants
        // Room.gravity for rooms without a gravity effect
        float default_room_gravity = 1.f;
        // the simulation always runs at a fixed rate
        uint32_t ticks_per_second = RW_TICKS_PER_SECOND;
        // Rain World's screen, the size vanilla room cameras are laid out for
        float screen_width = 1366.f;
        float screen_height = 768.f;
        // UnityEngine.Random for world level rolls (item spawns...)
        rw::rng random{0x2545F491u};

        // text storage, indexed by dcon::text_key
        std::vector<std::string> text;
        std::unordered_map<std::string, dcon::room_id> room_by_name;
        std::unordered_map<std::string, dcon::region_id> region_by_name;
        std::unordered_map<uint32_t, dcon::palette_id> palette_by_number;

        dcon::text_key add_text(std::string_view sv) {
            text.emplace_back(sv);
            return dcon::text_key(dcon::text_key::value_base_t(text.size() - 1));
        }
        std::string_view to_string_view(dcon::text_key k) const {
            return k ? std::string_view{text[size_t(k.index())]} : std::string_view{};
        }
        dcon::room_id find_room(std::string_view name) const;
        // the palette object for "palette<number>.png", created on first use
        dcon::palette_id palette_for(uint32_t number) {
            if (auto const it = palette_by_number.find(number); it != palette_by_number.end())
                return it->second;
            auto const pid = world.create_palette();
            world.palette_set_number(pid, uint16_t(number));
            palette_by_number.emplace(number, pid);
            return pid;
        }
    };

    std::unique_ptr<game_state> make_game_state();
}
