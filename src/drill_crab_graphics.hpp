#pragma once

// Port of DrillCrabGraphics, in its sprite order: the two eyes (a stalk, a
// Bézier long mesh from behind the body, and the "pixel" eye at its end,
// drawn in as the crab hides), the faint JaggedCircle blob of the body
// ("Futile_White" at alpha 0.1: a jagged disc), the drill's five sprites
// ("CrabDrillCoverA/B", "CrabDrillBodyA/B", "CrabDrillHead"), its three rings
// of teeth ("SnailTail", turning with drillRotation as the drill spins up and
// shaking with it) and the six legs (a long mesh per segment, its contour
// from LegContour, a "Circle20" at each joint). ApplyPalette's colours: the
// palette's black, lifted 10% and 20% towards white (the tower crab's a
// little rusty, its legs a gradient). The tower crab's depth layer
// (CustomDepth, the TowerCrabLayerer) isn't ported: it is drawn in the play
// layer.

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace drill_crab_graphics {
    // GraphicsModule.Reset when a camera looks at the creature's room again
    void reset_positions(sys::game_state& state, dcon::creature_id c);
    // DrillCrabGraphics.Update, once per tick after the creature's
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // DrawSprites (ApplyPalette from `pal`); the meshes' vertices go to `strip`
    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip);
}
