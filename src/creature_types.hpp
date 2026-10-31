#pragma once

// The creature object's type tag and its payload, and the creature parts'.
//
// Every creature, the slugcat included, is one `creature` object of the
// data container: what most kinds have (stun, death, grasps, the AI's
// tracker and destination...) is
// a property of its own (SoA, see dcon_generated.txt), and what only its kind
// has is the `payload`, a union of one small struct per kind (at most 64
// bytes: one cache line per creature). A kind's bigger state (its AI
// modules, its graphics module) is a per-kind data object (`centipede_data`,
// `lizard_data`...) whose index the payload keeps.
//
// The same for the repeated parts of a creature with state of their own
// (`creature_part`: a daddy's tentacles and tube graphics): what every part
// has is SoA, the rest a `part_payload` per kind, pointing at a per-kind data
// object for the vector pools.
//
// The payloads are defined here, before the data container's objects (the
// id types come first, dcon_generated_ids.hpp): they keep their data object
// as its plain index (`data`), the creatures they refer to as their ids.

#include <cstdint>
#include <cstring>
#include <type_traits>

#include "dcon_generated_ids.hpp"
#include "centipede_types.hpp"
#include "cicada_types.hpp"
#include "daddy_types.hpp"
#include "dropbug_types.hpp"
#include "leech_types.hpp"
#include "lizard_types.hpp"
#include "math.hpp"
#include "tube_worm_types.hpp"
#include "needle_worm_types.hpp"
#include "pole_mimic_types.hpp"
#include "tentacle_plant_types.hpp"
#include "jetfish_types.hpp"
#include "scavenger_types.hpp"
#include "deer_types.hpp"
#include "miros_types.hpp"
#include "barnacle_types.hpp"
#include "egg_bug_types.hpp"
#include "garbage_worm_types.hpp"
#include "lantern_mouse_types.hpp"
#include "loach_types.hpp"
#include "moth_grub_types.hpp"
#include "overseer_types.hpp"
#include "snail_types.hpp"
#include "sky_whale_types.hpp"
#include "yeek_types.hpp"
#include "vulture_types.hpp"

namespace creature {
    // CreatureTemplate.Type of the ported creatures. The order is the update
    // order of a room: the slugcats first (player.cpp runs them, before the
    // rest), then the kinds in the order every per kind loop used to run in
    enum class type : uint8_t {
        slugcat,
        centipede,
        dropbug,
        cicada,
        lizard,
        daddy,
        leech,
        tube_worm,
        // noodlefly (BigNeedleWorm and its SmallNeedleWorm young)
        needle_worm,
        // pole plant
        pole_mimic,
        // monster kelp
        tentacle_plant,
        jetfish,
        scavenger,
        // rain deer
        deer,
        miros,
        // the Watcher's barnacle
        barnacle,
        // egg bug and Downpour's fire bug (the payload's variant)
        egg_bug,
        garbage_worm,
        lantern_mouse,
        // the Watcher's loach (Loach, RotLoach: the payload's variant)
        loach,
        // the Watcher's moth grub
        moth_grub,
        // Five Pebbles' (and Moon's) little eyes
        overseer,
        snail,
        // the Watcher's sky whale
        sky_whale,
        // Downpour's Yeek
        yeek,
        // Vulture, KingVulture, MirosVulture (the payload's variant)
        vulture,
        count
    };
    inline constexpr uint32_t type_count = uint32_t(type::count);
    inline constexpr type all_types[type_count] = {type::slugcat, type::centipede, type::dropbug, type::cicada, type::lizard, type::daddy, type::leech, type::tube_worm,
        type::needle_worm, type::pole_mimic, type::tentacle_plant, type::jetfish, type::scavenger, type::deer, type::miros,
        type::barnacle, type::egg_bug, type::garbage_worm, type::lantern_mouse, type::loach, type::moth_grub, type::overseer, type::snail,
        type::sky_whale, type::yeek, type::vulture};

    // Creature.DamageType, as far as the ports tell them apart
    enum class damage_type : uint8_t {
        blunt,
        stab,
        explosion,
        electric,
        bite,
    };


