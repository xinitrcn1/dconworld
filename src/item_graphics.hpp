#pragma once

// DrawSprites (+ InitiateSprites, ApplyPalette) of the items with the game's
// own elements: Rock (a Pebble and its throw trail), Spear (SmallSpear,
// Spearmaster's BioSpear needles), ExplosiveSpear (SpearRag and the rag's
// mesh), ElectricSpear (the zapper pebbles), DataPearl (JetFishEyeA, the
// tinyStar highlight and the glimmer's light); ScavengerOutpost (the pole,
// the antlers with their skulls) and its PearlStrings.

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace item_graphics {
    // what ApplyPalette reads: RoomPalette.blackColor and darkness, and
    // whether the story is at Saint's time (the pearls grey out)
    struct palette {
        rw::vec3 black{0.f, 0.f, 0.f};
        float darkness = 0.f;
        bool saint = false;
    };

    // the item's sprites in vanilla order (the Items container); false for
    // the item types not ported here
    bool build(sys::game_state const& state, world::room_view const& room, dcon::item_id it, float alpha, palette const& pal,
        std::vector<sprites::draw_cmd>& out);

    // DataPearl's third sprite: the glimmer's light (Futile_White with the
    // FlatLightBehindTerrain shader in the Foreground container); false
    // while it doesn't shine
    struct light {
        rw::vec2 pos{0.f, 0.f};
        float rad = 0.f;
        rw::vec3 color{1.f, 1.f, 1.f};
        float alpha = 0.f;
    };
    bool pearl_light(sys::game_state const& state, dcon::item_id it, float alpha, palette const& pal, light& out);

    // ScavengerOutpost.DrawSprites (the Midground container): the pole, both
    // layers of antlers with their red paint, the three skulls
    void outpost(sys::game_state const& state, dcon::outpost_id o, palette const& pal, std::vector<sprites::draw_cmd>& out);
    // PearlString.DrawSprites of the room's outposts (the Items container)
    void pearl_strings(sys::game_state const& state, dcon::room_id room, float alpha, palette const& pal, std::vector<sprites::draw_cmd>& out);
}
