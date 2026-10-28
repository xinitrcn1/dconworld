#include <algorithm>
#include <cstring>
#include <cmath>
#include <optional>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

#include "dr_mp3.h"
#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"

#include "audio.hpp"
#include "fsb5.hpp"
#include "reports.hpp"
#include "text.hpp"
#include "unity_bundle.hpp"

namespace audio {
    using rw::vec2;
    namespace mathf = rw::mathf;

    namespace {
        constexpr int mix_rate = 44100;
        constexpr float sfx_volume = 1.3f;
        constexpr float music_volume = 1.f;
        constexpr size_t max_voices = 48;
        // songs are decoded this far ahead of playback
        constexpr float music_buffer_seconds = 0.75f;
        // a song a trigger played can't be triggered again for this long
        constexpr double song_rest_seconds = 15.0 * 60.0;
        constexpr float ticks_per_second = 40.f;

        struct sample_ref {
            std::string name;
            float min_vol = 1.f, max_vol = 1.f;
            float min_pitch = 1.f, max_pitch = 1.f;
        };
        // one line of sounds.txt
        struct sound_def {
            std::vector<sample_ref> samples;
            bool play_all = false;
            float silent_chance = 0.f;
        };

        // the AudioClips of one of the game's sound bundles, by lowercase name
        struct clip_bank {
            unity::bundle bundle;
            uint64_t resource = 0;
            std::unordered_map<std::string, unity::audio_clip> clips;
            bool open = false;
        };

        struct song {
            std::string name;
            std::optional<layered_fs::mapped_file> file;
            // rebuilt Ogg stream or decoded PCM of a bundled clip
            std::vector<uint8_t> ogg_data;
            std::vector<float> pcm;
            size_t pcm_pos = 0;
            drmp3 mp3{};
            bool is_mp3 = false;
            stb_vorbis* ogg = nullptr;
            int channels = 2;
            int rate = mix_rate;
            SDL_AudioStream* stream = nullptr;
            float gain = 0.f;
            float target = 0.f;
            float fade_speed = 1.f; // gain per second
            float prio = 0.f;
            int32_t rooms_left = -1;
            bool ending = false;
            bool eof = false;

            ~song() {
                if (stream != nullptr)
                    SDL_DestroyAudioStream(stream);
                if (is_mp3)
                    drmp3_uninit(&mp3);
                if (ogg != nullptr)
                    stb_vorbis_close(ogg);
            }
        };
    }

    struct state {
        layered_fs::filesystem const* fs = nullptr;
        SDL_AudioDeviceID device = 0;
        SDL_AudioSpec spec{SDL_AUDIO_F32, 2, mix_rate};
        sound_def defs[size_t(sound::sound_id::count)];
        // lowercase sample name -> interleaved stereo float at mix_rate (empty: missing)
        std::unordered_map<std::string, std::vector<float>> samples;
        std::vector<SDL_AudioStream*> voices;
        std::unique_ptr<song> current;
        std::vector<std::unique_ptr<song>> fading;
        std::unordered_map<std::string, double> last_played; // song -> clock
        clip_bank sfx_bank;
        clip_bank song_bank;
        // sample name -> number of "<name>_<n>" variants (0: the name itself)
        std::unordered_map<std::string, int> variants;
        uint32_t last_room = 0xFFFFFFFFu;
        double clock = 0.0;
        rw::rng random;
        std::vector<float> scratch;
    };

    void state_deleter::operator()(state* s) const noexcept {
        if (s == nullptr)
            return;
        s->current.reset();
        s->fading.clear();
        for (auto* v : s->voices)
            SDL_DestroyAudioStream(v);
        if (s->device != 0)
            SDL_CloseAudioDevice(s->device);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        delete s;
    }

    namespace {
        std::string_view trim(std::string_view sv) {
            while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r'))
                sv.remove_prefix(1);
            while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r'))
                sv.remove_suffix(1);
            return sv;
        }

        template<typename F>
        void split(std::string_view sv, char sep, F&& f) {
            for (size_t p = 0;;) {
                size_t const q = sv.find(sep, p);
                f(sv.substr(p, q == std::string_view::npos ? std::string_view::npos : q - p));
                if (q == std::string_view::npos)
                    return;
                p = q + 1;
            }
        }

