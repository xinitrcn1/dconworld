#pragma once

// rain deer state that isn't a plain property of the creature (Deer): what
// its payload (creature_types.hpp) and its data object (`deer_data`,
// dcon_generated.txt) keep that isn't a number. Its legs (DeerTentacle) are
// creature_parts with a `deer_leg` data object each.

#include <cstdint>
#include <optional>

#include "dcon_generated_ids.hpp"
#include "math.hpp"
#include "pathing_types.hpp"

namespace deer {
    inline constexpr uint32_t leg_count = 4;
    // DeerTentacle: six TentacleChunks
    inline constexpr uint32_t leg_chunks = 6;
    // Deer.playersInAntlers (co-op)
    inline constexpr uint32_t max_riders = 4;
    // StuckTracker.totalTrackedLastPositions
    inline constexpr uint32_t stuck_positions = 40;
    // DeerGraphics.Antlers: the spine and at most 30 branches
    inline constexpr uint32_t max_antler_parts = 32;
    // DeerGraphics: Random.Range(10, 20) body danglers, Random.Range(5, 10) horn danglers
    inline constexpr uint32_t max_body_danglers = 19;
    inline constexpr uint32_t max_horn_danglers = 9;
    inline constexpr uint32_t max_danglers = max_body_danglers + max_horn_danglers;

    // The Deer template: Air at 1, so every open tile; DeerAI.TravelPreference
    // pulls its path away from the terrain (preferredHeight against the
    // terrain proximity): here open_air, 0.3 tenths a tile closer than 10
    inline constexpr pathing::movement movement{{10, 10, 10, 10, 10, 10, 0}, 0, 3};

    // DeerAI.Behavior
    enum class behavior : uint8_t {
        idle,
        escape_rain,
        track_spores,
        kneeling,
        get_unstuck,
    };

    // Tentacle.TentacleChunk (DeerTentacle's: no Rope)
    struct leg_chunk {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        float rad = 4.f;
        float t_pos = 0.f;
        rw::ivec2 contact_point{0, 0};
        rw::ivec2 last_contact_point{0, 0};
        rw::vec2 phase_from{0.f, 0.f};
        int32_t phases_to_same_location = 0;
        rw::ivec2 after_phase_stuck_pos{0, 0};
        float phase = -1.f;
        int32_t phase_attempts = 0;
        float stretched_fac = 1.f;
        uint8_t collide_with_terrain = 1;
    };

    // Tentacle + DeerTentacle, the scalars and the chunks (the segments and
    // the grab path are the deer_leg object's vector pools); tentacleNumber
    // is the creature_part's index
    struct leg {
        // Tentacle
        float ideal_length = 220.f;
        float update_counter = 0.f;
        int32_t pull_counter = 0;
        int32_t pulls_this_tick = 0;
        std::optional<rw::vec2> float_grab_dest;
        int32_t backtrack_from = -1;
        float retract_fac = 0.f;
        uint8_t limp = 0;
        // its connectedChunk (1 for the front pair, 2 for the hind pair)
        uint8_t connected_chunk = 1;
        // DeerTentacle
        rw::vec2 tentacle_dir{0.f, 1.f};
        float max_length = 400.f;
        rw::vec2 desired_grab_pos{0.f, 0.f};
        uint8_t attached_at_tip = 0;
        int32_t frames_without_reaching = 0;
        int32_t grab_delay = 0;
        int32_t sure_of_footing_counter = 0;
        float smoothed_footing_security = 0.f;
        uint8_t play_step_sound = 0;
        float step_sound_vol = 0.f;
        leg_chunk c[leg_chunks];
    };

    // Deer.PlayerInAntlers.AntlerPoint: a point of a part of the antlers on
    // one side (part < 0: null)
    struct antler_point_ref {
        int16_t part = -1;
        int16_t segment = 0;
        float side = 1.f;
        constexpr bool valid() const noexcept { return part >= 0; }
    };
    // Deer.PlayerInAntlers.GrabStance (has: not null; lower may be null)
    struct grab_stance {
        antler_point_ref upper;
        antler_point_ref lower;
        uint8_t has = 0;
    };
    // Deer.PlayerInAntlers: a slugcat hanging on, by its creature
    struct rider {
        dcon::creature_id player;
        grab_stance stance;
        grab_stance next_stance;
        float mov_prog = 0.f;
        rw::vec2 climb_goal{0.f, 0.f};
        uint8_t dangle = 1;
        uint8_t force_side_change = 0;
        uint8_t disconnected = 0;
        rw::vec2 hand_grab_points[2] = {{0.f, 0.f}, {0.f, 0.f}};
        antler_point_ref odd_hand_grip;
        uint8_t odd_hand = 0;
    };
    struct riders {
        rider r[max_riders];
        uint8_t count = 0;
    };

    // DeerGraphics.Antlers.parts: where each part starts among the
    // flattened antler points (deer_data.antlers) and how many it has
    struct antler_index {
        uint16_t start[max_antler_parts] = {};
        uint8_t length[max_antler_parts] = {};
        uint8_t count = 0;
    };

    // DeerAI with its StuckTracker, DeerAbstractAI.sporePos and damageGoHome,
    // Deer.eatObject
    struct ai_state {
        int32_t time_in_this_room = 0;
        int32_t kneel_counter = 0;
        int32_t close_eyes_counter = 0;
        int32_t lay_down_and_rest_counter = 0;
        int32_t resting_counter = 0;
        int32_t minor_wander_timer = 0;
        int32_t seriously_stuck = 0;
        std::optional<rw::ivec2> rest_pos;
        rw::ivec2 in_room_destination{0, 0};
        rw::ivec2 cant_eat_from{-1000, -1000};
        // goToPuffBall: the puff ball and where it was last seen
        dcon::item_id puff;
        rw::ivec2 puff_seen{0, 0};
        uint8_t puff_visible = 0;
        uint8_t last_player_in_antlers = 0;
        uint8_t damage_go_home = 0;
        // sporePos: a room and a tile (no room: null)
        dcon::room_id spore_room;
        rw::ivec2 spore_tile{0, 0};
        // Deer.eatObject
        dcon::item_id eat_object;
        // StuckTracker: pastPositions (a ring, newest at head), stuckCounter
        rw::ivec2 past[stuck_positions] = {};
        uint8_t past_count = 0;
        uint8_t past_head = 0;
        uint8_t satisfied = 0;
        int32_t stuck_counter = 0;
    };

    // Dangler.DanglerSegment
    struct dangler_segment {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        float rad = 5.f;
        float con_rad = 0.f;
        float stretched_rad = 5.f;
    };

    // the graphics module's state (purely cosmetic; flip and the antlers'
    // random movement are also what a rider climbs by: CurrentFaceDir)
    struct graphics_state {
        float flip = 0.f;
        float last_flip = 0.f;
        float antler_random_movement = 0.f;
        float last_antler_random_movement = 0.f;
        float antler_random_movement_vel = 0.f;
        float chunks_rotat[4] = {};
        uint8_t body_danglers = 0;
        uint8_t horn_danglers = 0;
        // the segments of every dangler (deer_data.danglers, in dangler order)
        uint8_t dangler_segs[max_danglers] = {};
        uint8_t body_dangler_positions[max_body_danglers][2] = {};
        float body_dangler_orientations[max_body_danglers][5] = {};
        int16_t horn_dangler_positions[max_horn_danglers][3] = {};
        rw::vec2 look_point[2] = {{0.f, 0.f}, {0.f, 0.f}};
        int32_t blink = 0;
        float resting = 0.f;
        float last_resting = 0.f;
    };
}
