#pragma once

// Port of MirosBirdGraphics: the body, the two legs (thigh, shin, piston and
// foot), the neck mesh, the head with its two beak halves and their teeth,
// the neck and tail plumage, the eye and its glowing trail (the light
// sources the foot sparks give off aren't ported).

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace miros_graphics {
    // MirosBirdGraphics' constructor: the layout rolled from the creature's
    // seed (once, when the bird is made), then Reset
    void init(sys::game_state& state, dcon::creature_id c);
    // GraphicsModule.Reset when a camera looks at the creature's room again
    void reset_positions(sys::game_state& state, dcon::creature_id c);
    // the graphics module's Update, once per tick after the creature's
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // DrawSprites (ApplyPalette from `pal`); a TriangleMesh.MakeLongMesh's
    // vertices go to `strip`
    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip);
}