        void open_bank(state& a, clip_bank& bank, char const* name) {
            auto const f = a.fs->get_root().get_single_subdir("AssetBundles").get_single_file(name);
            auto of = f.open_file(*a.fs);
            if (!of || !bank.bundle.open(std::move(*of))) {
                reports::add_log(std::string{"audio: can't open the "} + name + " bundle");
                return;
            }
            std::vector<unity::audio_clip> clips;
            if (!unity::audio_clips(bank.bundle, clips, bank.resource)) {
                reports::add_log(std::string{"audio: can't read the "} + name + " bundle's clips");
                return;
            }
            for (auto& c : clips) {
                auto key = text::to_lowercase_native(c.name);
                bank.clips.insert_or_assign(std::move(key), std::move(c));
            }
            bank.open = true;
        }

        // the FSB5 bytes of a bundled clip
        bool clip_data(clip_bank const& bank, std::string const& lname, std::vector<uint8_t>& out, fsb5::info& info) {
            auto const it = bank.clips.find(lname);
            if (it == bank.clips.end())
                return false;
            return bank.bundle.read(bank.resource + it->second.offset, it->second.size, out)
                && fsb5::parse(out.data(), out.size(), info);
        }

        // SoundLoader's table: "Name[/FLAGS] : sample/vol=x/minPitch=y, sample2..."
        void load_sound_table(state& a) {
            auto const f = a.fs->get_root().get_single_subdir("soundeffects").get_single_file("sounds.txt");
            auto const of = f.open_file(*a.fs);
            if (!of) {
                reports::add_log("audio: no soundeffects/sounds.txt");
                return;
            }
            std::unordered_map<std::string, sound_def> table;
            split(of->to_string_view(), '\n', [&](std::string_view line) {
                line = trim(line);
                if (line.empty() || line.starts_with("//"))
                    return;
                size_t const colon = line.find(':');
                if (colon == std::string_view::npos)
                    return;
                sound_def d;
                std::string name;
                bool first = true;
                split(trim(line.substr(0, colon)), '/', [&](std::string_view tok) {
                    tok = trim(tok);
                    if (first) {
                        name = text::to_lowercase_native(std::string{tok});
                        first = false;
                    } else if (tok == "PLAYALL") {
                        d.play_all = true;
                    } else if (tok.starts_with("silentChance=")) {
                        d.silent_chance = std::strtof(std::string{tok.substr(13)}.c_str(), nullptr);
                    }
                });
                split(line.substr(colon + 1), ',', [&](std::string_view entry) {
                    entry = trim(entry);
                    if (entry.empty())
                        return;
                    sample_ref s;
                    bool head = true;
                    split(entry, '/', [&](std::string_view tok) {
                        tok = trim(tok);
                        size_t const eq = tok.find('=');
                        if (head) {
                            s.name = text::to_lowercase_native(std::string{tok});
                            head = false;
                            return;
                        }
                        if (eq == std::string_view::npos)
                            return;
                        auto const key = tok.substr(0, eq);
                        float const v = std::strtof(std::string{tok.substr(eq + 1)}.c_str(), nullptr);
                        if (key == "vol") s.min_vol = s.max_vol = v;
                        else if (key == "minVol") s.min_vol = v;
                        else if (key == "maxVol") s.max_vol = v;
                        else if (key == "pitch") s.min_pitch = s.max_pitch = v;
                        else if (key == "minPitch") s.min_pitch = v;
                        else if (key == "maxPitch") s.max_pitch = v;
                    });
                    if (!s.name.empty())
                        d.samples.push_back(std::move(s));
                });
                if (!name.empty() && !d.samples.empty())
                    table.insert_or_assign(std::move(name), std::move(d));
            });
            for (size_t i = 0; i < size_t(sound::sound_id::count); ++i) {
                auto const it = table.find(text::to_lowercase_native(std::string{sound::to_string(sound::sound_id(i))}));
                if (it != table.end())
                    a.defs[i] = it->second;
            }
        }

