#pragma once

// Port of YeekGraphics (and Yeek.YeekTail, which only the drawing reads):
// the body feathers (LizardScaleA/B by the plumage), the body and first
// tail as one long mesh, the neck, the second tail, the two legs
// (LizardArm_10..40 by their stretch), the head (SnailShellA) and the two
// eyes (Circle20), in vanilla sprite order; the head turns to what it
// fears, the eyes blink and shudder, the legs sit, kick out in a hop, step
// along a pole or a corridor, dangle.

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace yeek_graphics {
    // GraphicsModule.Reset when a camera looks at the creature's room again
    // (the tails' YeekTail.Reset, the legs' YeekLeg.Reset)
    void reset_positions(sys::game_state& state, dcon::creature_id c);
    // the graphics module's Update (and the YeekTails'), once per tick after
    // the creature's
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // DrawSprites (ApplyPalette from `pal`); the TriangleMeshes' vertices go
    // to `strip`
    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip);
}
