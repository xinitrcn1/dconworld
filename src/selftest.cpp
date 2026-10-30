#include <algorithm>
#include <cmath>
#include <span>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>
#include <chrono>
#include <memory>
#include <thread>

#include <tbb/global_control.h>

#include "selftest.hpp"
#include "audio.hpp"
#include "physics.hpp"
#include "centipede.hpp"
#include "cicada.hpp"
#include "dropbug.hpp"
#include "leech.hpp"
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
#include "rain.hpp"
#include "spawns.hpp"
#include "world.hpp"

namespace selftest {
    using rw::ivec2;
    using rw::vec2;
    using world::terrain;

    rw::vec2 find_spawn(world::room_view const& room, rw::vec2 near) {
        // air tile with ground below and two free tiles above, closest to `near`
        float best = std::numeric_limits<float>::max();
        vec2 result{room.pixel_width() / 2.f, room.pixel_height() / 2.f};
        for (int32_t y = 1; y < room.height - 2; ++y) {
            for (int32_t x = 1; x < room.width - 1; ++x) {
                auto const t = room.get_tile(x, y);
                auto const below = room.get_terrain(x, y - 1);
                if (t.t != terrain::air || t.shortcut != 0 || (below != terrain::solid && below != terrain::floor))
                    continue;
                if (room.get_terrain(x, y + 1) != terrain::air || room.get_terrain(x, y + 2) != terrain::air)
                    continue;
                vec2 const p = world::room_view::middle_of_tile(x, y);
                float const d = rw::distance(p, near);
                if (d < best) {
                    best = d;
                    result = vec2{p.x, p.y - 10.f + 8.f};
                }
            }
        }
        return result;
    }

    namespace {
        bool custom_dist_ok(vec2 a, vec2 b) {
            return rw::custom::dist_less(a, b, 30.f);
        }

        // Builds a level file from ASCII art (top row first) and loads it
        // through the real parser. '#' solid, '/' slope, '=' floor, '|' vertical
        // beam, '-' horizontal beam, '+' both beams, ' ' air; pipes: 'S' an
        // entrance, '.' a pipe through the rock, 'D' a den at its end, 'E' a
        // room exit at its end.
        dcon::room_id make_room(sys::game_state& gs, dcon::region_id region, std::vector<std::string> const& rows, int32_t water_level = -1) {
            int32_t const h = int32_t(rows.size());
            int32_t const wd = int32_t(rows[0].size());
            std::string txt = "TEST\n" + std::to_string(wd) + "*" + std::to_string(h) + "|" + std::to_string(water_level) + "|0\n";
            txt += "0*0|0|0\n0,0\nBorder: Solid\n\n\n\n\n0\n\n";
            for (int32_t x = 0; x < wd; ++x) {
                for (int32_t r = 0; r < h; ++r) {
                    switch (rows[size_t(r)][size_t(x)]) {
                    case '#': txt += "1"; break;
                    case '/': txt += "2"; break;
                    case '=': txt += "3"; break;
                    case '|': txt += "0,1"; break;
                    case '-': txt += "0,2"; break;
                    case '+': txt += "0,1,2"; break;
                    case 'S': txt += "4,3"; break;
                    case '.': txt += "1,3"; break;
                    case 'D': txt += "1,5"; break;
                    case 'E': txt += "1,4"; break;
                    default: txt += "0"; break;
                    }
                    txt += "|";
                }
            }
            txt += "\n";
            parsers::error_handler err;
            auto const rid = world::add_room(gs, "test_" + std::to_string(gs.world.room_size()), region, txt, err);
            if (!rid || err.error_count > 0)
                std::printf("make_room failed:\n%s", err.accumulated_errors.c_str());
            else
                gs.world.room_set_items_spawned(rid, true); // tests place their own rocks
            return rid;
        }

        // a rot placed object, as if read from the room's settings
        void add_rot(sys::game_state& gs, dcon::room_id room, world::rot_kind kind, vec2 pos, vec2 handle, world::rot_custom_data const& custom = {}) {
            auto const o = gs.world.create_rot_placed();
            gs.world.rot_placed_set_kind(o, kind);
            gs.world.rot_placed_set_pos(o, pos);
            gs.world.rot_placed_set_handle(o, handle);
            gs.world.rot_placed_set_custom(o, custom);
            gs.world.try_create_rot_placed_location(o, room);
        }

        // a Watcher warp point, as if read from the room's settings
        struct warp_desc {
            vec2 pos;
            dcon::room_id dest;
            vec2 dest_pos;
            bool one_way = false;
            bool one_way_entrance = false;
            bool ripple = false;
            int16_t uses = -1; // limited use when >= 0
        };
        void add_warp(sys::game_state& gs, dcon::room_id room, warp_desc const& d) {
            auto& w = gs.world;
            auto const o = w.create_warp_placed();
            w.warp_placed_set_pos(o, d.pos);
            if (d.dest) {
                w.warp_placed_set_dest_room(o, w.room_get_name(d.dest));
                w.warp_placed_set_dest_pos(o, d.dest_pos);
            }
            w.warp_placed_set_one_way(o, d.one_way);
            w.warp_placed_set_one_way_entrance(o, d.one_way_entrance);
            w.warp_placed_set_ripple_warp(o, d.ripple);
            w.warp_placed_set_limited_use(o, d.uses >= 0);
            w.warp_placed_set_uses(o, d.uses);
            w.warp_placed_set_effect(o, world::warp_effect{});
            w.try_create_warp_placed_location(o, room);
        }

        struct sim {
            sys::game_state& gs;
            world::room_view room;
            dcon::player_id p;
            uint32_t ticks = 0;

            sim(sys::game_state& gs, dcon::room_id rid, vec2 feet, player::slugcat_name name = player::slugcat_name::white)
                : gs{gs}, room{world::get_room_view(gs, rid)} {
                p = player::create(gs, rid, feet, name);
                gs.world.player_set_flip_direction(p, 1);
            }
            ~sim() { player::destroy(gs, p); }
            dcon::body_chunk_id chunk(int i) const { return player::body_chunk(gs, p, uint32_t(i)); }
            vec2 pos(int i) const { return gs.world.body_chunk_get_pos(chunk(i)); }
            vec2 vel(int i) const { return gs.world.body_chunk_get_vel(chunk(i)); }
            rw::ivec2 contact(int i) const { return gs.world.body_chunk_get_contact_point(chunk(i)); }
            player::body_mode_index body_mode() const { return gs.world.player_get_body_mode(p); }
            player::animation_index animation() const { return gs.world.player_get_animation(p); }
            int32_t stun() const { return gs.world.player_get_stun(p); }
            bool dead() const { return gs.world.player_get_dead(p) != 0; }
            void step(int32_t x = 0, int32_t y = 0, bool jmp = false, bool thrw = false, bool pckp = false) {
                auto const in = player::make_input(float(x), float(y), jmp, thrw, pckp);
                simulation::tick(gs, std::span{&p, 1}, std::span{&in, 1});
                ++ticks;
            }
            void run(uint32_t n, int32_t x = 0, int32_t y = 0, bool jmp = false) {
                for (uint32_t i = 0; i < n; ++i)
                    step(x, y, jmp);
            }
            // run until the predicate holds, at most n ticks
            bool until(uint32_t n, std::function<bool()> const& pred, int32_t x = 0, int32_t y = 0, bool jmp = false, bool pckp = false) {
                for (uint32_t i = 0; i < n; ++i) {
                    if (pred())
                        return true;
                    step(x, y, jmp, false, pckp);
                }
                return pred();
            }
            std::string describe() const {
                char buf[256];
                std::snprintf(buf, sizeof(buf), "t=%u mode=%s anim=%s c0=(%.1f,%.1f) c1=(%.1f,%.1f) v0=(%.2f,%.2f) stun=%d",
                    ticks, player::to_string(body_mode()), player::to_string(animation()),
                    pos(0).x, pos(0).y, pos(1).x, pos(1).y, vel(0).x, vel(0).y, stun());
                return buf;
            }
        };

        struct results {
            int passed = 0;
            int failed = 0;
            void check(char const* name, bool ok, std::string const& detail) {
                std::printf("[%s] %-28s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
                (ok ? passed : failed)++;
            }
        };

        std::vector<std::string> box(int32_t w, int32_t h, std::function<char(int32_t, int32_t)> const& interior) {
            std::vector<std::string> rows(size_t(h), std::string(size_t(w), ' '));
            for (int32_t r = 0; r < h; ++r)
                for (int32_t x = 0; x < w; ++x) {
                    int32_t const y = h - 1 - r; // y up
                    rows[size_t(r)][size_t(x)] = (x == 0 || y == 0 || x == w - 1 || y == h - 1) ? '#' : interior(x, y);
                }
            return rows;
        }
        vec2 feet_at(int32_t tx, int32_t ty) {
            return vec2{10.f + float(tx) * 20.f, float(ty) * 20.f + 8.f};
        }

        // a 40x20 room on a two tile thick floor with pipes into the floor:
        // `den_x` a den's entrance (0 none), `exit_x` a room exit's (0 none);
        // each pipe goes down one tile and ends beside it, away from the
        // room's middle
        std::vector<std::string> pipe_room(int32_t den_x, int32_t exit_x, std::function<char(int32_t, int32_t)> const& extra = nullptr) {
            return box(40, 20, [=](int32_t x, int32_t y) {
                for (auto const& [px, end] : {std::pair{den_x, 'D'}, std::pair{exit_x, 'E'}}) {
                    if (px == 0)
                        continue;
                    int32_t const side = px < 20 ? -1 : 1;
                    if (x == px && y == 2)
                        return 'S';
                    if (x == px && y == 1)
                        return '.';
                    if (x == px + side && y == 1)
                        return end;
                    if ((x == px + side && y == 2) || (x == px && y == 3))
                        return '#';
                }
                if (y == 1)
                    return '#';
                return extra ? extra(x, y) : ' ';
            });
        }
        // the room's exitAndDenIndex entries, in order
        std::vector<dcon::room_exit_id> exits_of(sys::game_state& gs, dcon::room_id room) {
            std::vector<dcon::room_exit_id> v;
            gs.world.room_for_each_exit_location(room, [&](dcon::exit_location_id el) { v.push_back(gs.world.exit_location_get_exit(el)); });
            std::sort(v.begin(), v.end(), [&](auto a, auto b) { return gs.world.room_exit_get_index(a) < gs.world.room_exit_get_index(b); });
            return v;
        }
        void tick_world(sys::game_state& gs, uint32_t n) {
            for (uint32_t i = 0; i < n; ++i) {
                simulation::tick(gs, {}, {});
                gs.sounds.clear();
                gs.music.clear();
            }
        }
    }

    namespace {
        // A few rooms full of creatures, items and rot, two slugcats with
        // random input, 300 ticks on `threads` threads: a hash of the world
        uint64_t determinism_run(uint32_t threads) {
            tbb::global_control limit{tbb::global_control::max_allowed_parallelism, threads};
            auto const gs_ptr = sys::make_game_state();
            auto& gs = *gs_ptr;
            auto const region = gs.world.create_region();
            gs.world.region_set_name(region, gs.add_text("test"));
            std::vector<dcon::room_id> rooms;
            rooms.push_back(make_room(gs, region, box(50, 24, [](int32_t x, int32_t y) { return x == 25 && y < 18 ? '|' : y == 10 && x > 30 ? '-' : ' '; })));
            rooms.push_back(make_room(gs, region, box(50, 24, [](int32_t, int32_t) { return ' '; }), 8));
            rooms.push_back(make_room(gs, region, box(40, 30, [](int32_t x, int32_t y) { return y > 20 && x > 10 && x < 30 ? '#' : ' '; })));
            add_rot(gs, rooms[2], world::rot_kind::corruption, vec2{30.f, 30.f}, vec2{0.f, 160.f});
            rooms.push_back(make_room(gs, region, box(60, 20, [](int32_t x, int32_t y) { return y < 3 && x > 20 && x < 40 ? '#' : ' '; })));
            for (auto const rid : rooms) {
                spawn_creatures(gs, rid);
                // a second lizard: the spitting kind
                lizard::create(gs, rid, world::room_view::middle_of_tile(30, 2), lizard::breed::red);
                for (int i = 0; i < 6; ++i)
                    items::create(gs, rid, world::room_view::middle_of_tile(5 + i * 6, 3),
                        i % 3 == 0 ? world::item_type::spear : i % 3 == 1 ? world::item_type::rock : world::item_type::bomb);
            }
            // a den with a lizard and a centipede coming out of it, the rain
            // half way through: they go back in, the slugcat out in the
            // open is thrown around
            rooms.push_back(make_room(gs, region, pipe_room(30, 0)));
            simulation::populate(gs);
            for (auto const k : {creature::spawn_desc{creature::type::lizard, -1.f, false, uint8_t(lizard::breed::green)},
                     creature::spawn_desc{creature::type::centipede, 0.5f}}) {
                auto const c = creature::create(gs, rooms.back(), world::room_view::middle_of_tile(30, 2), k);
                dens::set_home(gs, c, rooms.back(), 0);
                dens::put_in_den(gs, c, 20);
            }
            rain::rain_in(gs, region, 150);
            std::vector<dcon::player_id> ps{player::create(gs, rooms[0], feet_at(10, 1)), player::create(gs, rooms[2], feet_at(5, 1), player::slugcat_name::red),
                player::create(gs, rooms.back(), feet_at(10, 2))};
            rw::rng rng{0xDE7E2u};
            std::vector<player::input_package> in(ps.size());
            for (uint32_t t = 0; t < 300; ++t) {
                if (t % 10 == 0)
                    for (auto& i : in)
                        i = player::make_input(float(rng.range(-1, 2)), float(rng.range(-1, 2)), rng.value() < 0.3f, rng.value() < 0.2f, rng.value() < 0.2f);
                simulation::tick(gs, ps, in);
                gs.sounds.clear();
                gs.music.clear();
            }
            return simulation::hash(gs);
        }
    }

