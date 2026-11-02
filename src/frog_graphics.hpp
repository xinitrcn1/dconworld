#pragma once

// Port of FrogGraphics (and its Umbilical): the horns ("LizardScaleA0" over
// "LizardScaleB0"), the two hind legs and the two arms (one "FrogLegXY"
// element each, picked by the limb's stretch), the body (four "Circle20":
// head, rump, the belly's two swelling with the feeds and the croak) and
// the eyes ("tinyStar" and "Circle4" with "pixel" pupils), coloured by
// ApplyPalette (the belly reddening with the blood it drank, the poison's
// tint). The four limbs are Limbs: they step along (FindGrip) while it
// walks, tuck under it sitting, dangle in the air and kick while it is
// latched on. The umbilical, the blood thread from a latched frog to its
// host, is a rope (sprites::add_rope) of 5 to 7 points that falls apart
// once the frog lets go. Vanilla moves a latched frog to the Background
// container; here it keeps its place in the creatures' draw order.

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace frog_graphics {
    // GraphicsModule.Reset when a camera looks at the creature's room again
    void reset_positions(sys::game_state& state, dcon::creature_id c);
    // the graphics module's Update (and the Umbilical's), once per tick
    // after the creature's
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // DrawSprites (ApplyPalette from `pal`), then the umbilical's rope to
    // `ropes`
    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip, std::vector<sprites::rope_point>& ropes);
}
