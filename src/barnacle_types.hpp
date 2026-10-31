#pragma once

// Barnacle state that isn't a plain property of the creature (Watcher's
// Barnacle, BarnacleAI, BarnacleGraphics): what its data object
// (`barnacle_data`, dcon_generated.txt) keeps that isn't a hot number: the
// CreatureParams rolled from the ID, the body's movement state the payload
// has no room for, BarnacleAI's sub behaviours and spots, and the graphics
// module (the six legs and the shell's cones, purely cosmetic). All plain
// trivially copyable structs (no std::optional: a `has_` flag beside each
// nullable value).

#include <cstdint>

#include "dcon_generated_ids.hpp"
#include "math.hpp"
#include "pathing_types.hpp"
#include "player_types.hpp"

namespace barnacle {
    // How it paths (StaticWorld.InitBarnacle): floors and walls at 1.5,
    // poles and ceilings at 2, corridors at 3 Unwanted (the dearest here,
    // like the other ports' Unwanted), swimming at waterPathingResistance 1
    inline constexpr pathing::movement walk{{15, 100, 20, 15, 20, 0, 0}, 10};

    // BarnacleAI.Behavior (InvestigateSound and GetUnstuck never come up:
    // no NoiseTracker, no StuckTracker)
    enum class behavior : uint8_t {
        idle,
        travelling,
        flee,
        investigate_sound,
        escape_rain,
        get_unstuck,
    };

    // BarnacleAI.SubBehavior
    enum class sub_behavior : uint8_t {
        none,
        travel_to_water,
        travel_to_idle,
        temporary_panic,
    };

    // Barnacle.CreatureParams (rolled from the creature's ID.RandomSeed)
    struct creature_params {
        float total_mass = 3.6f;
        float size_multiplier = 1.f;
        float travel_speed_factor = 0.5f;
        float flee_speed_factor = 1.25f;
        float idle_speed_factor = 0.5f;
        float travel_swim_factor = 0.3f;
        float flee_swim_factor = 0.6f;
        float idle_swim_factor = 0.3f;
        uint8_t separate_swim_behavior = 1;
        float jump_out_of_water_force = 1.f;
        float leg_thickness = 1.f;
        rw::vec3 dark_shell_color{0.5f, 0.4f, 0.3f};
        rw::vec3 light_shell_color{0.7f, 0.6f, 0.5f};
        float eye_size_multiplier = 1.f;
        int32_t cones = 15;
        float cone_rad_variance = 0.f;
        float cone_length_variance = 0.5f;
        float cone_color_variance = 0.f;
        float tip_rad_factor = 0.46153846f;
    };

    // MovementConnection as the path field gives it: a step from a tile to
    // the next (a diagonal one is an OpenDiagonal special move, more than a
    // tile down a DropToFloor)
    struct connection {
        rw::ivec2 start{-1, -1};
        rw::ivec2 dest{-1, -1};
        uint8_t valid = 0;
    };

    // Barnacle.cs's movement state the payload has no room for
    struct body_state {
        rw::vec2 swim_dir{0.f, 0.f};
        float swim_speed = 0.f;
        float surf_speed = 0.f;
        float slow_down_for_precision = 0.f;
        rw::vec2 jump_target{0.f, 0.f};
        int32_t act_counter = 0;
        int32_t move_counter = 0;
        int32_t wet_counter = 0;
        rw::vec2 recent_position{0.f, 0.f};
        rw::ivec2 special_move_destination{0, 0};
        connection last_followed;
        int32_t clink_sound_cooldown = 0;
        // IPlayerEdible: bites left
        int32_t bites = 5;
    };

    // BarnacleAI's own state (the payload keeps the hot numbers)
    struct ai_state {
        sub_behavior sub = sub_behavior::none;
        int32_t in_sub_behavior_time = 0;
        int32_t next_sub_behavior_time = 0;
        int32_t in_water_time = 0;
        // bestNarrowPos, bestNarrowScore, blacklistIdlePos
        uint8_t has_best_narrow = 0;
        rw::vec2 best_narrow_pos{0.f, 0.f};
        int32_t best_narrow_score = 0;
        uint8_t has_blacklist = 0;
        rw::vec2 blacklist_idle_pos{0.f, 0.f};
        // waterIdleSpot, panicSpot, nextPanicSpotShuffle
        uint8_t has_water_idle = 0;
        rw::vec2 water_idle_spot{0.f, 0.f};
        uint8_t has_panic_spot = 0;
        rw::vec2 panic_spot{0.f, 0.f};
        int32_t next_panic_spot_shuffle = 0;
        // floatGoalPos (fgp), exploreCoordinate
        uint8_t has_float_goal = 0;
        rw::vec2 float_goal{0.f, 0.f};
        uint8_t has_explore = 0;
        rw::ivec2 explore{0, 0};
        // following the leader: leaderIdleSpot, its expiry, whether it was a
        // distant one; invalidLeaderTime
        rw::vec2 leader_idle_spot{0.f, 0.f};
        int32_t leader_idle_spot_expiry = 0;
        uint8_t leader_idle_spot_was_distant = 0;
        int32_t invalid_leader_time = 0;
        // fear (the ThreatTracker's smoothed utility, once the shell is
        // gone), rainFear
        float fear = 0.f;
        float rain_fear = 0.f;
    };

    // BarnacleGraphics.Cone (basePos, tipPos on the shell's sphere)
    struct cone {
        rw::vec3 base_pos{0.f, 0.f, 0.f};
        rw::vec3 tip_pos{0.f, 0.f, 0.f};
        float base_rad = 0.f;
        float tip_rad = 0.f;
        float eye_rad = 0.f;
        float lightness = 0.5f;
    };
    // CreatureParams.cones: Random.Range(13, 20)
    inline constexpr uint32_t max_cones = 20;
    inline constexpr uint32_t leg_count = 6;
    // BarnacleGraphics innerRad, outerRad, legLength
    inline constexpr float inner_rad = 13.f;
    inline constexpr float outer_rad = 35.f;
    inline constexpr float leg_length = 22.f;

    // BarnacleGraphics (purely cosmetic): the legs (Limb), the cones laid
    // out by GenerateCones, roll, walk sway, blink, the CreatureLooker's pick
    struct graphics_state {
        player::hand_state legs[leg_count];
        cone cones[max_cones];
        uint8_t cone_count = 0;
        float roll = 0.f;
        float last_roll = 0.f;
        float walk_sway = 0.f;
        int32_t blink = 0;
        dcon::creature_id look;
        int32_t look_counter = 0;
    };
}
