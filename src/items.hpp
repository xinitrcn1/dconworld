#pragma once

// Items: Rock (Weapon/PlayerCarryableItem) and the item half of the player
// (Player.GrabUpdate, ThrowObject, PickupCandidate, carrying). Spears and
// the other carryables are not ported yet.

#include <cstdint>

#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace items {
    // Rock constructor + Weapon.PlaceInRoom
    dcon::item_id create_rock(sys::game_state& state, dcon::room_id room, rw::vec2 pos);
    void destroy(sys::game_state& state, dcon::item_id item);
    // Room.Loaded on the first realisation: placed rocks from the room file
    // and RandomItemDensity rocks (no-op after the first call for a room)
    void spawn_room_items(sys::game_state& state, dcon::room_id room);
    // Weapon/Rock.Update for every free or thrown item in the room
    void update_room(sys::game_state& state, world::room_view const& room);
    // Player.GrabUpdate (pick up, drop, throw), end of Player.MovementUpdate
    void grab_update(sys::game_state& state, dcon::player_id p, world::room_view const& room);
    // Moves carried items to the hands (Player.Update, after the body moved)
    void carry_update(sys::game_state& state, dcon::player_id p);
    // Where the hand holding grasps[hand] is (stands in for SlugcatHand)
    rw::vec2 hand_position(sys::game_state const& state, dcon::player_id p, uint32_t hand);
    // Releases every grasp of the player (death, reset)
    void release_all(sys::game_state& state, dcon::player_id p);

    dcon::physical_object_id body(sys::game_state const& state, dcon::item_id item) noexcept;
    dcon::body_chunk_id chunk(sys::game_state const& state, dcon::item_id item) noexcept;
    dcon::player_id grabber(sys::game_state const& state, dcon::item_id item) noexcept;
}
