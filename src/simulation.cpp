#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

#include <tbb/parallel_for.h>

#include "simulation.hpp"
#include "centipede.hpp"
#include "centipede_graphics.hpp"
#include "cicada.hpp"
#include "cicada_graphics.hpp"
#include "lizard.hpp"
#include "lizard_graphics.hpp"
#include "daddy.hpp"
#include "daddy_graphics.hpp"
#include "dropbug.hpp"
#include "dropbug_graphics.hpp"
#include "gates.hpp"
#include "items.hpp"
#include "outposts.hpp"
#include "pathing.hpp"
#include "player.hpp"
#include "player_graphics.hpp"
#include "rooms.hpp"
#include "rot.hpp"
#include "warps.hpp"
#include "particles.hpp"
#include "water.hpp"
#include "terrain.hpp"
#include "shortcuts.hpp"
#include "world.hpp"

namespace rooms {
    namespace {
        template<typename ID, typename All, typename Room>
        void fill(sys::room_buckets<ID>& b, size_t room_count, All&& all, Room&& room_of) {
            // a counting sort by room: ids come in ascending order, and stay so
            b.first.assign(room_count + 1, 0);
            b.ids.clear();
            all([&](ID id) {
                auto const r = room_of(id);
                if (r && size_t(r.index()) < room_count)
                    ++b.first[size_t(r.index()) + 1];
            });
            for (size_t i = 0; i < room_count; ++i)
                b.first[i + 1] += b.first[i];
            b.ids.resize(b.first[room_count]);
            std::vector<uint32_t> at(b.first.begin(), b.first.end() - 1);
            all([&](ID id) {
                auto const r = room_of(id);
                if (r && size_t(r.index()) < room_count)
                    b.ids[at[size_t(r.index())]++] = id;
            });
        }
    }

    void build_index(sys::game_state& state) {
        auto const& w = state.world;
        auto& x = state.index;
        size_t const n = size_t(w.room_size());
        auto const of = [&](auto id) { return room_of(state, id); };
        fill(x.players, n, [&](auto&& f) { w.for_each_player(f); }, of);
        fill(x.centipedes, n, [&](auto&& f) { w.for_each_centipede(f); }, of);
        fill(x.dropbugs, n, [&](auto&& f) { w.for_each_dropbug(f); }, of);
        fill(x.cicadas, n, [&](auto&& f) { w.for_each_cicada(f); }, of);
        fill(x.lizards, n, [&](auto&& f) { w.for_each_lizard(f); }, of);
        fill(x.daddies, n, [&](auto&& f) { w.for_each_daddy(f); }, of);
        fill(x.items, n, [&](auto&& f) { w.for_each_item(f); }, of);
        fill(x.spits, n, [&](auto&& f) { w.for_each_lizard_spit(f); }, of);
    }
}

namespace simulation {
    using rw::vec2;

    dcon::room_id player_room(sys::game_state const& state, dcon::player_id p) noexcept {
        return state.world.physical_object_get_room_from_object_location(player::body(state, p));
    }

    void populate(sys::game_state& state) {
        auto& w = state.world;
        state.rooms.resize(size_t(w.room_size()));
        std::vector<dcon::room_id> fresh;
        w.for_each_room([&](dcon::room_id r) {
            if (!state.runtime(r).populated)
                fresh.push_back(r);
        });
        if (fresh.empty())
            return;
        // every room its own generator: the world seed (game_state::random)
        // and the room's name, so a room rolls the same whatever order the
        // rooms are populated or updated in
        for (auto const r : fresh) {
            uint32_t h = 2166136261u;
            for (char const c : state.to_string_view(w.room_get_name(r)))
                h = (h ^ uint8_t(c)) * 16777619u;
            h ^= state.random.state * 0x9E3779B1u;
            h ^= h >> 16;
            w.room_set_random(r, rw::rng{h != 0 ? h : 0x2545F491u});
            state.runtime(r).cosmetic = rw::rng{(h * 0x85EBCA6Bu) | 1u};
        }
        // Room.Loaded's first visit, for every room at once: what creates
        // objects goes room by room in order...
        for (auto const r : fresh) {
            items::spawn_room_items(state, r);
            rot::spawn_room(state, r);
            warps::spawn_room(state, r);
            state.runtime(r).populated = true;
        }
        // ...the sand height maps only touch their own room
        tbb::parallel_for(size_t(0), fresh.size(), [&](size_t i) { terrain::sand(state, fresh[i]); });
    }

