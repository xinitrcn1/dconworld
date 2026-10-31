#pragma once

// Egg bug state that isn't a plain property of the creature (EggBug.cs,
// EggBugAI, EggBugGraphics; FireBug is the same class with Downpour's
// template): what its data object (`egg_bug_data`, dcon_generated.txt)
// keeps, and the state of the eggs it drops (EggBugEgg, FireEgg: the
// `egg_bug_egg_data` object of those items, egg_bug_egg.hpp)

#include <cstdint>
#include <optional>

#include "dcon_generated_ids.hpp"
#include "math.hpp"
#include "pathing_types.hpp"
#include "player_types.hpp"

namespace egg_bug {
    // the template: EggBug, or Downpour's FireBug
    enum class variant : uint8_t {
        egg_bug,
        fire_bug,
    };
    inline constexpr uint32_t variant_count = 2;

    // EggBugAI.Behavior (+ MoreSlugcatsEnums.EggBugBehavior.Kill)
    enum class behavior : uint8_t {
        idle,
        flee,
        hunt,
        escape_rain,
        return_prey,
        kill,
    };

    // CreatureTemplate's TileTypeResistances: EggBug floors and corridors at
    // 1, poles at 2.5, walls at 50 (the dearest a resistance can be here:
    // 25.5); FireBug floors, corridors and walls at 1, poles at 2.5,
    // ceilings at 1.2. Both AirAndSurface: they swim on the surface only
    inline constexpr pathing::movement egg_movement{{10, 10, 25, 255, 0, 0, 0}, 0};
    inline constexpr pathing::movement fire_movement{{10, 10, 25, 10, 12, 0, 0}, 0};

    // MovementConnection, as far as EggBug.Run looks at one: a step of a
    // path field from a tile to the next (`valid` false is
    // default(MovementConnection))
    struct movement_connection {
        rw::ivec2 start{0, 0};
        rw::ivec2 dest{0, 0};
        bool valid = false;
    };

    // EggBug's movement state the payload has no room for
    struct move_state {
        // specialMoveDestination, lastFollowedConnection
        rw::ivec2 special_move_destination{0, 0};
        movement_connection last_followed;
        // awayFromTerrainDir, antennaDir (a point it listens to)
        rw::vec2 away_from_terrain_dir{0.f, 0.f};
        rw::vec2 antenna_dir{0.f, 0.f};
    };

    // EggBugAI's state the payload has no room for
    struct ai_state {
        // idlePosCounter, tempIdlePos, walkWithBug
        int32_t idle_pos_counter = 0;
        rw::ivec2 temp_idle_pos{0, 0};
        std::optional<rw::ivec2> walk_with_bug;
        // noiseRectionDelay
        int32_t noise_reaction_delay = 0;
        // UtilityComparer: the threat tracker's smoothed utility (Lerp 0.5
        // up, Tick 0.005 down) and the prey tracker's (Lerp 0.15)
        float threat_smoothed = 0.f;
        float prey_smoothed = 0.f;
    };

    // FireBug: the spines left where the eggs were (spineExtensions,
    // stabSpine), CarryObject's stabbing (stabTicker, nextStab, stabCount,
    // overdeadTicker)
    struct fire_state {
        float spine_extensions[2][3] = {};
        uint8_t stab_spine[2][3] = {};
        int32_t stab_ticker = 0;
        int32_t next_stab = 30;
        int32_t stab_count = 0;
        int32_t overdead_ticker = 0;
    };

    // EggBugGraphics (purely cosmetic)
    struct graphics_state {
        // tailEnd, antennas[side, 4], legs[side, leg] (Limb), eggs[side, 3]
        player::body_part tail_end;
        player::body_part antennas[2][4];
        player::hand_state legs[2][2];
        player::body_part eggs[2][3];
        float flip = 0.f;
        float last_flip = 0.f;
        rw::vec2 z_rotat{0.f, 1.f};
        rw::vec2 last_z_rotat{0.f, 1.f};
        int32_t legs_dangle_counter = 0;
        float leg_length = 35.f;
        // drawTicker (counted per update here, DrawSprites isn't stateful)
        float draw_ticker = 0.f;
    };

    // EggBugEgg / FireEgg (an item of type egg_bug_egg or fire_egg): the
    // AbstractBugEgg's hue, the swell, the liquid it drips, the bites, the
    // tail segments (segments[i, 0..2]: position, last position, velocity:
    // 5 for EggBugEgg, 3 for FireEgg), the rotation velocity; FireEgg's fuse
    // (activeCounter), thrower, and where it is stuck (a wall point, or a
    // creature's chunk)
    inline constexpr uint32_t max_egg_segments = 5;
    struct egg_state {
        float hue = 0.f;
        float swell = 1.f;
        float liquid = 0.f;
        float liquid_deplete = 60.f;
        float rot_vel = 0.f;
        float darkness = 0.f;
        rw::vec2 seg_pos[max_egg_segments] = {};
        rw::vec2 seg_last[max_egg_segments] = {};
        rw::vec2 seg_vel[max_egg_segments] = {};
        uint8_t segment_count = 5;
        uint8_t bites = 2;
        uint8_t fire = 0;
        // FireEgg: Weapon.Mode StuckInWall / StuckInCreature
        uint8_t stuck_in_wall = 0;
        uint8_t stuck_chunk = 0;
        int32_t active_counter = 0;
        rw::vec2 stuck_pos{0.f, 0.f};
        dcon::creature_id thrown_by;
        dcon::creature_id stuck_in;
    };
}
