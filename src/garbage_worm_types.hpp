#pragma once

// Garbage worm state that isn't a plain property of the creature
// (GarbageWorm, GarbageWormAI, GarbageWormState, GarbageWormGraphics): what
// its data object (`garbage_worm_data`, dcon_generated.txt) keeps that isn't
// a number of the payload
//
// The one tentacle is kept with the worm (like the monster kelp's): a worm
// has exactly one, its TentacleChunks fixed at creation, and nothing a
// creature_part would add.

#include <cstdint>

#include "dcon_generated_ids.hpp"
#include "math.hpp"

namespace garbage_worm {
    // GarbageWorm: tentacle.tChunks = (int)(15 * Lerp(bodySize, 1, 0.5)),
    // at most 15
    inline constexpr uint32_t max_tentacle_chunks = 15;

    // GarbageWormAI.Behavior isn't an enum in vanilla: what the log shows
    // (creature.behavior)
    enum class behavior : uint8_t {
        // no interesting creature: looking at the garbage on the floor
        search,
        // looking at the most interesting creature it tracks
        watch,
        // AI.attackCounter running: diving into the garbage
        dig,
        // drawn back into its hole (extended 0)
        hidden,
    };

    // Tentacle.TentacleChunk, what the worm's tentacle uses of it (no
    // segments, rope or phasing: see the module's header)
    struct tentacle_chunk {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        float rad = 2.f;
        // TentacleChunk.stretchedFac (for stretchedRad)
        float stretched_fac = 1.f;
        rw::ivec2 contact{0, 0};
    };
    // GarbageWorm.tentacle: its chunks (tChunks[count - 1] is the Tip),
    // idealLength, retractFac, limp, floatGrabDest and the TentacleProps'
    // goalAttractionSpeedTip the worm changes every tick
    struct tentacle_state {
        tentacle_chunk c[max_tentacle_chunks];
        uint8_t count = 0;
        uint8_t limp = 0;
        uint8_t has_grab_dest = 0;
        float ideal_length = 400.f;
        float retract_fac = 0.f;
        float goal_attraction_speed_tip = 1.4f;
        rw::vec2 float_grab_dest{0.f, 0.f};
    };

    // GarbageWormAI.CreatureInterest, one per creature its Tracker keeps
    struct creature_interest {
        dcon::creature_id who;
        float base_interest = 0.f;
        float interest = 0.f;
        float danger = 0.f;
        int32_t ticks_since_discovered = 0;
        int32_t ticks_since_visual = 0;
        uint8_t last_visual = 0;
    };
    inline constexpr uint32_t max_interests = 10;
    // MapFloor: up to 12 * bodySize tiles each way
    inline constexpr uint32_t max_floor_tiles = 24;

    // GarbageWormAI
    struct ai_state {
        creature_interest interests[max_interests];
        uint8_t interest_count = 0;
        // currentInterest
        dcon::creature_id current;
        // floorTiles, lookAtFloor
        rw::ivec2 floor_tiles[max_floor_tiles];
        uint8_t floor_count = 0;
        rw::ivec2 look_at_floor{0, 0};
        // stress (0..1), retractCounter, shakeDir
        float stress = 0.f;
        int32_t retract_counter = 0;
        rw::vec2 shake_dir{0.f, 1.f};
        int32_t attack_counter = 0;
        int32_t search_counter = 0;
        int32_t come_back_out_counter = 0;
        uint8_t show_as_angry = 0;
    };

    // GarbageWormState.angryAt: the slugcats it is angry at (EntityIDs in
    // vanilla), shared with the other worms of its room now and then
    inline constexpr uint32_t max_angry_at = 4;
    struct angry_list {
        dcon::creature_id who[max_angry_at];
        uint8_t count = 0;
    };

    // GarbageWormGraphics (purely cosmetic)
    struct graphics_state {
        float sin_wave = 0.f;
        float number_of_waves_on_body = 1.8f;
        float sin_speed = 0.016666668f;
        float swallow[max_tentacle_chunks] = {};
        float last_extended = 1.f;
        float extended = 1.f;
    };
}
