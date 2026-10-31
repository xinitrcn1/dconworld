#pragma once

// Port of VultureGraphics (with VultureFeather, VultureAppendage and the
// drawing of KingTusks): the danglers under the body, the wings (meshes)
// and their feathers, the shields, the body, the neck, the head with its
// eyes and mask (the king's arrow and neck tubes, its tusks with their
// laser sights and wires; the miros vulture's beak, teeth, eye trail and
// laser), in vanilla sprite order. Purely cosmetic.

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace vulture_graphics {
    // VultureGraphics' constructor: the layout rolled from the creature's
    // seed (once, when the vulture is made), then Reset
    void init(sys::game_state& state, dcon::creature_id c);
    // GraphicsModule.Reset when a camera looks at the creature's room again
    void reset_positions(sys::game_state& state, dcon::creature_id c);
    // the graphics module's Update, once per tick after the creature's
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // VultureGraphics.MakeColorWave (taking off, falling)
    void make_color_wave(sys::game_state& state, dcon::creature_id c, int32_t delay);
    // DrawSprites (ApplyPalette from `pal`); a TriangleMesh.MakeLongMesh's
    // vertices go to `strip`
    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip);
    // Vulture.LaserLight (the miros vulture's): where the beam meets the
    // terrain and the laser's counter (its radius); false without one
    bool laser_light(sys::game_state const& state, dcon::creature_id c, float alpha, rw::vec2& pos, float& counter);
}
