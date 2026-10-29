#pragma once

// Items: Rock and Spear (Weapon/PlayerCarryableItem) and the item half of
// the player (Player.GrabUpdate, ThrowObject, PickupCandidate, carrying,
// Spearmaster's needles). Spears stick in walls when thrown straight at them
// (turning their tiles into beams) and bounce off otherwise; only Artificer
// can pull them back out, like vanilla without Remix.

#include <cstdint>
#include <string_view>

#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace items {
    // Rock constructor + Weapon.PlaceInRoom
    dcon::item_id create_rock(sys::game_state& state, dcon::room_id room, rw::vec2 pos);
    // Spear constructor + PlaceInRoom
    dcon::item_id create_spear(sys::game_state& state, dcon::room_id room, rw::vec2 pos);
    // the spear's ends (tail, tip) for drawing, interpolated between ticks
    void spear_ends(sys::game_state const& state, dcon::item_id spear, float alpha, rw::vec2& tail, rw::vec2& tip);
    inline constexpr float spear_length = 44.f;
    void destroy(sys::game_state& state, dcon::item_id item);
    // any item type, placed exactly at `pos`
    dcon::item_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos, world::item_type type);
    // our own colour for a DataPearl type
    rw::vec3 pearl_color(std::string_view name);
    // Room.Loaded on the first realisation: placed rocks from the room file
    // and RandomItemDensity rocks (no-op after the first call for a room)
    void spawn_room_items(sys::game_state& state, dcon::room_id room);
    // Weapon/Rock.Update for every free or thrown item in the room
    void update_room(sys::game_state& state, world::room_view const& room);
    // Player.GrabUpdate (pick up, drop, throw), end of Player.MovementUpdate
    void grab_update(sys::game_state& state, dcon::player_id p, world::room_view const& room);
    // Moves carried items to the hands (Player.GraphicsModuleUpdated, after
    // the player graphics moved the hands)
    void carry_update(sys::game_state& state, dcon::player_id p);
    // the item held in grasps[hand], if any
    dcon::item_id held(sys::game_state const& state, dcon::player_id p, uint32_t hand);
    // Player.SpearOnBack.GraphicsModuleUpdated: the back spear follows the body
    void back_spear_update(sys::game_state& state, dcon::player_id p);
    // Releases every grasp of the player (death, reset)
    void release_all(sys::game_state& state, dcon::player_id p);

    dcon::physical_object_id body(sys::game_state const& state, dcon::item_id item) noexcept;
    dcon::body_chunk_id chunk(sys::game_state const& state, dcon::item_id item) noexcept;
    dcon::player_id grabber(sys::game_state const& state, dcon::item_id item) noexcept;
}