    // CreatureTemplate.Type of everything ported, the slugcat included: the
    // relationship table's rows and columns (relationships.cpp). The lizard
    // breeds and the centipede, leech and long legs variants in their kinds'
    // enum order (the grapple worm and the kinds ported after it last, in
    // the order they were ported)
    enum class tmpl : uint8_t {
        slugcat,
        pink_lizard, green_lizard, blue_lizard, yellow_lizard, white_lizard, red_lizard, black_lizard, salamander,
        cyan_lizard, spit_lizard, eel_lizard, zoop_lizard, train_lizard, blizzard_lizard, basilisk_lizard, indigo_lizard, peach_lizard,
        centipede, small_centipede, centiwing, red_centipede, aqua_centi,
        leech, sea_leech, jungle_leech,
        cicada_a, cicada_b,
        daddy_long_legs, brother_long_legs, terror_long_legs, hunter_daddy,
        drop_bug,
        tube_worm,
        big_needle_worm, small_needle_worm,
        tentacle_plant, pole_mimic,
        jet_fish,
        scavenger,
        deer,
        miros_bird,
        barnacle,
        egg_bug, fire_bug,
        scavenger_elite, scavenger_disciple,
        garbage_worm,
        lantern_mouse,
        loach,
        vulture, king_vulture, miros_vulture,
        moth_grub,
        overseer,
        snail,
        sky_whale,
        yeek,
        count
    };
    inline constexpr uint32_t tmpl_count = uint32_t(tmpl::count);

    // CreatureTemplate.Relationship.Type (DoesntTrack has no use here)
    enum class rel : uint8_t {
        ignores,
        eats,
        attacks,
        afraid,
        uncomfortable,
        agressive_rival,
        stay_out_of_way,
        antagonizes,
        plays_with,
        social_dependent,
        pack,
    };
    struct relationship {
        rel type = rel::ignores;
        float intensity = 0.f;
        // Relationship.GoForKill
        constexpr bool go_for_kill() const noexcept { return (type == rel::eats || type == rel::attacks) && intensity > 0.f; }
    };

    // A creature's grasps (Creature.grasps[0], [1]): what each holds (a
    // PhysicalObject: a creature's body or an item's) and the chunk it holds
    // it by
    struct grasp_pair {
        dcon::physical_object_id obj[2];
        uint8_t chunk[2] = {0, 0};
    };

    // the AI module a tracked creature is sorted into
    // (IUseARelationshipTracker.ModuleToTrackRelationship)
    enum class track_module : uint8_t { none, prey, threat, aggression };

    // Tracker.CreatureRepresentation with its RelationshipTracker
    // DynamicRelationship, and what the module tracking it keeps of it
    // (PreyTracker.TrackedPrey, ThreatTracker.ThreatCreature's severity,
    // AgressionTracker.AngerRelationship). Positions are in the tracker's
    // room: a creature in another room is out of sight
    struct tracked {
        dcon::creature_id who;
        uint8_t visual = 0;
        track_module module = track_module::none;
        // a kind's TrackedCreatureState (LizardTrackState.spear,
        // CicadaTrackState: armed, caught...), bits
        uint8_t state = 0;
        // the dynamic relationship (currentRelationship)
        relationship rel;
        int32_t age = 0;
        int32_t ticks_since_seen = 0;
        int32_t forget_counter = 0;
        // lastSeenCoord, as the chunk position it was seen at
        rw::vec2 last_seen{0.f, 0.f};
        // PreyTracker.TrackedPrey
        int32_t unreachable_counter = 0;
        int32_t at_position_cant_see = 0;
        rw::ivec2 last_best_guess{-1, -1};
        // AgressionTracker.AngerRelationship
        float anger = 0.f;
        float base_anger = 0.f;
    };
    inline constexpr uint32_t max_tracked = 10;

    // Tracker (maxTrackedCreatures 10 for every ported AI), with the
    // PreyTracker's, ThreatTracker's and AgressionTracker's picks
    struct tracker_state {
        tracked reps[max_tracked];
        uint8_t count = 0;
        // PreyTracker.currentPrey, ThreatTracker.mostThreateningCreature,
        // AgressionTracker.highestAgressionTarget
        dcon::creature_id prey;
        dcon::creature_id threat;
        dcon::creature_id aggression;
        float frustration = 0.f;
        // ThreatTracker
        float current_threat = 0.f;
        int32_t threat_reset_counter = 0;
        int32_t flee_anti_flicker = 0;
        rw::ivec2 flee_dest{-1, -1};
        float highest_anger = 0.f;
    };