        std::vector<float> const& sample(state& a, std::string const& name) {
            if (auto const it = a.samples.find(name); it != a.samples.end())
                return it->second;
            std::vector<float> out;
            auto const f = a.fs->get_root().get_single_subdir("loadedsoundeffects").get_single_file(name + ".wav");
            auto const of = f.open_file(*a.fs);
            // the game's own loose files are empty placeholders (mods may not be)
            if (of && of->size > 0) {
                SDL_AudioSpec src{};
                Uint8* buf = nullptr;
                Uint32 len = 0;
                auto* io = SDL_IOFromConstMem(of->base, of->size);
                bool const ok = io != nullptr && SDL_LoadWAV_IO(io, true, &src, &buf, &len);
                if (!ok)
                    reports::add_log("audio: can't load \"" + f.path + "\": " + SDL_GetError());
                if (ok) {
                    Uint8* conv = nullptr;
                    int conv_len = 0;
                    if (SDL_ConvertAudioSamples(&src, buf, int(len), &a.spec, &conv, &conv_len)) {
                        out.assign(reinterpret_cast<float*>(conv), reinterpret_cast<float*>(conv) + conv_len / int(sizeof(float)));
                        SDL_free(conv);
                    }
                    SDL_free(buf);
                }
            } else {
                std::vector<uint8_t> raw;
                fsb5::info info;
                std::vector<float> pcm;
                if (clip_data(a.sfx_bank, name, raw, info) && fsb5::decode(raw.data(), raw.size(), info, pcm)) {
                    SDL_AudioSpec const src{SDL_AUDIO_F32, info.channels, info.rate};
                    Uint8* conv = nullptr;
                    int conv_len = 0;
                    if (SDL_ConvertAudioSamples(&src, reinterpret_cast<Uint8 const*>(pcm.data()), int(pcm.size() * sizeof(float)), &a.spec, &conv, &conv_len)) {
                        out.assign(reinterpret_cast<float*>(conv), reinterpret_cast<float*>(conv) + conv_len / int(sizeof(float)));
                        SDL_free(conv);
                    }
                }
            }
            return a.samples.emplace(name, std::move(out)).first->second;
        }

        // SoundLoader: a sample may come as "<name>_1".."<name>_n", one is
        // picked at random each time
        std::string pick_variant(state& a, std::string const& name) {
            auto it = a.variants.find(name);
            if (it == a.variants.end()) {
                int n = 0;
                if (!a.sfx_bank.clips.contains(name))
                    while (a.sfx_bank.clips.contains(name + "_" + std::to_string(n + 1)))
                        ++n;
                it = a.variants.emplace(name, n).first;
            }
            if (it->second == 0)
                return name;
            return name + "_" + std::to_string(a.random.range(1, it->second + 1));
        }

        void play_voice(state& a, std::vector<float> const& data, float gain, float pan, float pitch) {
            if (data.empty() || gain <= 0.001f)
                return;
            if (a.voices.size() >= max_voices) {
                SDL_DestroyAudioStream(a.voices.front());
                a.voices.erase(a.voices.begin());
            }
            auto* s = SDL_CreateAudioStream(&a.spec, &a.spec);
            if (s == nullptr)
                return;
            // equal power pan baked into the stereo data
            float const l = std::sqrt(0.5f * (1.f - pan)) * 1.41421356f;
            float const r = std::sqrt(0.5f * (1.f + pan)) * 1.41421356f;
            a.scratch.resize(data.size());
            for (size_t i = 0; i + 1 < data.size(); i += 2) {
                a.scratch[i] = data[i] * mathf::min(l, 1.f);
                a.scratch[i + 1] = data[i + 1] * mathf::min(r, 1.f);
            }
            SDL_SetAudioStreamFrequencyRatio(s, mathf::clamp(pitch, 0.1f, 10.f));
            SDL_SetAudioStreamGain(s, gain);
            SDL_PutAudioStreamData(s, a.scratch.data(), int(a.scratch.size() * sizeof(float)));
            SDL_FlushAudioStream(s);
            SDL_BindAudioStream(a.device, s);
            a.voices.push_back(s);
        }

        void play_request(state& a, sound::sound_request const& r, uint32_t viewed_room, vec2 center, vec2 size) {
            auto const& d = a.defs[size_t(r.id)];
            if (d.samples.empty())
                return;
            if (d.silent_chance > 0.f && a.random.value() < d.silent_chance)
                return;
            float gain = r.vol * sfx_volume;
            float pan = 0.f;
            if (r.positional && r.room != 0xFFFFFFFFu) {
                if (r.room != viewed_room)
                    return;
                pan = mathf::clamp((r.pos.x - center.x) / (size.x * 0.5f), -1.f, 1.f) * 0.75f;
                // fades out past the edges of the screen
                float const dx = mathf::max(0.f, std::fabs(r.pos.x - center.x) - size.x * 0.5f);
                float const dy = mathf::max(0.f, std::fabs(r.pos.y - center.y) - size.y * 0.5f);
                gain *= mathf::clamp01(1.f - std::sqrt(dx * dx + dy * dy) / 700.f);
            }
            auto const one = [&](sample_ref const& s) {
                float const v = mathf::lerp(s.min_vol, s.max_vol, a.random.value());
                float const p = mathf::lerp(s.min_pitch, s.max_pitch, a.random.value());
                play_voice(a, sample(a, pick_variant(a, s.name)), gain * v, pan, p * r.pitch);
            };
            if (d.play_all) {
                for (auto const& s : d.samples)
                    one(s);
            } else {
                one(d.samples[size_t(a.random.range(0, int32_t(d.samples.size())))]);
            }
        }

