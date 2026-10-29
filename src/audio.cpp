#include <algorithm>
#include <cstring>
#include <cmath>
#include <cstdio>
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
        constexpr size_t max_loops = 24;
        constexpr size_t max_ambient = 16;
        // loops are fed this far ahead (their pan is baked into what is fed)
        constexpr float loop_buffer_seconds = 0.1f;
        constexpr float ambient_buffer_seconds = 0.15f;
        // the shortest fade out of a loop, against clicks (vanilla cuts)
        constexpr float declick_seconds = 0.06f;
        // and in
        constexpr float fade_in_seconds = 0.02f;
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
            // SoundTrigger's own vol/pitch ranges (the header's flags) and
            // rangeFac
            float min_vol = 1.f, max_vol = 1.f;
            float min_pitch = 1.f, max_pitch = 1.f;
            float range = 1.f;
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
            // ambient clips: start over at the end, fed as stereo with `pan`
            // baked in, not as far ahead
            bool loop = false;
            bool panned = false;
            float pan = 0.f;
            float buffer_seconds = music_buffer_seconds;

            ~song() {
                if (stream != nullptr)
                    SDL_DestroyAudioStream(stream);
                if (is_mp3)
                    drmp3_uninit(&mp3);
                if (ogg != nullptr)
                    stb_vorbis_close(ogg);
            }
        };

        // the device side of a loop: a stream per sample (PLAYALL plays
        // them all), fed the sample over and over
        struct loop_voice {
            struct layer {
                std::vector<float> const* data = nullptr; // interleaved stereo at mix_rate
                size_t pos = 0;                           // in floats
                float vol = 1.f;
                float pitch = 1.f;
                SDL_AudioStream* stream = nullptr;
            };
            std::vector<layer> layers;
            bool used = false;
        };

        // AmbientSoundPlayer: one of the heard room's ambient sounds
        struct ambient_player {
            world::ambient_type type = world::ambient_type::omni;
            std::string sample; // as written
            float volume = 0.5f;
            float pitch = 1.f;
            vec2 pos{0.f, 0.f};
            vec2 direction{0.f, -1.f};
            float rad = 50.f;
            float taper = 0.1f;
            std::unique_ptr<song> clip;
            float envelope = 0.f;
            bool stopping = false;
        };

        // offline: when a loop started and stopped (seconds), for the dump's report
        struct loop_event {
            std::string what;
            double start = 0.0;
            double stop = -1.0;
            float peak = 0.f;
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
        // the loops (game_state::loops of the last tick seen)
        std::vector<loop_state> loops;
        std::vector<loop_voice> loop_voices;
        uint32_t loop_tick = 0;
        bool loop_tick_seen = false;
        // the heard room's ambient sounds, and the room they are for
        clip_bank ambient_bank;
        std::vector<ambient_player> ambient;
        uint32_t ambient_room = 0xFFFFFFFFu;
        // offline: everything mixed so far (interleaved stereo at mix_rate)
        bool offline = false;
        std::vector<float> mixed;
        double mix_frames_due = 0.0;
        std::vector<loop_event> loop_events;
        std::vector<size_t> loop_event_of; // per loop_voices entry
    };

    void state_deleter::operator()(state* s) const noexcept {
        if (s == nullptr)
            return;
        s->current.reset();
        s->fading.clear();
        s->ambient.clear();
        for (auto* v : s->voices)
            SDL_DestroyAudioStream(v);
        for (auto& v : s->loop_voices)
            for (auto& l : v.layers)
                if (l.stream != nullptr)
                    SDL_DestroyAudioStream(l.stream);
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
                    } else if (size_t const eq = tok.find('='); eq != std::string_view::npos) {
                        auto const key = tok.substr(0, eq);
                        float const v = std::strtof(std::string{tok.substr(eq + 1)}.c_str(), nullptr);
                        if (key == "silentChance") d.silent_chance = v;
                        else if (key == "vol") d.min_vol = d.max_vol = v;
                        else if (key == "minVol") d.min_vol = v;
                        else if (key == "maxVol") d.max_vol = v;
                        else if (key == "pitch") d.min_pitch = d.max_pitch = v;
                        else if (key == "minPitch") d.min_pitch = v;
                        else if (key == "maxPitch") d.max_pitch = v;
                        else if (key == "rangeFac") d.range = v;
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

        // How loud a sound at `pos` is heard and where it pans: panned across
        // the screen, fading out past its edges (sounds.txt's rangeFac
        // stretches the fade)
        float place(vec2 pos, vec2 center, vec2 size, float range, float& pan) {
            pan = mathf::clamp((pos.x - center.x) / (size.x * 0.5f), -1.f, 1.f) * 0.75f;
            float const dx = mathf::max(0.f, std::fabs(pos.x - center.x) - size.x * 0.5f);
            float const dy = mathf::max(0.f, std::fabs(pos.y - center.y) - size.y * 0.5f);
            return mathf::clamp01(1.f - std::sqrt(dx * dx + dy * dy) / (700.f * mathf::max(range, 0.05f)));
        }

        // equal power pan of stereo data: the left and right gains
        void pan_gains(float pan, float& l, float& r) {
            l = mathf::min(std::sqrt(0.5f * (1.f - pan)) * 1.41421356f, 1.f);
            r = mathf::min(std::sqrt(0.5f * (1.f + pan)) * 1.41421356f, 1.f);
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
            float l, r;
            pan_gains(pan, l, r);
            a.scratch.resize(data.size());
            for (size_t i = 0; i + 1 < data.size(); i += 2) {
                a.scratch[i] = data[i] * l;
                a.scratch[i + 1] = data[i + 1] * r;
            }
            SDL_SetAudioStreamFrequencyRatio(s, mathf::clamp(pitch, 0.1f, 10.f));
            SDL_SetAudioStreamGain(s, gain);
            SDL_PutAudioStreamData(s, a.scratch.data(), int(a.scratch.size() * sizeof(float)));
            SDL_FlushAudioStream(s);
            if (a.device != 0)
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
                gain *= place(r.pos, center, size, d.range, pan);
            }
            auto const one = [&](sample_ref const& s) {
                float const v = mathf::lerp(s.min_vol, s.max_vol, a.random.value()) * mathf::lerp(d.min_vol, d.max_vol, a.random.value());
                float const p = mathf::lerp(s.min_pitch, s.max_pitch, a.random.value()) * mathf::lerp(d.min_pitch, d.max_pitch, a.random.value());
                play_voice(a, sample(a, pick_variant(a, s.name)), gain * v, pan, p * r.pitch);
            };
            if (d.play_all) {
                for (auto const& s : d.samples)
                    one(s);
            } else {
                one(d.samples[size_t(a.random.range(0, int32_t(d.samples.size())))]);
            }
        }

        // the stream a song or ambient clip is fed into: its own format, or
        // stereo for panned ones
        bool make_stream(state& a, song& sg) {
            SDL_AudioSpec const src{SDL_AUDIO_F32, sg.panned ? 2 : sg.channels, sg.rate};
            sg.stream = SDL_CreateAudioStream(&src, &a.spec);
            if (sg.stream == nullptr)
                return false;
            SDL_SetAudioStreamGain(sg.stream, 0.f);
            if (a.device != 0)
                SDL_BindAudioStream(a.device, sg.stream);
            return true;
        }

        // a clip of one of the game's bundles: Vorbis rebuilt as Ogg and
        // streamed, PCM decoded whole
        bool open_bundled(song& sg, clip_bank const& bank, std::string const& lname) {
            std::vector<uint8_t> raw;
            fsb5::info info;
            if (!clip_data(bank, lname, raw, info))
                return false;
            sg.file.reset();
            if (info.mode == 15) {
                if (!fsb5::to_ogg(raw.data(), raw.size(), info, sg.ogg_data))
                    return false;
                int err = 0;
                sg.ogg = stb_vorbis_open_memory(sg.ogg_data.data(), int(sg.ogg_data.size()), &err, nullptr);
                if (sg.ogg == nullptr)
                    return false;
            } else if (!fsb5::decode(raw.data(), raw.size(), info, sg.pcm)) {
                return false;
            }
            sg.channels = info.channels;
            sg.rate = info.rate;
            return true;
        }

        // a loose .ogg or .mp3 (mods; the game's own are empty placeholders)
        bool open_loose(song& sg, std::optional<layered_fs::mapped_file> of, bool mp3) {
            sg.file = std::move(of);
            if (mp3) {
                if (!drmp3_init_memory(&sg.mp3, sg.file->base, sg.file->size, nullptr))
                    return false;
                sg.is_mp3 = true;
                sg.channels = int(sg.mp3.channels);
                sg.rate = int(sg.mp3.sampleRate);
                return true;
            }
            int err = 0;
            sg.ogg = stb_vorbis_open_memory(static_cast<unsigned char const*>(sg.file->base), int(sg.file->size), &err, nullptr);
            if (sg.ogg == nullptr)
                return false;
            auto const info = stb_vorbis_get_info(sg.ogg);
            sg.channels = info.channels;
            sg.rate = int(info.sample_rate);
            return true;
        }

        std::unique_ptr<song> open_song(state& a, std::string const& name) {
            auto const dir = a.fs->get_root().get_single_subdir("music").get_single_subdir("songs");
            auto sg = std::make_unique<song>();
            sg->name = name;
            for (char const* ext : {".mp3", ".ogg"}) {
                auto of = dir.get_single_file(name + ext).open_file(*a.fs);
                if (!of || of->size == 0)
                    continue;
                if (!open_loose(*sg, std::move(of), ext[1] == 'm') || !make_stream(a, *sg))
                    return nullptr;
                return sg;
            }
            // bundled (the loose files are empty)
            if (!a.song_bank.clips.contains(text::to_lowercase_native(name))) {
                reports::add_log("audio: no song \"" + name + "\"");
                return nullptr;
            }
            if (!open_bundled(*sg, a.song_bank, text::to_lowercase_native(name))) {
                reports::add_log("audio: can't decode song \"" + name + "\"");
                return nullptr;
            }
            if (!make_stream(a, *sg))
                return nullptr;
            return sg;
        }

        // SoundLoader.RequestAmbientAudioClip: a mod's loose
        // loadedsoundeffects/ambient/<sample> first, else the
        // loadedsoundeffects_ambient bundle's clip (named without the
        // extension). Looping, panned
        std::unique_ptr<song> open_ambient(state& a, std::string const& sample) {
            auto sg = std::make_unique<song>();
            sg->name = sample;
            sg->loop = true;
            sg->panned = true;
            sg->buffer_seconds = ambient_buffer_seconds;
            std::string const lname = text::to_lowercase_native(sample);
            auto of = a.fs->get_root().get_single_subdir("loadedsoundeffects").get_single_subdir("ambient").get_single_file(sample).open_file(*a.fs);
            bool ok = false;
            if (of && of->size > 0) {
                if (lname.ends_with(".ogg")) {
                    ok = open_loose(*sg, std::move(of), false);
                } else if (lname.ends_with(".wav")) {
                    SDL_AudioSpec src{};
                    Uint8* buf = nullptr;
                    Uint32 len = 0;
                    auto* io = SDL_IOFromConstMem(of->base, of->size);
                    if (io != nullptr && SDL_LoadWAV_IO(io, true, &src, &buf, &len)) {
                        SDL_AudioSpec const dst{SDL_AUDIO_F32, src.channels, src.freq};
                        Uint8* conv = nullptr;
                        int conv_len = 0;
                        if (SDL_ConvertAudioSamples(&src, buf, int(len), &dst, &conv, &conv_len)) {
                            sg->pcm.assign(reinterpret_cast<float*>(conv), reinterpret_cast<float*>(conv) + conv_len / int(sizeof(float)));
                            sg->channels = src.channels;
                            sg->rate = src.freq;
                            ok = !sg->pcm.empty();
                            SDL_free(conv);
                        }
                        SDL_free(buf);
                    }
                }
            }
            if (!ok) {
                std::string base = lname;
                if (size_t const dot = base.rfind('.'); dot != std::string::npos)
                    base.resize(dot);
                ok = open_bundled(*sg, a.ambient_bank, base);
            }
            if (!ok || sg->channels < 1 || sg->channels > 2 || !make_stream(a, *sg))
                return nullptr;
            return sg;
        }

        // the next `frames` frames of a song or clip, in its own format;
        // a looping one starts over at its end
        size_t read_frames(song& s, float* buf, int frames) {
            for (int pass = 0; pass < 2; ++pass) {
                size_t got = 0;
                if (!s.pcm.empty()) {
                    got = std::min<size_t>(size_t(frames), (s.pcm.size() - s.pcm_pos) / size_t(s.channels));
                    std::memcpy(buf, s.pcm.data() + s.pcm_pos, got * size_t(s.channels) * sizeof(float));
                    s.pcm_pos += got * size_t(s.channels);
                } else if (s.is_mp3) {
                    got = size_t(drmp3_read_pcm_frames_f32(&s.mp3, drmp3_uint64(frames), buf));
                } else if (s.ogg != nullptr) {
                    got = size_t(stb_vorbis_get_samples_float_interleaved(s.ogg, s.channels, buf, frames * s.channels));
                }
                if (got > 0 || !s.loop)
                    return got;
                // AudioSource.loop
                s.pcm_pos = 0;
                if (s.is_mp3)
                    drmp3_seek_to_pcm_frame(&s.mp3, 0);
                else if (s.ogg != nullptr)
                    stb_vorbis_seek_start(s.ogg);
            }
            return 0;
        }

        void feed(state& a, song& s) {
            int const out_channels = s.panned ? 2 : s.channels;
            int const want = int(float(s.rate * out_channels) * sizeof(float) * s.buffer_seconds);
            float buf[4096 * 2];
            float l = 1.f, r = 1.f;
            pan_gains(s.pan, l, r);
            while (!s.eof && SDL_GetAudioStreamQueued(s.stream) < want) {
                int const frames = 4096;
                size_t const got = read_frames(s, buf, frames);
                if (got == 0) {
                    s.eof = true;
                    SDL_FlushAudioStream(s.stream);
                    break;
                }
                if (s.panned) {
                    // to stereo (mono clips doubled), the pan baked in
                    for (size_t i = got; i-- > 0;) {
                        float const x0 = buf[i * size_t(s.channels)];
                        float const x1 = s.channels == 2 ? buf[i * 2 + 1] : x0;
                        buf[i * 2] = x0 * l;
                        buf[i * 2 + 1] = x1 * r;
                    }
                }
                SDL_PutAudioStreamData(s.stream, buf, int(got * size_t(out_channels) * sizeof(float)));
            }
            (void)a;
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

        //
        // Loops: the device side of loop_state
        //
        void destroy_voice(state& a, uint32_t v) {
            if (v >= a.loop_voices.size())
                return;
            auto& lv = a.loop_voices[v];
            for (auto& l : lv.layers)
                if (l.stream != nullptr)
                    SDL_DestroyAudioStream(l.stream);
            lv.layers.clear();
            lv.used = false;
            if (a.offline && v < a.loop_event_of.size() && a.loop_event_of[v] < a.loop_events.size())
                a.loop_events[a.loop_event_of[v]].stop = a.clock;
        }

        // VirtualMicrophone.PlaySound(id, emitter, loop: true): a sample
        // (every one for PLAYALL) with SoundLoader's random volume and pitch,
        // looped from its start
        void start_voice(state& a, loop_state& l) {
            auto const& d = a.defs[size_t(l.req.id)];
            if (d.samples.empty() || (d.silent_chance > 0.f && a.random.value() < d.silent_chance))
                return; // tracked all the same: silent, like a missing trigger
            uint32_t v = 0;
            while (v < a.loop_voices.size() && a.loop_voices[v].used)
                ++v;
            if (v == a.loop_voices.size())
                a.loop_voices.emplace_back();
            auto& lv = a.loop_voices[v];
            auto const one = [&](sample_ref const& sr) {
                std::string const name = pick_variant(a, sr.name);
                std::vector<float> const* data = &sample(a, name);
                if (data->empty())
                    return;
                loop_voice::layer layer;
                layer.data = data;
                layer.vol = mathf::lerp(sr.min_vol, sr.max_vol, a.random.value()) * mathf::lerp(d.min_vol, d.max_vol, a.random.value());
                layer.pitch = mathf::lerp(sr.min_pitch, sr.max_pitch, a.random.value()) * mathf::lerp(d.min_pitch, d.max_pitch, a.random.value());
                layer.stream = SDL_CreateAudioStream(&a.spec, &a.spec);
                if (layer.stream == nullptr)
                    return;
                SDL_SetAudioStreamGain(layer.stream, 0.f);
                if (a.device != 0)
                    SDL_BindAudioStream(a.device, layer.stream);
                lv.layers.push_back(layer);
            };
            if (d.play_all) {
                for (auto const& sr : d.samples)
                    one(sr);
            } else {
                one(d.samples[size_t(a.random.range(0, int32_t(d.samples.size())))]);
            }
            if (lv.layers.empty())
                return;
            lv.used = true;
            l.voice = v;
            if (a.offline) {
                a.loop_event_of.resize(a.loop_voices.size(), size_t(-1));
                a.loop_event_of[v] = a.loop_events.size();
                char owner[32];
                std::snprintf(owner, sizeof(owner), "%016llx", static_cast<unsigned long long>(l.req.owner));
                a.loop_events.push_back(loop_event{std::string{sound::to_string(l.req.id)} + " (owner " + owner + ")", a.clock});
            }
        }

        // ObjectSound/RectangularSound/DisembodiedLoop.Update: the emitter's
        // volume, pitch and position this frame, the sample fed a little ahead
        void update_voice(state& a, loop_state const& l, vec2 center, vec2 size) {
            if (l.voice >= a.loop_voices.size())
                return;
            auto& lv = a.loop_voices[l.voice];
            auto const& d = a.defs[size_t(l.req.id)];
            float gain = l.req.vol * sfx_volume * l.envelope;
            float pan = 0.f;
            switch (l.req.shape) {
            case sound::loop_shape::positioned:
                gain *= place(l.req.pos, center, size, d.range, pan);
                break;
            case sound::loop_shape::rect: {
                // RectangularSound: the rect's point nearest the listener
                vec2 const p{mathf::clamp(center.x, l.req.pos.x, l.req.rect_max.x), mathf::clamp(center.y, l.req.pos.y, l.req.rect_max.y)};
                gain *= place(p, center, size, d.range, pan);
                break;
            }
            case sound::loop_shape::disembodied:
                pan = mathf::clamp(l.req.pan, -1.f, 1.f);
                break;
            }
            float lg, rg;
            pan_gains(pan, lg, rg);
            for (auto& layer : lv.layers) {
                float const ratio = mathf::clamp(layer.pitch * l.req.pitch, 0.1f, 10.f);
                SDL_SetAudioStreamFrequencyRatio(layer.stream, ratio);
                SDL_SetAudioStreamGain(layer.stream, gain * layer.vol);
                auto const& data = *layer.data;
                int const want = int(float(mix_rate * 2) * sizeof(float) * loop_buffer_seconds * ratio);
                while (SDL_GetAudioStreamQueued(layer.stream) < want) {
                    size_t const n = std::min<size_t>(4096, data.size() - layer.pos);
                    a.scratch.resize(n);
                    for (size_t i = 0; i + 1 < n; i += 2) {
                        a.scratch[i] = data[layer.pos + i] * lg;
                        a.scratch[i + 1] = data[layer.pos + i + 1] * rg;
                    }
                    SDL_PutAudioStreamData(layer.stream, a.scratch.data(), int(n * sizeof(float)));
                    layer.pos += n;
                    if (layer.pos + 1 >= data.size())
                        layer.pos = 0;
                }
            }
            if (a.offline && l.voice < a.loop_event_of.size() && a.loop_event_of[l.voice] < a.loop_events.size()) {
                auto& e = a.loop_events[a.loop_event_of[l.voice]];
                e.peak = mathf::max(e.peak, gain);
            }
        }

        void update_loops(state& a, sys::game_state const& gs, uint32_t room, vec2 center, vec2 size, float dt) {
            // a new tick: its requests (paused, the loops just go on)
            if (!a.loop_tick_seen || gs.loop_tick != a.loop_tick) {
                a.loop_tick = gs.loop_tick;
                a.loop_tick_seen = true;
                std::vector<size_t> started;
                upkeep(a.loops, gs.loops, room, max_loops, started);
                for (size_t const i : started)
                    start_voice(a, a.loops[i]);
            }
            std::vector<uint32_t> dropped;
            fade(a.loops, dt, dropped);
            for (uint32_t const v : dropped)
                destroy_voice(a, v);
            for (auto const& l : a.loops)
                update_voice(a, l, center, size);
        }

        //
        // AmbientSoundPlayer
        //
        // VirtualMicrophone.ambientSoundCloseEnoughCheck: an omnidirectional
        // sound carries on into the next room playing the same sample
        bool close_enough(ambient_player const& p, world::ambient_type type, std::string_view sample) {
            return p.type == world::ambient_type::omni && type == world::ambient_type::omni && p.sample == sample
                && text::to_lowercase_native(std::string{sample}) != "rwtw_sigil.ogg";
        }

        // VirtualMicrophone.NewRoom: keep the omnidirectional players the new
        // room has too (taking its volume and pitch), stop the rest, start
        // the room's others
        void new_room(state& a, sys::game_state const& gs, uint32_t room) {
            auto const& w = gs.world;
            a.ambient_room = room;
            std::vector<dcon::ambient_sound_id> sounds;
            if (room != 0xFFFFFFFFu)
                w.room_for_each_ambient_sound_location(dcon::room_id{dcon::room_id::value_base_t(room)}, [&](dcon::ambient_sound_location_id l) {
                    sounds.push_back(w.ambient_sound_location_get_sound(l));
                });
            std::vector<bool> taken(sounds.size(), false);
            for (auto& p : a.ambient) {
                if (p.stopping)
                    continue;
                bool keep = false;
                for (size_t i = 0; i < sounds.size() && !keep; ++i) {
                    if (!taken[i] && close_enough(p, w.ambient_sound_get_type(sounds[i]), gs.to_string_view(w.ambient_sound_get_sample(sounds[i])))) {
                        keep = taken[i] = true;
                        p.volume = w.ambient_sound_get_volume(sounds[i]);
                        p.pitch = w.ambient_sound_get_pitch(sounds[i]);
                    }
                }
                p.stopping = !keep;
            }
            for (size_t i = 0; i < sounds.size(); ++i) {
                if (taken[i] || a.ambient.size() >= max_ambient * 2)
                    continue;
                auto const s = sounds[i];
                ambient_player p;
                p.type = w.ambient_sound_get_type(s);
                p.sample = std::string{gs.to_string_view(w.ambient_sound_get_sample(s))};
                p.volume = w.ambient_sound_get_volume(s);
                p.pitch = w.ambient_sound_get_pitch(s);
                p.pos = w.ambient_sound_get_pos(s);
                p.direction = w.ambient_sound_get_direction(s);
                p.rad = w.ambient_sound_get_rad(s);
                p.taper = w.ambient_sound_get_taper(s);
                p.clip = open_ambient(a, p.sample);
                if (!p.clip) {
                    reports::add_log("audio: no ambient sound \"" + p.sample + "\"");
                    continue;
                }
                a.ambient.push_back(std::move(p));
            }
        }

        // AmbientSoundPlayer.DrawUpdate: Vol, Pan and the sound's pitch
        void update_ambient(state& a, sys::game_state const& gs, uint32_t room, vec2 listener, float dt) {
            if (room != a.ambient_room)
                new_room(a, gs, room);
            for (auto& p : a.ambient) {
                p.envelope = p.stopping ? p.envelope - dt / declick_seconds : mathf::min(1.f, p.envelope + dt / declick_seconds);
                float vol = 1.f;
                p.clip->pan = 0.f;
                if (p.type == world::ambient_type::spot) {
                    float const taper_rad = p.rad + (p.rad * 2.f + 1000.f) * p.taper;
                    vol = mathf::inverse_lerp(taper_rad, p.rad, rw::distance(p.pos, listener));
                    p.clip->pan = rw::custom::lerp_map(listener.x, p.pos.x - taper_rad, p.pos.x + taper_rad, 1.f, -1.f);
                } else if (p.type == world::ambient_type::directional) {
                    p.clip->pan = mathf::clamp(p.direction.x, -1.f, 1.f);
                }
                SDL_SetAudioStreamGain(p.clip->stream, mathf::max(0.f, vol * p.volume * sfx_volume * p.envelope));
                SDL_SetAudioStreamFrequencyRatio(p.clip->stream, mathf::clamp(p.pitch, 0.1f, 10.f));
                feed(a, *p.clip);
            }
            std::erase_if(a.ambient, [](ambient_player const& p) { return p.stopping && p.envelope <= 0.f; });
        }

        //
        // Offline: the device's mix, by hand
        //
        void mix_offline(state& a, float dt) {
            a.mix_frames_due += double(dt) * double(mix_rate);
            size_t const frames = size_t(a.mix_frames_due);
            a.mix_frames_due -= double(frames);
            if (frames == 0)
                return;
            size_t const at = a.mixed.size();
            a.mixed.resize(at + frames * 2, 0.f);
            std::vector<float> buf(frames * 2);
            auto const pull = [&](SDL_AudioStream* st) {
                if (st == nullptr)
                    return;
                int const got = SDL_GetAudioStreamData(st, buf.data(), int(buf.size() * sizeof(float)));
                for (int i = 0; i < got / int(sizeof(float)); ++i)
                    a.mixed[at + size_t(i)] += buf[size_t(i)];
            };
            for (auto* v : a.voices)
                pull(v);
            for (auto const& lv : a.loop_voices)
                for (auto const& l : lv.layers)
                    pull(l.stream);
            for (auto const& p : a.ambient)
                pull(p.clip->stream);
            if (a.current)
                pull(a.current->stream);
            for (auto const& sg : a.fading)
                pull(sg->stream);
        }

        handle open(sys::game_state const& gs, bool offline) {
            if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
                reports::add_log(std::string{"audio: "} + SDL_GetError());
                if (!offline)
                    return nullptr;
            }
            handle a{new state};
            a->fs = &gs.fs;
            a->offline = offline;
            if (!offline) {
                a->random.state ^= uint32_t(SDL_GetTicksNS()) | 1u;
                a->device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &a->spec);
                if (a->device == 0) {
                    reports::add_log(std::string{"audio: no playback device: "} + SDL_GetError());
                    return nullptr;
                }
            }
            load_sound_table(*a);
            open_bank(*a, a->sfx_bank, "loadedsoundeffects");
            open_bank(*a, a->song_bank, "music_songs");
            open_bank(*a, a->ambient_bank, "loadedsoundeffects_ambient");
            reports::add_log("audio: " + std::to_string(a->sfx_bank.clips.size()) + " sound clips, " + std::to_string(a->ambient_bank.clips.size())
                + " ambient clips, " + std::to_string(a->song_bank.clips.size()) + " songs");
            return a;
        }
    }

    void upkeep(std::vector<loop_state>& loops, std::span<sound::loop_request const> requests, uint32_t heard_room, size_t max_loops,
        std::vector<size_t>& started) {
        auto const stop = [](loop_state& l, bool declick_only) {
            l.stopping = true;
            l.fade_out = declick_only ? declick_seconds : mathf::max(declick_seconds, float(l.req.fade_out_frames) / 60.f);
        };
        std::vector<bool> seen(loops.size(), false);
        size_t playing = 0;
        for (auto const& l : loops)
            playing += l.stopping ? 0 : 1;
        for (auto const& r : requests) {
            if (r.room != heard_room || r.id >= sound::sound_id::count)
                continue;
            size_t i = 0;
            while (i < loops.size() && (loops[i].stopping || loops[i].req.owner != r.owner))
                ++i;
            if (i < loops.size() && loops[i].req.id == r.id && loops[i].req.room == r.room) {
                loops[i].req = r;
                seen[i] = true;
                continue;
            }
            if (i < loops.size()) {
                // DynamicSoundLoop: another sound, a new emitter
                stop(loops[i], false);
                --playing;
            }
            if (playing >= max_loops)
                continue;
            loop_state l;
            l.req = r;
            started.push_back(loops.size());
            loops.push_back(l);
            seen.push_back(true);
            ++playing;
        }
        for (size_t i = 0; i < loops.size(); ++i) {
            // a loop left behind in another room just stops, even one
            // already on a long fade
            if (loops[i].req.room != heard_room) {
                if (!loops[i].stopping || loops[i].fade_out > declick_seconds)
                    stop(loops[i], true);
                continue;
            }
            // requireActiveUpkeep: not kept alive this tick
            if (!seen[i] && !loops[i].stopping)
                stop(loops[i], false);
        }
    }

    void fade(std::vector<loop_state>& loops, float dt, std::vector<uint32_t>& dropped) {
        for (auto& l : loops) {
            if (l.stopping)
                l.envelope -= dt / mathf::max(l.fade_out, 0.001f);
            else
                l.envelope = mathf::min(1.f, l.envelope + dt / fade_in_seconds);
        }
        std::erase_if(loops, [&](loop_state const& l) {
            if (!l.stopping || l.envelope > 0.f)
                return false;
            if (l.voice != 0xFFFFFFFFu)
                dropped.push_back(l.voice);
            return true;
        });
    }

    handle create(sys::game_state const& gs) {
        return open(gs, false);
    }

    handle create_offline(sys::game_state const& gs) {
        return open(gs, true);
    }

    void update(state& a, sys::game_state& gs, dcon::room_id viewed_room, vec2 view_center, vec2 view_size, vec2 listener, float dt) {
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

        update_loops(a, gs, room, view_center, view_size, dt);
        update_ambient(a, gs, room, listener, dt);
        if (a.offline)
            mix_offline(a, dt);

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
        // the loops on their own: which have a sample, which sounds.txt leaves out
        {
            std::string ok_list, none;
            uint32_t ok_count = 0, loop_count = 0;
            for (size_t i = size_t(sound::sound_id::cicada_wings_loop); i < size_t(sound::sound_id::count); ++i) {
                ++loop_count;
                bool any = false;
                for (auto const& sr : a.defs[i].samples)
                    any = any || !sample(a, pick_variant(a, sr.name)).empty();
                if (any) {
                    ++ok_count;
                    ok_list += std::string{" "} + sound::to_string(sound::sound_id(i));
                } else {
                    none += std::string{" "} + sound::to_string(sound::sound_id(i)) + (a.defs[i].samples.empty() ? " (not in sounds.txt)" : " (samples missing)");
                }
            }
            reports::add_log("audio: loops: " + std::to_string(ok_count) + "/" + std::to_string(loop_count) + " playable:" + ok_list);
            if (!none.empty())
                reports::add_log("audio: loops without a sample:" + none);
        }
        // every room's ambient sounds: each distinct clip opened and its first
        // second decoded
        {
            auto const& w = gs.world;
            std::vector<std::string> names;
            w.for_each_ambient_sound([&](dcon::ambient_sound_id id) {
                names.emplace_back(gs.to_string_view(w.ambient_sound_get_sample(id)));
            });
            size_t const placed = names.size();
            std::sort(names.begin(), names.end());
            names.erase(std::unique(names.begin(), names.end()), names.end());
            uint32_t ok_count = 0;
            std::string bad;
            float quietest = 1.f;
            for (auto const& n : names) {
                auto sg = open_ambient(a, n);
                float peak = 0.f;
                if (sg) {
                    float buf[4096 * 2];
                    size_t frames = 0;
                    while (frames < size_t(sg->rate)) {
                        size_t const got = read_frames(*sg, buf, 4096);
                        if (got == 0)
                            break;
                        frames += got;
                        for (size_t i = 0; i < got * size_t(sg->channels); ++i)
                            peak = std::max(peak, std::fabs(buf[i]));
                    }
                }
                if (sg && peak > 0.f) {
                    ++ok_count;
                    quietest = std::min(quietest, peak);
                } else {
                    bad += " " + n;
                }
            }
            reports::add_log("audio: ambient sounds: " + std::to_string(placed) + " in the rooms, " + std::to_string(ok_count) + "/"
                + std::to_string(names.size()) + " clips decode (quietest first second peaks " + std::to_string(quietest) + ")"
                + (bad.empty() ? std::string{} : "; failed:" + bad));
        }
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
        return playable > 0 && songs_ok > 0;
    }

    bool dump(state const& a, char const* path) {
        std::FILE* f = std::fopen(path, "wb");
        if (f == nullptr) {
            reports::add_log(std::string{"audio: can't write "} + path);
            return false;
        }
        size_t const n = a.mixed.size();
        auto const u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
        auto const u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
        std::fwrite("RIFF", 1, 4, f);
        u32(uint32_t(36 + n * 2));
        std::fwrite("WAVEfmt ", 1, 8, f);
        u32(16);
        u16(1);
        u16(2);
        u32(mix_rate);
        u32(mix_rate * 4);
        u16(4);
        u16(16);
        std::fwrite("data", 1, 4, f);
        u32(uint32_t(n * 2));
        std::vector<int16_t> pcm(n);
        for (size_t i = 0; i < n; ++i)
            pcm[i] = int16_t(mathf::clamp(a.mixed[i], -1.f, 1.f) * 32767.f);
        std::fwrite(pcm.data(), 2, n, f);
        std::fclose(f);
        // the loudness every half second, and what looped when
        std::string line = "audio: rms per 0.5s:";
        size_t const window = size_t(mix_rate / 2) * 2;
        for (size_t at = 0; at < n; at += window) {
            double sum = 0.0;
            size_t const end = std::min(n, at + window);
            for (size_t i = at; i < end; ++i)
                sum += double(a.mixed[i]) * double(a.mixed[i]);
            char b[16];
            std::snprintf(b, sizeof(b), " %.3f", std::sqrt(sum / double(std::max<size_t>(end - at, 1))));
            line += b;
        }
        reports::add_log(line);
        for (auto const& e : a.loop_events) {
            char b[96];
            std::snprintf(b, sizeof(b), " %.2fs..%s peak gain %.3f", e.start, e.stop < 0.0 ? "end" : (std::to_string(e.stop).substr(0, 5) + "s").c_str(), e.peak);
            reports::add_log("audio: loop " + e.what + b);
        }
        for (auto const& p : a.ambient)
            reports::add_log("audio: ambient " + p.sample + " playing at the end");
        reports::add_log(std::string{"audio: wrote "} + path + " (" + std::to_string(n / 2 / mix_rate) + "s)");
        return true;
    }

    std::string current_song(state const& a) {
        return a.current ? a.current->name : std::string{};
    }
}
