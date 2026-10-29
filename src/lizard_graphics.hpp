#pragma once

// Port of LizardGraphics' core body: the four (Caramel: six) LizardLimbs
// stepping from grip to grip (their grip is what lets the lizard run, see
// Lizard.GetFrameSpeed), the tail's TailSegments, the head on its neck and
// the tongue's segments, the walk bob, the depth rotation, blinking and the
// head colour flashes, White's camouflage and Black's lighting up, and
// DrawSprites with the game's own elements (Circle20 body circles, the body
// and tail TriangleMeshes, LizardArm_NN / LizardArmColor_NN legs,
// LizardJaw/LowerTeeth/UpperTeeth/Head/Eyes heads in four angles, the
// tongue) in vanilla sprite order (AddToContainer).
//
// The LizardCosmetics (pass 2) are not drawn yet: see the hooks in
// lizard_graphics.cpp (the constructor's AddCosmetic calls, Update and the
// AddToContainer order).

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace lizard_graphics {
    // What DrawSprites and ApplyPalette take from the room and its palette
    struct palette {
        rw::vec3 black{0.f, 0.f, 0.f}; // blackColor
        // what RoomCamera.PixelColorAtCoordinate gives behind the lizard (the
        // White lizard's camouflage picks it up)
        rw::vec3 pick_up{0.5f, 0.5f, 0.5f};
    };

    // the LizardGraphics constructor (and GraphicsModule.Reset)
    void reset(sys::game_state& state, dcon::lizard_id l);
    // GraphicsModule.Reset: every part back on the body
    void reset_positions(sys::game_state& state, dcon::lizard_id l);
    // LizardGraphics.Update, once per tick after the lizard's update
    void update(sys::game_state& state, dcon::lizard_id l, world::room_view const& room);
    // LizardGraphics.DrawSprites (+ ApplyPalette); `alpha` interpolates
    // between ticks
    void build(sys::game_state const& state, dcon::lizard_id l, float alpha, palette const& pal, std::vector<sprites::draw_cmd>& out);
    // the head's colour (HeadColor) and the body's (BodyColor at the hips),
    // for the plain shapes drawn without the game's atlases
    rw::vec3 head_color(sys::game_state const& state, dcon::lizard_id l, float alpha, palette const& pal);
    rw::vec3 body_color(sys::game_state const& state, dcon::lizard_id l, float f, palette const& pal);

    // LizardGraphics.Stun / WhiteFlicker / CreatureSpotted
    void stun(sys::game_state& state, dcon::lizard_id l, int32_t st);
    void white_flicker(sys::game_state& state, dcon::lizard_id l, int32_t fl);
    void creature_spotted(sys::game_state& state, dcon::lizard_id l, bool first_spot, dcon::player_id p);
    // LizardGraphics.HeadRotation
    float head_rotation(sys::game_state const& state, dcon::lizard_id l, float alpha);
}
