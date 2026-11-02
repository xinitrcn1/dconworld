#pragma once

// Port of StowawayBugGraphics: the three heads (StowawayRopeGraphics: a
// rope graphic strung along each head's chunk ropes, thick at the body and
// thin to the tip, through the GPU rope pipeline; a TrapHook and a
// mouseBodyA at the tip), the body (mouseBodyB at the maw in the colour of
// the wall behind it), the feelers (ropes fading from the body colour to the
// wall's), the teeth (SpiderLeg0A/B pairs fanning open with the mouth), the
// hood (a long mesh from the ceiling to the maw, two DangleFruit0A lips) and
// the lumps on the ceiling (LizardScaleA6 feelers, KrakenShield0 masses)
// laid out from the creature's seed. Drawn in vanilla's container order:
// the heads (Background), the body, feelers and teeth (Midground), then the
// hood and the lumps (Items).

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace stowaway_graphics {
    // GraphicsModule.Reset (the rope graphics laid from the body to the tips)
    // when a camera looks at the creature's room again
    void reset_positions(sys::game_state& state, dcon::creature_id c);
    // the graphics module's Update, once per tick after the creature's: the
    // rope graphics, the bite's overshoot
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // DrawSprites (ApplyPalette from `pal`); the meshes' vertices go to
    // `strip`, the heads' and feelers' rope points to `ropes`
    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip, std::vector<sprites::rope_point>& ropes);
}
