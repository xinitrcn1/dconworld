#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "worm_grass.hpp"
#include "creature.hpp"
#include "dens.hpp"
#include "items.hpp"
#include "pathing.hpp"
#include "player.hpp"
#include "rooms.hpp"

namespace worm_grass {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        constexpr float pi = mathf::pi;

        // CreatureTemplate.wormGrassImmune, of the templates ported
        bool worm_grass_immune(creature::tmpl t) noexcept {
            using creature::tmpl;
            return t == tmpl::green_lizard || t == tmpl::spit_lizard || t == tmpl::tentacle_plant || t == tmpl::pole_mimic;
        }
        // AbstractCreature.tentacleImmune (the world file's TentacleImmune tag)
        bool tentacle_immune(sys::game_state const& state, dcon::creature_id c) noexcept {
            return (state.world.creature_get_spawn_flags(c) & dens::flag_tentacle_immune) != 0;
        }

        // Creature.RandomChunk
        dcon::body_chunk_id random_chunk(sys::game_state const& state, dcon::creature_id c, rw::rng& r) {
            uint32_t const n = creature::chunk_count(state, c);
            return creature::chunk(state, c, uint32_t(r.range(0, int32_t(n))));
        }

        dcon::worm_grass_patch_id patch_of(sys::game_state const& state, dcon::worm_grass_worm_id wm) {
            return state.world.worm_grass_worm_get_patch_from_worm_grass_worm_membership(wm);
        }

        // the patch's entry for `c` (nullptr: not tracking it)
        tracked* find_tracked(tracked_set& ts, dcon::creature_id c) {
            for (uint32_t i = 0; i < ts.count; ++i)
                if (ts.t[i].creature == c)
                    return &ts.t[i];
            return nullptr;
        }
        void remove_tracked(tracked_set& ts, uint32_t i) {
            for (uint32_t j = i; j + 1 < ts.count; ++j)
                ts.t[j] = ts.t[j + 1];
            --ts.count;
        }

        // Creature.CollideWithTerrain
        void collide_with_terrain(sys::game_state& state, dcon::creature_id c, bool v) {
            uint32_t const n = creature::chunk_count(state, c);
            for (uint32_t i = 0; i < n; ++i)
                state.world.body_chunk_set_collide_with_terrain(creature::chunk(state, c, i), v ? 1 : 0);
        }

        //
        // WormGrass.Worm
        //

        // Worm.Reset
        void worm_reset(sys::game_state& state, dcon::worm_grass_worm_id wm, vec2 base, float i_fac, float reach_height, float length_fac) {
            auto& w = state.world;
            w.worm_grass_worm_set_reach_height(wm, reach_height);
            w.worm_grass_worm_set_base_pos(wm, base);
            w.worm_grass_worm_set_i_fac(wm, i_fac);
            w.worm_grass_worm_set_length_fac(wm, length_fac);
            vec2 const below = base + vec2{0.f, -10.f};
            w.worm_grass_worm_set_below_ground_pos(wm, below);
            w.worm_grass_worm_set_under_ground_stuck_pos(wm, std::nullopt);
            w.worm_grass_worm_set_attached_creature(wm, dcon::creature_id{});
            float const length = (reach_height - base.y) * length_fac;
            w.worm_grass_worm_set_length(wm, length);
            vec2 const pos = base + vec2{0.f, length};
            w.worm_grass_worm_set_pos(wm, pos);
            w.worm_grass_worm_set_last_pos(wm, pos);
            auto& sg = w.worm_grass_worm_get_segments(wm);
            if (sg.count == 0) {
                sg.count = uint8_t(custom::int_clamp(int32_t(mathf::lerp(length / 10.f, 5.f, 0.5f)), 2, 4));
                for (uint32_t i = 0; i < sg.count; ++i) {
                    float num = float(i) / float(sg.count - 1);
                    num = mathf::lerp(num, 0.5f, 0.2f);
                    float const rad = mathf::lerp(1.5f, 5.f, std::sin(num * pi)) * mathf::lerp(1.f, 1.3f, i_fac);
                    sg.s[i] = segment{base, base, vec2{0.f, 0.f}, rad, rad};
                }
            }
            for (uint32_t j = 0; j < sg.count; ++j) {
                sg.s[j].pos = rw::lerp(pos, below, float(j) / float(sg.count - 1));
                sg.s[j].last_pos = sg.s[j].pos;
                sg.s[j].vel = vec2{0.f, 0.f};
            }
        }

        dcon::worm_grass_worm_id create_worm(sys::game_state& state, dcon::worm_grass_patch_id p, vec2 base, float reach_height, float i_fac, float length_fac,
            bool cosmetic, rw::rng& r) {
            auto& w = state.world;
            auto const wm = w.create_worm_grass_worm();
            w.try_create_worm_grass_worm_membership(wm, p);
            w.worm_grass_worm_set_cosmetic(wm, cosmetic);
            w.worm_grass_worm_set_color(wm, uint8_t(r.range(0, 11)));
            w.worm_grass_worm_set_vel(wm, vec2{0.f, 0.f});
            w.worm_grass_worm_set_excitement(wm, 0.f);
            w.worm_grass_worm_set_drag_force(wm, 0.f);
            w.worm_grass_worm_set_focus_creature(wm, dcon::creature_id{});
            w.worm_grass_worm_set_culled(wm, true);
            w.worm_grass_worm_set_segments(wm, segments{});
            worm_reset(state, wm, base, i_fac, reach_height, length_fac);
            return wm;
        }

