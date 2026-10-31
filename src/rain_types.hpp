#pragma once

// The rain cycle's state (RainCycle, GlobalRain, GlobalRain.DeathRain), one
// per region (rain.hpp), stored in the data container as the region's `rain`

#include <cstdint>

#include "math.hpp"

namespace rain {
    // GlobalRain.DeathRain.DeathRainMode, in ExtEnum order (AlternateBuildUp
    // comes after Mayhem, Downpour's Pulses after it)
    enum class death_mode : uint8_t {
        none,
        calm_before_storm,
        grade_a_build_up,
        grade_a_plateu,
        grade_b_build_up,
        grade_b_plateu,
        final_build_up,
        mayhem,
        alternate_build_up,
        pulses,
    };

    struct cycle {
        // RainCycle
        int32_t timer = 0;
        int32_t cycle_length = 0;
        int32_t base_cycle_length = 0;
        int32_t sun_down_start_time = 0;
        int32_t day_night_counter = 0;
        uint8_t death_rain_has_hit = 0;
        // GlobalRain
        float intensity = 0.f;
        float shader_light = -1.f;
        float screen_shake = 0.f;
        float micro_screen_shake = 0.f;
        float rumble_sound = 0.f;
        float bullet_rain_density = 0.f;
        float rain_direction = 0.f;
        float last_rain_direction = 0.f;
        float rain_direction_get_to = 0.f;
        float flood = 0.f;
        float flood_speed = 0.f;
        // GlobalRain.DeathRain (mode none: no death rain)
        death_mode mode = death_mode::none;
        float time_in_this_mode = 0.f;
        float progression = 0.f;
        float calm_before_storm_sunlight = 0.f;
        // the short rain (rain.hpp): ticks since it hit, how long it lasts
        // (rolled when it hits), and how much faster than vanilla's its
        // phases run to fit in the window (1: vanilla's pace)
        int32_t death_rain_ticks = 0;
        int32_t death_rain_length = 0;
        float time_scale = 1.f;
        // every phase's length in the window (ticks, by death_mode), rolled
        // when the rain hits
        float phase_ticks[10] = {};
        // after it: the flood draining away, ticks left
        int32_t drain_ticks = 0;
        // cycles finished (the rain came and went)
        uint32_t cycles = 0;
    };
}
