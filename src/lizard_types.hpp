#pragma once

// Lizard state that isn't a plain property of the lizard object: the breeds
// (LizardBreeds / LizardBreedParams), Lizard.cs's movement state, LizardAI
// and its modules, LizardTongue, LizardJumpModule, and the LizardGraphics
// state, stored with the lizard

#include <array>
#include <cstdint>

#include "math.hpp"
#include "pathing_types.hpp"
#include "player_types.hpp"

namespace lizard {
    // CreatureTemplate.Type of the lizard breeds (vanilla, Downpour, Watcher)
    enum class breed : uint8_t {
        pink,
        green,
        blue,
        yellow,
        white,
        red,
        black,
        salamander,
        cyan,
        caramel, // SpitLizard
        eel,
        zoop,
        train,
        blizzard,
        basilisk,
        indigo,
        peach,
        count
    };
    inline constexpr uint32_t breed_count = uint32_t(breed::count);

    // LizardBreedParams.SpeedMultiplier
    struct speed_multiplier {
        float speed = 0.1f;
        float horizontal = 1.f;
        float up = 1.f;
        float down = 1.f;
    };

    // LizardTongue's per breed constants (its constructor)
    struct tongue_params {
        float range = 0.f;
        float elastic_range = 0.f;
        float lash_out_speed = 0.f;
        float reel_in_speed = 0.f;
        float chunk_drag = 0.f;
        float terrain_drag = 0.f;
        float drag_elasticity = 0.f;
        float empty_elasticity = 0.f;
        float involuntary_release_chance = 0.f;
        float voluntary_release_chance = 0.f;
        float attach_terrain_chance = 0.5f;
        float pull_at_chunk_ratio = 1.f;
        float detatch_min_distance_terrain = 20.f;
        float detatch_min_distance_creature = 20.f;
        float tot_r_extra_limit = 40.f;
        float tot_r = 0.f;
        bool base_drag_only = false;
        bool attaches_background_walls = false;
    };

    // LizardBreedParams, with what LizardBreeds.BreedTemplate puts in the
    // CreatureTemplate next to it
    struct breed_params {
        float toughness = 0.f;
        float stun_toughness = 0.f;
        float bite_damage = 0.f;
        float bite_damage_chance = 0.f;
        float aggression_curve_exponent = 0.925f;
        float danger = 0.f;
        int32_t bite_delay = 0;
        float base_speed = 0.f;
        float bite_in_front = 0.f;
        float bite_rad_bonus = 0.f;
        float bite_homing_speed = 0.f;
        float bite_chance = 0.f;
        float attempt_bite_radius = 0.f;
        float get_free_bite_chance = 0.f;
        rw::vec3 standard_color{0.f, 0.f, 0.f};
        int32_t regain_footing_counter = 0;
        float body_mass = 0.f;
        float body_size_fac = 0.f;
        float body_length_fac = 1.f;
        float body_rad_fac = 1.f;
        float pull_down_fac = 1.f;
        float floor_leverage = 0.f;
        float max_muscle_power = 0.f;
        // terrainSpeeds by pathing::access (vanilla indexes AItile.Accessibility)
        speed_multiplier terrain_speeds[pathing::access_count];
        float wiggle_speed = 0.f;
        int32_t wiggle_delay = 0;
        float body_stiffnes = 0.f;
        float swim_speed = 0.f;
        int32_t idle_counter_subtract_when_close_to_idle_pos = 0;
        float head_shield_angle = 0.f;
        bool can_exit_lounge = false;
        bool can_exit_lounge_warm_up = false;
        float find_lounge_direction = 0.f;
        float lounge_distance = 0.f;
        int32_t pre_lounge_crouch = 0;
        float pre_lounge_crouch_movement = 0.f;
        float lounge_speed = 0.f;
        int32_t lounge_propulsion_frames = 0;
        int32_t lounge_maximum_frames = 0;
        float lounge_jumpyness = 0.f;
        int32_t lounge_delay = 0;
        float risk_of_double_lounge_delay = 0.f;
        int32_t post_lounge_stun = 0;
        float lounge_tendensy = 0.f;
        float perfect_vision_angle = 0.f;
        float periferal_vision_angle = 0.f;
        int32_t shake_prey = 100;
        float bite_dominance = 0.f;
        float limb_size = 0.f;
        float limb_thickness = 0.f;
        float step_length = 0.f;
        float lift_feet = 0.f;
        float feet_down = 0.f;
        float no_grip_speed = 0.f;
        float limb_speed = 0.f;
        float limb_quickness = 0.f;
        int32_t limb_grip_delay = 0;
        bool smoothen_leg_movement = false;
        float leg_pair_displacement = 0.f;
        float walk_bob = 0.f;
        int32_t tail_segments = 0;
        float tail_stiffness = 0.f;
        float tail_stiffness_decline = 0.f;
        float tail_length_factor = 0.f;
        float tail_coloration_start = 0.f;
        float tail_coloration_exponent = 0.f;
        float head_size = 0.f;
        float neck_stiffness = 0.f;
        float jaw_open_angle = 0.f;
        float jaw_open_lower_jaw_fac = 0.f;
        float jaw_open_move_jaws_apart = 0.f;
        std::array<int32_t, 5> head_graphics{};
        int32_t frames_between_look_focus_change = 0;
        bool tongue = false;
        float tongue_attack_range = 0.f;
        int32_t tongue_warm_up = 0;
        int32_t tongue_segments = 0;
        float tongue_chance = 0.f;
        float taming_difficulty = 1.f;

