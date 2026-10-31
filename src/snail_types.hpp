#pragma once

// Snail state that isn't a plain property of the creature (Snail.cs): what
// its data object (`snail_data`, dcon_generated.txt) keeps besides the
// payload: the rolls of its constructor (Snail.miniClicks, shellColor) and
// the SnailGraphics state (purely cosmetic)

#include <cstdint>

#include "dcon_generated_ids.hpp"
#include "math.hpp"
#include "pathing_types.hpp"
#include "player_types.hpp"

namespace snail {
    // CreatureTemplate (Snail): Floor, Corridor, Climb, Wall, Ceiling at 1
    // (no Air: it drops between surfaces), Amphibious with a
    // waterPathingResistance of 1 (anywhere under the surface)
    inline constexpr pathing::movement crawl{{10, 10, 10, 10, 10, 0, 0}, 10};

    // Snail.miniClicks: where along the clickCounter's way to 1 the warning
    // clicks come
    inline constexpr uint32_t mini_click_count = 5;

    // what the constructor rolls from the ID's seed (UnityEngine.Random
    // InitState(ID.RandomSeed)): the warning clicks, the shell's two colours
    struct looks {
        float mini_clicks[mini_click_count] = {};
        rw::vec3 shell_color[2] = {rw::vec3{0.f, 0.f, 0.f}, rw::vec3{0.f, 0.f, 0.f}};
    };

    // SnailGraphics: the two limbs (Limb), the head and the tail
    // (GenericBodyPart), CreatureLooker's lookCreature, movingHand,
    // handChangeCounter, bodyDir, shadowExtensionFac
    struct graphics_state {
        player::hand_state limbs[2];
        player::body_part head;
        player::body_part tail;
        dcon::creature_id look;
        int32_t hand_change_counter = 0;
        rw::vec2 body_dir{0.f, 1.f};
        float shadow_extension_fac = 0.f;
        uint8_t moving_hand = 0;
    };
}