    // AbstractRoom.CreatureRoomAttraction
    enum class attraction : uint8_t { neutral, forbidden, avoid, like, stay };
    struct attractions {
        attraction v[tmpl_count] = {};
    };

    // A trip to other rooms (AbstractCreatureAI.path and migrationDestination):
    // the rooms ahead, each with the exit node of the room before it that
    // leads there, the next one first
    struct trip {
        static constexpr uint32_t max_hops = 12;
        uint16_t room[max_hops] = {};
        uint8_t exit[max_hops] = {};
        uint8_t hops = 0;
        uint8_t next = 0;
    };

    // the payload of a kind: what only that kind has, the hottest of it here
    // and its data object's index
    struct slugcat_payload {
        // its Player.cs state and controller (the `player` object)
        uint16_t player;
    };

    struct centipede_payload {
        float size;
        rw::vec2 move_to_pos;
        int32_t change_dir_counter;
        int32_t direction_change_block;
        int32_t no_follow_con_counter;
        float shock_charge;
        float double_grab_charge;
        int32_t shock_give_up_counter;
        // CentipedeAI
        float run;
        float excitement;
        rw::ivec2 temp_idle_pos;
        int32_t annoying_collisions;
        // movement: bodyDirection true means chunk 0 leads
        uint8_t body_direction;
        uint8_t moving;
        // the template (Centiwing, Small, Red, AquaCenti) and Centipede.flying
        centipede::variant variant;
        uint8_t flying;
        // centipede_data (the variants' own state is there)
        uint16_t data;
    };

    struct dropbug_payload {
        int32_t footing_counter;
        int32_t out_of_water_footing;
        float charging;
        float attempt_bite;
        float drop_anticipation;
        float in_ceiling_mode;
        float run_cycle;
        rw::vec2 travel_dir;
        // voiceSound still playing: ticks left of the hiss
        int32_t voice;
        int32_t after_drop_attack_delay;
        int32_t grab_on_next_attack;
        int32_t release_grabbed_counter;
        uint8_t sitting;
        uint8_t swimming;
        uint8_t jumping;
        uint8_t from_ceiling_jump;
        // jumpAtChunk: a creature's chunk, else dropbug_data's
        // jump_at_pos
        dcon::creature_id jump_at;
        uint8_t jump_at_chunk;
        // dropbug_data
        uint16_t data;
    };

    struct cicada_payload {
        float sin_counter;
        int32_t wait_to_fly_counter;
        float flying_power;
        int32_t charge_counter;
        rw::vec2 charge_dir;
        int32_t no_sticky_counter;
        int32_t cant_pick_up_counter;
        float stamina;
        float struggle_against_player;
        float player_jump_boost;
        // Cicada.gender: CicadaA
        uint8_t gender;
        uint8_t flying;
        int8_t flip_h;
        // stickyCling: a creature's chunk
        dcon::creature_id sticky;
        uint8_t sticky_chunk;
        uint8_t cant_pick_up_player;
        uint8_t currently_lifting_player;
        // cicada_data
        uint16_t data;
    };

    struct lizard_payload {
        // Lizard.effectColor
        rw::vec3 effect_color;
        // AbstractCreature.personality
        float dominance;
        float energy;
        // Lizard.spawnDataEvil ({Mean:x})
        float spawn_data_evil;
        // LizardState
        float throat_health;
        lizard::limb_health limb_health;
        lizard::breed breed;
        // lizard_data
        uint16_t data;
    };

    struct daddy_payload {
        // DaddyLongLegs.graphicsSeed (the DaddyGraphics layout's seed)
        int32_t graphics_seed;
        rw::vec3 effect_color;
        rw::vec3 eye_color;
        daddy::variant variant;
        // colorClass (SizeClass and HDmode follow from the variant)
        uint8_t color_class;
        // its parts (creature_part, in vanilla order): the tentacles, then
        // the tubes (the leg graphics, then the dead legs, then the danglers)
        uint8_t tentacle_count;
        uint8_t dead_leg_count;
        uint8_t dangler_count;
        // daddy_data
        uint16_t data;
    };

