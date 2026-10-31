#pragma once

// Jetfish state that isn't a plain property of the creature (JetFish): what
// its data object (`jetfish_data`, dcon_generated.txt) keeps that isn't a
// number: the IndividualVariations, the trail of body directions, its
// SocialMemory of the slugcats, and the JetFishGraphics state (purely
// cosmetic)

#include <cstdint>

#include "dcon_generated_ids.hpp"
#include "math.hpp"
#include "pathing_types.hpp"
#include "player_types.hpp"

namespace jetfish {
    // JetFish: WaterOnly with Air at 1 (and so everything below it): every
    // tile under the surface at the swimmers' resistance of 1, nothing out
    // of the water (FishPather)
    inline constexpr pathing::movement swim{{0, 0, 0, 0, 0, 0, 0}, 10};

    // JetFishAI.Behavior
    enum class behavior : uint8_t {
        idle,
        flee,
        hunt,
        escape_rain,
        return_prey,
        go_to_food,
        get_unstuck,
    };

    // JetFish.IndividualVariations (GenerateIVars, rolled from the
    // creature's ID.RandomSeed)
    struct individual_variations {
        float fatness = 1.f;
        float tentacle_length = 0.5f;
        float tentacle_fatness = 0.5f;
        float flipper_size = 1.f;
        float flipper_orientation = 0.5f;
        uint8_t tentacle_contour = 0;
        uint8_t whiskers = 0;
        uint8_t flipper = 0;
        int32_t whisker_seed = 0;
        rw::vec3 eye_color{1.f, 0.f, 1.f};
    };

    // JetFish.trail: the last 20 directions from the head to the tail,
    // the newest first
    inline constexpr uint32_t trail_length = 20;
    struct trail_state {
        rw::vec2 dir[trail_length];
        uint8_t count = 0;
    };

    // SocialMemory: how much it likes the slugcats it met (Relationship.like,
    // -1 to 1), the few it remembers
    inline constexpr uint32_t remembered = 4;
    struct social_memory {
        dcon::creature_id who[remembered];
        float like[remembered] = {};
    };

    // JetFishGraphics: the two tentacles (TailSegments, 6 each, with their
    // connectionRad), the two flippers and the whiskers (GenericBodyParts,
    // a pair per whisker) with their directions and props, the swim cycle,
    // the z rotation and the eyes out of the water
    inline constexpr uint32_t tail_segments = 6;
    inline constexpr uint32_t max_whiskers = 3;
    struct graphics_state {
        player::body_part tails[2][tail_segments];
        float tail_connection_rad[2][tail_segments] = {};
        player::body_part flippers[2];
        player::body_part whiskers[2][max_whiskers];
        rw::vec2 whisker_directions[max_whiskers];
        float whisker_props[max_whiskers][5] = {};
        float swim = 0.f;
        rw::vec2 z_rotation{0.f, 0.f};
        rw::vec2 last_z_rotation{0.f, 0.f};
        float air_eyes = 0.f;
        // the JetFishFlipper element's sourcePixelSize.x (flipperGraphWidth)
        float flipper_graph_width = 22.f;
    };
}
