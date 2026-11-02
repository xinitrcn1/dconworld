#include <algorithm>
#include <cmath>
#include <span>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>
#include <chrono>
#include <memory>
#include <thread>
#include <tuple>

#include <tbb/global_control.h>

#include "support.hpp"
#include "reports.hpp"
#include "startup.hpp"
#include "keyed_random.hpp"
#include "tracker.hpp"
#include "gates.hpp"
#include "audio.hpp"
#include "physics.hpp"
#include "centipede.hpp"
#include "cicada.hpp"
#include "dropbug.hpp"
#include "leech.hpp"
#include "tube_worm.hpp"
#include "needle_worm.hpp"
#include "pole_mimic.hpp"
#include "tentacle_plant.hpp"
#include "jetfish.hpp"
#include "scavenger.hpp"
#include "deer.hpp"
#include "miros.hpp"
#include "barnacle.hpp"
#include "egg_bug.hpp"
#include "garbage_worm.hpp"
#include "lantern_mouse.hpp"
#include "loach.hpp"
#include "moth_grub.hpp"
#include "overseer.hpp"
#include "snail.hpp"
#include "sky_whale.hpp"
#include "yeek.hpp"
#include "vulture.hpp"
#include "lizard.hpp"
#include "daddy.hpp"
#include "items.hpp"
#include "outposts.hpp"
#include "player_graphics.hpp"
#include "pathing.hpp"
#include "player.hpp"
#include "rot.hpp"
#include "warps.hpp"
#include "particles.hpp"
#include "water.hpp"
#include "terrain.hpp"
#include "simulation.hpp"
#include "dens.hpp"
#include "migrate.hpp"
#include "npc.hpp"
#include "piggyback.hpp"
#include "rain.hpp"
#include "spawns.hpp"
#include "world.hpp"

namespace selftest {
    using rw::ivec2;
    using rw::vec2;
    using world::terrain;
    using world::find_spawn;

    namespace {
        bool finite_chunks(sys::game_state const& gs, dcon::physical_object_id o) {
            bool ok = true;
            gs.world.physical_object_for_each_chunk_ownership_as_owner(o, [&](dcon::chunk_ownership_id co) {
                vec2 const q = gs.world.body_chunk_get_pos(gs.world.chunk_ownership_get_chunk(co));
                ok = ok && std::isfinite(q.x) && std::isfinite(q.y);
            });
            return ok;
        }

        bool left_room(world::room_view const& room, vec2 hp) {
            return hp.y < -20.f || hp.x < -20.f || hp.x > room.pixel_width() + 20.f;
        }
    }