    struct leech_payload {
        rw::vec2 swim_direction;
        // offShootGoal (has_goal: not null)
        rw::vec2 off_shoot_goal;
        float air_drown;
        float land_walk_cycle;
        int32_t charge_counter;
        int32_t loner_counter;
        // its place in its school (LeechSchool.leeches is in this order)
        uint32_t school_order;
        // the leech whose leech_data keeps its school (creature index)
        uint16_t school;
        int8_t land_walk_dir;
        leech::variant variant;
        // huntPrey
        dcon::creature_id hunt_prey;
        uint8_t has_goal;
        uint8_t flee_from_rain;
        // leech_data
        uint16_t data;
    };

    struct tube_worm_payload {
        // TubeWorm.lungs (it drowns under the water)
        float lungs;
        float goal_on_rope_pos;
        float on_rope_pos;
        float crawl_speed;
        float walk_cycle;
        float last_walk;
        int32_t sleep_counter;
        int32_t tried_move;
        int32_t grappling_counter;
        int32_t shoot_tries;
        // the end it moves (step: its tongue reaches out, the other holds)
        uint8_t step;
        uint8_t sleeping;
        // grabbed by a slugcat last tick (lastGrabbed); its jump button
        // asked for the tongue (useBool)
        uint8_t last_grabbed;
        uint8_t use_bool;
        // tube_worm_data
        uint16_t data;
    };

    struct needle_worm_payload {
        // NeedleWorm: flying, screaming, the getting unstuck terms
        float flying;
        float screaming;
        float extra_movement_force;
        float stuck_at_same_pos;
        float really_stuck_at_same_pos;
        float broken_line_of_sight;
        float segments_stuck_on_terrain;
        float crawl_sin;
        rw::vec2 look_dir;
        int32_t no_shortcuts;
        // NeedleWormAI.flySpeed
        float fly_speed;
        // the template (BigNeedleWorm, SmallNeedleWorm)
        needle_worm::variant variant;
        uint8_t flying_this_frame;
        uint8_t at_dest_this_frame;
        uint8_t last_pos_counter;
        // needle_worm_data
        uint16_t data;
    };

    struct pole_mimic_payload {
        // PoleMimic
        float extended;
        float mimic;
        float get_to_goal_force;
        float force_into_shortcut;
        int32_t wake_up_counter;
        int32_t mimic_delay_counter;
        int32_t hunt_counter;
        int32_t angered_and_aggressive;
        // AbstractCreature.personality.aggression
        float aggression;
        // huntChunk: a creature's chunk
        dcon::creature_id hunt;
        uint8_t hunt_chunk;
        uint8_t tip_attached;
        uint8_t free_standing;
        uint8_t want_to_wake_up;
        uint8_t just_out_of_shortcut;
        // its root is a den's pipe (else it is gone once it withdraws)
        uint8_t has_den;
        // made, not out in a room yet: the den lets it out as a pole
        // (vanilla's PlaceInRoom, not SpitOutOfShortCut)
        uint8_t fresh;
        // pole_mimic_data
        uint16_t data;
    };

    struct tentacle_plant_payload {
        // TentaclePlant: rootPos, stickOutDir, idlePos, attackDir
        rw::vec2 root_pos;
        rw::vec2 stick_out_dir;
        rw::vec2 idle_pos;
        rw::vec2 attack_dir;
        float attack;
        float can_grab;
        float extended;
        float force_into_shortcut;
        // AbstractCreature.personality.aggression
        float aggression;
        // rooted out of a den: ticks it stays drawn in after a threat (its
        // remainInDenCounter in the den it doesn't have)
        int32_t hide;
        // tentacle_plant_data
        uint16_t data;
    };

    struct jetfish_payload {
        // JetFish.swimDir, swimSpeed, jetActive, jetWater,
        // slowDownForPrecision, turnSpeed, diveSpeed, surfSpeed
        rw::vec2 swim_dir;
        float swim_speed;
        float jet_active;
        float jet_water;
        float slow_down_for_precision;
        float turn_speed;
        float dive_speed;
        float surf_speed;
        // JetFish.grabable: a slugcat can pick it up while above 0
        int32_t grabable;
        // JetFishAI.attackCounter, getAwayCounter and getAwayFromCreature
        int32_t attack_counter;
        int32_t get_away_counter;
        dcon::creature_id get_away_from;
        // JetFish.allDry, albino
        uint8_t all_dry;
        uint8_t albino;
        // jetfish_data
        uint16_t data;
    };

