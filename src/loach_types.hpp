#pragma once

// Loach state that isn't a plain property of the creature (Watcher's
// Loach): what its payload (creature_types.hpp) and its data object
// (`loach_data`, dcon_generated.txt) keep that isn't a number. Its six legs
// (LoachLeg) and five mouth tentacles (LoachTentacle), both Tentacles, are
// creature_parts with a `loach_limb` data object each.

#include <cstdint>
#include <optional>

#include "daddy_types.hpp"
#include "dcon_generated_ids.hpp"
#include "math.hpp"
#include "pathing_types.hpp"

namespace loach {
    // Loach / RotLoach (the template: Loach.Rotted)
    enum class variant : uint8_t { loach, rot };
    inline constexpr uint32_t variant_count = 2;

    // Loach: bodyChunks[6], legs[6], tentacle[5]
    inline constexpr uint32_t body_chunks = 6;
    inline constexpr uint32_t leg_count = 6;
    inline constexpr uint32_t tentacle_count = 5;
    // LoachLeg and LoachTentacle: six TentacleChunks each
    inline constexpr uint32_t limb_chunks = 6;
    // StuckTracker.totalTrackedLastPositions (LoachAI constructor)
    inline constexpr uint32_t stuck_positions = 40;
    // Loach.eatObjects (vanilla's list; a few at a time is plenty)
    inline constexpr uint32_t max_eat_objects = 4;
    // LoachGraphics: danglers[20], spots[35], LoachTentacleGraphic(15)
    inline constexpr uint32_t dangler_count = 20;
    inline constexpr uint32_t spot_count = 35;
    inline constexpr uint32_t tentacle_graphic_segments = 15;

    // The Loach template: Air at 1, so every open tile; LoachAI.TravelPreference
    // pulls its path to preferredHeight against the terrain proximity (here
    // open_air, like the rain deer's)
    inline constexpr pathing::movement movement{{10, 10, 10, 10, 10, 10, 0}, 0, 3};

    // LoachAI.Behavior
    enum class behavior : uint8_t {
        idle,
        travelling,
        flee,
        hunt,
        return_prey,
        investigate_sound,
        escape_rain,
        get_unstuck,
    };

    // LoachTentacle.Task
    enum class task : uint8_t {
        search_ground,
        recede,
        hunt,
        hunt_backup,
        lurch_search,
        digest,
    };

    // Tentacle.TentacleChunk (no Rope)
    struct limb_chunk {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        float rad = 4.f;
        float t_pos = 0.f;
        rw::ivec2 contact_point{0, 0};
        rw::ivec2 last_contact_point{0, 0};
        rw::vec2 phase_from{0.f, 0.f};
        int32_t phases_to_same_location = 0;
        rw::ivec2 after_phase_stuck_pos{0, 0};
        float phase = -1.f;
        int32_t phase_attempts = 0;
        float stretched_fac = 1.f;
        uint8_t collide_with_terrain = 1;
    };

    // Tentacle + LoachLeg or LoachTentacle, the scalars and the chunks (the
    // segments and the grab path are the loach_limb object's vector pools);
    // tentacleNumber is the creature_part's index
    struct limb {
        // Tentacle
        float ideal_length = 200.f;
        float update_counter = 0.f;
        int32_t pull_counter = 0;
        int32_t pulls_this_tick = 0;
        std::optional<rw::vec2> float_grab_dest;
        int32_t backtrack_from = -1;
        float retract_fac = 0.f;
        uint8_t limp = 0;
        // its connectedChunk (a leg: 1 or 2; a tentacle: the head, 0)
        uint8_t connected_chunk = 1;
        // a LoachTentacle (else a LoachLeg)
        uint8_t mouth = 0;
        // tProps.goalAttractionSpeedTip (LoachTentacle.SwitchTask changes it)
        float goal_attraction_speed_tip = 0.75f;
        // LoachLeg
        rw::vec2 tentacle_dir{0.f, 1.f};
        float max_length = 320.f;
        rw::vec2 desired_grab_pos{0.f, 0.f};
        uint8_t attached_at_tip = 0;
        uint8_t play_step_sound = 0;
        int32_t frames_without_reaching = 0;
        int32_t grab_delay = 0;
        int32_t sure_of_footing_counter = 0;
        float smoothed_footing_security = 0.f;
        float fatigue = 0.f;
        float step_sound_vol = 0.f;
        // LoachTentacle: task, graspAge, grabChunk (a creature's chunk),
        // huntedCreature (a tracked creature), sticky, the HuntBackup swing
        task current = task::search_ground;
        int32_t grasp_age = 0;
        dcon::creature_id grab;
        uint8_t grab_chunk = 0;
        uint8_t hunt_backup_check1 = 0;
        uint8_t hunt_backup_check2 = 0;
        uint8_t up = 0;
        dcon::creature_id hunted;
        float sticky = 0.f;
        int32_t lurch_search_update = 3;
        float bezier_t = 0.f;
        rw::vec2 prey_pos{0.f, 0.f};
        limb_chunk c[limb_chunks];
    };

