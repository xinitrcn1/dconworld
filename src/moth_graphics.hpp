#pragma once

// Port of BigMothGraphics (the big and the small moth): in sprite order, the
// wings behind the body (a "Circle20" root and a long mesh each, MothWing
// shader not ported: the vertex colours with their alpha), the legs behind
// it (a "Circle20" knee, the thigh and shin long meshes, a "pixel" tag on a
// tagged leg), the abdomen and the thorax (long meshes in the body colour
// shaded to the depth colour), the legs and wings in front, the tongue (its
// long mesh as one rope of the GPU rope pipeline), the two antennas (two
// long meshes each), the eyes ("Circle20") around the head mesh. What is in
// front swaps with the side the moth faces. The moth drawn as a shadow high
// over the room (shadowMode, the "Shadows" container) isn't drawn.

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace moth_graphics {
    // GraphicsModule.Reset when a camera looks at the creature's room again
    void reset_positions(sys::game_state& state, dcon::creature_id c);
    // the graphics module's Update, once per tick after the creature's
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // DrawSprites (ApplyPalette from `pal`); the meshes' vertices go to
    // `strip`, the tongue's rope points to `ropes`
    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip, std::vector<sprites::rope_point>& ropes);
}
