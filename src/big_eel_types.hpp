#pragma once

// Leviathan state (BigEel.cs, BigEelAI, BigEelGraphics) that isn't a plain
// property of the creature: what its data object (`big_eel_data`,
// dcon_generated.txt) keeps, and its payload (creature::big_eel_payload,
// kept here beside the rest; creature_types.hpp puts it in the union).

#include <cmath>
#include <cstdint>

#include "dcon_generated_ids.hpp"
#include "math.hpp"
#include "pathing_types.hpp"
#include "player_types.hpp"

namespace big_eel {
    // BigEel: WaterOnly with Air at 1 (so every tile under the surface, at
    // the swimmers' resistance of 1), and AImap.IsTooCloseToTerrain keeps it
    // 4 tiles off the terrain: here the open_air preference for open water
    inline constexpr pathing::movement swim{{0, 0, 0, 0, 0, 0, 0}, 10, 5};

    // bodyChunks.Length
    inline constexpr uint32_t chunk_count = 20;

    // BigEelAI.Behavior
    enum class behavior : uint8_t {
        idle,
        hunt,
        escape_rain,
        return_prey,
    };

    // BigEel.IndividualVariations (GenerateIVars, rolled from the creature's
    // ID.RandomSeed): the two pattern colours as HSL (hue, saturation,
    // lightness)
    struct individual_variations {
        float pattern_displacement = 0.f;
        int32_t fins_seed = 0;
        rw::vec3 pattern_a{0.f, 0.f, 0.f};
        rw::vec3 pattern_b{0.f, 0.f, 0.f};
    };

    // BigEel.clampedObjects: the chunks caught in the jaws (a creature's or
    // an item's) and their distance along the jaws
    inline constexpr uint32_t max_clamped = 16;
    struct clamped_object {
        dcon::creature_id who;
        dcon::item_id item;
        uint8_t chunk = 0;
        float distance = 0.f;
    };
    struct clamped_list {
        clamped_object o[max_clamped];
        uint8_t count = 0;
    };

    // BigEelAI.focusCreature and what stands in for BigEelAbstractAI (its
    // timeInRoom and goToCoordinate): the way out over a side of the room
    // to the sea (`leave_side` -1 left, 1 right, 0 staying), the border tile
    // it heads for, and how long it has been on its way out
    struct ai_state {
        dcon::creature_id focus;
        int32_t roam_timer = 0;
        int32_t leave_counter = 0;
        rw::ivec2 exit_tile{-1, -1};
        int8_t leave_side = 0;
        // leaving because of the rain (EscapeRain), not to roam
        uint8_t escape = 0;
        // a neighbouring room has a sea exit too (worked out when it came
        // into the room, outside the rooms' parallel update)
        uint8_t can_roam = 0;
    };

    // HSLColor.Lerp (the hue by Mathf.LerpAngle) of (hue, saturation,
    // lightness) colours
    inline rw::vec3 hsl_lerp(rw::vec3 a, rw::vec3 b, float t) noexcept {
        t = rw::mathf::clamp01(t);
        float const from = a.x * 360.f, to = b.x * 360.f;
        // Mathf.Repeat(to - from, 360), the shorter way round
        float d = to - from;
        d = rw::mathf::clamp(d - std::floor(d / 360.f) * 360.f, 0.f, 360.f);
        if (d > 180.f)
            d -= 360.f;
        return rw::vec3{(from + d * t) / 360.f, rw::mathf::lerp(a.y, b.y, t), rw::mathf::lerp(a.z, b.z, t)};
    }

    // BigEelGraphics' constructor layout, rolled from iVars.finsSeed (purely
    // cosmetic): the fins (6 or 7, finsData[fin, 0] their length and
    // [fin, 1] a roll, their TailSegments' radii and connectionRad), the
    // body chunks' sprite rotation and flip (chunksData), the scales and
    // the 40 eyes (eyesData, eyeScales)
    inline constexpr uint32_t tail_segments = 60;
    inline constexpr uint32_t max_fins = 7;
    inline constexpr uint32_t max_fin_segments = 16;
    inline constexpr uint32_t eye_count = 40;
    inline constexpr uint32_t max_scales = 40;
    struct graphics_layout {
        uint8_t fins = 6;
        uint8_t fin_segments[max_fins] = {};
        float fin_length[max_fins] = {};
        float fin_roll[max_fins] = {};
        float fin_rad[max_fins][max_fin_segments] = {};
        float fin_connection_rad[max_fins] = {};
        float chunk_rotation[chunk_count] = {};
        float chunk_flip[chunk_count] = {};
        int32_t scale_start = 1;
        uint8_t scales = 20;
        float scale_size = 1.f;
        rw::vec2 eyes[eye_count];
        float eye_scales[eye_count][3] = {};
    };

    // BigEelGraphics' moving parts (purely cosmetic; the simulation never
    // reads them): the 60 tail TailSegments, the fins' (fin, side,
    // segment), the vibrating segment after a bite, the tail's swim cycle,
    // the jaw charge as last drawn
    struct graphics_state {
        player::body_part tail[tail_segments];
        player::body_part fins[max_fins][2][max_fin_segments];
        int32_t vibrate_segment = 0;
        float tail_swim = 0.f;
        float last_tail_swim = 0.f;
        float jaw_charge = 0.f;
        float last_jaw_charge = 0.f;
    };
}

namespace creature {
    struct big_eel_payload {
        // BigEel.swimDir, attackPos (has_attack_pos: not null)
        rw::vec2 swim_dir;
        rw::vec2 attack_pos;
        // swimMotion, jawCharge, jawChargeFatigue, beakGap, swimSpeed
        float swim_motion;
        float jaw_charge;
        float jaw_charge_fatigue;
        float beak_gap;
        float swim_speed;
        // BigEelAI.timeInThisRoom, hungerDelay
        float time_in_this_room;
        int32_t hunger_delay;
        // off at sea (out over a side of its room, or not in yet): ticks
        // before it swims back in
        int16_t hidden_timer;
        uint8_t has_attack_pos;
        // BigEel.snapFrame, albino; off at sea
        uint8_t snap_frame;
        uint8_t albino;
        uint8_t hidden;
        // big_eel_data
        uint16_t data;
    };
}
