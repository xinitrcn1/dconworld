#pragma once

// Port of ScavengerGraphics: the spine of five draw positions (head, neck,
// chest, waist, hips) the body, neck, head, eyes and teeth are drawn along,
// the two ScavengerHands (arms with the game's ScavengerHandA/B) and
// ScavengerLegs, the Eartlers, the tail and the back cosmetic
// (HardBackSpikes or WobblyBackTufts with the LizardScale elements), the
// colours and patterns rolled from the creature's seed like the vanilla
// constructor does. The hands carry the items it holds.

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace scavenger_graphics {
    // ScavengerGraphics' constructor rolls (IndividualVariations,
    // GenerateColors, the cosmetics, the eartlers, the teeth) from the seed:
    // once, when the scavenger is made
    void roll(sys::game_state& state, dcon::creature_id c);
    // GraphicsModule.Reset when a camera looks at the creature's room again
    void reset_positions(sys::game_state& state, dcon::creature_id c);
    // the graphics module's Update, once per tick after the creature's
    // (then Scavenger.GraphicsModuleUpdated: the held items to the hands)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // ScavengerGraphics.ShockReaction
    void shock_reaction(sys::game_state& state, dcon::creature_id c, float intensity);
    // DrawSprites (ApplyPalette from `pal`); a TriangleMesh.MakeLongMesh's
    // vertices go to `strip`
    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip);
}
