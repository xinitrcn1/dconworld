#pragma once

// creature::target, apart from creature_types.hpp so the kinds' own state
// (lizard_types.hpp, ...) can keep one

#include <cstdint>

namespace creature {
    // A creature or a slugcat: what a tracker represents, a grasp holds, an
    // attack hits (AbstractCreature / Creature in vanilla). 0 is none, a
    // creature is its index + 1, a slugcat its index with the high bit set
    struct target {
        uint16_t v = 0;
        static constexpr uint16_t player_flag = 0x8000;
        constexpr explicit operator bool() const noexcept { return v != 0; }
        constexpr bool operator==(target const&) const noexcept = default;
        constexpr bool is_player() const noexcept { return (v & player_flag) != 0; }
        constexpr bool is_creature() const noexcept { return v != 0 && (v & player_flag) == 0; }
    };
}
