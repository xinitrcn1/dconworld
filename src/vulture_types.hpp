#pragma once

// Vulture state that isn't a plain property of the creature (Vulture.cs with
// its variants KingVulture and MirosVulture): what its payload
// (creature_types.hpp) and its data object (`vulture_data`,
// dcon_generated.txt) keep that isn't a number: the neck (a Tentacle), the
// thrusters, the AI, the king's tusks (KingTusks) and the graphics module.
// Its wings (VultureTentacle) are creature_parts with a `vulture_wing` data
// object each.

#include <cstdint>
#include <optional>

#include "dcon_generated_ids.hpp"
#include "math.hpp"
#include "pathing_types.hpp"
#include "player_types.hpp"

namespace vulture {
    // the template: Vulture, KingVulture, MirosVulture (DLCShared)
    enum class variant : uint8_t { vulture, king, miros };
    inline constexpr uint32_t variant_count = 3;
    inline char const* to_string(variant v) noexcept {
        switch (v) {
        case variant::vulture: return "vulture";
        case variant::king: return "king vulture";
        case variant::miros: return "miros vulture";
        }
        return "vulture";
    }

    // Vulture's template: Air at 1 (so every open tile). VultureAI.
    // TravelPreference adds (6 - terrainProximity) * 5 resistance a step
    // (8 for the king, 10 and +100 next to walls for the miros): here
    // open_air, a tenth a tile closer than 10 to the terrain
    inline constexpr pathing::movement flyer{{10, 10, 10, 10, 10, 10, 0}, 0, 10};

    // VultureAI.Behavior (GoToMask isn't ported: no mask is dropped; `sky`
    // isn't vanilla: flying off over the top of the room with its prey,
    // where vanilla's leave through the sky exits)
    enum class behavior : uint8_t {
        idle,
        hunt,
        escape_rain,
        return_prey,
        get_unstuck,
        disencouraged,
        sky,
    };

    // the body chunks: 0 the main chunk (the front), 1 the back, 2 and 3 the
    // wings' bases, 4 the head
    inline constexpr uint32_t chunk_count = 5;
    inline constexpr uint32_t head = 4;
    // the wings: two, four for the miros vulture
    inline constexpr uint32_t max_wings = 4;
    // VultureTentacle: 8 TentacleChunks, 10 for the king
    inline constexpr uint32_t max_wing_chunks = 10;
    // the neck: four TentacleChunks
    inline constexpr uint32_t neck_chunks = 4;

    // Tentacle.TentacleChunk (no Rope: the vulture's tProps.rope is off)
    struct tentacle_chunk {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        float rad = 5.f;
        float t_pos = 0.f;
        float phase = -1.f;
        rw::vec2 phase_from{0.f, 0.f};
        int32_t phases_to_same_location = 0;
        rw::ivec2 after_phase_stuck_pos{0, 0};
        int32_t phase_attempts = 0;
        float stretched_fac = 1.f;
        rw::ivec2 contact_point{0, 0};
        rw::ivec2 last_contact_point{0, 0};
        uint8_t collide_with_terrain = 1;
    };

    // Tentacle's scalars (the tile segments and the grab path are lists
    // kept elsewhere: the neck's in its neck_state, a wing's in its
    // vulture_wing object's vector pools)
    struct tentacle {
        float ideal_length = 100.f;
        float update_counter = 0.f;
        int32_t pull_counter = 0;
        int32_t pulls_this_tick = 0;
        std::optional<rw::vec2> float_grab_dest;
        int32_t backtrack_from = -1;
        float retract_fac = 0.f;
        uint8_t limp = 0;
        // its connectedChunk (the neck's 0, a wing's 2 or 3)
        uint8_t connected_chunk = 0;
    };

    // A short list of room tiles (the neck's Tentacle.segments and
    // grabPath), kept in place in the data object; the neck works on
    // std::vector copies of them while the vulture updates (longer lists
    // are cut)
    inline constexpr uint32_t max_neck_tiles = 64;
    struct tile_list {
        rw::ivec2 v[max_neck_tiles];
        uint8_t n = 0;
    };

    // Vulture.neck: a Tentacle(bodyChunks[0], 100) (120 for the king) of four chunks
    struct neck_state {
        tentacle t;
        tentacle_chunk c[neck_chunks];
        tile_list segments;
        tile_list grab_path;
    };

    // VultureTentacle.Mode
    enum class wing_mode : uint8_t { climb, fly };

