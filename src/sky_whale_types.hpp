#pragma once

// Sky whale state that isn't a plain property of the creature (SkyWhale):
// what its data object (`sky_whale_data`, dcon_generated.txt) keeps that
// isn't a hot number: the CreatureParams rolled from its seed, the
// SkyWhaleAI's node walk and idle spot, the whiskers the slugcat climbs
// (SkyWhaleWhisker, an IClimbableVine), the gas jets out of its stab
// wounds, and the SkyWhaleGraphics state and layout (purely cosmetic)

#include <cstdint>

#include "dcon_generated_ids.hpp"
#include "math.hpp"
#include "pathing_types.hpp"
#include "player_types.hpp"

namespace sky_whale {
    // SkyWhale's template: Air (and everything below it) at 1. SkyWhalePather
    // keeps 4 tiles off the terrain (an IllegalConnection closer): here the
    // open air cost of CicadaPather stands in, and the AI's spots keep the
    // 4 tiles (sky_whale.cpp)
    inline constexpr pathing::movement fly{{10, 10, 10, 10, 10, 10, 0}, 0, 10};

    // SkyWhaleAI.Behavior, and (not vanilla) leaving the room over its side:
    // tryingToLeaveRoom heading for the offscreen den, and the
    // SkyWhaleAbstractAI's roaming between rooms that a realized room never
    // runs here (every room is realized)
    enum class behavior : uint8_t {
        idle,
        follow_path,
        escape_rain,
        get_unstuck,
        leave,
    };

    // SkyWhale.CreatureParams (GenerateIVars from the ID's seed; the rest
    // of the class' fields are its constants)
    struct creature_params {
        float size_multiplier = 4.f;
        float ruggedness = 0.f;
        float fin_sharpness = 0.f;
        // SkyWhaleAbstractAI.timeToTire (1700 + 4000 * (1 - energy)): how long
        // it roams a room with nodes before it moves on
        int32_t abstract_time_to_tire = 3700;
    };

    // the room's SkyWhalePathfinding placed objects the AI walks: at most
    // this many are looked at
    inline constexpr uint32_t max_nodes = 32;

    // SkyWhaleAI's state the payload has no room for
    struct ai_state {
        // the nodes of the room it has been to (usedPathfindingNodes, by
        // their order in the room's placed objects)
        uint32_t used_nodes = 0;
        // tempIdlePos (x < 0: none in this room yet)
        rw::ivec2 temp_idle_pos{-1, -1};
        // currentNodePosition
        rw::vec2 current_node_pos{0.f, 0.f};
        // the goal of the distance field it follows when the way is blocked
        rw::ivec2 field_goal{-1, -1};
        // StuckTracker (simplified): where its head was at the last check,
        // the ticks to the next, how stuck it is (the utility) and the spot
        // it heads for to get unstuck
        rw::vec2 stuck_pos{0.f, 0.f};
        int32_t stuck_check = 0;
        float stuck = 0.f;
        rw::ivec2 unstuck_goal{-1, -1};
        // the SkyWhaleAbstractAI's timeInRoom; the ticks spent leaving
        int32_t abstract_time_in_room = 0;
        int32_t leave_counter = 0;
    };

    // SkyWhale.SkyWhaleWhisker: up to 22 SimpleSegments hanging off the snout
    // (AddWhiskers rolls 14 or 17, +-30% with the ruggedness)
    inline constexpr uint32_t whisker_count = 6;
    inline constexpr uint32_t max_whisker_segments = 24;
    // the segments the slugcat can't climb (forbiddenSegments)
    inline constexpr uint32_t forbidden_segments = 3;
    struct whisker {
        rw::vec2 pos[max_whisker_segments];
        rw::vec2 last_pos[max_whisker_segments];
        rw::vec2 vel[max_whisker_segments];
        uint8_t segments = 0;
        float angle = 0.f;
        float side = 1.f;
        float start_radius = 1.5f;
        float end_radius = 0.1f;
        int32_t time_since_climbed = 100;
    };
    struct whisker_set {
        whisker w[whisker_count];
        // 0 for the AltVariant (no whiskers)
        uint8_t count = 0;
    };

