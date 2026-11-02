#pragma once

// The iterators' state (Oracle.cs, OracleBehavior and its SS/SL/CL/RM
// subclasses' movement, Oracle.OracleArm, OracleGraphics) that its data
// object (`oracle_data`, dcon_generated.txt) keeps: what isn't a number of
// the creature payload

#include <cstdint>

#include "dcon_generated_ids.hpp"
#include "math.hpp"
#include "player_types.hpp"

namespace oracle {
    // Oracle.OracleID, with Rivulet's rotted Five Pebbles (OracleID.SS in the
    // RM region: OracleGraphics.IsRottedPebbles, SSOracleRotBehavior) told
    // apart. ST (Chal_AI, arena only) and the Rubicon's (HR_AI, after
    // Saint's ascension) aren't made
    enum class id : uint8_t {
        ss, // Five Pebbles (SS_AI): SSOracleBehavior
        sl, // Looks to the Moon (SL_AI): SLOracleBehavior
        dm, // Spearmaster's Moon (DM_AI): SSOracleBehavior
        cl, // Saint's Pebbles (CL_AI): CLOracleBehavior
        rm, // Rivulet's Pebbles (RM_AI): SSOracleRotBehavior
    };
    // OracleGraphics.IsPebbles (OracleID.SS: the rotted one too)
    constexpr bool is_pebbles(id i) noexcept { return i == id::ss || i == id::rm; }
    // the ones sitting on the floor and crawling (holdKnees, Crawl)
    constexpr bool sits(id i) noexcept { return i == id::sl || i == id::cl || i == id::rm; }

    // the creature payload (creature_types.hpp aliases it as
    // creature::oracle_payload): its oracle_data and which oracle it is
    struct payload {
        uint16_t data = 0;
        id which = id::ss;
    };
    static_assert(sizeof(payload) <= 64);

    // SSOracleBehavior.MovementBehavior / SLOracleBehavior.MovementBehavior,
    // as far as an oracle on its own moves (the rest need the slugcat)
    enum class movement : uint8_t {
        idle,
        meditate,
        // SL's ShowMedia (with no projected image it ends after 11 ticks)
        show_media,
    };

    // Oracle.OracleArm.Joint
    inline constexpr uint32_t joint_count = 4;
    struct arm_joint {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        float total_length = 0.f;
        float inv_kin_flip = 1.f;
    };
    // Oracle.OracleArm
    struct arm_state {
        arm_joint joints[joint_count];
        rw::vec2 corners[4];
        float frame_pos = 0.f;
        float last_frame_pos = 0.f;
        uint8_t base_moving = 0;
        uint8_t is_active = 0;
        // SL in Sofanthiel's timeline: the base hangs elsewhere
        uint8_t sofanthiel = 0;
    };

    // SetUpMarbles' PebblesPearls (SS, DM): the ones not orbiting the oracle
    // hang where they were put (orbiters of another marble are taken at
    // its place): what Idle investigates
    inline constexpr uint32_t marble_count = 38;

    // Oracle + the behaviour's movement state (one struct for every kind:
    // the fields a kind doesn't use stay as they are)
    struct body_state {
        // Oracle.health (SL: neurons left / 5), stun, spasms, dazed
        float health = 1.f;
        int32_t stun = 0;
        int32_t spasms = 0;
        float dazed = 0.f;
        int32_t behavior_time = 0;
        movement move = movement::idle;
        uint8_t floaty = 0;
        // SL/CL/RM: holdKnees; SL: moonActive (Saint's Moon floats)
        uint8_t hold_knees = 1;
        uint8_t moon_active = 0;
        // RainWorldGame.IsMoonActive (Saint's timeline, neurons left)
        uint8_t moon_awake = 0;
        // MoonHasRobe (SL with a gown)
        uint8_t robe = 0;
        // OracleBehavior.lookPoint
        rw::vec2 look_point{0.f, 0.f};
        // SS/SL path: currentGetTo, lastPos, nextPos, the handles, baseIdeal
        rw::vec2 current_get_to{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 next_pos{0.f, 0.f};
        rw::vec2 last_pos_handle{0.f, 0.f};
        rw::vec2 next_pos_handle{0.f, 0.f};
        rw::vec2 base_ideal{0.f, 0.f};
        float path_progression = 1.f;
        float investigate_angle = 0.f;
        float invst_ang_speed = 0.f;
        // SSOracleBehavior.working, getToWorking
        float working = 1.f;
        float get_to_working = 1.f;
        // investigateMarble (index into the marbles, -1 none)
        int32_t investigate_marble = -1;
        int32_t consistent_base_pos_counter = 0;
        int32_t all_still_counter = 0;
        // SL/CL/RM: crawlCounter, dontHoldKnees; SL active: idleCounter,
        // meditateTick, showMediaPhaseTime, timeOutOfSitZone
        float crawl_counter = 0.f;
        int32_t dont_hold_knees = 0;
        float idle_counter = 0.f;
        int32_t meditate_tick = 0;
        int32_t show_media_phase_time = 0;
        int32_t time_out_of_sit_zone = 0;
    };