    int npc_trip(sys::game_state& gs, std::string const& from_name, std::string const& to_name, uint32_t ticks, player::slugcat_name name, uint32_t seed) {
        auto& w = gs.world;
        if (!npc::debug().experiment.empty()) {
            // --npc-exp room,pole_x,climb_to_y,mode,dir[,turn] (npc.hpp's debug_options)
            char const* exp = npc::debug().experiment.c_str();
            // --npc-exp room,x,y,S,<script>: standing on tile x,y, the script
            // (--script's letters) played, every tick printed (a movement tech
            // tried in a real room: the crawl-turn vault in UW_E03)
            char const* sc = std::strstr(exp, ",S,");
            if (!sc)
                sc = std::strstr(exp, ",S+,");
            if (!sc)
                sc = std::strstr(exp, ",SW,");
            if (sc) {
                char rn[64];
                int fx = 0, fy = 0;
                std::sscanf(exp, "%63[^,],%d,%d", rn, &fx, &fy);
                gs.spawn_dens = false;
                gs.rain_frozen = true;
                simulation::populate(gs);
                auto const room = gs.find_room(rn);
                auto p = player::create(gs, room, world::room_view::middle_of_tile(fx, fy) + vec2{0.f, -2.f}, name == player::slugcat_name::count ? player::slugcat_name::white : name);
                bool const spear = sc[2] == '+';
                // ("...,SW,<script>": a grapple worm at its feet, `P` picks it up)
                bool const worm = sc[2] == 'W';
                // ("...,S+,<script>": a spear at its feet to start with, `P` picks it up)
                if (spear)
                    items::create_on_tile(gs, room, w.body_chunk_get_pos(player::body_chunk(gs, p, 1)), world::item_type::spear);
                for (int t = 0; t < 20; ++t) {
                    auto const in = player::make_input(0.f, 0.f, false, false, false);
                    simulation::tick(gs, std::span{&p, 1}, std::span{&in, 1});
                }
                if (worm)
                    tube_worm::create(gs, room, w.body_chunk_get_pos(player::body_chunk(gs, p, 1)));
                std::string const script = sc + (spear || worm ? 4 : 3);
                size_t pos = 0;
                int t = 0;
                float top = -1e9f;
                auto const show = [&](char const* l) {
                    vec2 const h = w.body_chunk_get_pos(player::body_chunk(gs, p, 0)), f = w.body_chunk_get_pos(player::body_chunk(gs, p, 1));
                    top = std::max(top, f.y);
                    std::printf("  t=%3d %-4s head %.1f,%.1f feet %.1f,%.1f anim %s mode %s%s\n", t, l, h.x, h.y, f.x, f.y, player::to_string(w.player_get_animation(p)),
                        player::to_string(w.player_get_body_mode(p)), items::held(gs, p, 0) || items::held(gs, p, 1) || tube_worm::held_by(gs, p) ? " holding" : "");
                };
                show("-");
                while (pos < script.size()) {
                    size_t const end = std::min(script.find(',', pos), script.size());
                    std::string const tok = script.substr(pos, end - pos);
                    pos = end + 1;
                    size_t i = 0;
                    float ax = 0.f, ay = 0.f;
                    bool jmp = false, thrw = false, pckp = false;
                    for (; i < tok.size() && std::isalpha(static_cast<unsigned char>(tok[i])); ++i)
                        switch (tok[i]) {
                        case 'U': ay += 1.f; break;
                        case 'D': ay -= 1.f; break;
                        case 'L': ax -= 1.f; break;
                        case 'R': ax += 1.f; break;
                        case 'J': jmp = true; break;
                        case 'X': thrw = true; break;
                        case 'P': pckp = true; break;
                        default: break;
                        }
                    int const n = i < tok.size() ? std::atoi(tok.c_str() + i) : 1;
                    std::string const letters = tok.substr(0, i);
                    for (int k = 0; k < n; ++k, ++t) {
                        auto const in = player::make_input(ax, ay, jmp, thrw, pckp);
                        simulation::tick(gs, std::span{&p, 1}, std::span{&in, 1});
                        show(letters.c_str());
                    }
                }
                std::printf("highest feet %.1f (start %.1f): %.1f px, %.1f tiles\n", top, float(fy) * 20.f, top - float(fy) * 20.f, (top - float(fy) * 20.f) / 20.f);
                rooms::for_each_item(gs, room, [&](dcon::item_id it) {
                    if (w.item_get_mode(it) == world::weapon_mode::stuck_in_wall)
                        std::printf("stuck spear at %.1f,%.1f (tile %d,%d)\n", w.item_get_stuck_pos(it).x, w.item_get_stuck_pos(it).y,
                            world::room_view::get_tile_position(w.item_get_stuck_pos(it)).x, world::room_view::get_tile_position(w.item_get_stuck_pos(it)).y);
                });
                return 0;
            }
            char rn[64];
            int px, cy, mode, dir, turn = 8;
            std::sscanf(exp, "%63[^,],%d,%d,%d,%d,%d", rn, &px, &cy, &mode, &dir, &turn);
            gs.spawn_dens = false;
            gs.rain_frozen = true;
            simulation::populate(gs);
            auto const room = gs.find_room(rn);
            auto p = player::create(gs, room, world::room_view::middle_of_tile(px, cy - 1) + vec2{0.f, -8.f}, name == player::slugcat_name::count ? player::slugcat_name::white : name);
            auto step = [&](int x, int y, bool j) {
                auto const in = player::make_input(float(x), float(y), j, false, false);
                simulation::tick(gs, std::span{&p, 1}, std::span{&in, 1});
            };
            auto const anim = [&] { return w.player_get_animation(p); };
            auto const head = [&] { return world::room_view::get_tile_position(w.body_chunk_get_pos(player::body_chunk(gs, p, 0))); };
            for (int t = 0; t < 30 && anim() != player::animation_index::climb_on_beam; ++t)
                step(0, 1, false);
            for (int t = 0; t < 300 && head().y < cy && anim() != player::animation_index::beam_tip; ++t)
                step(0, 1, false);
            if (mode == 1 || mode == 4)
                for (int t = 0; t < 100 && anim() != player::animation_index::beam_tip; ++t)
                    step(0, 1, false);
            std::printf("before: head %d,%d anim %s\n", head().x, head().y, player::to_string(anim()));
            int best_y = 0;
            for (int t = 0; t < 160; ++t) {
                static int phase = 0, cool = 0;
                if (mode == 4 || mode == 5) {
                    // (4 from the tip, 5 off the side) into the wall on `dir`'s side, off it the other way holding up
                    if (phase == 0) {
                        step(dir, 0, mode == 4 ? t < 20 : (t >= 6 && t < 16));
                        if (t > 4 && (w.player_get_body_mode(p) == player::body_mode_index::wall_climb
                            || w.body_chunk_get_contact_point(player::body_chunk(gs, p, 0)).x == dir))
                            phase = 1;
                    } else {
                        if (cool < 3)
                            step(dir, 0, false);
                        else
                            step(-dir, 1, cool < 13);
                        ++cool;
                    }
                } else if (mode == 3) {
                    // off the pole towards `dir`, back towards the wall behind, off it towards `dir` again
                    auto const ch0 = player::body_chunk(gs, p, 0);
                    if (phase == 0) {
                        step(dir, 0, t >= 6 && t < 16);
                        if (t >= 6 + turn)
                            phase = 1;
                    } else if (phase == 1) {
                        step(-dir, 1, false);
                        if (w.player_get_body_mode(p) == player::body_mode_index::wall_climb || w.body_chunk_get_contact_point(ch0).x == -dir
                            || w.player_get_animation(p) == player::animation_index::ledge_grab)
                            phase = 2;
                    } else {
                        if (cool < 2)
                            step(-dir, 0, false);
                        else
                            step(dir, cool > 6 ? 1 : 0, cool < 12);
                        ++cool;
                    }
                } else if (mode == 2) {
                    // off the pole into the wall on `dir`'s side, off the wall the other way
                    if (phase == 0) {
                        step(dir, 0, t >= 6 && t < 16);
                        if (t > 8 && (w.player_get_body_mode(p) == player::body_mode_index::wall_climb
                            || w.body_chunk_get_contact_point(player::body_chunk(gs, p, 0)).x == dir))
                            phase = 1;
                    } else {
                        bool const past = (w.body_chunk_get_pos(player::body_chunk(gs, p, 0)).x - (float(px) * 20.f + 10.f)) * float(-dir) > 12.f;
                        // into the wall with jump let go, then jump off it
                        if (cool < 3)
                            step(dir, 0, false);
                        else
                            step(-dir, past ? 1 : 0, cool < 13);
                        ++cool;
                    }
                } else if (mode == 0)
                    step(dir, t < 8 ? 0 : 1, t >= 6 && t < 16);
                else
                    step(dir, t > 20 ? 1 : 0, t < 20);
                best_y = std::max(best_y, world::room_view::get_tile_position(w.body_chunk_get_pos(player::body_chunk(gs, p, 1))).y);
                if (t % 10 == 0)
                    std::printf("  t=%d head %d,%d feet %d,%d anim %s mode %s\n", t, head().x, head().y,
                        world::room_view::get_tile_position(w.body_chunk_get_pos(player::body_chunk(gs, p, 1))).x,
                        world::room_view::get_tile_position(w.body_chunk_get_pos(player::body_chunk(gs, p, 1))).y, player::to_string(anim()),
                        player::to_string(w.player_get_body_mode(p)));
            }
            return 0;
        }
        gs.spawn_dens = false;
        gs.npc_shelter_chance = 0.f;
        gs.rain_frozen = true;
        simulation::populate(gs);
        auto const from = gs.find_room(from_name), to = gs.find_room(to_name);
        if (!from || !to || !w.room_get_is_shelter(from) || !w.room_get_is_shelter(to)) {
            std::printf("FAIL: %s and %s must both be shelters\n", from_name.c_str(), to_name.c_str());
            return 1;
        }
        auto const view = world::get_room_view(gs, from);
        rw::rng rng{seed | 1u};
        auto const p = npc::create(gs, from, find_spawn(view, vec2{view.pixel_width() * 0.5f, view.pixel_height() * 0.5f}), rng, {}, name);
        auto const n = npc::of(gs, p);
        auto const c = player::creature_of(gs, p);
        w.slugcat_npc_set_target(n, to);
        w.slugcat_npc_get_nav(n).goal_timer = 0;
        if (!npc::debug().route.empty())
            std::printf("route:\n%s", npc::debug_route(gs, npc::debug().route).c_str());
        auto const reachable = npc::reachable_shelters(gs, p);
        bool const sees = std::find(reachable.begin(), reachable.end(), to) != reachable.end();
        std::printf("%s from %s to %s: %zu shelters reachable, %s among them\n", npc::describe(gs, p).c_str(), from_name.c_str(), to_name.c_str(),
            reachable.size(), sees ? "the target" : "NOT the target");
        auto const room_name = [&](dcon::room_id r) { return r ? std::string{gs.to_string_view(w.room_get_name(r))} : std::string{"-"}; };
        auto const fail = [&](uint32_t t, char const* why) {
            auto const r = rooms::room_of(gs, p);
            auto const at = world::room_view::get_tile_position(w.body_chunk_get_pos(player::body_chunk(gs, p, 1)));
            std::printf("FAIL at tick %u: %s in %s at tile %d,%d: %s\n", t, why, room_name(r).c_str(), at.x, at.y, npc::describe(gs, p).c_str());
            if (r)
                std::printf("%s", npc::debug_room(gs, r, at).c_str());
            return 1;
        };
        if (!sees && !npc::debug().guess) {
            std::printf("%s%s", npc::debug_routes(gs, p).c_str(), npc::debug_reach(gs, p).c_str());
            return fail(0, "no way to the target");
        }
        uint32_t const no_way = npc::stats().no_way, stuck = npc::stats().stuck;
        dcon::room_id last;
        for (uint32_t t = 0; t < ticks; ++t) {
            simulation::tick(gs, {}, {});
            gs.sounds.clear();
            gs.music.clear();
            auto const r = rooms::room_of(gs, p);
            if (r != last && !player::in_shortcut(gs, p)) {
                auto const& trip = w.creature_get_trip(c);
                std::printf("  tick %6u: %s (trip %u/%u)\n", t, room_name(r).c_str(), uint32_t(trip.next), uint32_t(trip.hops));
                last = r;
            }
            if (auto const& tr = npc::debug().trace_room; !tr.empty() && r && (tr == "*" || room_name(r) == tr) && t % uint32_t(std::max(1, npc::debug().trace_every)) == 0) {
                auto const& nav = w.slugcat_npc_get_nav(n);
                vec2 const a = w.body_chunk_get_pos(player::body_chunk(gs, p, 0)), b = w.body_chunk_get_pos(player::body_chunk(gs, p, 1));
                auto const& in = w.player_get_input(p)[0];
                std::printf("    t=%u in(%d,%d,%d) cj %d wj %d flip %d mt %d %s step %d->%d,%d aim %d,%d goal %d,%d best %u stuck %d mode %s anim %s head %.0f,%.0f (%d,%d) feet (%d,%d)\n", t,
                    in.x, in.y, int(in.jmp), w.player_get_can_jump(p), w.player_get_want_to_jump(p), w.player_get_flip_direction(p), nav.move_timer, npc::describe(gs, p).c_str(),
                    int(nav.last_kind), nav.last_step.x, nav.last_step.y, nav.aim.x, nav.aim.y,
                    nav.goal_tile.x, nav.goal_tile.y, uint32_t(nav.best), nav.stuck, player::to_string(w.player_get_body_mode(p)), player::to_string(w.player_get_animation(p)), a.x, a.y,
                    world::room_view::get_tile_position(a).x, world::room_view::get_tile_position(a).y, world::room_view::get_tile_position(b).x, world::room_view::get_tile_position(b).y);
            }
            if (player::dead(gs, p) || w.player_get_eaten(p))
                return fail(t, "died");
            if (!player::in_shortcut(gs, p) && w.body_chunk_get_pos(player::body_chunk(gs, p, 1)).y < -40.f)
                return fail(t, "fell out of the room (a death in the game)");
            // (a guessed trip finds out where it can't go by getting stuck)
            if (npc::stats().no_way != no_way && !npc::debug().guess)
                return fail(t, "found no way on");
            if (npc::stats().stuck != stuck && !npc::debug().guess)
                return fail(t, "got stuck");
            if (r == to && w.slugcat_npc_get_nav(n).what == npc::goal::rest) {
                std::printf("PASS: in %s after %u ticks (%.1f s), %s\n", to_name.c_str(), t, float(t) / 40.f, npc::stats_line(gs).c_str());
                return 0;
            }
        }
        return fail(ticks, "out of time");
    }

