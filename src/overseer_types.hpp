#pragma once

// Overseer state that isn't a plain property of the creature: what its data
// object (`overseer_data`, dcon_generated.txt) keeps. Overseer.cs's root,
// zip path and zip progress (`body_state`), OverseerAI with the parts of
// OverseerAbstractAI a realized overseer needs (`ai_state`), and the
// OverseerGraphics module with its mycelia and the HologramMatrix lens
// (`graphics_state`, purely cosmetic)

#include <cstdint>

#include "dcon_generated_ids.hpp"
#include "math.hpp"

namespace overseer {
    // Overseer.Mode
    enum class mode : uint8_t {
        watching,
        withdrawing,
        zipping,
        emerging,
        sitting_in_wall,
        conversing,
        projecting,
    };

    // Overseer.zipProgs
    inline constexpr uint32_t zip_prog_count = 5;
    // Overseer.zipPath, capped (FindZipPath walks at most 200 tiles, inside
    // the 60 tile zip pathing window; InterRoomZipPath 50)
    inline constexpr uint32_t max_zip_path = 160;

    struct body_state {
        // Overseer.rootPos, lastRootPos, rootDir, lastRootDir, nextHoverTile
        rw::vec2 root_pos{0.f, 0.f};
        rw::vec2 last_root_pos{0.f, 0.f};
        rw::vec2 root_dir{0.f, 0.f};
        rw::vec2 last_root_dir{0.f, 0.f};
        rw::ivec2 next_hover_tile{0, 0};
        float zip_progs[zip_prog_count] = {};
        // conversationPartner, lastConversationPartner (overseers of its room)
        dcon::creature_id conversation_partner;
        dcon::creature_id last_conversation_partner;
        // zipPath (zipPathCount: `path_count`)
        rw::ivec2 path[max_zip_path];
        uint16_t path_count = 1;
    };

    // OverseerAI.lastLookAtAirPositions and avoidPositions (10 each),
    // OverseerAbstractAI.lastRooms (10)
    inline constexpr uint32_t remembered = 10;

    struct ai_state {
        // OverseerAI.lookAt, lookAtAdd, slowLookAt
        rw::vec2 look_at{0.f, 0.f};
        rw::vec2 look_at_add{0.f, 0.f};
        rw::vec2 slow_look_at{0.f, 0.f};
        rw::vec2 last_look_at_air[remembered];
        uint8_t last_look_at_air_count = 0;
        int32_t look_at_same_air_pos_counter = 0;
        // casualInterestCreature (none: null), casualInterestBonus
        dcon::creature_id casual_interest;
        float casual_interest_bonus = 0.f;
        rw::ivec2 temp_hover_tile{16, 21};
        float target_stationary = 0.f;
        rw::ivec2 avoid[remembered];
        uint8_t avoid_count = 0;
        rw::vec2 last_target_pos{0.f, 0.f};
        float bring_up_lens = 0.f;
        float random_bring_up_lens_bonus = 0.f;
        // lookAtFlyingWeapon: the weapon's position and velocity as the last
        // INotifyOfFlyingWeapons told them, for as long as it keeps flying
        // (`weapon_ticks` > 0)
        rw::vec2 weapon_pos{0.f, 0.f};
        rw::vec2 weapon_vel{0.f, 0.f};
        int32_t weapon_ticks = 0;
        // zipPathingMatrix: its centre (ResetZipPathingMatrix) and how many
        // cells UpdateZipMatrix has filled since (the matrix itself is
        // worked out when a zip path is needed)
        rw::ivec2 matrix_center{0, 0};
        int32_t matrix_expanded = 1;

        // OverseerAbstractAI: targetCreature, lastTargetCreature,
        // targetCreatureCounter
        dcon::creature_id target;
        dcon::creature_id last_target;
        int32_t target_counter = 0;
        // abstractAI.destination's room (none: the room it is in), or the
        // world's offscreen den
        dcon::room_id destination;
        uint8_t destination_offscreen = 0;
        // lastRooms, newest first
        dcon::room_id last_rooms[remembered];
        uint8_t last_room_count = 0;
        // the player guide: goToPlayer, playerGuideCounter
        uint8_t go_to_player = 0;
        int32_t guide_counter = 0;
    };

    // OverseerGraphics: the mycelia (2 to 6 Mycelium, 3 to 5 points each),
    // the body mesh's segments, the HologramMatrix's lines (at most 3 * 6 +
    // 8 * 5) and movements (3 + up to 8)
    inline constexpr uint32_t max_mycelia = 6;
    inline constexpr uint32_t max_mycelium_points = 5;
    inline constexpr uint32_t mesh_segments = 10;
    inline constexpr uint32_t max_holo_lines = 64;
    inline constexpr uint32_t max_holo_movements = 11;

    // CoralBrain.Mycelium: points[i, 0] pos, [i, 1] lastPos, [i, 2] vel
    struct mycelium {
        rw::vec2 pos[max_mycelium_points];
        rw::vec2 last_pos[max_mycelium_points];
        rw::vec2 vel[max_mycelium_points];
        uint8_t count = 0;
        float con_rad = 0.f;
        float length = 0.f;
        int32_t rest = 0;
    };

    // HologramMatrix.Line
    struct holo_line {
        rw::vec3 a{0.f, 0.f, 0.f};
        rw::vec3 b{0.f, 0.f, 0.f};
        rw::vec3 alt_a{0.f, 0.f, 0.f};
        rw::vec3 alt_b{0.f, 0.f, 0.f};
        uint8_t rotation = 0;
        uint8_t movement = 0;
    };

    struct graphics_state {
        // cosmeticLookAt, lastComseticLookAt
        rw::vec2 cosmetic_look_at{0.f, 0.f};
        rw::vec2 last_cosmetic_look_at{0.f, 0.f};
        float convo_mode = 0.f;
        float last_convo_mode = 0.f;
        float holo_lens_up = 0.f;
        float last_holo_lens_up = 0.f;
        mycelium mycelia[max_mycelia];
        uint8_t mycelia_count = 0;
        float mycelia_con_rad = 0.f;
        float mycelia_stuck_at = 0.5f;
        float color_mycelia_from = 0.5f;
        float mycelia_movements[2][5] = {};
        // totalLength (the body mesh's length the last time it was laid out)
        float total_length = 1.f;
        // HologramMatrix: lines, rotations[2, 6], movements[3 + n, 6]
        holo_line lines[max_holo_lines];
        uint8_t line_count = 0;
        float rotations[2][6] = {};
        float movements[max_holo_movements][6] = {};
        uint8_t movement_count = 0;
    };
}
