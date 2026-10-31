#pragma once

// Port of SkyWhaleGraphics: the body (a TriangleMesh over the snout, the six
// chunks and five tail segments, smoothed into 27 segments) with its
// underside, the two tail flukes, the markings (freckles, scars, the
// AltVariant's pebbles), the eye, the armour plates, five pairs of
// waving fins and the whiskers, in vanilla sprite order; the fins and the
// whiskers on the far side of the roll go behind the body (the two sorted
// FContainers).

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sky_whale_types.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace sky_whale_graphics {
    // the constructor's layout, rolled from the creature's ID.RandomSeed:
    // the segments' radii, the fins, markings, armour and eye
    sky_whale::graphics_layout make_layout(sky_whale::creature_params const& p, bool alt, int32_t seed);
    // GraphicsModule.Reset when a camera looks at the creature's room again
    void reset_positions(sys::game_state& state, dcon::creature_id c);
    // the graphics module's Update, once per tick after the creature's
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // DrawSprites (ApplyPalette from `pal`); the meshes' vertices go to
    // `strip`
    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip);
}