    int npc_soak(sys::game_state& gs, uint32_t ticks) {
        using clock = std::chrono::steady_clock;
        auto const t0 = clock::now();
        simulation::populate(gs);
        auto const t1 = clock::now();
        auto& w = gs.world;
        std::printf("populated in %.1f s (slugcat routes included)\n%s\n", std::chrono::duration<double>(t1 - t0).count(), npc::stats_line(gs).c_str());
        // the slugcats it made: how many shelters each could head for
        uint32_t none = 0, total = 0;
        w.for_each_slugcat_npc([&](dcon::slugcat_npc_id n) {
            ++total;
            bool const stuck = npc::reachable_shelters(gs, w.slugcat_npc_get_player(n)).empty();
            if (stuck && npc::debug().unreachable) {
                auto const p = w.slugcat_npc_get_player(n);
                if (none < 3)
                    std::printf("%s", npc::debug_routes(gs, p).c_str());
                std::printf("%s from %s %s", npc::describe(gs, p).c_str(), std::string{gs.to_string_view(w.room_get_name(rooms::room_of(gs, p)))}.c_str(),
                    npc::debug_reach(gs, p).c_str());
            }
            none += stuck ? 1 : 0;
        });
        std::printf("%u of %u NPC slugcats have no shelter they can get to\n", none, total);
        auto const& dbg = npc::debug();
        if (dbg.dead_ends)
            std::printf("%s", npc::debug_dead_ends(gs).c_str());
        if (auto const er = gs.find_room(dbg.edges_room); !dbg.edges_room.empty() && er) {
            auto const view = world::get_room_view(gs, er);
            for (int y = dbg.edges_lo.y; y <= dbg.edges_hi.y; ++y)
                for (int x = dbg.edges_lo.x; x <= dbg.edges_hi.x; ++x)
                    if (auto const j = pathing::debug_jumps(gs, view, rw::ivec2{x, y}); !j.empty())
                        std::printf("from %d,%d:\n%s", x, y, j.c_str());
        }
        if (auto const room = gs.find_room(dbg.room); !dbg.room.empty() && room)
            std::printf("%s", npc::debug_room(gs, room, rw::ivec2{-1, -1}).c_str());
        double ms = 0.;
        for (uint32_t t = 1; t <= ticks; ++t) {
            auto const a = clock::now();
            simulation::tick(gs, {}, {});
            ms += std::chrono::duration<double, std::milli>(clock::now() - a).count();
            gs.sounds.clear();
            gs.music.clear();
            // --npc-trace: every NPC in that room
            if (!dbg.trace_room.empty() && t % uint32_t(std::max(1, dbg.trace_every)) == 0)
                w.for_each_slugcat_npc([&](dcon::slugcat_npc_id n) {
                    auto const p = w.slugcat_npc_get_player(n);
                    auto const room = rooms::room_of(gs, p);
                    if (!room || (dbg.trace_room != "*" && gs.find_room(dbg.trace_room) != room) || player::dead(gs, p))
                        return;
                    auto const& nav = w.slugcat_npc_get_nav(n);
                    vec2 const feet = w.body_chunk_get_pos(player::body_chunk(gs, p, 1));
                    auto const& in = w.player_get_input(p)[0];
                    vec2 const head = w.body_chunk_get_pos(player::body_chunk(gs, p, 0));
                    std::printf("  t=%u #%u %s in %s in(%d,%d,%d) head (%.0f,%.0f) feet (%.0f,%.0f) mode %s anim %s step %d->%d,%d stuck %d fails %u\n", t,
                        uint32_t(n.index()), npc::describe(gs, p).c_str(), std::string{gs.to_string_view(w.room_get_name(room))}.c_str(), in.x, in.y, int(in.jmp),
                        head.x / 20.f, head.y / 20.f, feet.x / 20.f, feet.y / 20.f, player::to_string(w.player_get_body_mode(p)),
                        player::to_string(w.player_get_animation(p)), int(nav.last_kind), nav.last_step.x, nav.last_step.y, nav.stuck, uint32_t(nav.fails));
                });
            if (t % 1000 == 0 || t == ticks)
                std::printf("tick %u (%.2f ms/tick): %s\n", t, ms / double(t), npc::stats_line(gs).c_str());
        }
        // where the living ones are, by what they are doing
        uint32_t by[4] = {}, dead = 0, killed[creature::tmpl_count + 1] = {};
        w.for_each_slugcat_npc([&](dcon::slugcat_npc_id n) {
            auto const p = w.slugcat_npc_get_player(n);
            if (player::dead(gs, p)) {
                ++dead;
                uint8_t const k = w.creature_get_killed_by(player::creature_of(gs, p));
                ++killed[k == 0 || k == creature::killed_by_other ? creature::tmpl_count : k - 1u];
                return;
            }
            ++by[size_t(w.slugcat_npc_get_nav(n).what)];
        });
        std::printf("alive: %u starting, %u travelling, %u fleeing, %u resting; %u dead:", by[0], by[1], by[2], by[3], dead);
        for (uint32_t i = 0; i < creature::tmpl_count; ++i)
            if (killed[i])
                std::printf(" %s %u", creature::to_string(creature::tmpl(i)), killed[i]);
        if (killed[creature::tmpl_count])
            std::printf(" other (rain, falls...) %u", killed[creature::tmpl_count]);
        std::printf("\n");
        if (npc::debug().list)
            w.for_each_slugcat_npc([&](dcon::slugcat_npc_id n) {
                auto const p = w.slugcat_npc_get_player(n);
                auto const room = rooms::room_of(gs, p);
                auto const& nav = w.slugcat_npc_get_nav(n);
                std::printf("  %s in %s at (%.0f,%.0f) stuck %d fails %u\n", npc::describe(gs, p).c_str(), room ? std::string{gs.to_string_view(w.room_get_name(room))}.c_str() : "-",
                    w.body_chunk_get_pos(player::body_chunk(gs, p, 1)).x, w.body_chunk_get_pos(player::body_chunk(gs, p, 1)).y, nav.stuck, uint32_t(nav.fails));
            });
        return 0;
    }