    // SkyWhale.GasJet: smoke out of a stab wound, on its chunk (relative
    // position and direction in the chunk's rotation frame)
    inline constexpr uint32_t max_gas_jets = 3;
    struct gas_jet {
        rw::vec2 rel_pos{0.f, 0.f};
        rw::vec2 rel_dir{0.f, 0.f};
        int16_t life = 0;
        int16_t max_life = 0;
        uint8_t chunk = 0;
    };
    struct gas_jets {
        gas_jet j[max_gas_jets];
    };

    //
    // SkyWhaleGraphics
    //
    // the body: 6 chunks and 5 tail segments, smoothed into
    // (6 + 5) * 2 - 1 + 6 snout segments
    inline constexpr uint32_t chunk_count = 6;
    inline constexpr uint32_t tail_count = 5;
    inline constexpr uint32_t snout_segments = 6;
    inline constexpr uint32_t smooth_count = (chunk_count + tail_count) * 2 - 1 + snout_segments;
    inline constexpr uint32_t fin_rows = 5;

    // SkyWhaleGraphics.Marking.ColorMode
    enum class marking_color : uint8_t { scar, light_skin, dark_skin, black };
    // the marking elements (Marking.sprite)
    enum class marking_sprite : uint8_t {
        tiny_star,
        scar_a0, scar_a1, scar_a2, scar_a3, scar_a4,
        scar_b0, scar_b1, scar_b2, scar_b3, scar_b4,
        pebble5, pebble6, pebble7, pebble8,
        circle4,
    };
    struct marking {
        float segment = 0.f;
        float angle = 0.f;
        float sprite_rotation = 0.f;
        float width = 1.f;
        float height = 1.f;
        marking_sprite sprite = marking_sprite::tiny_star;
        marking_color color = marking_color::scar;
    };
    // freckles (20 to 59) and scars (up to 79), or the AltVariant's 80 to
    // 119 pebbles
    inline constexpr uint32_t max_markings = 140;
    struct armor_plate {
        float segment = 0.f;
        float angle = 0.f;
        float length = 0.f;
    };
    inline constexpr uint32_t max_armor = 8;

    // the graphics module's constructor: what it lays out once, from the
    // seed (purely cosmetic, never changes)
    struct graphics_layout {
        float seg_rad[smooth_count] = {};
        float seg_dist[smooth_count] = {};
        float tail_rad[tail_count] = {};
        float tail_seg_separation = 0.f;
        // Fin: segment, length, attachmentAngle ([row][side])
        uint8_t fin_segment[fin_rows][2] = {};
        float fin_length[fin_rows][2] = {};
        float fin_attachment_angle[fin_rows][2] = {};
        marking markings[max_markings];
        uint8_t marking_count = 0;
        armor_plate armor[max_armor];
        uint8_t armor_count = 0;
        float eye_on_body_angle = 0.f;
        float eye_on_body_segment = 0.f;
        int32_t swim_timer_start = 0;
    };

    struct body_segment {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
    };
    struct fin_state {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        float angle = 0.f;
        float last_angle = 0.f;
    };
    // the graphics module's moving parts
    struct graphics_state {
        body_segment smooth[smooth_count];
        player::body_part tail[tail_count];
        fin_state fins[fin_rows][2];
        int32_t blink = 0;
        int32_t swim_timer = 0;
        // CreatureLooker (simplified): the creature it looks at, re-picked
        // every so often
        dcon::creature_id look;
        dcon::creature_id last_look;
        int32_t look_counter = 0;
        rw::vec2 look_pos{0.f, 0.f};
        uint8_t has_look = 0;
    };
}
