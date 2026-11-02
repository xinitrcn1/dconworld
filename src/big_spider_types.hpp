#pragma once

// Big spider state that isn't a plain property of the creature (BigSpider.cs,
// BigSpiderAI, BigSpiderGraphics; SpitterSpider and Downpour's MotherSpider
// are the same classes with their own templates): what its data object
// (`big_spider_data`, dcon_generated.txt) keeps, and the state of the dart
// maggots a spitter shoots (DartMaggot: the `dart_maggot_data` object of
// those items, dart_maggot.hpp)

#include <cstdint>
#include <optional>

#include "dcon_generated_ids.hpp"
#include "math.hpp"
#include "pathing_types.hpp"
#include "player_types.hpp"

namespace big_spider {
    // the template: BigSpider, SpitterSpider, or Downpour's MotherSpider
    enum class variant : uint8_t {
        big,
        spitter,
        mother,
    };
    inline constexpr uint32_t variant_count = 3;

    // BigSpiderAI.Behavior
    enum class behavior : uint8_t {
        idle,
        flee,
        hunt,
        escape_rain,
        return_prey,
        get_unstuck,
        revive_buddy,
    };

    // CreatureTemplate's TileTypeResistances (all three: SpitterSpider and
    // MotherSpider inherit BigSpider's): floors and corridors at 1, poles at
    // 1.5, walls and ceilings at 3. AirAndSurface: they swim on the surface
    inline constexpr pathing::movement spider_movement{{10, 10, 15, 30, 30, 0, 0}, 0};

    // MovementConnection, as far as BigSpider.Run looks at one (`valid`
    // false is default(MovementConnection))
    struct movement_connection {
        rw::ivec2 start{0, 0};
        rw::ivec2 dest{0, 0};
        bool valid = false;
    };

    // grabChunks[side, leg]: a creature's chunk a leg holds on to
    struct grab_chunk {
        dcon::creature_id who;
        uint8_t chunk = 0;
    };

    // BigSpider's movement and fighting state the payload has no room for
    struct move_state {
        // specialMoveDestination, lastFollowedConnection
        rw::ivec2 special_move_destination{0, 0};
        movement_connection last_followed;
        // jumpAtPos
        rw::vec2 jump_at_pos{0.f, 0.f};
        // spitPos (has_spit_pos: not null), spitDir
        rw::vec2 spit_pos{0.f, 0.f};
        rw::vec2 spit_dir{0.f, 1.f};
        bool has_spit_pos = false;
        // carryObjectMass, bounceSoundVol
        float carry_object_mass = 0.f;
        float bounce_sound_vol = 1.f;
        // revivingBuddy
        dcon::creature_id reviving_buddy;
        grab_chunk grab[2][4];
        // spewBabies (the mother's young are out)
        bool spew_babies = false;
        // what the graphics module takes from Jump and the jump at prey
        // (vanilla's legs set straight from BigSpider.Jump / Act: here the
        // graphics read it): the jumps so far and the last one's direction,
        // and the prey it leaps at this tick
        uint32_t jump_count = 0;
        rw::vec2 jump_dir{0.f, 0.f};
        rw::vec2 jump_target{0.f, 0.f};
        bool jump_target_valid = false;
    };

    // BigSpiderAI.SpiderTrackState of a tracked creature
    struct track_state {
        dcon::creature_id who;
        int32_t accustomed = 0;
        int32_t tagged = 0;
        float total_mass = 1.f;
        bool consious = false;
        bool close = false;
        bool armed = false;
        bool alive = true;
    };
    inline constexpr uint32_t max_track = 10;
    inline constexpr uint32_t max_other_spiders = 8;
    inline constexpr uint32_t max_prev_idle = 9;

    // BigSpiderAI's state the payload has no room for
    struct ai_state {
        // idlePos, tempIdlePos, idlePosCounter, previdlePositions,
        // idleTowardsPosition
        rw::ivec2 idle_pos{0, 0};
        rw::ivec2 temp_idle_pos{0, 0};
        int32_t idle_pos_counter = 0;
        rw::ivec2 prev_idle[max_prev_idle] = {};
        uint8_t prev_idle_count = 0;
        std::optional<rw::ivec2> idle_towards;
        // stayAway, noiseRectionDelay
        bool stay_away = false;
        int32_t noise_reaction_delay = 0;
        // UtilityComparer: the threat tracker's smoothed utility (Lerp 0.5
        // up, Tick 0.005 down) and the prey tracker's (Lerp 0.2 up, Tick
        // 0.01 down)
        float threat_smoothed = 0.f;
        float prey_smoothed = 0.f;
        // reviveBuddy, otherSpiders
        dcon::creature_id revive_buddy;
        dcon::creature_id other_spiders[max_other_spiders];
        uint8_t other_spider_count = 0;
        // the RelationshipTracker's SpiderTrackStates
        track_state track[max_track];
    };