        void attach(sys::game_state& state, dcon::worm_grass_worm_id wm, dcon::creature_id c, uint32_t k) {
            auto& w = state.world;
            auto const ch = creature::chunk(state, c, k);
            vec2 const pos = w.worm_grass_worm_get_pos(wm);
            w.worm_grass_worm_set_attached_creature(wm, c);
            w.worm_grass_worm_set_attached_chunk(wm, uint8_t(k));
            w.worm_grass_worm_set_focus_creature(wm, dcon::creature_id{});
            // BodyChunk.Rotation is (0, 1) here (no rotation chunks): the
            // direction is kept as it is
            w.worm_grass_worm_set_attached_dir(wm, custom::dir_vec(w.body_chunk_get_pos(ch), pos));
            w.worm_grass_worm_set_last_vis_pos(wm, pos);
            w.worm_grass_worm_set_drag_force(wm, 0.f);
        }

        // Worm.Attached
        void attached(sys::game_state& state, room_view const& room, dcon::worm_grass_worm_id wm) {
            auto& w = state.world;
            auto const c = w.worm_grass_worm_get_attached_creature(wm);
            auto const detach = [&] { w.worm_grass_worm_set_attached_creature(wm, dcon::creature_id{}); };
            // (owner.room != room)
            if (!creature::here(state, c, room.id) || w.worm_grass_worm_get_attached_chunk(wm) >= creature::chunk_count(state, c)) {
                detach();
                w.worm_grass_worm_set_excitement(wm, 0.f);
                return;
            }
            auto const ch = creature::chunk(state, c, w.worm_grass_worm_get_attached_chunk(wm));
            float const crad = w.body_chunk_get_rad(ch);
            vec2 const value = w.body_chunk_get_pos(ch) + w.worm_grass_worm_get_attached_dir(wm) * crad * 0.7f;
            w.worm_grass_worm_set_pos(wm, value);
            w.worm_grass_worm_set_vel(wm, w.body_chunk_get_vel(ch));
            w.worm_grass_worm_set_excitement(wm, 1.f);
            auto& ts = w.worm_grass_patch_get_tracked(patch_of(state, wm));
            tracked* const t = find_tracked(ts, c);
            if (t == nullptr) {
                detach();
                w.worm_grass_worm_set_under_ground_stuck_pos(wm, value);
                return;
            }
            bool const cosmetic = w.worm_grass_worm_get_cosmetic(wm);
            if (cosmetic && t->pull == 0.f) {
                detach();
                return;
            }
            vec2 const base = w.worm_grass_worm_get_base_pos(wm);
            float const length = w.worm_grass_worm_get_length(wm);
            vec2 const vector = base + vec2{0.f, (crad + 20.f) * -t->bury};
            if (!custom::dist_less(vector, value, length * 4.f)) {
                detach();
                return;
            }
            if (cosmetic)
                return;
            float const mass = w.body_chunk_get_mass(ch);
            float const total = creature::total_mass(state, c);
            float drag = w.worm_grass_worm_get_drag_force(wm);
            vec2& cvel = w.body_chunk_get_vel(ch);
            vec2 const cpos = w.body_chunk_get_pos(ch);
            cvel *= mathf::lerp(1.f, 0.f, mathf::pow(mathf::inverse_lerp(0.3f, 0.8f, drag), 2.f + mass * mathf::max(1.f, total) * 15.f) / (total * 1.43f));
            cvel += custom::dir_vec(cpos, vector)
                * custom::lerp_map(rw::distance(cpos, vector), length / 2.f, length * 4.f, mathf::lerp(0.0001f, 0.001f, mathf::pow(drag, 4.f)), mathf::lerp(0.05f, 0.5f, drag))
                / mass;
            drag += mathf::lerp(1.f / mathf::lerp(4000.f, 100.f, drag), 1.f / (mathf::max(rw::distance(w.body_chunk_get_last_pos(ch), cpos), 1.f) * 250.f), 1.f - drag)
                / (total * 1.43f);
            if (!creature::consious(state, c))
                drag += 1.f / (57.f * (total * mathf::max(1.f, total)));
            if (drag > 1.f || t->bury > 0.f) {
                drag = 1.f;
                w.worm_grass_worm_set_under_ground_stuck_pos(wm, value);
                if (!creature::held(state, c))
                    t->bury = mathf::min(1.f, t->bury + 1.f / 60.f);
            } else if (room.visual_contact(base, value)) {
                w.worm_grass_worm_set_last_vis_pos(wm, value);
            } else {
                w.worm_grass_worm_set_pos(wm, w.worm_grass_worm_get_last_vis_pos(wm));
                detach();
            }
            w.worm_grass_worm_set_drag_force(wm, drag);
            // (Player.GraspWiggle: not ported, a slugcat can't wiggle free)
        }

