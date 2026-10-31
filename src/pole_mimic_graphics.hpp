#pragma once

// Port of PoleMimicGraphics: the stalk (a PoleMimicRopeGraphics strung
// along the tentacle's chunk ropes, drawn as one long mesh in the
// mimicked pole's colour) and its leaf pairs (LizardScaleA scales, the
// first 60% of them with LizardScaleB decorations reddening towards the
// root), which fold flat against the stalk while it looks like a pole and
// open up, flapping, when it wakes.

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace pole_mimic_graphics {
    // InitiateSprites (the leaves and rope segments laid out again) when a
    // camera looks at the creature's room again
    void reset_positions(sys::game_state& state, dcon::creature_id c);
    // the graphics module's Update, once per tick after the creature's
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // DrawSprites (ApplyPalette from `pal`); a TriangleMesh.MakeLongMesh's
    // vertices go to `strip`
    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip, std::vector<sprites::rope_point>& ropes);
}
