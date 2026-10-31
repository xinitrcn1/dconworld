#pragma once

// Runtime half of the parser generator: the tokenizer, primitive value parsers
// and the plain structs filled by the generated parse_<object>() functions
// (see parser_defs.txt, which is compiled into parser_defs_generated.hpp).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "world_types.hpp"

namespace sys {
    struct game_state;
}

namespace parsers {
    struct token_generator;
    struct token {
        uint32_t pos = 0;
        uint32_t len = 0;
        uint32_t line = 0;
        enum class type : uint8_t {
            identifier,
            semicolon,
            comma,
            lparen,
            rparen,
            lbrace,
            rbrace,
            newline,
            unknown
        } type = type::unknown;
        std::string_view to_string_view(token_generator const& gen) const noexcept;
    };
    struct token_generator {
        using const_iterator = std::vector<token>::const_iterator;
        std::string_view view;
        std::vector<token> tokens;
        const_iterator cur_tok;
        explicit token_generator(std::string_view sv);
    };

    struct error_handler {
        std::string filename;
        uint32_t line = 0;
        std::string accumulated_errors;
        std::string accumulated_warnings;
        uint32_t error_count = 0;
        uint32_t warning_count = 0;
        bool fatal = false;

        void add_error(std::string_view sv);
        void add_warning(std::string_view sv);
        void add_standalone_error(std::string_view sv);
        void add_standalone_warning(std::string_view sv);
    };

    uint32_t parse_uint(std::string_view contents, error_handler& err) noexcept;
    int32_t parse_int(std::string_view contents, error_handler& err) noexcept;
    float parse_float(std::string_view contents, error_handler& err) noexcept;
    bool parse_bool(std::string_view contents, error_handler& err) noexcept;
    std::string parse_string(std::string_view contents, error_handler& err) noexcept;

    struct color_3f {
        float r = 0.f, g = 0.f, b = 0.f;
    };
    struct key_value_list {
        std::string key;
        std::vector<std::string> values;
    };
    using string_list = std::vector<std::string>;
    key_value_list parse_key_value_list(token_generator& gen, error_handler& err, token_generator::const_iterator& it) noexcept;
    string_list parse_string_list(token_generator& gen, error_handler& err, token_generator::const_iterator& it) noexcept;
    color_3f parse_color(token_generator& gen, error_handler& err, token_generator::const_iterator& it) noexcept;

    struct region_context {
        sys::game_state const& state;
        uint32_t region;
    };
    struct room_context {
        sys::game_state const& state;
    };

    struct region_properties {
        string_list room_setting_templates;
        std::vector<std::string> subregions;
        color_3f corruption_effect_color{0.f, 0.f, 1.f};
        color_3f corruption_eye_color{0.f, 0.f, 1.f};
        uint32_t palette = 0;
        uint32_t bat_deplete_cycles_min = 2;
        uint32_t bat_deplete_cycles_max = 7;
        uint32_t bat_deplete_cycles_max_if_less_than_five_left = 4;
        uint32_t bat_deplete_cycles_max_if_less_than_two_left = 3;
        uint32_t bats_per_active_swarm_room = 10;
        uint32_t overseers_min = 4;
        uint32_t overseers_max = 4;
        uint32_t ghost_rel_karma_req = 0;
        float overseers_spawn_chance = 0.8f;
        float player_guide_overseer_spawn_chance = 1.f;
        bool albinos = false;

        void subregion(string_list&& sl, error_handler& err, region_context& ctx) noexcept;
        void finish() noexcept {}
    };

    // All optional: templates are layered (region default/template, then room)
    struct room_template {
        std::string template_;
        std::optional<std::string> danger_type;
        std::optional<uint32_t> palette;
        std::optional<uint32_t> effect_color_a;
        std::optional<uint32_t> effect_color_b;
        std::optional<uint32_t> fade_palette_index;
        std::optional<float> fade_palette_amount;
        std::optional<float> grime;
        std::optional<float> clouds;
        std::optional<float> ceiling_drips;
        std::optional<float> wave_speed;
        std::optional<float> wave_length;
        std::optional<float> wave_amplitude;
        std::optional<float> second_wave_length;
        std::optional<float> second_wave_amplitude;
        std::optional<float> water_reflection_alpha;
        std::optional<float> bkg_drone_volume;
        std::optional<float> bkg_drone_no_threat_volume;
        std::optional<float> random_item_density;
        std::optional<float> random_item_spear_chance;
        std::optional<float> rumble_intensity;
        std::optional<float> rain_intensity;
        std::optional<bool> room_specific_script_active;
        std::optional<bool> wet_terrain;
        // the Watcher's sand (TerrainCurve's _terrainParams, its TerrainPalette)
        std::optional<std::string> terrain_palette;
        std::optional<float> terrain_light;
        std::optional<float> terrain_grain;
        std::optional<float> terrain_edge_radius;
        std::optional<float> terrain_sky_fade;
        // "AmbientSounds:", read by world.cpp (not the generated parser)
        std::vector<world::ambient_sound_desc> ambient_sounds;

        void fade_palette(string_list&& sl, error_handler& err, room_context& ctx) noexcept;
        void finish() noexcept {}
        // Overlay every value set in `o` on top of this template
        void apply(room_template const& o) noexcept;
    };
}