    int fuzz(sys::game_state& gs, uint32_t ticks_per_room) {
        uint32_t bad = 0;
        auto& w = gs.world;
        // Every room is simulated all the time: one of each creature goes
        // in every room first, then slugcats visit the rooms a batch at a
        // time (one slugcat per room, every other room ticking along
        // unvisited), with random input
        constexpr uint32_t batch = 128;
        std::vector<world_creatures> creatures(size_t(w.room_size()));
        uint32_t lizards = 0, daddies = 0, bugs_in_ceiling = 0, leeches = 0, schools = 0;
        uint32_t centipedes_of[centipede::variant_count] = {}, shells_before = 0;
        w.for_each_room([&](dcon::room_id rid) {
            auto& c = creatures[size_t(rid.index())];
            c = spawn_creatures(gs, rid);
            lizards += c.lizard ? 1 : 0;
            daddies += c.daddy ? 1 : 0;
            if (c.dropbug && dropbug::sitting_in_ceiling(gs, c.dropbug))
                ++bugs_in_ceiling;
            if (c.centipede) {
                ++centipedes_of[uint32_t(c.centipede_variant)];
                shells_before += uint32_t(__builtin_popcount(w.centipede_data_get_shells(creature::centipede_data(w, c.centipede))));
            }
            for (auto const l : c.leeches)
                leeches += l ? 1 : 0;
            schools += c.leeches[0] ? 1 : 0;
        });
        // the world files' creatures (simulation::populate made them in their
        // dens) come out over the first seconds; the rain comes half way
        // through the visits: they go home, the slugcats in the open get
        // it, the next cycle lets the creatures back out
        uint32_t natural = 0;
        w.for_each_creature([&](dcon::creature_id c) { natural += w.creature_get_home_room(c) ? 1 : 0; });
        uint32_t const rain_at = uint32_t((size_t(w.room_size()) + batch - 1) / batch) * ticks_per_room / 2;
        w.for_each_region([&](dcon::region_id r) { rain::rain_in(gs, r, int32_t(rain_at)); });
        std::vector<dcon::player_id> players;
        for (uint32_t i = 0; i < batch; ++i)
            players.push_back(player::create(gs, dcon::room_id{0}, vec2{0.f, 0.f}));
        uint32_t music_requests = 0;
        uint32_t deaths = 0;
        uint32_t eaten = 0;
        uint32_t lost = 0;
        uint32_t bugs_dropped = 0, bugs_lost = 0;
        uint32_t worms_stuck = 0, worms_held = 0, worms_swung = 0, worms_lost = 0;
        uint32_t cicadas_sat = 0, cicadas_charged = 0, cicadas_lost = 0, cicadas_held = 0, cicadas_carried = 0;
        uint32_t lizards_hunted = 0, lizards_bit = 0, lizards_tongued = 0, lizards_jumped = 0, lizards_spat = 0, lizards_swam = 0, lizards_lost = 0;
        uint32_t daddies_gripped = 0, daddies_hunted = 0, daddies_held = 0, daddies_ate = 0, daddies_lost = 0;
        uint32_t wings_flew = 0, babies_held = 0, babies_eaten = 0, aquapedes_swam = 0;
        uint32_t leeches_latched = 0, leeches_drowned_slugcat = 0, leeches_lost = 0;
        // what happened in a visited room
        struct visit {
            dcon::room_id room;
            dcon::player_id p;
            rw::rng rng;
            int32_t ix = 0, iy = 0;
            bool jmp = false;
            bool here = true; // the slugcat hasn't left through a pipe
            bool bug_dropped = false, cicada_sat = false, cicada_charged = false, cicada_held = false;
            bool lizard_hunted = false, lizard_bit = false, lizard_tongued = false, lizard_jumped = false, lizard_spat = false, lizard_swam = false;
            bool daddy_gripped = false, daddy_hunted = false, daddy_held = false, daddy_ate = false;
            bool wing_flew = false, baby_held = false, baby_eaten = false, aqua_swam = false;
            bool leech_latched = false;
            bool worm_stuck = false, worm_held = false, worm_swung = false;
        };
        std::vector<dcon::room_id> all;
        w.for_each_room([&](dcon::room_id rid) { all.push_back(rid); });
        std::vector<player::input_package> inputs;
        for (size_t b0 = 0; b0 < all.size(); b0 += batch) {
            std::vector<visit> v;
            for (size_t i = b0; i < std::min(all.size(), b0 + batch); ++i) {
                auto const rid = all[i];
                auto const room = world::get_room_view(gs, rid);
                vec2 near{0.f, 0.f};
                w.room_for_each_camera_location(rid, [&](dcon::camera_location_id cl) {
                    if (near == vec2{0.f, 0.f})
                        near = w.room_camera_get_position(w.camera_location_get_camera(cl)) + vec2{gs.screen_width / 2.f, gs.screen_height / 2.f};
                });
                auto const p = players[i - b0];
                player::reset(gs, p, rid, find_spawn(room, near));
                player_graphics::reset(gs, p);
                v.push_back(visit{rid, p, rw::rng{uint32_t(rid.index()) * 0x9E3779B1u | 1u}});
            }
            std::span<dcon::player_id const> const driven{players.data(), v.size()};
            for (uint32_t t = 0; t < ticks_per_room; ++t) {
                inputs.clear();
                for (auto& x : v) {
                    auto& rng = x.rng;
                    if (t % 12 == 0) {
                        x.ix = rng.range(-1, 2);
                        x.iy = rng.range(-1, 2);
                        x.jmp = rng.value() < 0.3f;
                    }
                    inputs.push_back(x.here ? player::make_input(float(x.ix), float(x.iy), x.jmp, rng.value() < 0.05f, rng.value() < 0.05f) : player::input_package{});
                }
                simulation::tick(gs, driven, inputs);
                music_requests += uint32_t(gs.music.size());
                gs.music.clear();
                gs.sounds.clear();
                for (auto& x : v) {
                    if (!x.here)
                        continue;
                    if (simulation::player_room(gs, x.p) != x.room) {
                        x.here = false; // went through a pipe
                        continue;
                    }
                    auto const& c = creatures[size_t(x.room.index())];
                    bool finite = finite_chunks(gs, player::body(gs, x.p));
                    if (c.centipede) {
                        auto const cc = c.centipede;
                        if (creature::is(w, cc, creature::type::centipede)) {
                            finite = finite && finite_chunks(gs, centipede::body(gs, cc));
                            auto const& cp = w.creature_get_payload(cc).centipede;
                            x.wing_flew = x.wing_flew || (cp.variant == centipede::variant::wing && cp.flying);
                            x.baby_held = x.baby_held || (cp.variant == centipede::variant::small && creature::holds(w, player::creature_of(gs, x.p), cc));
                            x.aqua_swam = x.aqua_swam || (cp.variant == centipede::variant::aqua
                                && physics::object_submersion(gs, world::get_room_view(gs, x.room), centipede::body(gs, cc)) > 0.5f);
                        } else {
                            x.baby_eaten = x.baby_eaten || c.centipede_variant == centipede::variant::small;
                        }
                    }
                    for (auto const l : c.leeches)
                        if (l && creature::is(w, l, creature::type::leech)) {
                            finite = finite && finite_chunks(gs, leech::body(gs, l));
                            x.leech_latched = x.leech_latched || leech::attached_to(gs, l) == player::creature_of(gs, x.p);
                        }
                    if (c.dropbug) {
                        finite = finite && finite_chunks(gs, dropbug::body(gs, c.dropbug));
                        x.bug_dropped = x.bug_dropped || w.creature_get_payload(c.dropbug).dropbug.from_ceiling_jump;
                    }
                    if (c.tube_worm && creature::is(w, c.tube_worm, creature::type::tube_worm)) {
                        finite = finite && finite_chunks(gs, tube_worm::body(gs, c.tube_worm));
                        for (uint32_t i = 0; i < 2; ++i)
                            x.worm_stuck = x.worm_stuck || tube_worm::tongue_of(gs, c.tube_worm, i).mode == tube_worm::tongue_mode::attached_to_terrain;
                        x.worm_held = x.worm_held || tube_worm::held_by(gs, x.p) == c.tube_worm;
                        x.worm_swung = x.worm_swung || w.player_get_animation(x.p) == player::animation_index::grappling_swing;
                    }
                    if (c.cicada) {
                        finite = finite && finite_chunks(gs, cicada::body(gs, c.cicada));
                        x.cicada_sat = x.cicada_sat || !w.creature_get_payload(c.cicada).cicada.flying;
                        x.cicada_charged = x.cicada_charged || cicada::charging(gs, c.cicada);
                        x.cicada_held = x.cicada_held || cicada::held_by(gs, x.p) == c.cicada;
                    }
                    if (c.lizard) {
                        finite = finite && finite_chunks(gs, lizard::body(gs, c.lizard));
                        auto const l = c.lizard;
                        x.lizard_hunted = x.lizard_hunted || lizard::behavior(w.creature_get_behavior(l)) == lizard::behavior::hunt;
                        x.lizard_bit = x.lizard_bit || lizard::grasped(gs, l) == player::creature_of(gs, x.p);
                        x.lizard_tongued = x.lizard_tongued || w.lizard_data_get_tongue(creature::lizard_data(w, l)).state == lizard::tongue_state::lashing_out;
                        x.lizard_jumped = x.lizard_jumped || w.lizard_data_get_core(creature::lizard_data(w, l)).anim == lizard::animation::jumping;
                        x.lizard_spat = x.lizard_spat || w.lizard_data_get_core(creature::lizard_data(w, l)).anim == lizard::animation::spit;
                        x.lizard_swam = x.lizard_swam || w.lizard_data_get_core(creature::lizard_data(w, l)).swim > 0.5f;
                    }
                    if (c.daddy) {
                        auto const dll = c.daddy;
                        finite = finite && finite_chunks(gs, daddy::body(gs, dll));
                        for (uint32_t k = 0; k < daddy::tentacle_count(gs, dll); ++k)
                            for (auto const& tc : w.daddy_tentacle_get_chunks(daddy::tentacle_of(gs, dll, k)))
                                finite = finite && std::isfinite(tc.pos.x) && std::isfinite(tc.pos.y);
                        x.daddy_gripped = x.daddy_gripped || daddy::legs_gripping(gs, dll) > 0;
                        x.daddy_hunted = x.daddy_hunted || daddy::behavior(w.creature_get_behavior(dll)) == daddy::behavior::hunt;
                        x.daddy_held = x.daddy_held || daddy::holds(gs, dll, player::creature_of(gs, x.p));
                        x.daddy_ate = x.daddy_ate || daddy::eating(gs, dll, player::creature_of(gs, x.p));
                    }
                    if (!finite) {
                        std::printf("room %s: non finite position at tick %u\n", std::string{gs.to_string_view(w.room_get_name(x.room))}.c_str(), t);
                        ++bad;
                        x.here = false;
                    }
                }
            }
            for (auto& x : v) {
                auto const room = world::get_room_view(gs, x.room);
                auto const name = std::string{gs.to_string_view(w.room_get_name(x.room))};
                auto const& c = creatures[size_t(x.room.index())];
                auto const in_room = [&](dcon::physical_object_id o) { return w.physical_object_get_room_from_object_location(o) == x.room; };
                if (c.centipede && creature::is(w, c.centipede, creature::type::centipede) && in_room(centipede::body(gs, c.centipede))) {
                    vec2 const hp = w.body_chunk_get_pos(centipede::chunk(gs, c.centipede, 0));
                    if (left_room(room, hp) && ++lost <= 5)
                        std::printf("  centipede left %s at (%.0f, %.0f)\n", name.c_str(), hp.x, hp.y);
                }
                if (c.dropbug && in_room(dropbug::body(gs, c.dropbug))) {
                    vec2 const hp = w.body_chunk_get_pos(dropbug::chunk(gs, c.dropbug, 0));
                    if (left_room(room, hp) && ++bugs_lost <= 5)
                        std::printf("  dropwig left %s at (%.0f, %.0f)\n", name.c_str(), hp.x, hp.y);
                }
                bugs_dropped += x.bug_dropped ? 1 : 0;
                if (c.tube_worm && creature::is(w, c.tube_worm, creature::type::tube_worm) && in_room(tube_worm::body(gs, c.tube_worm))) {
                    vec2 const hp = w.body_chunk_get_pos(tube_worm::chunk(gs, c.tube_worm, 0));
                    if (left_room(room, hp) && ++worms_lost <= 5)
                        std::printf("  grapple worm left %s at (%.0f, %.0f)\n", name.c_str(), hp.x, hp.y);
                }
                worms_stuck += x.worm_stuck ? 1 : 0;
                worms_held += x.worm_held ? 1 : 0;
                worms_swung += x.worm_swung ? 1 : 0;
                if (c.cicada) {
                    vec2 const hp = w.body_chunk_get_pos(cicada::chunk(gs, c.cicada, 0));
                    if (!in_room(cicada::body(gs, c.cicada)) && dens::where_is(gs, c.cicada) == dens::where::room)
                        ++cicadas_carried; // through a pipe, in the slugcat's hands
                    else if (left_room(room, hp) && ++cicadas_lost <= 5)
                        std::printf("  squidcada left %s at (%.0f, %.0f)\n", name.c_str(), hp.x, hp.y);
                }
                cicadas_sat += x.cicada_sat ? 1 : 0;
                cicadas_charged += x.cicada_charged ? 1 : 0;
                cicadas_held += x.cicada_held ? 1 : 0;
                if (c.lizard && in_room(lizard::body(gs, c.lizard))) {
                    vec2 const hp = w.body_chunk_get_pos(lizard::chunk(gs, c.lizard, 0));
                    if (left_room(room, hp) && ++lizards_lost <= 5)
                        std::printf("  %s lizard left %s at (%.0f, %.0f)\n", lizard::to_string(c.breed), name.c_str(), hp.x, hp.y);
                }
                lizards_hunted += x.lizard_hunted ? 1 : 0;
                lizards_bit += x.lizard_bit ? 1 : 0;
                lizards_tongued += x.lizard_tongued ? 1 : 0;
                lizards_jumped += x.lizard_jumped ? 1 : 0;
                lizards_spat += x.lizard_spat ? 1 : 0;
                lizards_swam += x.lizard_swam ? 1 : 0;
                if (c.daddy && in_room(daddy::body(gs, c.daddy))) {
                    vec2 const hp = daddy::middle_of_body(gs, c.daddy);
                    if (left_room(room, hp) && ++daddies_lost <= 5)
                        std::printf("  %s long legs left %s at (%.0f, %.0f)\n", daddy::to_string(c.variant), name.c_str(), hp.x, hp.y);
                }
                daddies_gripped += x.daddy_gripped ? 1 : 0;
                daddies_hunted += x.daddy_hunted ? 1 : 0;
                daddies_held += x.daddy_held ? 1 : 0;
                daddies_ate += x.daddy_ate ? 1 : 0;
                wings_flew += x.wing_flew ? 1 : 0;
                babies_held += x.baby_held ? 1 : 0;
                babies_eaten += x.baby_eaten ? 1 : 0;
                aquapedes_swam += x.aqua_swam ? 1 : 0;
                leeches_latched += x.leech_latched ? 1 : 0;
                if (x.leech_latched && player::dead(gs, x.p))
                    ++leeches_drowned_slugcat;
                for (auto const l : c.leeches)
                    if (l && creature::is(w, l, creature::type::leech) && in_room(leech::body(gs, l))) {
                        vec2 const hp = w.body_chunk_get_pos(leech::chunk(gs, l, 0));
                        if (left_room(room, hp) && ++leeches_lost <= 5)
                            std::printf("  %s left %s at (%.0f, %.0f)\n", leech::to_string(c.leech_variant), name.c_str(), hp.x, hp.y);
                    }
                if (w.player_get_eaten(x.p))
                    ++eaten;
                else if (player::dead(gs, x.p))
                    ++deaths;
            }
        }
        // the creatures nobody visited for a while kept going too
        uint32_t unvisited_bad = 0;
        auto const check_all = [&](dcon::physical_object_id o) {
            if (!finite_chunks(gs, o))
                ++unvisited_bad;
        };
        w.for_each_creature([&](dcon::creature_id c) { check_all(creature::body(gs, c)); });
        if (unvisited_bad > 0)
            std::printf("%u creatures with a non finite position\n", unvisited_bad);
        bad += unvisited_bad;
        uint32_t const world_ticks = gs.tick;
        uint32_t shells_after = 0, leeches_dead = 0;
        for (auto& c : creatures) {
            if (c.centipede && creature::is(w, c.centipede, creature::type::centipede))
                shells_after += uint32_t(__builtin_popcount(w.centipede_data_get_shells(creature::centipede_data(w, c.centipede))));
            for (auto const l : c.leeches)
                leeches_dead += l && creature::is(w, l, creature::type::leech) && w.creature_get_dead(l) ? 1 : 0;
        }
        for (auto& c : creatures) {
            for (auto const t : creature::all_types)
                if (auto const id = c.of(t); id && creature::is(w, id, t))
                    creature::destroy(gs, id);
            for (auto const l : c.leeches)
                if (l && creature::is(w, l, creature::type::leech))
                    creature::destroy(gs, l);
        }

        uint32_t rocks = 0;
        w.for_each_item([&](dcon::item_id) { ++rocks; });
        std::printf("fuzzed %u rooms x %u ticks (%u ticks of the whole world, %u slugcats at a time), %u failures, %u rocks/spears spawned, %u music requests from %u triggers\n",
            w.room_size(), ticks_per_room, world_ticks, batch, bad, rocks, music_requests, w.music_trigger_size());
        uint32_t tolls = 0, strung = 0, loot = 0, pearls = 0;
        w.for_each_outpost([&](dcon::outpost_id o) { tolls += w.outpost_get_toll_spears(o); });
        w.for_each_pearl_string([&](dcon::pearl_string_id ps) { strung += w.pearl_string_get_pearls(ps).size(); });
        w.for_each_treasury([&](dcon::treasury_id t) { loot += w.treasury_get_loot(t); });
        w.for_each_item([&](dcon::item_id it) { pearls += w.item_get_type(it) == world::item_type::data_pearl ? 1 : 0; });
        std::printf("scavenger loot: %u outposts with %u toll spears and %u pearl strings (%u pearls), %u treasuries with %u items, %u data pearls placed in all\n",
            w.outpost_size(), tolls, w.pearl_string_size(), strung, w.treasury_size(), loot, pearls);
        std::printf("centipedes: one per room, %u fell out of their room, %u slugcats killed, %u path fields built\n", lost, deaths, pathing::builds());
        std::printf("centipede variants (in turn): %u plain, %u baby (%u picked up, %u eaten), %u centiwings (%u flew), %u red, %u aquapedes (%u swam); %u shells knocked off\n",
            centipedes_of[0], centipedes_of[1], babies_held, babies_eaten, centipedes_of[2], wings_flew, centipedes_of[3], centipedes_of[4], aquapedes_swam,
            shells_before - std::min(shells_before, shells_after));
        std::printf("leeches: %u in %u schools (every variant in turn, in the rooms with water), %u latched on to the slugcat (%u of those slugcats died), %u died, %u fell out of their room\n",
            leeches, schools, leeches_latched, leeches_drowned_slugcat, leeches_dead, leeches_lost);
        std::printf("dropwigs: one per room, %u placed in the ceiling, %u dropped from it, %u fell out of their room\n", bugs_in_ceiling, bugs_dropped, bugs_lost);
        std::printf("grapple worms: one per room, %u stuck a tongue to the terrain, %u picked up (%u swung from), %u fell out of their room\n",
            worms_stuck, worms_held, worms_swung, worms_lost);
        std::printf("squidcadas: one per room, %u sat down, %u bump attacked, %u picked up (%u carried through a pipe), %u fell out of their room\n",
            cicadas_sat, cicadas_charged, cicadas_held, cicadas_carried, cicadas_lost);
        std::printf("lizards: %u (one per room, every breed in turn), %u hunted, %u bit the slugcat, %u shot a tongue, %u jumped, %u spat, %u swam, %u fell out of their room\n",
            lizards, lizards_hunted, lizards_bit, lizards_tongued, lizards_jumped, lizards_spat, lizards_swam, lizards_lost);
        std::printf("daddy long legs: %u (one per room, every variant in turn), %u gripped the terrain, %u hunted, %u held the slugcat, %u ate it, %u fell out of their room\n",
            daddies, daddies_gripped, daddies_hunted, daddies_held, daddies_ate, daddies_lost);
        {
            uint32_t out = 0, in_pipes = 0, in_dens = 0, dead = 0;
            w.for_each_creature([&](dcon::creature_id c) {
                if (!w.creature_get_home_room(c))
                    return;
                auto const where = dens::where_is(gs, c);
                (where == dens::where::room ? out : where == dens::where::shortcut ? in_pipes : in_dens) += 1;
                dead += w.creature_get_dead(c) ? 1 : 0;
            });
            auto const& ds = dens::stats();
            auto const& rs = rain::stats();
            std::printf("rain: due after %u ticks in every region, %u death rains came and went (once per group of regions), %u slugcats and %u creatures killed by it\n",
                rain_at, rs.cycles_finished.load(), rs.players_killed.load(), rs.creatures_killed.load());
            std::printf("dens: %u creatures from the world files (at the end %u out, %u in pipes, %u in dens, %u dead); %u went into pipes, %u into dens, %u came out of dens, %u changed rooms, %u prey taken into a pipe (%u creatures eaten there)\n",
                natural, out, in_pipes, in_dens, dead, ds.into_pipes.load(), ds.into_dens.load(), ds.out_of_dens.load(), ds.room_changes.load(), ds.prey_taken.load(),
                ds.creatures_eaten.load());
            // the fights: kills by killer and victim template, the most
            // frequent pairs
            auto const& k = creature::kills();
            uint32_t total = 0, slugcats = 0;
            std::vector<std::tuple<uint32_t, creature::tmpl, creature::tmpl>> pairs;
            for (uint32_t a = 0; a < creature::tmpl_count; ++a)
                for (uint32_t b = 0; b < creature::tmpl_count; ++b)
                    if (uint32_t const n = k.n[a][b].load()) {
                        total += n;
                        slugcats += b == uint32_t(creature::tmpl::slugcat) ? n : 0;
                        pairs.emplace_back(n, creature::tmpl(a), creature::tmpl(b));
                    }
            std::sort(pairs.begin(), pairs.end(), [](auto const& x, auto const& y) { return std::get<0>(x) > std::get<0>(y); });
            std::string top;
            for (size_t i = 0; i < pairs.size() && i < 10; ++i)
                top += std::string{i ? ", " : ""} + creature::to_string(std::get<1>(pairs[i])) + " killed " + creature::to_string(std::get<2>(pairs[i])) + " x"
                    + std::to_string(std::get<0>(pairs[i]));
            std::printf("fights: %u kills by creatures (%u slugcats, %u creatures; %zu killer/victim pairs): %s\n", total, slugcats, total - slugcats, pairs.size(),
                top.empty() ? "none" : top.c_str());
        }
        uint32_t legs = 0, tendrils = 0, vines = 0, eyes = 0, max_bulbs = 0;
        w.for_each_rot_tube([&](dcon::rot_tube_id t) {
            auto const k = w.rot_tube_get_kind(t);
            (k == world::rot_tube_kind::leg ? legs : k == world::rot_tube_kind::tendril ? tendrils : vines) += 1;
        });
        w.for_each_rot_bulb([&](dcon::rot_bulb_id b) { eyes += w.rot_bulb_get_has_eye(b) ? 1 : 0; });
        w.for_each_rot_system([&](dcon::rot_system_id rs) {
            uint32_t n = 0;
            w.rot_system_for_each_rot_bulb_membership(rs, [&](dcon::rot_bulb_membership_id) { ++n; });
            max_bulbs = std::max(max_bulbs, n);
        });
        std::printf("rot: %u placed objects, %u rooms with rot, %u bulbs (%u eyes, at most %u in a room), %u legs, %u tendrils, %u vines, %u slugcats eaten\n",
            w.rot_placed_size(), w.rot_system_size(), w.rot_bulb_size(), eyes, max_bulbs, legs, tendrils, vines, eaten);

        // every connected room exit pipe: sit in the entrance, push into it and
        // expect to come out in the connected room (a batch of pipes at a time)
        struct pipe {
            dcon::room_id room;
            dcon::room_id target;
            ivec2 start;
            ivec2 dir;
        };
        std::vector<pipe> pipes;
        w.for_each_shortcut([&](dcon::shortcut_id sc) {
            if (w.shortcut_get_type(sc) != world::shortcut_type::room_exit)
                return;
            auto const exit = w.shortcut_get_exit_from_shortcut_destination(sc);
            dcon::room_id const target = exit ? dcon::room_id{w.room_exit_get_target_from_exit_connection(exit)} : dcon::room_id{};
            if (!target)
                return;
            auto const rid = w.shortcut_get_room_from_shortcut_location(sc);
            ivec2 const start = w.shortcut_get_start(sc);
            pipes.push_back(pipe{rid, target, start, world::get_room_view(gs, rid).shortcut_entrance_hole_direction(start)});
        });
        uint32_t arrived = 0;
        std::vector<std::string> failed_pipes;
        for (size_t b0 = 0; b0 < pipes.size(); b0 += batch) {
            size_t const n = std::min(pipes.size() - b0, size_t(batch));
            std::vector<bool> done(n, false);
            inputs.clear();
            for (size_t i = 0; i < n; ++i) {
                auto const& pp = pipes[b0 + i];
                player::reset(gs, players[i], pp.room, world::room_view::middle_of_tile(pp.start) - vec2{0.f, 8.f});
                inputs.push_back(player::make_input(float(-pp.dir.x), float(-pp.dir.y), false, false, false));
            }
            std::span<dcon::player_id const> const driven{players.data(), n};
            size_t left = n;
            for (uint32_t t = 0; t < 2400 && left > 0; ++t) {
                simulation::tick(gs, driven, inputs);
                gs.music.clear();
                gs.sounds.clear();
                for (size_t i = 0; i < n; ++i)
                    if (!done[i] && simulation::player_room(gs, players[i]) == pipes[b0 + i].target && !player::in_shortcut(gs, players[i])) {
                        done[i] = true;
                        inputs[i] = player::input_package{};
                        --left;
                        ++arrived;
                    }
            }
            for (size_t i = 0; i < n; ++i)
                if (!done[i] && failed_pipes.size() < 10)
                    failed_pipes.push_back(std::string{gs.to_string_view(w.room_get_name(pipes[b0 + i].room))} + " (" + std::to_string(pipes[b0 + i].start.x) + ","
                        + std::to_string(pipes[b0 + i].start.y) + ")");
        }
        std::printf("pipes: %u of %zu connected room exits delivered the slugcat\n", arrived, pipes.size());
        {
            auto const& ds = dens::stats();
            // the trips, to the end (the pipes' ticks included): rooms and
            // regions changed
            uint32_t rooms_changed = 0, travellers = 0, elsewhere = 0;
            w.for_each_creature([&](dcon::creature_id c) {
                rooms_changed += w.creature_get_rooms_changed(c);
                travellers += w.creature_get_rooms_changed(c) > 0 ? 1 : 0;
                auto const home = w.creature_get_home_room(c);
                auto const at = dens::where_is(gs, c) == dens::where::room ? creature::room_of(gs, c) : w.creature_get_away_room(c);
                if (home && at && w.room_get_region_from_region_membership(home) != w.room_get_region_from_region_membership(at))
                    ++elsewhere;
            });
            std::printf("travel: %u trips planned (%u to another region, %u through a warp), %u rooms entered by %u creatures still alive, %u carried by a warp, %u region changes through gates, %u through warps; "
                        "%u creatures in another region than their den at the end\n",
                ds.trips.load(), ds.cross_trips.load(), ds.warp_trips.load(), rooms_changed, travellers, ds.warp_carried.load(), ds.gated_regions.load(), ds.warped_regions.load(), elsewhere);
        }
        for (auto const& f : failed_pipes)
            std::printf("  pipe stuck: %s\n", f.c_str());
        for (auto const p : players)
            player::destroy(gs, p);
        return bad == 0 ? 0 : 1;
    }

