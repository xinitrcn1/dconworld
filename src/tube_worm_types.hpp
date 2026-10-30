#pragma once

// Grapple worm state that isn't a plain property of the worm: its two
// tongues (TubeWorm.Tongue, each with its Rope) and the TubeWormGraphics
// state (purely cosmetic), stored with the worm

#include <cstdint>

#include "dcon_generated_ids.hpp"
#include "math.hpp"
#include "player_types.hpp"
#include "world_types.hpp"

namespace tube_worm {
    // TubeWorm.Tongue.Mode
    enum class tongue_mode : uint8_t {
        retracted,
        shooting_out,
        attached_to_terrain,
        attached_to_object,
        retracting,
    };

    // TubeWorm.Tongue: tongues[0] shoots from the front end (chunk 0, or the
    // slugcat holding the worm), tongues[1] from the back end (chunk 1)
    struct tongue {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        rw::vec2 terrain_stuck_pos{0.f, 0.f};
        // attachedChunk: a creature's chunk (a slugcat's included)
        dcon::creature_id attached;
        uint8_t attached_chunk = 0;
        tongue_mode mode = tongue_mode::retracted;
        uint8_t returning = 0;
        float requested_rope_length = 0.f;
        float ideal_rope_length = 150.f;
        float elastic = 0.f;
        float rope_extend_speed = 0.f;
        // new Rope(room, baseChunk.pos, pos, 1f)
        world::rope_state rope;

        // Tongue.Attached, Tongue.Free
        constexpr bool is_attached() const noexcept { return mode == tongue_mode::attached_to_terrain || mode == tongue_mode::attached_to_object; }
        constexpr bool free() const noexcept { return mode == tongue_mode::shooting_out || mode == tongue_mode::retracting; }
    };
    struct tongue_pair {
        tongue t[2];
    };

    // TubeWormGraphics: four GenericBodyParts (the front tongue's root, the
    // two chunks, the back tongue's root) and the 20 RopeSegments laid
    // along the tongues and the body
    inline constexpr uint32_t body_parts = 4;
    inline constexpr uint32_t rope_segments = 20;
    struct rope_segment {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        uint8_t claimed_for_bend = 0;
    };
    struct graphics_state {
        player::body_part body[body_parts];
        rope_segment rope[rope_segments];
        float stretch = 0.f;
        float last_stretch = 0.f;
        // the body's colour, rolled from the creature's seed
        rw::vec3 color{0.f, 0.f, 0.f};
    };
}
