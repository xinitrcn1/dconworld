#pragma once

// Port of DropBugGraphics: the four long legs reaching for grips (or folded
// up to the ceiling spot), their knees, the mandibles, the tail pinchers,
// the antennae, the body's breathing, and DrawSprites with the game's own
// elements (Circle20, pixel, CentipedeLegA/B, CentipedeBackShell) and its
// TriangleMeshes in vanilla sprite order. Its colour fades from the
// palette's black into the camouflage of the ceiling while it sits there.

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace dropbug_graphics {
    // What DrawSprites and ApplyPalette take from the room and its palette
    struct palette {
        rw::vec3 black{0.f, 0.f, 0.f}; // blackColor
        rw::vec3 fog{0.f, 0.f, 0.f};   // fogColor
        float fog_amount = 0.f;
        rw::vec3 camo{0.f, 0.f, 0.f};  // texture.GetPixel(4, 3)
        float darkness = 0.f;
        rw::vec2 light_angle{0.f, 1.f}; // Room.lightAngle
    };

    // the DropBugGraphics constructor (and GraphicsModule.Reset)
    void reset(sys::game_state& state, dcon::dropbug_id d);
    // DropBugGraphics.Update, once per tick after the dropbug's update
    void update(sys::game_state& state, dcon::dropbug_id d, world::room_view const& room);
    // DropBugGraphics.DrawSprites (+ ApplyPalette, RefreshColor); `alpha`
    // interpolates between ticks
    void build(sys::game_state const& state, dcon::dropbug_id d, float alpha, palette const& pal, std::vector<sprites::draw_cmd>& out);
}
