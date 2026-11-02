#pragma once

// Port of BigEelGraphics: the body (one long mesh over the twenty chunks and
// a 60 segment tail, smoothed, the head widened: drawn as the GPU rope),
// the jagged discs over the chunks, the 6 or 7 pairs of fins (TailSegment
// chains waving with the swim: ropes too), the scales along the sides
// (LizardScaleA4), the jaws (EelJaw sprites on their hydraulic arms,
// "pixel" rods and Circle20 joints by inverse kinematics) and the 40 eyes
// on the head (Cicada8head, Cicada0shield), in vanilla sprite order. The
// EelBody and EelFin shaders aren't ported (docs/DIVERGENCES.md).

#include <vector>

#include "big_eel_types.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace big_eel_graphics {
    // the constructor's layout, rolled from iVars.finsSeed
    big_eel::graphics_layout make_layout(int32_t fins_seed);
    // GraphicsModule.Reset when a camera looks at the creature's room again
    void reset_positions(sys::game_state& state, dcon::creature_id c);
    // the graphics module's Update, once per tick after the creature's
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // DrawSprites (ApplyPalette from `pal`); the body and fins' rope points
    // go to `ropes` (`strip` unused)
    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip, std::vector<sprites::rope_point>& ropes);
}
