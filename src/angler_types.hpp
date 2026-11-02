#pragma once

// Angler state (the Watcher's Angler, AnglerAI, AnglerGraphics): its payload
// (creature_types.hpp's angler_payload is this struct) and what its data
// object (`angler_data`, dcon_generated.txt) keeps: the looks rolled from
// its ID (AnglerGraphics' constructor: the teeth, the lure arm's lengths,
// the eye), the lure (AnglerGraphics.lightPos, moved into the simulation:
// the AI sees from it), the AI's goals and the purely cosmetic graphics
// state.

#include <cstdint>

#include "dcon_generated_ids.hpp"
#include "math.hpp"
#include "pathing_types.hpp"

namespace angler {
    // Angler: bodyChunks[3] (head, middle, tail)
    inline constexpr uint32_t body_chunks = 3;

    // The Angler template: WaterOnly, Air at 1 (FishPather): every tile
    // under the surface at the swimmers' resistance of 1 (the jetfish's
    // movement, so they share its path fields). AImap.IsTooCloseToTerrain's
    // keeping it 2 tiles off the walls isn't ported
    inline constexpr pathing::movement swim{{0, 0, 0, 0, 0, 0, 0}, 10};

    // AnglerAI.Behavior
    enum class behavior : uint8_t {
        idle,
        flee,
        hunt,
        stalk,
        escape_rain,
        get_unstuck,
    };

    // Angler + AnglerAI's numbers: the creature's payload (at most 64 bytes)
    struct payload {
        // Angler.swimDir, flip, rudderTurnWay, mouthOpenness, swimSpeed,
        // swimSpeedGoal, squeezeFac
        rw::vec2 swim_dir;
        float flip;
        float rudder_turn_way;
        float mouth_openness;
        float swim_speed;
        float swim_speed_goal;
        float squeeze_fac;
        // AnglerGraphics.angerRiskTarget, which the AI sets (the graphics
        // ease their angerRisk to it)
        float anger_risk_target;
        // Angler.consiousCounter
        int32_t consious_counter;
        // AnglerAI.angerTimer, preyAwayTimer, lockedCreature
        int16_t anger_timer;
        int16_t prey_away_timer;
        dcon::creature_id locked;
        // Angler.jawState, jawProgress, squeeze
        uint8_t jaw_state;
        uint8_t jaw_progress;
        uint8_t squeeze;
        // angler_data
        uint16_t data;
    };

    // AnglerGraphics.Tooth
    struct tooth {
        float length = 10.f;
        float rad = 1.f;
        float curve = 0.1f;
        float forward_tilt = 1.f;
    };
    // AnglerGraphics.Spike (the seven are always the same)
    struct spike {
        float length;
        float rad;
        bool top_side;
        float placement;
    };
    inline constexpr uint32_t spike_count = 7;
    inline constexpr spike spikes[spike_count] = {
        {40.f, 2.f, true, 1.f},
        {60.f, 2.f, true, 0.5f},
        {60.f, 2.f, false, 0.5f},
        {20.f, 1.5f, true, 0.25f},
        {20.f, 1.5f, false, 0.25f},
        {20.f, 1.5f, true, 0.65f},
        {20.f, 1.5f, false, 0.65f},
    };
    // Random.Range(7, 14) teeth a jaw at most
    inline constexpr uint32_t max_teeth = 13;
    // numberOfProps: Random.Range(3, 7)
    inline constexpr uint32_t max_props = 6;

    // AnglerGraphics' constructor rolls (Random.InitState(ID.RandomSeed))
    struct looks {
        uint8_t number_of_props = 5;
        uint8_t bottom_count = 0;
        uint8_t top_count = 0;
        tooth bottom_teeth[max_teeth];
        tooth top_teeth[max_teeth];
        float eye_size = 1.f;
        rw::vec3 eye_color{1.f, 0.5f, 0.f};
        float rudder_length = 0.5f;
        float cone_rad = 20.f;
        float arm_low_length = 25.f;
        float arm_high_length = 30.f;
        float arm_low_extra = 10.f;
    };

    // AnglerGraphics.lightPos, lastLightPos, lightVel: the lure at the end
    // of its arm (Angler.GlowPos)
    struct lure_state {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
    };

    // AnglerGraphics (purely cosmetic): the lights' flicker[2, 3], how long
    // the lights stay on (lightLinger), angerRisk, the propellers' spin
    struct graphics_state {
        float flicker[2][3] = {};
        int32_t light_linger = 0;
        float anger_risk = 0.f;
        float prop_spin = 0.f;
    };
}