        // CreatureTemplate
        float visual_radius = 0.f;
        float water_vision = 0.f;
        float through_surface_vision = 0.f;
        float movement_based_vision = 0.3f;
        float water_pathing_resistance = 3.f;
        float base_damage_resistance = 0.f;
        float base_stun_resistance = 0.f;
        // damageRestistances[Bite] (damage, stun) and [Explosion] (damage)
        float bite_resistance[2] = {2.5f, 3.f};
        float explosion_resistance = 0.f;
        float body_size = 2.f;
        // TileTypeResistances (+ waterPathingResistance for the swimmers)
        pathing::movement movement;
        // Lizard.Swimmer, LizardBreedParams.WallClimber, Lizard.Burrower
        bool swimmer = false;
        bool wall_climber = false;
        bool burrower = false;
        tongue_params tongue_p;
    };

    // Lizard.Animation, in vanilla order (EnterAnimation compares indices)
    enum class animation : uint8_t {
        standard,
        hear_sound,
        prey_re_spotted,
        prey_spotted,
        fighting_stance,
        threat_spotted,
        threat_re_spotted,
        shoot_tongue,
        spit,
        prepare_to_jump,
        jumping,
        prepare_to_lounge,
        lounge,
        shake_prey,
    };

    // LizardAI.Behavior
    enum class behavior : uint8_t {
        idle,
        hunt,
        flee,
        travelling,
        escape_rain,
        return_prey,
        injured,
        fighting,
        frustrated,
        acting_out_mission,
        lurk,
        investigate_sound,
        go_to_spit_pos,
        follow_friend,
    };

    // MovementConnection.MovementType, the ones a lizard meets here: the path
    // fields' steps (Standard, OpenDiagonal, the drops) and the LizardTurn and
    // ReachUp its own code makes up
    enum class move_type : uint8_t {
        standard,
        open_diagonal,
        drop_to_floor,
        drop_to_climb,
        lizard_turn,
        reach_up,
    };

    // MovementConnection (`valid` false is default(MovementConnection))
    struct movement_connection {
        move_type type = move_type::standard;
        rw::ivec2 start{0, 0};
        rw::ivec2 dest{0, 0};
        bool valid = false;
        constexpr bool operator==(movement_connection const&) const = default;
    };

    // Lizard.MovementAnimation (with its BodyPartDestinations)
    struct movement_animation {
        struct destination {
            uint8_t part = 0;
            rw::vec2 pos{0.f, 0.f};
            float finished_dist = 0.f;
            float max_distance = 0.f;
        };
        movement_connection connection;
        int32_t step = 0;
        int32_t total_steps = 0;
        destination destinations[3];
        uint8_t count = 0;
        float friction = 0.f;
        float speed = 0.f;
        bool active = false;
    };

    // LizardTongue.State
    enum class tongue_state : uint8_t {
        hidden,
        lashing_out,
        attatched,
        attached_in_small_object,
        retracting,
        stuck_in_terrain,
    };

