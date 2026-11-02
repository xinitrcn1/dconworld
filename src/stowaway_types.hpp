#pragma once

// Stowaway state that isn't a plain property of the creature (Downpour's
// StowawayBug, StowawayBugAI, StowawayBugState, StowawayBugGraphics): what
// its data object (`stowaway_data`, dcon_generated.txt) keeps that isn't a
// number of the payload: the placement, the three heads' Tentacle scalars
// (their TentacleChunks, tile segments and the feelers' points are the data
// object's vector pools) and the graphics module's rope graphics.

#include <cstdint>

#include "dcon_generated_ids.hpp"
#include "math.hpp"
#include "world_types.hpp"

namespace stowaway {
    // StowawayBugAI.Behavior
    enum class behavior : uint8_t {
        idle,
        escape_rain,
        hidden,
        attacking,
        digesting,
        sleeping,
    };

    // CreatureTemplate.grasps: one head (a Tentacle) per grasp
    inline constexpr uint32_t head_count = 3;
    // heads[j].tChunks.Length
    inline constexpr uint32_t head_chunks = 12;
    // tentacles.Length: Random.Range(7, 12)
    inline constexpr uint32_t max_feelers = 11;
    // what it can be eating at once (EatObject): what its heads held
    inline constexpr uint32_t max_eat = 4;
    // StowawayRopeGraphics: RopeGraphic(40)
    inline constexpr uint32_t rope_segments = 40;

    // Tentacle.TentacleChunk (tProps.rope: each hangs on a Rope from the one
    // before it)
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
        world::rope_state rope;
    };

    // one head: its Tentacle's scalars, headFired, headCooldown, and how many
    // of the segments pool's tiles are its (the heads' segments follow each
    // other there, head 0's first)
    struct head_state {
        float ideal_length = 0.f;
        float update_counter = 0.f;
        int32_t pull_counter = 0;
        int32_t pulls_this_tick = 0;
        int32_t backtrack_from = -1;
        // retractFac (its setter clamps it to 0..1)
        float retract_fac = 1.f;
        float cooldown = 0.f;
        uint16_t seg_count = 0;
        uint8_t limp = 1;
        uint8_t fired = 0;
    };

    // StowawayBug.EatObject: a chunk of what it killed, pulled into its mouth
    struct eat_object {
        dcon::creature_id who;
        uint8_t chunk = 0;
        float distance = 0.f;
        float progression = 0.f;
    };

    // a feeler's point: tentacles[i][j, 0..2] (position, last position, velocity)
    struct feeler_point {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
    };

    // StowawayBug's placement and the rest of its scalars that aren't the
    // payload's
    struct body_state {
        // StowawayBugState.HomePos, aimPos; originalPos
        rw::vec2 home_pos{0.f, 0.f};
        rw::vec2 aim_pos{0.f, -20.f};
        rw::vec2 original_pos{0.f, 0.f};
        // placedDirection, goalDirection, currentDirection, lastDirection
        rw::vec2 placed_direction{0.f, -1.f};
        rw::vec2 goal_direction{0.f, -1.f};
        rw::vec2 current_direction{0.f, -1.f};
        rw::vec2 last_direction{0.f, -1.f};
        // colorPickPos (where ApplyPalette reads the level's colour)
        rw::vec2 color_pick_pos{0.f, 0.f};
        // headLength
        float head_length = 150.f;
        // StowawayBugState: digestionTimeStart, digestionLength
        int32_t digestion_time_start = 0;
        int32_t digestion_length = 0;
        // teethCount, teethSeed
        int32_t teeth_count = 3;
        int32_t teeth_seed = 1;
        // the feelers: how many, and each one's point count (their points
        // follow each other in the feelers pool)
        uint8_t feeler_count = 0;
        uint8_t feeler_len[max_feelers] = {};
        head_state heads[head_count];
        // grasps[2] (the creature grasps only have two: the third head's is
        // kept here, counted in its body's grabbed_by_count)
        dcon::creature_id extra_grasp;
        uint8_t extra_grasp_chunk = 0;
        uint8_t eat_count = 0;
        eat_object eat[max_eat];
    };

    // RopeGraphic.Segment
    struct rope_segment {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        int32_t bend_index = -1;
    };

    // StowawayBugGraphics' own scalars (mouthOpen, biting, digestPrey are
    // the simulation's, in the payload; the body orbs and the teeth are
    // rolled from the creature's seeds when drawn)
    struct graphics_state {
        float over_shoot = 0.f;
        float over_shoot_counter = 0.f;
        float over_shoot_scale = 0.f;
        // the rope graphics' segments are made (reset_positions)
        uint8_t initiated = 0;
    };
}