        // Worm.Act
        void act(sys::game_state& state, room_view const& room, dcon::worm_grass_worm_id wm, rw::rng& r) {
            auto& w = state.world;
            vec2 const pos = w.worm_grass_worm_get_pos(wm);
            vec2& vel = w.worm_grass_worm_get_vel(wm);
            float exc = w.worm_grass_worm_get_excitement(wm);
            float const i_fac = w.worm_grass_worm_get_i_fac(wm);
            float num = std::numeric_limits<float>::max();
            float a = 0.f;
            auto focus = w.worm_grass_worm_get_focus_creature(wm);
            if (focus && !creature::here(state, focus, room.id))
                focus = dcon::creature_id{};
            if (focus) {
                float const m = creature::total_mass(state, focus);
                float const mk = mathf::lerp(m, 1.f, 0.5f);
                num = rw::distance(pos, w.body_chunk_get_pos(random_chunk(state, focus, r)));
                a = m;
                auto const rc = random_chunk(state, focus, r);
                vel += (custom::dir_vec(pos, w.body_chunk_get_pos(rc) + custom::rnv(r) * w.body_chunk_get_rad(rc)) + custom::rnv(r) * 0.5f) * r.value()
                    * custom::lerp_map(num, 60.f * mk, 400.f * mk, 3.5f, 0.f) * exc;
                exc = mathf::min(exc + m / mathf::lerp(60.f, 10.f, i_fac), 1.f);
                vec2 const main = w.body_chunk_get_pos(creature::chunk(state, focus, creature::main_chunk(state, focus)));
                if (num > 400.f * mk || main.y < w.worm_grass_worm_get_base_pos(wm).y - 50.f
                    || !room.visual_contact(pos, w.body_chunk_get_pos(random_chunk(state, focus, r))))
                    focus = dcon::creature_id{};
            } else if (exc > 0.f) {
                vel += custom::rnv(r) * 2.f * r.value() * exc;
                exc = mathf::max(exc - 1.f / mathf::lerp(40.f, 300.f, r.value()), 0.f);
            }
            w.worm_grass_worm_set_excitement(wm, exc);
            w.worm_grass_worm_set_focus_creature(wm, focus);
            // grabbing whatever it touches
            if (exc > 0.5f) {
                bool done = false;
                rooms::for_each_creature(state, room.id, [&](dcon::creature_id c) {
                    if (done || !creature::here(state, c, room.id) || tentacle_immune(state, c) || !(mathf::pow(r.value(), 0.7f) * 2.f < exc))
                        return;
                    uint32_t const n = creature::chunk_count(state, c);
                    for (uint32_t i = 0; i < n; ++i) {
                        auto const ch = creature::chunk(state, c, i);
                        if (custom::dist_less(pos, w.body_chunk_get_pos(ch), w.body_chunk_get_rad(ch))) {
                            attach(state, wm, c, i);
                            done = true;
                            break;
                        }
                    }
                });
                if (done)
                    return;
            }
            // noticing one of the patch's creatures
            auto& ts = w.worm_grass_patch_get_tracked(patch_of(state, wm));
            if (ts.count > 0) {
                auto const c = ts.t[r.range(0, int32_t(ts.count))].creature;
                if (c != focus && creature::here(state, c, room.id)) {
                    float const m = creature::total_mass(state, c);
                    float const num4 = rw::distance(pos, w.body_chunk_get_pos(random_chunk(state, c, r)));
                    if (num4 < mathf::pow(r.value(), 15.f) * 400.f * mathf::lerp(m, 1.f, 0.5f) && num4 * mathf::lerp(a, 2.f, 0.85f) < num * mathf::lerp(m, 2.f, 0.85f)
                        && room.visual_contact(pos, w.body_chunk_get_pos(random_chunk(state, c, r))))
                        w.worm_grass_worm_set_focus_creature(wm, c);
                }
            }
            // (WormGrass.repulsiveObjects: only the Gooieduck adds them, not ported)
        }