    struct scavenger_payload {
        // Scavenger.flip, stuckCounter, commitToMoveCounter, movMode
        float flip;
        int32_t stuck_counter;
        int32_t commit_to_move_counter;
        scavenger::move_mode mov_mode;
        // moving, drop, climbingUpComing
        uint8_t moving;
        uint8_t drop;
        uint8_t climbing_up_coming;
        // which template: Scavenger, ScavengerElite, ScavengerDisciple
        scavenger::variant variant;
        // scavenger_data
        uint16_t data;
    };

    struct deer_payload {
        // Deer.bodDir, moveDirection
        rw::vec2 bod_dir;
        rw::vec2 move_direction;
        float flip_dir;
        float resting;
        float preferred_height;
        float next_floor_height;
        float enter_room_force_push;
        int32_t hesist_counter;
        int32_t eat_counter;
        int32_t violence_reaction;
        uint8_t stay_still;
        uint8_t legs_grabbing;
        uint8_t held_back_by_leg;
        uint8_t worm_grass_below;
        // deer_data
        uint16_t data;
    };

    struct miros_payload {
        // MirosBird
        rw::vec2 move_dir;
        float forward_power;
        float weight_down_to_stand_on_both_legs;
        float body_flip;
        float last_body_flip;
        float jaw_open;
        float last_jaw_open;
        float jaw_vel;
        float keep_jaw_open_pos;
        float run_cycle;
        float last_run_cycle;
        int16_t jaw_slam_pause;
        int16_t jaw_keep_open_pause;
        int16_t stuck_counter;
        // miros_data
        uint16_t data;
    };

    struct barnacle_payload {
        // AbstractCreature.personality.dominance (who climbs over whom)
        float dominance;
        // BarnacleAI.runSpeed, groupDiscomfort, nextGroupDiscomfortTick
        float run_speed;
        float group_discomfort;
        float next_group_discomfort_tick;
        // Barnacle.footingCounter, shakeTime, shakeCooldown,
        // delayBeforeJumping, temporaryDamageImmunity, specialMoveCounter
        int16_t footing_counter;
        int16_t shake_time;
        int16_t shake_cooldown;
        int16_t delay_before_jumping;
        int16_t temporary_damage_immunity;
        int16_t special_move_counter;
        // BarnacleAI.temporaryPanic, runAwayTime
        int16_t temporary_panic;
        int16_t run_away_time;
        // BarnacleAI.leader (its followers name it here; `leading` is every
        // barnacle of the room naming it)
        dcon::creature_id leader;
        // hasShell (State.miscWatcherSaveFlags == 0), BarnacleAI.idleSplash,
        // scattering, nearNarrowPos, Swimming last tick
        uint8_t has_shell;
        uint8_t idle_splash;
        uint8_t scattering;
        uint8_t near_narrow_pos;
        uint8_t swimming;
        // barnacle_data
        uint16_t data;
    };

    struct egg_bug_payload {
        // EggBug.hue, runSpeed, runCycle; EggBugAI.fear; antennaAttention
        float hue;
        float run_speed;
        float run_cycle;
        float fear;
        float antenna_attention;
        rw::vec2 travel_dir;
        // footingCounter, outOfWaterFooting, specialMoveCounter, noJumps,
        // shake, the fire bug's timeWithoutEggs
        int16_t footing_counter;
        int16_t out_of_water_footing;
        int16_t special_move_counter;
        int16_t no_jumps;
        int16_t shake;
        int16_t time_without_eggs;
        // the template (EggBug, FireBug)
        egg_bug::variant variant;
        // eggsLeft, dropEggs, dropSpears, sitting, swimming
        uint8_t eggs_left;
        uint8_t drop_eggs;
        uint8_t drop_spears;
        uint8_t sitting;
        uint8_t swimming;
        // egg_bug_data
        uint16_t data;
    };

    struct garbage_worm_payload {
        // GarbageWorm.rootPos, lookPoint, chargePos (has_charge: not null)
        rw::vec2 root_pos;
        rw::vec2 look_point;
        rw::vec2 charge_pos;
        // GarbageWormState.bodySize; GarbageWorm.extended, retractSpeed
        float body_size;
        float extended;
        float retract_speed;
        // GarbageWorm.hole: its index in the room's garbage holes (-1 none)
        int16_t hole;
        uint8_t has_charge;
        // grabSpears, lastExtended, canBeHitByWeapons
        uint8_t grab_spears;
        uint8_t last_extended;
        uint8_t can_be_hit;
        // garbage_worm_data
        uint16_t data;
    };

