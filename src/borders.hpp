#pragma once

// The room's border exits (ShortcutMapper.MapRoomBorders, Room.borderExits):
// the runs of open tiles along the edges of a room where a creature can fly
// or walk in from off screen. SideExits on the left and right edges (no
// water), SkyExits along the top row, SeaExits in water anywhere along the
// border (rooms with water only). Their AbstractRoomNodes come after the
// room's pipes' (exits, dens, region transportation) in the same order;
// entranceWidth is the run's tile count. Shelters and gates have none.
//
// Mapped on demand from the room's tiles (it's a walk around the border,
// three times): offscreen.hpp keeps what it needs from them.

#include <cstdint>
#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace borders {
    // AbstractRoomNode.Type of a border exit
    enum class kind : uint8_t { side, sky, sea };

    // RoomBorderExit
    struct exit {
        kind type = kind::side;
        std::vector<rw::ivec2> tiles;
        // AbstractRoomNode.entranceWidth
        int32_t width() const noexcept { return int32_t(tiles.size()); }
    };

    // ShortcutMapper.ValidBorderTile
    bool valid_tile(world::room_view const& room, rw::ivec2 pos, kind type) noexcept;
    // ShortcutMapper.MapRoomBorders (none for a shelter or a gate)
    std::vector<exit> map(sys::game_state const& state, world::room_view const& room);

    // whether the room has a border exit of `type` at least `min_width` wide
    bool any(sys::game_state const& state, world::room_view const& room, kind type, int32_t min_width);
}
