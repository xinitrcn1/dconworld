#pragma once

// The movement kinds of pathing.hpp, apart so sys::game_state can key its
// path field cache by them

#include <cstdint>

namespace pathing {
    // AItile.Accessibility, as AImapper.FindAccessibilityOfCurrentTile
    // classifies a tile (the Watcher's TerrainManager tiles aside)
    enum class access : uint8_t { floor, corridor, climb, wall, ceiling, air, solid };
    inline constexpr uint32_t access_count = 7;

    // What a creature template may walk on: CreatureTemplate's
    // TileTypeResistances, in tenths (10 is a resistance of 1, 0 forbidden),
    // per accessibility, and whether deep water is open to it (swimmers:
    // `swim` is then the template's waterPathingResistance in tenths).
    // `open_air` is CicadaPather's preference for open space: tenths added
    // to a step per tile its destination is closer than 10 to the terrain
    // (AImap.terrainProximity), away from the goal
    // `mid_air` is the flying centiwing's CentipedeAI.TravelPreference:
    // LerpMap(terrainProximity, 1, 6, mid_air, 0) tenths added to a step
    // onto a tile 2 or more tiles from the terrain (vanilla adds up to 500
    // on top of 1; here up to 50 on top of 10, for the 16 bit costs)
    // `slugcat` is the NPC slugcats' own walking (npc.hpp): standing on
    // the ground, crawling corridors, climbing poles, swimming, dropping
    // off ledges, jumping up to ledges and pouncing over gaps (`jump`s);
    // the resistances are ignored
    struct movement {
        uint8_t resistance[access_count] = {};
        uint8_t swim = 0;
        uint8_t open_air = 0;
        uint8_t mid_air = 0;
        uint8_t slugcat = 0;
        constexpr bool allows(access a) const noexcept { return resistance[uint32_t(a)] != 0; }
        constexpr bool operator==(movement const&) const = default;
    };
    // Centipede: every surface, at one cost (SmallCentipede and
    // RedCentipede inherit it)
    inline constexpr movement climber{{10, 10, 10, 10, 10, 0, 0}, 0};
    // Centiwing: Air at 1, so every open tile. Crawling, TravelPreference
    // adds 1000 to a step onto a tile it wouldn't rather climb than fly
    // over: here the air tiles cost the most a resistance can (25.5);
    // flying, it adds the mid air cost
    inline constexpr movement centiwing_crawl{{10, 10, 10, 10, 10, 255, 0}, 0};
    inline constexpr movement centiwing_fly{{10, 10, 10, 10, 10, 10, 0}, 0, 0, 50};
    // AquaCenti: WaterOnly, swims at 1; floors and corridors Unwanted at 1,
    // poles at 10, walls and ceilings at 100 (Unwanted: only when there is
    // no other way, here the dearest resistances)
    inline constexpr movement aquacenti{{200, 200, 250, 255, 255, 0, 0}, 10};
    // DropBug: floors and corridors at 1, poles at 1.5, walls and ceilings
    // at 3 (it swims on the surface only: AirAndSurface)
    inline constexpr movement drop_bug{{10, 10, 15, 30, 30, 0, 0}, 0};
    // StandardGroundCreature: floors and corridors, poles at 2.5 (what
    // DropBugAI measures exit distances with)
    inline constexpr movement standard_ground{{10, 10, 25, 0, 0, 0, 0}, 0};
    // Cicada: Air at 1, and so everything below it (CreatureTemplate caps
    // the lesser accessibilities at the highest allowed one), through open
    // space (CicadaPather: vanilla adds up to 100 resistance a step near
    // walls on top of 1; here up to 10 on top of 1, so long paths stay inside
    // the fields' 16 bit costs)
    inline constexpr movement cicada{{10, 10, 10, 10, 10, 10, 0}, 0, 10};
    // DaddyLongLegs (and the templates inheriting it): Air at 1, so every
    // open tile at 1; its tentacles find the grips to get there
    inline constexpr movement daddy{{10, 10, 10, 10, 10, 10, 0}, 0, 0};
    // the NPC slugcat: floors, corridors, poles at 1.5, swimming at 2
    inline constexpr movement slugcat{{10, 10, 15, 0, 0, 0, 0}, 20, 0, 0, 1};

    // How the slugcat gets from a tile to the next: a step (walking,
    // crawling, climbing, swimming), letting go above a drop (off a pole,
    // over a ledge's edge), jumping up to a ledge or a pole at most 3
    // tiles up, a running jump or a charge pounce over a gap, a jump from
    // a pole to another one, from a pole's tip (onto the ground, or off a
    // wall back to a pole over it), wall jumps up a narrow shaft, a pipe
    // within the room
    enum class step_kind : uint8_t { walk, drop, rise, leap, pounce, swing, tip, bounce, wall, pipe, vault };
    // A slugcat move other than a plain step or a straight drop, between
    // two tiles of a room (row major indices), its cost in tenths and its
    // side (-1 left, 1 right, 0 straight up)
    struct jump {
        uint32_t from = 0;
        uint32_t to = 0;
        uint16_t cost = 0;
        step_kind kind = step_kind::walk;
        int8_t dir = 0;
    };
}