        std::unique_ptr<song> open_song(state& a, std::string const& name) {
            auto const dir = a.fs->get_root().get_single_subdir("music").get_single_subdir("songs");
            auto sg = std::make_unique<song>();
            sg->name = name;
            for (char const* ext : {".mp3", ".ogg"}) {
                auto of = dir.get_single_file(name + ext).open_file(*a.fs);
                if (!of || of->size == 0)
                    continue;
                sg->file = std::move(of);
                if (ext[1] == 'm') {
                    if (!drmp3_init_memory(&sg->mp3, sg->file->base, sg->file->size, nullptr))
                        return nullptr;
                    sg->is_mp3 = true;
                    sg->channels = int(sg->mp3.channels);
                    sg->rate = int(sg->mp3.sampleRate);
                } else {
                    int err = 0;
                    sg->ogg = stb_vorbis_open_memory(static_cast<unsigned char const*>(sg->file->base), int(sg->file->size), &err, nullptr);
                    if (sg->ogg == nullptr)
                        return nullptr;
                    auto const info = stb_vorbis_get_info(sg->ogg);
                    sg->channels = info.channels;
                    sg->rate = int(info.sample_rate);
                }
                SDL_AudioSpec const src{SDL_AUDIO_F32, sg->channels, sg->rate};
                sg->stream = SDL_CreateAudioStream(&src, &a.spec);
                if (sg->stream == nullptr)
                    return nullptr;
                SDL_SetAudioStreamGain(sg->stream, 0.f);
                SDL_BindAudioStream(a.device, sg->stream);
                return sg;
            }
            // bundled (the loose files are empty)
            std::vector<uint8_t> raw;
            fsb5::info info;
            if (!clip_data(a.song_bank, text::to_lowercase_native(name), raw, info)) {
                reports::add_log("audio: no song \"" + name + "\"");
                return nullptr;
            }
            sg->file.reset();
            if (info.mode == 15) {
                if (!fsb5::to_ogg(raw.data(), raw.size(), info, sg->ogg_data))
                    return nullptr;
                int err = 0;
                sg->ogg = stb_vorbis_open_memory(sg->ogg_data.data(), int(sg->ogg_data.size()), &err, nullptr);
                if (sg->ogg == nullptr) {
                    reports::add_log("audio: can't decode song \"" + name + "\"");
                    return nullptr;
                }
            } else if (!fsb5::decode(raw.data(), raw.size(), info, sg->pcm)) {
                return nullptr;
            }
            sg->channels = info.channels;
            sg->rate = info.rate;
            SDL_AudioSpec const src{SDL_AUDIO_F32, sg->channels, sg->rate};
            sg->stream = SDL_CreateAudioStream(&src, &a.spec);
            if (sg->stream == nullptr)
                return nullptr;
            SDL_SetAudioStreamGain(sg->stream, 0.f);
            SDL_BindAudioStream(a.device, sg->stream);
            return sg;
        }

        void feed(state& a, song& s) {
            int const want = int(float(s.rate * s.channels) * sizeof(float) * music_buffer_seconds);
            float buf[4096 * 2];
            while (!s.eof && SDL_GetAudioStreamQueued(s.stream) < want) {
                int const frames = int(sizeof(buf) / sizeof(float)) / s.channels;
                size_t got = 0;
                if (!s.pcm.empty()) {
                    got = std::min<size_t>(size_t(frames), (s.pcm.size() - s.pcm_pos) / size_t(s.channels));
                    std::memcpy(buf, s.pcm.data() + s.pcm_pos, got * size_t(s.channels) * sizeof(float));
                    s.pcm_pos += got * size_t(s.channels);
                } else if (s.is_mp3)
                    got = size_t(drmp3_read_pcm_frames_f32(&s.mp3, drmp3_uint64(frames), buf));
                else
                    got = size_t(stb_vorbis_get_samples_float_interleaved(s.ogg, s.channels, buf, frames * s.channels));
                if (got == 0) {
                    s.eof = true;
                    SDL_FlushAudioStream(s.stream);
                    break;
                }
                SDL_PutAudioStreamData(s.stream, buf, int(got * size_t(s.channels) * sizeof(float)));
            }
        }

