#pragma once

// The Watcher's warp points (Watcher.WarpPoint): a tear in a room that, once
// the Watcher stands in it for a moment, pulls everything around into it and
// carries the Watcher (and whatever got pulled along) to another room. One
// way warps only work from their entrance, the exit just receives; ripple
// warps only work from the ripple layer (camouflaged at ripple level 5);
// limited use warps close once used up; warps without a destination pick a
// DynamicWarpTarget room of another region.
//
// The screen wide transition (RoomCamera.warpPointTimer) is
// sys::game_state::warp; render.cpp draws it and the tears.

#include <cstdint>

#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace warps {
    // WarpPoint.PullRadius (no strong pull), creatureSuckRadius
    inline constexpr float pull_radius = 150.f;
    inline constexpr float suck_radius = 1000.f;
    // WarpTear's sprite: Futile_White scaled to an eighth of the screen width
    inline constexpr float tear_radius = 8.f * (1366.f * 0.125f + 8.f);

    // the warp points of a room are realised on its first visit
    void spawn_room(sys::game_state& state, dcon::room_id room);
    // WarpPoint.Update for every warp point of the room
    void update_room(sys::game_state& state, world::room_view const& room);
    // RoomCamera.warpPointTimer, once per tick
    void update_sequence(sys::game_state& state);

    // Creatures travel through warps on their own (migrate.hpp): where a
    // warp leads (a DynamicWarpTarget for one without a destination; no
    // room: nowhere), a room's `k`th warp, its position
    struct target {
        dcon::room_id room;
        rw::vec2 pos{0.f, 0.f};
    };
    target where_to(sys::game_state const& state, dcon::warp_point_id wp) noexcept;
    dcon::warp_point_id point(sys::game_state const& state, dcon::room_id room, uint32_t k) noexcept;
    // it can be taken (not closed, not a one way warp's exit)
    bool leads_away(sys::game_state const& state, dcon::warp_point_id wp) noexcept;
    rw::vec2 position(sys::game_state const& state, dcon::warp_point_id wp) noexcept;
    // `c` waits to go through the warp: one that is ready sets off (its
    // whole sequence) and carries it and what it holds to where it leads,
    // wherever in the room it is; one opening, cooling down or closed
    // carries nothing (cooling down, 3 s of something waiting by it opens
    // it again). A transaction of the warp's room
    void carry(sys::game_state& state, dcon::creature_id c, dcon::warp_point_id wp);

    // The Watcher's dynamic warps (not vanilla's way of making them: its
    // ability held while crouched, player.cpp): a tear at `pos` open for
    // 30 s, leading to a DynamicWarpTarget of another region picked as it
    // opens (whoever follows comes out in the same place), set off by
    // anything that comes close. Serial; none when there is nowhere to go.
    // Not among a room's warps by number (`point`, the route tables)
    inline constexpr int32_t dynamic_life = 30 * 40;
    dcon::warp_point_id spawn_dynamic(sys::game_state& state, dcon::room_id room, rw::vec2 pos, dcon::player_id owner);
    bool is_dynamic(sys::game_state const& state, dcon::warp_point_id wp) noexcept;

    // the slugcat can use warps (only the Watcher)
    bool is_watcher(sys::game_state const& state, dcon::player_id p) noexcept;
    // RainWorldGame.ActiveRippleLayer: 1 while the Watcher is camouflaged at
    // ripple level 5 (the ripple dimension), else 0
    int32_t active_ripple_layer(sys::game_state const& state) noexcept;
    // WarpTear's visible openness (openAnimation * fadeAnim), interpolated
    float openness(sys::game_state const& state, dcon::warp_point_id w, float alpha) noexcept;
    bool one_way_exit(sys::game_state const& state, dcon::warp_placed_id w) noexcept;
    // the warp is on the other layer: drawn as faint ripples only
    bool other_layer(sys::game_state const& state, dcon::warp_point_id w) noexcept;

    // RippleRing
    void spawn_ripple_ring(sys::game_state& state, dcon::room_id room, rw::vec2 pos, float rad, float intensity, float seconds);
    void update_ripple_rings(sys::game_state& state);
}
