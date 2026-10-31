#pragma once

// Scavenger state that isn't a plain property of the creature (Scavenger.cs,
// a simpler ScavengerAI, ScavengerGraphics): what its payload
// (creature_types.hpp) and its data object (`scavenger_data`,
// dcon_generated.txt) keep that isn't a number

#include <cstdint>

#include "dcon_generated_ids.hpp"
#include "math.hpp"
#include "pathing_types.hpp"
#include "player_types.hpp"

namespace scavenger {
    // The Scavenger template's TileTypeResistances: floors and corridors at
    // 1, poles at 2.5 (its swimming on the surface comes on top)
    inline constexpr pathing::movement movement{{10, 10, 25, 0, 0, 0, 0}, 0};

    // Which Scavenger template: the plain one, Downpour's (DLCShared)
    // ScavengerElite, the Watcher's ScavengerDisciple (a Templar). The
    // payload's `variant` (creature_types.hpp), template_of's offset from
    // tmpl::scavenger
    enum class variant : uint8_t { scavenger, elite, disciple };
    inline constexpr uint32_t variant_count = 3;

    // ScavengerAI.Behavior, the ones kept (no Investigate, FindPackLeader,
    // LeaveRoom, GuardOutpost, CommunicateWithPlayer)
    enum class behavior : uint8_t {
        idle,
        flee,
        attack,
        escape_rain,
        injured,
        scavenge,
        travel,
    };

    // Scavenger.MovementMode
    enum class move_mode : uint8_t { run, stand_still, crawl, climb, swim };

    // Scavenger.ScavengerAnimation, the throwing ones (ThrowCharge, Throw)
    // and DLCShared's jumps (PrepareToJump, Jumping)
    enum class animation : uint8_t { none, throw_charge, throw_, prepare_to_jump, jumping };

    // ScavengerAI.ViolenceType (no ForFun); NonLethal only for the
    // Disciple (a Templar) without its blood lust
    enum class violence_type : uint8_t { none, warning, lethal, non_lethal };

    // Scavenger.JumpFinder.JumpInstruction (goalCell: a tile of the room's
    // path field)
    struct jump_instruction {
        rw::vec2 start_pos{0.f, 0.f};
        rw::vec2 init_vel{0.f, 0.f};
        float power = 0.f;
        int32_t tick = 0;
        rw::ivec2 goal{-1, -1};
        uint8_t has_goal = 0;
        uint8_t grab_when_landing = 0;
    };

    // Scavenger.JumpFinder: throws test jumps from a tile of the path ahead,
    // keeps the one landing closest to the destination
    struct jump_finder {
        rw::ivec2 start{-1, -1};
        int32_t fade = 0;
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        uint8_t ventured = 0;
        // bestJump (has_best: not null); vanilla's bestJump is the same
        // object as currentJump once a landing on it was found, so its tick
        // keeps counting with the current one's (best_shares)
        uint8_t has_best = 0;
        uint8_t best_shares = 0;
        jump_instruction current;
        jump_instruction best;
    };

    // MovementConnection, as far as the scavenger looks at one: a step of the
    // path field (MovementType Standard, OpenDiagonal, DropToFloor)
    struct movement_connection {
        enum kind : uint8_t { standard, diagonal, drop };
        rw::ivec2 start{-1, -1};
        rw::ivec2 dest{-1, -1};
        kind type = standard;
        bool valid = false;
        constexpr bool is_drop() const noexcept { return valid && type == drop; }
    };

    // AbstractCreature.Personality (rolled from the ID's RandomSeed)
    struct personality {
        float sympathy = 0.5f;
        float energy = 0.5f;
        float bravery = 0.5f;
        float nervous = 0.5f;
        float aggression = 0.5f;
        float dominance = 0.5f;
    };

    // Scavenger.SetUpCombatSkills
    struct combat_skills {
        float dodge = 0.5f;
        float mid_range = 0.5f;
        float melee = 0.5f;
        float blocking = 0.5f;
        float reaction = 0.5f;
    };

