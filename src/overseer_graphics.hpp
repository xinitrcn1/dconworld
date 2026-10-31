#pragma once

// Port of OverseerGraphics: the stalk (a long mesh from the root to the eye,
// a bezier that zips along the zip path while the overseer travels through
// the terrain), the bulb with its white, inner glow and pupil looking at
// what the overseer looks at, the glow, the mycelia (Mycelium, drawn twice
// like vanilla's doubleRenderMycelia) and the HologramMatrix lens it brings
// up when it studies something, in vanilla sprite order. The OverseerZip,
// FlatLight and Hologram shaders are drawn as plain alpha colours (the
// glow as a soft disc).

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace overseer_graphics {
    // the constructor's layout, rolled from the ID's seed: the mycelia, the
    // lens's shape, the movements (made with the creature)
    void setup(sys::game_state& state, dcon::creature_id c, int32_t seed);
    // GraphicsModule.Reset when a camera looks at the creature's room again
    void reset_positions(sys::game_state& state, dcon::creature_id c);
    // OverseerGraphics.Emerge: the mycelia start over at the root
    void emerge(sys::game_state& state, dcon::creature_id c);
    // the graphics module's Update, once per tick after the creature's
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // DrawSprites (ApplyPalette's earthColor from `pal`); the long meshes'
    // vertices go to `strip`
    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip);
    // OverseerGraphics.MainColor (by ownerIterator)
    rw::vec3 main_color(sys::game_state const& state, dcon::creature_id c) noexcept;
}
