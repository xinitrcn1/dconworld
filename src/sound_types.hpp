#pragma once

// Sound and music requests made by the simulation. The simulation never talks
// to the audio device: it queues requests on the game_state and the audio
// module (audio.*) plays whatever it finds there, so headless runs and tests
// just drop them.

#include <cstdint>
#include <string>

#include "math.hpp"

namespace sound {
    // SoundID: the ones the ported code plays. Names match soundeffects/sounds.txt
    enum class sound_id : uint16_t {
        slugcat_terrain_impact_light,
        slugcat_terrain_impact_medium,
        slugcat_terrain_impact_hard,
        slugcat_terrain_impact_death,
        slugcat_floor_impact_standard,
        slugcat_floor_impact_stealthy,
        slugcat_roll_init,
        slugcat_normal_jump,
        slugcat_flip_jump,
        slugcat_wall_jump,
        slugcat_rocket_jump,
        slugcat_super_jump,
        slugcat_crouch_jump,
        slugcat_from_vertical_pole_jump,
        slugcat_from_horizontal_pole_jump,
        slugcat_belly_slide_init,
        slugcat_throw_rock,
        slugcat_throw_spear,
        slugcat_pick_up_rock,
        slugcat_pick_up_spear,
        slugcat_lay_down_object,
        rock_hit_wall,
        spear_stick_in_wall,
        spear_bounce_off_wall,
        spear_stick_in_ground,
        weapon_skid,
        player_tick_along_in_shortcut,
        gate_poles_and_rails_in,
        gate_rails_collide,
        sm_spear_pull,
        sm_spear_grab,
        slugcat_stash_spear_on_back,
        rock_hit_creature,
        centipede_shock,
        centipede_attach,
        grab_corruption_tube,
        fire_spear_explode,
        count
    };
    char const* to_string(sound_id id) noexcept;

    // Room.PlaySound: `pos` in room pixels of `room` (an invalid room means
    // "not positional", played centred)
    struct sound_request {
        sound_id id = sound_id::count;
        uint32_t room = 0xFFFFFFFFu; // dcon::room_id index
        rw::vec2 pos{0.f, 0.f};
        bool positional = true;
        float vol = 1.f;
        float pitch = 1.f;
    };

    // MusicPlayer.GameRequestsSong / GameRequestsSongStop
    struct music_request {
        bool stop = false;
        std::string song;  // without extension
        float vol = 0.3f;
        float prio = 0.5f;
        float fade_in = 0.f;  // ticks
        float fade_out = 0.f; // ticks, for stops
        int32_t rooms = -1;   // rooms the song lasts, -1 forever
    };
}