    int bench(std::function<std::unique_ptr<sys::game_state>()> const& load, uint32_t ticks, std::string const& mode) {
        // the world as it is without creatures (items, rot, water, sand,
        // gates, the rain), then with a creature of each kind in every room,
        // then with the world files' creatures (out of their dens as they
        // come); each twice: one thread, then every thread
        uint32_t const cores = std::max(1u, std::thread::hardware_concurrency());
        bool same = true;
        char const* const modes[] = {"none", "each", "dens"};
        for (int m = 0; m < 3; ++m) {
        if (!mode.empty() && mode != modes[m])
            continue;
        uint64_t hashes[2] = {};
        double ms[2] = {};
        for (int run = 0; run < 2; ++run) {
            auto gs_ptr = load();
            if (!gs_ptr)
                return 1;
            auto& gs = *gs_ptr;
            auto& w = gs.world;
            gs.spawn_dens = m == 2;
            simulation::populate(gs);
            uint32_t creatures = 0;
            if (m == 1)
                w.for_each_room([&](dcon::room_id rid) {
                    auto const c = spawn_creatures(gs, rid);
                    creatures += (c.centipede ? 1 : 0) + (c.dropbug ? 1 : 0) + (c.cicada ? 1 : 0) + (c.lizard ? 1 : 0) + (c.daddy ? 1 : 0);
                    for (auto const l : c.leeches)
                        creatures += l ? 1 : 0;
                });
            if (m == 2)
                w.for_each_creature([&](dcon::creature_id) { ++creatures; });
            // one slugcat, viewed, standing in the first room
            auto const first = dcon::room_id{0};
            auto const room = world::get_room_view(gs, first);
            auto const p = player::create(gs, first, find_spawn(room, vec2{room.pixel_width() / 2.f, room.pixel_height() / 2.f}));
            uint32_t const threads = run == 0 ? 1u : cores;
            tbb::global_control limit{tbb::global_control::max_allowed_parallelism, threads};
            player::input_package const in{};
            // a few ticks to settle (path fields, first falls) before timing;
            // the den population gets 1000 (25 s): their remainInDenCounters
            // (100 to 1000 ticks) have run out, every creature that wants to
            // is out
            for (uint32_t t = 0; t < (m == 2 ? 1000u : 10u); ++t) {
                simulation::tick(gs, std::span{&p, 1}, std::span{&in, 1});
                gs.sounds.clear();
                gs.music.clear();
            }
            uint32_t const builds0 = pathing::builds();
            simulation::times() = {};
            auto const t0 = std::chrono::steady_clock::now();
            for (uint32_t t = 0; t < ticks; ++t) {
                simulation::tick(gs, std::span{&p, 1}, std::span{&in, 1});
                gs.sounds.clear();
                gs.music.clear();
            }
            auto const t1 = std::chrono::steady_clock::now();
            ms[run] = std::chrono::duration<double, std::milli>(t1 - t0).count() / double(std::max(1u, ticks));
            hashes[run] = simulation::hash(gs);
            uint32_t chunks = 0;
            w.for_each_body_chunk([&](dcon::body_chunk_id) { ++chunks; });
            uint32_t out = 0, in_pipes = 0, in_dens = 0;
            w.for_each_creature([&](dcon::creature_id c) {
                if (creature::is_slugcat(w, c))
                    return;
                auto const where = dens::where_is(gs, c);
                (where == dens::where::room ? out : where == dens::where::shortcut ? in_pipes : in_dens) += 1;
            });
            std::printf("[%s] %u rooms, %u creatures (at the end %u out, %u in pipes, %u in dens), %u items, %u body chunks; %u thread%s: %.2f ms/tick (%.1f ticks/s), %.1f path fields built a tick, world hash %016llx\n",
                modes[m], w.room_size(), creatures, out, in_pipes, in_dens, w.item_size(), chunks, threads, threads == 1 ? "" : "s", ms[run], 1000.0 / ms[run],
                double(pathing::builds() - builds0) / double(std::max(1u, ticks)), (unsigned long long)hashes[run]);
            auto const& tt = simulation::times();
            std::printf("  per tick: %.2f ms serial before, %.2f ms rooms (parallel), %.2f ms serial after\n", tt.serial_before / tt.ticks,
                tt.rooms / tt.ticks, tt.serial_after / tt.ticks);
            std::fflush(stdout);
        }
        same = same && hashes[0] == hashes[1];
        std::printf("%u cores: %.2fx faster on every thread; 1 thread and %u threads %s\n", cores, ms[0] / ms[1], cores,
            hashes[0] == hashes[1] ? "agree (deterministic)" : "DISAGREE");
        }
        return same ? 0 : 1;
    }
}

