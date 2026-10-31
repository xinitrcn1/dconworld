#pragma once

// monster kelp state that isn't a plain property of the creature (TentaclePlant): what
// its payload (creature_types.hpp) and its data object (`tentacle_plant_data`,
// dcon_generated.txt) keep that isn't a number
//
// The one tentacle is kept with the plant, not as a creature_part: a plant
// has exactly one, always, with a fixed eight TentacleChunks and nothing of
// its own a part would add (no stun or health of its own, unlike a daddy's
// tentacles: the plant is the tentacle). So its state is a few properties of
// the plant's data object, the tile lists in vector pools like
// daddy_tentacle's, with no part object, index or ownership to keep.

#include <cstdint>
#include <optional>

#include "dcon_generated_ids.hpp"
#include "math.hpp"
#include "world_types.hpp"

namespace tentacle_plant {
    // TentaclePlant: tentacle.tChunks (8), new Tentacle(.., 300f), rootRad, tipRad
    inline constexpr uint32_t chunk_count = 8;
    inline constexpr float ideal_length = 300.f;
    inline constexpr float root_rad = 8.f;
    inline constexpr float tip_rad = 1.f;
    // TentaclePlantGraphics: RopeGraphic(40), danglers[50]
    inline constexpr uint32_t rope_segments = 40;
    inline constexpr uint32_t dangler_count = 50;

    // Tentacle.TentacleChunk
    struct tentacle_chunk {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        float rad = 1.f;
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
    struct chunk_array {
        tentacle_chunk c[chunk_count];
    };

    // Tentacle, the scalars (the segments and grab path are the data
    // object's vector pools; connectedChunk is bodyChunks[1], the root)
    struct tentacle {
        float update_counter = 0.f;
        int32_t pull_counter = 0;
        int32_t pulls_this_tick = 0;
        std::optional<rw::vec2> float_grab_dest;
        int32_t backtrack_from = -1;
        float retract_fac = 0.f;
        uint8_t limp = 0;
    };

    // RopeGraphic.Segment (TentaclePlantRopeGraphics)
    struct rope_segment {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        int32_t bend_index = -1;
    };
    // Dangler.DanglerSegment (its own rad, not BodyPart's, which stays 0)
    struct dangler_segment {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        float rad = 5.f;
        float stretched_rad = 5.f;
        float con_rad = 5.f;
    };
    // a Dangler: danglerProps[i, 0..2] and its segments in the segment pool
    struct dangler {
        float props[3] = {0.f, 0.f, 0.f};
        uint16_t first = 0;
        uint8_t count = 0;
    };

    // the graphics module's state (purely cosmetic): the rope, the danglers
    // and their segments are the data object's vector pools, laid out (from
    // the plant's seed, like the TentaclePlantGraphics constructor) the
    // first time a camera looks at the plant
    struct graphics_state {
        uint8_t built = 0;
    };
}
