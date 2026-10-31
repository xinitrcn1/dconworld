#pragma once

// miros bird state that isn't a plain property of the creature (MirosBird): what
// its payload (creature_types.hpp) and its data object (`miros_data`,
// dcon_generated.txt) keep that isn't a number: its two legs (BirdLeg), its
// neck (a Tentacle), the past positions, the graphics module

#include <cstdint>
#include <optional>

#include "dcon_generated_ids.hpp"
#include "math.hpp"
#include "pathing_types.hpp"

namespace miros {
    // MirosBirdAI.Behavior (GetUnstuck isn't ported: the body's stuck shake
    // is what gets it loose)
    enum class behavior : uint8_t {
        idle,
        flee,
        hunt,
        escape_rain,
        return_prey,
    };

    // How it paths. Vanilla's template walks Air (every open tile) and
    // MirosBirdAI.TravelPreference keeps its path five tiles over the
    // floor, where its body rides on its legs; here the path runs over the
    // floor tiles themselves (dropping off ledges), from the floor tile under
    // the body, and the body is steered at the tiles `body_lift` above them
    inline constexpr pathing::movement walker{{10, 0, 0, 0, 0, 0, 0}, 0};
    inline constexpr float body_lift = 80.f;

    // MirosBird.BirdLeg.Joint
    struct joint {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
    };

    // MirosBird.BirdLeg. The two legs are kept in the data object (a
    // `leg_pair` property) rather than as creature_parts: there are always
    // exactly two, made and dropped with the bird, they have no stun or
    // health of their own, and each one's update reads and resets the
    // other's (the other leg's groundContact, leg 1 lifting leg 0's foot),
    // so one property updated in one pass is the simplest layout
    struct leg {
        // joints[0..2]: Hip, Knee, Foot
        joint joints[3];
        uint8_t ground_contact = 0;
        uint8_t mode_conservative_bias = 0;
        float spring_power = 0.f;
        float last_spring_pow = 0.f;
        float forward_power = 0.f;
        rw::vec2 spring_dir{0.f, 0.f};
        std::optional<rw::vec2> foot_secure_pos;
        std::optional<rw::vec2> move_to_pos;
        std::optional<rw::vec2> last_foot_secure_pos;
        rw::vec2 move_from_pos{0.f, 0.f};
        float move_progress = 0.f;
        int32_t foot_secure_frames = 0;
        // the sparks' glow (lightUp, lightUpPos1/2)
        int32_t light_up = 0;
        rw::vec2 light_up_pos1{0.f, 0.f};
        rw::vec2 light_up_pos2{0.f, 0.f};
        float flip = 0.f;
        float last_run_mode = 0.f;
        float run_mode = 0.f;
    };
    struct leg_pair {
        leg l[2];
    };

    // A short list of room tiles (the neck's Tentacle.segments and
    // grabPath), kept in place in the data object; the neck works on
    // std::vector copies of them while it updates (longer lists are cut)
    inline constexpr uint32_t max_neck_tiles = 64;
    struct tile_list {
        rw::ivec2 v[max_neck_tiles];
        uint8_t n = 0;
    };

    // Tentacle.TentacleChunk (no Rope: the neck's tProps.rope is off)
    struct neck_chunk {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        float rad = 3.f;
        float t_pos = 0.f;
        float phase = -1.f;
        rw::vec2 phase_from{0.f, 0.f};
        int32_t phases_to_same_location = 0;
        rw::ivec2 after_phase_stuck_pos{0, 0};
        int32_t phase_attempts = 0;
        float stretched_fac = 1.f;
        rw::ivec2 contact_point{0, 0};
        rw::ivec2 last_contact_point{0, 0};
    };

    // MirosBird.neck: a Tentacle(bodyChunks[1], 60) of three chunks
    inline constexpr uint32_t neck_chunks = 3;
    struct neck_state {
        float update_counter = 0.f;
        int32_t pull_counter = 0;
        int32_t pulls_this_tick = 0;
        std::optional<rw::vec2> float_grab_dest;
        int32_t backtrack_from = -1;
        float retract_fac = 0.f;
        uint8_t limp = 0;
        neck_chunk c[neck_chunks];
        tile_list segments;
        tile_list grab_path;
    };

    // MirosBird.pastPositions: the last 40 tiles, the newest first
    struct past_positions {
        static constexpr int32_t kept = 40;
        rw::ivec2 tile[kept];
        int32_t count = 0;
    };

    // MirosBirdGraphics.Plumage.Feather
    struct feather {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        float length = 0.f;
        float lazy = 0.f;
    };
    // NeckPlumage has at most 30 feathers, TailPlumage 30
    inline constexpr uint32_t max_feathers = 30;
    // BeakGraphic.teeth: Random.Range(10, 20) of them
    inline constexpr uint32_t max_teeth = 20;
    // EyeTrail.savPoss
    inline constexpr uint32_t eye_trail_positions = 10;

    // the graphics module's state (purely cosmetic): the layout its
    // constructor rolls from the creature's seed, and what its Update moves
    struct graphics_state {
        int32_t plumage_graphic = 0;
        float plumage_graph_length = 17.f;
        float neck_fatness = 1.f;
        float beak_fatness = 0.5f;
        float plumage_length = 1.f;
        float plumage_width = 1.f;
        float plumage_gravity = 1.f;
        float plumage_friction = 0.8f;
        float plumage_direction = 5.f;
        float plumage_density = 0.5f;
        float eye_size = 1.f;
        float tigh_size = 1.f;
        rw::vec3 eye_col{1.f, 0.6f, 0.f};
        // BeakGraphic.teeth[i, 0..2] of both halves
        uint8_t teeth_count[2] = {0, 0};
        float teeth[2][max_teeth][3] = {};
        // the plumage it has (each is left out one time in 18)
        uint8_t has_neck_plumage = 0;
        uint8_t has_tail_plumage = 0;
        // NeckPlumage.dirType
        uint8_t neck_dir_type = 0;
        uint8_t neck_count = 0;
        uint8_t tail_count = 0;
        feather neck[max_feathers];
        feather tail[max_feathers];
        // TailPlumage.positions, flips
        rw::vec2 tail_positions[max_feathers];
        float tail_flips[max_feathers] = {};
        float head_flip = 0.f;
        float last_head_flip = 0.f;
        // EyeTrail.positionsList, the newest first (its colorsList is the
        // eye colour throughout)
        rw::vec2 eye_trail[eye_trail_positions];
        uint8_t eye_trail_count = 0;
    };
}