    int run() {
        auto const gs_ptr = sys::make_game_state();
        auto& gs = *gs_ptr;
        auto const region = gs.world.create_region();
        gs.world.region_set_name(region, gs.add_text("test"));
        // the movement and creature tests run without the rain (its own
        // tests below have their own worlds)
        gs.rain_ready = true;
        gs.world.region_set_never_rains(region, true);
        rain::new_cycle(gs, region);
        results r;

        auto const flat = make_room(gs, region, box(40, 20, [](int32_t, int32_t) { return ' '; }));
        float const floor_y = 20.f; // top of the y=0 solid row

        {
            sim s(gs, flat, feet_at(10, 4));
            s.run(80);
            bool const ok = s.contact(1).y == -1 && std::fabs(s.vel(1).y) < 0.5f && std::fabs(s.pos(1).y - (floor_y + 8.f)) < 1.f;
            r.check("rest on floor", ok, s.describe());
        }
        {
            sim s(gs, flat, feet_at(10, 1));
            s.run(40);
            s.step(0, 1);
            s.run(40);
            bool const ok = s.body_mode() == player::body_mode_index::stand && gs.world.player_get_standing(s.p) && s.pos(0).y > s.pos(1).y + 10.f;
            r.check("stand up", ok, s.describe());
        }
        {
            sim s(gs, flat, feet_at(5, 1));
            s.run(30);
            s.step(0, 1);
            s.run(30);
            s.run(40, 1);
            float avg = 0.f;
            for (int i = 0; i < 20; ++i) {
                float const x0 = s.pos(1).x;
                s.step(1);
                avg += s.pos(1).x - x0;
            }
            avg /= 20.f;
            char d[64];
            std::snprintf(d, sizeof(d), " avg_speed=%.2f px/tick", avg);
            r.check("run speed ~4px/tick", avg > 3.5f && avg < 4.6f, s.describe() + d);
        }
        {
            sim s(gs, flat, feet_at(10, 1));
            s.run(30);
            s.step(0, 1);
            s.run(30);
            float const y0 = s.pos(1).y;
            float peak = y0;
            s.step(0, 0, true);
            for (int i = 0; i < 40; ++i) {
                s.step(0, 0, true);
                peak = std::max(peak, s.pos(1).y);
            }
            s.run(40);
            char d[64];
            std::snprintf(d, sizeof(d), " jump_height=%.1fpx", peak - y0);
            r.check("standing jump", peak - y0 > 20.f && peak - y0 < 120.f && s.contact(1).y == -1, s.describe() + d);
        }
        {
            // a vertical pole running up from the floor
            auto const pole = make_room(gs, region, box(20, 20, [](int32_t x, int32_t y) { return x == 10 && y < 15 ? '|' : ' '; }));
            sim s(gs, pole, feet_at(10, 1));
            s.run(30);
            bool const grabbed = s.until(20, [&] { return s.animation() == player::animation_index::climb_on_beam; }, 0, 1);
            float const y0 = s.pos(0).y;
            s.run(40, 0, 1);
            r.check("grab & climb pole", grabbed && s.pos(0).y > y0 + 30.f, s.describe());
        }
        {
            // one tile high corridor along y=1, the head starts inside it
            auto const tunnel = make_room(gs, region, box(30, 10, [](int32_t, int32_t y) { return y >= 2 ? '#' : ' '; }));
            sim s(gs, tunnel, feet_at(5, 1));
            physics::hard_set_position(gs, s.chunk(0), vec2{10.f + 6.f * 20.f, 30.f});
            bool const entered = s.until(20, [&] { return s.body_mode() == player::body_mode_index::corridor_climb; });
            float const x0 = s.pos(0).x;
            s.run(40, 1);
            r.check("corridor crawl", entered && s.pos(0).x > x0 + 40.f, s.describe());
        }
        {
            auto const pool = make_room(gs, region, box(30, 20, [](int32_t, int32_t) { return ' '; }), 8);
            sim s(gs, pool, feet_at(10, 15));
            bool const swims = s.until(120, [&] { return s.body_mode() == player::body_mode_index::swimming; });
            s.run(60);
            r.check("enters water / swims", swims && s.body_mode() == player::body_mode_index::swimming, s.describe());
        }
        {
            auto const shaft = make_room(gs, region, box(10, 50, [](int32_t, int32_t) { return ' '; }));
            sim s(gs, shaft, feet_at(5, 47));
            bool const stunned = s.until(200, [&] { return s.stun() > 0 || s.dead(); });
            r.check("long fall stuns", stunned && !s.dead(), s.describe());
        }
        {
            auto const pit = make_room(gs, region, box(10, 120, [](int32_t, int32_t) { return ' '; }));
            sim s(gs, pit, feet_at(5, 117));
            bool const dead = s.until(300, [&] { return s.dead(); });
            r.check("very long fall kills", dead, s.describe());
        }
        {
            // jump towards a wall and hold into it
            sim s(gs, flat, feet_at(35, 1));
            s.run(30);
            s.step(0, 1);
            s.run(20);
            s.step(1, 0, true);
            bool const climbing = s.until(40, [&] { return s.body_mode() == player::body_mode_index::wall_climb; }, 1, 0, true);
            r.check("wall climb", climbing, s.describe());
        }
        {
            // 45 degree slope going up to the right
            auto const ramp = make_room(gs, region, box(40, 20, [](int32_t x, int32_t y) {
                int32_t const h = x - 10;
                if (x >= 10 && x < 20 && y == h + 1) return '/';
                if (x >= 10 && x < 20 && y <= h) return '#';
                if (x >= 20 && y <= 10) return '#';
                return ' ';
            }));
            sim s(gs, ramp, feet_at(5, 1));
            s.run(30);
            s.step(0, 1);
            s.run(20);
            s.run(120, 1);
            r.check("walk up slope", s.pos(1).y > 120.f, s.describe());
        }
        {
            // horizontal beam three tiles above the floor
            auto const bar = make_room(gs, region, box(30, 20, [](int32_t, int32_t y) { return y == 4 ? '-' : ' '; }));
            sim s(gs, bar, feet_at(10, 1));
            s.run(30);
            s.step(0, 1);
            s.run(20);
            s.step(0, 1, true);
            bool const hangs = s.until(40, [&] { return s.animation() == player::animation_index::hang_from_beam; }, 0, 1, true);
            r.check("jump & grab horizontal beam", hangs, s.describe());
        }
        {
            // landing from a height while holding down-diagonal rolls
            sim s(gs, flat, feet_at(10, 12));
            bool const rolled = s.until(80, [&] { return s.animation() == player::animation_index::roll; }, 1, -1);
            r.check("roll on landing", rolled, s.describe());
        }
        {
            // run, crouch with down-diagonal and jump: belly slide
            sim s(gs, flat, feet_at(5, 1));
            s.run(30);
            s.step(0, 1);
            s.run(20);
            s.run(20, 1);
            // dropping to all fours while running opens a short slide window
            bool const fours = s.until(20, [&] { return s.animation() == player::animation_index::down_on_fours; }, 1, -1);
            std::string const at = s.describe();
            bool const slid = s.until(10, [&] { return s.animation() == player::animation_index::belly_slide; }, 1, -1, true);
            r.check("belly slide", fours && slid, (fours ? s.describe() : "never DownOnFours: " + at));
        }

        {
            // pick up a rock lying at the feet, throw it, it flies and drops
            sim s(gs, flat, feet_at(10, 1));
            auto const rock = items::create_rock(gs, flat, feet_at(11, 1));
            s.run(30);
            s.step(0, 0, false, false, true);
            s.run(5);
            bool const held = items::grabber(gs, rock) == s.p && gs.world.item_get_mode(rock) == world::weapon_mode::carried;
            r.check("pick up rock", held, s.describe());

            s.step(0, 1);
            s.run(20);
            float const x0 = gs.world.body_chunk_get_pos(items::chunk(gs, rock)).x;
            s.step(0, 0, false, true);
            bool const thrown = gs.world.item_get_mode(rock) == world::weapon_mode::thrown && gs.world.body_chunk_get_vel(items::chunk(gs, rock)).x > 30.f
                && !items::grabber(gs, rock);
            // it stays a projectile until it slows down or hits the wall
            bool const landed = s.until(200, [&] { return gs.world.item_get_mode(rock) == world::weapon_mode::free; });
            float const dx = gs.world.body_chunk_get_pos(items::chunk(gs, rock)).x - x0;
            char d[96];
            std::snprintf(d, sizeof(d), " thrown=%d flew=%.0fpx", int(thrown), dx);
            r.check("throw rock", thrown && landed && dx > 200.f, s.describe() + d);

            // walk to it, pick it up again, then drop it holding down
            s.run(30);
            vec2 const rp = gs.world.body_chunk_get_pos(items::chunk(gs, rock));
            s.until(400, [&] { return std::fabs(s.pos(1).x - rp.x) < 15.f; }, rp.x > s.pos(1).x ? 1 : -1);
            s.run(45); // forbiddenToPlayer runs out
            s.step(0, 0, false, false, true);
            s.run(3);
            bool const regrabbed = items::grabber(gs, rock) == s.p;
            for (int i = 0; i < 6; ++i)
                s.step(0, -1);
            s.step(0, -1, false, false, true);
            s.run(3, 0, -1);
            r.check("pick up again & drop", regrabbed && !items::grabber(gs, rock), s.describe());
            items::destroy(gs, rock);
        }
        {
            // a spear thrown at a wall from up close sticks and becomes a beam
            auto& w = gs.world;
            auto const spear_room = make_room(gs, region, box(40, 20, [](int32_t, int32_t) { return ' '; }));
            sim s(gs, spear_room, feet_at(33, 1));
            auto const spear = items::create_spear(gs, spear_room, feet_at(34, 1));
            s.run(30);
            s.step(0, 0, false, false, true);
            s.run(5);
            bool const held = items::grabber(gs, spear) == s.p;
            s.step(0, 1);
            s.run(20);
            s.step(1, 0, false, true);
            bool const stuck = s.until(30, [&] { return w.item_get_mode(spear) == world::weapon_mode::stuck_in_wall; });
            s.run(2);
            auto const view = world::get_room_view(gs, spear_room);
            vec2 const sp = w.body_chunk_get_pos(items::chunk(gs, spear));
            bool const beam = view.get_tile(sp).horizontal_beam();
            // Survivor can't pull it out
            s.run(50);
            s.until(200, [&] { return std::fabs(s.pos(0).x - sp.x) < 20.f; }, 1);
            s.run(45);
            s.step(0, 0, false, false, true);
            s.run(5);
            bool const stays = w.item_get_mode(spear) == world::weapon_mode::stuck_in_wall && !items::grabber(gs, spear);
            char d[128];
            std::snprintf(d, sizeof(d), " held=%d stuck=%d beam=%d stays=%d", int(held), int(stuck), int(beam), int(stays));
            r.check("spear sticks in wall", held && stuck && beam && stays, s.describe() + d);
        }
        {
            // thrown from across the room it rarely sticks: count bounces
            auto& w = gs.world;
            auto const spear_room = make_room(gs, region, box(40, 20, [](int32_t, int32_t) { return ' '; }));
            int bounced = 0, stuck = 0;
            for (int k = 0; k < 12; ++k) {
                sim s(gs, spear_room, feet_at(12, 1));
                auto const spear = items::create_spear(gs, spear_room, feet_at(13, 1));
                s.run(20);
                s.step(0, 0, false, false, true);
                s.run(5);
                s.step(0, 1);
                s.run(20);
                s.step(1, 0, false, true);
                s.until(60, [&] { return w.item_get_mode(spear) != world::weapon_mode::thrown && w.item_get_mode(spear) != world::weapon_mode::carried; });
                if (w.item_get_mode(spear) == world::weapon_mode::stuck_in_wall)
                    ++stuck;
                else
                    ++bounced;
                // destroying a stuck spear gives its tiles their beams back
                items::destroy(gs, spear);
            }
            char d[64];
            std::snprintf(d, sizeof(d), "stuck %d, bounced %d of 12", stuck, bounced);
            r.check("far spears mostly bounce", bounced > stuck && stuck > 0, d);
        }
        {
            // Spearmaster holding pick up pulls a needle out of its tail
            sim s(gs, flat, feet_at(10, 1), player::slugcat_name::spear);
            s.run(30);
            dcon::item_id needle;
            bool const made = s.until(200, [&] {
                for (uint32_t hand = 0; hand < 2; ++hand)
                    if (auto const it = items::held(gs, s.p, hand))
                        needle = it;
                return bool(needle);
            }, 0, 0, false, true);
            bool const is_needle = made && gs.world.item_get_type(needle) == world::item_type::spear && gs.world.item_get_needle(needle);
            r.check("spearmaster makes a spear", is_needle, s.describe());
            s.run(5);
            s.step(1, 0);
            s.step(0, 0, false, true);
            bool const flying = needle && gs.world.item_get_mode(needle) == world::weapon_mode::thrown;
            char d[64];
            std::snprintf(d, sizeof(d), " mode=%d grabbed=%d", needle ? int(gs.world.item_get_mode(needle)) : -1, needle ? int(bool(items::grabber(gs, needle))) : -1);
            r.check("spearmaster throws it", flying, s.describe() + d);
            if (needle)
                items::destroy(gs, needle);
        }
        {
            // Hunter stows a spear on its back by holding pick up, and takes it back
            sim s(gs, flat, feet_at(10, 1), player::slugcat_name::red);
            auto const spear = items::create_spear(gs, flat, feet_at(11, 1));
            s.run(30);
            s.step(0, 0, false, false, true);
            s.run(3);
            bool const held = items::grabber(gs, spear) == s.p;
            s.until(40, [&] { return bool(gs.world.player_get_back_spear(s.p)); }, 0, 0, false, true);
            bool const on_back = gs.world.player_get_back_spear(s.p) == spear && gs.world.item_get_mode(spear) == world::weapon_mode::on_back
                && !items::grabber(gs, spear);
            s.run(10);
            vec2 const sp = gs.world.body_chunk_get_pos(items::chunk(gs, spear));
            bool const follows = custom_dist_ok(sp, s.pos(0));
            s.until(40, [&] { return items::grabber(gs, spear) == s.p; }, 0, 0, false, true);
            bool const back_in_hand = items::grabber(gs, spear) == s.p && !gs.world.player_get_back_spear(s.p);
            char d[96];
            std::snprintf(d, sizeof(d), " held=%d on_back=%d follows=%d back_in_hand=%d", int(held), int(on_back), int(follows), int(back_in_hand));
            r.check("hunter back spear", held && on_back && follows && back_in_hand, s.describe() + d);
            items::destroy(gs, spear);
        }
        {
            // pathing: a wall with a gap; the field leads around it
            auto const walled = make_room(gs, region, box(30, 15, [](int32_t x, int32_t y) {
                return (x == 15 && y < 10) ? '#' : ' ';
            }));
            uint32_t const before = pathing::builds();
            auto const f = pathing::field_to(gs, walled, ivec2{25, 1}, pathing::climber);
            auto const again = pathing::field_to(gs, walled, ivec2{25, 1}, pathing::climber);
            ivec2 t{5, 1};
            int steps = 0;
            while (t != ivec2{25, 1} && steps < 200) {
                auto const next = pathing::next_step(gs, f, t);
                if (!next)
                    break;
                t = *next;
                ++steps;
            }
            char d[96];
            std::snprintf(d, sizeof(d), "reached in %d steps, %u build(s) for two requests", steps, pathing::builds() - before);
            r.check("on demand path field", t == ivec2{25, 1} && f == again && pathing::builds() - before == 1, d);
        }
        {
            // a big centipede finds the slugcat, grabs it and shocks it dead
            auto const pit = make_room(gs, region, box(40, 20, [](int32_t, int32_t) { return ' '; }));
            sim s(gs, pit, feet_at(8, 1));
            auto const c = centipede::create(gs, pit, feet_at(30, 1), 0.8f);
            bool grabbed = false;
            bool const killed = s.until(3000, [&] {
                grabbed = grabbed || gs.world.physical_object_get_grabbed_by_count(player::body(gs, s.p)) > 0;
                return s.dead();
            });
            char d[96];
            std::snprintf(d, sizeof(d), " grabbed=%d killed=%d behavior=%d", int(grabbed), int(killed), int(world::centipede_behavior(gs.world.creature_get_behavior(c))));
            r.check("centipede hunts and shocks", grabbed && killed, s.describe() + d);
            centipede::destroy(gs, c);
        }
        {
            // a small one is afraid of the slugcat; rocks stun it
            auto const pit = make_room(gs, region, box(40, 20, [](int32_t, int32_t) { return ' '; }));
            sim s(gs, pit, feet_at(18, 1));
            auto const c = centipede::create(gs, pit, feet_at(23, 1), 0.f);
            s.run(60);
            bool const flees = world::centipede_behavior(gs.world.creature_get_behavior(c)) == world::centipede_behavior::flee;
            // a spear's worth of damage (a rock barely stuns a healthy one)
            centipede::violence(gs, c, 0, vec2{0.f, 0.f}, 1.f, 0.f);
            bool const stunned = gs.world.creature_get_stun(c) > 0;
            char d[64];
            std::snprintf(d, sizeof(d), " flees=%d stunned=%d", int(flees), int(stunned));
            r.check("small centipede flees, spears stun", flees && stunned && !s.dead(), s.describe() + d);
            centipede::destroy(gs, c);
        }
        {
            // a centiwing on one ledge flies over a bottomless pit to the
            // other one and lands there (the slugcat shut away out of sight)
            int32_t const wd = 44, ht = 26;
            std::vector<std::string> rows;
            for (int32_t r0 = 0; r0 < ht; ++r0) {
                int32_t const y = ht - 1 - r0;
                std::string row(size_t(wd), ' ');
                for (int32_t x = 0; x < wd; ++x) {
                    bool const border = x == 0 || x == wd - 1 || y == ht - 1;
                    bool const ledge = y < 10 && (x < 10 || x > 33);
                    bool const closet = y >= 19 && x < 7 && !(y >= 21 && y <= 22 && x > 1 && x < 5);
                    row[size_t(x)] = border || ledge || closet ? '#' : ' ';
                }
                rows.push_back(row);
            }
            auto const gap = make_room(gs, region, rows);
            sim s(gs, gap, feet_at(3, 21));
            auto const c = centipede::create(gs, gap, world::room_view::middle_of_tile(6, 10), -1.f, centipede::variant::wing);
            ivec2 const target{38, 10};
            gs.world.creature_set_destination(c, target);
            gs.world.creature_get_payload(c).centipede.temp_idle_pos = target;
            bool flew = false;
            float lowest = 1e9f;
            auto const head = [&] { return gs.world.body_chunk_get_pos(centipede::chunk(gs, c, centipede::head_index(gs, c))); };
            bool const landed = s.until(2000, [&] {
                bool const flying = gs.world.creature_get_payload(c).centipede.flying;
                flew = flew || flying;
                for (uint32_t i = 0; i < centipede::chunk_count(gs, c); ++i)
                    lowest = std::min(lowest, gs.world.body_chunk_get_pos(centipede::chunk(gs, c, i)).y);
                return flew && !flying && world::room_view::get_tile_position(head()).x > 33;
            });
            char d[128];
            std::snprintf(d, sizeof(d), " flew=%d landed=%d head=(%.0f,%.0f) lowest=%.0f", int(flew), int(landed), head().x, head().y, lowest);
            r.check("centiwing flies over a gap", flew && landed && lowest > 100.f, s.describe() + d);
            centipede::destroy(gs, c);
        }
        {
            // a red centipede hunts the slugcat down and its shock kills it;
            // a heavy blow on a shell knocks it off but glances off the armour
            auto const pit = make_room(gs, region, box(40, 20, [](int32_t, int32_t) { return ' '; }));
            sim s(gs, pit, feet_at(8, 1));
            auto const c = centipede::create(gs, pit, feet_at(30, 1), -1.f, centipede::variant::red);
            bool const killed = s.until(3000, [&] { return s.dead(); });
            uint32_t k = centipede::chunk_count(gs, c) / 2;
            while (k + 1 < centipede::chunk_count(gs, c) && !centipede::has_shell(gs, c, k))
                ++k;
            bool const had = centipede::has_shell(gs, c, k);
            float const health = gs.world.creature_get_health(c);
            centipede::violence(gs, c, k, vec2{2.f, 0.f}, 1.f, 0.f, creature::damage_type::stab);
            bool const off = had && !centipede::has_shell(gs, c, k);
            float const hurt = health - gs.world.creature_get_health(c);
            char d[128];
            std::snprintf(d, sizeof(d), " chunks=%u killed=%d shell off=%d hurt=%.4f", centipede::chunk_count(gs, c), int(killed), int(off), hurt);
            r.check("red centipede kills, sheds shells", killed && off && hurt < 0.05f && centipede::chunk_count(gs, c) == 18, s.describe() + d);
            centipede::destroy(gs, c);
        }
        {
            // a baby centipede is picked up with one hand and eaten bite by
            // bite (two food pips); one held without eating it shocks the slugcat
            auto const pit = make_room(gs, region, box(40, 20, [](int32_t, int32_t) { return ' '; }));
            sim s(gs, pit, feet_at(10, 1));
            auto const c = centipede::create(gs, pit, feet_at(11, 1), -1.f, centipede::variant::small);
            s.run(5);
            s.step(0, 0, false, false, true);
            s.step();
            bool const held = centipede::held_in_hand(gs, s.p, 0) == c || centipede::held_in_hand(gs, s.p, 1) == c;
            int32_t const food0 = gs.world.player_get_food(s.p);
            bool const eaten = s.until(400, [&] { return !creature::is(gs.world, c, creature::type::centipede); }, 0, 0, false, true);
            int32_t const food = gs.world.player_get_food(s.p) - food0;
            auto const c2 = centipede::create(gs, pit, s.pos(1) + vec2{14.f, 0.f}, -1.f, centipede::variant::small);
            s.run(3);
            s.step(0, 0, false, false, true);
            s.step();
            bool const held2 = centipede::held_in_hand(gs, s.p, 0) == c2 || centipede::held_in_hand(gs, s.p, 1) == c2;
            bool const shocked = s.until(200, [&] { return s.stun() > 0; });
            bool const dropped = !centipede::held_in_hand(gs, s.p, 0) && !centipede::held_in_hand(gs, s.p, 1);
            char d[128];
            std::snprintf(d, sizeof(d), " held=%d eaten=%d food=%d held2=%d shocked=%d dropped=%d", int(held), int(eaten), food, int(held2), int(shocked), int(dropped));
            r.check("baby centipede eaten, or shocks", held && eaten && food == 2 && held2 && shocked && dropped && !s.dead(), s.describe() + d);
            if (creature::is(gs.world, c2, creature::type::centipede))
                centipede::destroy(gs, c2);
        }
        {
            // an aquapede swims about under the water, lit by its Glower
            auto const pool = make_room(gs, region, box(40, 24, [](int32_t, int32_t) { return ' '; }), 18);
            sim s(gs, pool, feet_at(3, 20));
            auto const c = centipede::create(gs, pool, world::room_view::middle_of_tile(20, 8), -1.f, centipede::variant::aqua);
            vec2 const start = gs.world.body_chunk_get_pos(centipede::chunk(gs, c, 0));
            uint32_t under = 0;
            for (int t = 0; t < 300; ++t) {
                s.step();
                if (physics::object_submersion(gs, s.room, centipede::body(gs, c)) > 0.5f)
                    ++under;
            }
            float const moved = rw::distance(start, gs.world.body_chunk_get_pos(centipede::chunk(gs, c, 0)));
            float const glow = gs.world.centipede_data_get_glow(creature::centipede_data(gs.world, c));
            char d[128];
            std::snprintf(d, sizeof(d), " under=%u/300 moved=%.0f glow=%.2f", under, moved, glow);
            r.check("aquapede swims under water", under > 280 && moved > 40.f && glow > 0.f, s.describe() + d);
            centipede::destroy(gs, c);
        }
        {
            // a school of leeches finds the slugcat at the bottom of their
            // pool, latches on and holds it down: it runs out of breath far
            // more than floating up alone
            auto const run_pool = [&](int leeches, uint32_t& most, bool& schooled) {
                auto const pool = make_room(gs, region, box(40, 24, [](int32_t, int32_t) { return ' '; }), 18);
                sim s(gs, pool, feet_at(20, 3));
                std::vector<dcon::creature_id> ls;
                for (int i = 0; i < leeches; ++i)
                    ls.push_back(leech::create(gs, pool, world::room_view::middle_of_tile(14 + i, 3)));
                float air = 1.f;
                for (int t = 0; t < 2000; ++t) {
                    s.step();
                    uint32_t on = 0;
                    for (auto const l : ls)
                        on += leech::attached_to(gs, l) == s.p ? 1 : 0;
                    most = std::max(most, on);
                    schooled = schooled || (!ls.empty() && leech::school_size(gs, ls[0]) > 1);
                    air = std::min(air, gs.world.player_get_air_in_lungs(s.p));
                }
                for (auto const l : ls)
                    leech::destroy(gs, l);
                return air;
            };
            uint32_t most = 0, none = 0;
            bool schooled = false, alone_schooled = false;
            float const air = run_pool(5, most, schooled);
            float const alone = run_pool(0, none, alone_schooled);
            char d[128];
            std::snprintf(d, sizeof(d), "schooled=%d latched=%u air %.2f (%.2f without leeches)", int(schooled), most, air, alone);
            r.check("leeches latch on, drag it under", schooled && most > 0 && air < alone - 0.1f, d);
        }
        {
            // a dropwig waits in the ceiling, drops on the slugcat walking
            // under it and bites it (or hunts it down after missing)
            // (a thick ceiling: the camera doesn't see the top row of the room,
            // and ValidCeilingSpot wants the spot and the floor under it in view)
            auto const pit = make_room(gs, region, box(40, 20, [](int32_t, int32_t y) { return y >= 15 ? '#' : ' '; }));
            sim s(gs, pit, feet_at(3, 1));
            auto const d = dropbug::create(gs, pit, world::room_view::middle_of_tile(20, 8));
            s.run(100);
            bool const waits = dropbug::sitting_in_ceiling(gs, d) && gs.world.creature_get_payload(d).dropbug.in_ceiling_mode == 1.f
                && world::room_view::get_tile_position(gs.world.body_chunk_get_pos(dropbug::chunk(gs, d, 1))).y == 14;
            bool dropped = false;
            bool const hurt = s.until(1500, [&] {
                dropped = dropped || gs.world.creature_get_payload(d).dropbug.from_ceiling_jump;
                return s.stun() > 0 || s.dead() || dropbug::grasped(gs, d) == s.p;
            }, 1); // walking right, under it and on to the wall
            char d_[96];
            std::snprintf(d_, sizeof(d_), " waits=%d dropped=%d hurt=%d behavior=%d", int(waits), int(dropped), int(hurt), int(::dropbug::behavior(gs.world.creature_get_behavior(d))));
            r.check("dropwig drops and bites", waits && dropped && hurt, s.describe() + d_);
            dropbug::destroy(gs, d);
        }
        {
            // a squidcada left alone flies off and settles on the terrain; one
            // the slugcat picks up boosts its jump, slows a long fall to a
            // glide and tires doing it
            auto const hall = make_room(gs, region, box(40, 70, [](int32_t, int32_t) { return ' '; }));
            sim s(gs, hall, feet_at(3, 1));
            auto const c = cicada::create(gs, hall, world::room_view::middle_of_tile(30, 20), true);
            bool const sat = s.until(2000, [&] { return !gs.world.creature_get_payload(c).cicada.flying && cicada::consious(gs, c); });
            // a plain jump to compare with
            auto const jump = [&] {
                s.step(0, 1); // up off all fours first
                s.run(30);
                float const y0 = s.pos(1).y;
                float peak = y0;
                for (int i = 0; i < 60; ++i) {
                    s.step(0, 0, true);
                    peak = std::max(peak, s.pos(1).y);
                }
                s.run(60);
                return peak - y0;
            };
            float const plain = jump();
            // fetched to the slugcat's hands
            for (uint32_t i = 0; i < 2; ++i)
                physics::hard_set_position(gs, cicada::chunk(gs, c, i), s.pos(0) + vec2{6.f, 4.f - float(i) * 12.f});
            s.step(0, 0, false, false, true);
            s.step();
            bool const held = cicada::held_by(gs, s.p) == c;
            s.run(60); // it gets its breath back
            float const boosted = jump();
            // high up in the room, still holding it
            vec2 const up{0.f, 1000.f};
            for (uint32_t i = 0; i < 2; ++i) {
                physics::hard_set_position(gs, s.chunk(int(i)), s.pos(int(i)) + up);
                physics::hard_set_position(gs, cicada::chunk(gs, c, i), gs.world.body_chunk_get_pos(cicada::chunk(gs, c, i)) + up);
            }
            // (a free fall from there takes about 47 ticks)
            uint32_t const t0 = s.ticks;
            s.step(0, 0, true);
            bool const landed = s.until(1500, [&] { return s.contact(1).y == -1 || s.dead(); }, 0, 0, true);
            uint32_t const glide = s.ticks - t0;
            bool const tired = gs.world.creature_get_payload(c).cicada.stamina < 0.5f;
            bool const still_held = cicada::held_by(gs, s.p) == c;
            char d[192];
            std::snprintf(d, sizeof(d), " sat=%d held=%d jump=%.0f/%.0fpx glide=%u ticks tired=%d alive=%d", int(sat), int(held), boosted, plain, glide, int(tired), int(!s.dead()));
            r.check("squidcada lifts the slugcat", sat && held && boosted > plain * 1.2f && landed && glide > 80 && tired && still_held && !s.dead(), s.describe() + d);
            // let go with pick up
            s.run(20);
            s.step(0, 0, false, false, true);
            s.step();
            r.check("squidcada let go", !cicada::held_by(gs, s.p), s.describe());
            cicada::destroy(gs, c);
        }
        {
            // a dark squidcada near the slugcat pesters it: bump attacks and
            // clinging on
            auto const hall = make_room(gs, region, box(40, 30, [](int32_t, int32_t) { return ' '; }));
            sim s(gs, hall, feet_at(20, 1));
            auto const c = cicada::create(gs, hall, world::room_view::middle_of_tile(24, 6), false);
            bool antagonized = false, charged = false, clung = false;
            s.until(2400, [&] {
                antagonized = antagonized || ::cicada::behavior(gs.world.creature_get_behavior(c)) == cicada::behavior::antagonize;
                charged = charged || cicada::charging(gs, c);
                clung = clung || gs.world.creature_get_payload(c).cicada.sticky_player != 0;
                return antagonized && (charged || clung) && s.stun() == 0;
            });
            char d[96];
            std::snprintf(d, sizeof(d), " antagonized=%d charged=%d clung=%d", int(antagonized), int(charged), int(clung));
            r.check("squidcada pesters the slugcat", antagonized && (charged || clung), s.describe() + d);
            cicada::destroy(gs, c);
        }
        {
            // a green lizard spots the slugcat across the room, runs it down
            // and bites it; held in its jaws the slugcat is done for
            auto const hall = make_room(gs, region, box(50, 20, [](int32_t, int32_t) { return ' '; }));
            sim s(gs, hall, feet_at(5, 1));
            auto const l = lizard::create(gs, hall, world::room_view::middle_of_tile(40, 1), lizard::breed::green);
            bool spotted = false, hunted = false, bit = false;
            bool const killed = s.until(4000, [&] {
                spotted = spotted || gs.world.creature_get_sees_player(l);
                hunted = hunted || lizard::behavior(gs.world.creature_get_behavior(l)) == lizard::behavior::hunt;
                bit = bit || lizard::grasped(gs, l) == s.p;
                return s.dead();
            });
            char d[96];
            std::snprintf(d, sizeof(d), " spotted=%d hunted=%d bit=%d killed=%d", int(spotted), int(hunted), int(bit), int(killed));
            r.check("green lizard kills the slugcat", spotted && hunted && bit && killed, s.describe() + d);
            lizard::destroy(gs, l);
            gs.sounds.clear();
        }
        {
            // a pink one climbs a pole down to the slugcat and gets it too
            auto const hall = make_room(gs, region, box(40, 24, [](int32_t x, int32_t y) {
                if (y == 12 && x >= 26)
                    return '#';
                return x == 25 && y <= 12 ? '|' : ' ';
            }));
            sim s(gs, hall, feet_at(6, 1));
            auto const l = lizard::create(gs, hall, world::room_view::middle_of_tile(29, 13), lizard::breed::pink);
            bool bit = false, climbed = false;
            bool const killed = s.until(5000, [&] {
                bit = bit || lizard::grasped(gs, l) == s.p;
                climbed = climbed || pathing::tile_access(s.room, world::room_view::get_tile_position(gs.world.body_chunk_get_pos(lizard::chunk(gs, l, 0)))) == pathing::access::climb;
                return s.dead();
            });
            char d[96];
            std::snprintf(d, sizeof(d), " climbed=%d bit=%d killed=%d", int(climbed), int(bit), int(killed));
            r.check("pink lizard comes down a pole", climbed && bit && killed, s.describe() + d);
            lizard::destroy(gs, l);
            gs.sounds.clear();
        }
        {
            // a blue lizard's tongue catches the slugcat from a distance
            auto const hall = make_room(gs, region, box(40, 20, [](int32_t, int32_t) { return ' '; }));
            sim s(gs, hall, feet_at(10, 1));
            // the lizard's rolls (its personality, when it shoots and bites)
            // from a fixed seed: what the other tests used up of
            // game_state::random doesn't decide whether this one passes
            gs.random.state = 15838u;
            auto const l = lizard::create(gs, hall, world::room_view::middle_of_tile(17, 1), lizard::breed::blue);
            bool shot = false;
            bool const caught = s.until(3000, [&] {
                auto const& t = gs.world.lizard_data_get_tongue(creature::lizard_data(gs.world, l));
                shot = shot || t.state == lizard::tongue_state::lashing_out;
                return t.state == lizard::tongue_state::attatched && t.attached_player == uint8_t(s.p.index() + 1);
            });
            char d[64];
            std::snprintf(d, sizeof(d), " shot=%d caught=%d", int(shot), int(caught));
            r.check("blue lizard tongue grabs", shot && caught, s.describe() + d);
            lizard::destroy(gs, l);
            gs.sounds.clear();
        }
        {
            // a salamander swims across the pool towards the slugcat on the bank
            auto const pool = make_room(gs, region, box(50, 20, [](int32_t x, int32_t y) { return x >= 40 && y < 9 ? '#' : ' '; }), 8);
            sim s(gs, pool, feet_at(45, 9));
            gs.random.state = 7919u; // (a fixed seed, like the blue lizard's)
            auto const l = lizard::create(gs, pool, world::room_view::middle_of_tile(5, 3), lizard::breed::salamander);
            float const x0 = gs.world.body_chunk_get_pos(lizard::chunk(gs, l, 0)).x;
            bool swam = false;
            float furthest = x0;
            s.until(3000, [&] {
                swam = swam || gs.world.lizard_data_get_core(creature::lizard_data(gs.world, l)).swim > 0.5f;
                furthest = std::max(furthest, gs.world.body_chunk_get_pos(lizard::chunk(gs, l, 0)).x);
                return swam && furthest > x0 + 400.f;
            });
            char d[96];
            std::snprintf(d, sizeof(d), " swam=%d moved=%.0fpx", int(swam), furthest - x0);
            r.check("salamander swims", swam && furthest > x0 + 400.f, s.describe() + d);
            lizard::destroy(gs, l);
            gs.sounds.clear();
        }
        {
            // a red lizard spits at the slugcat on a ledge it can't get to
            auto const hall = make_room(gs, region, box(50, 24, [](int32_t x, int32_t y) { return y == 12 && x >= 30 ? '#' : ' '; }));
            sim s(gs, hall, feet_at(31, 13));
            auto const l = lizard::create(gs, hall, world::room_view::middle_of_tile(10, 1), lizard::breed::red);
            bool spat = false, flew = false;
            s.until(3000, [&] {
                spat = spat || gs.world.lizard_data_get_core(creature::lizard_data(gs.world, l)).anim == lizard::animation::spit;
                gs.world.for_each_lizard_spit([&](dcon::lizard_spit_id) { flew = true; });
                return spat && flew;
            });
            char d[64];
            std::snprintf(d, sizeof(d), " spat=%d flew=%d", int(spat), int(flew));
            r.check("red lizard spits", spat && flew, s.describe() + d);
            lizard::destroy(gs, l);
            gs.sounds.clear();
        }
        {
            // a cyan lizard finds a jump on its way to the slugcat
            auto const hall = make_room(gs, region, box(50, 24, [](int32_t x, int32_t y) { return y <= 4 && x > 12 && x < 36 ? (y == 0 ? '#' : ' ') : (y <= 4 ? '#' : ' '); }));
            sim s(gs, hall, feet_at(44, 5));
            auto const l = lizard::create(gs, hall, world::room_view::middle_of_tile(5, 5), lizard::breed::cyan);
            bool jumped = false;
            s.until(4000, [&] {
                jumped = jumped || gs.world.lizard_data_get_core(creature::lizard_data(gs.world, l)).anim == lizard::animation::jumping;
                return jumped;
            });
            r.check("cyan lizard jumps", jumped, s.describe() + (jumped ? " jumped=1" : " jumped=0"));
            lizard::destroy(gs, l);
            gs.sounds.clear();
        }
        {
            // a daddy long legs over a slugcat standing still: its tentacles
            // feel it, hunt it, grab it and drag it to the body, which eats it
            auto const hall = make_room(gs, region, box(30, 14, [](int32_t, int32_t) { return ' '; }));
            sim s(gs, hall, feet_at(15, 1));
            auto const d = daddy::create(gs, hall, world::room_view::middle_of_tile(15, 6), daddy::variant::daddy);
            bool gripped = false, hunted = false, held = false, eating = false;
            bool const eaten = s.until(4000, [&] {
                gripped = gripped || daddy::legs_gripping(gs, d) > 0;
                hunted = hunted || daddy::behavior(gs.world.creature_get_behavior(d)) == daddy::behavior::hunt;
                held = held || daddy::holds(gs, d, s.p);
                eating = eating || daddy::eating(gs, d, s.p);
                return gs.world.player_get_eaten(s.p) != 0;
            });
            char dd[128];
            std::snprintf(dd, sizeof(dd), " gripped=%d hunted=%d held=%d eating=%d eaten=%d dead=%d", int(gripped), int(hunted), int(held), int(eating), int(eaten), int(s.dead()));
            r.check("daddy long legs eats slugcat", gripped && hunted && held && eating && eaten && s.dead(), s.describe() + dd);
            daddy::destroy(gs, d);
            gs.sounds.clear();
        }
        {
            // one sent to the far side of a wall finds its way over it on a
            // real-time distance field, the tentacles reaching for grips
            auto const hall = make_room(gs, region, box(60, 22, [](int32_t x, int32_t y) {
                if (x == 30 && y <= 14)
                    return '#';
                if ((x == 4 && y <= 3) || (y == 3 && x <= 4))
                    return '#'; // the slugcat's sealed pocket, out of reach
                return ' ';
            }));
            sim s(gs, hall, feet_at(2, 1));
            auto const d = daddy::create(gs, hall, world::room_view::middle_of_tile(46, 8), daddy::variant::daddy);
            ivec2 const target{12, 10};
            uint32_t const builds = pathing::builds();
            bool over = false;
            bool const arrived = s.until(6000, [&] {
                auto& ai = gs.world.daddy_data_get_ai(creature::daddy_data(gs.world, d));
                gs.world.creature_set_destination(d, target);
                ai.new_idle_pos_counter = 1000000;
                vec2 const m = daddy::middle_of_body(gs, d);
                over = over || (m.x < 30.f * 20.f && m.y > 14.f * 20.f);
                return rw::distance(m, world::room_view::middle_of_tile(target)) < 80.f;
            });
            char dd[128];
            vec2 const m = daddy::middle_of_body(gs, d);
            std::snprintf(dd, sizeof(dd), " body=(%.0f,%.0f) over=%d arrived=%d fields=%u", m.x, m.y, int(over), int(arrived), pathing::builds() - builds);
            r.check("daddy long legs climbs over", over && arrived && pathing::builds() > builds, s.describe() + dd);
            daddy::destroy(gs, d);
            gs.sounds.clear();
        }
        {
            // a brother long legs hit by a spear is hurt and stunned, a rock
            // on a tentacle stuns that tentacle
            auto const hall = make_room(gs, region, box(40, 16, [](int32_t, int32_t) { return ' '; }));
            sim s(gs, hall, feet_at(3, 1));
            auto const d = daddy::create(gs, hall, world::room_view::middle_of_tile(25, 4), daddy::variant::brother);
            s.run(60);
            vec2 const m = daddy::middle_of_body(gs, d);
            auto const spear = items::create_spear(gs, hall, m - vec2{60.f, 0.f});
            gs.world.body_chunk_set_vel(items::chunk(gs, spear), vec2{40.f, 0.f});
            gs.world.item_set_mode(spear, world::weapon_mode::thrown);
            gs.world.item_set_throw_dir(spear, ivec2{1, 0});
            gs.world.item_set_throw_mode_frames(spear, -1);
            bool stunned = false;
            for (int i = 0; i < 6; ++i) {
                s.step();
                stunned = stunned || gs.world.creature_get_stun(d) > 0;
            }
            float const health = gs.world.creature_get_health(d);
            // rocks thrown across tentacle segments away from the body (one
            // the projectile trace says it would hit), until one is stunned
            bool limp = false;
            std::vector<dcon::item_id> rocks;
            for (int attempt = 0; attempt < 8 && !limp; ++attempt) {
                vec2 const mid = daddy::middle_of_body(gs, d);
                vec2 at{0.f, 0.f};
                float side = 1.f;
                bool found = false;
                for (uint32_t k = 0; k < daddy::tentacle_count(gs, d) && !found; ++k) {
                    auto const cs = gs.world.daddy_tentacle_get_chunks(daddy::tentacle_of(gs, d, k));
                    for (uint32_t i = 1 + uint32_t(attempt) % 2; i < cs.size() && !found; ++i) {
                        vec2 const c = (cs.at(i - 1).pos + cs.at(i).pos) * 0.5f;
                        float const sd = c.x > mid.x ? 1.f : -1.f;
                        dcon::creature_id hd;
                        creature::hit_part hp;
                        if (std::fabs(c.x - mid.x) > 60.f && c.y > 50.f && creature::trace(gs, hall, c + vec2{sd * 30.f, 0.f}, c - vec2{sd * 30.f, 0.f}, 5.f, hd, hp) && hp.tentacle >= 0) {
                            at = c;
                            side = sd;
                            found = true;
                        }
                    }
                }
                if (!found) {
                    s.run(10);
                    continue;
                }
                auto const rock = items::create_rock(gs, hall, at + vec2{side * 20.f, 0.f});
                rocks.push_back(rock);
                gs.world.body_chunk_set_vel(items::chunk(gs, rock), vec2{-side * 35.f, 0.f});
                gs.world.item_set_mode(rock, world::weapon_mode::thrown);
                gs.world.item_set_throw_dir(rock, ivec2{int32_t(-side), 0});
                gs.world.item_set_throw_mode_frames(rock, -1);
                for (int i = 0; i < 6; ++i) {
                    s.step();
                    for (uint32_t k = 0; k < daddy::tentacle_count(gs, d); ++k)
                        limp = limp || gs.world.creature_part_get_stun(daddy::tentacle_part(gs, d, k)) > 0;
                }
            }
            char dd[96];
            std::snprintf(dd, sizeof(dd), " stunned=%d health=%.3f tentacle_stunned=%d", int(stunned), health, int(limp));
            r.check("daddy long legs hit", stunned && health < 1.f && limp, s.describe() + dd);
            items::destroy(gs, spear);
            for (auto const rk : rocks)
                items::destroy(gs, rk);
            daddy::destroy(gs, d);
            gs.sounds.clear();
        }
        {
            // a rot vine across a tall room: dropping onto it holding up grabs
            // it, climbing moves along it, jumping lets go
            auto const room = make_room(gs, region, box(40, 30, [](int32_t, int32_t) { return ' '; }));
            // the vine only grows with some rot in the room: a little in a corner
            add_rot(gs, room, world::rot_kind::corruption, vec2{30.f, 30.f}, vec2{0.f, 25.f});
            add_rot(gs, room, world::rot_kind::tube, vec2{110.f, 510.f}, vec2{580.f, 0.f});
            sim s(gs, room, feet_at(30, 1));
            s.run(150); // the vine settles into its sag
            player::reset(gs, s.p, room, vec2{400.f, 560.f});
            bool const grabbed = s.until(120, [&] { return s.animation() == player::animation_index::vine_grab; }, 0, 1);
            s.run(60);
            bool const held = s.animation() == player::animation_index::vine_grab && s.pos(1).y > 100.f;
            float const x0 = s.pos(0).x;
            s.run(120, 1);
            float const climbed = s.pos(0).x - x0;
            bool const still_on = s.animation() == player::animation_index::vine_grab;
            s.step(0, 1, true);
            s.step(0, 1, false);
            bool const let_go = s.animation() != player::animation_index::vine_grab;
            char d[128];
            std::snprintf(d, sizeof(d), " grabbed=%d held=%d climbed=%.0fpx still_on=%d let_go=%d", int(grabbed), int(held), climbed, int(still_on), int(let_go));
            r.check("climb a rot vine", grabbed && held && climbed > 60.f && still_on && let_go, s.describe() + d);
        }
        {
            // lying in thick rot: the bulbs nibble until the slugcat is pulled in and eaten
            auto const room = make_room(gs, region, box(20, 10, [](int32_t, int32_t) { return ' '; }));
            add_rot(gs, room, world::rot_kind::corruption, vec2{30.f, 30.f}, vec2{0.f, 160.f});
            sim s(gs, room, feet_at(2, 1));
            s.step(0, -1);
            bool const eaten = s.until(1200, [&] { return gs.world.player_get_eaten(s.p) != 0; }, 0, -1);
            uint32_t legs = 0, meals = 0;
            auto const sys = rot::system_of(gs, room);
            gs.world.rot_system_for_each_rot_tube_membership(sys, [&](dcon::rot_tube_membership_id) { ++legs; });
            gs.world.rot_system_for_each_rot_meal_membership(sys, [&](dcon::rot_meal_membership_id) { ++meals; });
            char d[96];
            std::snprintf(d, sizeof(d), " eaten=%d dead=%d legs=%u meals_left=%u", int(eaten), int(s.dead()), legs, meals);
            r.check("rot eats a resting slugcat", eaten && s.dead() && meals == 0, s.describe() + d);
            // respawning brings it back whole
            player::reset(gs, s.p, room, feet_at(15, 1));
            s.run(40);
            r.check("respawn after being eaten", !gs.world.player_get_eaten(s.p) && s.contact(1).y == -1, s.describe());
        }
        {
            // PassiveCorruption only slows creatures down and grows tendrils under ceilings
            auto const room = make_room(gs, region, box(20, 10, [](int32_t x, int32_t y) { return y >= 5 && x > 2 && x < 17 ? '#' : ' '; }));
            world::rot_custom_data c;
            c.leg_chance = 1.f;
            add_rot(gs, room, world::rot_kind::passive_corruption, vec2{200.f, 100.f}, vec2{0.f, 140.f}, c);
            sim s(gs, room, feet_at(9, 1));
            s.run(600, 0, -1);
            uint32_t tendrils = 0, legs = 0;
            gs.world.rot_system_for_each_rot_tube_membership(rot::system_of(gs, room), [&](dcon::rot_tube_membership_id m) {
                auto const k = gs.world.rot_tube_get_kind(gs.world.rot_tube_membership_get_tube(m));
                (k == world::rot_tube_kind::tendril ? tendrils : legs) += 1;
            });
            char d[96];
            std::snprintf(d, sizeof(d), " eaten=%d tendrils=%u legs=%u", int(gs.world.player_get_eaten(s.p)), tendrils, legs);
            r.check("passive rot doesn't eat", !gs.world.player_get_eaten(s.p) && !s.dead() && tendrils > 0 && legs == 0, s.describe() + d);
        }
        {
            // the Watcher standing in a warp is carried to the paired warp of
            // another room, a rock lying next to it comes along at once
            auto& w = gs.world;
            auto const a = make_room(gs, region, box(30, 20, [](int32_t, int32_t) { return ' '; }));
            auto const b = make_room(gs, region, box(30, 20, [](int32_t, int32_t) { return ' '; }));
            add_warp(gs, a, {feet_at(15, 1) + vec2{0.f, 20.f}, b, feet_at(10, 1) + vec2{0.f, 20.f}});
            add_warp(gs, b, {feet_at(10, 1) + vec2{0.f, 20.f}, a, feet_at(15, 1) + vec2{0.f, 20.f}});
            gs.ripple_level = 1.f;
            sim s(gs, a, feet_at(15, 1), player::slugcat_name::watcher);
            auto const rock = items::create_rock(gs, a, feet_at(18, 1));
            bool const opened = s.until(300, [&] {
                bool open = false;
                w.room_for_each_warp_point_location(a, [&](dcon::warp_point_location_id l) { open = open || warps::openness(gs, w.warp_point_location_get_point(l), 1.f) > 0.5f; });
                return open;
            });
            bool const started = s.until(600, [&] { return gs.warp.active; });
            bool const arrived = s.until(400, [&] { return simulation::player_room(gs, s.p) == b; });
            bool const rock_too = w.physical_object_get_room_from_object_location(items::body(gs, rock)) == b;
            float const dist = rw::distance(s.pos(1), feet_at(10, 1) + vec2{0.f, 20.f});
            // floating out of the exit while the transition clears
            bool const hovered = s.until(100, [&] { return w.player_get_levitation_active(s.p) != 0; });
            bool const done = s.until(600, [&] { return !gs.warp.active; });
            // the gravity comes back once out
            s.run(120);
            bool const landed = s.contact(1).y == -1;
            char d[160];
            std::snprintf(d, sizeof(d), " opened=%d started=%d arrived=%d rock=%d dist=%.0f hovered=%d done=%d landed=%d", int(opened), int(started), int(arrived),
                int(rock_too), dist, int(hovered), int(done), int(landed));
            r.check("watcher warps, items come along", opened && started && arrived && rock_too && dist < 60.f && hovered && done && landed, s.describe() + d);
            items::destroy(gs, rock);
            // the arrival warp needs cooling down before taking the Watcher back
            s.run(200);
            r.check("no warping straight back", simulation::player_room(gs, s.p) == b && !gs.warp.active, s.describe());
        }
        {
            // only the Watcher can use warps
            auto const a = make_room(gs, region, box(30, 20, [](int32_t, int32_t) { return ' '; }));
            auto const b = make_room(gs, region, box(30, 20, [](int32_t, int32_t) { return ' '; }));
            add_warp(gs, a, {feet_at(15, 1) + vec2{0.f, 20.f}, b, feet_at(10, 1)});
            sim s(gs, a, feet_at(15, 1), player::slugcat_name::white);
            s.run(900);
            r.check("survivor can't warp", simulation::player_room(gs, s.p) == a && !gs.warp.active, s.describe());
        }
        {
            // a one way warp's exit never takes anyone anywhere, its entrance
            // works once and closes (limited use)
            auto& w = gs.world;
            auto const a = make_room(gs, region, box(30, 20, [](int32_t, int32_t) { return ' '; }));
            auto const b = make_room(gs, region, box(30, 20, [](int32_t, int32_t) { return ' '; }));
            add_warp(gs, a, {feet_at(15, 1) + vec2{0.f, 20.f}, b, feet_at(10, 1) + vec2{0.f, 20.f}, true, true, false, 1});
            add_warp(gs, b, {feet_at(10, 1) + vec2{0.f, 20.f}, a, feet_at(15, 1) + vec2{0.f, 20.f}, true, false});
            sim s(gs, a, feet_at(15, 1), player::slugcat_name::watcher);
            bool const there = s.until(1200, [&] { return simulation::player_room(gs, s.p) == b; });
            s.until(800, [&] { return !gs.warp.active; });
            s.run(1500);
            bool const stayed = simulation::player_room(gs, s.p) == b;
            world::warp_state entrance = world::warp_state::ready;
            w.room_for_each_warp_point_location(a, [&](dcon::warp_point_location_id l) { entrance = w.warp_point_get_state(w.warp_point_location_get_point(l)); });
            char d[96];
            std::snprintf(d, sizeof(d), " through=%d stayed_at_exit=%d entrance_closed=%d", int(there), int(stayed), int(entrance == world::warp_state::closed));
            r.check("one way warp", there && stayed && entrance == world::warp_state::closed, s.describe() + d);
        }
        {
            // holding the ability key: camouflage after 80 ticks, and from
            // ripple level 3 the Watcher floats while holding it
            auto& w = gs.world;
            auto const room = make_room(gs, region, box(30, 20, [](int32_t, int32_t) { return ' '; }));
            gs.ripple_level = 3.f;
            sim s(gs, room, feet_at(10, 1), player::slugcat_name::watcher);
            s.run(40);
            float const y0 = s.pos(1).y;
            float peak = y0;
            auto hold = player::make_input(0.f, 0.f, false, false, false);
            hold.spec = true;
            for (int i = 0; i < 100; ++i) {
                simulation::tick(gs, std::span{&s.p, 1}, std::span{&hold, 1});
                peak = std::max(peak, s.pos(1).y);
            }
            bool const camo = w.player_get_is_camo(s.p) != 0;
            float const fade = w.player_get_camo_progress(s.p);
            s.run(200);
            bool const down = s.contact(1).y == -1;
            char d[128];
            std::snprintf(d, sizeof(d), " camo=%d fade=%.2f lift=%.0fpx back_down=%d", int(camo), fade, peak - y0, int(down));
            r.check("camouflage and levitation", camo && fade > 0.5f && peak - y0 > 15.f && down, s.describe() + d);
        }
        {
            // a ripple warp only works from the ripple layer (camouflaged at ripple level 5)
            auto const a = make_room(gs, region, box(30, 20, [](int32_t, int32_t) { return ' '; }));
            auto const b = make_room(gs, region, box(30, 20, [](int32_t, int32_t) { return ' '; }));
            add_warp(gs, a, {feet_at(15, 1) + vec2{0.f, 20.f}, b, feet_at(10, 1) + vec2{0.f, 20.f}, false, false, true});
            gs.ripple_level = 5.f;
            sim s(gs, a, feet_at(15, 1), player::slugcat_name::watcher);
            s.run(700);
            bool const blocked = simulation::player_room(gs, s.p) == a && !gs.warp.active;
            auto hold = player::make_input(0.f, 0.f, false, false, false);
            hold.spec = true;
            for (int i = 0; i < 85; ++i)
                simulation::tick(gs, std::span{&s.p, 1}, std::span{&hold, 1});
            bool const layer = warps::active_ripple_layer(gs) == 1;
            bool const through = s.until(1500, [&] { return simulation::player_room(gs, s.p) == b; });
            char d[96];
            std::snprintf(d, sizeof(d), " blocked_outside=%d ripple_layer=%d through=%d", int(blocked), int(layer), int(through));
            r.check("ripple warp needs the ripple layer", blocked && layer && through, s.describe() + d);
            s.until(800, [&] { return !gs.warp.active; });
            gs.ripple_level = 1.f;
        }
        {
            // a rock dropped into water: the surface dips and rings outwards,
            // droplets fly, bubbles follow it down; the waves settle again
            auto const pool = make_room(gs, region, box(40, 20, [](int32_t, int32_t) { return ' '; }), 8);
            sim s(gs, pool, feet_at(3, 12));
            gs.particles.clear();
            s.run(20);
            auto const* surf = water::surface(gs, pool);
            auto const rock = items::create_rock(gs, pool, vec2{400.f, 340.f});
            gs.world.body_chunk_set_vel(items::chunk(gs, rock), vec2{0.f, -12.f});
            float dip = 0.f, far_wave = 0.f;
            size_t drops = 0, bubbles = 0;
            for (int i = 0; i < 60; ++i) {
                s.step();
                surf = water::surface(gs, pool);
                if (surf == nullptr)
                    break;
                dip = std::min(dip, water::height(gs, *surf, 400.f, 0.f, 1.f) - water::height(gs, *surf, 700.f, 0.f, 1.f));
                for (auto const& p : gs.particles) {
                    drops += p.kind == uint8_t(particles::kind::drop) ? 1 : 0;
                    bubbles += p.kind == uint8_t(particles::kind::bubble) ? 1 : 0;
                }
            }
            // the ripple reaches further out later
            for (int i = 0; i < 40 && surf; ++i) {
                s.step();
                far_wave = std::max(far_wave, std::fabs(water::height(gs, *surf, 520.f, 0.f, 1.f) - water::height(gs, *surf, 520.f, 0.f, 0.f)));
            }
            s.run(1500);
            float calm = 0.f;
            for (uint32_t i = 0; surf && i < surf->h.size(); ++i)
                calm = std::max(calm, std::fabs(surf->h[i]));
            char d[128];
            std::snprintf(d, sizeof(d), " surface=%d dip=%.1f moving_out=%.2f drops=%zu bubbles=%zu settled=%.2f", int(surf != nullptr), dip, far_wave, drops, bubbles, calm);
            r.check("water splash and ripples", surf && dip < -2.f && far_wave > 0.05f && drops > 0 && bubbles > 0 && calm < 1.f, s.describe() + d);
            items::destroy(gs, rock);
        }
        {
            // a rock thrown at a wall throws sparks
            sim s(gs, flat, feet_at(10, 1));
            gs.particles.clear();
            auto const rock = items::create_rock(gs, flat, feet_at(35, 3));
            gs.world.body_chunk_set_vel(items::chunk(gs, rock), vec2{20.f, 0.f});
            gs.world.item_set_mode(rock, world::weapon_mode::thrown);
            size_t sparks = 0;
            for (int i = 0; i < 20; ++i) {
                s.step();
                for (auto const& p : gs.particles)
                    sparks += p.kind == uint8_t(particles::kind::spark) ? 1 : 0;
            }
            r.check("hits throw sparks", sparks > 0, s.describe() + " sparks=" + std::to_string(sparks));
            items::destroy(gs, rock);
        }
        {
            // rain stops at the water instead of falling through it
            auto const pool = make_room(gs, region, box(40, 20, [](int32_t, int32_t) { return ' '; }), 8);
            sim s(gs, pool, feet_at(3, 12));
            gs.particles.clear();
            size_t under = 0, rain = 0;
            for (int i = 0; i < 120; ++i) {
                // a sealed test room: rain it from just under the ceiling
                particles::spawn(gs, pool, particles::kind::rain, vec2{100.f + float(i % 30) * 20.f, 360.f}, vec2{-1.f, -24.f}, 1.f, 3.f, rw::vec3{1.f, 1.f, 1.f});
                s.step();
                for (auto const& p : gs.particles) {
                    if (p.kind != uint8_t(particles::kind::rain) || p.room != pool)
                        continue;
                    ++rain;
                    under += p.pos.y < s.room.float_water_level - 40.f ? 1 : 0;
                }
            }
            r.check("rain stops at the water", rain > 0 && under == 0, s.describe() + " rain=" + std::to_string(rain) + " under=" + std::to_string(under));
        }
        {
            // the Artificer blows up under water instead of drowning
            auto& w = gs.world;
            auto const pool = make_room(gs, region, box(40, 20, [](int32_t, int32_t) { return ' '; }), 17);
            sim s(gs, pool, feet_at(20, 1), player::slugcat_name::artificer);
            bool const blew = s.until(2000, [&] { return s.dead(); });
            float const drown = w.player_get_drown(s.p);
            bool flash = false;
            for (auto const& p : gs.particles)
                flash = flash || (p.kind == uint8_t(particles::kind::flash) && p.room == pool);
            char d[96];
            std::snprintf(d, sizeof(d), " dead=%d drown=%.2f air=%.2f flash=%d", int(blew), drown, w.player_get_air_in_lungs(s.p), int(flash));
            r.check("artificer explodes under water", blew && drown == 0.f && w.player_get_air_in_lungs(s.p) > 0.5f && flash, s.describe() + d);
        }
        {
            // sand: standing on it leaves a shallow mark without sinking in;
            // a sandstorm works the dunes towards smooth sine dunes again
            auto& w = gs.world;
            auto const room = make_room(gs, region, box(60, 20, [](int32_t, int32_t) { return ' '; }));
            for (int k = 0; k < 5; ++k) {
                auto const t = w.create_placed_thing();
                w.placed_thing_set_kind(t, world::thing_kind::terrain_handle);
                w.placed_thing_set_pos(t, vec2{100.f + float(k) * 250.f, 120.f + (k % 2 == 0 ? 0.f : 40.f)});
                w.placed_thing_set_handle(t, vec2{-80.f, 0.f});
                w.placed_thing_set_handle2(t, vec2{80.f, 0.f});
                w.placed_thing_set_extra(t, 20.f);
                w.try_create_thing_location(t, room);
            }
            sim s(gs, room, vec2{600.f, 200.f});
            s.run(5);
            auto const* sand = ::terrain::sand(gs, room);
            float const before = sand ? ::terrain::sand_height(*sand, 600.f) : 0.f;
            s.run(400);
            float const after = sand ? ::terrain::sand_height(*sand, 600.f) : 0.f;
            bool const on_top = s.pos(1).y - 8.f >= after - 1.f && s.contact(1).y == -1;
            // a storm, the slugcat out of the way
            player::reset(gs, s.p, room, vec2{40.f, 300.f});
            auto const rough = [&] {
                float worst = 0.f;
                for (size_t i = 1; i + 1 < sand->h.size(); ++i)
                    worst = std::max(worst, std::fabs(sand->h[i - 1] - 2.f * sand->h[i] + sand->h[i + 1]));
                return worst;
            };
            float const rough0 = rough();
            gs.storm = true;
            s.run(3000);
            gs.storm = false;
            float const rough1 = rough();
            gs.particles.clear();
            char d[160];
            std::snprintf(d, sizeof(d), " sand=%d mark=%.2f on_top=%d roughness %.3f -> %.3f", int(sand != nullptr), before - after, int(on_top), rough0, rough1);
            r.check("sand marks and sandstorm", sand && before - after > 0.2f && before - after < 8.f && on_top && rough1 < rough0, s.describe() + d);
        }
        {
            // RegionGate: floor at y < 8, doors at x 14/23/32, rows 8..16
            auto const gate_room = make_room(gs, region, box(48, 25, [](int32_t x, int32_t y) {
                if (y < 8) return '#';
                if ((x == 23 || x == 24) && y <= 16) return '#'; // the middle door starts closed
                return ' ';
            }));
            auto& w = gs.world;
            w.room_set_is_gate(gate_room, true);
            auto const g = w.create_gate();
            w.gate_set_mode(g, world::gate_mode::middle_closed);
            w.gate_set_door_closed(g, rw::vec3{0.f, 1.f, 0.f});
            w.gate_set_door_goal(g, rw::vec3{0.f, 1.f, 0.f});
            w.gate_set_last_closed(g, 0b010);
            w.try_create_gate_location(g, gate_room);
            sim s(gs, gate_room, feet_at(19, 8));
            // stand still in zone 1 until the airlock cycles and the middle opens
            bool const opened = s.until(1200, [&] { return w.gate_get_mode(g) == world::gate_mode::middle_open; });
            auto const view = world::get_room_view(gs, gate_room);
            bool const passable = view.get_tile(23, 9).t == terrain::air && view.get_tile(14, 9).t == terrain::solid;
            s.run(200, 1);
            bool const through = s.pos(1).x > 27.f * 20.f;
            bool const reset = s.until(1500, [&] { return w.gate_get_mode(g) == world::gate_mode::middle_closed; });
            char d[96];
            std::snprintf(d, sizeof(d), " opened=%d passable=%d through=%d rearmed=%d", int(opened), int(passable), int(through), int(reset));
            r.check("region gate cycle", opened && passable && through && reset && view.get_tile(14, 9).t == terrain::air, s.describe() + d);
        }

        // a placed object from the room's settings, spawned on the first visit
        auto const add_thing = [&](dcon::room_id room, world::thing_kind kind, vec2 pos, vec2 handle = {}, vec2 handle2 = {}, float extra = 0.f,
                                   std::string_view name = {}) {
            auto& w = gs.world;
            auto const t = w.create_placed_thing();
            w.placed_thing_set_kind(t, kind);
            w.placed_thing_set_pos(t, pos);
            w.placed_thing_set_handle(t, handle);
            w.placed_thing_set_handle2(t, handle2);
            w.placed_thing_set_extra(t, extra);
            if (!name.empty())
                w.placed_thing_set_name(t, gs.add_text(name));
            w.try_create_thing_location(t, room);
        };
        {
            // a placed CC pearl: its colours, the glimmer, picked up and tossed
            auto& w = gs.world;
            auto const room = make_room(gs, region, box(30, 12, [](int32_t, int32_t) { return ' '; }));
            add_thing(room, world::thing_kind::data_pearl, vec2{250.f, 30.f}, {}, {}, 0.f, "cc");
            w.room_set_items_spawned(room, false);
            items::spawn_room_items(gs, room);
            dcon::item_id pearl;
            w.for_each_item([&](dcon::item_id it) {
                if (w.physical_object_get_room_from_object_location(items::body(gs, it)) == room && w.item_get_type(it) == world::item_type::data_pearl)
                    pearl = it;
            });
            bool const placed = pearl && rw::distance(w.body_chunk_get_pos(items::chunk(gs, pearl)), vec2{250.f, 30.f}) < 1.f;
            bool const colours = pearl && w.item_get_color(pearl) == rw::vec3{0.9f, 0.6f, 0.1f} && w.item_get_pearl(pearl).has_highlight
                && w.item_get_pearl(pearl).highlight == rw::vec3{1.f, 1.f, 0.f};
            sim s(gs, room, feet_at(11, 1));
            float glimmer = 0.f;
            s.until(120, [&] {
                glimmer = std::max(glimmer, pearl ? w.item_get_pearl(pearl).glimmer : 0.f);
                return false;
            });
            s.step(0, 0, false, false, true);
            s.run(5);
            bool const held = pearl && items::grabber(gs, pearl) == s.p;
            s.run(10);
            s.step(0, 0, false, true);
            bool const tossed = pearl && !items::grabber(gs, pearl) && w.item_get_mode(pearl) == world::weapon_mode::free;
            float const x0 = pearl ? w.body_chunk_get_pos(items::chunk(gs, pearl)).x : 0.f;
            bool const landed = s.until(120, [&] { return pearl && w.body_chunk_get_contact_point(items::chunk(gs, pearl)).y < 0; });
            float const dx = pearl ? w.body_chunk_get_pos(items::chunk(gs, pearl)).x - x0 : 0.f;
            char d[160];
            std::snprintf(d, sizeof(d), " placed=%d colours=%d glimmer=%.2f held=%d tossed=%d landed=%d dx=%.0f", int(placed), int(colours), glimmer,
                int(held), int(tossed), int(landed), dx);
            r.check("data pearl placed and tossed", placed && colours && glimmer > 0.f && held && tossed && landed && dx > 10.f, s.describe() + d);
            if (pearl)
                items::destroy(gs, pearl);
        }
        {
            // a scavenger toll: the outpost's spears and pearl strings, a
            // treasury's loot and a reliable spear, on the room's first visit
            auto& w = gs.world;
            auto const room = make_room(gs, region, box(80, 30, [](int32_t, int32_t) { return ' '; }));
            vec2 const post{800.f, 20.f};
            add_thing(room, world::thing_kind::scav_outpost, post, vec2{10.f, 224.f}, vec2{25.f, 75.f}, 0.5760869f);
            add_thing(room, world::thing_kind::scav_treasury, vec2{300.f, 30.f}, vec2{120.f, 0.f});
            add_thing(room, world::thing_kind::reliable_spear, vec2{1400.f, 30.f});
            uint32_t const before = w.outpost_size();
            w.room_set_items_spawned(room, false);
            items::spawn_room_items(gs, room);
            dcon::outpost_id o;
            w.room_for_each_outpost_location(room, [&](dcon::outpost_location_id l) { o = w.outpost_location_get_outpost(l); });
            dcon::treasury_id tr;
            w.room_for_each_treasury_location(room, [&](dcon::treasury_location_id l) { tr = w.treasury_location_get_treasury(l); });
            uint32_t spears = 0, pearls = 0, loot = 0, reliable = 0;
            w.for_each_item([&](dcon::item_id it) {
                if (w.physical_object_get_room_from_object_location(items::body(gs, it)) != room)
                    return;
                vec2 const p = w.body_chunk_get_pos(items::chunk(gs, it));
                auto const t = w.item_get_type(it);
                if (t == world::item_type::data_pearl)
                    ++pearls;
                if (t == world::item_type::spear && p.x > 1300.f)
                    ++reliable;
                else if (t == world::item_type::spear && std::fabs(p.x - post.x) < 300.f)
                    ++spears;
                if (rw::distance(p, vec2{300.f, 30.f}) < 125.f)
                    ++loot;
            });
            uint32_t strings = 0, strung = 0;
            if (o)
                w.outpost_for_each_string_outpost(o, [&](dcon::string_outpost_id so) {
                    ++strings;
                    strung += w.pearl_string_get_pearls(w.string_outpost_get_string(so)).size();
                });
            uint32_t const tolls = o ? w.outpost_get_toll_spears(o) : 0;
            // the strings hang their pearls from the antlers (a soft rope: gravity
            // stretches it a little between the constraint passes)
            sim s(gs, room, feet_at(60, 1));
            s.run(200);
            float worst = 0.f;
            if (o)
                w.outpost_for_each_string_outpost(o, [&](dcon::string_outpost_id so) {
                    auto const st = w.string_outpost_get_string(so);
                    auto const ps = w.pearl_string_get_pearls(st);
                    vec2 prev = outposts::attached_pos(gs, st);
                    for (uint32_t i = 0; i < ps.size(); ++i) {
                        auto const it = outposts::pearl_item(gs, ps.at(i));
                        if (!it)
                            continue;
                        vec2 const p = w.body_chunk_get_pos(items::chunk(gs, it));
                        worst = std::max(worst, rw::distance(p, prev) - ps.at(i).rad);
                        prev = p;
                    }
                });
            char d[200];
            std::snprintf(d, sizeof(d), " outposts=%u toll=%u spears=%u strings=%u strung=%u pearls=%u loot=%u/%u reliable=%u stretch=%.1f",
                w.outpost_size() - before, tolls, spears, strings, strung, pearls, loot, tr ? uint32_t(w.treasury_get_loot(tr)) : 0u, reliable, worst);
            bool const ok = o && tolls >= 13 && tolls <= 24 && spears >= tolls && strings >= 5 && strings < 15 && strung >= strings && pearls >= strung
                && tr && w.treasury_get_loot(tr) > 0 && loot >= w.treasury_get_loot(tr) && reliable == 1 && worst < 15.f;
            r.check("scavenger toll and treasury", ok, s.describe() + d);
        }

        {
            // the loops' bookkeeping on the audio side, without a device:
            // kept up tick by tick, faded out once not asked for, restarted
            // on another sound, only for the room heard
            using sound::sound_id;
            auto const req = [](uint64_t owner, sound_id id, uint32_t room, uint16_t fade_frames = 0) {
                sound::loop_request q;
                q.owner = owner;
                q.id = id;
                q.room = room;
                q.fade_out_frames = fade_frames;
                return q;
            };
            std::vector<audio::loop_state> loops;
            std::vector<size_t> started;
            std::vector<uint32_t> dropped;
            auto const find = [&](uint64_t owner) -> audio::loop_state const* {
                for (auto const& l : loops)
                    if (l.req.owner == owner && !l.stopping)
                        return &l;
                return nullptr;
            };
            std::vector<sound::loop_request> tick{req(1, sound_id::cicada_wings_loop, 7), req(2, sound_id::centipede_crawl_loop, 7),
                req(3, sound_id::slugcat_roll_loop, 8)};
            audio::upkeep(loops, tick, 7, 24, started);
            bool const both = started.size() == 2 && loops.size() == 2 && find(1) && find(2) && !find(3);
            audio::fade(loops, 0.1f, dropped);
            bool const up = find(1) && find(1)->envelope == 1.f;
            // owner 2 isn't kept up: fades out, then goes
            tick = {req(1, sound_id::cicada_wings_loop, 7, 60)};
            started.clear();
            audio::upkeep(loops, tick, 7, 24, started);
            audio::fade(loops, 0.02f, dropped);
            bool const fading = loops.size() == 2 && !find(2) && started.empty();
            audio::fade(loops, 0.1f, dropped);
            bool const gone = loops.size() == 1 && find(1);
            // a paused game (no new tick) leaves them alone: nothing to do
            // here, upkeep only runs per tick. Another sound: the old one
            // fades out over its 60 frames, a new one starts
            tick = {req(1, sound_id::cicada_wings_bump_attack_charge_loop, 7)};
            started.clear();
            audio::upkeep(loops, tick, 7, 24, started);
            bool const swapped = started.size() == 1 && loops.size() == 2 && loops[0].stopping && loops[0].fade_out > 0.9f
                && find(1)->req.id == sound_id::cicada_wings_bump_attack_charge_loop;
            audio::fade(loops, 0.5f, dropped);
            bool const slow = loops.size() == 2;
            // the view moves to another room: what was left behind stops at once
            tick = {req(1, sound_id::cicada_wings_bump_attack_charge_loop, 7)};
            audio::upkeep(loops, tick, 8, 24, started);
            audio::fade(loops, 0.1f, dropped);
            bool const left = loops.empty();
            // at most max_loops at once
            tick.clear();
            for (uint64_t o = 10; o < 40; ++o)
                tick.push_back(req(o, sound_id::centipede_crawl_loop, 8));
            started.clear();
            audio::upkeep(loops, tick, 8, 24, started);
            bool const capped = loops.size() == 24;
            char d[128];
            std::snprintf(d, sizeof(d), " both=%d up=%d fading=%d gone=%d swapped=%d slow=%d left=%d capped=%d", int(both), int(up), int(fading),
                int(gone), int(swapped), int(slow), int(left), int(capped));
            r.check("loop upkeep and fade out", both && up && fading && gone && swapped && slow && left && capped, d);
        }
        {
            // the simulation side: a squidcada's wing hum is asked for every
            // tick it flies in a viewed room, not from a room nobody views,
            // and not once it is destroyed
            auto const near = make_room(gs, region, box(40, 30, [](int32_t, int32_t) { return ' '; }));
            auto const far = make_room(gs, region, box(40, 30, [](int32_t, int32_t) { return ' '; }));
            sim s(gs, near, feet_at(3, 1));
            auto const a = cicada::create(gs, near, world::room_view::middle_of_tile(30, 20), true);
            auto const b = cicada::create(gs, far, world::room_view::middle_of_tile(30, 20), true);
            auto const key_a = sound::loop_key(sound::loop_owner::creature, uint32_t(a.index()));
            auto const key_b = sound::loop_key(sound::loop_owner::creature, uint32_t(b.index()));
            auto const asked = [&](uint64_t key) {
                for (auto const& l : gs.loops)
                    if (l.owner == key && l.vol > 0.f)
                        return true;
                return false;
            };
            bool far_heard = false;
            uint32_t const tick0 = gs.loop_tick;
            bool const hums = s.until(1200, [&] {
                far_heard = far_heard || asked(key_b);
                return asked(key_a);
            });
            bool const ticked = gs.loop_tick > tick0;
            // a camera on the far room: its squidcada is heard too
            gs.camera_rooms.assign(1, far);
            bool const viewed = s.until(1200, [&] { return asked(key_b); });
            gs.camera_rooms.clear();
            s.step();
            bool const unviewed = !asked(key_b);
            cicada::destroy(gs, a);
            s.step();
            bool const destroyed = !asked(key_a);
            cicada::destroy(gs, b);
            gs.sounds.clear();
            char d[128];
            std::snprintf(d, sizeof(d), " hums=%d far=%d ticked=%d viewed=%d unviewed=%d destroyed=%d", int(hums), int(far_heard), int(ticked),
                int(viewed), int(unviewed), int(destroyed));
            r.check("loops only where viewed", hums && !far_heard && ticked && viewed && unviewed && destroyed, s.describe() + d);
        }
        {
            // RainCycle timing and the gate groups: A and B linked by a gate
            // share A's timer, C (linked to the never raining D only) has its
            // own, D (every room DangerType None, like Five Pebbles) never rains
            auto const st_ptr = sys::make_game_state();
            auto& st = *st_ptr;
            auto& w = st.world;
            dcon::region_id rg[4];
            char const* const names[] = {"ta", "tb", "tc", "ss"};
            dcon::room_id rr[4];
            for (int i = 0; i < 4; ++i) {
                rg[i] = w.create_region();
                w.region_set_name(rg[i], st.add_text(names[i]));
                rr[i] = make_room(st, rg[i], box(20, 10, [](int32_t, int32_t) { return ' '; }));
            }
            w.room_set_danger_type(rr[3], world::danger_type::none);
            st.spawn_dens = false;
            simulation::populate(st);
            rain::group(st, {{rg[0], rg[1]}, {rg[2], rg[3]}});
            bool const leaders = w.region_get_rain_leader(rg[1]) == rg[0] && w.region_get_rain_leader(rg[0]) == rg[0]
                && w.region_get_rain_leader(rg[2]) == rg[2] && w.region_get_rain_leader(rg[3]) == rg[3] && w.region_get_never_rains(rg[3]);
            // vanilla's story cycle: 400 to 800 seconds
            bool lengths = true;
            for (auto const g : rg)
                lengths = lengths && w.region_get_rain(g).cycle_length >= 16000 && w.region_get_rain(g).cycle_length <= 32000;
            rain::rain_in(st, rg[0], 50);
            rain::rain_in(st, rg[2], 2000);
            bool synced = true, hit = false, deadly_max = false, flooded = false;
            int32_t hit_at = -1, over_at = -1;
            float peak = 0.f;
            for (int32_t t = 0; t < 600; ++t) {
                tick_world(st, 1);
                auto const& a = w.region_get_rain(rg[0]);
                auto const& b = w.region_get_rain(rg[1]);
                synced = synced && a.timer == b.timer && a.cycle_length == b.cycle_length && a.intensity == b.intensity;
                if (rain::deadly(a) && hit_at < 0)
                    hit_at = t;
                if (hit_at >= 0 && !rain::deadly(a) && over_at < 0)
                    over_at = t;
                hit = hit || rain::deadly(a);
                peak = std::max(peak, a.intensity);
                deadly_max = deadly_max || a.mode == rain::death_mode::mayhem;
                flooded = flooded || a.flood > 100.f;
            }
            auto const& a = w.region_get_rain(rg[0]);
            auto const& c = w.region_get_rain(rg[2]);
            bool const restarted = a.cycles == 1 && a.timer < 300 && a.cycle_length >= 16000 && !rain::deadly(a);
            bool const own = !rain::deadly(c) && rain::time_until_rain(c) > 1000 && c.cycle_length != a.cycle_length;
            bool const dry = rain::tracker_utility(st, rr[3]) == 0.f && !rain::rains_in(st, rr[3]) && rain::rains_in(st, rr[0]);
            char d[256];
            std::snprintf(d, sizeof(d), " leaders=%d lengths=%d synced=%d hit_at=%d over_at=%d peak=%.2f mayhem=%d flooded=%d restarted=%d own=%d dry=%d",
                int(leaders), int(lengths), int(synced), hit_at, over_at, peak, int(deadly_max), int(flooded), int(restarted), int(own), int(dry));
            r.check("rain cycles and gate groups", leaders && lengths && synced && hit && over_at - hit_at == rain::deadly_rain_ticks && peak > 0.95f
                && deadly_max && flooded && restarted && own && dry, d);
        }
        {
            // the deadly rain: a slugcat under the open sky dies, one in a
            // shelter doesn't
            auto const st_ptr = sys::make_game_state();
            auto& st = *st_ptr;
            auto& w = st.world;
            auto const rgn = w.create_region();
            w.region_set_name(rgn, st.add_text("rain"));
            auto const open = make_room(st, rgn, box(40, 20, [](int32_t x, int32_t y) { return y == 1 || (y > 12 && x > 25) ? '#' : ' '; }));
            // (a room with nothing over its floor but the room's top border)
            for (int32_t x = 1; x < 39; ++x)
                st.world.tile_set_terrain(dcon::tile_id{dcon::tile_id::value_base_t(st.world.room_get_first_tile(open).id.index() + 19 * 40 + x)}, world::terrain::air);
            auto const shelter = make_room(st, rgn, box(20, 10, [](int32_t, int32_t) { return ' '; }));
            w.room_set_is_shelter(shelter, true);
            st.spawn_dens = false;
            simulation::populate(st);
            std::vector<dcon::player_id> ps{player::create(st, open, feet_at(10, 2)), player::create(st, shelter, feet_at(10, 1))};
            rain::rain_in(st, rgn, 20);
            std::vector<player::input_package> in(ps.size());
            bool pushed = false;
            float death = 0.f;
            for (uint32_t t = 0; t < 440; ++t) {
                vec2 const before = w.body_chunk_get_vel(player::body_chunk(st, ps[0], 0));
                simulation::tick(st, ps, in);
                st.sounds.clear();
                st.music.clear();
                pushed = pushed || rw::magnitude(w.body_chunk_get_vel(player::body_chunk(st, ps[0], 0)) - before) > 3.f;
                death = std::max(death, w.player_get_rain_death(ps[0]));
            }
            bool const exposed_dead = w.player_get_dead(ps[0]) != 0;
            bool const sheltered = !w.player_get_dead(ps[1]) && w.player_get_rain_death(ps[1]) == 0.f;
            char d[160];
            std::snprintf(d, sizeof(d), " exposed dead=%d pushed=%d rainDeath %.2f; sheltered alive=%d", int(exposed_dead), int(pushed), death, int(sheltered));
            r.check("deadly rain, shelter", exposed_dead && pushed && sheltered, d);
            for (auto const p : ps)
                player::destroy(st, p);
        }
        {
            // a lizard comes out of its den, goes back in as the rain
            // approaches, waits it out and comes back out of the same den
            auto const st_ptr = sys::make_game_state();
            auto& st = *st_ptr;
            auto& w = st.world;
            auto const rgn = w.create_region();
            w.region_set_name(rgn, st.add_text("dens"));
            auto const room = make_room(st, rgn, pipe_room(34, 0));
            st.spawn_dens = false;
            simulation::populate(st);
            auto const den = dens::den_entrance(st, room, 0);
            auto const l = creature::create(st, room, world::room_view::middle_of_tile(34, 2), creature::spawn_desc{creature::type::lizard, -1.f, false, uint8_t(lizard::breed::pink)});
            dens::set_home(st, l, room, 0);
            dens::put_in_den(st, l, 10);
            bool const starts_in = dens::where_is(st, l) == dens::where::den && !creature::room_of(st, l);
            tick_world(st, 60);
            bool const came_out = dens::where_is(st, l) == dens::where::room && creature::room_of(st, l) == room;
            // off to the far side of the room for a while
            tick_world(st, 400);
            float const away = std::fabs(w.body_chunk_get_pos(creature::chunk(st, l, 0)).x - world::room_view::middle_of_tile(34, 2).x);
            rain::rain_in(st, rgn, 900);
            bool went_in = false, escaped = false;
            for (uint32_t t = 0; t < 900 && !went_in; ++t) {
                tick_world(st, 1);
                escaped = escaped || lizard::behavior(w.creature_get_behavior(l)) == lizard::behavior::escape_rain;
                went_in = dens::where_is(st, l) == dens::where::den;
            }
            // the rain, then the next cycle: out again
            bool stayed = true, out_again = false;
            vec2 out_at{0.f, 0.f};
            for (uint32_t t = 0; t < 2600 && !out_again; ++t) {
                tick_world(st, 1);
                if (rain::deadly(w.region_get_rain(rgn)))
                    stayed = stayed && dens::where_is(st, l) != dens::where::room;
                if (dens::where_is(st, l) == dens::where::room) {
                    out_again = true;
                    out_at = w.body_chunk_get_pos(creature::chunk(st, l, 0));
                }
            }
            bool const same_den = den && out_again && rw::distance(out_at, world::room_view::middle_of_tile(*den)) < 60.f;
            char d[200];
            std::snprintf(d, sizeof(d), " den=%d in=%d out=%d wandered %.0fpx escape=%d back_in=%d stayed=%d out_again=%d same_den=%d cycles=%u", int(bool(den)),
                int(starts_in), int(came_out), away, int(escaped), int(went_in), int(stayed), int(out_again), int(same_den), w.region_get_rain(rgn).cycles);
            r.check("den before the rain", starts_in && came_out && escaped && went_in && stayed && out_again && same_den, d);
        }
        {
            // a centipede whose den is in the next room goes through the
            // room exit pipe to get to it
            auto const st_ptr = sys::make_game_state();
            auto& st = *st_ptr;
            auto& w = st.world;
            auto const rgn = w.create_region();
            w.region_set_name(rgn, st.add_text("pipes"));
            auto const a = make_room(st, rgn, pipe_room(0, 34));
            auto const b = make_room(st, rgn, pipe_room(5, 34));
            auto const ea = exits_of(st, a), eb = exits_of(st, b);
            bool const built = ea.size() == 1 && eb.size() == 2;
            if (built) {
                w.room_exit_set_target_from_exit_connection(ea[0], b);
                w.room_exit_set_target_from_exit_connection(eb[0], a);
            }
            st.spawn_dens = false;
            simulation::populate(st);
            auto const c = creature::create(st, a, world::room_view::middle_of_tile(8, 2), creature::spawn_desc{creature::type::centipede, 0.4f});
            dens::set_home(st, c, b, 1);
            rain::rain_in(st, rgn, 2400);
            bool piped = false, crossed = false, home = false;
            for (uint32_t t = 0; t < 2400 && !home; ++t) {
                tick_world(st, 1);
                piped = piped || dens::where_is(st, c) == dens::where::shortcut;
                crossed = crossed || creature::room_of(st, c) == b;
                home = dens::where_is(st, c) == dens::where::den && w.creature_get_away_room(c) == b;
            }
            char d[160];
            std::snprintf(d, sizeof(d), " built=%d piped=%d in_b=%d home=%d moves=%u", int(built), int(piped), int(crossed), int(home), dens::stats().room_changes.load());
            r.check("through a pipe to its den", built && piped && crossed && home, d);
        }
        {
            // a world file's creatures: den spawns, counts, lineages,
            // campaign filters, spawn data
            std::string const text = "ROOMS\nTEST_A : DISCONNECTED\nEND ROOMS\n\nCREATURES\n"
                "//all\n"
                "TEST_A : 3-Green, 3-CicadaA-2, 3-Scavenger\n"
                "(White,Yellow)TEST_A : 3-Pink-{Seed:1234}\n"
                "(Red)TEST_A : 3-Red\n"
                "(X-White)TEST_A : 3-Black\n"
                "LINEAGE : TEST_A : 3 : NONE-0.1, Blue-0.5, Red-0\n"
                "LINEAGE : TEST_A : 3 : Centipede-{0.7}-0.5, Red-0\n"
                "TEST_A : 3-Small Centipede-{Night,Seed:55}-3\n"
                "OFFSCREEN : 0-Vulture-2\n"
                "END CREATURES\n";
            auto const white = spawns::parse(text, player::slugcat_name::white);
            auto const red = spawns::parse(text, player::slugcat_name::red);
            auto const has = [](std::vector<spawns::spawn> const& v, std::string const& type) {
                return std::any_of(v.begin(), v.end(), [&](auto const& x) { return x.type == type; });
            };
            bool parsed = white.size() == 8 && white[1].count == 2 && white[3].data == "{Seed:1234}" && white[4].lineage && white[4].type == "NONE"
                && white[5].type == "Centipede" && white[5].data == "{0.7}" && white[6].type == "Small Centipede" && white[6].count == 3 && white[7].room == "OFFSCREEN";
            bool const filters = has(white, "Pink") && !has(white, "Red") && !has(white, "Black") && has(red, "Red") && has(red, "Black") && !has(red, "Pink");
            auto const night = spawns::parse_data("{Night,Seed:55}");
            auto const size = spawns::parse_data("{0.7}");
            bool const data = (night.flags & dens::flag_night) && night.seed == 55 && size.size == 0.7f && spawns::parse_data("{PreCycle,Lavasafe}").flags == (dens::flag_pre_cycle | dens::flag_lava_safe);
            bool const kinds = spawns::kind_of("Small Centipede") && spawns::kind_of("Small Centipede")->variety == uint8_t(centipede::variant::small)
                && spawns::kind_of("Caramel")->variety == uint8_t(lizard::breed::caramel) && spawns::kind_of("CicadaA")->size == 1.f
                && spawns::kind_of("TerrorLongLegs")->variety == uint8_t(daddy::variant::mother) && !spawns::kind_of("Scavenger") && !spawns::kind_of("NONE");
            // made in a room's den: the count, the skipped kind, NONE, the seed
            auto const st_ptr = sys::make_game_state();
            auto& st = *st_ptr;
            auto const rgn = st.world.create_region();
            st.world.region_set_name(rgn, st.add_text("test"));
            auto const room = make_room(st, rgn, pipe_room(34, 0));
            st.spawn_dens = false;
            simulation::populate(st);
            spawns::report rep;
            uint32_t made = 0;
            for (auto const& x : white) {
                if (x.room == "OFFSCREEN" || (x.lineage && x.type == "NONE"))
                    continue;
                // (the room's only den is node 0)
                auto y = x;
                y.den = 0;
                made += spawns::spawn_in_den(st, room, y, rep);
            }
            uint32_t in_den = 0, nights = 0;
            st.world.for_each_creature([&](dcon::creature_id c) {
                in_den += dens::where_is(st, c) == dens::where::den ? 1 : 0;
                nights += st.world.creature_get_spawn_flags(c) & dens::flag_night ? 1 : 0;
            });
            bool const spawned = made == 8 && in_den == 8 && nights == 3 && rep.skipped["Scavenger"] == 1;
            char d[200];
            std::snprintf(d, sizeof(d), " parsed=%d (%zu lines) filters=%d data=%d kinds=%d made=%u in_den=%u night=%u", int(parsed), white.size(), int(filters),
                int(data), int(kinds), made, in_den, nights);
            r.check("world file creature spawns", parsed && filters && data && kinds && spawned, d);
        }
        {
            // every room updates in parallel: the same world comes out on one
            // thread and on every thread
            uint32_t const cores = std::max(2u, std::thread::hardware_concurrency());
            uint64_t const one = determinism_run(1);
            uint64_t const many = determinism_run(cores);
            char d[96];
            std::snprintf(d, sizeof(d), "1 thread %016llx, %u threads %016llx", (unsigned long long)one, cores, (unsigned long long)many);
            r.check("same world on any thread count", one == many, d);
        }

        std::printf("\n%d passed, %d failed\n", r.passed, r.failed);
        return r.failed == 0 ? 0 : 1;
    }

