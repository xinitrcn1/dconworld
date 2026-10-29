#pragma once

// The creature object's type tag and its payload, and the creature parts'.
//
// Every creature is one `creature` object of the data container: what most
// kinds have (stun, health, the tracked slugcat, the AI's destination...) is
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
// The payloads are defined here, before the data container's id types: they
// keep their data object as its plain index (`data`).

#include <cstdint>
#include <cstring>
#include <type_traits>

#include "centipede_types.hpp"
#include "cicada_types.hpp"
#include "daddy_types.hpp"
#include "dropbug_types.hpp"
#include "leech_types.hpp"
#include "lizard_types.hpp"
#include "math.hpp"

namespace creature {
    // CreatureTemplate.Type of the ported creatures. The order is the update
    // order of a room (and the order every per kind loop used to run in)
    enum class type : uint8_t {
        centipede,
        dropbug,
        cicada,
        lizard,
        daddy,
        leech,
        count
    };
    inline constexpr uint32_t type_count = uint32_t(type::count);
    inline constexpr type all_types[type_count] = {type::centipede, type::dropbug, type::cicada, type::lizard, type::daddy, type::leech};

    // the payload of a kind: what only that kind has, the hottest of it here
    // and its data object's index
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
        // jumpAtChunk: a player's chunk (player index + 1, 0 none), else
        // dropbug_data's jump_at_pos
        uint8_t jump_at_player;
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
        // stickyCling: a player's chunk (player index + 1, 0 none)
        uint8_t sticky_player;
        uint8_t sticky_chunk;
        uint8_t cant_pick_up_player;
        uint8_t currently_lifting_player;
        // grabbedBy: the player holding it (index + 1, 0 none) in its
        // grasps[0] (both hands: TwoHands), and the chunk it holds
        uint8_t held_by;
        uint8_t held_chunk;
        // cicada_data
        uint16_t data;
    };

    struct lizard_payload {
        // Lizard.effectColor
        rw::vec3 effect_color;
        // AbstractCreature.personality
        float dominance;
        float energy;
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
        // huntPrey: a slugcat (index + 1, 0 none)
        uint8_t hunt_prey;
        uint8_t has_goal;
        uint8_t flee_from_rain;
        // leech_data
        uint16_t data;
    };

    // creature.payload: the struct of the creature's type
    union payload {
        centipede_payload centipede;
        dropbug_payload dropbug;
        cicada_payload cicada;
        lizard_payload lizard;
        daddy_payload daddy;
        leech_payload leech;

        // all zero, like a fresh property of the data container
        payload() noexcept { std::memset(static_cast<void*>(this), 0, sizeof *this); }
    };
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