    namespace {
        // ActiveTriggerChecker for the music triggers of the player's room
        void update_music_triggers(sys::game_state& state, dcon::player_id p, dcon::room_id room) {
            auto& w = state.world;
            bool const entered = w.player_get_trigger_room(p) != room;
            if (entered) {
                // a realised room gets fresh trigger checkers
                w.player_set_trigger_room(p, room);
                w.room_for_each_music_trigger_location(room, [&](dcon::music_trigger_location_id l) {
                    auto const t = w.music_trigger_location_get_trigger(l);
                    w.music_trigger_set_fired(t, false);
                    w.music_trigger_set_counter(t, -1);
                });
            }
            vec2 const main = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
            w.room_for_each_music_trigger_location(room, [&](dcon::music_trigger_location_id l) {
                auto const t = w.music_trigger_location_get_trigger(l);
                if (w.music_trigger_get_fired(t) && !w.music_trigger_get_multi_use(t))
                    return;
                bool fire = false;
                int16_t& counter = w.music_trigger_get_counter(t);
                if (counter >= 0) {
                    if (++counter >= w.music_trigger_get_delay(t)) {
                        fire = true;
                        counter = -1;
                    }
                } else {
                    bool const positive = w.music_trigger_get_spot(t)
                        ? rw::custom::dist_less(main, w.music_trigger_get_pos(t), w.music_trigger_get_rad(t))
                        : entered;
                    if (positive) {
                        if (w.music_trigger_get_delay(t) < 1)
                            fire = true;
                        else
                            counter = 0;
                    }
                }
                if (!fire)
                    return;
                w.music_trigger_set_fired(t, true);
                float const chance = w.music_trigger_get_fire_chance(t);
                if (chance < 1.f && state.room_random(room).value() >= chance)
                    return;
                sound::music_request r;
                r.stop = w.music_trigger_get_stop(t);
                r.song = std::string{state.to_string_view(w.music_trigger_get_song(t))};
                r.vol = w.music_trigger_get_vol(t);
                r.prio = w.music_trigger_get_prio(t);
                r.fade_in = w.music_trigger_get_fade_in(t);
                r.fade_out = w.music_trigger_get_fade_out(t);
                r.rooms = w.music_trigger_get_rooms(t);
                // only from the rooms a camera looks at
                auto& music = state.parallel ? state.runtime(room).music : state.music;
                if (state.viewed(room) && music.size() < 16)
                    music.push_back(std::move(r));
            });
        }

        // the players of this tick and what drives them
        struct driven {
            dcon::player_id p;
            ::player::input_package in;
        };

        // One room's tick: its players, creatures, items, rot, water, sand
        // and gate. Runs in parallel with every other room's: it reads and
        // writes only what is in the room, and leaves the rest (new or
        // deleted objects, anything changing rooms) to `state.defer`
        void update_room(sys::game_state& state, dcon::room_id room, std::span<driven const> players) {
            rooms::updating = room;
            auto& rt = state.runtime(room);
            // noises are heard the tick they happen
            rt.noises.clear();
            pathing::trim(state, room);
            auto const view = world::get_room_view(state, room);
            for (auto const& [p, in] : players) {
                update_music_triggers(state, p, room);
                player::update(state, p, view, in);
                player_graphics::update(state, p, view);
                // Player.GraphicsModuleUpdated: held items follow the hands
                items::carry_update(state, p);
                items::back_spear_update(state, p);
            }
            centipede::update_room(state, view);
            centipede::collide(state, view);
            dropbug::update_room(state, view);
            dropbug::collide(state, view);
            cicada::update_room(state, view);
            cicada::collide(state, view);
            lizard::update_room(state, view);
            lizard::collide(state, view);
            daddy::update_room(state, view);
            daddy::collide(state, view);
            outposts::update_room(state, view);
            items::update_room(state, view);
            rot::update_room(state, view);
            water::update_room(state, view);
            terrain::update_room(state, view);
            particles::ambient(state, view);
            gates::update_room(state, room);
        }

        // InitiateGraphicsModule + Reset: a room a camera starts looking at
        // gets its creatures' graphics modules back, the parts on the bodies
        void graphics_came_into_view(sys::game_state& state, dcon::room_id room) {
            auto& w = state.world;
            rooms::for_each_centipede(state, room, [&](dcon::centipede_id c) { centipede_graphics::reset_positions(state, c); });
            rooms::for_each_dropbug(state, room, [&](dcon::dropbug_id d) { dropbug_graphics::reset_positions(state, d); });
            rooms::for_each_cicada(state, room, [&](dcon::cicada_id c) { cicada_graphics::reset_positions(state, c); });
            rooms::for_each_lizard(state, room, [&](dcon::lizard_id l) {
                lizard_graphics::reset_positions(state, l);
                // no limb has found a grip yet: Lizard.LegsGripping's 2 until
                // the first update
                auto& g = w.lizard_get_graphics(l);
                g.legs_grabbing = 2;
                g.no_grip_counter = 0;
            });
            rooms::for_each_daddy(state, room, [&](dcon::daddy_id d) { daddy_graphics::reset(state, d); });
        }
    }

    tick_times& times() noexcept {
        static tick_times t;
        return t;
    }

