#pragma once

// The water surface of a room, our own take on Water.cs: vanilla runs a 1D
// wave equation along x and draws two rows of it with a sine on top. Here
// the surface is a small height field over x and depth (front to back) that
// propagates and reflects waves off the walls cutting through it, damped
// like a viscous liquid, with a few travelling swells of different lengths
// and directions crossing over it. Bodies entering, leaving or moving along
// the surface push it and throw droplets; rain and drops ripple it.
//
// It is cosmetic (sys::room_runtime::water, never saved): the physics keep
// floating on the room's flat water level, like most of vanilla does. Every
// room with water runs its own, viewed or not.

#include <cstdint>

#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace water {
    // rows of the height field, front (0) to back
    inline constexpr uint32_t rows = 6;
    // how far up the back row is drawn from the front one (the 3D body)
    inline constexpr float depth_px = 18.f;

    // Water.Update for the room's surface (created on first use)
    void update_room(sys::game_state& state, world::room_view const& room);
    // the room's surface, if it has water and was simulated
    sys::game_state::water_surface const* surface(sys::game_state const& state, dcon::room_id room) noexcept;
    // displacement of the surface at `x`, row `z` (0..rows-1, fractional),
    // simulated plus the swells, interpolated between ticks
    float height(sys::game_state const& state, sys::game_state::water_surface const& s, float x, float z, float alpha) noexcept;
    // a push down (positive) or up at `x` over `width` pixels of the front rows
    void disturb(sys::game_state& state, dcon::room_id room, float x, float strength, float width);
}
