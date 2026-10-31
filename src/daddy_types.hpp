#pragma once

// Daddy long legs state that isn't a plain property of the daddy object:
// Tentacle/DaddyTentacle, the DaddyAI modules (a one slugcat Tracker, the
// NoiseTracker), the eat list and the DaddyGraphics parts (eyes, tube
// graphics, the Hunter's dummy), stored with the daddy and its tentacle
// and tube objects

#include <cstdint>
#include <optional>

#include "math.hpp"
#include "player_types.hpp"
#include "dcon_generated_ids.hpp"
#include "world_types.hpp"

namespace daddy {
    // The CreatureTemplates DaddyLongLegs.cs runs: DaddyLongLegs, BrotherLongLegs,
    // Downpour's TerrorLongLegs (the mother long legs: superSizeMe) and
    // HunterDaddy (HDmode: Hunter's corpse wearing the rot)
    enum class variant : uint8_t {
        daddy,
        brother,
        mother,
        grandmother,
        hunter,
    };
    inline constexpr uint32_t variant_count = 4;

    // DaddyAI.Behavior
    enum class behavior : uint8_t {
        idle,
        hunt,
        escape_rain,
        examine_sound,
    };

    // DaddyTentacle.Task (`none`: the null a fresh DaddyTentacle starts with)
    enum class task : uint8_t {
        none,
        locomotion,
        hunt,
        examine_sound,
        grabbing,
    };

    // bounds of the constructor's rolls: Random.Range(4, 16) chunks for the
    // mother, Random.Range(5, 13) tentacles, DaddyState.tentacleHealth[13],
    // Random.Range(5, 15) tubes less the dead legs
    inline constexpr uint32_t max_chunks = 30;
    inline constexpr uint32_t max_tentacles = 12;
    inline constexpr uint32_t max_tubes = 16;
    inline constexpr uint32_t max_sources = 8;
    inline constexpr uint32_t max_eats = 4;
    inline constexpr uint32_t past_positions = 80;

    // Tentacle.TentacleChunk
    struct tentacle_chunk {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        float rad = 3.f;
        float t_pos = 0.f;
        rw::ivec2 contact_point{0, 0};
        rw::ivec2 last_contact_point{0, 0};
        rw::vec2 phase_from{0.f, 0.f};
        int32_t phases_to_same_location = 0;
        rw::ivec2 after_phase_stuck_pos{0, 0};
        float phase = -1.f;
        // currentSegmentTrail[0], the only entry anything reads
        rw::ivec2 trail{0, 0};
        int32_t phase_attempts = 0;
        float stretched_fac = 1.f;
        // DaddyTentacle.chunksStickSounds[i]
        int32_t stick_sounds = 0;
        world::rope_state rope;
    };

    // Tentacle + DaddyTentacle, the scalars (the segments, grab path and
    // chunks are the daddy_tentacle object's vector pools)
    struct tentacle {
        // Tentacle
        float ideal_length = 0.f;
        uint8_t connected_chunk = 0;
        float update_counter = 0.f;
        int32_t pull_counter = 0;
        int32_t pulls_this_tick = 0;
        std::optional<rw::vec2> float_grab_dest;
        int32_t backtrack_from = -1;
        float retract_fac = 0.f;
        uint8_t limp = 0;
        // DaddyTentacle (tentacleNumber and stun are the creature_part's
        // index and stun)
        rw::vec2 tentacle_dir{0.f, 1.f};
        float away_from_body_rotation = 0.f;
        uint8_t at_grab_dest = 0;
        int32_t found_no_grab_pos = 0;
        float chunks_gripping = 0.f;
        int32_t secondary_grab_back_track_counter = 0;
        uint8_t last_back_track = 0;
        rw::ivec2 secondary_grab_pos{0, 0};
        rw::vec2 preliminary_grab_dest{0.f, 0.f};
        rw::vec2 ideal_grab_pos{0.f, 0.f};
        uint8_t needed_for_locomotion = 0;
        // huntCreature: the creature or slugcat it tracks
        dcon::creature_id hunt;
        // checkSound: the noise source's id (0 none)
        uint16_t check_sound = 0;
        int32_t sound_check_counter = 0;
        int32_t sound_check_timer = 0;
        std::optional<rw::vec2> examine_sound_pos;
        // grabChunk: a creature's or slugcat's chunk
        dcon::creature_id grab;
        uint8_t grab_chunk = 0;
        float sticky = 0.f;
        task current = task::none;
    };