        bool finished(song const& s) {
            return (s.ending && s.gain <= 0.f) || (s.eof && SDL_GetAudioStreamQueued(s.stream) == 0 && SDL_GetAudioStreamAvailable(s.stream) == 0);
        }

        void fade_out(state& a, float seconds) {
            if (!a.current)
                return;
            a.current->ending = true;
            a.current->target = 0.f;
            a.current->fade_speed = mathf::max(a.current->gain, 0.01f) / mathf::max(seconds, 0.05f);
            a.fading.push_back(std::move(a.current));
        }

        void start_song(state& a, std::string const& name, float vol, float fade_in_seconds, float prio, int32_t rooms) {
            auto sg = open_song(a, name);
            if (!sg)
                return;
            fade_out(a, 1.5f);
            sg->target = vol * music_volume;
            sg->fade_speed = sg->target / mathf::max(fade_in_seconds, 0.05f);
            sg->prio = prio;
            sg->rooms_left = rooms;
            a.last_played[name] = a.clock;
            reports::add_log("music: " + name);
            a.current = std::move(sg);
        }

        // MusicPlayer.GameRequestsSong / GameRequestsSongStop
        void music_request(state& a, sound::music_request const& r) {
            if (r.stop) {
                fade_out(a, r.fade_out / ticks_per_second);
                return;
            }
            if (r.song.empty() || r.song == "NO SONG")
                return;
            if (a.current && (a.current->name == r.song || r.prio <= a.current->prio))
                return;
            if (auto const it = a.last_played.find(r.song); it != a.last_played.end() && a.clock - it->second < song_rest_seconds)
                return;
            start_song(a, r.song, r.vol, r.fade_in / ticks_per_second, r.prio, r.rooms);
        }

        void update_song(song& s, float dt) {
            if (s.gain < s.target)
                s.gain = mathf::min(s.target, s.gain + s.fade_speed * dt);
            else if (s.gain > s.target)
                s.gain = mathf::max(s.target, s.gain - s.fade_speed * dt);
            SDL_SetAudioStreamGain(s.stream, s.gain);
        }
    }

