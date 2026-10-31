#pragma once

// DropBug state that isn't a plain property of the dropbug object: its
// AI's behaviours and modules, and the DropBugGraphics state (purely
// cosmetic), stored with the dropbug

#include <cstdint>

#include "math.hpp"
#include "player_types.hpp"

namespace dropbug {
    // DropBugAI.Behavior
    enum class behavior : uint8_t {
        idle,
        flee,
        hunt,
        escape_rain,
        return_prey,
        get_unstuck,
        sit_in_ceiling,
        leave_room,
        injured,
    };

    // MovementConnection, as far as DropBug.Run looks at one: a step of a
    // path field from a tile to the next (`valid` false is default(MovementConnection))
    struct movement_connection {
        rw::ivec2 start{0, 0};
        rw::ivec2 dest{0, 0};
        bool valid = false;
    };

    // DropBugAI.CeilingSitModule
    struct ceiling_module {
        rw::ivec2 ceiling_pos{0, 0};
        rw::ivec2 stay_away_from_pos{0, 0};
        rw::ivec2 random_attractor{0, 0};
        int32_t win_streak = 0;
        int32_t spear_danger = 0;
        int32_t room_size = 0;
        float drop_delay = 0.f;
        uint8_t anywhere_to_sit = 0;
        int32_t anywhere_to_sit_counter = 0;
        uint8_t anticipate_drop = 0;
        uint8_t last_anticipate_drop = 0;
    };

    // StuckTracker with trackPastPositions: the last 80 tiles (newest first)
    struct stuck_tracker {
        static constexpr int32_t tracked = 80;
        rw::ivec2 past[tracked];
        int32_t count = 0;
        int32_t head = 0; // index of the newest
        int32_t counter = 0;
    };

    // UtilityComparer's smoothed utilities (the trackers with a smoother)
    struct utilities {
        float prey = 0.f;
        float prey_weight = 0.07f;
        float injury = 0.f;
        float ceiling = 0.f;
        float ceiling_weight = 0.7f;
    };

    // DropBugGraphics
    struct graphics_state {
        // drawPositions[i, 0] (now) and [i, 1] (last tick)
        rw::vec2 draw_positions[3][2];
        float flip = 0.f;
        float last_flip = 0.f;
        // legs[side, leg] (Limb) and their knees (GenericBodyPart)
        player::hand_state legs[2][2];
        player::body_part knees[2][2];
        rw::vec2 legs_travel_dirs[2][2];
        int32_t legs_dangle_counter = 0;
        player::body_part mandibles[2];
        float mandible_movements[2][2];
        player::body_part pinchers[2];
        player::body_part antennae[2];
        player::body_part tail_end;
        float leg_length = 45.f;
        rw::vec2 breath_dir{0.f, 0.f};
        float legs_thickness = 1.f;
        float body_thickness = 1.f;
        float antennae_length = 1.f;
        float hue = 0.6722222f;
        float colored_antennae = 0.f;
        float pinchers_flip = 0.f;
        float last_pinchers_flip = 0.f;
        float pinchers_length = 50.f;
        float vibrate = 0.f;
        float last_vibrate = 0.f;
        float ceiling_mode = 0.f;
        float last_ceiling_mode = 0.f;
        float deep_ceiling_mode = 0.f;
        float last_deep_ceiling_mode = 0.f;
        uint8_t ceiling_jump = 0;
        // (its UnityEngine.Random: rw::random keyed by the creature)
    };
}