    // VultureTentacle: the Tentacle and its own state; tentacleNumber is
    // the creature_part's index, its stun the part's stun and its
    // VultureState.wingHealth the part's health
    struct wing {
        tentacle t;
        wing_mode mode = wing_mode::climb;
        rw::vec2 desired_grab_pos{0.f, 0.f};
        uint8_t attached_at_tip = 0;
        uint8_t play_grab_sound = 0;
        uint8_t chunk_count = 8;
        int32_t segments_gripping_terrain = 0;
        int32_t frames_without_reaching = 0;
        int32_t other_tentacle_is_flying = 0;
        int32_t grab_delay = 0;
        int32_t frames_of_hitting_terrain = 0;
        // flyingMode (fm, clamped 0..1)
        float flying_mode = 0.f;
        tentacle_chunk c[max_wing_chunks];
    };

    // Vulture.VultureThruster's state (the rest follows from its index)
    struct thruster {
        int32_t thrust = 0;
        uint8_t last_active = 0;
    };

    // Vulture's fields the payload has no room for
    struct body_state {
        uint8_t hanging_in_tentacle = 0;
        int32_t cant_find_new_grip_counter = 0;
        int32_t release_gripping_tentacle = 0;
        rw::ivec2 hover_pos{0, 0};
        int32_t dont_switch_modes_counter = 0;
        int32_t time_since_last_takeoff = 0;
        rw::vec2 landing_brake_pos{0.f, 0.f};
        int32_t stuck = 0;
        int32_t stuck_shake_duration = 0;
        // lastConnection's DestTile (x < -100: none yet)
        rw::ivec2 last_connection{-1000, -1000};
        // snapAt (a creature's chunk) or snapAtPos
        dcon::creature_id snap_at;
        uint8_t snap_at_chunk = 0;
        rw::vec2 snap_at_pos{0.f, 0.f};
        // the miros vulture's jaw (jawVel, keepJawOpenPos, jawSlamPause,
        // jawKeepOpenPause)
        float jaw_vel = 0.f;
        float keep_jaw_open_pos = 0.f;
        int32_t jaw_slam_pause = 0;
        int32_t jaw_keep_open_pause = 0;
        thruster thrusters[4];
        // GoThroughFloors (the vulture's own, safari control aside: always)
        uint8_t go_through_floors = 1;
        // the colour waves the body asked of its graphics module
        // (MakeColorWave on take off and falling): how many, the last delay
        uint8_t color_waves = 0;
        int32_t color_wave_delay = 0;
    };

    // VultureAI (simplified) with its DisencouragedTracker, and the
    // creature looker's look creature (its focus)
    struct ai_state {
        int32_t time_in_room = 0;
        float disencouraged = 0.f;
        uint8_t prey_in_tusk_charge_range = 0;
        // focusCreature
        dcon::creature_id focus;
        // kingTuskShootPos
        rw::ivec2 king_tusk_shoot_pos{0, 0};
        // where it roams to while idle (not vanilla: its abstract AI moves
        // it between rooms through the sky exits)
        rw::ivec2 idle_pos{-1, -1};
        int32_t idle_counter = 0;
        // the sky (not vanilla): the column it flies up through, and how
        // long it stays up there out of sight
        int32_t sky_x = -1;
        int32_t sky_counter = 0;
    };

    // KingTusks.Tusk.Mode
    enum class tusk_mode : uint8_t { attached, charging, shooting_out, stuck_in_creature, stuck_in_wall, dangling, retracting };

    // a point mass of the tusk (chunkPoints[i, 0..2]) or of its wire
    struct point {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
    };
    inline constexpr uint32_t wire_points = 20;
    // KingTusks.Tusk
    struct tusk {
        tusk_mode mode = tusk_mode::attached;
        int32_t mode_counter = 0;
        float attached = 1.f;
        point chunk_points[2];
        rw::vec2 shoot_dir{0.f, 1.f};
        float stuck = 0.f;
        float laser_alpha = 0.f;
        float last_laser_alpha = 0.f;
        float laser_power = 0.f;
        float curr_wire_length = 0.f;
        float wire_extra_slack = 0.f;
        float wire_loose = 0.f;
        float last_wire_loose = 0.f;
        float elasticity = 0.9f;
        // impaleChunk: a creature's chunk
        dcon::creature_id impale;
        uint8_t impale_chunk = 0;
        // stuckInWallPos
        uint8_t has_wall_pos = 0;
        rw::vec2 wall_pos{0.f, 0.f};
        rw::vec2 z_rot{0.f, 0.f};
        rw::vec2 last_z_rot{0.f, 0.f};
        // the rope exists (a straight line here, see DIVERGENCES)
        uint8_t has_rope = 0;
        // the wire's look (moved where a camera looks)
        point wire[wire_points];
    };
    inline constexpr uint32_t target_trail_length = 15;
    // KingTusks
    struct king_tusks {
        tusk t[2];
        float head_rot = 0.f;
        float last_head_rot = 0.f;
        float eyes_out = 0.f;
        float last_eyes_out = 0.f;
        float eyes_out_cycle = 0.f;
        float eyes_home_in = 0.f;
        float last_eyes_home = 0.f;
        rw::vec2 prey_pos{0.f, 0.f};
        rw::vec2 last_prey_pos{0.f, 0.f};
        rw::vec2 prey_vel_estimate{0.f, 0.f};
        int32_t no_shoot_delay = 0;
        float pattern_displace = 0.f;
        rw::vec2 target_trail[target_trail_length];
        // targetRep
        dcon::creature_id target;
    };