    // LizardTongue: what it sticks to is a slugcat's chunk (player index + 1)
    struct tongue {
        tongue_state state = tongue_state::hidden;
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        rw::vec2 graph_pos[2];
        float dist = 0.f;
        float reel_in = 0.f;
        int32_t stuck_counter = 0;
        int32_t delay = 0;
        uint8_t attached_player = 0;
        uint8_t attached_chunk = 0;
        bool bite_inanimate = false;
    };

    // LizardJumpModule.JumpFinder.JumpInstruction: the goal cell is a tile,
    // its PathingCell the lizard's path field read at that tile
    struct jump_instruction {
        rw::ivec2 goal{0, 0};
        bool has_goal = false;
        int32_t tick = 0;
        rw::vec2 start_pos{0.f, 0.f};
        rw::vec2 init_vel{0.f, 0.f};
        float power = 0.f;
        bool grab_when_landing = false;
        bool set = false; // != default(JumpInstruction)
    };

    // LizardJumpModule.JumpFinder
    struct jump_finder {
        bool active = false;
        bool chain_jump = false;
        rw::ivec2 start{0, 0};
        int32_t fade = 0;
        jump_instruction best;
        jump_instruction current;
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        bool ventured_away_from_terrain = false;
        bool has_landing_direction = false;
        rw::vec2 landing_direction{0.f, 0.f};
    };

    // LizardJumpModule (Cyan): a few finders testing jumps from tiles ahead
    // on the path, the jump it acts on (actOnJump, kept as a copy)
    struct jump_module {
        static constexpr int32_t max_finders = 4;
        jump_finder finders[max_finders];
        jump_finder act_on;
        bool has_act_on = false;
        int32_t add_delay = 0;
        int32_t jump_counter = 0;
        int32_t spin = 0;
        // lizardCell: the path cost at the last tile it had one, unreachable if none
        uint16_t lizard_cell = 0xFFFF;
    };

    // StuckTracker (trackPastPositions, 40 positions from 30 on, 5 tiles, 10
    // close ones) with its MoveBacklog
    struct stuck_tracker {
        static constexpr int32_t tracked = 40;
        rw::ivec2 past[tracked];
        int32_t count = 0;
        int32_t head = 0; // index of the newest
        int32_t counter = 0;
        static constexpr int32_t backlog_size = 10; // trackedMoves
        movement_connection backlog[backlog_size];
        int32_t backlog_head = 0;
        bool satisfied_with_this_position = false;
    };

    // LizardAI.LizardSpitTracker (Red, Caramel)
    struct spit_tracker {
        bool spitting = false;
        bool want_to_spit = false;
        bool target_reachable = false;
        rw::ivec2 want_to_spit_at_pos{0, 0};
        rw::ivec2 temp_spit_from_pos{0, 0};
        rw::ivec2 spit_from_pos{0, 0};
        int32_t random_cycle = 0;
        int32_t delay = 0;
        int32_t dont_wanna_spit_counter = 0;
        int32_t wanna_spit_counter = 0;
        int32_t go_to_spit_pos_counter = 0;
        float aim_up = 0.f;
        float general_aim_up = 0.5f;
        // aimForChunk: the player's chunk (player index + 1, chunk)
        uint8_t aim_player = 0;
        uint8_t aim_chunk = 0;
    };

    // LizardAI.LurkTracker (White, Salamander, Eel, Peach)
    struct lurk_tracker {
        rw::ivec2 lurk_position{0, 0};
        rw::ivec2 look_position{0, 0};
        int32_t best_vis_look = 0;
    };

    // LizardAI's modules, in the order the UtilityComparer compares them
    enum class module : uint8_t { threat, prey, rain, injury, aggression, mission, noise, friend_tracker, lurk, none };

    // UtilityComparer: the smoothed (weighted) utilities of the modules with
    // a smoother, and the winner of the last comparison
    struct utilities {
        float threat = 0.f;
        float prey = 0.f;
        module best = module::none;
        float best_utility = 0.f;
    };

    // Tracker's creature representation of the slugcat it knows of
    struct tracked_player {
        uint8_t player = 0; // index + 1, 0 none
        bool visual_contact = false;
        int32_t ticks_since_seen = 0;
        int32_t age = 0;
        rw::vec2 last_seen_pos{0.f, 0.f};
        // PreyTracker.TrackedPrey
        int32_t unreachable_counter = 0;
        int32_t at_position_but_cant_see_counter = 0;
        rw::ivec2 last_best_guess{0, 0};
    };

