#pragma once

// Frog state that isn't a plain number of the payload (the Watcher's
// Frog.cs, FrogAI, FrogGraphics, FrogState): what its data object
// (`frog_data`, dcon_generated.txt) keeps. Everything here is trivially
// copyable (no optionals: flags instead).

#include <cstdint>

#include "dcon_generated_ids.hpp"
#include "math.hpp"
#include "pathing_types.hpp"
#include "player_types.hpp"

namespace frog {
    // FrogAI.Behavior
    enum class behavior : uint8_t {
        idle,
        travelling,
        flee,
        hunt,
        return_prey,
        investigate_sound,
        escape_rain,
    };

    // CreatureTemplate's TileTypeResistances (StaticWorld.InitFrog): floors,
    // curved floors, corridors and poles at 1, walls at 50 (the dearest a
    // resistance can be here: 25.5), ceilings and air unallowed;
    // Amphibious, canSwim (waterPathingResistance 1)
    inline constexpr pathing::movement movement{{10, 10, 10, 255, 0, 0, 0}, 10};

    // Frog.maxFeeds; Frog.feedSize (what a feed takes of a slugcat's food)
    inline constexpr int32_t max_feeds = 5;
    inline constexpr int32_t feed_take = 1;
    // Frog.bites at its making; IPlayerEdible.FoodPoints
    inline constexpr int32_t start_bites = 3;
    inline constexpr int32_t food_points = 1;

    // The frog's creature payload (creature_types.hpp's `frog_payload` is
    // this): Frog.cs's hot numbers, FrogState.feedSize, FrogAI.runSpeed
    struct payload {
        // Frog.sitting, struggleDir; FrogAI.runSpeed
        float sitting;
        float struggle_dir;
        float run_speed;
        // Frog.walkDir
        rw::vec2 walk_dir;
        // footingCounter, specialMoveCounter, croakCounter, croakDuration,
        // noJumps, jumpStun, throwLatchGracePeriod, timeAttached,
        // painSoundCooldown, kickUntilDetachAndDead, feedCD
        int16_t footing_counter;
        int16_t special_move_counter;
        int16_t croak_counter;
        int16_t croak_duration;
        int16_t no_jumps;
        int16_t jump_stun;
        int16_t throw_latch_grace;
        int16_t time_attached;
        int16_t pain_sound_cooldown;
        int16_t kick_until_dead;
        int16_t feed_cd;
        // FrogState.feedSize, Frog.bites, attachSide (-1, 1)
        uint8_t feed_size;
        uint8_t bites;
        int8_t attach_side;
        // throwLatch, jumping, walking, KickUntilDetached
        uint8_t throw_latch;
        uint8_t jumping;
        uint8_t walking;
        uint8_t kick_until_detached;
        // what the simulation asks of the graphics module (consumed there):
        // DoneWithJump, MakeUmbilical (makeNewUmbilical)
        uint8_t done_with_jump;
        uint8_t make_new_umbilical;
        // frog_data
        uint16_t data;
    };
    static_assert(sizeof(payload) <= 64);

    // MovementConnection, as far as Frog.Act looks at one: a step of a path
    // field from a tile to the next (`valid` false is
    // default(MovementConnection))
    struct movement_connection {
        rw::ivec2 start{0, 0};
        rw::ivec2 dest{0, 0};
        bool valid = false;
    };

    // Frog's state the payload has no room for
    struct move_state {
        // specialMoveDestination, lastFollowedConnection
        rw::ivec2 special_move_destination{0, 0};
        movement_connection last_followed;
        // ImmuneToLatch, thrownBy (creature ids, none when null)
        dcon::creature_id immune_to_latch;
        dcon::creature_id thrown_by;
        // throwDir, firstFrameTraceFromPos (has_first_frame: not null)
        rw::ivec2 throw_dir{0, 0};
        rw::vec2 first_frame_trace_from{0.f, 0.f};
        uint8_t has_first_frame = 0;
        // Creature.injectedPoison, Frog.injectedPoisonColor
        float injected_poison = 0.f;
        rw::vec3 poison_color{1.f, 1.f, 1.f};
        // the slugcat food count when it last fed off one (currentTargetFood)
        int32_t current_target_food = 0;
    };

    // FrogAI's state
    struct ai_state {
        // panic, lastDistressLength, hunger, fear, rainFear, excitement
        int32_t panic = 0;
        int32_t last_distress_length = 0;
        float hunger = 0.f;
        float fear = 0.f;
        float rain_fear = 0.f;
        float excitement = 0.f;
        // UtilityComparer: the threat tracker's smoothed (weighted) utility
        // (Lerp 0.5 up, Tick 1 / ((lastDistressLength + 20) * 3) down) and
        // the prey tracker's (Lerp 0.15)
        float threat_smoothed = 0.f;
        float prey_smoothed = 0.f;
        // idleCounter, idleSpotWinStreak, forbiddenIdleSpot (has_forbidden),
        // idleRestlessness, unableToFindComfortablePosition
        int32_t idle_counter = 0;
        int32_t idle_spot_win_streak = 0;
        rw::ivec2 forbidden_idle_spot{-1, -1};
        uint8_t has_forbidden = 0;
        int32_t idle_restlessness = 0;
        int32_t unable_to_find_comfortable = 0;
        // packLeader (a frog of the room), findLeaderTick
        dcon::creature_id pack_leader;
        int32_t find_leader_tick = 0;
    };

    // FrogGraphics.GenerateColors and the rest of what its constructor rolls
    // from the ID's seed: bodyColor, bellyColor, eyeColor; horns, hornScale
    struct looks {
        rw::vec3 body{0.5f, 0.3f, 0.2f};
        rw::vec3 belly{0.8f, 0.7f, 0.5f};
        rw::vec3 eye{0.5f, 0.3f, 0.2f};
        int32_t horns = 0;
        float horn_scale = 1.f;
    };

    // FrogGraphics.Umbilical's Segment
    struct umbilical_segment {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        float life = 0.f;
        float life_time = 1.f;
    };
    inline constexpr uint32_t max_umbilical_points = 8;

    // FrogGraphics (purely cosmetic): limbs[4] (legs 0, 1 on the hips, arms
    // 2, 3 on the shoulders), kick[2], roll, smoothedSitting, walkCounter,
    // movingLeg, kickCounter, its CreatureLooker; the Umbilical (a
    // CosmeticSprite of vanilla's room, kept with the frog here)
    struct graphics_state {
        player::hand_state limbs[4];
        rw::vec2 kick[2];
        float roll = 0.f;
        float last_roll = 0.f;
        float smoothed_sitting = 0.f;
        float last_smoothed_sitting = 0.f;
        int32_t walk_counter = 0;
        int32_t moving_leg = 0;
        int32_t kick_counter = 0;
        // CreatureLooker (lookCreature, lookFocusDelay)
        dcon::creature_id look_creature;
        int32_t look_focus_delay = 0;
        // Umbilical: alive (has_umbilical), its points, the creature and the
        // chunk of it it hangs from (bodypartToUse), the room it is in
        uint8_t has_umbilical = 0;
        uint8_t umbilical_count = 0;
        uint8_t umbilical_chunk = 0;
        dcon::creature_id umbilical_host;
        umbilical_segment umbilical[max_umbilical_points];
        // the umbilical was made for the current attachment
        uint8_t umbilical_made = 0;
    };
}