    // RopeGraphic.Segment
    struct rope_segment {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        int32_t bend_index = -1;
    };
    // DaddyTubeGraphic.Bump
    struct bump {
        rw::vec2 pos{0.f, 0.f};
        float size = 0.f;
        float eye_size = 0.f;
    };
    // the DaddyTubeGraphics: DaddyLegGraphic (one per tentacle),
    // DaddyDeadLeg, DaddyDangleTube
    enum class tube_kind : uint8_t {
        leg,
        dead_leg,
        dangler,
    };
    // DaddyDangleTube.Connection: a body chunk, or a point along a leg graphic
    struct dangle_connection {
        int8_t chunk = -1;
        int8_t leg = -1;
        float leg_pos = 0.f;
    };

    // DaddyLongLegs.EatObject: a creature's or slugcat's chunk pulled into the body
    struct eat_object {
        dcon::creature_id who;
        uint8_t chunk = 0;
        float distance = 0.f;
        float progression = 0.f;
    };

    // DaddyLongLegs' movement state and its eat list
    struct body_state {
        rw::vec2 move_direction{0.f, -1.f};
        uint8_t moving = 0;
        int32_t stuck_counter = 0;
        uint8_t squeeze = 0;
        float squeeze_fac = 0.f;
        int32_t eyes_closed = 0;
        int32_t digesting_counter = 0;
        int32_t not_following_path_counter = 0;
        float unconditional_support = 0.f;
        // pastPositions, newest first (a ring of the last 80 tiles)
        rw::ivec2 past[past_positions];
        int32_t past_count = 0;
        int32_t past_head = 0;
        // Creature.deaf
        int32_t deaf = 0;
        eat_object eats[max_eats];
        uint8_t eat_count = 0;
    };

    // NoiseTracker.TheorizedSource; `id` names it for the tentacles and eyes
    // looking at it (0 is none)
    struct noise_source {
        rw::vec2 pos{0.f, 0.f};
        int32_t age = 0;
        uint16_t id = 0;
        dcon::creature_id who; // creatureRep
        uint8_t slated_for_deletion = 0;
    };
    struct noise_tracker {
        noise_source sources[max_sources];
        uint8_t count = 0;
        uint16_t next_id = 1;
        uint16_t sound_to_examine = 0;
        float mysterious_noises = 0.f;
        int32_t mysterious_noise_counter = 0;
        int32_t forget_time = 320;
    };

    // DaddyAI with its Tracker, PreyTracker and NoiseTracker. A blind
    // creature knows only what its tentacles feel (and hears): one
    // representation here, the creature or slugcat it hunts (felt this
    // tick, ticks since, where); the behavior and destination are the
    // creature's
    struct ai_state {
        dcon::creature_id seen;
        uint8_t sees = 0;
        int32_t ticks_since_seen = 0;
        rw::vec2 last_seen_pos{0.f, 0.f};
        int32_t new_idle_pos_counter = 0;
        // the goal of the field it follows (see daddy.cpp's destination_field)
        rw::ivec2 path_goal{0, 0};
        uint8_t has_path_goal = 0;
        uint32_t path_goal_tick = 0;
        int32_t react_noise_time = 0;
        rw::ivec2 react_target{0, 0};
        // felt this tick (SeeCreature)
        uint8_t seen_this_tick = 0;
        noise_tracker noise;
    };

    // DaddyGraphics.Eye, one per body chunk
    struct eye {
        rw::vec2 dir{0.f, 0.f};
        rw::vec2 last_dir{0.f, 0.f};
        float rad = 1.f;
        float rotats[2] = {0.f, 0.f};
        float focus = 0.f;
        float last_focus = 0.f;
        float get_to_focus = 0.f;
        float closed = 0.f;
        float last_closed = 0.f;
        int32_t eyes_closed_delay = 0;
        uint16_t sound_source = 0;
        dcon::creature_id creature_seen;
        float light = 0.f;
        float flash = 0.f;
        // the JaggedCircle's rim, rolled once (the shader's noise)
        uint32_t jag_seed = 0;
    };

    // DaddyGraphics.HunterDummy: the slugcat body of the Hunter long legs
    struct hunter_dummy {
        player::body_part tail[4];
        player::body_part head;
        player::body_part legs;
        // drawPositions[chunk, 0 now / 1 last]
        rw::vec2 draw_pos[2][2] = {};
        rw::vec2 legs_direction{0.f, -1.f};
        float breath = 0.f;
        float last_breath = 0.f;
    };

    // DaddyGraphics
    struct graphics_state {
        float digesting = 0.f;
        // digestLoop's volume (a StaticSoundLoop at the middle of the body)
        float digest_loop_volume = 0.f;
        int32_t reaction_sound_delay = 0;
        int32_t feel_something_reaction_delay = 0;
        eye eyes[max_chunks];
        hunter_dummy dummy;
        // (its UnityEngine.Random is the creature's graphics_random)
    };
}