    // LizardAI and the modules it keeps
    struct ai_state {
        behavior current = behavior::idle;
        float excitement = 0.5f;
        int32_t panic = 0;
        float hunger = 0.f;
        float fear = 0.f;
        float rain_fear = 0.f;
        int32_t last_distress_length = 0;
        float run_speed = 0.f;
        float current_utility = 0.f;
        int32_t idle_counter = 0;
        int32_t idle_spot_win_streak = 0;
        rw::ivec2 forbidden_idle_spot{-1, -1};
        int32_t idle_restlessness = 0;
        int32_t unable_to_find_comfortable_position = 0;
        // focusCreature / casualAggressionTarget: the tracked slugcat
        bool focus = false;
        bool casual_aggression = false;
        int32_t time_in_room = 0;
        int32_t attempt_bite_frames = 0;
        rw::ivec2 destination{0, 0};
        tracked_player rep;
        // PreyTracker.frustration
        float frustration = 0.f;
        utilities u;
        stuck_tracker stuck;
        // NoiseTracker: the sound it examines (soundToExamine)
        bool examining = false;
        rw::ivec2 examine_pos{0, 0};
        float mysterious_noises = 0.f;
        int32_t mysterious_noise_counter = 0;
        // SocialMemory: the temporary like of the slugcat (every player is one here)
        float temp_like = 0.f;
        spit_tracker spit;
        lurk_tracker lurk;
        // YellowAI
        int32_t communicating = 0;
        float comm_flicker = 0.f;
        // LizardPather.forbiddenEntranceCounter isn't ported; the field it
        // follows is the one to `destination`
    };

    // LizardGraphics.IndividualVariations
    struct individual_variations {
        float head_size = 1.f;
        float fatness = 1.f;
        float tail_length = 1.f;
        float tail_fatness = 1.f;
        float tail_color = 0.f;
    };

    // LizardLimb
    struct limb {
        player::hand_state h;
        bool retract = false;
        bool reaching_for_terrain = false;
        bool currently_disabled = false;
        bool extra_long_step = false;
        bool sound_bool = false;
        bool terrain_contact = false;
        int32_t grip_counter = 0;
        float joint_dist = 0.f;
        float flip = 0.f;
        rw::vec2 grab_pos{0.f, 0.f};
    };

    inline constexpr int32_t max_tail = 16;
    inline constexpr int32_t max_tongue = 10;
    inline constexpr int32_t max_limbs = 6;

    // TailSegment (connectionRad and its stretch)
    struct tail_segment {
        player::body_part bp;
        float connection_rad = 0.f;
        bool terrain_contact = false;
    };

    // LizardGraphics. Cosmetics (pass 2): LizardGraphics.cosmetics and their
    // own state belong at the end of this struct, after `random`
    struct graphics_state {
        int32_t limb_count = 4;
        limb limbs[max_limbs];
        rw::vec2 draw_positions[3][2];
        tail_segment tail[max_tail];
        player::body_part tongue[max_tongue];
        player::body_part head;
        int32_t legs_grabbing = 0;
        int32_t front_legs_grabbing = 0;
        int32_t hind_legs_grabbing = 0;
        int32_t no_grip_counter = 0;
        float front_bob = 0.f;
        float hind_bob = 0.f;
        float depth_rotation = 0.f;
        float last_depth_rotation = 0.f;
        float head_depth_rotation = 0.f;
        float last_head_depth_rotation = 0.f;
        bool stunned_last_update = false;
        bool rotate_while_stunned = false;
        float last_blink = 0.f;
        float blink = 0.f;
        int32_t flicker = 0;
        float flicker_color = 0.f;
        int32_t white_flicker = 0;
        bool every_second_draw = false;
        float last_breath = 0.f;
        float breath = 0.f;
        rw::vec2 tail_direction{0.f, 0.f};
        rw::vec2 look_pos{0.f, 0.f};
        // CreatureLooker: whether it looks at the slugcat, and its timer
        bool look_creature = false;
        int32_t look_counter = 0;
        float head_color_setter = 0.f;
        rw::vec2 eyes[2][3];
        float eye_beams_active = 0.f;
        // the White lizard's camouflage: whiteCamoColor is the colour it
        // picks up from behind it (resolved when drawn, see lizard_graphics),
        // tinted by its dominance displays and glitch fits
        float white_camo_color_amount = -1.f;
        float white_camo_color_amount_drag = 1.f;
        rw::vec3 white_tint{0.f, 0.f, 0.f};
        float white_tint_amount = 0.f;
        float white_dominance_hue = 0.f;
        int32_t white_glitch_fit = 0;
        bool black_salamander = false;
        rw::vec3 ivar_body_color{0.f, 0.f, 0.f};
        float body_length = 0.f;
        float tail_length = 0.f;
        float head_connection_rad = 0.f;
        float show_dominance = 0.f;
        float black_lizard_light_up_head = 0.f;
        float voice_visualization_intensity = 0.f;
        float last_voice_visualization_intensity = 0.f;
        float voice_visualization = 0.f;
        float last_voice_visualization = 0.f;
        int32_t override_head_graphic = -1;
        individual_variations ivars;
        // UnityEngine.Random as the graphics module uses it
        rw::rng random;
    };

