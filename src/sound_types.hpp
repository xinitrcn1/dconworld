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
        drop_bug_step,
        drop_bug_drop_from_ceiling,
        drop_bug_prepare_jump,
        drop_bug_jump,
        drop_bug_grab_creature,
        drop_bug_voice,
        cicada_wings_takeoff,
        cicada_wings_stop,
        cicada_wings_violent_stop,
        cicada_wings_prepare_bump_attack,
        cicada_wings_start_bump_attack,
        cicada_wings_exit_bump_attack,
        cicada_landing,
        cicada_tentacle_climb,
        cicada_tentacles_grab_player,
        cicada_tentacles_detatch,
        cicada_bump_attack_hit_player,
        cicada_bump_attack_hit_npc,
        cicada_bump_attack_hit_terrain,
        cicada_light_terrain_impact,
        cicada_heavy_terrain_impact,
        slugcat_pick_up_creature,
        slugcat_throw_creature,
        slugcat_lay_down_creature,
        lizard_light_terrain_impact,
        lizard_heavy_terrain_impact,
        lizard_jaws_grab_player,
        lizard_jaws_shut_miss_creature,
        lizard_head_shield_deflect,
        lizard_prepare_to_shoot_tongue,
        lizard_prepare_lunge_attack_init,
        lizard_grab_pole,
        lizard_tongue_attatch_player,
        lizard_tongue_attatch_terrain,
        lizard_tongue_detatch_player,
        lizard_tongue_detatch_npc_or_terrain,
        lizard_tongue_go_back_in_mouth,
        blue_lizard_tongue_shoot_out,
        white_lizard_tongue_shoot_out,
        red_lizard_spit,
        red_lizard_spit_hit_player,
        red_lizard_spit_hit_wall,
        cyan_lizard_prepare_small_jump,
        cyan_lizard_prepare_medium_jump,
        cyan_lizard_prepare_powerful_jump,
        cyan_lizard_small_jump,
        cyan_lizard_medium_jump,
        cyan_lizard_powerful_jump,
        lizard_green_foot_release,
        lizard_bluewhite_foot_release,
        lizard_pinkyellowred_foot_release,
        lizard_voice_pink_a,
        lizard_voice_pink_b,
        lizard_voice_pink_c,
        lizard_voice_pink_d,
        lizard_voice_pink_e,
        lizard_voice_green_a,
        lizard_voice_blue_a,
        daddy_and_bro_tentacle_grab_terrain,
        daddy_and_bro_tentacle_release_terrain,
        daddy_and_bro_tentacle_grab_creature,
        daddy_and_bro_tentacle_release_creature,
        daddy_digestion_init,
        bro_digestion_init,
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
