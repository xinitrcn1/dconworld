#include <cmath>
#include <span>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "selftest.hpp"
#include "physics.hpp"
#include "centipede.hpp"
#include "cicada.hpp"
#include "dropbug.hpp"
#include "lizard.hpp"
#include "items.hpp"
#include "pathing.hpp"
#include "player.hpp"
#include "rot.hpp"
#include "warps.hpp"
#include "particles.hpp"
#include "water.hpp"
#include "terrain.hpp"
#include "simulation.hpp"
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
                gs.world.player_for_each_grasp_as_grabber(s.p, [&](dcon::grasp_id g) { needle = gs.world.grasp_get_grabbed(g); });
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
            std::snprintf(d, sizeof(d), " grabbed=%d killed=%d behavior=%d", int(grabbed), int(killed), int(gs.world.centipede_get_behavior(c)));
            r.check("centipede hunts and shocks", grabbed && killed, s.describe() + d);
            centipede::destroy(gs, c);
        }
        {
            // a small one is afraid of the slugcat; rocks stun it
            auto const pit = make_room(gs, region, box(40, 20, [](int32_t, int32_t) { return ' '; }));
            sim s(gs, pit, feet_at(18, 1));
            auto const c = centipede::create(gs, pit, feet_at(23, 1), 0.f);
            s.run(60);
            bool const flees = gs.world.centipede_get_behavior(c) == world::centipede_behavior::flee;
            // a spear's worth of damage (a rock barely stuns a healthy one)
            centipede::violence(gs, c, 0, vec2{0.f, 0.f}, 1.f, 0.f);
            bool const stunned = gs.world.centipede_get_stun(c) > 0;
            char d[64];
            std::snprintf(d, sizeof(d), " flees=%d stunned=%d", int(flees), int(stunned));
            r.check("small centipede flees, spears stun", flees && stunned && !s.dead(), s.describe() + d);
            centipede::destroy(gs, c);
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
            bool const waits = dropbug::sitting_in_ceiling(gs, d) && gs.world.dropbug_get_in_ceiling_mode(d) == 1.f
                && world::room_view::get_tile_position(gs.world.body_chunk_get_pos(dropbug::chunk(gs, d, 1))).y == 14;
            bool dropped = false;
            bool const hurt = s.until(1500, [&] {
                dropped = dropped || gs.world.dropbug_get_from_ceiling_jump(d);
                return s.stun() > 0 || s.dead() || dropbug::grasped(gs, d) == s.p;
            }, 1); // walking right, under it and on to the wall
            char d_[96];
            std::snprintf(d_, sizeof(d_), " waits=%d dropped=%d hurt=%d behavior=%d", int(waits), int(dropped), int(hurt), int(gs.world.dropbug_get_behavior(d)));
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
            bool const sat = s.until(2000, [&] { return !gs.world.cicada_get_flying(c) && cicada::consious(gs, c); });
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
            bool const tired = gs.world.cicada_get_stamina(c) < 0.5f;
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
                antagonized = antagonized || gs.world.cicada_get_behavior(c) == cicada::behavior::antagonize;
                charged = charged || cicada::charging(gs, c);
                clung = clung || gs.world.cicada_get_sticky_player(c) != 0;
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
                spotted = spotted || gs.world.lizard_get_ai(l).rep.visual_contact;
                hunted = hunted || gs.world.lizard_get_ai(l).current == lizard::behavior::hunt;
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
            auto const l = lizard::create(gs, hall, world::room_view::middle_of_tile(17, 1), lizard::breed::blue);
            bool shot = false;
            bool const caught = s.until(3000, [&] {
                auto const& t = gs.world.lizard_get_tongue(l);
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
            auto const l = lizard::create(gs, pool, world::room_view::middle_of_tile(5, 3), lizard::breed::salamander);
            float const x0 = gs.world.body_chunk_get_pos(lizard::chunk(gs, l, 0)).x;
            bool swam = false;
            float furthest = x0;
            s.until(3000, [&] {
                swam = swam || gs.world.lizard_get_core(l).swim > 0.5f;
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
                spat = spat || gs.world.lizard_get_core(l).anim == lizard::animation::spit;
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
                jumped = jumped || gs.world.lizard_get_core(l).anim == lizard::animation::jumping;
                return jumped;
            });
            r.check("cyan lizard jumps", jumped, s.describe() + (jumped ? " jumped=1" : " jumped=0"));
            lizard::destroy(gs, l);
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
        uint32_t music_requests = 0;
        uint32_t deaths = 0;
        uint32_t eaten = 0;
        uint32_t lost = 0;
        rw::rng bug_rng{0x51ED27u};
        uint32_t bugs_in_ceiling = 0, bugs_dropped = 0, bugs_lost = 0;
        bool bug_dropped = false;
        rw::rng cicada_rng{0xC1CADAu};
        uint32_t cicadas_sat = 0, cicadas_charged = 0, cicadas_lost = 0, cicadas_held = 0, cicadas_carried = 0;
        bool cicada_sat = false, cicada_charged = false, cicada_held = false;
        rw::rng lizard_rng{0x112A2Du};
        uint32_t lizard_breed = 0, lizards = 0, lizards_hunted = 0, lizards_bit = 0, lizards_tongued = 0, lizards_jumped = 0, lizards_spat = 0, lizards_swam = 0, lizards_lost = 0;
        bool lizard_hunted = false, lizard_bit = false, lizard_tongued = false, lizard_jumped = false, lizard_spat = false, lizard_swam = false;
        w.for_each_room([&](dcon::room_id rid) {
            auto const room = world::get_room_view(gs, rid);
            vec2 near{0.f, 0.f};
            w.room_for_each_camera_location(rid, [&](dcon::camera_location_id cl) {
                if (near == vec2{0.f, 0.f})
                    near = w.room_camera_get_position(w.camera_location_get_camera(cl)) + vec2{gs.screen_width / 2.f, gs.screen_height / 2.f};
            });
            player::reset(gs, p, rid, find_spawn(room, near));
            // a centipede in every room, somewhere it can stand
            dcon::centipede_id centi;
            for (int k = 0; k < 50 && !centi; ++k) {
                ivec2 const t{rng.range(1, std::max(2, room.width - 1)), rng.range(1, std::max(2, room.height - 1))};
                if (pathing::accessible(room, t, pathing::climber))
                    centi = centipede::create(gs, rid, world::room_view::middle_of_tile(t), rng.value());
            }
            // and a dropwig, placed in the room's best ceiling spot like a story session does
            dcon::dropbug_id bug;
            for (int k = 0; k < 50 && !bug; ++k) {
                ivec2 const t{bug_rng.range(1, std::max(2, room.width - 1)), bug_rng.range(1, std::max(2, room.height - 1))};
                if (pathing::accessible(room, t, pathing::drop_bug))
                    bug = dropbug::create(gs, rid, world::room_view::middle_of_tile(t), true);
            }
            if (bug && dropbug::sitting_in_ceiling(gs, bug))
                ++bugs_in_ceiling;
            // and a squidcada somewhere in the air, alternating the colours
            dcon::cicada_id cic;
            for (int k = 0; k < 50 && !cic; ++k) {
                ivec2 const t{cicada_rng.range(1, std::max(2, room.width - 1)), cicada_rng.range(1, std::max(2, room.height - 1))};
                if (pathing::accessible(room, t, pathing::cicada))
                    cic = cicada::create(gs, rid, world::room_view::middle_of_tile(t), cicada_rng.value() < 0.5f);
            }
            // and a lizard standing somewhere, the next breed in every room
            dcon::lizard_id liz;
            auto const breed = lizard::breed(lizard_breed++ % lizard::breed_count);
            for (int k = 0; k < 50 && !liz; ++k) {
                ivec2 const t{lizard_rng.range(1, std::max(2, room.width - 1)), lizard_rng.range(1, std::max(2, room.height - 1))};
                if (pathing::accessible(room, t, lizard::params(breed).movement) && !room.get_tile(t).solid())
                    liz = lizard::create(gs, rid, world::room_view::middle_of_tile(t), breed);
            }
            if (liz)
                ++lizards;
            int32_t ix = 0, iy = 0;
            bool jmp = false;
            for (uint32_t t = 0; t < ticks_per_room; ++t) {
                if (t % 12 == 0) {
                    ix = rng.range(-1, 2);
                    iy = rng.range(-1, 2);
                    jmp = rng.value() < 0.3f;
                }
                auto const in = player::make_input(float(ix), float(iy), jmp, rng.value() < 0.05f, rng.value() < 0.05f);
                simulation::tick(gs, std::span{&p, 1}, std::span{&in, 1});
                music_requests += uint32_t(gs.music.size());
                gs.music.clear();
                gs.sounds.clear();
                if (simulation::player_room(gs, p) != rid)
                    break; // went through a pipe
                bool finite = true;
                for (uint32_t i = 0; i < 2; ++i) {
                    vec2 const q = w.body_chunk_get_pos(player::body_chunk(gs, p, i));
                    finite = finite && std::isfinite(q.x) && std::isfinite(q.y);
                }
                if (centi)
                    for (uint32_t i = 0; i < centipede::chunk_count(gs, centi); ++i) {
                        vec2 const q = w.body_chunk_get_pos(centipede::chunk(gs, centi, i));
                        finite = finite && std::isfinite(q.x) && std::isfinite(q.y);
                    }
                if (bug) {
                    for (uint32_t i = 0; i < 3; ++i) {
                        vec2 const q = w.body_chunk_get_pos(dropbug::chunk(gs, bug, i));
                        finite = finite && std::isfinite(q.x) && std::isfinite(q.y);
                    }
                    bug_dropped = bug_dropped || w.dropbug_get_from_ceiling_jump(bug);
                }
                if (cic) {
                    for (uint32_t i = 0; i < 2; ++i) {
                        vec2 const q = w.body_chunk_get_pos(cicada::chunk(gs, cic, i));
                        finite = finite && std::isfinite(q.x) && std::isfinite(q.y);
                    }
                    cicada_sat = cicada_sat || !w.cicada_get_flying(cic);
                    cicada_charged = cicada_charged || cicada::charging(gs, cic);
                    cicada_held = cicada_held || cicada::held_by(gs, p) == cic;
                }
                if (liz) {
                    for (uint32_t i = 0; i < 3; ++i) {
                        vec2 const q = w.body_chunk_get_pos(lizard::chunk(gs, liz, i));
                        finite = finite && std::isfinite(q.x) && std::isfinite(q.y);
                    }
                    lizard_hunted = lizard_hunted || w.lizard_get_ai(liz).current == lizard::behavior::hunt;
                    lizard_bit = lizard_bit || lizard::grasped(gs, liz) == p;
                    lizard_tongued = lizard_tongued || w.lizard_get_tongue(liz).state == lizard::tongue_state::lashing_out;
                    lizard_jumped = lizard_jumped || w.lizard_get_core(liz).anim == lizard::animation::jumping;
                    lizard_spat = lizard_spat || w.lizard_get_core(liz).anim == lizard::animation::spit;
                    lizard_swam = lizard_swam || w.lizard_get_core(liz).swim > 0.5f;
                }
                if (!finite) {
                    std::printf("room %s: non finite position at tick %u\n", std::string{gs.to_string_view(w.room_get_name(rid))}.c_str(), t);
                    ++bad;
                    break;
                }
            }
            if (centi) {
                vec2 const hp = w.body_chunk_get_pos(centipede::chunk(gs, centi, 0));
                if (hp.y < -20.f || hp.x < -20.f || hp.x > room.pixel_width() + 20.f) {
                    ++lost;
                    if (lost <= 5)
                        std::printf("  centipede left %s at (%.0f, %.0f)\n", std::string{gs.to_string_view(w.room_get_name(rid))}.c_str(), hp.x, hp.y);
                }
                centipede::destroy(gs, centi);
            }
            if (bug) {
                vec2 const hp = w.body_chunk_get_pos(dropbug::chunk(gs, bug, 0));
                if (hp.y < -20.f || hp.x < -20.f || hp.x > room.pixel_width() + 20.f) {
                    ++bugs_lost;
                    if (bugs_lost <= 5)
                        std::printf("  dropwig left %s at (%.0f, %.0f)\n", std::string{gs.to_string_view(w.room_get_name(rid))}.c_str(), hp.x, hp.y);
                }
                bugs_dropped += bug_dropped ? 1 : 0;
                bug_dropped = false;
                dropbug::destroy(gs, bug);
            }
            if (cic) {
                vec2 const hp = w.body_chunk_get_pos(cicada::chunk(gs, cic, 0));
                if (w.physical_object_get_room_from_object_location(cicada::body(gs, cic)) != rid)
                    ++cicadas_carried; // through a pipe, in the slugcat's hands
                else if (hp.y < -20.f || hp.x < -20.f || hp.x > room.pixel_width() + 20.f) {
                    ++cicadas_lost;
                    if (cicadas_lost <= 5)
                        std::printf("  squidcada left %s at (%.0f, %.0f)\n", std::string{gs.to_string_view(w.room_get_name(rid))}.c_str(), hp.x, hp.y);
                }
                cicadas_sat += cicada_sat ? 1 : 0;
                cicadas_charged += cicada_charged ? 1 : 0;
                cicadas_held += cicada_held ? 1 : 0;
                cicada_sat = cicada_charged = cicada_held = false;
                cicada::destroy(gs, cic);
            }
            if (liz) {
                vec2 const hp = w.body_chunk_get_pos(lizard::chunk(gs, liz, 0));
                if (hp.y < -20.f || hp.x < -20.f || hp.x > room.pixel_width() + 20.f) {
                    ++lizards_lost;
                    if (lizards_lost <= 5)
                        std::printf("  %s lizard left %s at (%.0f, %.0f)\n", lizard::to_string(breed), std::string{gs.to_string_view(w.room_get_name(rid))}.c_str(), hp.x, hp.y);
                }
                lizards_hunted += lizard_hunted ? 1 : 0;
                lizards_bit += lizard_bit ? 1 : 0;
                lizards_tongued += lizard_tongued ? 1 : 0;
                lizards_jumped += lizard_jumped ? 1 : 0;
                lizards_spat += lizard_spat ? 1 : 0;
                lizards_swam += lizard_swam ? 1 : 0;
                lizard_hunted = lizard_bit = lizard_tongued = lizard_jumped = lizard_spat = lizard_swam = false;
                lizard::destroy(gs, liz);
            }
            if (w.player_get_eaten(p))
                ++eaten;
            else if (w.player_get_dead(p))
                ++deaths;
        });
        uint32_t rocks = 0;
        w.for_each_item([&](dcon::item_id) { ++rocks; });
        std::printf("fuzzed %u rooms x %u ticks, %u failures, %u rocks/spears spawned, %u music requests from %u triggers\n",
            w.room_size(), ticks_per_room, bad, rocks, music_requests, w.music_trigger_size());
        std::printf("centipedes: one per room, %u fell out of their room, %u slugcats killed, %u path fields built\n", lost, deaths, pathing::builds());
        std::printf("dropwigs: one per room, %u placed in the ceiling, %u dropped from it, %u fell out of their room\n", bugs_in_ceiling, bugs_dropped, bugs_lost);
        std::printf("squidcadas: one per room, %u sat down, %u bump attacked, %u picked up (%u carried through a pipe), %u fell out of their room\n",
            cicadas_sat, cicadas_charged, cicadas_held, cicadas_carried, cicadas_lost);
        std::printf("lizards: %u (one per room, every breed in turn), %u hunted, %u bit the slugcat, %u shot a tongue, %u jumped, %u spat, %u swam, %u fell out of their room\n",
            lizards, lizards_hunted, lizards_bit, lizards_tongued, lizards_jumped, lizards_spat, lizards_swam, lizards_lost);
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
        // expect to come out in the connected room
        uint32_t pipes = 0, arrived = 0;
        std::vector<std::string> failed_pipes;
        w.for_each_shortcut([&](dcon::shortcut_id sc) {
            if (w.shortcut_get_type(sc) != world::shortcut_type::room_exit)
                return;
            auto const exit = w.shortcut_get_exit_from_shortcut_destination(sc);
            dcon::room_id const target = exit ? dcon::room_id{w.room_exit_get_target_from_exit_connection(exit)} : dcon::room_id{};
            if (!target)
                return;
            auto const rid = w.shortcut_get_room_from_shortcut_location(sc);
            auto const room = world::get_room_view(gs, rid);
            ivec2 const start = w.shortcut_get_start(sc);
            ivec2 const dir = room.shortcut_entrance_hole_direction(start);
            ++pipes;
            player::reset(gs, p, rid, world::room_view::middle_of_tile(start) - vec2{0.f, 8.f});
            auto const in = player::make_input(float(-dir.x), float(-dir.y), false, false, false);
            uint32_t const before = arrived;
            for (uint32_t t = 0; t < 2400; ++t) {
                simulation::tick(gs, std::span{&p, 1}, std::span{&in, 1});
                if (simulation::player_room(gs, p) == target && !w.player_get_in_shortcut(p)) {
                    ++arrived;
                    break;
                }
            }
            if (arrived == before && failed_pipes.size() < 10)
                failed_pipes.push_back(std::string{gs.to_string_view(w.room_get_name(rid))} + " (" + std::to_string(start.x) + "," + std::to_string(start.y) + ")");
        });
        std::printf("pipes: %u of %u connected room exits delivered the slugcat\n", arrived, pipes);
        for (auto const& f : failed_pipes)
            std::printf("  pipe stuck: %s\n", f.c_str());
        player::destroy(gs, p);
        return bad == 0 ? 0 : 1;
    }
}