    // Loach.EatObject: a creature's chunk being swallowed
    struct eat_object {
        dcon::creature_id who;
        uint8_t chunk = 0;
        float distance = 0.f;
        float progression = 0.f;
    };

    // LoachAI with its StuckTracker, LoachState (interest, the creature it
    // is angry at: PreyTracker.AddPrey's), Loach's own counters and its
    // eatObjects
    struct ai_state {
        int32_t time_in_this_room = 0;
        int32_t idle_counter = 0;
        int32_t idle_spot_win_streak = 0;
        rw::ivec2 forbidden_idle_spot{-1, -1};
        behavior last_behavior = behavior::idle;
        // LoachState.interest, and the prey it was given (SetNewTarget +
        // ForgetAllPrey + AddPrey): tracked as Eats while interest lasts
        int32_t interest = 0;
        dcon::creature_id target;
        // focusCreature
        dcon::creature_id focus;
        // Loach
        int32_t vocalize_sound_cooldown = 0;
        int32_t random_stumble_counter = 500;
        int32_t spasm_chunk = 0;
        rw::vec2 spasm_force{0.f, 0.f};
        uint8_t lurch_sound = 0;
        uint8_t floor_detected = 0;
        // StuckTracker: pastPositions (a ring, newest at head), stuckCounter
        rw::ivec2 past[stuck_positions] = {};
        uint8_t past_count = 0;
        uint8_t past_head = 0;
        uint8_t satisfied = 0;
        int32_t stuck_counter = 0;
        // eatObjects
        eat_object eats[max_eat_objects];
        uint8_t eat_count = 0;
    };

    // Dangler.DanglerSegment
    struct dangler_segment {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        float rad = 3.f;
        float con_rad = 4.f;
        float stretched_rad = 3.f;
    };

    // LoachGraphics.DanglerAttachment
    struct dangler_attachment {
        uint8_t first_chunk = 0;
        uint8_t segs = 0;
        float lerp = 0.f;
        rw::vec2 offset{0.f, 0.f};
    };

    // LoachGraphics.SpotData
    struct spot {
        rw::vec2 attachment{0.f, 0.f};
        float lightness = 0.f;
        float scale = 1.f;
        float angle_factor = 0.f;
    };

    // the graphics module's state (purely cosmetic)
    struct graphics_state {
        float body_length = 0.f;
        int32_t breath_timer = 0;
        float breath_factor = 0.f;
        float last_breath_factor = 0.f;
        float walk_timer = 0.f;
        float walk_sway = 0.f;
        float last_walk_sway = 0.f;
        int32_t blink = 0;
        float flip = 0.f;
        float last_flip = 0.f;
        float resting = 0.f;
        float last_resting = 0.f;
        // CreatureLooker (simplified): where the eyes look, relative
        rw::vec2 look_point[2] = {{0.f, 0.f}, {0.f, 0.f}};
        dangler_attachment danglers[dangler_count];
        spot spots[spot_count];
        // LoachTentacleGraphic: RopeGraphic(15) per tentacle
        daddy::rope_segment tentacle_segments[tentacle_count][tentacle_graphic_segments];
    };
}