    //
    // VultureGraphics (purely cosmetic)
    //
    // VultureFeather.ContractMode
    enum class contract_mode : uint8_t { even, jerky, jammed };
    // VultureFeather (a BodyPart)
    struct feather {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        float wing_position = 0.f;
        float extended_fac = 0.f;
        float width = 3.f;
        float contracted_length = 10.f;
        float extended_length = 10.f;
        float contract_speed = 0.f;
        float lose = 0.f;
        float broken_color = 0.f;
        float forced_alpha = 0.f;
        float lightness_bonus = 0.f;
        float saturation_bonus = 0.f;
        int32_t terrain_contact_timer = 0;
        contract_mode mode = contract_mode::even;
    };
    // feathersPerWing: 13..19 (15..24 for the king, 6..7 for the miros)
    inline constexpr uint32_t max_feathers_per_wing = 24;
    inline constexpr uint32_t max_feathers = 48;
    // VultureAppendage (a BodyPart): 2..11 a side (up to 14 for the king)
    inline constexpr uint32_t max_appendages = 14;
    struct appendage {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        float rad = 3.f;
        float stretched_rad = 3.f;
        float con_rad = 3.f;
    };
    // VultureGraphics.WingColorWave
    inline constexpr uint32_t max_color_waves = 8;
    struct color_wave {
        int32_t delay = 0;
        float speed = 0.f;
        float lightness = 0.f;
        float saturation = 0.f;
        float force_alpha = 0.f;
        float position = 0.f;
        float last_position = 0.f;
    };
    // the king's neckTubes: two of 15 points
    inline constexpr uint32_t neck_tube_points = 15;
    // the miros vulture's EyeTrail.savPoss, BeakGraphic.teeth
    inline constexpr uint32_t eye_trail_positions = 15;
    inline constexpr uint32_t max_teeth = 20;

    // the graphics module's state: the layout its constructor rolls from
    // the creature's seed, and what its Update moves
    struct graphics_state {
        // ColorA, ColorB: HSLColor (hue, saturation, lightness)
        rw::vec3 color_a{0.f, 0.f, 0.f};
        rw::vec3 color_b{0.f, 0.f, 0.f};
        uint8_t albino = 0;
        uint8_t feathers_per_wing = 0;
        feather feathers[max_feathers];
        uint8_t appendage_count[2] = {0, 0};
        appendage appendages[2][max_appendages];
        // the (not miros) tusks: GenericBodyParts and tuskRotations
        player::body_part tusks[2];
        float tusk_rotations[2] = {0.f, 0.f};
        int32_t head_graphic = 0;
        int32_t change_head_graphic_counter = 0;
        // shells[i, 0..2]: pos, vel, lastPos
        rw::vec2 shells[4][3];
        int32_t shell_mode_change_counter[2] = {0, 0};
        float shell_modes[2][2] = {};
        color_wave waves[max_color_waves];
        uint8_t wave_count = 0;
        // the king's neckTubes[s, i, 0..2]: pos, last pos, vel
        rw::vec2 neck_tubes[2][neck_tube_points][3];
        // the miros vulture: eyes, beak, laser
        float eye_size = 1.f;
        rw::vec3 eye_col{1.f, 0.6f, 0.f};
        float beak_fatness = 0.5f;
        uint8_t teeth_count[2] = {0, 0};
        float teeth[2][max_teeth][3] = {};
        float head_flip = 0.f;
        float last_head_flip = 0.f;
        float laser_active = 0.f;
        float last_laser_active = 0.f;
        float flash = 0.f;
        float last_flash = 0.f;
        rw::vec2 eye_trail[eye_trail_positions];
        uint8_t eye_trail_count = 0;
        float eye_trail_ticker = 0.f;
        // shadowMode: high over the room, drawn as nothing
        uint8_t shadow_mode = 0;
        // the body's colour wave requests it has made
        uint8_t color_waves_seen = 0;
    };
}
