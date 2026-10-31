#pragma once

// Port of BarnacleGraphics: six two piece legs (CentipedeLegA/B, Limbs
// finding grips around the shell's underside, paddling under water), the
// body (two JaggedCircle discs), the bare creature's eyes on stalks
// (LizardScaleA5 + pixel) and, with the shell on, its cones (GenerateCones:
// a TriangleMesh and four Circle20 discs each, turned with the body's roll
// and drawn back to front), in vanilla sprite order.

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace barnacle_graphics {
    // GraphicsModule.Reset when a camera looks at the creature's room again
    // (and the constructor's GenerateCones, from the ID's seed, the first time)
    void reset_positions(sys::game_state& state, dcon::creature_id c);
    // the graphics module's Update, once per tick after the creature's
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // DrawSprites (ApplyPalette from `pal`); the cones' meshes go to `strip`
    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip);
}
