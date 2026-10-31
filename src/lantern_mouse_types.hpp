#pragma once

// Lantern mouse state that isn't a plain property of the creature
// (LanternMouse, MouseAI, MouseGraphics): what its data object
// (`lantern_mouse_data`, dcon_generated.txt) keeps that isn't a hot number:
// the IndividualVariations, the rope with its bends, MouseAI's dangle spot
// and counters, and the MouseGraphics state (purely cosmetic, but its light
// is what lights up the dark around a carried mouse)

#include <cstdint>

#include "dcon_generated_ids.hpp"
#include "math.hpp"
#include "pathing_types.hpp"
#include "player_types.hpp"

namespace lantern_mouse {
    // CreatureTemplate (LanternMouse): Floor and Corridor at 1, Climb at 2.5
    // (pathing::standard_ground's resistances); it swims on the surface only
    // (AirAndSurface)
    inline constexpr pathing::movement walk{{10, 10, 25, 0, 0, 0, 0}, 0};

    // MouseAI.Behavior (Hunt and ReturnPrey are never chosen by MouseAI)
    enum class behavior : uint8_t {
        idle,
        flee,
        escape_rain,
    };

    // LanternMouse.IndividualVariations (GenerateIVars, rolled from the
    // creature's ID.RandomSeed): dominance and the HSL colour's hue
    // (saturation 1, lightness 0.8 always)
    struct individual_variations {
        float dominance = 0.f;
        float hue = 0.f;
    };

    // LanternMouse.ropeAttatchedPos, lastRopeClearPos, ropeBends. The rope
    // lets go (DetatchRope) once it has more than 3 bends, so 4 is the most
    // it ever holds (the 5th slot keeps the add that triggers it)
    inline constexpr uint32_t max_bends = 5;
    struct rope_state {
        rw::vec2 attached{0.f, 0.f};
        rw::vec2 last_clear{0.f, 0.f};
        rw::vec2 bends[max_bends];
        // 0: no rope (ropeAttatchedPos null); else ropeBends.Count
        uint8_t bend_count = 0;
    };

    // MovementConnection as the path fields give them: a step from a tile to
    // a neighbour (Standard), a diagonal (OpenDiagonal) or a drop
    // (DropToFloor)
    struct movement_connection {
        rw::ivec2 start{0, 0};
        rw::ivec2 dest{0, 0};
        bool valid = false;
    };

    // MouseAI.Dangle: the tile the body hangs in and the tile the rope is
    // tied to, both in the mouse's room (a dangle of another room is
    // dropped when it changes rooms)
    struct dangle {
        rw::ivec2 body_pos{0, 0};
        rw::ivec2 attached_pos{0, 0};
        bool valid = false;
    };

    // MouseAI's state beyond the creature's behaviour and destination, and
    // LanternMouse's movement bookkeeping
    struct ai_state {
        // MouseAI.fear, pullUp, wantToSleep
        float fear = 0.f;
        float pull_up = 0.f;
        uint8_t want_to_sleep = 0;
        // MouseAI.dangle, walkWithMouse, dangleChecksCounter, idlePosCounter
        dangle dangle_spot;
        rw::ivec2 walk_with{0, 0};
        uint8_t has_walk_with = 0;
        int32_t dangle_checks = 0;
        int32_t idle_pos_counter = 0;
        // LanternMouse.specialMoveDestination, lastFollowedConnection,
        // struggleCountdownA/B
        rw::ivec2 special_move_destination{0, 0};
        movement_connection last_followed;
        int32_t struggle_a = 0;
        int32_t struggle_b = 0;
    };

    // MouseGraphics: the head and tail (BodyParts), the four limbs (Limb:
    // [chunk][side], `retract` beside them), the look direction, the profile
    // and back-to-camera turn, the eyes, the rope's drawn positions, the
    // light's modes (lightMode, flicker, charging...), and the creature it
    // looks at (CreatureLooker, simplified)
    struct graphics_state {
        player::body_part head;
        player::body_part tail;
        player::hand_state limbs[2][2];
        uint8_t retract[2][2] = {};
        rw::vec2 look_dir{0.f, 0.f};
        rw::vec2 last_look_dir{0.f, 0.f};
        float profile_fac = 0.f;
        float last_profile_fac = 0.f;
        float back_to_cam = 0.f;
        float last_back_to_cam = 0.f;
        int32_t ouch_eyes = 0;
        int32_t blink = 0;
        // ropePositions[j, 0] (now) and [j, 1] (last tick)
        rw::vec2 rope_positions[4][2];
        float run_cycle = 0.f;
        float last_run_cycle = 0.f;
        float light_mode = 0.f;
        float flickering_fac = 0.f;
        float flicker = 0.f;
        float flicker_duration = 1.f;
        float charging = 0.f;
        float super_charge = 0.f;
        float breath = 0.f;
        float unconsious_profile_fac = 0.f;
        dcon::creature_id look_at;
        uint8_t hands_out_while_hanging = 0;
        uint8_t hang_backwards = 0;
        uint8_t last_consious = 0;
        uint8_t made = 0;
    };
}