    // Scavenger.cs movement state the payload has no room for
    struct body_state {
        // connections: the path ahead (up to five steps), the first one first
        static constexpr int32_t max_connections = 5;
        movement_connection connections[max_connections];
        uint8_t connection_count = 0;
        movement_connection commited_to_move;
        int32_t commited_move_follow_chunk = 0;
        rw::ivec2 occupy_tile{-1, -1};
        rw::ivec2 last_non_solid_tile{0, 0};
        rw::ivec2 climb_orientation{1, 1};
        // swingPos, nextSwingPos, knucklePos (has_*: not null)
        rw::vec2 swing_pos{0.f, 0.f};
        rw::vec2 next_swing_pos{0.f, 0.f};
        rw::vec2 knuckle_pos{0.f, 0.f};
        uint8_t has_swing_pos = 0;
        uint8_t has_next_swing_pos = 0;
        uint8_t has_knuckle_pos = 0;
        uint8_t swing_arm = 0;
        float swing_radius = 50.f;
        float swing_progress = 0.f;
        int32_t swing_climb_counter = 0;
        int32_t swinging_forbidden = 0;
        int32_t move_mode_change_counter = 0;
        int32_t footing_counter = 0;
        int32_t not_following_path_counter = 0;
        int32_t stuck_on_shortcut_counter = 0;
        // pastPositions (the newest first), a ring
        static constexpr int32_t max_past = 60;
        rw::ivec2 past[max_past];
        int32_t past_count = 0;
        int32_t past_head = 0;
        // lookPoint (the AI moves it about)
        rw::vec2 look_point{0.f, 0.f};
        // The Elite's and the Templar's jumps (JumpLogicUpdate): its
        // jumpFinders, actOnJump (has_act), jumpCounter, addDelay, jumpCell
        // (a tile of the path field), spin
        static constexpr int32_t max_jump_finders = 4;
        jump_finder finders[max_jump_finders];
        uint8_t finder_count = 0;
        uint8_t has_act = 0;
        uint8_t has_jump_cell = 0;
        int8_t spin = 0;
        rw::ivec2 act_start{-1, -1};
        int32_t act_fade = 0;
        jump_instruction act;
        int32_t jump_counter = 0;
        int32_t add_delay = 0;
        rw::ivec2 jump_cell{-1, -1};
        // PrepareToJumpAnimation.animationLength, cycleTime
        int32_t jump_anim_length = 30;
        float jump_cycle = 0.f;
    };

    // ScavengerAI, simplified, and Scavenger.cs's combat (the animation
    // included)
    struct ai_state {
        float agitation = 0.f;
        float scared = 0.f;
        float run_speed_goal = 0.f;
        // the like of the slugcat (every slugcat counts as the same one):
        // below zero once a slugcat hurt it or a scavenger of its room
        float like = 0.f;
        // ticks it has threatened the hostile slugcat (Warning) before going
        // Lethal
        int32_t warned = 0;
        violence_type violence = violence_type::none;
        int32_t age = 0;
        int32_t idle_counter = 0;
        rw::ivec2 test_idle_pos{0, 0};
        rw::ivec2 test_throw_pos{0, 0};
        int32_t change_attack_position_delay = 0;
        int32_t arrange_inventory_counter = 0;
        int32_t scavage_item_check = 0;
        // scavengeCandidate (none: no item)
        dcon::item_id scavenge_candidate;
        // focusCreature
        dcon::creature_id focus;
        // UpdateLookPoint's testLookPos, alreadyLookedAtPos
        rw::vec2 test_look_pos{0.f, 0.f};
        rw::vec2 already_looked_at_pos{0.f, 0.f};
        // CombatUpdate
        float reflex_build_up = 0.f;
        // immediatelyThrowAtChunk: a creature's chunk
        dcon::creature_id melee_target;
        uint8_t melee_chunk = 0;
        // the animation: ThrowCharge (target: a creature's chunk, else
        // aim_target) and Throw (its flip, the item thrown)
        animation anim = animation::none;
        int32_t anim_age = 0;
        int32_t discontinue = 0;
        float charge_cycle = 0.f;
        float charge_shake = 0.f;
        dcon::creature_id charge_target;
        uint8_t charge_chunk = 0;
        uint8_t has_aim_target = 0;
        rw::vec2 aim_target{0.f, 0.f};
        float throw_flip = 1.f;
        dcon::item_id thrown_item;