        // Worm.Update; `culled`: no camera looks at the room
        void worm_update(sys::game_state& state, room_view const& room, dcon::worm_grass_worm_id wm, bool culled, rw::rng& r) {
            auto& w = state.world;
            bool const last_culled = w.worm_grass_worm_get_culled(wm);
            w.worm_grass_worm_set_culled(wm, culled);
            {
                vec2& pos = w.worm_grass_worm_get_pos(wm);
                vec2& vel = w.worm_grass_worm_get_vel(wm);
                w.worm_grass_worm_set_last_pos(wm, pos);
                pos += vel;
                vel *= 0.9f;
                vel.y += 0.6f;
                if (room.get_tile(pos).solid())
                    vel *= 0.5f;
            }
            if (w.worm_grass_worm_get_attached_creature(wm)) {
                attached(state, room, wm);
            } else if (auto const stuck = w.worm_grass_worm_get_under_ground_stuck_pos(wm); stuck) {
                w.worm_grass_worm_set_vel(wm, vec2{0.f, 0.f});
                w.worm_grass_worm_set_pos(wm, *stuck);
                if (r.value() < 0.025f)
                    w.worm_grass_worm_set_under_ground_stuck_pos(wm, std::nullopt);
            } else {
                act(state, room, wm, r);
            }
            vec2 const base = w.worm_grass_worm_get_base_pos(wm);
            float const length = w.worm_grass_worm_get_length(wm);
            vec2& pos = w.worm_grass_worm_get_pos(wm);
            vec2& vel = w.worm_grass_worm_get_vel(wm);
            if (!w.worm_grass_worm_get_attached_creature(wm) && !custom::dist_less(pos, base, length)) {
                vec2 const a = custom::dir_vec(pos, base);
                float const num = rw::distance(pos, base);
                pos += a * (num - length) * 0.1f;
                vel += a * (num - length) * 0.1f;
            }
            auto& sg = w.worm_grass_worm_get_segments(wm);
            uint32_t const n = sg.count;
            if (!culled) {
                if (last_culled) {
                    worm_reset(state, wm, base, w.worm_grass_worm_get_i_fac(wm), w.worm_grass_worm_get_reach_height(wm), w.worm_grass_worm_get_length_fac(wm));
                } else {
                    float const num2 = length / float(n);
                    float const exc = w.worm_grass_worm_get_excitement(wm);
                    for (uint32_t i = 0; i < n; ++i) {
                        float const num3 = float(i) / float(n - 1);
                        segment& s = sg.s[i];
                        s.last_pos = s.pos;
                        s.pos += s.vel;
                        s.vel *= 0.9f;
                        s.vel += custom::rnv(r) * exc;
                        s.vel += (base + vec2{0.f, length * (1.f - num3)} - s.pos) * mathf::inverse_lerp(0.f, length * 20.f, rw::distance(base, s.pos));
                        if (i > 0) {
                            segment& p = sg.s[i - 1];
                            vec2 const a2 = custom::dir_vec(s.pos, p.pos);
                            float const num4 = rw::distance(s.pos, p.pos);
                            s.pos += a2 * (num4 - num2) * 0.25f;
                            s.vel += a2 * (num4 - num2) * 0.25f;
                            p.pos -= a2 * (num4 - num2) * 0.25f;
                            p.vel -= a2 * (num4 - num2) * 0.25f;
                            float num5 = num2 / num4;
                            // (vanilla's precedence: 1 - Sin(i / n) * pi)
                            num5 = mathf::lerp(num5, 1.f, 1.f - std::sin(float(i) / float(n)) * pi);
                            p.stretched_rad = p.rad
                                * (num4 < num2 ? mathf::min(mathf::lerp(num5, 1.f, 0.4f), 2.f) : mathf::pow(num5, custom::lerp_map(length, 35.f, 50.f, 0.5f, 0.85f)));
                        }
                    }
                }
            }
            sg.s[0].pos = w.worm_grass_worm_get_pos(wm);
            sg.s[0].vel = w.worm_grass_worm_get_vel(wm);
            sg.s[n - 1].pos = w.worm_grass_worm_get_below_ground_pos(wm);
            sg.s[n - 1].vel = vec2{0.f, 0.f};
        }

        //
        // WormGrass.WormGrassPatch
        //

        // WormGrassPatch.LoseGrip
        void lose_grip(tracked& t) {
            if (t.bury > 0.f)
                return;
            t.bury = mathf::max(0.f, t.bury - 0.1f);
            t.pull = mathf::max(0.f, t.pull - 0.1f);
        }

        // Destroy: an eaten slugcat stays, dead and gone; any other creature
        // is gone at the end of the tick
        void consume(sys::game_state& state, dcon::room_id room, dcon::creature_id c) {
            auto& w = state.world;
            creature::release_target(state, c, room);
            if (auto const pl = creature::player_of(state, c)) {
                // (the worms holding it let go next tick: it's no longer here;
                // the caller drops this patch's entry)
                items::release_all(state, pl);
                creature::die(state, c);
                w.player_set_eaten(pl, 1);
                return;
            }
            if (!w.creature_get_dead(c))
                w.creature_set_killed_by(c, creature::killed_by_other);
            creature::die(state, c);
            state.defer(room, [c](sys::game_state& st) {
                if (st.world.creature_is_valid(c))
                    creature::destroy(st, c);
            });
        }

        // CreatureAndPull.Update (Remix's): a dead creature is gone 20 ticks
        // after the worms got it down, unless a slugcat holds it. True once
        // it is eaten
        bool pull_update(sys::game_state& state, dcon::room_id room, tracked& t) {
            auto& w = state.world;
            auto const c = t.creature;
            bool const dead = w.creature_get_dead(c) != 0;
            uint32_t grasp = 0;
            auto const holder = creature::held(state, c) ? creature::holder(state, c, &grasp) : dcon::creature_id{};
            if (!dead || (holder && creature::is_slugcat(w, holder))) {
                if (dead) {
                    t.pull = 0.f;
                    t.bury = 0.f;
                    if (t.consume_timer > 0)
                        collide_with_terrain(state, c, true);
                }
                t.consume_timer = 0;
                return false;
            }
            if (t.consume_timer > 0) {
                ++t.consume_timer;
                collide_with_terrain(state, c, false);
                auto const main = creature::chunk(state, c, creature::main_chunk(state, c));
                w.body_chunk_set_vel(main, vec2{0.f, 0.f});
                if (t.consume_timer >= 20 || w.body_chunk_get_pos(main).y < t.consumed_pos.y - 40.f) {
                    consume(state, room, c);
                    return true;
                }
            }
            return false;
        }