    void tick(sys::game_state& state, std::span<dcon::player_id const> players, std::span<::player::input_package const> inputs) {
        using clock = std::chrono::steady_clock;
        auto const t0 = clock::now();
        auto& w = state.world;
        // rooms added since (tests build their own) get their first visit
        populate(state);

        // the players out in a room update with it; the ones in a pipe travel
        // it first, serially (coming out moves them to another room)
        std::vector<driven> out;
        for (size_t i = 0; i < players.size(); ++i) {
            auto const p = players[i];
            if (w.player_get_in_shortcut(p))
                shortcuts::update_vessel(state, p);
            else
                out.push_back(driven{p, i < inputs.size() ? inputs[i] : ::player::input_package{}});
        }

        // the rooms a camera looks at: the players' and the app's
        for (auto& rt : state.rooms)
            rt.viewed = false;
        auto const view = [&](dcon::room_id r) {
            if (state.has_runtime(r))
                state.runtime(r).viewed = true;
        };
        w.for_each_player([&](dcon::player_id p) { view(player_room(state, p)); });
        for (auto const r : state.camera_rooms)
            view(r);
        for (size_t r = 0; r < state.rooms.size(); ++r) {
            auto& rt = state.rooms[r];
            if (rt.viewed && !rt.was_viewed)
                graphics_came_into_view(state, dcon::room_id{dcon::room_id::value_base_t(r)});
            rt.was_viewed = rt.viewed;
        }

        // every room, in parallel, each with its players in input order
        rooms::build_index(state);
        std::vector<uint32_t> first(state.rooms.size() + 1, 0);
        for (auto const& d : out)
            if (auto const r = player_room(state, d.p))
                ++first[size_t(r.index()) + 1];
        for (size_t r = 0; r < state.rooms.size(); ++r)
            first[r + 1] += first[r];
        std::vector<driven> by_room(out.size());
        {
            std::vector<uint32_t> at(first.begin(), first.end() - 1);
            for (auto const& d : out)
                if (auto const r = player_room(state, d.p))
                    by_room[at[size_t(r.index())]++] = d;
        }
        size_t const first_sound = state.sounds.size();
        auto const t1 = clock::now();
        state.index.valid = true;
        state.parallel = true;
        tbb::parallel_for(size_t(0), size_t(w.room_size()), [&](size_t r) {
            dcon::room_id const room{dcon::room_id::value_base_t(r)};
            update_room(state, room, std::span<driven const>{by_room.data() + first[r], by_room.data() + first[r + 1]});
        });
        state.parallel = false;
        state.index.valid = false;
        auto const t2 = clock::now();

        // what the rooms left for after: their transactions, room by room in
        // order, then their sounds, music, particles and rings
        for (auto& rt : state.rooms) {
            for (auto& f : rt.deferred)
                f(state);
            rt.deferred.clear();
        }
        for (auto& rt : state.rooms) {
            for (auto const& s : rt.sounds)
                if (state.sounds.size() < 256)
                    state.sounds.push_back(s);
            rt.sounds.clear();
            for (auto& m : rt.music)
                if (state.music.size() < 16)
                    state.music.push_back(std::move(m));
            rt.music.clear();
            state.particles.insert(state.particles.end(), rt.particles.begin(), rt.particles.end());
            rt.particles.clear();
            state.ripple_rings.insert(state.ripple_rings.end(), rt.ripple_rings.begin(), rt.ripple_rings.end());
            rt.ripple_rings.clear();
        }

        // the warps reach across rooms (the transfer, the screen wide
        // sequence): every room's, serially
        w.for_each_room([&](dcon::room_id room) {
            if (w.room_range_of_warp_point_location(room).first != w.room_range_of_warp_point_location(room).second)
                warps::update_room(state, world::get_room_view(state, room));
        });
        warps::update_sequence(state);
        particles::from_sounds(state, first_sound);
        particles::update(state);
        warps::update_ripple_rings(state);
        ++state.tick;
        auto const t3 = clock::now();
        auto& tt = times();
        tt.serial_before += std::chrono::duration<double, std::milli>(t1 - t0).count();
        tt.rooms += std::chrono::duration<double, std::milli>(t2 - t1).count();
        tt.serial_after += std::chrono::duration<double, std::milli>(t3 - t2).count();
        ++tt.ticks;
    }

    uint64_t hash(sys::game_state const& state) {
        auto const& w = state.world;
        // FNV-1a over every body chunk's position and velocity, in id order
        uint64_t h = 1469598103934665603ull;
        auto const mix = [&](float f) {
            uint32_t u;
            std::memcpy(&u, &f, sizeof u);
            for (int i = 0; i < 4; ++i)
                h = (h ^ ((u >> (i * 8)) & 0xFFu)) * 1099511628211ull;
        };
        w.for_each_body_chunk([&](dcon::body_chunk_id c) {
            vec2 const p = w.body_chunk_get_pos(c);
            vec2 const v = w.body_chunk_get_vel(c);
            mix(p.x);
            mix(p.y);
            mix(v.x);
            mix(v.y);
        });
        return h;
    }
}