        // The Templar's (the Disciple's) ScavengerAI: bloodLust, PingCD,
        // stunnedCreature, targetedBodyChunk, Scavenger.boomerangCharge; the
        // spear it threw, watched for HitAnObjectWithWeapon
        int32_t blood_lust = 0;
        int32_t ping_cd = 0;
        dcon::creature_id stunned_creature;
        dcon::creature_id targeted;
        uint8_t targeted_chunk = 0;
        uint8_t boomerang_charge = 0;
        dcon::item_id watched_throw;
        // Scavenger.karmaLevels, karmicArmorTriggered, karmicArmorTimer,
        // karmicArmorResetTime
        int32_t karma_levels = 0;
        uint8_t armor_triggered = 0;
        int32_t armor_timer = 45;
        int32_t armor_reset_time = 45;
        // the KarmicArmor object (armor_alive: in the room): its karmaLevel
        // and shakeAmount
        uint8_t armor_alive = 0;
        int32_t armor_level = 0;
        float armor_shake = 0.f;
        // KarmicShockwave (the armor exploding): frame (< 0: none), the
        // creatures it hit
        static constexpr int32_t max_hit = 16;
        int32_t shock_frame = -1;
        rw::vec2 shock_pos{0.f, 0.f};
        uint8_t shock_hit_count = 0;
        dcon::creature_id shock_hit[max_hit];
        // DisciplePing (ping_frame < 0: none): where, rad, radIncrement,
        // the objects it hit
        int32_t ping_frame = -1;
        rw::vec2 ping_pos{0.f, 0.f};
        float ping_rad = 0.f;
        float ping_inc = 0.f;
        uint8_t ping_hit_count = 0;
        dcon::creature_id ping_hit[max_hit];
        // Scavenger.readyToReleaseMask (the Elite's VultureMask dropped)
        uint8_t mask_released = 0;
    };

    // HSLColor
    struct hsl {
        float hue = 0.f;
        float saturation = 0.f;
        float lightness = 0.f;
    };

    // ScavengerCosmetic.BackDecals.Pattern
    enum class back_pattern : uint8_t { spine_ridge, double_spine_ridge, random_back_blotch };