    struct lantern_mouse_payload {
        // LanternMouse.runSpeed, ropeLength, runCycle, swingFac, swingFacGetTo,
        // profileFac, ropeStretch, fallAsleep, wakeUp; AirBreatherCreature.lungs
        float run_speed;
        float rope_length;
        float run_cycle;
        float swing_fac;
        float swing_fac_get_to;
        float profile_fac;
        float rope_stretch;
        float fall_asleep;
        float wake_up;
        float lungs;
        // LanternMouse.footingCounter (Footing: over 20)
        int32_t footing_counter;
        // MouseState.battery (4000 new; runs down once dead)
        int32_t battery;
        // specialMoveCounter, ropeInvincible, voiceCounter
        int16_t special_move_counter;
        int16_t rope_invincible;
        int16_t voice_counter;
        // sitting, shrinkingRope, currentlyClimbingCorridor, carried
        uint8_t sitting;
        uint8_t shrinking_rope;
        uint8_t climbing_corridor;
        uint8_t carried;
        // lantern_mouse_data
        uint16_t data;
    };

    struct loach_payload {
        // Loach.bodDir, moveDirection
        rw::vec2 bod_dir;
        rw::vec2 move_direction;
        float flip_dir;
        float resting;
        float preferred_height;
        float next_floor_height;
        float enter_room_force_push;
        float sac_lift_factor;
        float head_lift_factor;
        float height_lerp;
        int16_t hesist_counter;
        int16_t lurch_progress;
        int16_t digesting_counter;
        uint8_t stay_still;
        uint8_t legs_grabbing;
        uint8_t lurching;
        uint8_t held_back_by_leg;
        uint8_t worm_grass_below;
        // Loach or RotLoach
        loach::variant variant;
        // loach_data
        uint16_t data;
    };

    struct moth_grub_payload {
        // MothGrub.localHeadWay, lookAtPoint, lookOffWay
        rw::vec2 local_head_way;
        rw::vec2 look_at_point;
        rw::vec2 look_off_way;
        int32_t look_off_time;
        int32_t look_wiggle;
        // selfRightTimer, selfRightWasTilted, selfRightState
        int16_t self_right_timer;
        int8_t self_right_was_tilted;
        uint8_t self_right_state;
        // AbstractCreature.personality.energy (the graphics)
        float energy;
        // moth_grub_data
        uint16_t data;
    };

    struct overseer_payload {
        // Overseer.hoverTile, rootTile
        rw::ivec2 hover_tile;
        rw::ivec2 root_tile;
        // extended, lastExtended, dying, lastDying, size, zipProgSpeed
        float extended;
        float last_extended;
        float dying;
        float last_dying;
        float size;
        float zip_prog_speed;
        int32_t stationary_counter;
        int16_t conversation_delay;
        // mode, afterWithdrawMode
        overseer::mode mode;
        overseer::mode after_withdraw_mode;
        // zipMeshDirection, leaveRoomOnZipCompletion
        uint8_t zip_mesh_direction;
        uint8_t leave_room_on_zip;
        // OverseerAbstractAI.ownerIterator (the colour), playerGuide
        uint8_t owner_iterator;
        uint8_t guide;
        // in the offscreen den: hidden in its wall until the rain is over
        uint8_t offscreen;
        // Destroy at the end of the dying flicker is queued
        uint8_t destroy_queued;
        // overseer_data
        uint16_t data;
    };

    struct snail_payload {
        // Snail.size, clickCounter, SnailAI.scared
        float size;
        float click_counter;
        float scared;
        // Snail.shellDirection, breathingForce, suckPoint (has_suck_point:
        // not null), dropToTile (has_drop_to_tile)
        rw::vec2 shell_direction;
        rw::vec2 breathing_force;
        rw::vec2 suck_point;
        rw::ivec2 drop_to_tile;
        // Snail.triggerTicker, allowedInNonAccesibleTile
        int16_t trigger_ticker;
        int16_t allowed_in_non_accessible_tile;
        uint8_t has_suck_point;
        uint8_t has_drop_to_tile;
        // Snail.triggered, justClicked, bloated; SnailAI.move (outOfShell);
        // lastOutOfShell
        uint8_t triggered;
        uint8_t just_clicked;
        uint8_t bloated;
        uint8_t move;
        uint8_t last_out_of_shell;
        // snail_data
        uint16_t data;
    };

