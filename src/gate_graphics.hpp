#pragma once

// Port of RegionGateGraphics (with its DoorGraphic and Clamp), the WaterGate's
// two WaterFalls, its SteamSmoke, the ElectricGate's lamps and battery meter:
// the gate room's machinery (cogs, rails, clamps, pillow blocks with their
// arms, the armour plates, the lock segments and bolts, the big screws, the
// poles), the heaters, the water streams and the steam. Purely cosmetic and
// render side: its state lives here (keyed by the gate), stepped to the
// world's tick each frame the gate's room is drawn, and only ever reads the
// simulation (the gate's mode, counters and doors), which never reads it.

#include <string_view>
#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace gate_graphics {
    // One sprite of the gate in draw order: a plain command, or (colored2) a
    // sprite with the ColoredSprite2 shader, which colours each texel from
    // the palette: its blue picks the palette's row 0..2, its red pushes the
    // depth from the sprite's (1 - alpha) towards 1. pal_near are the three
    // rows' colours at the sprite's depth, pal_far at depth 1 (both with the
    // sprite's tint and the fog applied); the renderer mixes them by the red
    struct cmd {
        sprites::draw_cmd c;
        bool colored2 = false;
        rw::vec3 pal_near[3]{};
        rw::vec3 pal_far[3]{};
    };

    // a LightSource of the gate (the ElectricGate's lamps, the WaterGate's
    // heater light): its alpha before the palette's darkness, which counts
    // by `darkness_factor` (LightSource.affectedByPaletteDarkness)
    struct light {
        rw::vec2 pos{0.f, 0.f};
        float rad = 0.f;
        float alpha = 0.f;
        rw::vec3 color{1.f, 1.f, 1.f};
        bool flat = false;
        float darkness_factor = 0.f;
    };

    // an element of the gate atlas: without it the renderer falls back to
    // plain slabs
    inline constexpr std::string_view probe_element = "RegionGate_Cog";

    // Steps the drawing state of the gate in `room` (if any) to the world's
    // tick (made afresh when its room wasn't drawn for a while), then its
    // DrawSprites (ApplyPalette from `pal`) into `out`. `heater_size`:
    // RegionGate_Heater's source size in the atlas. Render thread only
    void build(sys::game_state const& s, world::room_view const& room, float alpha, sprites::room_palette const& pal, rw::vec2 heater_size,
        std::vector<cmd>& out);
    // the gate's LightSources as of the last build (none without one)
    void lights(sys::game_state const& s, dcon::room_id room, std::vector<light>& out);
}
