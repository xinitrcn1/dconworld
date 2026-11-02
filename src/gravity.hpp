#pragma once

// Zero gravity and changing gravity: the ZeroG and BrokenZeroG room effects
// (AntiGravity, AntiGravity.BrokenAntiGravity).
//
// Room.gravity is 1 unless the room has one of the effects; then every tick
// AntiGravity.Update sets it to 1 - the anti gravity: the ZeroG amount (below
// 1 mapped to 0..0.85) or, for BrokenZeroG, its amount times the region's
// broken cycle's current anti gravity, whichever is more. Everything in the
// room reads the room's gravity (world::room_view::gravity, the room's
// `gravity` property): BodyChunk physics (PhysicalObject.gravity = g *
// room.gravity), buoyancy, the slugcat's ZeroG body mode, the creatures'
// own zero gravity branches.
//
// The broken cycle (the SS rooms' flickering anti gravity): on or off for 8
// to 18 seconds (RainWorld's gravityFlickerCycleMin/Max), each switch
// fading over 3 seconds (an S curve over the later 65% of it) with the
// lights flickering along. Vanilla keeps one on the world's RainCycle; with
// the whole world simulated every region has its own, made the first tick a
// room of the region has the effect, its rolls keyed by the region (no
// shared generator), so the world updates it in any order.
//
// update_world runs once a tick before the rooms update (serially: a loop
// over the regions and the rooms, nothing else), so every room's gravity is
// fixed for the tick before anything reads it.

#include <cstdint>

#include "gamestate.hpp"
#include "gravity_types.hpp"

namespace gravity {
    // RainWorld's setup values gravityFlickerCycleMin / Max (seconds)
    inline constexpr int32_t flicker_cycle_min = 8;
    inline constexpr int32_t flicker_cycle_max = 18;

    // BrokenAntiGravity.CurrentAntiGravity
    float current_anti_gravity(broken_cycle const& c) noexcept;

    // Once a tick, before the rooms: every region's broken cycle
    // (BrokenAntiGravity.Update), then every room with an effect's gravity
    // (AntiGravity.Update)
    void update_world(sys::game_state& state);

    // the room's broken cycle (its region's), nullptr when the room has no
    // BrokenZeroG effect or the cycle isn't made yet
    broken_cycle const* cycle_of(sys::game_state const& state, dcon::room_id room) noexcept;
    // Room.ElectricPower's broken anti gravity part: the flickering lights
    // (1 for rooms without a BrokenZeroG effect)
    float electric_power(sys::game_state const& state, dcon::room_id room) noexcept;
    // RoomCamera's DarkPalette for a room without rain: 1 - the lights of
    // its broken cycle (0 for rooms without a BrokenZeroG effect)
    float dark_palette(sys::game_state const& state, dcon::room_id room) noexcept;
    // Tongue.isZeroGMode's light test: the broken cycle's lights fully on
    // (true for rooms without the effect)
    bool lights_fully_on(sys::game_state const& state, dcon::room_id room) noexcept;
    // a switch is in progress (BrokenAntiGravity.Update's screen shake:
    // the amount, 0 when none) for a viewed room's camera
    float switch_shake(sys::game_state const& state, dcon::room_id room) noexcept;
}
