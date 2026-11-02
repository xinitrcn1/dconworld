#pragma once

// Cosmetic room decorations of the Watcher that only the renderer needs,
// kept per viewed room on the render side (the simulation never reads them):
//
//  - FloatingDebris "Rot Islands" (FloatingDebris.RotFloater): floating rot
//    blobs strung together by hanging wires, DynamicLevelElement meshes
//    drawn into the level (gpu::pipeline::dynamic_level).
//  - KarmaFlowerPatch: little golden karma flowers on the floors around the
//    patches, swaying in the room's FlowerWind effect.
//  - the floor bubbles of particles.hpp's bottom_bubble (Bubble's sprite).
//
// The placed objects are read from the room's settings file the first time
// the room is drawn (vanilla makes them when the room is realised).

#include <cstdint>
#include <vector>

#include <glm/mat4x4.hpp>

#include "gamestate.hpp"
#include "gpu.hpp"
#include "math.hpp"
#include "sprites.hpp"

namespace room_decor {
    // Brings the room's decorations up to the simulation's tick (vanilla
    // updates them only while the room is viewed). Call once per frame
    // before the drawing functions below.
    void step(sys::game_state const& s, dcon::room_id room);

    // what the level shaders of the frame share (render.cpp's level_frame)
    struct level_params {
        glm::mat4 proj{1.f};
        rw::vec2 screen{1.f, 1.f};
        rw::vec2 view_pos{0.f, 0.f};
        float zoom = 1.f;
        rw::vec2 light_dir{0.f, 0.f};
        int32_t palette = 0;
        glm::ivec2 effect_colors{-1, -1};
        float night_time = 0.f;
        float rain = 0.f;  // vanilla's _RAIN
        float light = 1.f; // vanilla's _light
        std::vector<rw::vec2> const* cams = nullptr; // the room's camera images' bottom lefts
        gpu::texture cameras = 0, palettes = 0, effects = 0;
    };
    // The Rot Islands of the room, behind the sprites (they are level
    // pixels). Uploads its own vertices.
    void draw_rot_islands(gpu::context& g, sys::game_state const& s, dcon::room_id room, float alpha, level_params const& f);

    // KarmaFlowerPatch.DrawSprites' stems, petals and rings (the Items
    // container). `depth_point`: RoomCamera.ApplyDepth's point (the view's
    // centre); `black`, `fog`: the palette's; `ring_size`: FoodCircleA's
    // element size (FSprite.width/height are set)
    void flower_cmds(sys::game_state const& s, dcon::room_id room, float alpha, rw::vec2 depth_point, rw::vec3 black, rw::vec3 fog, rw::vec2 ring_size,
        std::vector<sprites::draw_cmd>& out, std::vector<sprites::strip_vertex>& strip);

    // Bubble.DrawSprites of the room's bottom_bubble particles (LizardBubble5);
    // `color`: what vanilla's colour marker turns into under the water
    // (RoomPalette.waterColor1)
    void bubble_cmds(sys::game_state const& s, dcon::room_id room, float alpha, rw::vec3 color, std::vector<sprites::draw_cmd>& out);
}
