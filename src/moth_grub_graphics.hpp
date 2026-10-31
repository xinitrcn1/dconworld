#pragma once

// Port of MothGrubGraphics: the body as one long mesh wrapped around the
// chunks (two Bézier curves from the tail end to the head, through handles
// beside the body chunks), the mouth (two crossed "pixel" sprites at the
// front end) and four little limbs (long meshes, two a side, fluttering
// with `flapness`), in the body colour lerped towards the palette's black.
// The five chunk sprites ("Circle20") are invisible in vanilla and left out.

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace moth_grub_graphics {
    // GraphicsModule.Reset when a camera looks at the creature's room again
    void reset_positions(sys::game_state& state, dcon::creature_id c);
    // the graphics module's Update, once per tick after the creature's
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // DrawSprites (ApplyPalette from `pal`); the meshes' vertices go to `strip`
    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip);
}
