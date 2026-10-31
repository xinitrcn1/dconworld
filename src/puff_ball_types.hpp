#pragma once

// PuffBall's own state (puff_ball.hpp): the puff_ball_data object of every
// puff ball item

#include <cstdint>

#include "math.hpp"

namespace puff_ball {
    // PuffBall: (int)Lerp(3, 15, Random.value) segments, Random.Range(6, 11) dots
    inline constexpr uint32_t max_segments = 15;
    inline constexpr uint32_t max_dots = 10;

    struct state {
        // segments[i, 0..2]: position, last position, velocity
        rw::vec2 seg_pos[max_segments] = {};
        rw::vec2 seg_last[max_segments] = {};
        rw::vec2 seg_vel[max_segments] = {};
        rw::vec2 dots[max_dots] = {};
        uint8_t segment_count = 3;
        uint8_t dot_count = 6;
        uint8_t last_mode_thrown = 0;
        float being_eaten = 0.f;
        float swallowed = 0.f;
    };
}
