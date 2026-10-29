#pragma once

// CentipedeGraphics state (purely cosmetic), stored with the centipede

#include <cstdint>

#include "math.hpp"
#include "player_types.hpp"

namespace centipede {
    // Centipede bodies: 7 to 17 chunks (create's size lerp)
    inline constexpr uint32_t max_segments = 17;

    struct graphics_state {
        // legs[segment][side] (Limb)
        player::hand_state legs[max_segments][2];
        float leg_lengths[max_segments];
        // whiskers[end][part][side] (GenericBodyPart): feelers at both ends
        player::body_part whiskers[2][2][2];
        // bodyRotations[i, 0] (now) and [i, 1] (last tick), at the head, the
        // middle and the tail
        rw::vec2 body_rotations[3][2];
        float default_rotat = 0.f;
        float saturation = 0.9f;
        float walk_cycle = 0.f;
        float body_dir = 0.f;
        float last_body_dir = 0.f;
        // the whiskers' twitching
        rw::rng random;
    };
}