namespace {
    // what ctest takes for a skipped test
    constexpr int skipped = 77;

    void print_usage(char const* argv0) {
        std::printf(
            "usage: %s [options] <one of the checks>\n"
            "the checks, on the game's world (exit %d when its data isn't found):\n"
            "  --fuzz [ticks]      soak test the slugcat in every room\n"
            "  --bench [ticks]     time the whole world on one thread and on every thread, and\n"
            "                      whether both end the same: without creatures, one of each kind\n"
            "                      in every room, the world files' den population\n"
            "  --bench-mode <m>    only one of them: none, each, dens\n"
            "  --npc-soak [ticks]  the world with its NPC slugcats (--npcs) for that many ticks, headless:\n"
            "                      how they get on and what they cost\n"
            "  --npc-trip <from> <to> [ticks]  one NPC slugcat (--slugcat, else random; --npc-seed) from\n"
            "                      shelter to shelter, no creatures, the rain frozen: pass or fail\n"
            "options:\n"
            "  --root <dir>        add a StreamingAssets-like root (repeatable, first has priority)\n"
            "                      default: $RW_ROOT or the Steam install\n"
            "  --slugcat <name>    the campaign (and --npc-trip's slugcat)\n"
            "  --npc-seed <n>      the --npc-trip slugcat's rolls\n"
            "  --no-dens           no world file creatures in their dens\n"
            "  --no-friendly-fire  a slugcat's thrown weapons pass through slugcats (NPCs included)\n"
            "  --npcs <share>      the share of shelters with a group of NPC slugcats (0..1, default 0.25)\n"
            "  --no-npcs           no NPC slugcats\n"
            "  NPC tracing and debugging (docs/SlugAI.md), with --npc-trip or --npc-soak (and rwcpp_selftest):\n"
            "  --npc-trace <room|*>[,every]  the NPC's state, input and move every few ticks in that room\n"
            "  --npc-route <r1,r2,...>  whether each room of a route can be walked from the way in to the way out\n"
            "  --npc-maps          with --npc-route, a reachability map of each room that can't\n"
            "  --npc-room <room>   dump a room's poles, pipes and walkable tiles (--npc-soak)\n"
            "  --npc-goal <exit>   with --npc-room, mark what reaches that exit; --npc-box <x0,y0,x1,y1>\n"
            "                      lists the walkable tiles there with their distance to it\n"
            "  --npc-edges <room,x0,y0,x1,y1>  the jumps leaving those tiles (--npc-soak)\n"
            "  --npc-dead-ends     the exits no other exit can be walked to from (--npc-soak)\n"
            "  --npc-unreachable   why the first NPCs with no shelter in reach have none (--npc-soak)\n"
            "  --npc-list          every NPC at the end of --npc-soak\n"
            "  --npc-guess         --npc-trip even when no walkable way is known (a guessed trip)\n"
                        "  --npc-exp <room,x,y,mode,dir[,turn]>  with --npc-trip: a scripted jump off a pole\n"
            "  --npc-soak [ticks]  the world with its NPC slugcats (--npcs) for that many ticks, headless:\n"
            "                      how they get on and what they cost, then exit\n"
            , argv0, skipped);
    }
}