    struct sky_whale_payload {
        // SkyWhale.flyDir, roll, lastRoll, rollOffset, targetRollOffset,
        // rollOffsetVel, flyingPower
        rw::vec2 fly_dir;
        float roll;
        float last_roll;
        float roll_offset;
        float target_roll_offset;
        float roll_offset_vel;
        float flying_power;
        // CreatureParams.sizeMultiplier
        float size;
        // SkyWhaleAI: timeInRoom, timeToTire, idlePosCounter
        int32_t time_in_room;
        int32_t time_to_tire;
        int32_t idle_pos_counter;
        // off screen (over the room's side, or not in yet): ticks before it
        // flies back in
        int16_t hidden_timer;
        // the AltVariant (superSizeMe); off screen; leaving over the side
        // (tryingToLeaveRoom), which side (-1 left, 1 right, 0 not picked)
        uint8_t alt;
        uint8_t hidden;
        uint8_t leaving;
        int8_t leave_side;
        // SkyWhaleAI.currentNode (-1 none), testNodeIndex
        int8_t current_node;
        uint8_t test_node_index;
        // sky_whale_data
        uint16_t data;
    };

    struct yeek_payload {
        // Yeek.bodyDirection, climbingOrientation, controlledJumpVelocity
        rw::vec2 body_direction;
        rw::vec2 climbing_orientation;
        rw::vec2 controlled_jump_velocity;
        // Yeek.headLeadingCounter, yeekCallCounter, grabCooldown,
        // interestInClimbingPoles, ceilingClearance; YeekAI.fearCounter
        float head_leading_counter;
        float yeek_call_counter;
        float grab_cooldown;
        float interest_in_climbing_poles;
        float ceiling_clearance;
        float fear_counter;
        // Yeek.timeSinceHop, timeSinceJump, maxJumpCounter
        int16_t time_since_hop;
        int16_t time_since_jump;
        int16_t max_jump_counter;
        // Yeek.hardFacingDir, climbingMode, tunnelCrawlingMode, usingStandardMass
        int8_t hard_facing_dir;
        uint8_t climbing;
        uint8_t tunnel;
        uint8_t standard_mass;
        // ticks to the next climbing step (YeekGraphics.CanAdvanceClimb)
        uint8_t climb_step;
        // CreatureState.meatLeft (meatPoints 2); the eating slugcat's Player.eatMeat
        uint8_t meat_left;
        uint8_t eat_meat;
        // yeek_data
        uint16_t data;
    };

    struct vulture_payload {
        // Vulture.moveDirection, jetFuel, wingFlap, wingFlapAmplitude,
        // tuskCharge, jawOpen, lastJawOpen
        rw::vec2 move_direction;
        float jet_fuel;
        float wing_flap;
        float wing_flap_amplitude;
        float tusk_charge;
        float jaw_open;
        float last_jaw_open;
        // snapFrames, laserCounter, landingBrake, stuckShake
        int16_t snap_frames;
        int16_t laser_counter;
        int16_t landing_brake;
        int16_t stuck_shake;
        // the template (Vulture, KingVulture, MirosVulture), VultureState.mask
        vulture::variant variant;
        uint8_t mask;
        uint8_t hover_still;
        uint8_t last_hover_still;
        // its wings (creature_parts, tentacles[i] by their index)
        uint8_t wing_count;
        // vulture_data
        uint16_t data;
    };

    // creature.payload: the struct of the creature's type
    union payload {
        slugcat_payload slugcat;
        centipede_payload centipede;
        dropbug_payload dropbug;
        cicada_payload cicada;
        lizard_payload lizard;
        daddy_payload daddy;
        leech_payload leech;
        tube_worm_payload tube_worm;
        needle_worm_payload needle_worm;
        pole_mimic_payload pole_mimic;
        tentacle_plant_payload tentacle_plant;
        jetfish_payload jetfish;
        scavenger_payload scavenger;
        deer_payload deer;
        miros_payload miros;
        barnacle_payload barnacle;
        egg_bug_payload egg_bug;
        garbage_worm_payload garbage_worm;
        lantern_mouse_payload lantern_mouse;
        loach_payload loach;
        moth_grub_payload moth_grub;
        overseer_payload overseer;
        snail_payload snail;
        sky_whale_payload sky_whale;
        yeek_payload yeek;
        vulture_payload vulture;

