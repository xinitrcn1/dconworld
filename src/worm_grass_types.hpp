#pragma once

// Worm grass state that isn't a plain property of its data objects
// (WormGrass.WormGrassPatch, WormGrass.Worm)

#include <cstdint>

#include "dcon_generated_ids.hpp"
#include "math.hpp"

namespace worm_grass {
    // WormGrass.Worm.Segment
    struct segment {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        float rad = 0.f;
        float stretched_rad = 0.f;
    };
    // Worm.segments: 2 to 4 of them
    struct segments {
        static constexpr uint32_t capacity = 4;
        segment s[capacity];
        uint8_t count = 0;
    };

    // WormGrassPatch.CreatureAndPull: a creature the patch has noticed, how
    // hard it pulls it down and how far it has buried it
    struct tracked {
        dcon::creature_id creature;
        float pull = 0.f;
        float bury = 0.f;
        int32_t consume_timer = 0;
        rw::vec2 consumed_pos{0.f, 0.f};
    };
    // WormGrassPatch.trackedCreatures (vanilla's list has no limit; a patch
    // tracking this many notices no more until one leaves)
    struct tracked_set {
        static constexpr uint32_t capacity = 8;
        tracked t[capacity];
        uint8_t count = 0;
    };
}
