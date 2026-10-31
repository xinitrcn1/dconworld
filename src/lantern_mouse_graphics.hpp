#pragma once

// Port of MouseGraphics: the rope (three CustomFSprite "pixel" quads fading
// from the palette's black to the body colour), the two body sprites
// (mouseBodyA/B), the four legs (mouseFrontLeg/mouseHindLeg), the back
// spots (mouseSpot), the head (mouseHead0..4, Up, Down) and the eyes
// (mouseEyeA/B0..5 and a pixel highlight), in vanilla sprite order, with its
// light modes: charging up on the rope, switching on, flickering, fading
// with the battery. The LightSource it keeps in a dark room is `light`.

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace lantern_mouse_graphics {
    // GraphicsModule.Reset when a camera looks at the creature's room again
    // (and the constructor's first roll of the hanging pose)
    void reset_positions(sys::game_state& state, dcon::creature_id c);
    // MouseGraphics.SpawnAsHanging: already lit when it starts out on a rope
    void spawn_as_hanging(sys::game_state& state, dcon::creature_id c);
    // the graphics module's Update, once per tick after the creature's
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // DrawSprites (ApplyPalette from `pal`; Room.Darkness is the palette's
    // darkness); the rope quads need no strip vertices
    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip);

    // MouseGraphics.lightSource: a LightSource between the head and the
    // rear (30% of the way), its radius, alpha and colour, while the room is
    // dark (Room.Darkness > 0; `darkness` the palette's). False without one
    struct light {
        rw::vec2 pos{0.f, 0.f};
        float rad = 0.f;
        rw::vec3 color{1.f, 1.f, 1.f};
        float alpha = 0.f;
    };
    bool light_of(sys::game_state const& state, dcon::creature_id c, float alpha, float darkness, light& out);

    // the creature side's touches of the graphics module (LanternMouse.Squeak,
    // TerrainImpact, Carried): the ouch eyes, a shove of the head, the body
    // parts jostled, the unconscious profile re-rolled
    void ouch(sys::game_state& state, dcon::creature_id c, int32_t ticks);
    void shove_head(sys::game_state& state, dcon::creature_id c, rw::vec2 by);
    void jostle(sys::game_state& state, dcon::creature_id c, rw::rng& rng);
    void reset_unconsious_profile(sys::game_state& state, dcon::creature_id c);
}
