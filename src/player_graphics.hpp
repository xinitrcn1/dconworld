#pragma once

// Simplified port of PlayerGraphics: the head, legs and tail body parts and
// the sprite selection of DrawSprites (body, hips, tail mesh, head, legs,
// face), using vanilla element names so DressMySlugcat style atlases drop in.
// Arms/hands (Limb IK), breathing, look targets and class specific extras
// are not ported yet.

#include <cstdint>
#include <string_view>
#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace player_graphics {
    // One FSprite, in room space: rotation is Futile's (degrees, clockwise),
    // anchor is relative to the element size like FSprite.anchorX/anchorY
    struct sprite {
        std::string_view element;
        rw::vec2 pos;
        float rotation = 0.f;
        rw::vec2 scale{1.f, 1.f};
        rw::vec2 anchor{0.5f, 0.5f};
        rw::vec3 color{1.f, 1.f, 1.f};
    };
    // A draw command, in vanilla sprite order: either a sprite or an
    // untextured triangle (the tail TriangleMesh)
    struct draw_cmd {
        bool is_triangle = false;
        sprite spr;
        rw::vec2 tri[3];
        rw::vec3 color{1.f, 1.f, 1.f};
        // per vertex colours with alpha (VertexColorSprite, glows)
        bool vertex_colors = false;
        rw::vec4 vcol[3];
    };
    // the Watcher's Void Weaver decorations (longer tail, fluff, halo, gold eyes)
    void set_weaver(sys::game_state& state, dcon::player_id p, bool on);
    bool weaver(sys::game_state const& state, dcon::player_id p);

    // Puts every body part back on the body (PlayerGraphics.Reset)
    void reset(sys::game_state& state, dcon::player_id p);
    // PlayerGraphics.Update, once per tick after the player update
    void update(sys::game_state& state, dcon::player_id p, world::room_view const& room);
    // PlayerGraphics.ThrowObject: the throwing hand follows the item a moment
    void throw_object(sys::game_state& state, dcon::player_id p, uint32_t hand, dcon::item_id item);
    // where SlugcatHand `hand` is (carried items sit there)
    rw::vec2 hand_position(sys::game_state const& state, dcon::player_id p, uint32_t hand);
    // PlayerGraphics.DrawSprites; `alpha` interpolates between ticks
    void build(sys::game_state const& state, dcon::player_id p, float alpha, std::vector<draw_cmd>& out);
}
