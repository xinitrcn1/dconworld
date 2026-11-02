#pragma once

// Vanilla's AboveCloudsView (BackgroundScene), the sky scene Room.Loaded
// makes for the AboveCloudsView and PinkSky room effects (the room's own
// Effects line or its settings template's): the day sky illustration, the
// distant buildings (and their lightning), the Watcher's pink horizon fog
// and sun, the close clouds, the distant clouds, the flying clouds and the
// fog over them, each with vanilla's parallax (BackgroundScene.DrawPos
// against the room's place on the region map) and its shader (Background,
// BackgroundAdditive, DistantBkgObject, Cloud, CloudDistant:
// src/shaders/background.frag).
//
// The illustrations are read from the install (illustrations/<name>.png
// through the layered file system) on first use; a missing cloud texture
// falls back on procedural noise, a missing illustration is left out.
// Drawn before the level: the level's sky pixels are transparent, so the
// scene shows exactly where vanilla's BackgroundClipVanilla lets it.
// Everything here is render-side (the lightning's flicker included).

#include <glm/mat4x4.hpp>

#include "gamestate.hpp"
#include "render.hpp"
#include "world.hpp"

namespace render::background_scene {
    struct frame_params {
        glm::mat4 proj{1.f};
        camera cam;
        float view_w = 1.f;      // room pixels the view spans
        float view_h = 1.f;
        int32_t palette = 0;     // the room palette's layer
        float night_time = 0.f;  // towards the rain palette
        float rain = 0.f;        // vanilla's _RAIN
    };

    // Draws `room`'s sky scene, if it has one, into the frame (before the
    // level's camera images); false when the room has none
    bool draw(state& r, sys::game_state const& s, world::room_view const& room, frame_params const& f);
}