        // WormGrassPatch.InteractWithCreature; true once the creature is eaten
        bool interact(sys::game_state& state, room_view const& room, dcon::worm_grass_patch_id p, tracked& t, rw::rng& r) {
            auto& w = state.world;
            auto const c = t.creature;
            if (pull_update(state, room.id, t))
                return true;
            auto const tiles = w.worm_grass_patch_get_tiles(p);
            auto const sizes = w.worm_grass_patch_get_sizes(p);
            int32_t const count = int32_t(tiles.size());
            ivec2 const left = tiles[0];
            ivec2 const right = tiles[uint32_t(count - 1)];
            uint32_t const n = creature::chunk_count(state, c);
            if (t.bury == 0.f) {
                vec2 const main = w.body_chunk_get_pos(creature::chunk(state, c, creature::main_chunk(state, c)));
                if (main.x < float(left.x) * 20.f - 200.f || main.x > float(right.x) * 20.f + 200.f || main.y > float(w.worm_grass_patch_get_highest_tile(p)) * 20.f + 300.f) {
                    lose_grip(t);
                    return false;
                }
                bool flag = false;
                int32_t const num = room_view::get_tile_position(main).x - left.x;
                if (num > 0 && num < count - 1)
                    for (uint32_t i = 0; i < n && !flag; ++i)
                        flag = room.visual_contact(room_view::middle_of_tile(tiles[uint32_t(num)]),
                            room_view::middle_of_tile(w.body_chunk_get_pos(creature::chunk(state, c, i))));
                for (int32_t i = 0; i < count && !flag; ++i)
                    flag = room.visual_contact(room_view::middle_of_tile(tiles[uint32_t(i)]), room_view::middle_of_tile(w.body_chunk_get_pos(random_chunk(state, c, r))));
                if (!flag) {
                    lose_grip(t);
                    return false;
                }
            }
            bool flag2 = false;
            if (t.bury > 0.f) {
                flag2 = true;
                if (!w.creature_get_dead(c)) {
                    w.creature_set_killed_by(c, creature::killed_by_other);
                    creature::die(state, c);
                }
                // CreatureAndPull.Consume
                if (t.consume_timer == 0) {
                    t.consume_timer = 1;
                    t.consumed_pos = w.body_chunk_get_pos(creature::chunk(state, c, creature::main_chunk(state, c)));
                }
                collide_with_terrain(state, c, false);
            }
            float const total = creature::total_mass(state, c);
            bool const conscious = creature::consious(state, c);
            bool const slugcat = creature::is_slugcat(w, c);
            bool const held = creature::held(state, c);
            bool flag3 = false;
            for (uint32_t i = 0; i < n; ++i) {
                auto const ch = creature::chunk(state, c, i);
                vec2& cpos = w.body_chunk_get_pos(ch);
                vec2& cvel = w.body_chunk_get_vel(ch);
                float const crad = w.body_chunk_get_rad(ch);
                ivec2 const ct = room_view::get_tile_position(cpos);
                float num4 = std::numeric_limits<float>::lowest();
                int32_t num5 = -1;
                for (int32_t k = 0; k < count && num5 == -1; ++k)
                    if (tiles[uint32_t(k)].x == ct.x)
                        num5 = k;
                float num7 = 0.f;
                if (num5 > -1) {
                    vec2 const sz = sizes[uint32_t(num5)];
                    num4 = room_view::middle_of_tile(tiles[uint32_t(num5)]).y - 10.f;
                    float const value = cpos.y - crad - num4;
                    num7 = mathf::pow(mathf::inverse_lerp(sz.x * 50.f, sz.x * 50.f * 0.5f, value), 6.f) * sz.y;
                    // a creature fights its way up if there's anywhere to go
                    if (conscious && !slugcat) {
                        for (int32_t j = ct.y; j <= room.height; ++j) {
                            if (pathing::accessible(room, ivec2{tiles[uint32_t(num5)].x, j}, pathing::standard_ground)) {
                                cvel.y = mathf::lerp(cvel.y, 14.f, mathf::pow(num7, 0.2f) * mathf::pow(1.f - t.pull, 0.8f));
                                break;
                            }
                        }
                    }
                }
                num7 = mathf::pow(num7, 1.f + total);
                if (num7 > 0.f) {
                    flag3 = true;
                    t.pull = mathf::min(1.f, t.pull + 1.f / mathf::lerp(800.f, 140.f, num7 / total));
                }
                if (held) {
                    flag3 = false;
                    t.pull = 0.f;
                }
                num7 *= t.pull;
                cvel.x *= mathf::lerp(1.f, 0.f, mathf::pow(num7, 6.f));
                cvel.y -= 10.f * num7;
                cvel += custom::rnv(r) * t.bury * 3.f * num7;
                if (w.creature_get_type(c) == creature::type::tube_worm)
                    num7 = 0.2f;
                if (t.pull >= 1.f)
                    t.bury = mathf::min(1.f, t.bury + 1.f / mathf::lerp(600.f, 40.f, num7 / total));
                if (flag2) {
                    if (t.bury < 1.f && room.get_tile(cpos + vec2{0.f, -20.f}).solid() && cpos.y < num4 + crad) {
                        cpos.y = num4 + crad;
                        flag2 = false;
                    }
                    if (t.bury < 1.f || cpos.y > num4 + crad - 1.f)
                        flag2 = false;
                }
            }
            if (!flag3) {
                lose_grip(t);
                return false;
            }
            if (flag2) {
                consume(state, room.id, c);
                return true;
            }
            return false;
        }

