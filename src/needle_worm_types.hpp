#pragma once

// Noodlefly state that isn't a plain property of the creature (BigNeedleWorm /
// SmallNeedleWorm (NeedleWorm.cs) and their AIs): what its payload
// (creature_types.hpp) and its data object (`needle_worm_data`,
// dcon_generated.txt) keep that isn't a number

#include <cstdint>

#include "dcon_generated_ids.hpp"
#include "math.hpp"
#include "pathing_types.hpp"
#include "player_types.hpp"
#include "world_types.hpp"

namespace needle_worm {
    // the template: BigNeedleWorm (the adult) or SmallNeedleWorm (its young)
    enum class variant : uint8_t { big, small };
    inline constexpr uint32_t variant_count = 2;

    // NeedleWormAI.Behavior (Hunt and GetUnstuck are never picked here)
    enum class behavior : uint8_t {
        idle,
        flee,
        hunt,
        attack,
        faux_attack,
        get_unstuck,
        escape_rain,
        migrate,
    };

    // BigNeedleWorm (SmallNeedleWorm inherits it): Air at 1, and so
    // everything below it (CreatureTemplate caps the lesser accessibilities
    // at the highest allowed one). NeedleWormAI.TravelPreference adds
    // LerpMap(terrain proximity, 1, 6, 100, 0, 2) resistance to a step near
    // the walls: here CicadaPather's open air cost, up to 10 on top of 1 (the
    // fields' 16 bit costs)
    inline constexpr pathing::movement fly{{10, 10, 10, 10, 10, 10, 0}, 0, 10};

    // the body: 5 chunks and 10 tail segments for the adult, 3 and 4 for the young
    inline constexpr uint32_t max_chunks = 5;
    inline constexpr uint32_t max_tail = 10;
    inline constexpr uint32_t max_segments = max_chunks + max_tail;
    // BigNeedleWorm.fangLength
    inline constexpr float fang_length = 50.f;
    // SmallNeedleWorm.FoodPoints
    inline constexpr int32_t food_points = 2;

    // NeedleWorm.tail[i, 0..2] (position, last position, velocity), and
    // segmentsInRopeMode per segment (the chunks first, then the tail)
    struct tail_state {
        rw::vec2 pos[max_tail] = {};
        rw::vec2 last_pos[max_tail] = {};
        rw::vec2 vel[max_tail] = {};
        uint8_t rope_mode[max_segments] = {};
    };

    // NeedleWorm.connectionRopes: a rope between every two segments
    struct connection_ropes {
        world::rope_state r[max_segments - 1];
    };

    // NeedleWorm.prevPositions (newest first)
    struct prev_positions {
        rw::vec2 p[10] = {};
    };

    // BigNeedleWorm's attack: the charge, the swish through the air, the
    // fang stuck in a wall or in a creature
    struct attack_state {
        float attack_ready = 0.f;
        float charging_attack = 0.f;
        // swishDir (has_swish: not null), swishCounter
        rw::vec2 swish_dir{0.f, 0.f};
        uint8_t has_swish = 0;
        int32_t swish_counter = 0;
        // stuckInWallPos (in_wall: not null), stuckDir, stuckTime
        uint8_t in_wall = 0;
        rw::vec2 stuck_in_wall_pos{0.f, 0.f};
        rw::vec2 stuck_dir{0.f, 0.f};
        float stuck_time = 0.f;
        // impaleChunk: a creature's chunk, and impaleDistances[i, 0] (no
        // rotationChunk here)
        dcon::creature_id impale;
        uint8_t impale_chunk = 0;
        float impale_distances[max_chunks] = {};
        int32_t lame_counter = 0;
        // swishAdd (has_swish_add: not null)
        rw::vec2 swish_add{0.f, 0.f};
        uint8_t has_swish_add = 0;
        int32_t dodge_delay = 0;
    };

    // BigNeedleWormAI's own state
    struct big_ai_state {
        rw::vec2 attack_from_pos{0.f, 0.f};
        rw::vec2 attack_target_pos{0.f, 0.f};
        rw::vec2 target_vel{0.f, 0.f};
        int32_t attack_counter = 0;
        int32_t target_chunk = 0;
        float ideal_attack_dist = 200.f;
        int32_t respond_scream_counter = 0;
        // keepCloseToCreature (a crying young's closest creature)
        dcon::creature_id keep_close_to;
        // the prey tracker's smoothed utility (FloatTweenBasic Lerp 0.15)
        float prey_utility = 0.f;
    };

    // SocialMemory: the adult's like and temporary like of the creatures
    // that hurt it or its young (the least disliked forgotten for a new one)
    struct social_entry {
        dcon::creature_id who;
        float like = 0.f;
        float temp_like = 0.f;
    };
    struct social_memory {
        static constexpr uint32_t size = 4;
        social_entry e[size];
    };

    // NeedleWormAI's idle wandering
    struct ai_state {
        rw::ivec2 idle_pos{0, 0};
        rw::ivec2 last_assigned_idle_pos{0, 0};
        int32_t idle_counter = 0;
        int32_t add_old_idle_pos_delay = 0;
        int32_t in_free_space_counter = 100;
        int32_t in_room_counter = 0;
        float fly_height_add = 0.f;
        // focusCreature
        dcon::creature_id focus;
        // the creature hurting it right now (a noodlefly's stab sets it;
        // otherwise the closest slugcat is blamed), for SocialEventRecognizer
        dcon::creature_id attacker;
        // oldIdlePositions (oldest first; vanilla keeps 20 for the adult)
        static constexpr int32_t max_old = 10;
        rw::ivec2 old_idle[max_old] = {};
        int32_t old_count = 0;
    };

    // SmallNeedleWorm: its mother (the closest adult in its room), hanging
    // on her tail, the cry when caught, the bites a slugcat takes of it
    struct small_state {
        dcon::creature_id mother;
        uint8_t mom_tail_segment = 0;
        uint8_t has_screamed = 0;
        uint8_t bites = 5;
        float scream_counter = 0.f;
    };

    // tracked.state bits (NeedleWormAI.NeedleWormTrackState: close, holdingChild)
    inline constexpr uint8_t track_close = 1;
    inline constexpr uint8_t track_holding_child = 2;

    // NeedleWormGraphics
    struct graphics_state {
        // snout[], wings[side, i], legs[side, i] (GenericBodyParts)
        player::body_part snout[5];
        player::body_part wings[2][2];
        player::body_part legs[2][3];
        float wing_flap = 0.f;
        float last_wing_flap = 0.f;
        float flying = 0.f;
        float last_flying = 0.f;
        rw::vec2 z_rot{0.f, 1.f};
        rw::vec2 last_z_rot{0.f, 1.f};
        float fang_out = 0.f;
        float last_fang_out = 0.f;
        float fang_black = 0.f;
        float thin_tail = 0.f;
        float whiplash = -1.f;
        float last_whiplash = -1.f;
        // the looks, rolled from the creature's ID
        float wings_size = 1.f;
        float legs_fac = 1.f;
        float fatness = 0.5f;
        float snout_length = 1.f;
        float hue = 0.5f;
        float lightness = 0.4f;
        float hue_div = 0.f;
        uint8_t cos_bools[4] = {};
    };
}
