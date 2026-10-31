#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

#include <tbb/parallel_for.h>

#include "simulation.hpp"
#include "creature.hpp"
#include "dens.hpp"
#include "migrate.hpp"
#include "npc.hpp"
#include "rain.hpp"
#include "spawns.hpp"
#include "reports.hpp"
#include "gates.hpp"
#include "items.hpp"
#include "outposts.hpp"
#include "pathing.hpp"
#include "piggyback.hpp"
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
        // `bucket_of` an object's bucket, or `count` for none
        template<typename ID, typename All, typename Bucket>
        void fill(sys::room_buckets<ID>& b, size_t count, All&& all, Bucket&& bucket_of) {
            // a counting sort by bucket: ids come in ascending order, and stay so
            b.first.assign(count + 1, 0);
            b.ids.clear();
            all([&](ID id) {
                if (size_t const k = bucket_of(id); k < count)
                    ++b.first[k + 1];
            });
            for (size_t i = 0; i < count; ++i)
                b.first[i + 1] += b.first[i];
            b.ids.resize(b.first[count]);
            std::vector<uint32_t> at(b.first.begin(), b.first.end() - 1);
            all([&](ID id) {
                if (size_t const k = bucket_of(id); k < count)
                    b.ids[at[k]++] = id;
            });
        }
    }

    void build_index(sys::game_state& state) {
        auto const& w = state.world;
        auto& x = state.index;
        size_t const n = size_t(w.room_size());
        // a room's bucket, `n` for none
        auto const of = [&](auto id) {
            auto const r = room_of(state, id);
            return r && size_t(r.index()) < n ? size_t(r.index()) : n;
        };
        fill(x.players, n, [&](auto&& f) { w.for_each_player(f); }, of);
        // a room's creatures kind by kind
        size_t const nt = creature::type_count;
        fill(x.creatures, n * nt, [&](auto&& f) { w.for_each_creature(f); }, [&](dcon::creature_id c) {
            size_t const r = of(c);
            return r < n ? r * nt + size_t(w.creature_get_type(c)) : n * nt;
        });
        fill(x.items, n, [&](auto&& f) { w.for_each_item(f); }, of);
        fill(x.spits, n, [&](auto&& f) { w.for_each_lizard_spit(f); }, of);
        // the creatures in a room's pipes and dens (their bodies in no room;
        // a slugcat's vessel travels on its own, shortcuts.hpp)
        fill(x.away, n, [&](auto&& f) { w.for_each_creature(f); }, [&](dcon::creature_id c) {
            auto const r = w.creature_get_away(c) != 0 && w.creature_get_type(c) != creature::type::slugcat ? w.creature_get_away_room(c) : dcon::room_id{};
            return r && size_t(r.index()) < n ? size_t(r.index()) : n;
        });
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
        // the rain cycles (once for the world; rooms added later follow
        // their region's), then the world files' creatures in their dens
        // (logged for the game's world, not the tests' rooms)
        bool const game_world = !state.fs.roots.empty();
        if (!state.rain_ready) {
            rain::setup(state);
            migrate::load_attractions(state);
            state.rain_ready = true;
            if (game_world)
                reports::add_log(rain::describe_groups(state));
        } else {
            rain::add_rooms(state, fresh);
        }
        if (state.spawn_dens && !state.dens_populated) {
            state.dens_populated = true;
            auto const r = spawns::spawn_world(state);
            if (game_world)
                reports::add_log(spawns::to_string(r));
        }
        // the NPC slugcats in their shelters
        if (state.npc_shelter_chance > 0.f && !state.npcs_populated) {
            state.npcs_populated = true;
            uint32_t const made = npc::spawn_world(state, state.npc_shelter_chance);
            if (game_world)
                reports::add_log(std::to_string(made) + " NPC slugcats in the shelters");
        }
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

        // the players of this tick and what drives them: a controller's
        // input, or an NPC's brain (npc.hpp)
        struct driven {
            dcon::player_id p;
            ::player::input_package in;
            bool npc = false;
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
            for (auto const& [p, given, npc] : players) {
                ::player::input_package in = given;
                if (npc)
                    in = npc::think(state, p, view);
                else
                    update_music_triggers(state, p, room);
                player::update(state, p, view, in);
                player_graphics::update(state, p, view);
                // Player.GraphicsModuleUpdated: held items follow the hands
                items::carry_update(state, p);
                items::back_spear_update(state, p);
            }
            // the riders on their carriers' backs (SlugOnBack)
            piggyback::update_room(state, view);
            // the vessels in the room's pipes and the creatures in its dens
            dens::update_room(state, view);
            creature::update_room(state, view);
            outposts::update_room(state, view);
            items::update_room(state, view);
            rot::update_room(state, view);
            water::update_room(state, view);
            terrain::update_room(state, view);
            rain::update_room(state, view);
            particles::ambient(state, view);
            gates::update_room(state, room);
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
        // the loops are kept up tick by tick: this tick's start empty
        state.loops.clear();
        // every region's rain cycle
        rain::update_world(state);
        // the NPC slugcats' packs
        npc::update_world(state);

        // the players out in a room update with it; the ones in a pipe travel
        // it first, serially (coming out moves them to another room). The
        // controlled ones, then the NPCs
        std::vector<driven> out;
        // (a rider goes through a pipe on its carrier's back)
        for (size_t i = 0; i < players.size(); ++i) {
            auto const p = players[i];
            if (player::in_shortcut(state, p) && piggyback::riding(state, p))
                continue;
            if (player::in_shortcut(state, p))
                shortcuts::update_vessel(state, p);
            else
                out.push_back(driven{p, i < inputs.size() ? inputs[i] : ::player::input_package{}});
        }
        w.for_each_slugcat_npc([&](dcon::slugcat_npc_id n) {
            auto const p = w.slugcat_npc_get_player(n);
            if (player::in_shortcut(state, p) && piggyback::riding(state, p))
                return;
            if (player::in_shortcut(state, p))
                shortcuts::update_vessel(state, p);
            else
                out.push_back(driven{p, ::player::input_package{}, true});
        });

        // the rooms a camera looks at: the players' and the app's
        for (auto& rt : state.rooms)
            rt.viewed = false;
        auto const view = [&](dcon::room_id r) {
            if (state.has_runtime(r))
                state.runtime(r).viewed = true;
        };
        w.for_each_player([&](dcon::player_id p) {
            if (!npc::is_npc(state, p))
                view(player_room(state, p));
        });
        for (auto const r : state.camera_rooms)
            view(r);
        for (size_t r = 0; r < state.rooms.size(); ++r) {
            auto& rt = state.rooms[r];
            // InitiateGraphicsModule + Reset: a room a camera starts looking
            // at gets its creatures' graphics modules back, the parts on the
            // bodies
            if (rt.viewed && !rt.was_viewed)
                creature::came_into_view(state, dcon::room_id{dcon::room_id::value_base_t(r)});
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
            for (auto const& l : rt.loops)
                if (state.loops.size() < 256)
                    state.loops.push_back(l);
            rt.loops.clear();
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
        ++state.loop_tick;
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