    // BigSpiderAI.SpiderSpitModule (the spitter's)
    inline constexpr uint32_t target_trail_max = 7;
    struct spit_state {
        // spitAtCrit, taggedCreature
        dcon::creature_id spit_at;
        dcon::creature_id tagged;
        // goToSpitPos, goToSpitPosAntiFlicker, spitPos, targetPoint, aimDir,
        // noSitDelay, targetChunk
        bool go_to_spit_pos = false;
        int32_t anti_flicker = 0;
        rw::ivec2 spit_pos{0, 0};
        rw::vec2 target_point{0.f, 0.f};
        rw::vec2 aim_dir{0.f, 1.f};
        int32_t no_sit_delay = 0;
        int32_t target_chunk = 0;
        // targetTrail (newest first)
        rw::vec2 target_trail[target_trail_max] = {};
        uint8_t target_trail_count = 0;
        // ammo, ammoRegen, fastAmmoRegen, randomCritSpitDelay
        int32_t ammo = 4;
        float ammo_regen = 0.f;
        bool fast_ammo_regen = false;
        int32_t random_crit_spit_delay = 0;
    };

    // BigSpiderGraphics (purely cosmetic). The scales' segments
    // (scales[i][j, 0..2]: position, last position, velocity) are one flat
    // pool in sprite order; each scale (a strand) keeps its first segment
    // and count
    inline constexpr uint32_t max_scales = 37;
    inline constexpr uint32_t max_scale_segments = 170;
    struct scale_strand {
        uint8_t first = 0;
        uint8_t count = 0;
        // scaleStuckPositions, scaleSpecs[i, 0], scaleSpecs[i, 1]
        rw::vec2 stuck_pos{0.f, 0.f};
        rw::vec2 spec0{0.f, 0.f};
        rw::vec2 spec1{0.f, 0.f};
    };
    struct graphics_state {
        player::body_part tail_end;
        player::body_part mandibles[2];
        player::hand_state legs[2][4];
        // legFlips[side, leg, 0..1], legsTravelDirs
        float leg_flips[2][4][2] = {};
        rw::vec2 legs_travel_dirs[2][4] = {};
        int32_t legs_dangle_counter = 0;
        float leg_length = 0.f;
        float flip = 0.f;
        float last_flip = 0.f;
        rw::vec2 spikes_dir{0.f, 0.f};
        rw::vec2 breath_dir{0.f, 0.f};
        float breath_counter = 0.f;
        float last_breath_counter = 0.f;
        float mandibles_charge = 0.f;
        float last_mandibles_charge = 0.f;
        float legs_thickness = 1.f;
        float body_thickness = 1.f;
        rw::ivec2 dead_leg{-1, -1};
        // the jumps the legs reacted to (move_state.jump_count)
        uint32_t seen_jumps = 0;
        uint8_t scale_count = 0;
        uint8_t total_segments = 0;
        scale_strand scales[max_scales];
        rw::vec2 seg_pos[max_scale_segments] = {};
        rw::vec2 seg_last[max_scale_segments] = {};
        rw::vec2 seg_vel[max_scale_segments] = {};
    };

    // DartMaggot (an item of type dart_maggot) and its Umbilical (the
    // thread back to the spitter, cosmetic)
    enum class maggot_mode : uint8_t { free, shot, stuck_in_chunk, stuck_in_terrain };
    inline constexpr uint32_t maggot_mesh_segs = 9;
    inline constexpr uint32_t max_umbilical = 20;
    struct maggot_state {
        maggot_mode mode = maggot_mode::free;
        // body[i, 0..2]
        rw::vec2 body_pos[2] = {};
        rw::vec2 body_last[2] = {};
        rw::vec2 body_vel[2] = {};
        rw::vec2 wiggle_dir{0.f, 0.f};
        rw::vec2 wiggle_get_to_dir{0.f, 0.f};
        float squirm[maggot_mesh_segs][2] = {};
        float squirm_offset = 0.f;
        float squirm_add = 0.f;
        float squirm_add_get_to = 0.f;
        float squirm_width = 0.f;
        float squirm_width_get_to = 0.f;
        float squirm_amp = 0.f;
        float squirm_amp_get_to = 0.f;
        rw::vec2 stuck_pos{0.f, 0.f};
        rw::vec2 stuck_dir{0.f, 0.f};
        rw::vec2 needle_dir{0.f, 1.f};
        rw::vec2 last_needle_dir{0.f, 1.f};
        // shotBy, stuckInChunk
        dcon::creature_id shot_by;
        dcon::creature_id stuck_in;
        uint8_t stuck_chunk = 0;
        float age = 0.f;
        float life_time = 1500.f;
        int32_t sleep_counter = 0;
        float bloat = 0.f;
        float squeeze = -0.1f;
        float size_fac = 0.8f;
        float new_and_pink = 0.f;
        // the Umbilical: points[i, 0..3] (position, last position,
        // velocity, (life, lifetime)); count 0 none
        uint8_t umbilical_count = 0;
        rw::vec2 u_pos[max_umbilical] = {};
        rw::vec2 u_last[max_umbilical] = {};
        rw::vec2 u_vel[max_umbilical] = {};
        rw::vec2 u_life[max_umbilical] = {};
    };
    // DartMaggot.UntilSleepDelay
    inline constexpr int32_t until_sleep_delay = 340;
}