    handle create(sys::game_state const& gs) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            reports::add_log(std::string{"audio: "} + SDL_GetError());
            return nullptr;
        }
        handle a{new state};
        a->fs = &gs.fs;
        a->random.state ^= uint32_t(SDL_GetTicksNS()) | 1u;
        a->device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &a->spec);
        if (a->device == 0) {
            reports::add_log(std::string{"audio: no playback device: "} + SDL_GetError());
            return nullptr;
        }
        load_sound_table(*a);
        open_bank(*a, a->sfx_bank, "loadedsoundeffects");
        open_bank(*a, a->song_bank, "music_songs");
        reports::add_log("audio: " + std::to_string(a->sfx_bank.clips.size()) + " sound clips, " + std::to_string(a->song_bank.clips.size()) + " songs");
        return a;
    }

    void update(state& a, sys::game_state& gs, dcon::room_id viewed_room, vec2 view_center, vec2 view_size, float dt) {
        a.clock += double(dt);
        uint32_t const room = viewed_room ? uint32_t(viewed_room.index()) : 0xFFFFFFFFu;
        for (auto const& r : gs.sounds)
            play_request(a, r, room, view_center, view_size);
        gs.sounds.clear();
        for (auto const& r : gs.music)
            music_request(a, r);
        gs.music.clear();

        // MusicEvent.roomsRange: the song ends after that many rooms
        if (room != a.last_room) {
            if (a.last_room != 0xFFFFFFFFu && a.current && a.current->rooms_left > 0 && --a.current->rooms_left == 0)
                fade_out(a, 4.f);
            a.last_room = room;
        }

        std::erase_if(a.voices, [](SDL_AudioStream* v) {
            if (SDL_GetAudioStreamQueued(v) > 0 || SDL_GetAudioStreamAvailable(v) > 0)
                return false;
            SDL_DestroyAudioStream(v);
            return true;
        });
        if (a.current) {
            feed(a, *a.current);
            update_song(*a.current, dt);
            if (finished(*a.current))
                a.current.reset();
        }
        for (auto& s : a.fading) {
            feed(a, *s);
            update_song(*s, dt);
        }
        std::erase_if(a.fading, [](std::unique_ptr<song> const& s) { return finished(*s); });
    }

    void force_song(state& a, sys::game_state const& gs, dcon::region_id region) {
        auto const& w = gs.world;
        // the region's songs: whatever its music triggers play
        std::vector<std::pair<std::string, float>> songs;
        w.for_each_music_trigger([&](dcon::music_trigger_id t) {
            if (w.music_trigger_get_stop(t))
                return;
            auto const r = w.music_trigger_get_room_from_music_trigger_location(t);
            if (!r || w.room_get_region_from_region_membership(r) != region)
                return;
            std::string name{gs.to_string_view(w.music_trigger_get_song(t))};
            if (name.empty() || name == "NO SONG")
                return;
            for (auto const& s : songs)
                if (s.first == name)
                    return;
            songs.emplace_back(std::move(name), w.music_trigger_get_vol(t));
        });
        if (songs.empty())
            for (auto const& [key, clip] : a.song_bank.clips)
                songs.emplace_back(clip.name, 0.35f);
        if (songs.empty())
            return;
        if (a.current && songs.size() > 1)
            std::erase_if(songs, [&](auto const& s) { return s.first == a.current->name; });
        auto const& pick = songs[size_t(a.random.range(0, int32_t(songs.size())))];
        start_song(a, pick.first, pick.second, 0.5f, 1.f, -1);
    }

    void stop_song(state& a) {
        fade_out(a, 2.f);
    }

    bool self_check(state& a, sys::game_state const& gs) {
        uint32_t ids = 0, playable = 0, samples = 0, loaded = 0;
        std::string missing;
        for (size_t i = 0; i < size_t(sound::sound_id::count); ++i) {
            ++ids;
            bool any = false;
            for (auto const& s : a.defs[i].samples) {
                ++samples;
                if (!sample(a, pick_variant(a, s.name)).empty()) {
                    ++loaded;
                    any = true;
                } else {
                    missing += " " + s.name;
                }
            }
            if (any)
                ++playable;
            else
                missing += std::string{" ["} + sound::to_string(sound::sound_id(i)) + "]";
        }
        reports::add_log("audio: " + std::to_string(playable) + "/" + std::to_string(ids) + " sounds playable, "
            + std::to_string(loaded) + "/" + std::to_string(samples) + " samples loaded; missing:" + missing);
        uint32_t songs_ok = 0;
        for (char const* name : {"RW_1 - Urban Jungle", "NA_01 - Proxima"}) {
            auto sg = open_song(a, name);
            if (!sg)
                continue;
            float buf[4096];
            size_t frames = 0;
            float peak = 0.f;
            double sum = 0.0;
            size_t count = 0;
            // the whole song: a bad Vorbis setup would fail or produce noise
            for (size_t got = 1; got > 0 && frames < size_t(sg->rate) * 600;) {
                got = sg->is_mp3 ? size_t(drmp3_read_pcm_frames_f32(&sg->mp3, drmp3_uint64(4096 / sg->channels), buf))
                    : sg->ogg != nullptr ? size_t(stb_vorbis_get_samples_float_interleaved(sg->ogg, sg->channels, buf, 4096))
                    : 0;
                frames += got;
                for (size_t i = 0; i < got * size_t(sg->channels); ++i) {
                    peak = std::max(peak, std::fabs(buf[i]));
                    sum += double(buf[i]) * double(buf[i]);
                    ++count;
                }
            }
            reports::add_log(std::string{"audio: "} + name + ": " + std::to_string(sg->channels) + "ch " + std::to_string(sg->rate) + "Hz, decoded "
                + std::to_string(frames) + " frames (" + std::to_string(frames / size_t(sg->rate)) + "s), peak " + std::to_string(peak)
                + " rms " + std::to_string(std::sqrt(sum / double(std::max<size_t>(count, 1)))));
            songs_ok += frames > 0;
        }
        (void)gs;
        return playable > 0 && songs_ok > 0;
    }

    std::string current_song(state const& a) {
        return a.current ? a.current->name : std::string{};
    }
}
