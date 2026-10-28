#pragma once

// Sound effects and music through SDL3 audio. Sound effects come from
// soundeffects/sounds.txt (SoundLoader's table: sample names, volume and
// pitch ranges) and loadedsoundeffects/*.wav; samples that only exist inside
// the game's Unity asset bundles are skipped. Songs are music/songs/*.mp3/.ogg
// played like MusicPlayer: requested by the room music triggers, faded in and
// out, ended after the trigger's room count.
//
// Each sound effect is its own SDL_AudioStream bound to the device (SDL mixes
// them and resamples for the pitch); songs are streamed from dr_mp3 or
// stb_vorbis a little ahead of playback, from the main thread.

#include <memory>
#include <string>

#include "gamestate.hpp"
#include "math.hpp"

namespace audio {
    struct state;

    struct state_deleter {
        void operator()(state* s) const noexcept;
    };
    using handle = std::unique_ptr<state, state_deleter>;

    // Opens the default playback device; null when there is no audio
    handle create(sys::game_state const& gs);
    // Plays and clears the queued requests. Positional sounds are heard when
    // they are in `viewed_room`, panned and attenuated around `view_center`
    void update(state& a, sys::game_state& gs, dcon::room_id viewed_room, rw::vec2 view_center, rw::vec2 view_size, float dt);
    // Plays a song of the region right away (another one if a song is playing)
    void force_song(state& a, sys::game_state const& gs, dcon::region_id region);
    // Fades the current song out
    void stop_song(state& a);
    // Name of the song playing, empty for none
    std::string current_song(state const& a);
    // Loads every sound effect sample the ported code uses and decodes the
    // first seconds of a song; prints what is missing. Returns false if
    // nothing could be loaded
    bool self_check(state& a, sys::game_state const& gs);
}
