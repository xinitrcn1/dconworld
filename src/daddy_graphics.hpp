#pragma once

// Port of DaddyGraphics: the leg graphics (a RopeGraphic laid along each
// tentacle's chunk ropes), the dead legs hanging off the body and the
// dangling tubes slung between body and legs (GenericBodyPart chains), all
// DaddyTubeGraphics with their bumps and little eyes; one Eye per body
// chunk (the jagged bulb with two slits that open, close, focus and look at
// what the tentacles feel and the noises it hears), the Hunter long legs'
// slugcat dummy, and DrawSprites/ApplyPalette in vanilla sprite order with
// the game's own elements (Futile_White, Circle20, CorruptGrad, the
// slugcat's BodyA/HipsA/HeadA0/LegsAAir0/FaceDead).

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace daddy_graphics {
    // What ApplyPalette takes from the room's palette
    struct palette {
        rw::vec3 black{0.f, 0.f, 0.f}; // blackColor
    };

    // the DaddyGraphics constructor: the dead legs, danglers, bumps and eyes
    // rolled from the daddy's graphicsSeed (vanilla random order)
    void create(sys::game_state& state, dcon::daddy_id d);
    // GraphicsModule.Reset
    void reset(sys::game_state& state, dcon::daddy_id d);
    // DaddyGraphics.Update, once per tick after the daddy's update
    void update(sys::game_state& state, dcon::daddy_id d, world::room_view const& room);
    // DaddyGraphics.DrawSprites (+ ApplyPalette); `alpha` interpolates
    // between ticks
    void build(sys::game_state const& state, dcon::daddy_id d, float alpha, palette const& pal, std::vector<sprites::draw_cmd>& out);

    // DaddyGraphics.TentOwnerFeelSomethingWithTentacle (the slugcat's index + 1)
    void feel_something(sys::game_state& state, dcon::daddy_id d, uint8_t player, rw::vec2 feel_pos);
    // DaddyGraphics.ReactToNoise: an eye turns to the noise's source
    void react_to_noise(sys::game_state& state, dcon::daddy_id d, uint16_t source, rw::vec2 noise_pos, float strength);

    // what the plain shapes without the game's atlases draw with:
    // RotBodyColor, a tube's colour at a point along it (OnTubeEffectColorFac),
    // DaddyTubeGraphic.OneDimensionalTubePos / OnTubePos
    rw::vec3 body_color(sys::game_state const& state, dcon::daddy_id d, palette const& pal);
    rw::vec3 tube_color(sys::game_state const& state, dcon::daddy_id d, dcon::daddy_tube_id t, float float_pos, palette const& pal);
    rw::vec2 tube_pos(sys::game_state const& state, dcon::daddy_tube_id t, float float_pos, float alpha);
    rw::vec2 on_tube_pos(sys::game_state const& state, dcon::daddy_tube_id t, rw::vec2 pos, float alpha);
}
