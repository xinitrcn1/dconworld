#pragma once

// Leech state that isn't a plain property of the leech: its school's prey
// list and the LeechGraphics state (purely cosmetic), stored with the leech

#include <cstdint>

#include "math.hpp"
#include "player_types.hpp"
#include "dcon_generated_ids.hpp"

namespace leech {
    // CreatureTemplate.Type of the templates running Leech.cs: Leech, SeaLeech
    // and Downpour's JungleLeech
    enum class variant : uint8_t {
        leech,
        sea,
        jungle,
    };
    inline constexpr uint32_t variant_count = 3;

    // LeechSchool.LeechPrey: a creature or slugcat the school hunts
    struct school_prey {
        dcon::creature_id who;
        // Creature.leechedOut, as far as this school is concerned
        uint8_t leeched_out = 0;
        int32_t forget_counter = 0;
        float bites_after_death = 1.f;
    };
    // the prey a school can be after at once
    inline constexpr uint32_t max_prey = 4;
    struct school_prey_list {
        school_prey items[max_prey];
    };

    // LeechGraphics: the body (11 GenericBodyParts for a sea leech, 5
    // otherwise) and their radii
    inline constexpr uint32_t max_body = 11;
    struct graphics_state {
        player::body_part body[max_body];
        float radiuses[max_body];
        float sin_counter = 0.f;
        int32_t vibrate = 0;
    };
}
