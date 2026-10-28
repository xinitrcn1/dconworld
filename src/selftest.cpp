#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "selftest.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "world.hpp"

namespace selftest {
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
        // Builds a level file from ASCII art (top row first) and loads it
        // through the real parser. '#' solid, '/' slope, '=' floor, '|' vertical
        // beam, '-' horizontal beam, '+' both beams, ' ' air.
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
            return rid;
        }

        struct sim {
            sys::game_state& gs;
            world::room_view room;
            dcon::player_id p;
            uint32_t ticks = 0;

            sim(sys::game_state& gs, dcon::room_id rid, vec2 feet) : gs{gs}, room{world::get_room_view(gs, rid)} {
                p = player::create(gs, rid, feet);
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
            void step(int32_t x = 0, int32_t y = 0, bool jmp = false) {
                player::update(gs, p, room, player::make_input(float(x), float(y), jmp, false, false));
                ++ticks;
            }
            void run(uint32_t n, int32_t x = 0, int32_t y = 0, bool jmp = false) {
                for (uint32_t i = 0; i < n; ++i)
                    step(x, y, jmp);
            }
            // run until the predicate holds, at most n ticks
            bool until(uint32_t n, std::function<bool()> const& pred, int32_t x = 0, int32_t y = 0, bool jmp = false) {
                for (uint32_t i = 0; i < n; ++i) {
                    if (pred())
                        return true;
                    step(x, y, jmp);
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
    }

    int run() {
        auto const gs_ptr = sys::make_game_state();
        auto& gs = *gs_ptr;
        auto const region = gs.world.create_region();
        gs.world.region_set_name(region, gs.add_text("test"));
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

        std::printf("\n%d passed, %d failed\n", r.passed, r.failed);
        return r.failed == 0 ? 0 : 1;
    }

    int fuzz(sys::game_state& gs, uint32_t ticks_per_room) {
        uint32_t bad = 0;
        rw::rng rng;
        auto& w = gs.world;
        // one player body reused across every room
        auto const first = dcon::room_id{0};
        auto const p = player::create(gs, first, vec2{0.f, 0.f});
        w.for_each_room([&](dcon::room_id rid) {
            auto const room = world::get_room_view(gs, rid);
            vec2 near{0.f, 0.f};
            w.room_for_each_camera_location(rid, [&](dcon::camera_location_id cl) {
                if (near == vec2{0.f, 0.f})
                    near = w.room_camera_get_position(w.camera_location_get_camera(cl)) + vec2{gs.screen_width / 2.f, gs.screen_height / 2.f};
            });
            player::reset(gs, p, rid, find_spawn(room, near));
            int32_t ix = 0, iy = 0;
            bool jmp = false;
            for (uint32_t t = 0; t < ticks_per_room; ++t) {
                if (t % 12 == 0) {
                    ix = rng.range(-1, 2);
                    iy = rng.range(-1, 2);
                    jmp = rng.value() < 0.3f;
                }
                player::update(gs, p, room, player::make_input(float(ix), float(iy), jmp, false, false));
                bool finite = true;
                for (uint32_t i = 0; i < 2; ++i) {
                    vec2 const q = w.body_chunk_get_pos(player::body_chunk(gs, p, i));
                    finite = finite && std::isfinite(q.x) && std::isfinite(q.y);
                }
                if (!finite) {
                    std::printf("room %s: non finite position at tick %u\n", std::string{gs.to_string_view(w.room_get_name(rid))}.c_str(), t);
                    ++bad;
                    break;
                }
            }
        });
        player::destroy(gs, p);
        std::printf("fuzzed %u rooms x %u ticks, %u failures\n", w.room_size(), ticks_per_room, bad);
        return bad == 0 ? 0 : 1;
    }
}
