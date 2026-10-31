#pragma once

// Moth grub state that isn't a plain property of the creature (Watcher's
// MothGrub, MothGrubAI, MothGrubGraphics): what its data object
// (`moth_grub_data`, dcon_generated.txt) keeps that isn't a number of the
// payload

#include <cstdint>

#include "dcon_generated_ids.hpp"
#include "math.hpp"

namespace moth_grub {
    // MothGrubAI.Behavior
    enum class behavior : uint8_t {
        idle,
        flee,
        escape_rain,
    };

    // MothGrubAI: its CreatureLooker (lookCreature, lookFocusDelay)
    struct ai_state {
        dcon::creature_id look_creature;
        int32_t look_focus_delay = 0;
    };

    // MothGrubGraphics (purely cosmetic): wrapping[2, 10, 2] (the two sides
    // of the body, this tick and the last), limbs[2, 2, 6, 3] (side, front
    // or back, segment; pos, lastPos, vel), flapness, flapAnim, flapWarmUp
    inline constexpr uint32_t wrapping_points = 10;
    inline constexpr uint32_t limb_segments = 6;
    struct graphics_state {
        rw::vec2 wrapping[2][wrapping_points][2];
        rw::vec2 limbs[2][2][limb_segments][3];
        float flapness = 0.f;
        int32_t flap_anim = 0;
        int32_t flap_warm_up = 0;
    };
}
