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
        centipede_shell, // a shell knocked off a centipede (CentipedeShell)
        explosion_light, // Explosion.ExplosionLight: size the radius, alpha, colour, its life its lifeTime
        flare_light,     // a FlareBomb burning (its LightSource while `burning` runs 0..1): colour
        sand_puff,       // SandPuffSpawner.SandPuff: size its size (0..1); the colour is the terrain palette's dust (render.cpp)
    };

    void spawn(sys::game_state& state, dcon::room_id room, kind k, rw::vec2 pos, rw::vec2 vel, float size, float seconds, rw::vec3 color, float alpha = 1.f);
    // Explosion.ExplosionLight(pos, rad, alpha, lifeTime, color): drawn as
    // vanilla's three sprites (render.cpp's lights)
    void explosion_light(sys::game_state& state, dcon::room_id room, rw::vec2 pos, float rad, float alpha, int32_t life_time, rw::vec3 color);
    // droplets thrown up by something hitting the water at `pos`
    void splash(sys::game_state& state, dcon::room_id room, rw::vec2 pos, float strength);
    void bubbles(sys::game_state& state, dcon::room_id room, rw::vec2 pos, int count, float spread);
    // CentipedeShell: a shell plate tumbling away from `pos`, of the
    // centipede's hue and saturation, scaled like its sprites
    void centipede_shell(sys::game_state& state, dcon::room_id room, rw::vec2 pos, rw::vec2 vel, float hue, float saturation, float scale_x, float scale_y);

    // effects of the sounds played since `first_sound` (hits, landings, jumps)
    void from_sounds(sys::game_state& state, size_t first_sound);
    // rain, ceiling drips and the swimmers' bubbles of a realised room
    void ambient(sys::game_state& state, world::room_view const& room);
    // moves every particle, once per tick
    void update(sys::game_state& state);
}
