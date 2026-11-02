#pragma once

// Drill crab state that isn't a plain property of the creature (Watcher's
// DrillCrab, DrillCrabAI, DrillCrabState, DrillCrabGraphics): what its data
// object (`drill_crab_data`, dcon_generated.txt) and its legs' part payload
// (`drill_crab_leg_payload`, creature_types.hpp) keep

#include <cstdint>

#include "dcon_generated_ids.hpp"
#include "math.hpp"
#include "pathing_types.hpp"

namespace drill_crab {
    // the template: DrillCrab, or TowerCrab (its descendant: longer legs,
    // a bigger body, the same class with `Tower` set)
    enum class variant : uint8_t { drill, tower };
    inline constexpr uint32_t variant_count = 2;

    // DrillCrabAI.Behavior
    enum class behavior : uint8_t { idle, hide, flee, escape_rain };

    // DrillCrab.Leg.Mode
    enum class leg_mode : uint8_t { retracting, seeking, scanning };

    inline constexpr uint32_t leg_count = 6;
    // SmallLeg.segmentLengths (5), TowerLeg.segmentLengths (6)
    inline constexpr uint32_t small_leg_segments = 5;
    inline constexpr uint32_t tower_leg_segments = 6;
    inline constexpr uint32_t max_leg_segments = 6;

    // CreatureTemplate (StaticWorld.InitDrillCrab): Air at 1, so every open
    // tile. AImap.IsTooCloseToTerrain keeps vanilla's drill crab at least 2
    // tiles (the tower crab 3) from the terrain and at most 8 (18) above a
    // floor; the fields can't express that, so the paths go through any open
    // tile, pushed a little away from the walls (open_air, like the rain
    // deer's), and the crab's own steps (drill_crab.cpp accessible) keep the
    // vanilla rule
    inline constexpr pathing::movement movement{{10, 10, 10, 10, 10, 10, 0}, 0, 3};

    // DrillCrab.Leg.Segment (prevDist comes from the leg's index and lengthFac)
    struct segment {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
    };

    // DrillCrab.Eye (side: 1 for eyes[0], -1 for eyes[1])
    struct eye {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
    };

    // AbstractCreature.personality (rolled from the ID's seed)
    struct personality {
        float sympathy = 0.5f;
        float energy = 0.5f;
        float bravery = 0.5f;
        float nervous = 0.5f;
        float aggression = 0.5f;
        float dominance = 0.5f;
    };

    // DrillCrab.IndividualVariations
    struct individual_variations {
        int32_t extra_teeth = 0;
        float leg_thickness = 0.5f;
        float leg_uniformity = 0.5f;
        float leg_length = 0.5f;
        float body_size = 0.5f;
        float drill_size = 0.5f;
        float eyestalk_length = 0.5f;
        float eyestalk_asymmetry = 0.5f;
        float eye_lightness = 0.5f;
    };

    // StuckTracker(trackPastPositions, trackNotFollowingCurrentGoal)'s past
    // positions (totalTrackedLastPositions)
    inline constexpr uint32_t stuck_positions = 80;

    // DrillCrabAI: its CreatureLooker, fleeFromCreature, timers; the
    // UtilityComparer's smoothed threat; the StuckTracker
    struct ai_state {
        dcon::creature_id look_creature;
        int32_t look_focus_delay = 0;
        dcon::creature_id flee_from;
        int32_t idle_timer = 0;
        int32_t kneel_timer = 0;
        // the ThreatTracker's (and the DiscomfortTracker's, folded into it)
        // smoothed utility (FloatTweenUpAndDown: lerp 0.05 up, tick 0.025 down)
        float threat_smoothed = 0.f;
        // StuckTracker
        rw::ivec2 past[stuck_positions];
        uint8_t past_head = 0;
        uint8_t past_count = 0;
        uint8_t satisfied = 0;
        int32_t stuck_counter = 0;
    };

    // DrillCrab's leg segments, eyes and path following (moveTarget,
    // nextStepPos), idealHeight
    struct body_state {
        segment segments[leg_count][max_leg_segments];
        eye eyes[2];
        rw::vec2 move_target{0.f, 0.f};
        rw::vec2 next_step_pos{0.f, 0.f};
        float ideal_height = 150.f;
    };

    // DrillCrabGraphics (purely cosmetic): drillRotation, lastDrillRotation
    struct graphics_state {
        float drill_rotation = 0.f;
        float last_drill_rotation = 0.f;
    };
}
