#pragma once

// Plain types stored in the data container (see dcon_generated.txt)

#include <cstdint>
#include <optional>

#include "math.hpp"

namespace player {
    // Player.BodyModeIndex
    enum class body_mode_index : uint8_t {
        default_,
        crawl,
        stand,
        corridor_climb,
        climb_into_shortcut,
        wall_climb,
        climbing_on_beam,
        swimming,
        zero_g,
        stunned,
        dead,
    };

    // Player.AnimationIndex
    enum class animation_index : uint8_t {
        none,
        crawl_turn,
        stand_up,
        down_on_fours,
        ledge_crawl,
        ledge_grab,
        hang_from_beam,
        get_up_on_beam,
        stand_on_beam,
        climb_on_beam,
        get_up_to_beam_tip,
        hang_under_vertical_beam,
        beam_tip,
        corridor_turn,
        surface_swim,
        deep_swim,
        roll,
        flip,
        rocket_jump,
        belly_slide,
        antler_climb,
        grappling_swing,
        zero_g_swim,
        zero_g_pole_grab,
        vine_grab,
        dead,
    };

    // Player.InputPackage
    struct input_package {
        int32_t x = 0;
        int32_t y = 0;
        bool jmp = false;
        bool thrw = false;
        bool pckp = false;
        bool mp = false;
        bool spec = false;
        bool gamepad = false;
        rw::vec2 analogue_dir{0.f, 0.f};
        int32_t down_diagonal = 0;

        rw::ivec2 int_vec() const noexcept { return rw::ivec2{x, y}; }
        rw::ivec2 zero_g_gamepad_int_vec() const noexcept;
    };

    // SlugcatStats (base game slugcats)
    struct slugcat_stats {
        float runspeed_fac = 1.f;
        float body_weight_fac = 1.f;
        float pole_climb_speed_fac = 1.f;
        float corridor_climb_speed_fac = 1.f;
        float swim_force_fac = 1.f;
        float swim_boost_force = 3.f;
        float swim_boost_cost = 0.2f;
        int32_t swim_boost_cooldown = 20;
        float swim_boost_min_air = 0.5f;
        float drown_threshold = 1.f / 3.f;
        float lungs_fac = 1.f;
    };
    enum class slugcat_name : uint8_t { white, yellow, red };
    slugcat_stats make_stats(slugcat_name name) noexcept;

    // Player.input: the last 10 ticks of input, [0] is the current one
    struct input_history {
        input_package in[10];
        input_package& operator[](int i) noexcept { return in[i]; }
        input_package const& operator[](int i) const noexcept { return in[i]; }
    };
}
