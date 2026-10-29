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
// stb_vorbis a little ahead of playback, from the main thread. Loops (the
// simulation's loop_requests) are streams fed their sample over and over a
// little ahead; the room's ambient sounds (AmbientSoundPlayer) stream their
// clip from loadedsoundeffects_ambient like songs, looping.

#include <memory>
#include <span>
#include <string>
#include <vector>

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
    // No device: what would play is mixed into a buffer, `dump` writes it
    handle create_offline(sys::game_state const& gs);
    // Plays and clears the queued requests and keeps the loops up. Sounds are
    // heard when they are in `viewed_room`, panned and attenuated around
    // `view_center`; the room's spot ambient sounds are heard from
    // `listener` (VirtualMicrophone.listenerPoint: the followed slugcat)
    void update(state& a, sys::game_state& gs, dcon::room_id viewed_room, rw::vec2 view_center, rw::vec2 view_size, rw::vec2 listener, float dt);
    // Plays a song of the region right away (another one if a song is playing)
    void force_song(state& a, sys::game_state const& gs, dcon::region_id region);
    // Fades the current song out
    void stop_song(state& a);
    // Name of the song playing, empty for none
    std::string current_song(state const& a);
    // Loads every sound effect sample the ported code uses, every room's
    // ambient sound clips, and decodes songs; prints what is missing.
    // Returns false if nothing could be loaded
    bool self_check(state& a, sys::game_state const& gs);
    // Offline: writes what was mixed so far as a 16 bit stereo WAV and
    // logs its loudness every half second and the loops' lifetimes
    bool dump(state const& a, char const* path);

    // The loops' bookkeeping, apart from the device (the tests run it)
    struct loop_state {
        sound::loop_request req;
        // ramps up when it starts, down once it stops
        float envelope = 0.f;
        float fade_out = 0.f; // seconds
        bool stopping = false;
        uint32_t voice = 0xFFFFFFFFu; // the device side's voice, if any
    };
    // A new tick's requests against the loops (emitter upkeep): matched by
    // owner, a loop follows its request's volume, pitch and position. One
    // whose owner didn't ask again (turned it off, died, was destroyed, left
    // the room), asked for another sound or room, or is in a room nobody
    // hears starts fading out; a new owner or sound starts a new loop
    // (`started` gets their indices) while there are fewer than `max_loops`
    void upkeep(std::vector<loop_state>& loops, std::span<sound::loop_request const> requests, uint32_t heard_room, size_t max_loops,
        std::vector<size_t>& started);
    // The envelopes `dt` seconds on; loops that faded out are removed and
    // their voices appended to `dropped`
    void fade(std::vector<loop_state>& loops, float dt, std::vector<uint32_t>& dropped);
}
