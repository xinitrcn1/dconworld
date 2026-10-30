#pragma once

// Port of TubeWormGraphics: the short body (four GenericBodyParts: the two
// chunks and a root reaching out along each tongue in use) and the tongues
// as one rope of 20 segments laid along the tongues' Ropes and the body.
// DrawSprites: the body as two TriangleMeshes (the palette's black under
// the worm's colour; the TubeWorm shader's scrolling rings aren't ported, it
// is a flat colour), the rope as a pale pink mesh fading into the fog at
// its ends, and a Circle20 at each tip (with the game's atlases).

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace tube_worm_graphics {
    // what ApplyPalette reads of the room's palette
    struct palette {
        rw::vec3 black{0.f, 0.f, 0.f};
        rw::vec3 fog{0.f, 0.f, 0.f};
    };

    // the TubeWormGraphics constructor (its colour from the creature's seed)
    // and GraphicsModule.Reset
    void reset(sys::game_state& state, dcon::creature_id c);
    // TubeWormGraphics.Reset when a camera looks at the creature's room again
    void reset_positions(sys::game_state& state, dcon::creature_id c);
    // TubeWormGraphics.Update, once per tick after the worm's update (with
    // its Tube_Worm_Inch_Along_Tongue_LOOP)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // TubeWormGraphics.DrawSprites; `tips`: the Circle20 tips too (with
    // the game's atlases)
    void build(sys::game_state const& state, dcon::creature_id c, float alpha, palette const& pal, bool tips, std::vector<sprites::draw_cmd>& out);
}
