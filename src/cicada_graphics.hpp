#pragma once

// Port of CicadaGraphics: the four wings (fluttering while it flies, folded
// back along the body while it sits), the four tentacles (dangling in
// flight, gripping the terrain while it climbs, holding on to what it
// clings to or to the slugcat's hands carrying it), the body turning in
// depth (zRotation, picking one of the nine CicadaNbody angles), the eyes
// and blinking, and DrawSprites with the game's own elements (CicadaN
// body/head/shield/eyes1/eyes2, CicadaWingA/B, Circle20) and the tentacle
// TriangleMeshes in vanilla sprite order.

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace cicada_graphics {
    // What ApplyPalette takes from the room's palette
    struct palette {
        rw::vec3 black{0.f, 0.f, 0.f}; // blackColor
        rw::vec3 fog{0.f, 0.f, 0.f};   // fogColor
    };

    // the CicadaGraphics constructor (and GraphicsModule.Reset)
    void reset(sys::game_state& state, dcon::cicada_id c);
    // GraphicsModule.Reset when a camera looks at the creature's room again:
    // the parts back to the body, the look (rolled at creation) kept
    void reset_positions(sys::game_state& state, dcon::cicada_id id);
    // CicadaGraphics.Update, once per tick after the cicada's update
    void update(sys::game_state& state, dcon::cicada_id c, world::room_view const& room);
    // CicadaGraphics.DrawSprites (+ ApplyPalette); `alpha` interpolates
    // between ticks
    void build(sys::game_state const& state, dcon::cicada_id c, float alpha, palette const& pal, std::vector<sprites::draw_cmd>& out);
    // the colours ApplyPalette picks: body, shield (wings), eyes (the plain
    // shapes the renderer draws without the game's atlases use them too)
    void colors(sys::game_state const& state, dcon::cicada_id c, palette const& pal, rw::vec3& body, rw::vec3& shield, rw::vec3& eyes);
}
