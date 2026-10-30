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
// The LizardCosmetics are lizard_cosmetics.cpp's, hooked in where vanilla
// calls them (the constructor's AddCosmetic calls, Reset, Update and the
// AddToContainer order).

#include <vector>

#include "gamestate.hpp"
#include "lizard_types.hpp"
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
    void reset(sys::game_state& state, dcon::creature_id l);
    // GraphicsModule.Reset: every part back on the body
    void reset_positions(sys::game_state& state, dcon::creature_id l);
    // LizardGraphics.Update, once per tick after the lizard's update
    void update(sys::game_state& state, dcon::creature_id l, world::room_view const& room);
    // LizardGraphics.DrawSprites (+ ApplyPalette); `alpha` interpolates
    // between ticks
    void build(sys::game_state const& state, dcon::creature_id l, float alpha, palette const& pal, std::vector<sprites::draw_cmd>& out);
    // the head's colour (HeadColor) and the body's (BodyColor at the hips),
    // for the plain shapes drawn without the game's atlases
    rw::vec3 head_color(sys::game_state const& state, dcon::creature_id l, float alpha, palette const& pal);
    rw::vec3 body_color(sys::game_state const& state, dcon::creature_id l, float f, palette const& pal);

    // LizardGraphics.Stun / WhiteFlicker / CreatureSpotted
    void stun(sys::game_state& state, dcon::creature_id l, int32_t st);
    void white_flicker(sys::game_state& state, dcon::creature_id l, int32_t fl);
    void creature_spotted(sys::game_state& state, dcon::creature_id l, bool first_spot, dcon::creature_id t);
    // LizardGraphics.HeadRotation
    float head_rotation(sys::game_state const& state, dcon::creature_id l, float alpha);

    // What the LizardCosmetics (lizard_cosmetics.cpp) use of LizardGraphics.
    // `pal` is the lizard's own palette here (ApplyPalette's, which the
    // cosmetics get, with the Blizzard's and Indigo's own blackColor)
    namespace shared {
        // LizardGraphics.LizardSpineData
        struct spine_data {
            float f = 0.f;
            rw::vec2 pos{0.f, 0.f};
            rw::vec2 outer_pos{0.f, 0.f};
            rw::vec2 dir{0.f, 0.f};
            rw::vec2 perp{0.f, 0.f};
            float depth_rotation = 0.f;
            float rad = 0.f;
        };
        // what SpinePosition reads off the lizard at one time stacker (the
        // body positions, radii and tail), gathered once for many calls
        struct spine_frame {
            rw::vec2 body[4];
            float body_rad[3] = {};
            rw::vec2 head{0.f, 0.f};
            float head_rad = 0.f;
            float fatness = 1.f;
            float tail_fatness = 1.f;
            int32_t tail_count = 0;
            rw::vec2 tail[lizard::max_tail];
            float tail_rad[lizard::max_tail] = {}; // StretchedRad
            rw::vec2 tail_end_dir{0.f, 0.f}; // the last segment's (uninterpolated) direction
            float depth_rotation = 0.f;
            float log2_depth = 0.f; // log2 |depth_rotation|, for its per call power
            float tail_length = 0.f;
            float body_length = 0.f; // bodyChunkConnections[0].distance + [1].distance
        };
        spine_frame make_spine_frame(sys::game_state const& state, dcon::creature_id l, float alpha);
        // LizardGraphics.SpinePosition: `s` along the body and tail (0..1)
        spine_data spine_position(spine_frame const& fr, float s);
        // LizardGraphics.BodyAndTailLength
        float body_and_tail_length(lizard::graphics_state const& g);
        // HeadColor, BodyColor, DynamicBodyColor, effectColor, whiteCamoColor
        rw::vec3 head_color(sys::game_state const& state, dcon::creature_id l, float alpha, palette const& pal);
        rw::vec3 body_color(sys::game_state const& state, dcon::creature_id l, float f, palette const& pal);
        rw::vec3 dynamic_body_color(sys::game_state const& state, dcon::creature_id l, palette const& pal);
        rw::vec3 effect_color(sys::game_state const& state, dcon::creature_id l, palette const& pal);
        rw::vec3 white_camo_color(sys::game_state const& state, dcon::creature_id l, palette const& pal);
    }
}