    // OracleGraphics (purely cosmetic; only updated in viewed rooms)
    inline constexpr uint32_t gown_divs = 11;
    inline constexpr uint32_t halo_connections = 20;
    inline constexpr uint32_t halo_bits = 100; // rings of 10, 30, 60
    inline constexpr uint32_t halo_rings = 10;
    inline constexpr uint32_t umb_coords = 80;
    inline constexpr uint32_t umb_small = 14;
    inline constexpr uint32_t umb_small_points = 20;
    inline constexpr uint32_t disc_small = 10;
    inline constexpr uint32_t disc_small_points = 6;
    // Halo.Connection
    struct halo_connection {
        rw::vec2 stuck_at{0.f, 0.f};
        rw::vec2 handle{0.f, 0.f};
        float light_up = 0.f;
        float last_light_up = 0.f;
    };
    // Halo.MemoryBit
    struct halo_bit {
        float filled = 0.f;
        float last_filled = 0.f;
        float get_to_filled = 0.f;
        float fill_speed = 0.f;
        int32_t blink_counter = 0;
    };
    struct graphics_state {
        uint8_t initiated = 0;
        // head, hands, feet (GenericBodyPart), knees[side, now/last]
        player::body_part head;
        player::body_part hands[2];
        player::body_part feet[2];
        rw::vec2 knees[2][2];
        rw::vec2 look_dir{0.f, 0.f};
        rw::vec2 last_look_dir{0.f, 0.f};
        float eyes_open = 0.f;
        float last_eyes_open = 0.f;
        float breathe = 0.f;
        float breath_fac = 0.f;
        float last_breath_fac = 0.f;
        // ArmJointGraphics: cogRotat, lastCogRotat, lastAC
        float cog_rotat[joint_count] = {};
        float last_cog_rotat[joint_count] = {};
        float last_ac[joint_count] = {};
        // Gown.clothPoints[x, y, pos/lastPos/vel]
        rw::vec2 cloth[gown_divs][gown_divs][3];
        // Halo
        halo_connection connections[halo_connections];
        float connections_fire_chance = 0.f;
        halo_bit bits[halo_bits];
        float ring_rotations[halo_rings][5] = {};
        float expand = 1.f, last_expand = 1.f, get_to_expand = 1.f;
        float push = 0.f, last_push = 0.f, get_to_push = 0.f;
        float white = 0.f, last_white = 0.f, get_to_white = 0.f;
        // UbilicalCord: coord[i, pos/lastPos/vel], smallCords
        rw::vec2 coord[umb_coords][3];
        rw::vec2 small_cords[umb_small][umb_small_points][3];
        float small_lengths[umb_small] = {};
        rw::vec2 small_head_dirs[umb_small];
        uint8_t small_colors[umb_small] = {};
        // DisconnectedUbilicalCord (Moon's)
        rw::vec2 disc_cords[disc_small][disc_small_points][3];
        float disc_lengths[disc_small] = {};
        rw::vec2 disc_head_dirs[disc_small];
        uint8_t disc_colors[disc_small] = {};
        // DM's MoonSigilSprite alpha (kept by the sprite in vanilla)
        float sigil_alpha = 0.f;
    };
}