        // WormGrassPatch.Update
        void patch_update(sys::game_state& state, room_view const& room, dcon::worm_grass_patch_id p, rw::rng& r) {
            auto& w = state.world;
            auto& ts = w.worm_grass_patch_get_tracked(p);
            auto const tiles = w.worm_grass_patch_get_tiles(p);
            // a random creature of the room, noticed if it can be seen from
            // a random tile of the patch
            uint32_t creatures = 0;
            rooms::for_each_creature(state, room.id, [&](dcon::creature_id) { ++creatures; });
            if (creatures > 0) {
                uint32_t const pick = uint32_t(r.range(0, int32_t(creatures)));
                uint32_t i = 0;
                dcon::creature_id c;
                rooms::for_each_creature(state, room.id, [&](dcon::creature_id o) {
                    if (i++ == pick)
                        c = o;
                });
                if (c && creature::here(state, c, room.id) && tiles.size() > 0) {
                    uint32_t const n = creature::chunk_count(state, c);
                    if (n > 0
                        && room.visual_contact(room_view::middle_of_tile(tiles[uint32_t(r.range(0, int32_t(tiles.size())))]),
                            w.body_chunk_get_pos(creature::chunk(state, c, uint32_t(r.range(0, int32_t(n))))))
                        && !worm_grass_immune(creature::template_of(state, c)) && find_tracked(ts, c) == nullptr && ts.count < tracked_set::capacity) {
                        ts.t[ts.count] = tracked{c};
                        ++ts.count;
                    }
                }
            }
            for (int32_t i = int32_t(ts.count) - 1; i >= 0; --i) {
                // (destroyed, in a pipe, in another room)
                if (!creature::here(state, ts.t[i].creature, room.id) || creature::chunk_count(state, ts.t[i].creature) == 0) {
                    remove_tracked(ts, uint32_t(i));
                    continue;
                }
                if (interact(state, room, p, ts.t[i], r))
                    remove_tracked(ts, uint32_t(i));
            }
        }

        // WormGrassPatch.ShouldTileBeAdded
        bool should_tile_be_added(std::vector<ivec2> const& patch, ivec2 tile) {
            for (auto const t : patch)
                if ((tile.x == t.x - 1 || tile.x == t.x + 1) && std::abs(tile.y - t.y) < 2)
                    return true;
            return false;
        }

        // WormGrassPatch.SortTiles: the tiles' sizes and the worms
        void make_patch(sys::game_state& state, room_view const& room, std::vector<ivec2> tiles) {
            auto& w = state.world;
            std::stable_sort(tiles.begin(), tiles.end(), [](ivec2 a, ivec2 b) { return a.x < b.x; });
            auto const p = w.create_worm_grass_patch();
            w.try_create_worm_grass_location(p, room.id);
            // System.Random(LeftTile.x + LeftTile.y + room index)
            uint32_t seed = uint32_t(tiles[0].x + tiles[0].y + int32_t(room.id.index())) * 0x9E3779B1u ^ 0x2545F491u;
            rw::rng r{seed != 0 ? seed : 0x2545F491u};
            int32_t const n = int32_t(tiles.size());
            ivec2 const left = tiles[0];
            ivec2 const right = tiles[uint32_t(n - 1)];
            auto const position = [&](ivec2 t) { return mathf::inverse_lerp(float(left.x), float(right.x), float(t.x)); };
            bool const flag = room.get_tile(left + ivec2{-1, 0}).solid() && room.get_tile(left + ivec2{-1, 1}).solid();
            bool const flag2 = room.get_tile(right + ivec2{1, 0}).solid() && room.get_tile(right + ivec2{1, 1}).solid();
            std::vector<vec2> sizes(static_cast<size_t>(n));
            int32_t highest = std::numeric_limits<int32_t>::min();
            for (int32_t i = 0; i < n; ++i) {
                float const ps = position(tiles[uint32_t(i)]);
                float s0 = std::sin(ps * pi);
                if (flag)
                    s0 = mathf::lerp(s0, mathf::pow(1.f - ps, 2.f), 1.f - ps);
                if (flag2)
                    s0 = mathf::lerp(s0, mathf::pow(ps, 2.f), ps);
                float const s1 = mathf::min(mathf::inverse_lerp(3.f, 11.f, float(i)), mathf::inverse_lerp(float(n - 1 - 3), float(n - 1 - 11), float(i)));
                s0 = mathf::lerp(s0, 5.f, s1 * 0.5f);
                sizes[size_t(i)] = vec2{s0, s1};
                highest = std::max(highest, tiles[uint32_t(i)].y);
                // (the column of Tile.wormGrass over it: pathing, not ported)
            }
            {
                auto pt = w.worm_grass_patch_get_tiles(p);
                auto ps = w.worm_grass_patch_get_sizes(p);
                for (int32_t i = 0; i < n; ++i) {
                    pt.push_back(tiles[uint32_t(i)]);
                    ps.push_back(sizes[size_t(i)]);
                }
            }
            w.worm_grass_patch_set_highest_tile(p, highest);
            w.worm_grass_patch_set_tracked(p, tracked_set{});
            // the real worms first (in vanilla the cosmetic ones are added
            // later, around the camera)
            struct cosmetic_worm {
                vec2 base;
                float reach;
                float i_fac;
                float length_fac;
            };
            std::vector<cosmetic_worm> cosmetic;
            auto const reach_of = [&](int32_t j) { return room_view::middle_of_tile(tiles[uint32_t(j)]).y + sizes[size_t(j)].x * 30.f; };
            for (int32_t j = 0; j < n; ++j) {
                float const num2 = reach_of(j);
                float const a = j > 0 ? reach_of(j - 1) : num2;
                float const b = j < n - 1 ? reach_of(j + 1) : num2;
                int32_t const num3 = int32_t(mathf::lerp(3.f, 10.f, sizes[size_t(j)].x * mathf::lerp(1.f, 0.3f, sizes[size_t(j)].y)));
                for (int32_t k = 0; k < num3; ++k) {
                    float const num4 = (float(k) + 0.5f) / float(num3);
                    vec2 const v = room_view::middle_of_tile(tiles[uint32_t(j)]) + vec2{-10.f + 20.f * num4 + mathf::lerp(-4.f, 4.f, r.value()), -10.f};
                    float const num5 = mathf::lerp(num2, mathf::lerp(a, b, num4), 0.5f);
                    float const num6 = mathf::lerp(0.2f, 1.f, mathf::pow(r.value(), 0.8f));
                    if (r.value() < sizes[size_t(j)].y)
                        cosmetic.push_back(cosmetic_worm{v, num5, sizes[size_t(j)].y, num6});
                    else
                        create_worm(state, p, v, num5, sizes[size_t(j)].y, num6, false, r);
                }
            }
            for (auto const& c : cosmetic)
                create_worm(state, p, c.base, c.reach, c.i_fac, c.length_fac, true, r);
            w.worm_grass_patch_set_random(p, rw::rng{r.next() | 1u});
        }

