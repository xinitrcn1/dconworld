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
    struct movement {
        uint8_t resistance[access_count] = {};
        uint8_t swim = 0;
        uint8_t open_air = 0;
        constexpr bool allows(access a) const noexcept { return resistance[uint32_t(a)] != 0; }
        constexpr bool operator==(movement const&) const = default;
    };
    // Centipede: every surface, at one cost
    inline constexpr movement climber{{10, 10, 10, 10, 10, 0, 0}, 0};
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
}
