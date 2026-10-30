#pragma once

// Cicada state that isn't a plain property of the cicada object: its
// IndividualVariations, its AI's behaviours and modules, and the
// CicadaGraphics state (purely cosmetic), stored with the cicada

#include <cstdint>

#include "math.hpp"
#include "target_types.hpp"
#include "player_types.hpp"

namespace cicada {
    // CicadaAI.Behavior
    enum class behavior : uint8_t {
        idle,
        flee,
        hunt,
        escape_rain,
        antagonize,
        return_prey,
        get_unstuck,
        follow_friend,
    };

    // Cicada.IndividualVariations (color is HSLColor(hue, 1, 0.5): the hue)
    struct individual_variations {
        float fatness = 1.f;
        float wing_sound_pitch = 1.f;
        float default_wing_deployment = 0.f;
        float tentacle_length = 1.f;
        float tentacle_thickness = 1.f;
        float wing_thickness = 1.f;
        float wing_length = 1.f;
        int32_t busted_wing = -1;
        float hue = 0.55f;
    };

    // CicadaAI.CircleGroup, kept by the cicada at the head of the group
    // (group[0]); the others point at it through circle_leader
    struct circle_group {
        rw::vec2 center{0.f, 0.f};
        float radius = 40.f;
        float rotation = 0.f;
        float rotation_dir = 1.f;
        int32_t life_time = 0;
        uint8_t slated_for_deletion = 0;
    };

    // StuckTracker with trackPastPositions: the last 80 tiles (newest first)
    struct stuck_tracker {
        static constexpr int32_t tracked = 80;
        rw::ivec2 past[tracked];
        int32_t count = 0;
        int32_t head = 0; // index of the newest
        int32_t counter = 0;
        // GetUnstuckPosCalculator.unstuckGoalPosition
        rw::ivec2 unstuck_goal{0, 0};
    };

    // CicadaGraphics
    struct graphics_state {
        rw::vec2 z_rotation{0.f, 1.f};
        rw::vec2 last_z_rotation{0.f, 1.f};
        int32_t blink_counter = 0;
        rw::vec2 look_dir{0.f, 0.f};
        float look_rotation = 0.f;
        float wing_offset = 0.f;
        float wing_time_add = 0.f;
        // wings[side, wing] (BodyPart), tentacles[side, tentacle] (Limb)
        player::body_part wings[2][2];
        float wing_deployment[2][2] = {};
        float wing_deployment_speed[2][2] = {};
        float wing_deployment_get_to = 0.f;
        int32_t lazy_wing = 0;
        player::hand_state tentacles[2][2];
        float climb_counter = 0.f;
        float charging_visuals = 0.f;
        // currentLoop: 0 none, 1 Cicada_Wings_LOOP, 2 the bump attack's
        // prepare loop, 3 its charge loop
        uint8_t current_loop = 0;
        // CreatureLooker.lookCreature: a player index + 1, 0 none
        creature::target look_creature;
        // (its UnityEngine.Random is the creature's graphics_random)
    };
}
