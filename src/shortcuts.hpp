#pragma once

// Shortcut ("pipe") travel: Creature.enteringShortCut suck-in, the
// ShortCutVessel walking the shortcut path, SpitOutOfShortCut, and room to
// room transfers through room exits (ShortcutHandler / AbstractRoom
// connections). Vanilla advances one tile every third tick; here the vessel
// moves continuously at the same speed so the glow can glide along the pipe.

#include <cstdint>

#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace shortcuts {
    // tiles per tick (ShortcutHandler updates every 3rd tick, one tile each)
    inline constexpr float vessel_speed = 1.f / 3.f;

    // Player.MovementUpdate tail: start entering a shortcut the player pushes into
    void check_entrance(sys::game_state& state, dcon::player_id p, world::room_view const& room);
    // Creature.Update while enteringShortCut: pull the body into the hole.
    // Returns true while entering (the regular update is skipped)
    bool update_entering(sys::game_state& state, dcon::player_id p, world::room_view const& room);
    // One tick of the vessel; may move the player to another room
    void update_vessel(sys::game_state& state, dcon::player_id p);
    // Interpolated position of the vessel (room pixels), for the glow/camera
    rw::vec2 vessel_position(sys::game_state const& state, dcon::player_id p, float alpha);
}
