#pragma once

// The centipede's variants (the templates Centipede.cs, CentipedeAI and
// CentipedeGraphics switch on) and CentipedeGraphics state (purely
// cosmetic), stored with the centipede

#include <cstdint>

#include "math.hpp"
#include "player_types.hpp"

namespace centipede {
    // CreatureTemplate.Type of the templates running Centipede.cs:
    // Centipede, SmallCentipede (Small), Centiwing, RedCentipede (Red) and
    // Downpour's AquaCenti (the aquapede)
    enum class variant : uint8_t {
        centipede,
        small,
        wing,
        red,
        aqua,
    };
    inline constexpr uint32_t variant_count = 5;

    // Centipede bodies: 5 (small) to 18 (red) chunks
    inline constexpr uint32_t max_segments = 18;

    struct graphics_state {
        // legs[segment][side] (Limb)
        player::hand_state legs[max_segments][2];
        float leg_lengths[max_segments];
        // the centiwing's and the aquapede's wings (wingLengths), the
        // aquapede's wing element per side and wing (AquapedeWing1..4) and
        // its second shell sprites' alpha (the AquapedeBody shader's)
        float wing_lengths[max_segments];
        uint8_t aqua_wing[2][max_segments];
        float aqua_body_alpha[max_segments];
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
        float wing_flap_cycle = 0.f;
        float last_wing_flap_cycle = 0.f;
        float wing_swim_cycle = 0.f;
        float last_wing_swim_cycle = 0.f;
        float wings_folded = 1.f;
        float last_wings_folded = 1.f;
        // (the whiskers' twitching rolls the creature's graphics_random)
    };
}
