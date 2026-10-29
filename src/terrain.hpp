#pragma once

// The ground that isn't tiles: the Watcher's sand (TerrainManager's curve
// through the room's TerrainHandles) and mud pits.
//
// The sand is a 1D height map over x, built from the handles the first time
// the room is used and kept for the session only (never saved, never
// re-derived: loading the room again starts from the same dunes). Bodies
// standing or moving on it slowly press it down and push the sand aside, so
// the slugcat leaves marks without sinking in; slopes steeper than sand holds
// slump. During a sandstorm the wind slowly works the dunes back towards a
// smooth train of sine dunes, undoing whatever was pressed into them.
//
// In a mud pit bodies sink slowly and everything drags.

#include <cstdint>

#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace terrain {
    // the room's sand, if it has TerrainHandles (built on first use)
    sys::game_state::sand_field const* sand(sys::game_state& state, dcon::room_id room);
    sys::game_state::sand_field const* sand_if_built(sys::game_state const& state, dcon::room_id room) noexcept;
    float sand_height(sys::game_state::sand_field const& s, float x) noexcept;
    float sand_back(sys::game_state::sand_field const& s, float x) noexcept;
    // keeps a body chunk above the sand, called from BodyChunk.Update after
    // the tile collisions
    void collide_chunk(sys::game_state& state, world::room_view const& room, dcon::body_chunk_id c);
    // sand reshaping, sandstorm and mud, once per tick per realised room
    void update_room(sys::game_state& state, world::room_view const& room);
    // the sandstorm (F6's storm, in rooms with sand)
    bool sandstorm(sys::game_state const& state, dcon::room_id room) noexcept;
}
