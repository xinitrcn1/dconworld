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
        count
    };
    inline constexpr uint32_t type_count = uint32_t(type::count);
    inline constexpr type all_types[type_count] = {type::slugcat, type::centipede, type::dropbug, type::cicada, type::lizard, type::daddy, type::leech};

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
    // enum order
    enum class tmpl : uint8_t {
        slugcat,
        pink_lizard, green_lizard, blue_lizard, yellow_lizard, white_lizard, red_lizard, black_lizard, salamander,
        cyan_lizard, spit_lizard, eel_lizard, zoop_lizard, train_lizard, blizzard_lizard, basilisk_lizard, indigo_lizard, peach_lizard,
        centipede, small_centipede, centiwing, red_centipede, aqua_centi,
        leech, sea_leech, jungle_leech,
        cicada_a, cicada_b,
        daddy_long_legs, brother_long_legs, terror_long_legs, hunter_daddy,
        drop_bug,
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
        // SizeClass, colorClass, HDmode
        uint8_t size_class;
        uint8_t color_class;
        uint8_t hd;
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

    // creature.payload: the struct of the creature's type
    union payload {
        slugcat_payload slugcat;
        centipede_payload centipede;
        dropbug_payload dropbug;
        cicada_payload cicada;
        lizard_payload lizard;
        daddy_payload daddy;
        leech_payload leech;

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
    static_assert(sizeof(payload) <= 64);
    static_assert(std::is_trivially_copyable_v<payload>);

    // The kinds of creature_part
    enum class part_kind : uint8_t {
        // DaddyTentacle (Tentacle): its daddy_tentacle data object
        daddy_tentacle,
        // DaddyTubeGraphic: a leg graphic, a dead leg or a dangler
        daddy_tube,
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

    union part_payload {
        daddy_tentacle_payload daddy_tentacle;
        daddy_tube_payload daddy_tube;

        part_payload() noexcept { std::memset(static_cast<void*>(this), 0, sizeof *this); }
    };
    static_assert(sizeof(daddy_tentacle_payload) <= 64);
    static_assert(sizeof(daddy_tube_payload) <= 64);
    static_assert(sizeof(part_payload) <= 64);
    static_assert(std::is_trivially_copyable_v<part_payload>);
}
