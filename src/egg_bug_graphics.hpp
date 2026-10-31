#pragma once

// Port of EggBugGraphics: the body (a TriangleMesh along a bezier from the
// head to the tail end), the head (Circle20), the eyes, the two antennae
// (long meshes, the fire bug's zigzagging), the four legs (CentipedeLegA/B
// through Custom.InverseKinematic, gripping the terrain with Limb.FindGrip)
// and the six eggs dangling off the back, behind and in front of the body
// (EggBugEggColor, DangleFruit0A/B, JetFishEyeA; the fire bug's LegsAPole,
// JetFishEyeB and its spines FireBugSpear/FireBugSpearColor once the eggs
// are gone), in vanilla sprite order. EggColors/FireEggColors are shared
// with the eggs it drops (egg_bug_egg.hpp).

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace egg_bug_graphics {
    // GraphicsModule.Reset (and the constructor's body parts): every part
    // back at the chunk it hangs from
    void reset_positions(sys::game_state& state, dcon::creature_id c);
    // EggBugGraphics.Update, once per tick after the creature's
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // DrawSprites (ApplyPalette from `pal`); the TriangleMeshes' vertices go
    // to `strip`
    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip);

    // EggBugGraphics.EggColors (FireEggColors for `fire`): the eggs' three
    // colours (shell, colour, eye) of a hue in a room of that palette
    void egg_colors(rw::vec3 black, rw::vec3 fog, float hue, float darkness, bool fire, rw::vec3 (&out)[3]) noexcept;
}