    dcon::creature_id& world_creatures::of(creature::type t) noexcept {
        switch (t) {
        case creature::type::centipede: return centipede;
        case creature::type::dropbug: return dropbug;
        case creature::type::cicada: return cicada;
        case creature::type::lizard: return lizard;
        case creature::type::daddy: return daddy;
        case creature::type::leech: return leeches[0];
        case creature::type::count: break;
        }
        return centipede;
    }

    world_creatures spawn_creatures(sys::game_state& gs, dcon::room_id rid) {
        world_creatures out;
        auto const room = world::get_room_view(gs, rid);
        uint32_t const k0 = uint32_t(rid.index()) * 2654435761u;
        // the next lizard breed, daddy long legs, centipede and leech variant
        // in every room
        out.breed = lizard::breed(uint32_t(rid.index()) % lizard::breed_count);
        out.variant = daddy::variant(uint32_t(rid.index()) % daddy::variant_count);
        out.centipede_variant = centipede::variant(uint32_t(rid.index()) % centipede::variant_count);
        out.leech_variant = leech::variant(uint32_t(rid.index()) % leech::variant_count);
        for (auto const t : creature::all_types) {
            // each kind rolls its spot from its own seed
            uint32_t salt = 0;
            switch (t) {
            case creature::type::centipede: salt = 0xCE7712EDu; break;
            case creature::type::dropbug: salt = 0x51ED27u; break;
            case creature::type::cicada: salt = 0xC1CADAu; break;
            case creature::type::lizard: salt = 0x112A2Du; break;
            case creature::type::daddy: salt = 0xDADD1u; break;
            case creature::type::leech: salt = 0x1EEC4u; break;
            case creature::type::count: break;
            }
            rw::rng rng{(k0 ^ salt) | 1u};
            auto& id = out.of(t);
            for (int k = 0; k < 50 && !id; ++k) {
                ivec2 const tile{rng.range(1, std::max(2, room.width - 1)), rng.range(1, std::max(2, room.height - 1))};
                creature::spawn_desc d{t};
                bool ok = false;
                switch (t) {
                case creature::type::centipede:
                    // somewhere it can stand (an aquapede in the water if
                    // there is any), any size
                    ok = pathing::accessible(room, tile, out.centipede_variant == centipede::variant::aqua ? pathing::aquacenti : pathing::climber);
                    if (ok) {
                        d.size = rng.value();
                        d.variety = uint8_t(out.centipede_variant);
                    }
                    break;
                case creature::type::dropbug:
                    // placed in the room's best ceiling spot like a story session does
                    ok = pathing::accessible(room, tile, pathing::drop_bug);
                    d.relocate = true;
                    break;
                case creature::type::cicada:
                    // somewhere in the air, either colour
                    ok = pathing::accessible(room, tile, pathing::cicada);
                    if (ok)
                        d.size = rng.value() < 0.5f ? 1.f : 0.f;
                    break;
                case creature::type::lizard:
                    // standing somewhere
                    ok = pathing::accessible(room, tile, lizard::params(out.breed).movement) && !room.get_tile(tile).solid();
                    d.variety = uint8_t(out.breed);
                    break;
                case creature::type::daddy:
                    // in the open
                    ok = pathing::accessible(room, tile, pathing::daddy) && pathing::terrain_proximity(room, tile) > 1;
                    d.variety = uint8_t(out.variant);
                    break;
                case creature::type::leech:
                    // under the water (a jungle leech on the ground)
                    ok = out.leech_variant == leech::variant::jungle ? pathing::accessible(room, tile, pathing::standard_ground) && !room.get_tile(tile).solid()
                                                                     : room.get_tile(tile).water == 1 && !room.get_tile(tile).solid();
                    d.variety = uint8_t(out.leech_variant);
                    break;
                case creature::type::count:
                    break;
                }
                if (ok && t == creature::type::leech) {
                    for (uint32_t i = 0; i < world_creatures::school; ++i)
                        out.leeches[i] = creature::create(gs, rid, world::room_view::middle_of_tile(tile) + vec2{float(i) * 6.f - 6.f, 0.f}, d);
                } else if (ok) {
                    id = creature::create(gs, rid, world::room_view::middle_of_tile(tile), d);
                }
            }
        }
        return out;
    }

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
                            x.baby_held = x.baby_held || (cp.variant == centipede::variant::small && w.centipede_data_get_held_by(creature::centipede_data(w, cc)) == x.p.index() + 1);
                            x.aqua_swam = x.aqua_swam || (cp.variant == centipede::variant::aqua
                                && physics::object_submersion(gs, world::get_room_view(gs, x.room), centipede::body(gs, cc)) > 0.5f);
                        } else {
                            x.baby_eaten = x.baby_eaten || c.centipede_variant == centipede::variant::small;
                        }
                    }
                    for (auto const l : c.leeches)
                        if (l && creature::is(w, l, creature::type::leech)) {
                            finite = finite && finite_chunks(gs, leech::body(gs, l));
                            x.leech_latched = x.leech_latched || leech::attached_to(gs, l) == x.p;
                        }
                    if (c.dropbug) {
                        finite = finite && finite_chunks(gs, dropbug::body(gs, c.dropbug));
                        x.bug_dropped = x.bug_dropped || w.creature_get_payload(c.dropbug).dropbug.from_ceiling_jump;
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
                        x.lizard_bit = x.lizard_bit || lizard::grasped(gs, l) == x.p;
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
                        x.daddy_held = x.daddy_held || daddy::holds(gs, dll, x.p);
                        x.daddy_ate = x.daddy_ate || daddy::eating(gs, dll, x.p);
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
                if (x.leech_latched && w.player_get_dead(x.p))
                    ++leeches_drowned_slugcat;
                for (auto const l : c.leeches)
                    if (l && creature::is(w, l, creature::type::leech) && in_room(leech::body(gs, l))) {
                        vec2 const hp = w.body_chunk_get_pos(leech::chunk(gs, l, 0));
                        if (left_room(room, hp) && ++leeches_lost <= 5)
                            std::printf("  %s left %s at (%.0f, %.0f)\n", leech::to_string(c.leech_variant), name.c_str(), hp.x, hp.y);
                    }
                if (w.player_get_eaten(x.p))
                    ++eaten;
                else if (w.player_get_dead(x.p))
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
            std::printf("dens: %u creatures from the world files (at the end %u out, %u in pipes, %u in dens, %u dead); %u went into pipes, %u into dens, %u came out of dens, %u changed rooms, %u slugcats carried off\n",
                natural, out, in_pipes, in_dens, dead, ds.into_pipes.load(), ds.into_dens.load(), ds.out_of_dens.load(), ds.room_changes.load(), ds.prey_taken.load());
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
                    if (!done[i] && simulation::player_room(gs, players[i]) == pipes[b0 + i].target && !w.player_get_in_shortcut(players[i])) {
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
