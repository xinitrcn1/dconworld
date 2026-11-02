#pragma once

// Batfly state (Fly.cs, FlyAI.cs, FlyGraphics.cs): the payload's part
// (`body_state`, the creature_payload's `batfly`: what Fly keeps, flight
// and chain hanging) and what its data object (`batfly_data`,
// dcon_generated.txt) keeps: the AI (FlyAI: goals, threats, its flock)
// and the graphics module. See batfly.hpp.

#include <cstdint>

#include "dcon_generated_ids.hpp"
#include "math.hpp"
#include "pathing_types.hpp"

namespace batfly {
    // FlyAI.Behavior (LeaveRoom is never set by vanilla; Lure needs the
    // FlyLure item, not ported)
    enum class behavior : uint8_t {
        idle,
        swarm,
        drop,
        flee,
        flee_from_rain,
        leave_room,
        burrow,
        chain,
        lure,
    };
    // Fly.MovementMode
    enum class mov_mode : uint8_t {
        bat_flight,
        swarm_flight,
        passive,
        burrow,
        panic,
        hang,
    };
    // FlyAI.DropStatus
    enum class drop_status : uint8_t {
        havent_dropped,
        dropping,
        has_dropped,
    };

    // the Fly template: Air at 1 (so every accessibility at 1), no swimming
    // (AirOnly)
    inline constexpr pathing::movement movement{{10, 10, 10, 10, 10, 10, 0}, 0};

    // The payload (creature_types.hpp's `batfly_payload`): Fly's own fields
    struct body_state {
        // Fly.dir, burrowOrHangSpot (valid with has_spot)
        rw::vec2 dir;
        rw::vec2 spot;
        // flap, flapSpeed, flapDepth, lastFlapDepth
        float flap;
        float flap_speed;
        float flap_depth;
        float last_flap_depth;
        // drown, wallAvoidance
        float drown;
        float wall_avoidance;
        mov_mode mode;
        uint8_t has_spot;
        // chainBehaviorVariation, bites, eaten (the countdown to Destroy
        // once eaten)
        uint8_t chain_variation;
        uint8_t bites;
        uint8_t eaten;
        // FliesRoomAI.inHive: burrowed in its room's hive grass (`away` 2,
        // its den none: the room's batfly pass lets it out, batfly.cpp)
        uint8_t in_hive;
        // batfly_data
        uint16_t data;
    };

    // FlyAI.TrackedThreat
    inline constexpr uint32_t max_threats = 4;
    struct threat {
        dcon::creature_id crit;
        rw::vec2 last_seen_pos;
        int16_t ticks_since_seen;
        int16_t ticks_to_forget;
        uint8_t visual_contact;
        uint8_t currently_scared_of;
        uint8_t delete_me;
    };

    // FlyAI's fields. FlockBehavior is shared by reference in vanilla (the
    // leader advances its sinCycle and life): here every fly keeps its
    // flock's seed (the rolls of its constructor) and the tick it began, so
    // copies of a flock agree without sharing anything (batfly.cpp)
    struct ai_state {
        rw::vec2 local_goal;
        rw::vec2 lowest_other_pos;
        rw::vec2 stuck_pos;
        uint32_t flock_seed;
        uint32_t flock_birth;
        float afraid;
        int32_t panic_flee;
        int16_t stuck_counter;
        int16_t get_back_to_ground;
        int16_t no_swarm_counter;
        int16_t lured_counter;
        // followingDijkstraMap, leaveRoomDijkstra: an index into the room's
        // nodes (batfly.cpp: its exits by exitAndDenIndex, then its hives),
        // -1 none
        int8_t following;
        int8_t leave_room;
        drop_status drop;
        uint8_t flee_from_rain;
        uint8_t threat_count;
        threat threats[max_threats];
    };

    // FlyGraphics (purely cosmetic): lowerBody (pos, lastPos, vel),
    // wings[2, 2], horizontalSteeringCompensation (and last),
    // insectFlightWingSync, deathWingPositions
    struct graphics_state {
        rw::vec2 lower_pos;
        rw::vec2 lower_last_pos;
        rw::vec2 lower_vel;
        float wings[2][2];
        float steering;
        float last_steering;
        float wing_sync;
        float death_wing[2];
    };
}