        // all zero, like a fresh property of the data container
        payload() noexcept { std::memset(static_cast<void*>(this), 0, sizeof *this); }
    };
    static_assert(sizeof(slugcat_payload) <= 64);
    static_assert(sizeof(centipede_payload) <= 64);
    static_assert(sizeof(dropbug_payload) <= 64);
    static_assert(sizeof(cicada_payload) <= 64);
    static_assert(sizeof(lizard_payload) <= 64);
    static_assert(sizeof(daddy_payload) <= 64);
    static_assert(sizeof(leech_payload) <= 64);
    static_assert(sizeof(tube_worm_payload) <= 64);
    static_assert(sizeof(needle_worm_payload) <= 64);
    static_assert(sizeof(pole_mimic_payload) <= 64);
    static_assert(sizeof(tentacle_plant_payload) <= 64);
    static_assert(sizeof(jetfish_payload) <= 64);
    static_assert(sizeof(scavenger_payload) <= 64);
    static_assert(sizeof(deer_payload) <= 64);
    static_assert(sizeof(miros_payload) <= 64);
    static_assert(sizeof(barnacle_payload) <= 64);
    static_assert(sizeof(egg_bug_payload) <= 64);
    static_assert(sizeof(garbage_worm_payload) <= 64);
    static_assert(sizeof(lantern_mouse_payload) <= 64);
    static_assert(sizeof(loach_payload) <= 64);
    static_assert(sizeof(moth_grub_payload) <= 64);
    static_assert(sizeof(overseer_payload) <= 64);
    static_assert(sizeof(snail_payload) <= 64);
    static_assert(sizeof(sky_whale_payload) <= 64);
    static_assert(sizeof(yeek_payload) <= 64);
    static_assert(sizeof(vulture_payload) <= 64);
    static_assert(sizeof(payload) <= 64);
    static_assert(std::is_trivially_copyable_v<payload>);

    // The kinds of creature_part
    enum class part_kind : uint8_t {
        // DaddyTentacle (Tentacle): its daddy_tentacle data object
        daddy_tentacle,
        // DaddyTubeGraphic: a leg graphic, a dead leg or a dangler
        daddy_tube,
        // DeerTentacle (Tentacle): a rain deer's leg, its deer_leg data object
        deer_leg,
        // LoachLeg / LoachTentacle (Tentacle): a loach's leg or mouth
        // tentacle, its loach_limb data object
        loach_leg,
        loach_tentacle,
        // VultureTentacle (Tentacle): a vulture's wing, its vulture_wing data object
        vulture_wing,
    };

    struct daddy_tentacle_payload {
        // daddy_tentacle
        uint16_t data;
    };

    struct daddy_tube_payload {
        // DaddyDangleTube.Acon/Bcon
        daddy::dangle_connection a_con;
        daddy::dangle_connection b_con;
        // DaddyDeadLeg: deadness, its chunk (bodyParts[].connection), secondaryChunk
        float deadness;
        daddy::tube_kind kind;
        // a leg graphic's tentacle
        uint8_t leg;
        uint8_t chunk;
        uint8_t secondary_chunk;
        // daddy_tube
        uint16_t data;
    };

    struct deer_leg_payload {
        // deer_leg
        uint16_t data;
    };

    struct loach_limb_payload {
        // loach_limb
        uint16_t data;
    };

    struct vulture_wing_payload {
        // vulture_wing
        uint16_t data;
    };

    union part_payload {
        daddy_tentacle_payload daddy_tentacle;
        daddy_tube_payload daddy_tube;
        deer_leg_payload deer_leg;
        loach_limb_payload loach_limb;
        vulture_wing_payload vulture_wing;

        part_payload() noexcept { std::memset(static_cast<void*>(this), 0, sizeof *this); }
    };
    static_assert(sizeof(daddy_tentacle_payload) <= 64);
    static_assert(sizeof(daddy_tube_payload) <= 64);
    static_assert(sizeof(deer_leg_payload) <= 64);
    static_assert(sizeof(loach_limb_payload) <= 64);
    static_assert(sizeof(vulture_wing_payload) <= 64);
    static_assert(sizeof(part_payload) <= 64);
    static_assert(std::is_trivially_copyable_v<part_payload>);
}