int main(int argc, char* argv[]) {
    std::vector<std::string> roots;
    int32_t fuzz_ticks = -1;
    int32_t bench_ticks = -1;
    int32_t npc_soak_ticks = -1;
    std::string npc_trip_from, npc_trip_to;
    uint32_t npc_trip_ticks = 40 * 60 * 10;
    uint32_t npc_seed = 1;
    bool slugcat_given = false;
    std::string bench_mode;
    bool no_dens = false;
    bool friendly_fire = true;
    float npc_chance = npc::default_shelter_chance;
    player::slugcat_name slugcat = player::slugcat_name::white;
    for (int i = 1; i < argc; ++i) {
        std::string const a = argv[i];
        auto const next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        auto const optional_count = [&](int32_t fallback) {
            return i + 1 < argc && argv[i + 1][0] != '-' ? std::atoi(argv[++i]) : fallback;
        };
        if (a == "--root") {
            roots.push_back(next());
        } else if (a == "--slugcat") {
            slugcat = startup::parse_slugcat(next());
            slugcat_given = true;
        } else if (a == "--fuzz") {
            fuzz_ticks = optional_count(400);
        } else if (a == "--bench") {
            bench_ticks = optional_count(200);
        } else if (a == "--bench-mode") {
            bench_mode = next();
        } else if (a == "--npc-soak") {
            npc_soak_ticks = optional_count(6000);
        } else if (a == "--npc-trip") {
            npc_trip_from = next();
            npc_trip_to = next();
            npc_trip_ticks = uint32_t(optional_count(int32_t(npc_trip_ticks)));
        } else if (a == "--npc-seed") {
            npc_seed = uint32_t(std::atoi(next().c_str()));
        } else if (a == "--no-dens") {
            no_dens = true;
        } else if (a == "--no-friendly-fire") {
            friendly_fire = false;
        } else if (a == "--npcs") {
            npc_chance = float(std::atof(next().c_str()));
        } else if (a == "--no-npcs") {
            npc_chance = 0.f;
        } else if (a.starts_with("--npc-") && startup::npc_debug_option(a, next)) {
        } else {
            print_usage(argv[0]);
            return a == "--help" || a == "-h" ? 0 : 1;
        }
    }
    if (fuzz_ticks < 0 && bench_ticks < 0 && npc_soak_ticks < 0 && npc_trip_from.empty()) {
        print_usage(argv[0]);
        return 1;
    }
    if (roots.empty()) {
        auto const sa = startup::find_streaming_assets();
        if (sa.empty()) {
            reports::add_log("can't find Rain World's StreamingAssets, pass --root or set RW_ROOT: skipped");
            return skipped;
        }
        roots.push_back(sa);
    }
    if (bench_ticks >= 0) {
        bool first = true;
        return selftest::bench([&] {
            auto ptr = startup::load_world(roots, false, first);
            ptr->story_slugcat = slugcat;
            first = false;
            return ptr;
        }, uint32_t(bench_ticks), bench_mode);
    }
    auto const gs_ptr = startup::load_world(roots, false, true);
    auto& gs = *gs_ptr;
    gs.spawn_dens = !no_dens;
    gs.friendly_fire = friendly_fire;
    // (not for the fuzz: it drives slugcats of its own)
    gs.npc_shelter_chance = fuzz_ticks >= 0 ? 0.f : npc_chance;
    gs.story_slugcat = slugcat;
    if (!npc_trip_from.empty())
        return selftest::npc_trip(gs, npc_trip_from, npc_trip_to, npc_trip_ticks, slugcat_given ? slugcat : player::slugcat_name::count, npc_seed);
    if (npc_soak_ticks >= 0)
        return selftest::npc_soak(gs, uint32_t(npc_soak_ticks));
    simulation::populate(gs);
    return selftest::fuzz(gs, uint32_t(fuzz_ticks));
}
