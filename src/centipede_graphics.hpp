#pragma once

// Port of CentipedeGraphics for every variant: the legs gripping the terrain
// (dangling in flight), the feelers at both ends, the body rolling towards
// the surface it crawls on (back up in flight), the centiwing's and the
// aquapede's flapping wings, and DrawSprites with the game's own elements
// (CentipedeSegment, CentipedeBackShell/BellyShell, CentipedeLegA/B,
// CentipedeWing, AquapedeWing1-4, the red centipede's Cicada8body spikes
// and side plates) in vanilla sprite order; the shells knocked off
// (CentipedeShell.DrawSprites).

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace centipede_graphics {
    // the CentipedeGraphics constructor (and GraphicsModule.Reset)
    void reset(sys::game_state& state, dcon::creature_id c);
    // GraphicsModule.Reset when a camera looks at the creature's room again:
    // the parts back to the body, the look (rolled at creation) kept
    void reset_positions(sys::game_state& state, dcon::creature_id id);
    // CentipedeGraphics.Update, once per tick after the centipede's update
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // CentipedeGraphics.DrawSprites (+ ApplyPalette); `alpha` interpolates
    // between ticks, `black` is the palette's blackColor, `darkness` its darkness
    void build(sys::game_state const& state, dcon::creature_id c, float alpha, rw::vec3 black, float darkness, std::vector<sprites::draw_cmd>& out);
    // CentipedeShell.DrawSprites for a shell particle; `earth` is the
    // palette's fog and black mixed (ApplyPalette's earthColor)
    void build_shell(sys::particle const& p, float alpha, rw::vec3 black, rw::vec3 earth, float darkness, std::vector<sprites::draw_cmd>& out);
}