    // What ScavengerGraphics rolls from the ID's RandomSeed when it is made
    // (IndividualVariations, GenerateColors, the cosmetics, the eartlers,
    // the teeth): fixed for the creature's life
    struct looks {
        // IndividualVariations
        float head_size = 0.5f;
        float eartler_width = 0.5f;
        float neck_thickness = 0.5f;
        float hands_head_color = 0.f;
        float eye_size = 0.5f;
        float narrow_eyes = 0.f;
        float eyes_angle = 0.5f;
        float fatness = 0.5f;
        float narrow_waist = 0.5f;
        float legs_size = 0.5f;
        float arm_thickness = 0.5f;
        float wide_teeth = 0.5f;
        float pupil_size = 0.f;
        float scruffy = 0.f;
        float general_melanin = 0.5f;
        uint8_t colored_eartler_tips = 0;
        uint8_t deep_pupils = 0;
        uint8_t colored_pupils = 0;
        uint8_t tail_segs = 0;
        // IndividualVariations.WaistWidth
        constexpr float waist_width() const noexcept { return fatness * (1.f - narrow_waist); }
        // GenerateColors
        hsl body_color, head_color, decoration_color, belly_color, eye_color;
        float body_color_black = 0.f;
        float head_color_black = 0.f;
        float belly_color_black = 0.f;
        // the back cosmetic: HardBackSpikes (else WobblyBackTufts), with
        // BackTuftsAndRidges' and BackDecals' rolls
        static constexpr int32_t max_decals = 40;
        uint8_t hard_spikes = 0;
        back_pattern pattern = back_pattern::random_back_blotch;
        uint8_t scale_graf = 0;
        uint8_t use_detail_color = 0;
        uint8_t decal_count = 0;
        float x_flip = -1.f;
        float colored = 0.f;
        float general_size = 0.f;
        float top = 0.f;
        float bottom = 0.f;
        rw::vec2 positions[max_decals];
        float color_alphas[max_decals];
        // WobblyBackTufts: Scale.Stats, the scales' lengths and random dirs
        float scale_grav = 0.f;
        float scale_air_fric = 0.f;
        float scale_rigid = 0.f;
        float scale_rigid_grad_rad = 0.f;
        float scale_rigid_exp = 1.f;
        float scale_elastic = 0.f;
        float out_to_sides = 0.f;
        float down_along_spine = 0.f;
        float scale_lengths[max_decals];
        rw::vec2 random_dirs[max_decals];
        // Eartlers: the branches (mirrored in pairs), their vertices
        static constexpr int32_t max_branches = 10;
        static constexpr int32_t max_branch_vertices = 4;
        uint8_t branch_count = 0;
        uint8_t branch_length[max_branches] = {};
        rw::vec2 branch_pos[max_branches][max_branch_vertices];
        float branch_rad[max_branches][max_branch_vertices];
        // teeth[i, 0], teeth[i, 1]
        static constexpr int32_t max_teeth = 8;
        uint8_t teeth_count = 0;
        float teeth[max_teeth][2];
        // the mask (VultureMaskGraphics, not drawn): 0 none, the Elite's
        // 1 KrakenMask, 2 SpikeMask, 3 HornedMask, 4 SadMask, 5 the
        // Templar's
        uint8_t mask = 0;
    };

    // ScavengerHand (Limb), with its spearPosAdd and grabPos
    struct hand {
        player::hand_state limb;
        rw::vec2 spear_pos_add{0.f, 0.f};
        rw::vec2 last_spear_pos_add{0.f, 0.f};
        rw::vec2 grab_pos{0.f, 0.f};
        uint8_t has_grab_pos = 0;
    };

    // ScavengerGraphics' state from tick to tick (cosmetic, but for the
    // hands the held items follow)
    struct graphics_state {
        // drawPositions[i, 0] (now) and [i, 1] (last tick): head, neck,
        // chest, waist, hips
        rw::vec2 draw_positions[5][2];
        float spine_lengths[2] = {0.f, 0.f};
        hand hands[2];
        player::hand_state legs[2];
        // the TailSegments
        player::body_part tail[4];
        // WobblyBackTufts' Scales
        player::body_part scales[looks::max_decals];
        float flip = 0.f, last_flip = 0.f;
        float cycle = 0.f, last_cycle = 0.f;
        rw::vec2 body_axis{0.f, 1.f}, last_body_axis{0.f, 1.f};
        float rise_body = 0.f;
        float look_up = 0.f, last_look_up = 0.f;
        float neutral_face = 0.f, last_neutral_face = 0.f;
        float shifting_neutral_face = 0.f, shifting_neutral_face_goal = 0.f;
        float swim_arms = 0.f, last_swim_arms = 0.f;
        float eyes_open = 1.f, last_eyes_open = 1.f;
        int32_t blink = 0;
        rw::vec2 look_point{0.f, 0.f}, last_look_point{0.f, 0.f};
        float shake = 0.f, last_shake = 0.f;
        float eyes_pop = 0.f, last_eyes_pop = 0.f;
        float bristle = 0.f, last_bristle = 0.f;
        float eyes_pop_go_back = 0.f;
        // lastKnuckleSoundPos, lastLastKnuckleSoundPos, spearSound
        rw::vec2 last_knuckle_sound_pos{0.f, 0.f};
        rw::vec2 last_last_knuckle_sound_pos{0.f, 0.f};
        uint8_t spear_sound = 0;
        // reset since it came into view (the hands are where the items go)
        uint8_t ready = 0;
        // (its UnityEngine.Random is the creature's graphics_random)
    };
}