    // LizardState.limbHealth
    struct limb_health {
        float v[max_limbs] = {1.f, 1.f, 1.f, 1.f, 1.f, 1.f};
    };

    // Lizard.cs's own state
    struct body_state {
        int32_t bite_delay = 0;
        bool salamander_lurk = false;
        float swim = 0.f;
        int32_t grabbed_attack_counter = 0;
        int32_t turned_by_rock_direction = 0;
        int32_t turned_by_rock_counter = 0;
        float jaw_open = 0.f;
        float last_jaw_open = 0.f;
        float jaw_forced_shut = 0.f;
        rw::vec2 lounge_dir{0.f, 0.f};
        int32_t bubble = 0;
        float bubble_intensity = 0.f;
        int32_t lounge_delay = 0;
        int32_t post_lounge_stun = 0;
        int32_t time_in_animation = 0;
        int32_t time_to_remain_in_animation = -1;
        animation anim = animation::standard;
        movement_connection following;
        movement_connection last_following;
        movement_connection commited_to_drop;
        // upcomingConnections (at most five)
        movement_connection upcoming[5];
        int32_t upcoming_count = 0;
        movement_animation move_anim;
        bool apply_gravity = false;
        int32_t in_allowed_terrain_counter = 0;
        bool has_grip_point = false;
        rw::vec2 grip_point{0.f, 0.f};
        int32_t time_spent_trying_this_move = 0;
        float desperation_smoother = 0.f;
        int32_t snake_ticker = 0;
        rw::vec2 limbs_aim_for{0.f, 0.f};
        float straighten_out_needed = 0.f;
        bool narrow_upcoming = false;
        bool climb_upcoming = false;
        bool shortcut_upcoming = false;
        float body_wiggle = 0.f;
        int32_t body_wiggle_counter = 0; // bwc, clamped to 0..100
        // grasps[0]: the slugcat in its jaws (player index + 1, 0 none) and
        // the chunk, and how long it has held it (Player.dangerGraspTime)
        uint8_t grasp_player = 0;
        uint8_t grasp_chunk = 0;
        int32_t danger_grasp_time = 0;
        // LizardVoice: the emotion's sound while it lasts
        int32_t voice_index = -1;
        float voice_progression = 0.f;
        float voice_intensity = 0.f;
        float voice_length = 0.f;
        float voice_pitch = 1.f;
        uint8_t voice_trigger = 0; // 1 + index into the breed's voice sounds, 0 none
    };

    // LizardSpit: a gob of spit in flight or stuck to a slugcat
    struct spit_slime {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        int32_t connected = -1;
        float length = 0.f;
    };
    struct spit_state {
        static constexpr int32_t max_slime = 15;
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        int32_t fall_off = 0;
        float mass_left = 1.f;
        float dissapear_speed = 0.f;
        float my_aim_up = 0.f;
        uint8_t my_aim_player = 0;
        uint8_t my_aim_chunk = 0;
        uint8_t stick_player = 0;
        uint8_t stick_chunk = 0;
        int32_t slime_count = 0;
        spit_slime slime[max_slime];
        rw::vec3 color{1.f, 0.f, 0.f};
        float jagged_alpha = 0.f;
        float dot_alpha = 0.f;
        float slime_alpha[max_slime] = {};
        bool dead = false;
    };
}
