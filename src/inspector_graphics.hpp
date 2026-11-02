#pragma once

// Port of InspectorGraphics: the core (three pulsing Circle20 in the body
// colour), the mycelia (CoralBrain Mycelium long meshes trailing from the
// body), the wings (CicadaWingA sprites spinning about the body with the
// bodyRotation, flapping with the flight), and per head its neck
// (InspectorHeadRopeGraphics: a RopeGraphic laid along the head's tentacle
// ropes, drawn through the GPU rope pipeline instead of its long mesh),
// the head (Circle20), its four FlyWing jaws and its eye, in vanilla sprite
// order. The OverseerZip and CicadaWing shaders are drawn as plain colours
// (the wings faded by their alpha). The LightSources (the body's light and
// super glow, a headlight per head) are `lights` for render.cpp.

#include <cstdint>
#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace inspector_graphics {
    // GraphicsModule.Reset when a camera looks at the creature's room again
    // (the constructor's layout the first time)
    void reset_positions(sys::game_state& state, dcon::creature_id c);
    // the graphics module's Update, once per tick after the creature's
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // DrawSprites; the mycelia's long meshes go to `strip`, the necks' rope
    // points to `ropes`
    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip, std::vector<sprites::rope_point>& ropes);

    // Inspector.myLight, mySuperGlow (flat), headlights (flat): what
    // Inspector.Update sets them to (Alpha before the palette's darkness)
    struct light {
        rw::vec2 pos{0.f, 0.f};
        float rad = 0.f;
        float alpha = 0.f;
        rw::vec3 color{1.f, 1.f, 1.f};
        bool flat = false;
    };
    inline constexpr uint32_t max_lights = 5;
    // fills `out`, returns how many (none for a dead one gone dark, or out
    // of its room)
    uint32_t lights(sys::game_state const& state, dcon::creature_id c, float alpha, light (&out)[max_lights]);
}
