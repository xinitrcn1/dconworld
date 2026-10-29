#pragma once

// Small cosmetic effects (vanilla's RoomRain drops, WaterDrip, Spark,
// Bubble, the dust of hard landings and the flash of hits), kept in
// sys::game_state::particles and never saved. Hits and landings are spawned
// from the sounds the simulation played this tick, so the gameplay code
// doesn't need to know about them.

#include <cstddef>
#include <cstdint>

#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace particles {
    enum class kind : uint8_t {
        drop,   // a falling water drop (ceiling drips, splash droplets)
        rain,   // a rain streak
        spark,  // a bright fleck off a hard hit
        dust,   // a puff where something landed or pushed off hard
        bubble, // rising under water
        flash,  // a quick ring where something got hit
    };

    void spawn(sys::game_state& state, dcon::room_id room, kind k, rw::vec2 pos, rw::vec2 vel, float size, float seconds, rw::vec3 color, float alpha = 1.f);
    // droplets thrown up by something hitting the water at `pos`
    void splash(sys::game_state& state, dcon::room_id room, rw::vec2 pos, float strength);
    void bubbles(sys::game_state& state, dcon::room_id room, rw::vec2 pos, int count, float spread);

    // effects of the sounds played since `first_sound` (hits, landings, jumps)
    void from_sounds(sys::game_state& state, size_t first_sound);
    // rain, ceiling drips and the swimmers' bubbles of a realised room
    void ambient(sys::game_state& state, world::room_view const& room);
    // moves every particle, once per tick
    void update(sys::game_state& state);
}
