#pragma once

// pole plant state that isn't a plain property of the creature (PoleMimic): what
// its payload (creature_types.hpp) and its data object (`pole_mimic_data`,
// dcon_generated.txt) keep that isn't a number: the stalk's placement, its
// Tentacle's scalars (the tile segments, the grab path, the TentacleChunks
// and the stuck chunks are the data object's vector pools), and
// PoleMimicGraphics'

#include <cstdint>
#include <optional>

#include "dcon_generated_ids.hpp"
#include "math.hpp"
#include "world_types.hpp"

namespace pole_mimic {
    // Tentacle.TentacleChunk (tProps.rope: each hangs on a Rope from the one
    // before it)
    struct tentacle_chunk {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        // PoleMimic.rad
        float rad = 2.f;
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

    // PoleMimic.stickChunks[i]: the creature chunk stuck to tentacle chunk i
    // (none: `who` empty)
    struct stick {
        dcon::creature_id who;
        uint8_t chunk = 0;
    };

    // PoleMimic's placement (Initiate) and its Tentacle's scalars
    struct body_state {
        // shortCutPos (its den's pipe entrance, or the solid tile it grows
        // out of), rootPos, tipPos, stickOutDir, length
        rw::ivec2 short_cut_pos{0, 0};
        rw::vec2 root_pos{0.f, 0.f};
        rw::vec2 tip_pos{0.f, 0.f};
        rw::vec2 stick_out_dir{0.f, 1.f};
        float length = 0.f;
        // Tentacle
        float ideal_length = 0.f;
        float update_counter = 0.f;
        int32_t pull_counter = 0;
        int32_t pulls_this_tick = 0;
        std::optional<rw::vec2> float_grab_dest;
        int32_t backtrack_from = -1;
        // retractFac (its setter clamps it to 0..1)
        float retract_fac = 0.f;
        uint8_t limp = 0;
        // tProps.goalAttractionSpeed, alignToSegmentSpeed (PoleMimic sets
        // them every tick from getToGoalForce)
        float goal_attraction_speed = 0.5f;
        float align_to_segment_speed = 0.05f;
    };

    // RopeGraphic.Segment
    struct rope_segment {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        int32_t bend_index = -1;
    };

    // one leaf: PoleMimicGraphics.leaves[pair, side, 0..2] (position, last
    // position, velocity) and leavesMimic[pair, side, 0..4] (looking like
    // the pole now, last tick, its goal; the flip now, last tick), kept at
    // pair * 2 + side
    struct leaf {
        rw::vec2 p[3] = {};
        float m[5] = {};
    };

    // PoleMimicGraphics' scalars (the rope segments and the leaves are the
    // data object's vector pools, made when a camera first looks at it)
    struct graphics_state {
        float look_like_a_pole = 1.f;
        float last_look_like_a_pole = 1.f;
        float reveal = 0.f;
        float last_reveal = 0.f;
        float reveal_fac = 0.f;
        float flip_point = 1.f;
        uint8_t leaves_flip = 0;
        int32_t first_reveals = 0;
        // InitiateSprites: leafPairs, decoratedLeafPairs, leafSizeFac
        int32_t leaf_pairs = 0;
        int32_t decorated_leaf_pairs = 0;
        float leaf_size_fac = 1.f;
        // InitiateSprites ran (the leaves and rope segments are made)
        uint8_t initiated = 0;
    };
}
