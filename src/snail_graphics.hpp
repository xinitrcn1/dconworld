#pragma once

// Port of SnailGraphics: the tail (SnailTail), the two limbs (SnailLimb),
// the head (SnailHead) with its two pixel eyes, and the shell (SnailShellA
// and SnailShellB in the snail's two colours), in vanilla sprite order; the
// limbs step from grip to grip while it crawls, the head peeks out towards
// what makes it uneasy, the shell shakes while it charges and swells on a
// click.

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace snail_graphics {
    // GraphicsModule.Reset when a camera looks at the creature's room again
    void reset_positions(sys::game_state& state, dcon::creature_id c);
    // the graphics module's Update, once per tick after the creature's
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // DrawSprites (ApplyPalette from `pal`)
    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip);
}