        template<typename F>
        void for_each_patch(sys::game_state const& state, dcon::room_id room, F&& f) {
            state.world.room_for_each_worm_grass_location(room, [&](dcon::worm_grass_location_id l) { f(state.world.worm_grass_location_get_patch(l)); });
        }
        template<typename F>
        void for_each_worm(sys::game_state const& state, dcon::worm_grass_patch_id p, F&& f) {
            state.world.worm_grass_patch_for_each_worm_grass_worm_membership(p, [&](dcon::worm_grass_worm_membership_id m) { f(state.world.worm_grass_worm_membership_get_worm(m)); });
        }

        vec3 lerp3(vec3 a, vec3 b, float t) {
            t = mathf::clamp01(t);
            return a + (b - a) * t;
        }
    }

    void spawn_room(sys::game_state& state, dcon::room_id room) {
        auto& w = state.world;
        // (made once: a loaded game keeps its patches)
        auto const existing = w.room_range_of_worm_grass_location(room);
        if (existing.first != existing.second)
            return;
        room_view const view = world::get_room_view(state, room);
        // Room.LoadFromDataString: the tiles in the level file's order
        // (columns left to right, each top to bottom)
        std::vector<ivec2> tiles;
        for (int32_t x = 0; x < view.width; ++x)
            for (int32_t y = view.height - 1; y >= 0; --y)
                if (view.get_tile(x, y).flags & world::tile_worm_grass)
                    tiles.push_back(ivec2{x, y});
        if (tiles.empty())
            return;
        // the WormGrass constructor: a tile joins the first patch it touches
        std::vector<std::vector<ivec2>> patches;
        for (auto const t : tiles) {
            bool added = false;
            for (auto& p : patches) {
                if (should_tile_be_added(p, t)) {
                    p.push_back(t);
                    added = true;
                    break;
                }
            }
            if (!added)
                patches.push_back(std::vector<ivec2>{t});
        }
        for (auto& p : patches)
            make_patch(state, view, std::move(p));
    }

    void update_room(sys::game_state& state, room_view const& room) {
        auto& w = state.world;
        auto const range = w.room_range_of_worm_grass_location(room.id);
        if (range.first == range.second)
            return;
        bool const culled = !state.viewed(room.id);
        for_each_patch(state, room.id, [&](dcon::worm_grass_patch_id p) {
            auto& r = w.worm_grass_patch_get_random(p);
            // the worms (updated before the WormGrass in vanilla's list); the
            // cosmetic ones only where a camera looks
            for_each_worm(state, p, [&](dcon::worm_grass_worm_id wm) {
                if (w.worm_grass_worm_get_cosmetic(wm) && culled) {
                    w.worm_grass_worm_set_culled(wm, true);
                    return;
                }
                worm_update(state, room, wm, culled, r);
            });
            patch_update(state, room, p, r);
        });
    }

    void release(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        // the worm grass that has hold of it is its room's
        auto const room = creature::room_of(state, c);
        if (!room)
            return;
        for_each_patch(state, room, [&](dcon::worm_grass_patch_id p) {
            auto& ts = w.worm_grass_patch_get_tracked(p);
            for (int32_t i = int32_t(ts.count) - 1; i >= 0; --i)
                if (ts.t[i].creature == c)
                    remove_tracked(ts, uint32_t(i));
            for_each_worm(state, p, [&](dcon::worm_grass_worm_id wm) {
                if (w.worm_grass_worm_get_focus_creature(wm) == c)
                    w.worm_grass_worm_set_focus_creature(wm, dcon::creature_id{});
                if (w.worm_grass_worm_get_attached_creature(wm) == c)
                    w.worm_grass_worm_set_attached_creature(wm, dcon::creature_id{});
            });
        });
    }

    void explosion(sys::game_state& state, dcon::room_id room, vec2 at, float rad, float force) {
        auto& w = state.world;
        auto& r = state.room_random(room);
        for_each_patch(state, room, [&](dcon::worm_grass_patch_id p) {
            for_each_worm(state, p, [&](dcon::worm_grass_worm_id wm) {
                vec2 const pos = w.worm_grass_worm_get_pos(wm);
                if (!custom::dist_less(pos, at, rad * 2.f))
                    return;
                float const num = mathf::inverse_lerp(rad * 2.f, rad, rw::distance(pos, at));
                if (r.value() < num) {
                    w.worm_grass_worm_get_vel(wm) += custom::dir_vec(at, pos) * force * 2.f * num;
                    w.worm_grass_worm_set_excitement(wm, 0.f);
                    w.worm_grass_worm_set_focus_creature(wm, dcon::creature_id{});
                    w.worm_grass_worm_set_drag_force(wm, 0.f);
                    w.worm_grass_worm_set_attached_creature(wm, dcon::creature_id{});
                }
            });
        });
    }

    void build(sys::game_state const& state, dcon::room_id room, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip) {
        auto const& w = state.world;
        // RoomCamera.PixelColorAtCoordinate(belowGroundPos): the level image
        // isn't read, the palette's colour of the play layer's terrain
        // (depth 0) stands in
        vec3 const ground = pal.texel(0, 3);
        auto const draw = [&](dcon::worm_grass_worm_id wm) {
            // (culled: no update has moved it since a camera looked)
            if (w.worm_grass_worm_get_culled(wm))
                return;
            auto const& sg = w.worm_grass_worm_get_segments(wm);
            uint32_t const n = sg.count;
            if (n < 2)
                return;
            // ApplyPalette
            vec3 const tip = lerp3(pal.texel(int(w.worm_grass_worm_get_color(wm)), 3), vec3{1.f, 0.f, 0.f}, w.worm_grass_worm_get_i_fac(wm) * 0.5f);
            // DrawSprites: the long mesh (pointy tip: 4n - 1 vertices)
            vec2 verts[segments::capacity * 4];
            rw::vec4 cols[segments::capacity * 4];
            vec2 const below = w.worm_grass_worm_get_below_ground_pos(wm);
            vec2 vector = rw::lerp(w.worm_grass_worm_get_last_pos(wm), w.worm_grass_worm_get_pos(wm), alpha);
            vec2 const star = vector;
            float num = 0.2f;
            vector += custom::dir_vec(sg.s[1].pos, sg.s[0].pos) * 1.5f;
            for (uint32_t i = 0; i < n; ++i) {
                segment const& s = sg.s[i];
                vec2 const v2 = rw::lerp(s.last_pos, s.pos, alpha);
                vec2 const nrm = rw::normalized(v2 - vector);
                vec2 const a = custom::perpendicular_vector(nrm);
                float const d = rw::distance(v2, vector) / 5.f;
                verts[i * 4] = vector - a * (num + s.stretched_rad) * 0.5f + nrm * d;
                verts[i * 4 + 1] = vector + a * (num + s.stretched_rad) * 0.5f + nrm * d;
                float const f0 = float(i) / float(n - 1);
                float const f1 = (float(i) + 0.5f) / float(n - 1);
                vec3 const c0 = lerp3(tip, ground, f0);
                vec3 const c1 = lerp3(tip, ground, f1);
                cols[i * 4] = rw::vec4{c0, 1.f};
                cols[i * 4 + 1] = rw::vec4{c0, 1.f};
                cols[i * 4 + 2] = rw::vec4{c1, 1.f};
                if (i < n - 1) {
                    verts[i * 4 + 2] = v2 - a * s.stretched_rad - nrm * d;
                    verts[i * 4 + 3] = v2 + a * s.stretched_rad - nrm * d;
                    cols[i * 4 + 3] = rw::vec4{c1, 1.f};
                } else {
                    verts[i * 4 + 2] = below + vec2{0.f, -10.f};
                }
                num = s.stretched_rad;
                vector = v2;
            }
            sprites::long_mesh(out, strip, verts, cols, size_t(n * 4 - 1));
            // the tinyStar at the tip, while excited
            if (w.worm_grass_worm_get_excitement(wm) > 0.05f)
                sprites::add(out, sprites::sprite{"tinyStar", star, 0.f, vec2{1.f, 1.f}, vec2{0.5f, 0.5f}, vec3{0.2f, 0.f, 1.f}});
        };
        // the worms in the order they were added to the room: the patches'
        // own, then the cosmetic ones
        for (int pass = 0; pass < 2; ++pass)
            for_each_patch(state, room, [&](dcon::worm_grass_patch_id p) {
                for_each_worm(state, p, [&](dcon::worm_grass_worm_id wm) {
                    if (w.worm_grass_worm_get_cosmetic(wm) == (pass == 1))
                        draw(wm);
                });
            });
    }
}
