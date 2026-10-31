#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include "needle_worm.hpp"
#include "dens.hpp"
#include "migrate.hpp"
#include "needle_worm_graphics.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "rooms.hpp"
#include "rot.hpp"
#include "tracker.hpp"

namespace needle_worm {
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;
    using creature::relationship;
    using creature::rel;
    using creature::tmpl;

    namespace {
        constexpr float float_max = std::numeric_limits<float>::max();
        // CreatureTemplate: BigNeedleWorm (SmallNeedleWorm inherits what it
        // doesn't set)
        constexpr float big_damage_resistance = 0.4f;
        constexpr float big_stun_resistance = 1.f;
        constexpr float small_damage_resistance = 0.2f;
        constexpr float small_stun_resistance = 0.4f;
        constexpr float instant_death_damage_limit = 1.2f;
        constexpr float visual_radius = 900.f;

        creature::needle_worm_payload& pl(sys::game_state& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).needle_worm;
        }
        creature::needle_worm_payload const& pl(sys::game_state const& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).needle_worm;
        }
        dcon::needle_worm_data_id data(sys::game_state const& state, dcon::creature_id c) {
            return creature::needle_worm_data(state.world, c);
        }
        bool is_worm(sys::game_state const& state, dcon::creature_id c) {
            return creature::live(state, c) && creature::type_of(state.world, c) == creature::type::needle_worm;
        }
        bool is_big_worm(sys::game_state const& state, dcon::creature_id c) {
            return is_worm(state, c) && !is_small(state, c);
        }
        bool is_small_worm(sys::game_state const& state, dcon::creature_id c) {
            return is_worm(state, c) && is_small(state, c);
        }

        float float_dist(ivec2 a, ivec2 b) {
            return rw::magnitude(rw::to_vec2(a - b));
        }
        ivec2 clamp_to_room(room_view const& room, ivec2 t) {
            return ivec2{custom::int_clamp(t.x, 0, room.width - 1), custom::int_clamp(t.y, 0, room.height - 1)};
        }

        //
        // SharedPhysics' tile ray traces (as tube_worm.cpp's)
        //
        // the tiles of the line from `a` to `b` in order (RayTracedTilesArray's
        // walk), until `f` returns true; whether it did
        template<typename F>
        bool walk_tiles(vec2 a, vec2 b, F&& f) {
            float const ax = a.x / 20.f, ay = a.y / 20.f, bx = b.x / 20.f, by = b.y / 20.f;
            float const dx = std::fabs(bx - ax), dy = std::fabs(by - ay);
            int32_t x = int32_t(std::floor(ax));
            int32_t y = int32_t(std::floor(ay));
            float const dt_dx = 1.f / dx, dt_dy = 1.f / dy;
            int32_t n = 1;
            int32_t x_inc, y_inc;
            float t_next_h, t_next_v;
            if (dx == 0.f) {
                x_inc = 0;
                t_next_h = dt_dx;
            } else if (bx > ax) {
                x_inc = 1;
                n += int32_t(std::floor(bx)) - x;
                t_next_h = (std::floor(ax) + 1.f - ax) * dt_dx;
            } else {
                x_inc = -1;
                n += x - int32_t(std::floor(bx));
                t_next_h = (ax - std::floor(ax)) * dt_dx;
            }
            if (dy == 0.f) {
                y_inc = 0;
                t_next_v = dt_dy;
            } else if (by > ay) {
                y_inc = 1;
                n += int32_t(std::floor(by)) - y;
                t_next_v = (std::floor(ay) + 1.f - ay) * dt_dy;
            } else {
                y_inc = -1;
                n += y - int32_t(std::floor(by));
                t_next_v = (ay - std::floor(ay)) * dt_dy;
            }
            n = std::min(n, 4096);
            for (; n > 0; --n) {
                if (f(ivec2{x, y}))
                    return true;
                if (t_next_v < t_next_h) {
                    y += y_inc;
                    t_next_v += dt_dy;
                } else {
                    x += x_inc;
                    t_next_h += dt_dx;
                }
            }
            return false;
        }
        // Custom.VerticalCrossPoint / HorizontalCrossPoint
        vec2 vertical_cross_point(vec2 a, vec2 b, float x) {
            if (a.x == b.x)
                return vec2{x, std::numeric_limits<float>::quiet_NaN()};
            float const k = (a.y - b.y) / (a.x - b.x);
            return vec2{x, a.y - a.x * k + k * x};
        }
        vec2 horizontal_cross_point(vec2 a, vec2 b, float y) {
            if (a.y == b.y)
                return vec2{std::numeric_limits<float>::quiet_NaN(), y};
            if (a.x == b.x)
                return vec2{a.x, y};
            float const k = (a.y - b.y) / (a.x - b.x);
            return vec2{(y - (a.y - a.x * k)) / k, y};
        }
        // SharedPhysics.ExactTerrainRayTrace: where the segment from `a` to
        // `b` first enters a solid tile (Custom.RectCollision's lower left)
        // and the direction it came in through (its upper right)
        struct terrain_hit {
            vec2 pos;
            vec2 dir;
        };
        std::optional<terrain_hit> exact_terrain_ray_trace(room_view const& room, vec2 a, vec2 b) {
            ivec2 hit{0, 0};
            if (!walk_tiles(a, b, [&](ivec2 t) { return room.get_tile(t).solid() ? (hit = t, true) : false; }))
                return std::nullopt;
            vec2 const m = room_view::middle_of_tile(hit);
            float const left = m.x - 10.f, bottom = m.y - 10.f, right = m.x + 10.f, top = m.y + 10.f;
            vec2 const pos = b, last = a;
            vec2 const v1 = vertical_cross_point(last, pos, left);
            vec2 const v2 = vertical_cross_point(last, pos, right);
            vec2 v3 = horizontal_cross_point(last, pos, bottom);
            vec2 v4 = horizontal_cross_point(last, pos, top);
            if (last.x == pos.x) {
                v3 = vec2{last.x, bottom};
                v4 = vec2{last.x, top};
            }
            bool const inside = last.x > left && last.x < right && last.y > bottom && last.y < top;
            float dst = inside ? float_max : rw::distance(last, pos);
            terrain_hit out{pos, vec2{0.f, 0.f}};
            auto const take = [&](vec2 v, vec2 dir) {
                out = terrain_hit{v, dir};
                dst = rw::distance(last, v);
            };
            if (v1.y >= bottom && v1.y <= top && custom::dist_less(last, v1, dst))
                take(v1, vec2{1.f, 0.f});
            if (v2.y >= bottom && v2.y <= top && custom::dist_less(last, v2, dst))
                take(v2, vec2{-1.f, 0.f});
            if (v3.x >= left && v3.x <= right && custom::dist_less(last, v3, dst))
                take(v3, vec2{0.f, 1.f});
            if (v4.x >= left && v4.x <= right && custom::dist_less(last, v4, dst))
                take(v4, vec2{0.f, -1.f});
            return out;
        }

        // SharedPhysics.VerticalCollision + HorizontalCollision of a tail
        // segment (a point of radius `rad` going through floors), simplified
        // to the rows and columns it spans: whether it touched the terrain
        bool point_terrain_collision(room_view const& room, vec2& pos, vec2 last, vec2& vel, float rad) {
            bool contact = false;
            ivec2 const lt = room_view::get_tile_position(last);
            bool const last_solid = room.get_tile(last).solid();
            int32_t const y0 = room_view::get_tile_position(vec2{0.f, pos.y - rad + 1.f}).y;
            int32_t const y1 = room_view::get_tile_position(vec2{0.f, pos.y + rad - 1.f}).y;
            if (vel.x > 0.f) {
                int32_t const xa = room_view::get_tile_position(vec2{last.x + rad, 0.f}).x;
                int32_t const xb = room_view::get_tile_position(vec2{pos.x + rad + 0.01f, 0.f}).x;
                for (int32_t i = xa; i <= xb && !contact; ++i)
                    for (int32_t j = y0; j <= y1 && !contact; ++j)
                        if (room.get_tile(i, j).solid() && !room.get_tile(i - 1, j).solid() && (lt.x < i || last_solid)) {
                            pos.x = float(i) * 20.f - rad;
                            vel.x = 0.f;
                            contact = true;
                        }
            } else if (vel.x < 0.f) {
                int32_t const xa = room_view::get_tile_position(vec2{last.x - rad, 0.f}).x;
                int32_t const xb = room_view::get_tile_position(vec2{pos.x - rad - 0.01f, 0.f}).x;
                for (int32_t i = xa; i >= xb && !contact; --i)
                    for (int32_t j = y0; j <= y1 && !contact; ++j)
                        if (room.get_tile(i, j).solid() && !room.get_tile(i + 1, j).solid() && (lt.x > i || last_solid)) {
                            pos.x = float(i + 1) * 20.f + rad;
                            vel.x = 0.f;
                            contact = true;
                        }
            }
            bool vcontact = false;
            int32_t const x0 = room_view::get_tile_position(vec2{pos.x - rad + 1.f, 0.f}).x;
            int32_t const x1 = room_view::get_tile_position(vec2{pos.x + rad - 1.f, 0.f}).x;
            if (vel.y > 0.f) {
                int32_t const ya = room_view::get_tile_position(vec2{0.f, last.y + rad}).y;
                int32_t const yb = room_view::get_tile_position(vec2{0.f, pos.y + rad + 0.01f}).y;
                for (int32_t j = ya; j <= yb && !vcontact; ++j)
                    for (int32_t i = x0; i <= x1 && !vcontact; ++i)
                        if (room.get_tile(i, j).solid() && !room.get_tile(i, j - 1).solid() && (lt.y < j || last_solid)) {
                            pos.y = float(j) * 20.f - rad;
                            vel.y = 0.f;
                            vcontact = true;
                        }
            } else if (vel.y < 0.f) {
                int32_t const ya = room_view::get_tile_position(vec2{0.f, last.y - rad}).y;
                int32_t const yb = room_view::get_tile_position(vec2{0.f, pos.y - rad - 0.01f}).y;
                for (int32_t j = ya; j >= yb && !vcontact; --j)
                    for (int32_t i = x0; i <= x1 && !vcontact; ++i)
                        if (room.get_tile(i, j).solid() && !room.get_tile(i, j + 1).solid() && (lt.y > j || last_solid)) {
                            pos.y = float(j + 1) * 20.f + rad;
                            vel.y = 0.f;
                            vcontact = true;
                        }
            }
            return contact || vcontact;
        }

        //
        // Update's working set
        //
        struct ctx {
            sys::game_state& state;
            dcon::creature_id c;
            room_view const& room;
            creature::needle_worm_payload& x;
            dcon::needle_worm_data_id d;
            rw::rng rng;
            bool small;
            uint32_t n, nt, total;
            dcon::body_chunk_id ch[max_chunks];
            tail_state& tail;
            // the pathfinder's field to the destination, when asked for
            std::optional<pathing::field_id> field;

            // (keyed by the constructing call site too: several ctxs of one
            // worm in one tick roll apart)
            ctx(sys::game_state& s, dcon::creature_id cc, room_view const& r, std::source_location const l = std::source_location::current())
                : state(s), c(cc), room(r), x(pl(s, cc)), d(data(s, cc)), rng(rw::random(s.seed, s.tick, uint32_t(cc.value), l)), small(x.variant == variant::small),
                  n(chunk_count(small)), nt(tail_count(small)), total(total_segments(small)), tail(s.world.needle_worm_data_get_tail(d)) {
                for (uint32_t i = 0; i < n; ++i)
                    ch[i] = chunk(s, cc, i);
            }
            dcon::data_container& w() { return state.world; }
            vec2& pos(uint32_t i) { return w().body_chunk_get_pos(ch[i]); }
            vec2& vel(uint32_t i) { return w().body_chunk_get_vel(ch[i]); }
            vec2 last_pos(uint32_t i) { return w().body_chunk_get_last_pos(ch[i]); }
            float mass(uint32_t i) { return w().body_chunk_get_mass(ch[i]); }
            float rad(uint32_t i) { return w().body_chunk_get_rad(ch[i]); }
            vec2& main() { return pos(0); }
            float gravity() { return w().physical_object_get_g(body(state, c)); }
            // abstractCreature.pos
            ivec2 tile() { return room_view::get_tile_position(pos(0)); }
            ivec2 dest() { return w().creature_get_destination(c); }
            attack_state& atk() { return w().needle_worm_data_get_attack(d); }
            big_ai_state& big_ai() { return w().needle_worm_data_get_big_ai(d); }
            ai_state& ai() { return w().needle_worm_data_get_ai(d); }
            small_state& young() { return w().needle_worm_data_get_young(d); }
            behavior beh() { return behavior(w().creature_get_behavior(c)); }
            int32_t proximity(ivec2 t) { return pathing::ai_terrain_proximity(state, room.id, t); }
            int32_t proximity(vec2 p) { return proximity(room_view::get_tile_position(p)); }
            bool narrow(ivec2 t) { return pathing::narrow_space(room, t); }
            bool narrow(vec2 p) { return narrow(room_view::get_tile_position(p)); }

            // the segments: the chunks, then the tail
            vec2 seg_pos(uint32_t s) { return s < n ? pos(s) : tail.pos[s - n]; }
            void set_seg_pos(uint32_t s, vec2 v) {
                if (s < n)
                    pos(s) = v;
                else
                    tail.pos[s - n] = v;
            }
            void add_seg_pos(uint32_t s, vec2 v) {
                if (s < n)
                    pos(s) += v;
                else
                    tail.pos[s - n] += v;
            }
            vec2 seg_vel(uint32_t s) { return s < n ? vel(s) : tail.vel[s - n]; }
            void set_seg_vel(uint32_t s, vec2 v) {
                if (s < n)
                    vel(s) = v;
                else
                    tail.vel[s - n] = v;
            }
            void add_seg_vel(uint32_t s, vec2 v) {
                if (s < n)
                    vel(s) += v;
                else
                    tail.vel[s - n] += v;
            }
            void add_seg_vel_y(uint32_t s, float v) {
                if (s < n)
                    vel(s).y += v;
                else
                    tail.vel[s - n].y += v;
            }
            // GetSegmentMass, GetSegmentRadForCollision, GetSegmentRadForRopeLength
            float seg_mass(uint32_t s) {
                if (s < n)
                    return mass(s);
                return custom::lerp_map(float(s), float(n), float(total - 1), mass(n - 1) * 0.9f, mass(n - 1) * 0.01f, 0.3f);
            }
            float seg_rad_collision(uint32_t s) { return s < n ? rad(s) : 1.f; }
            float seg_rad_rope(uint32_t s) {
                if (s < n)
                    return rad(s) * (small ? 0.8f : 1.f);
                return custom::lerp_map(float(s), float(n), float(total - 1), rad(n - 1), small ? 8.f : 11.f);
            }
            // GetSegmentDir(seg, 1)
            vec2 seg_dir(uint32_t s) {
                if (s == 0)
                    return custom::dir_vec(seg_pos(1), seg_pos(0));
                return custom::dir_vec(seg_pos(s), seg_pos(s - 1));
            }
            // BigNeedleWorm.FangPos
            vec2 fang_pos() { return main() + custom::dir_vec(pos(1), main()) * fang_length; }

            pathing::field_id path() {
                if (!field)
                    field = pathing::field_to(state, room.id, dest(), fly);
                return *field;
            }
            bool accessible(ivec2 t) { return room.inside(t.x, t.y) && pathing::accessible(room, t, fly); }
            // PathFinder.CoordinateReachableAndGetbackable: judged by the
            // field to its destination (a flyer goes back the way it came)
            bool reachable(ivec2 t) { return accessible(t) && pathing::distance(state, path(), t) != pathing::unreachable; }
            // StandardPather.FollowPath from `from`: the next tile on the way,
            // the goal itself at the goal (the flight keeps steering about it)
            std::optional<ivec2> follow_path(ivec2 from) {
                from = clamp_to_room(room, from);
                if (!accessible(from))
                    return std::nullopt;
                if (auto const step = pathing::next_step(state, path(), from))
                    return step;
                if (pathing::distance(state, path(), from) == 0)
                    return from;
                return std::nullopt;
            }
        };

        // Creature.Stun
        void stun(ctx& x, int32_t st) {
            creature::stun(x.state, x.c, st);
        }

        //
        // NeedleWormAI
        //

        // ArtificialIntelligence.VisualContact + NeedleWormAI.VisualScore: it
        // sees ahead of its head, not behind it (from 60 to 100 out)
        bool visual_contact(sys::game_state const& state, dcon::creature_id c, room_view const& room, vec2 look) {
            auto const& w = state.world;
            vec2 const eye = w.body_chunk_get_pos(chunk(state, c, 0));
            vec2 const neck = w.body_chunk_get_pos(chunk(state, c, 1));
            if (!custom::dist_less(eye, look, visual_radius))
                return false;
            float score = mathf::inverse_lerp(visual_radius, 0.f, rw::distance(eye, look));
            score *= custom::lerp_map(rw::distance(eye, look), 60.f, 100.f, 1.f,
                mathf::inverse_lerp(-0.2f, 0.1f, rw::dot(rw::normalized(neck - eye), rw::normalized(eye - look))));
            return score > 0.f && room.visual_contact(eye, look);
        }

        // NeedleWormAI.TileInEnclosedArea
        bool tile_in_enclosed_area(ctx& x, ivec2 tile) {
            int32_t const prox = x.proximity(tile);
            if (prox < 2)
                return true;
            if (prox > 4)
                return false;
            int32_t num = 0;
            for (auto const d : custom::eight_directions) {
                if (x.room.get_tile(tile + d * 2).solid() || x.room.get_tile(tile + d * 3).solid() || x.room.get_tile(tile + d * 4).solid()) {
                    if (++num > 3)
                        return true;
                    for (int32_t j = 2; j < 6; ++j)
                        if (x.room.get_tile(tile - d * j).solid())
                            return true;
                }
            }
            return false;
        }

        // NeedleWormAI.MinFlyHeight (no worm grass below here)
        int32_t min_fly_height(ctx& x, bool enclosed) {
            if (enclosed)
                return 0;
            return 4 + int32_t(4.f * x.ai().fly_height_add);
        }

        // NeedleWorm.MoveUpFromFloor
        vec2 move_up_from_floor(ctx& x, ivec2 tile) {
            int32_t const num = min_fly_height(x, tile_in_enclosed_area(x, tile));
            int32_t const alt = pathing::floor_altitude(x.room, tile);
            if (alt > num)
                return room_view::middle_of_tile(tile);
            int32_t const up = num - alt;
            for (int32_t k = 0; k < up && x.proximity(tile + ivec2{0, 1}) > 1 && tile.y < x.room.height - 3; ++k)
                tile.y += 1;
            return room_view::middle_of_tile(tile);
        }

        // the tracker's settings (NeedleWormAI's constructor: Tracker(5, 10,
        // 450, ..., 10), ThreatTracker(3); BigNeedleWormAI's PreyTracker(5,
        // 1.3, 15, 1000))
        tracker::params const& tracker_params() {
            static tracker::params const p = [] {
                tracker::params q;
                q.see_around_corners = 5;
                q.frames_to_remember = 450;
                q.ghost_dismissal_range = 10;
                q.prey_max = 5;
                q.persistance_bias = 1.3f;
                q.sure_to_get = 15.f;
                q.sure_to_lose = 1000.f;
                q.threat_max = 3;
                return q;
            }();
            return p;
        }

        // SocialMemory of the adult
        social_entry* social_of(sys::game_state& state, dcon::creature_id c, dcon::creature_id t, bool make) {
            auto& m = state.world.needle_worm_data_get_social(data(state, c));
            social_entry* weakest = nullptr;
            for (auto& e : m.e) {
                if (e.who == t)
                    return &e;
                if (!e.who || !state.world.creature_is_valid(e.who)) {
                    if (!weakest || weakest->who)
                        weakest = &e;
                } else if (!weakest || (weakest->who && e.temp_like > weakest->temp_like)) {
                    weakest = &e;
                }
            }
            if (!make || !weakest)
                return nullptr;
            *weakest = social_entry{t, 0.f, 0.f};
            return weakest;
        }
        float temp_like(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) {
            auto const& m = state.world.needle_worm_data_get_social(data(state, c));
            for (auto const& e : m.e)
                if (e.who == t)
                    return e.temp_like;
            return 0.f;
        }

        // `t` carries one of the young (NeedleWormTrackState.holdingChild)
        bool holds_child(sys::game_state const& state, dcon::creature_id t) {
            for (uint32_t i = 0; i < 2; ++i)
                if (is_small_worm(state, creature::grasped(state.world, t, i)))
                    return true;
            return false;
        }

        // NeedleWormAI.UncomfortableToAfraidRelationshipModifier: what comes
        // close (or into a narrow space) is feared, by its size
        relationship uncomfortable_to_afraid(ctx& x, creature::tracked const& r, relationship cur) {
            auto* rep = tracker::find(tracker::of(x.state, x.c), r.who);
            if (!rep)
                return cur;
            ivec2 const guess = tracker::best_guess(r);
            if (rep->state & track_close) {
                if (x.ai().in_free_space_counter > 50 && float_dist(x.tile(), guess) > 10.f)
                    rep->state &= uint8_t(~track_close);
            } else if (float_dist(x.tile(), guess) < 5.f) {
                rep->state |= track_close;
            }
            if ((rep->state & track_close) || (x.room.inside(guess.x, guess.y) && x.narrow(guess))) {
                float const k = creature::live(x.state, r.who) ? custom::lerp_map(creature::total_mass(x.state, r.who), 0.25f, 4.f, 0.1f, 1.f) : 0.5f;
                return relationship{rel::afraid, cur.intensity * k};
            }
            return cur;
        }

        // BigNeedleWormAI / SmallNeedleWormAI.UpdateDynamicRelationship
        relationship relationship_to(ctx& x, creature::tracked const& r) {
            auto const t = r.who;
            if (!x.small) {
                if (creature::small_creature(creature::template_of(x.state, t)))
                    return relationship{};
                if (auto* rep = tracker::find(tracker::of(x.state, x.c), t); rep && r.visual) {
                    if (holds_child(x.state, t))
                        rep->state |= track_holding_child;
                    else
                        rep->state &= uint8_t(~track_holding_child);
                }
                if (r.state & track_holding_child)
                    return relationship{rel::attacks, 1.f};
                if (float const tl = temp_like(x.state, x.c, t); tl < -0.25f)
                    return relationship{rel::attacks, std::fabs(tl)};
                return uncomfortable_to_afraid(x, r, creature::static_relationship(x.state, x.c, t));
            }
            relationship rl = uncomfortable_to_afraid(x, r, creature::static_relationship(x.state, x.c, t));
            if (rl.type == rel::afraid && creature::dead(x.state, t))
                rl.intensity = 0.f;
            return rl;
        }

        // the trackers' questions, answered by the noodlefly
        bool hook_sees(void* u, dcon::body_chunk_id k) {
            auto& x = *static_cast<ctx*>(u);
            return visual_contact(x.state, x.c, x.room, x.state.world.body_chunk_get_pos(k));
        }
        bool hook_sees_point(void* u, vec2 p) {
            auto& x = *static_cast<ctx*>(u);
            return visual_contact(x.state, x.c, x.room, p);
        }
        relationship hook_relate(void* u, creature::tracked const& r) {
            return relationship_to(*static_cast<ctx*>(u), r);
        }
        bool hook_reach(void* u, ivec2 t) {
            return static_cast<ctx*>(u)->reachable(t);
        }
        tracker::hooks make_hooks(ctx& x) {
            tracker::hooks h;
            h.user = &x;
            h.sees = hook_sees;
            h.sees_point = hook_sees_point;
            h.relate = hook_relate;
            h.module = tracker::module_without_aggression;
            h.reach = hook_reach;
            return h;
        }

        // the mother of a young: a live adult out in its room
        dcon::creature_id mother_of(ctx& x) {
            auto const m = x.young().mother;
            return is_big_worm(x.state, m) && creature::here(x.state, m, x.room.id) && !x.state.world.creature_get_dead(m) ? m : dcon::creature_id{};
        }

        // BigNeedleWorm.NormalFlyingState
        bool normal_flying_state(sys::game_state const& state, dcon::creature_id c) {
            auto const& a = state.world.needle_worm_data_get_attack(data(state, c));
            return consious(state, c) && a.lame_counter < 1 && pl(state, c).flying > 0.5f && a.charging_attack < 0.5f && !a.has_swish && !a.in_wall && !a.impale;
        }

        // BigNeedleWormAI.SmallRespondCry, BigRespondCry
        void small_respond_cry(sys::game_state& state, dcon::creature_id m) {
            auto& b = state.world.needle_worm_data_get_big_ai(data(state, m));
            if (b.respond_scream_counter == 0)
                b.respond_scream_counter = rw::random(state.seed, state.tick, uint32_t(m.value)).range(10, 50);
        }
        void big_respond_cry(sys::game_state& state, dcon::creature_id m) {
            auto& b = state.world.needle_worm_data_get_big_ai(data(state, m));
            if (b.respond_scream_counter > -1)
                b.respond_scream_counter = -rw::random(state.seed, state.tick, uint32_t(m.value)).range(6, 16);
        }
        // BigNeedleWorm.SmallCry, BigCry (no sounds here: the snout's quiver)
        void small_cry(ctx& x) {
            if (x.atk().attack_ready > 0.7f || x.x.screaming > 0.1f)
                return;
            x.x.screaming = 0.5f;
        }
        void big_cry(ctx& x) {
            if (x.atk().attack_ready > 0.8f)
                return;
            x.x.screaming = 1.f;
        }

        // SmallNeedleWorm.ClosestCreature: what holds it, else the closest
        // big enough creature that isn't a noodlefly
        dcon::creature_id closest_creature(ctx& x) {
            if (auto const h = creature::holder(x.state, x.c))
                return h;
            dcon::creature_id best;
            float dst = float_max;
            rooms::for_each_creature(x.state, x.room.id, [&](dcon::creature_id o) {
                if (o == x.c || !creature::here(x.state, o, x.room.id) || x.w().creature_get_dead(o) || creature::type_of(x.w(), o) == creature::type::needle_worm
                || creature::small_creature(creature::template_of(x.state, o)))
                    return;
                float const d = rw::distance(x.main(), creature::main_pos(x.state, o));
                if (d < dst) {
                    dst = d;
                    best = o;
                }
            });
            return best;
        }

        // SmallNeedleWorm.SmallScream
        void small_scream(ctx& x, bool mother_respond) {
            x.x.screaming = mathf::lerp(0.4f, 0.6f, x.rng.value());
            auto const m = mother_of(x);
            if (!mother_respond || !m)
                return;
            if (auto const h = creature::holder(x.state, x.c)) {
                ctx mx(x.state, m, x.room);
                // (several young may scream for one mother in a tick)
                mx.rng = rw::random(x.state.seed, x.state.tick, rw::ids(uint32_t(m.value), uint32_t(x.c.value)));
                tracker::see(x.state, m, x.room, h, make_hooks(mx));
            }
            small_respond_cry(x.state, m);
        }

        // SmallNeedleWorm.Scream: it dies, and its mother turns on the
        // closest creature
        void scream(ctx& x) {
            auto& y = x.young();
            if (y.has_screamed)
                return;
            y.has_screamed = 1;
            creature::die(x.state, x.c);
            x.x.screaming = 1.f;
            auto const m = mother_of(x);
            if (!m)
                return;
            big_respond_cry(x.state, m);
            if (auto const cc = closest_creature(x))
                if (auto* e = social_of(x.state, m, cc, true)) {
                    e->like = -1.f;
                    e->temp_like = -1.f;
                }
        }

        // NeedleWormAI.IdleScore: BigNeedleWormAI's and SmallNeedleWormAI's
        float idle_score(ctx& x, ivec2 coord) {
            if (!x.room.inside(coord.x, coord.y) || !x.reachable(coord))
                return float_max;
            auto& a = x.ai();
            auto const& ts = tracker::of(x.state, x.c);
            float num = 0.f;
            auto const m = x.small ? mother_of(x) : dcon::creature_id{};
            if (m) {
                ivec2 const mt = room_view::get_tile_position(creature::main_pos(x.state, m));
                if (consious(x.state, m))
                    num += float_dist(coord, x.w().creature_get_destination(m));
                num += float_dist(coord, mt) * 1.1f;
            } else {
                for (int32_t i = 0; i < a.old_count; ++i)
                    num -= mathf::pow(mathf::min(30.f, float_dist(a.old_idle[i], coord)), 2.f) / (30.f * (1.f + float(i) * 0.2f));
            }
            for (uint32_t j = 0; j < ts.count; ++j) {
                auto const& r = ts.reps[j];
                if (r.rel.type == rel::ignores || creature::dead(x.state, r.who))
                    continue;
                ivec2 const g = tracker::best_guess(r);
                auto const t = creature::template_of(x.state, r.who);
                float range = 25.f;
                if (x.small)
                    range = (t == tmpl::big_needle_worm || t == tmpl::small_needle_worm) ? 3.f : 15.f;
                else if (t == tmpl::small_needle_worm)
                    continue;
                if (float_dist(coord, g) < range)
                    num += custom::lerp_map(float_dist(coord, g), 1.f, range, 30.f * creature::body_size(t), 0.f, 0.5f);
            }
            int32_t const prox = x.proximity(coord);
            if (x.small) {
                num += float_dist(x.tile(), coord) / 20.f;
                num -= float(mathf::min(prox, 3)) * 5.f;
            } else {
                num -= mathf::pow(float(mathf::min(prox, 5)), 2.f) * 2.f;
            }
            if (x.narrow(coord))
                num += 1000.f;
            bool const enclosed = tile_in_enclosed_area(x, coord);
            int32_t const alt = pathing::floor_altitude(x.room, coord);
            if (enclosed)
                num += 100.f;
            else
                num += mathf::min(12.f, float(std::abs(alt - min_fly_height(x, enclosed)))) * 0.75f;
            if (alt < min_fly_height(x, enclosed))
                num += 20.f;
            return num;
        }

        // NeedleWormAI.IdleBehavior
        void idle_behavior(ctx& x) {
            auto& a = x.ai();
            vec2 p;
            if (x.rng.value() < 0.15f)
                p = vec2{x.rng.value() * x.room.pixel_width(), x.rng.value() * x.room.pixel_height()};
            else if (x.rng.value() < 0.5f)
                p = room_view::middle_of_tile(a.idle_pos) + custom::rnv(x.rng) * x.rng.value() * 400.f;
            else
                p = x.main() + custom::rnv(x.rng) * x.rng.value() * 400.f;
            ivec2 const cand = room_view::get_tile_position(p);
            if (idle_score(x, cand) < idle_score(x, a.idle_pos))
                a.idle_pos = cand;
            ivec2& dest = x.w().creature_get_destination(x.c);
            if (x.small && mother_of(x)) {
                dest = a.idle_pos;
            } else if (idle_score(x, a.idle_pos) + float(a.idle_counter) * 2.f < idle_score(x, dest)) {
                if (a.add_old_idle_pos_delay < 1 && x.w().creature_get_time_in_room(x.c) > 120) {
                    a.add_old_idle_pos_delay = 90;
                    if (a.old_count == ai_state::max_old) {
                        for (int32_t i = 1; i < a.old_count; ++i)
                            a.old_idle[i - 1] = a.old_idle[i];
                        --a.old_count;
                    }
                    a.old_idle[a.old_count++] = a.last_assigned_idle_pos;
                    // (confirmedNarrowRooms isn't kept: every room is liked)
                    a.idle_counter = x.rng.range(100, 400);
                }
                dest = a.idle_pos;
                // field() was asked for the old destination
                x.field.reset();
                a.last_assigned_idle_pos = a.idle_pos;
            }
            --a.idle_counter;
            if (rw::distance(x.pos(1), room_view::middle_of_tile(dest)) < 60.f) {
                --a.idle_counter;
                if (a.in_free_space_counter < 50) {
                    --a.idle_counter;
                    if (tile_in_enclosed_area(x, dest))
                        a.idle_counter -= 2;
                }
            }
        }

        // NeedleWormAI.Update (after the modules)
        void ai_base_update(ctx& x) {
            auto& a = x.ai();
            if (a.add_old_idle_pos_delay > 0)
                --a.add_old_idle_pos_delay;
            ++a.in_room_counter;
            // abstractAI.AbstractBehavior(1): NeedleWormAbstractAI's
            // MigrationBehaviorRoll before the migration's own roll
            if (x.beh() == behavior::idle && x.rng.value() < mathf::inverse_lerp(1900.f, 3200.f, float(a.in_room_counter)) / 6.f)
                migrate::abstract_behavior(x.state, x.c, x.room);
            if (a.focus) {
                auto const* r = tracker::find(tracker::of(x.state, x.c), a.focus);
                if (!r || r->ticks_since_seen > 40)
                    a.focus = dcon::creature_id{};
            }
            ivec2 const t = x.tile();
            if (x.room.inside(t.x, t.y) && x.narrow(t))
                a.in_free_space_counter -= 3;
            else if (x.proximity(t) < 3)
                --a.in_free_space_counter;
            else if (x.proximity(t) > 5)
                ++a.in_free_space_counter;
            a.in_free_space_counter = custom::int_clamp(a.in_free_space_counter, 0, 100);
            x.x.fly_speed = custom::lerp_and_tick(x.x.fly_speed, x.beh() == behavior::idle ? 0.f : 1.f, 0.06f, 0.016666668f);
            if (x.beh() == behavior::flee)
                a.fly_height_add = mathf::min(1.f, a.fly_height_add + 0.016666668f);
            else
                a.fly_height_add = mathf::max(0.f, a.fly_height_add - 0.0045454544f);
        }

        // BigNeedleWormAI.AttackPosScore
        float attack_pos_score(ctx& x, vec2 test, vec2 target) {
            ivec2 const t = room_view::get_tile_position(test);
            if (!x.reachable(t) || !x.room.visual_contact(test, target))
                return float_max;
            int32_t const prox = x.proximity(t);
            if (prox < 2)
                return float_max;
            auto const& b = x.big_ai();
            float num = mathf::max(0.f, std::fabs(b.ideal_attack_dist - rw::distance(test, target)) - 50.f);
            num -= float(prox) * 20.f;
            num += rw::distance(test, x.main()) / 10.f;
            if (custom::dist_less(test, x.main(), 60.f))
                num -= float(b.attack_counter) * 5.f;
            if (test.x < 0.f || test.y < 0.f || test.x > x.room.pixel_width() || test.y > x.room.pixel_height())
                num += 1000.f;
            return num;
        }

        // BigNeedleWormAI.AttackBehavior: into a spot to charge from
        vec2 attack_behavior(ctx& x, creature::tracked const* r, vec2 target) {
            auto& b = x.big_ai();
            if (!r) {
                b.attack_from_pos = x.main();
                return target;
            }
            auto const t = r->who;
            x.ai().focus = t;
            b.ideal_attack_dist = is_big_worm(x.state, t) ? 400.f : 200.f;
            target = room_view::middle_of_tile(tracker::best_guess(*r));
            ivec2& dest = x.w().creature_get_destination(x.c);
            if (r->ticks_since_seen > 20 || !creature::here(x.state, t, x.room.id)) {
                dest = clamp_to_room(x.room, tracker::best_guess(*r));
                b.attack_from_pos = target;
            } else {
                if (r->visual) {
                    uint32_t const nk = std::max(1u, creature::chunk_count_of(x.state, t));
                    auto const k = creature::chunk_of(x.state, t, uint32_t(b.target_chunk) % nk);
                    target = x.w().body_chunk_get_pos(k);
                    b.target_vel = custom::move_towards(b.target_vel, target - x.w().body_chunk_get_last_pos(k), 0.075f);
                }
                if (x.atk().charging_attack < 0.5f) {
                    vec2 const test = (x.rng.value() < 0.5f ? b.attack_from_pos : b.attack_target_pos) + custom::rnv(x.rng) * 300.f * x.rng.value();
                    if (attack_pos_score(x, test, b.attack_target_pos) < attack_pos_score(x, b.attack_from_pos, b.attack_target_pos))
                        b.attack_from_pos = test;
                }
                if (x.beh() == behavior::attack && custom::dist_less(x.main(), b.attack_from_pos, 300.f))
                    ++b.attack_counter;
                dest = clamp_to_room(x.room, room_view::get_tile_position(b.attack_from_pos));
            }
            x.field.reset();
            if (custom::dist_less(x.main(), b.attack_from_pos, 800.f))
                ++b.attack_counter;
            return target;
        }

        // BigNeedleWormAI.Update
        void big_ai_update(ctx& x) {
            auto& w = x.w();
            tracker::hooks const h = make_hooks(x);
            tracker::update(x.state, x.c, x.room, x.tile(), tracker_params(), h);
            ai_base_update(x);
            auto& b = x.big_ai();
            if (b.respond_scream_counter < 0) {
                if (++b.respond_scream_counter == 0)
                    big_cry(x);
            } else if (b.respond_scream_counter > 0) {
                if (--b.respond_scream_counter == 0)
                    small_cry(x);
            }
            // SocialMemory.EvenOutAllTemps
            for (auto& e : w.needle_worm_data_get_social(x.d).e)
                e.temp_like = e.temp_like < e.like ? mathf::min(e.like, e.temp_like + 0.0005f) : mathf::max(e.like, e.temp_like - 0.0005f);

            // UtilityComparer: threat (1), prey (0.9, smoothed), rain (1),
            // the current module's continuation bonus 1.1 (no StuckTracker)
            auto const& ts = tracker::of(x.state, x.c);
            auto const* prey = tracker::prey(ts);
            behavior const cur = x.beh();
            float const raw_prey = prey ? tracker::prey_utility(x.state, x.c, x.room, x.tile(), tracker_params()) : 0.f;
            b.prey_utility = mathf::lerp(b.prey_utility, raw_prey, 0.15f);
            float threat = tracker::threat(ts) ? tracker::threat_utility(ts) : 0.f;
            float hunt = prey ? b.prey_utility * 0.9f : 0.f;
            float rain = dens::rain_utility(x.state, x.c, x.room.id);
            if (cur == behavior::flee)
                threat *= 1.1f;
            else if (cur == behavior::attack)
                hunt *= 1.1f;
            else if (cur == behavior::escape_rain)
                rain *= 1.1f;
            float utility = threat;
            behavior be = behavior::flee;
            if (hunt > utility) {
                utility = hunt;
                be = behavior::attack;
            }
            if (rain > utility) {
                utility = rain;
                be = behavior::escape_rain;
            }
            if (utility < 0.1f)
                be = behavior::idle;
            auto& keep = b.keep_close_to;
            if (keep && !(creature::live(x.state, keep) && creature::here(x.state, keep, x.room.id)))
                keep = dcon::creature_id{};
            if (keep) {
                if (be != behavior::attack)
                    be = behavior::faux_attack;
                if (x.rng.value() < 0.0125f)
                    keep = dcon::creature_id{};
            }
            w.creature_set_behavior(x.c, uint8_t(be));
            w.creature_set_current_utility(x.c, utility);

            --b.attack_counter;
            if (x.rng.value() < 0.008333334f)
                b.target_chunk = x.rng.range(0, 100);
            vec2 target = b.attack_target_pos;
            b.target_vel *= 0.99f;
            ivec2& dest = w.creature_get_destination(x.c);
            switch (be) {
            case behavior::idle:
                if (auto const t = migrate::travel_tile(x.state, x.c, x.room)) {
                    dest = *t;
                    x.field.reset();
                } else {
                    idle_behavior(x);
                }
                break;
            case behavior::flee:
                dest = clamp_to_room(x.room, tracker::flee_to(x.state, x.c, x.room, x.tile(), 10, 20, tracker_params(), h));
                x.field.reset();
                if (ts.threat)
                    x.ai().focus = ts.threat;
                break;
            case behavior::attack:
                target = attack_behavior(x, prey, target);
                break;
            case behavior::faux_attack:
                if (keep)
                    target = attack_behavior(x, tracker::find(ts, keep), target);
                break;
            case behavior::escape_rain:
                if (auto const t = dens::home_tile(x.state, x.c, x.room)) {
                    dest = *t;
                    x.field.reset();
                }
                break;
            default:
                break;
            }
            b.attack_target_pos = rw::lerp(b.attack_target_pos,
                target + b.target_vel * custom::lerp_map(rw::distance(b.attack_from_pos, target), 80.f, 400.f, 0.f, 30.f, 0.35f),
                mathf::inverse_lerp(0.9f, 0.5f, x.atk().charging_attack));
            // a creature it holds a grudge against is never lost for long
            uint32_t pop = 0;
            rooms::for_each_creature(x.state, x.room.id, [&](dcon::creature_id) { ++pop; });
            if (pop > 0) {
                uint32_t const pick = uint32_t(x.rng.range(0, int32_t(pop)));
                uint32_t i = 0;
                dcon::creature_id o;
                rooms::for_each_creature(x.state, x.room.id, [&](dcon::creature_id k) {
                    if (i++ == pick)
                        o = k;
                });
                if (o && o != x.c && creature::here(x.state, o, x.room.id) && !w.creature_get_dead(o) && temp_like(x.state, x.c, o) < -0.25f)
                    if (auto const* r = tracker::find(tracker::of(x.state, x.c), o); r && r->ticks_since_seen > 80)
                        tracker::see(x.state, x.c, x.room, o, h);
            }
            b.attack_counter = custom::int_clamp(b.attack_counter, 0, 100);
            // the destination may have changed: the path is asked for anew
            x.field.reset();
        }

        // SmallNeedleWormAI.Update
        void small_ai_update(ctx& x) {
            auto& w = x.w();
            tracker::hooks const h = make_hooks(x);
            tracker::update(x.state, x.c, x.room, x.tile(), tracker_params(), h);
            // the mother: the closest adult of the room, when it has none
            // (NeedleWormAbstractAI.FindMotherInRoom picks one of the room's
            // adults, by den)
            if (!mother_of(x)) {
                dcon::creature_id best;
                float dst = float_max;
                rooms::for_each_creature(x.state, x.room.id, creature::type::needle_worm, [&](dcon::creature_id o) {
                    if (o == x.c || is_small(x.state, o) || w.creature_get_dead(o) || !creature::here(x.state, o, x.room.id))
                        return;
                    float const d = rw::distance(x.main(), creature::main_pos(x.state, o));
                    if (d < dst) {
                        dst = d;
                        best = o;
                    }
                });
                x.young().mother = best;
            }
            auto const m = mother_of(x);
            ai_base_update(x);
            auto const& ts = tracker::of(x.state, x.c);
            float const threat = tracker::threat(ts) ? tracker::threat_utility(ts) : 0.f;
            float const rain = dens::rain_utility(x.state, x.c, x.room.id);
            float utility = threat;
            behavior be = behavior::flee;
            if (rain > utility) {
                utility = rain;
                be = behavior::escape_rain;
            }
            if (utility < 0.1f)
                be = behavior::idle;
            w.creature_set_behavior(x.c, uint8_t(be));
            w.creature_set_current_utility(x.c, utility);
            // it always knows where its mother is (followCreature)
            if (m)
                tracker::see(x.state, x.c, x.room, m, h);
            ivec2& dest = w.creature_get_destination(x.c);
            switch (be) {
            case behavior::idle: {
                std::optional<ivec2> t;
                if (!m)
                    t = migrate::travel_tile(x.state, x.c, x.room);
                if (t) {
                    dest = *t;
                    x.field.reset();
                } else {
                    idle_behavior(x);
                }
                if (m) {
                    if (float_dist(room_view::get_tile_position(creature::main_pos(x.state, m)), x.tile()) > 5.f)
                        x.x.fly_speed = 1.f;
                    if (x.rng.value() < 0.003125f && x.x.screaming == 0.f)
                        if (auto const* r = tracker::find(ts, m); r && r->ticks_since_seen > x.rng.range(80, 300))
                            small_scream(x, true);
                }
                break;
            }
            case behavior::flee:
                dest = clamp_to_room(x.room, tracker::flee_to(x.state, x.c, x.room, x.tile(), 10, 20, tracker_params(), h));
                x.field.reset();
                if (ts.threat)
                    x.ai().focus = ts.threat;
                break;
            case behavior::escape_rain:
                if (auto const t = dens::home_tile(x.state, x.c, x.room)) {
                    dest = *t;
                    x.field.reset();
                }
                break;
            default:
                break;
            }
            x.field.reset();
        }

        //
        // NeedleWorm: the body
        //

        // NeedleWorm.ResetTail
        void reset_tail(ctx& x, vec2 ps) {
            for (uint32_t i = 0; i < x.nt; ++i) {
                x.tail.pos[i] = ps + custom::rnv(x.rng) * x.rng.value();
                x.tail.last_pos[i] = x.tail.pos[i];
                x.tail.vel[i] = vec2{0.f, 0.f};
            }
            for (uint32_t j = x.n; j < x.total; ++j)
                x.tail.rope_mode[j] = 0;
            for (auto& r : x.w().needle_worm_data_get_ropes(x.d).r)
                r.bend_count = 0;
        }

        // NeedleWorm.SinMovementInBody: a wave down the body
        void sin_movement_in_body(ctx& x, float amp1, float amp2, float wave1, float wave2, bool add_to_positions) {
            vec2 add[max_segments] = {};
            for (uint32_t i = 1; i < x.total; ++i) {
                float const fi = float(i);
                add[i] = custom::perpendicular_vector(x.seg_pos(i), x.seg_pos(i - 1))
                    * std::sin(x.x.crawl_sin + fi * custom::lerp_map(fi, 1.f, float(x.total - 1), wave1, wave2))
                    * custom::lerp_map(fi, 1.f, float(x.total - 1), amp1, amp2);
            }
            for (uint32_t i = 1; i < x.total; ++i) {
                x.add_seg_vel(i, add[i]);
                if (add_to_positions)
                    x.add_seg_pos(i, add[i]);
            }
        }

        // NeedleWorm.SlowFlySpeed
        float slow_fly_speed(ctx& x) {
            return mathf::lerp(0.25f, x.small ? 0.7f : 0.5f, x.x.fly_speed);
        }

        // NeedleWorm.Crawl through a narrow space
        void crawl(ctx& x, ivec2 to) {
            physics::set_go_through_floors(x.state, body(x.state, x.c), true);
            if (x.tile() == x.dest()) {
                x.x.at_dest_this_frame = 1;
                return;
            }
            float const num = mathf::lerp(3.f, 7.f, x.x.stuck_at_same_pos);
            float const k = x.small ? 0.6f : 1.f;
            vec2 const goal = room_view::middle_of_tile(to);
            for (uint32_t i = 0; i < x.n; ++i) {
                float const t = mathf::inverse_lerp(0.f, float(x.n - 1), float(i));
                x.vel(i) *= 0.6f;
                x.vel(i) += custom::dir_vec(x.pos(i), goal) * mathf::lerp(num, num * -0.3f, t) * k;
                x.pos(i) += custom::dir_vec(x.pos(i), goal) * mathf::lerp(num / 7.f, num / 70.f, t) * k;
            }
            x.x.crawl_sin += 0.2f;
            sin_movement_in_body(x, 1.5f * k, 1.5f * k, 1.6f, x.small ? 1.2f : 0.2f, true);
        }

        // BigNeedleWorm.Dodge: a sideways jolt away from what comes at it
        bool dodge(ctx& x, vec2 proj, vec2 dir) {
            auto& a = x.atk();
            if (a.dodge_delay > 0 || x.x.flying == 0.f)
                return false;
            a.dodge_delay = 10;
            a.charging_attack /= 2.f;
            vec2 const side = custom::perpendicular_vector(dir) * mathf::sign(custom::distance_to_line(x.pos(1), proj, proj + dir));
            float dst = -float_max;
            int32_t far = -1;
            for (uint32_t i = 0; i < x.n; ++i)
                if (!custom::dist_less(proj, x.pos(i), dst)) {
                    dst = rw::distance(proj, x.pos(i));
                    far = int32_t(i);
                }
            if (far > -1)
                x.vel(uint32_t(far)) += side * 12.f * x.x.flying;
            for (uint32_t j = 0; j < x.total; ++j) {
                float const t = mathf::inverse_lerp(0.f, float(x.total - 1), float(j));
                x.add_seg_vel(j, side * 12.f * (1.f - t) * x.x.flying);
            }
            x.x.look_dir = custom::dir_vec(x.main() + side * 20.f, proj);
            return true;
        }

        // BigNeedleWorm.AttackCharge: winding up, the body drawn back along
        // the line of attack, the tail quivering; then the swish
        void attack_charge(ctx& x) {
            auto& a = x.atk();
            auto const& b = x.big_ai();
            float num3 = mathf::inverse_lerp(0.5f, 0.95f, a.attack_ready);
            vec2 const v = custom::dir_vec(b.attack_from_pos, b.attack_target_pos);
            float num4 = mathf::inverse_lerp(0.2f, 0.9f, rw::dot(v, custom::dir_vec(x.seg_pos(x.total / 2), x.main())));
            num4 *= mathf::inverse_lerp(20.f, 50.f, rw::distance(b.attack_target_pos, x.main()));
            if (a.charging_attack > 0.f || (custom::dist_less(x.pos(1), b.attack_from_pos, 40.f) && num4 > 0.5f)) {
                if (num3 == 1.f)
                    a.charging_attack = mathf::min(1.f, a.charging_attack + (0.1f + 0.9f * num4) / 55.f);
                else
                    a.charging_attack = mathf::max(0.f, a.charging_attack - 0.033333335f);
            } else {
                a.charging_attack = mathf::max(0.f, a.charging_attack - 0.033333335f);
            }
            constexpr float d = 1.f, d2 = 1.9f;
            vec2 const pull = rw::clamp_magnitude(b.attack_from_pos - v * a.charging_attack * 100.f - x.pos(1), 40.f) / 40.f * d2 * num3;
            for (uint32_t k = 1; k < x.total; ++k) {
                float const t = mathf::inverse_lerp(0.f, float(x.total - 1), float(k));
                x.set_seg_vel(k, x.seg_vel(k) * mathf::lerp(1.f, 0.75f, num3 * mathf::inverse_lerp(0.25f, 0.75f, t)));
                x.add_seg_vel(k, pull * (1.f - t) + v * mathf::lerp(3.f, -6.f, t) * num3 * d * mathf::inverse_lerp(0.75f + 0.6f * a.charging_attack, 0.5f, t));
            }
            if (a.charging_attack > 0.5f) {
                x.x.crawl_sin += 0.8f * a.charging_attack;
                sin_movement_in_body(x, 0.f, 1.5f * mathf::inverse_lerp(0.5f, 0.75f, a.charging_attack), 0.4f, 0.4f, false);
            }
            if (a.charging_attack >= 1.f && a.dodge_delay < 1 && x.room.visual_contact(x.main(), x.fang_pos())) {
                a.charging_attack = 0.f;
                a.swish_dir = v;
                a.has_swish = 1;
                a.swish_counter = 6;
            }
        }

        // NeedleWorm.Fly: the wings push along the look direction, the body
        // is pulled towards a point some steps ahead on its path
        void fly(ctx& x, std::optional<ivec2> conn) {
            auto& w = x.w();
            auto& p = x.x;
            physics::set_go_through_floors(x.state, body(x.state, x.c), true);
            bool flag = false;
            ivec2 const dest = x.dest();
            vec2 target;
            if (!conn || (float_dist(x.tile(), dest) < 20.f && x.room.visual_contact(room_view::middle_of_tile(x.tile()), room_view::middle_of_tile(dest)))) {
                target = room_view::middle_of_tile(dest);
                flag = !conn;
            } else {
                target = room_view::middle_of_tile(*conn);
                ivec2 mc = *conn;
                for (int i = 0; i < 10; ++i) {
                    auto const nx = x.follow_path(mc);
                    if (!nx || !x.room.visual_contact(x.main(), room_view::middle_of_tile(*nx))) {
                        flag = true;
                        break;
                    }
                    target = room_view::middle_of_tile(*nx);
                    if (*nx == dest || *nx == mc)
                        break;
                    mc = *nx;
                }
            }
            if (room_view::get_tile_position(target) != dest) {
                vec2 const up = move_up_from_floor(x, room_view::get_tile_position(target));
                if (x.room.visual_contact(x.main(), up))
                    target = up;
            }
            if (p.stuck_at_same_pos > 0.25f && !x.room.get_tile(x.tile() + ivec2{0, 2}).solid() && !tile_in_enclosed_area(x, x.tile()))
                target += custom::deg_to_vec(mathf::lerp(-15.f, 15.f, x.rng.value())) * 80.f * p.stuck_at_same_pos * (1.f - p.really_stuck_at_same_pos);
            bool const attacking = !x.small && x.beh() == behavior::attack;
            vec2& get_to = w.needle_worm_data_get_get_to_look_dir(x.d);
            if (attacking) {
                auto const& b = x.big_ai();
                vec2 const from = rw::lerp(x.pos(1), b.attack_from_pos, mathf::inverse_lerp(500.f, 200.f, rw::distance(x.pos(1), b.attack_from_pos)));
                p.look_dir = custom::move_towards(p.look_dir, custom::dir_vec(from, b.attack_target_pos), 0.34f);
            } else {
                // CreatureLooker (ForcedLookCreature the creature it keeps
                // close to; else the creature it focuses on)
                dcon::creature_id look = x.small ? dcon::creature_id{} : x.big_ai().keep_close_to;
                if (!look)
                    look = x.ai().focus;
                if (look)
                    if (auto const* r = tracker::find(tracker::of(x.state, x.c), look); r && r->ticks_since_seen < 40) {
                        vec2 const at = r->visual && creature::here(x.state, look, x.room.id) ? creature::main_pos(x.state, look)
                                                                                            : room_view::middle_of_tile(tracker::best_guess(*r));
                        get_to = rw::lerp(get_to, custom::dir_vec(x.pos(1), at),
                            mathf::inverse_lerp(800.f, 300.f, rw::distance(x.pos(1), at)) * mathf::inverse_lerp(40.f, 20.f, float(r->ticks_since_seen)));
                    }
                if (!custom::dist_less(x.main(), target, 90.f * (1.f - p.extra_movement_force)))
                    get_to = custom::move_towards(get_to, custom::dir_vec(x.main(), target), 0.05f);
                else if (x.rng.value() < 0.033333335f)
                    get_to = rw::normalized(p.look_dir + custom::rnv(x.rng) * x.rng.value());
                get_to += custom::rnv(x.rng) * 0.07f;
                p.look_dir = custom::move_towards(p.look_dir, get_to, 0.14f);
            }
            float const k = x.small ? 0.7f : 1.f;
            // the wings' thrust
            float const d = 0.6f * (1.f - p.extra_movement_force) * (x.small ? 0.5f : 1.f);
            vec2 const a = rw::slerp(p.look_dir, vec2{0.f, -1.f}, 0.25f * (1.f - p.extra_movement_force) * (attacking ? 0.f : 1.f));
            x.vel(0) += a * (x.small ? 0.8f : 1.2f) * d / x.mass(0);
            x.vel(2) -= a * (x.small ? 0.8f : 1.2f) * 0.95f * d / x.mass(2);
            p.broken_line_of_sight = custom::lerp_and_tick(p.broken_line_of_sight, flag ? 1.f : 0.f, 0.09f, 0.025f);
            if (!flag && rw::distance(x.pos(1), room_view::middle_of_tile(dest)) < 60.f) {
                // at the destination: hovering about it
                x.vel(1) = rw::lerp(x.vel(1), rw::clamp_magnitude(target - x.pos(1), 40.f) / 4.f, 0.2f);
                x.vel(0) *= 0.9f;
                x.vel(2) *= 0.9f;
                p.at_dest_this_frame = 1;
            } else {
                float num = mathf::lerp(mathf::inverse_lerp(30.f, 200.f, rw::distance(x.pos(1), target)), 1.f, p.extra_movement_force)
                    * mathf::lerp(slow_fly_speed(x), 1.f, p.extra_movement_force);
                vec2 mom{0.f, 0.f};
                float tm = 0.f;
                for (uint32_t j = 0; j < x.n; ++j) {
                    mom += x.vel(j) * x.mass(j);
                    tm += x.mass(j);
                }
                mom /= tm;
                mom = rw::clamp_magnitude(mom * 15.f, 200.f);
                // about to fly into the terrain: back away from it
                if (x.proximity(x.main()) > 1 && !x.room.visual_contact(x.main(), x.main() + mom)) {
                    num = mathf::lerp(num, 1.f, 1.f - p.stuck_at_same_pos);
                    p.extra_movement_force = mathf::lerp(p.extra_movement_force, 1.f, 1.f - p.stuck_at_same_pos);
                    target = rw::lerp(target, x.main() - mom, 1.f - p.stuck_at_same_pos);
                }
                int32_t const prox = x.proximity(x.main());
                if (prox > 1 && prox < 5 && x.proximity(x.main() + custom::dir_vec(x.main(), target) * 60.f) < prox) {
                    ivec2 const here = room_view::get_tile_position(x.main());
                    ivec2 best = here;
                    float score = -float_max;
                    for (auto const dd : custom::eight_directions) {
                        if (x.room.get_tile(here + dd).solid())
                            continue;
                        float s = rw::dot(rw::normalized(x.main() - target), rw::normalized(x.main() - room_view::middle_of_tile(here + dd)));
                        if (s > 0.f) {
                            s += float(x.proximity(here + dd));
                            if (s > score) {
                                score = s;
                                best = here + dd;
                            }
                        }
                    }
                    if (score > 0.f) {
                        num = mathf::lerp(num, 1.f, 1.f - p.stuck_at_same_pos);
                        target = rw::lerp(target, room_view::middle_of_tile(best), 1.f - p.stuck_at_same_pos);
                    }
                }
                if (conn)
                    x.vel(0) += custom::dir_vec(x.main(), room_view::middle_of_tile(*conn)) * num * 5.f * p.extra_movement_force * k;
                else
                    x.vel(0) += custom::dir_vec(x.main(), target) * num * 5.f * p.extra_movement_force * k;
                physics::set_go_through_floors(x.state, body(x.state, x.c), p.stuck_at_same_pos > 0.5f || custom::dir_vec(x.pos(1), target).y < 0.f);
                for (uint32_t l = 0; l < x.n; ++l) {
                    float const t = mathf::inverse_lerp(0.f, float(x.n - 1), float(l));
                    x.vel(l) *= mathf::lerp(0.8f, 1.f, mathf::pow(t, 0.3f));
                    x.vel(l) += custom::dir_vec(x.pos(l), target) * mathf::lerp(3.f + p.segments_stuck_on_terrain, 0.5f, t) * num * k;
                }
                for (uint32_t m = 1; m < x.total / 2; ++m) {
                    float const f = mathf::inverse_lerp(0.f, float(x.total / 2 - 1), float(m));
                    vec2 const back = custom::dir_vec(x.seg_pos(m), x.seg_pos(m - 1));
                    vec2 const to = custom::dir_vec(x.seg_pos(m), target);
                    x.add_seg_vel(m, back * mathf::inverse_lerp(-1.f, 1.f, rw::dot(back, to)) * mathf::lerp(1.3f, 0.f, mathf::pow(f, 0.5f)) * num * k);
                }
            }
            // back into the room
            if (x.rng.value() < 0.033333335f) {
                if (x.pos(1).x < 0.f)
                    x.vel(1).x += x.rng.value() * 8.f;
                else if (x.pos(1).x > x.room.pixel_width())
                    x.vel(1).x -= x.rng.value() * 8.f;
                if (x.pos(1).y < 0.f)
                    x.vel(1).y += x.rng.value() * 8.f;
                else if (x.pos(1).y > x.room.pixel_height())
                    x.vel(1).y -= x.rng.value() * 8.f;
            }
            if (x.small)
                return;

            // BigNeedleWorm.Fly
            auto& at = x.atk();
            auto& b = x.big_ai();
            if (std::fabs(p.look_dir.x) > 0.65f && p.look_dir.y < 0.2f && (p.look_dir.x < 0.f) != (x.pos(0).x < x.tail.pos[0].x)
            && normal_flying_state(x.state, x.c)) {
                // turning about: the head one way, the tail the other
                float const s = mathf::sign(p.look_dir.x);
                x.vel(0).x += s * 5.f;
                x.pos(0).x += s;
                x.vel(x.n - 1).x -= s * 5.f;
                x.pos(x.n - 1).x -= s;
                for (uint32_t i = 0; i < x.nt; ++i) {
                    x.tail.pos[i].x -= s;
                    x.tail.vel[i].x -= s * 6.f / float(1 + i);
                }
            }
            if (b.attack_counter > 65)
                at.attack_ready = custom::lerp_and_tick(at.attack_ready, mathf::inverse_lerp(65.f, 85.f, float(b.attack_counter)), 0.f, 0.00625f);
            else
                at.attack_ready = mathf::max(0.f, at.attack_ready - 0.016666668f);
            auto const* prey = tracker::prey(tracker::of(x.state, x.c));
            if (x.beh() == behavior::attack && prey && prey->ticks_since_seen < 20 && at.attack_ready > 0.5f)
                attack_charge(x);
            else
                at.charging_attack = 0.f;
            // a rival's swish coming at it
            auto const f = x.ai().focus;
            if (is_big_worm(x.state, f) && creature::here(x.state, f, x.room.id) && consious(x.state, x.c) && at.lame_counter < 1 && p.flying > 0.5f
            && at.charging_attack < 0.9f && !at.has_swish && !at.in_wall && !at.impale) {
                auto const* r = tracker::find(tracker::of(x.state, x.c), f);
                auto const& oa = w.needle_worm_data_get_attack(data(x.state, f));
                if (r && r->visual && (oa.has_swish || oa.charging_attack > 0.9f)) {
                    vec2 const o0 = w.body_chunk_get_pos(chunk(x.state, f, 0));
                    vec2 const v = oa.has_swish ? oa.swish_dir : custom::dir_vec(w.body_chunk_get_pos(chunk(x.state, f, 1)), o0);
                    if (custom::dist_less(x.main(), o0 + v * 140.f, 140.f) && dodge(x, o0, v)) {
                        at.charging_attack = 0.f;
                        at.attack_ready = mathf::min(0.75f, at.attack_ready);
                        at.lame_counter = 14;
                    }
                }
            }
        }

        // NeedleWorm.Act
        void act(ctx& x) {
            auto& p = x.x;
            if (x.small)
                small_ai_update(x);
            else
                big_ai_update(x);
            p.flying_this_frame = 1;
            p.extra_movement_force = custom::lerp_and_tick(p.extra_movement_force,
                0.25f * p.broken_line_of_sight + 0.25f * p.segments_stuck_on_terrain + 0.5f * p.stuck_at_same_pos, 0.07f, 0.025f);
            float const g = x.gravity();
            for (uint32_t i = 0; i < x.total; ++i) {
                float const v = mathf::inverse_lerp(0.f, float(x.total - 1), float(i));
                if (x.narrow(x.seg_pos(i)))
                    x.add_seg_vel_y(i, g);
                else
                    x.add_seg_vel_y(i, std::sin(mathf::inverse_lerp(0.f, 0.5f, v) * mathf::pi) * 1.6f * p.flying);
            }
            auto& prev = x.w().needle_worm_data_get_prev_positions(x.d);
            if (++p.last_pos_counter > 5) {
                p.last_pos_counter = 0;
                for (int j = 9; j > 0; --j)
                    prev.p[j] = prev.p[j - 1];
                prev.p[0] = x.main();
            }
            p.stuck_at_same_pos = custom::lerp_and_tick(p.stuck_at_same_pos,
                mathf::inverse_lerp(100.f, 40.f, rw::distance(x.main(), prev.p[9])) * slow_fly_speed(x) * 2.f, 0.07f, 0.025f);
            if (p.stuck_at_same_pos > 0.99f)
                p.really_stuck_at_same_pos = mathf::min(1.f, p.really_stuck_at_same_pos + 0.011111111f);
            else if (p.stuck_at_same_pos < 0.5f)
                p.really_stuck_at_same_pos = 0.f;
            else
                p.really_stuck_at_same_pos = mathf::max(0.f, p.really_stuck_at_same_pos - 0.011111111f);
            if (p.really_stuck_at_same_pos > 0.5f) {
                for (auto& m : x.tail.rope_mode)
                    m = 0;
                p.extra_movement_force = 1.f;
                for (uint32_t l = 0; l < x.total; ++l)
                    if (x.rng.value() < 0.2f)
                        x.add_seg_vel(l, custom::rnv(x.rng) * x.rng.value() * 11.f * mathf::inverse_lerp(0.5f, 1.f, p.really_stuck_at_same_pos));
            }
            // (shortcuts aren't steps of the fields: the pipes are entered by
            // dens.hpp when it heads for one)
            auto const conn = x.follow_path(x.tile());
            if (!conn) {
                x.vel(0) *= 0.6f;
                if (p.flying > 0.5f) {
                    p.flying_this_frame = 1;
                    fly(x, std::nullopt);
                }
                return;
            }
            ivec2 const t = x.tile();
            if ((x.room.inside(t.x, t.y) && x.narrow(t)) || x.narrow(*conn)) {
                crawl(x, *conn);
                p.flying_this_frame = 0;
                return;
            }
            fly(x, conn);
        }

        // NeedleWorm.AfterUpdate
        void after_update(ctx& x) {
            auto& p = x.x;
            p.flying = custom::lerp_and_tick(p.flying, p.flying_this_frame ? 1.f : 0.f, 0.11f, 0.033333335f);
            if (!consious(x.state, x.c) || p.at_dest_this_frame) {
                p.stuck_at_same_pos = 0.f;
                p.broken_line_of_sight = 0.f;
                p.segments_stuck_on_terrain = 0.f;
                p.really_stuck_at_same_pos = 0.f;
            }
        }

        // NeedleWorm.Update after PhysicalObject.Update: the tail and the
        // ropes holding the segments together, the body's stiffness
        void worm_update(ctx& x) {
            auto& w = x.w();
            auto& p = x.x;
            p.at_dest_this_frame = 0;
            if (p.no_shortcuts > 0)
                --p.no_shortcuts;
            p.screaming = mathf::max(0.f, p.screaming - 0.014285714f);
            w.physical_object_set_surface_friction(body(x.state, x.c), consious(x.state, x.c) ? 0.8f : 0.1f);
            int32_t num = 0;
            for (uint32_t i = 0; i < x.n; ++i)
                if (w.body_chunk_get_contact_point(x.ch[i]) != ivec2{0, 0})
                    ++num;
            auto& tail = x.tail;
            for (uint32_t l = 0; l < x.total; ++l)
                if (tail.rope_mode[l] < 10 && !x.room.get_tile(x.seg_pos(l)).solid()
                && (l == x.total - 1 || x.room.visual_contact(x.seg_pos(l), x.seg_pos(l + 1))))
                    ++tail.rope_mode[l];
            x.vel(1) += custom::rnv(x.rng) * p.flying * x.rng.value() * 0.2f;
            float num3 = 200.f;
            bool flag = false;
            vec2 gv = vec2{0.f, -x.gravity()} * mathf::inverse_lerp(60.f, 0.f, float(p.no_shortcuts));
            if (!x.small && x.atk().charging_attack > 0.5f) {
                auto const& b = x.big_ai();
                gv = rw::lerp(gv, custom::dir_vec(b.attack_from_pos, b.attack_target_pos) * -6.f, mathf::inverse_lerp(0.5f, 0.8f, x.atk().charging_attack));
            }
            float const air_friction = w.physical_object_get_air_friction(body(x.state, x.c));
            for (uint32_t m = 0; m < x.nt; ++m) {
                uint32_t const s = x.n + m;
                tail.last_pos[m] = tail.pos[m];
                tail.pos[m] += tail.vel[m];
                if (!custom::dist_less(x.pos(x.n - 1), tail.pos[m], num3) && !x.room.visual_contact(tail.pos[m], x.seg_pos(s - 1)))
                    tail.rope_mode[s] = 0;
                if (!flag && tail.rope_mode[s] >= 10) {
                    if (x.room.point_submerged(tail.pos[m])) {
                        tail.vel[m] *= 0.8f;
                        tail.vel[m] += vec2{0.f, 0.3f};
                    } else {
                        tail.vel[m] *= air_friction;
                        tail.vel[m] += gv;
                    }
                } else {
                    // pulled straight towards the segment before it
                    vec2 const dir = custom::dir_vec(tail.pos[m], x.seg_pos(s - 1));
                    tail.vel[m] *= 0.5f;
                    x.add_seg_vel(s - 1, dir * 2.f);
                    tail.vel[m] += dir * 2.f;
                    tail.pos[m] += dir * 2.f;
                    if (!flag && !x.room.visual_contact(tail.pos[m], x.seg_pos(s - 1)))
                        flag = true;
                }
                if (!flag && tail.rope_mode[s] >= 10) {
                    if (point_terrain_collision(x.room, tail.pos[m], tail.last_pos[m], tail.vel[m], x.seg_rad_collision(s))) {
                        tail.vel[m] *= 0.5f;
                        if (consious(x.state, x.c) && m > 0)
                            tail.vel[m] += custom::dir_vec(tail.pos[m], tail.pos[m - 1]) * p.extra_movement_force;
                        ++num;
                    }
                }
                num3 += x.seg_rad_rope(s) * 5.f;
            }
            auto& ropes = w.needle_worm_data_get_ropes(x.d).r;
            for (uint32_t k = 0; k + 1 < x.total; ++k) {
                float const len = x.seg_rad_rope(k) + x.seg_rad_rope(k + 1);
                float const ratio = x.seg_mass(k + 1) / (x.seg_mass(k) + x.seg_mass(k + 1));
                auto& r = ropes[k];
                if ((r.bend_count > 2 || !custom::dist_less(x.seg_pos(k), x.seg_pos(k + 1), len * 4.f)) && !x.room.visual_contact(x.seg_pos(k), x.seg_pos(k + 1)))
                    tail.rope_mode[k] = 0;
                if (tail.rope_mode[k] >= 10) {
                    // a rope wrapping around the corners in between
                    rot::update_rope(r, x.room, x.seg_pos(k), x.seg_pos(k + 1));
                    float const total = r.total_length;
                    if (total > len) {
                        vec2 a = custom::dir_vec(x.seg_pos(k), rot::rope_a_connect(r));
                        x.add_seg_vel(k, a * (total - len) * ratio);
                        x.add_seg_pos(k, a * (total - len) * ratio);
                        a = custom::dir_vec(x.seg_pos(k + 1), rot::rope_b_connect(r));
                        x.add_seg_vel(k + 1, a * (total - len) * (1.f - ratio));
                        x.add_seg_pos(k + 1, a * (total - len) * (1.f - ratio));
                    }
                } else {
                    r.bend_count = 0;
                    rot::update_rope(r, x.room, x.seg_pos(k), x.seg_pos(k + 1));
                    float const dist = rw::distance(x.seg_pos(k), x.seg_pos(k + 1));
                    if (dist > len) {
                        vec2 const a = custom::dir_vec(x.seg_pos(k), x.seg_pos(k + 1));
                        x.add_seg_vel(k, a * (dist - len) * ratio);
                        x.add_seg_pos(k, a * (dist - len) * ratio);
                        x.add_seg_vel(k + 1, a * -1.f * (dist - len) * (1.f - ratio));
                        x.add_seg_pos(k + 1, a * -1.f * (dist - len) * (1.f - ratio));
                    }
                }
            }
            // the body's stiffness: every segment pushed away from the ones
            // two to four further along
            if (p.really_stuck_at_same_pos < 1.f) {
                for (uint32_t s = 2; s < 5; ++s)
                    for (uint32_t i = 0; i + s < x.total; ++i) {
                        float f = mathf::inverse_lerp(0.f, float(x.total - s), float(i));
                        f = mathf::lerp(3.f, 1.5f, std::sin(f * mathf::pi)) / (0.5f + float(s) * 0.25f);
                        f *= 1.f - p.really_stuck_at_same_pos;
                        float const ratio = x.seg_mass(i + s) / (x.seg_mass(i) + x.seg_mass(i + s));
                        x.add_seg_vel(i, custom::dir_vec(x.seg_pos(i + s), x.seg_pos(i)) * f * ratio);
                        x.add_seg_vel(i + s, custom::dir_vec(x.seg_pos(i), x.seg_pos(i + s)) * f * (1.f - ratio));
                    }
            }
            p.segments_stuck_on_terrain = custom::lerp_and_tick(p.segments_stuck_on_terrain, float(num) / float(x.total), 0.07f, 0.025f);
        }

        //
        // BigNeedleWorm: the fang
        //

        // the chunk the fang holds, if it still can
        dcon::body_chunk_id impale_chunk(ctx& x) {
            auto const& a = x.atk();
            if (!a.impale || !creature::here(x.state, a.impale, x.room.id) || a.impale_chunk >= creature::chunk_count_of(x.state, a.impale))
                return dcon::body_chunk_id{};
            return creature::chunk_of(x.state, a.impale, a.impale_chunk);
        }

        // BigNeedleWorm.AttachToChunk: stuck in a creature, wriggling, until
        // it pulls free
        void attach_to_chunk(ctx& x) {
            auto& w = x.w();
            auto& a = x.atk();
            auto const k = impale_chunk(x);
            if (!k) {
                a.impale = dcon::creature_id{};
                return;
            }
            a.stuck_time += 0.0035714286f;
            float const num = custom::lerp_map(a.stuck_time, 0.4f, 1.f, 1.f, 0.5f);
            a.attack_ready = 1.f;
            x.x.flying_this_frame = a.stuck_time > 0.5f && consious(x.state, x.c) ? 1 : 0;
            vec2& kp = w.body_chunk_get_pos(k);
            vec2& kv = w.body_chunk_get_vel(k);
            float const km = w.body_chunk_get_mass(k);
            for (uint32_t i = 0; i < x.n; ++i) {
                float const d = custom::lerp_map(float(i), 1.f, float(x.n - 1), 1.f, 0.1f) * num;
                vec2 const v = custom::dir_vec(x.pos(i), kp) * (rw::distance(x.pos(i), kp) - a.impale_distances[i]);
                float const ratio = km / (x.mass(i) + km);
                x.vel(i) += v * ratio * d;
                x.pos(i) += v * ratio * d;
                kv -= v * (1.f - ratio) * d;
                kp -= v * (1.f - ratio) * d;
            }
            if (consious(x.state, x.c)) {
                for (uint32_t j = 1; j < x.total; ++j)
                    x.add_seg_vel(j, custom::rnv(x.rng) * (a.stuck_time * x.rng.value() * x.rng.value() * x.rng.value() * 18.f));
                x.x.crawl_sin += 0.2f + 0.4f * num;
                sin_movement_in_body(x, 2.5f * a.stuck_time, 2.5f * a.stuck_time, 0.6f, 0.6f, false);
            }
            if (a.stuck_time > 1.f || (a.stuck_time > 0.2f && (creature::held(x.state, x.c) || !x.room.visual_contact(x.main(), kp)))) {
                a.stuck_time = 0.f;
                x.vel(0) += custom::dir_vec(kp, x.main()) * 9.f;
                x.vel(1) += custom::dir_vec(kp, x.main()) * 7.f;
                a.impale = dcon::creature_id{};
            }
        }

        // BigNeedleWorm.StuckInChunk
        void stuck_in_chunk(ctx& x) {
            if (!impale_chunk(x) || x.main().y < -100.f) {
                x.atk().impale = dcon::creature_id{};
                return;
            }
            // (AttachToChunk(true) and (false): no rotation chunks here)
            attach_to_chunk(x);
            attach_to_chunk(x);
        }

        // BigNeedleWorm.StuckInWall: the fang stuck in the terrain, the
        // body wriggling free
        void stuck_in_wall(ctx& x) {
            auto& a = x.atk();
            auto& p = x.x;
            a.attack_ready = 1.f;
            p.flying_this_frame = a.stuck_time > 0.5f && consious(x.state, x.c) ? 1 : 0;
            float const num = custom::lerp_map(a.stuck_time, 0.3f, 1.f, 1.f, 0.5f);
            float along = 0.f;
            for (uint32_t i = 0; i < x.total / 2; ++i) {
                float const t = mathf::inverse_lerp(0.f, float(x.total / 2 - 1), float(i));
                if (i > 0)
                    along += x.seg_rad_rope(i - 1) + x.seg_rad_rope(i);
                x.set_seg_vel(i, x.seg_vel(i) * mathf::lerp(1.f, t, num * 0.9f));
                x.set_seg_pos(i, rw::lerp(x.seg_pos(i), a.stuck_in_wall_pos - a.stuck_dir * along, (1.f - t) * num * 0.9f));
            }
            if (consious(x.state, x.c)) {
                a.stuck_time += 0.0125f;
                for (uint32_t j = 1; j < x.total; ++j) {
                    float const r1 = x.rng.value();
                    float const r2 = x.rng.value();
                    x.add_seg_vel(j, a.stuck_dir * mathf::lerp(1.f, -7.f, r1 * r2)
                        + custom::rnv(x.rng) * (x.rng.value() * x.rng.value() * x.rng.value() * 18.f));
                }
                p.crawl_sin += 0.2f + 0.4f * num;
                sin_movement_in_body(x, 2.5f * a.stuck_time, 2.5f * a.stuck_time, 0.6f, 0.6f, false);
            } else {
                a.stuck_time += x.rng.value() / 150.f;
            }
            p.stuck_at_same_pos = 0.f;
            p.broken_line_of_sight = 0.f;
            p.segments_stuck_on_terrain = 0.f;
            p.really_stuck_at_same_pos = 0.f;
            if (a.stuck_time > 1.f || creature::held(x.state, x.c)) {
                if (consious(x.state, x.c)) {
                    x.vel(0) -= a.stuck_dir * 17.f * p.flying;
                    x.vel(1) -= a.stuck_dir * 19.f * p.flying;
                }
                a.stuck_time = 0.f;
                a.in_wall = 0;
                a.lame_counter = 7;
            }
        }

        // SharedPhysics.TraceProjectileAgainstBodyChunks (layer 1, rad 1)
        // with BigNeedleWorm.HitThisObject: the first creature chunk the
        // segment from `a` to `b` touches (not a young, not a leech), and
        // where it enters it
        struct chunk_hit {
            dcon::creature_id who;
            uint32_t chunk = 0;
            vec2 point{0.f, 0.f};
        };
        std::optional<chunk_hit> trace_chunks(ctx& x, vec2 a, vec2 b) {
            auto const& w = x.w();
            vec2 const dv = b - a;
            float const len = rw::magnitude(dv);
            if (len <= 0.f)
                return std::nullopt;
            vec2 const u = dv / len;
            std::optional<chunk_hit> out;
            float best = float_max;
            rooms::for_each_creature(x.state, x.room.id, [&](dcon::creature_id o) {
                if (o == x.c || !creature::here(x.state, o, x.room.id) || is_small_worm(x.state, o) || creature::type_of(w, o) == creature::type::leech)
                    return;
                uint32_t const nk = creature::chunk_count_of(x.state, o);
                for (uint32_t i = 0; i < nk; ++i) {
                    auto const k = creature::chunk_of(x.state, o, i);
                    vec2 const cp = w.body_chunk_get_pos(k);
                    float const r = w.body_chunk_get_rad(k) + 1.f;
                    vec2 const rel_pos = cp - a;
                    float const proj = rw::dot(rel_pos, u);
                    float const perp2 = rw::sqr_magnitude(rel_pos) - proj * proj;
                    if (perp2 > r * r)
                        continue;
                    float t = proj - std::sqrt(r * r - perp2);
                    if (t < 0.f) {
                        if (rw::sqr_magnitude(rel_pos) < r * r)
                            t = 0.f;
                        else
                            continue;
                    }
                    if (t > len || t >= best)
                        continue;
                    best = t;
                    out = chunk_hit{o, i, a + u * t};
                }
            });
            return out;
        }

        // BigNeedleWorm.Swish: the dash along the fang, into a wall or a creature
        void swish(ctx& x) {
            auto& a = x.atk();
            auto& p = x.x;
            p.flying_this_frame = 0;
            p.flying = 0.f;
            p.look_dir = a.swish_dir;
            a.dodge_delay = 30;
            --a.swish_counter;
            if (a.swish_counter < 1 || !a.has_swish) {
                for (uint32_t i = 0; i < x.total; ++i)
                    x.set_seg_vel(i, rw::clamp_magnitude(x.seg_vel(i) * 0.75f, 20.f));
                a.swish_counter = 0;
                a.has_swish = 0;
                a.lame_counter = 7;
                return;
            }
            float const num = 90.f + 90.f * std::sin(mathf::inverse_lerp(1.f, 5.f, float(a.swish_counter)) * mathf::pi);
            a.attack_ready = 1.f;
            vec2 const v = a.swish_dir;
            vec2 const fang = x.pos(0) + v * fang_length;
            vec2 to = x.pos(0) + v * (fang_length + num);
            vec2 const from = x.last_pos(0) + v * fang_length;
            vec2 const end = x.pos(0) + v * (fang_length + num);
            auto th = exact_terrain_ray_trace(x.room, from, end);
            auto ch = trace_chunks(x, from, end);
            if (th && ch) {
                if (rw::distance(from, th->pos) < rw::distance(from, ch->point))
                    ch.reset();
                else
                    th.reset();
            }
            if (th) {
                to = th->pos - v * fang_length * 0.7f;
                // (the WaterDrips and the screen shake aren't ported)
                if (rw::dot(v, th->dir) > 0.73f) {
                    // head on: the fang sticks in the wall
                    a.in_wall = 1;
                    a.stuck_in_wall_pos = to;
                    a.stuck_dir = a.swish_dir;
                    a.swish_counter = 0;
                    a.has_swish = 0;
                    stun(x, 60);
                    a.stuck_time = 0.f;
                } else {
                    // glancing: it bounces off
                    a.swish_counter = 0;
                    a.has_swish = 0;
                    a.lame_counter = 30;
                }
            } else if (ch) {
                // into a creature
                to = ch->point - v * fang_length * 0.7f;
                auto const k = creature::chunk_of(x.state, ch->who, ch->chunk);
                auto& w = x.w();
                a.stuck_dir = rw::slerp(a.swish_dir, custom::dir_vec(to, w.body_chunk_get_pos(k)), 0.4f);
                a.swish_counter = 0;
                a.has_swish = 0;
                a.impale = ch->who;
                a.impale_chunk = uint8_t(ch->chunk);
                float along = -fang_length / 4.f;
                for (uint32_t i = 0; i < x.n; ++i) {
                    if (i == 1)
                        along += fang_length / 4.f;
                    if (i > 0)
                        along += x.seg_rad_rope(i - 1) + x.seg_rad_rope(i) + fang_length / 4.f;
                    a.impale_distances[i] = rw::distance(to - v * along, w.body_chunk_get_pos(k));
                }
                // a noodlefly knows who stabbed it
                if (is_worm(x.state, ch->who))
                    w.needle_worm_data_get_ai(data(x.state, ch->who)).attacker = x.c;
                creature::violence_on(x.state, ch->who, ch->chunk, vec2{0.f, 0.f}, 1.22f, 60.f, creature::damage_type::stab, tmpl::big_needle_worm);
                float const km = w.body_chunk_get_mass(k);
                w.body_chunk_get_vel(k) += v * 12.f / km;
                w.body_chunk_get_pos(k) += v * 7.f / km;
                for (uint32_t l = 0; l < x.total; ++l)
                    x.set_seg_vel(l, rw::clamp_magnitude(x.seg_vel(l), 6.f));
                a.stuck_time = 0.f;
            }
            a.swish_add = to - fang;
            a.has_swish_add = 1;
            for (uint32_t m = 0; m < x.total; ++m) {
                float const t = mathf::inverse_lerp(0.f, float(x.total - 1), float(m));
                if (m < x.n)
                    x.set_seg_pos(m, x.seg_pos(m) + (to - fang));
                x.add_seg_vel(m, v * mathf::lerp(6.f, -0.2f, t));
            }
        }

        // BigNeedleWorm.Update
        void big_update(ctx& x) {
            auto& a = x.atk();
            auto& p = x.x;
            worm_update(x);
            if (a.dodge_delay > 0)
                --a.dodge_delay;
            if (a.lame_counter > 0)
                --a.lame_counter;
            else
                p.flying_this_frame = 0;
            if (a.impale)
                stuck_in_chunk(x);
            else if (a.in_wall)
                stuck_in_wall(x);
            else if (a.lame_counter < 1) {
                if (a.swish_counter > 0)
                    swish(x);
                else if (consious(x.state, x.c))
                    act(x);
                else {
                    a.attack_ready = mathf::max(0.f, a.attack_ready - 0.016666668f);
                    a.charging_attack = 0.f;
                }
            }
            if (a.has_swish_add)
                for (uint32_t i = 0; i < x.nt; ++i)
                    x.tail.pos[i] += a.swish_add * mathf::pow(mathf::inverse_lerp(float(x.nt - 1), 0.f, float(i)), 0.4f);
            a.has_swish_add = 0;
            after_update(x);
            if (a.impale)
                attach_to_chunk(x);
            // the fang out: what it pokes is pricked
            if (a.attack_ready > 0.8f && x.rng.value() < 0.33333334f && !a.has_swish && !a.in_wall && !a.impale
            && rw::dot(custom::dir_vec(x.last_pos(0), x.main()), custom::dir_vec(x.pos(1), x.main())) > -0.2f) {
                vec2 const fp = x.fang_pos();
                vec2 const dir = custom::dir_vec(x.pos(1), x.main());
                std::optional<std::pair<dcon::creature_id, uint32_t>> hits[8];
                uint32_t nh = 0;
                rooms::for_each_creature(x.state, x.room.id, [&](dcon::creature_id o) {
                    if (o == x.c || !creature::here(x.state, o, x.room.id) || creature::type_of(x.w(), o) == creature::type::leech)
                        return;
                    uint32_t const nk = creature::chunk_count_of(x.state, o);
                    for (uint32_t k = 0; k < nk && nh < 8; ++k) {
                        auto const ck = creature::chunk_of(x.state, o, k);
                        if (custom::dist_less(x.w().body_chunk_get_pos(ck), fp, x.w().body_chunk_get_rad(ck)))
                            hits[nh++] = std::pair{o, k};
                    }
                });
                for (uint32_t i = 0; i < nh; ++i) {
                    auto const [o, k] = *hits[i];
                    if (!creature::live(x.state, o))
                        continue;
                    if (is_worm(x.state, o))
                        x.w().needle_worm_data_get_ai(data(x.state, o)).attacker = x.c;
                    creature::violence_on(x.state, o, k, dir * 1.6f, 0.05f, 30.f, creature::damage_type::stab, tmpl::big_needle_worm);
                    x.vel(0) -= dir * 1.6f / x.mass(0);
                }
            }
        }

        //
        // SmallNeedleWorm
        //

        // SmallNeedleWorm.MotherAttachable
        bool mother_attachable(ctx& x) {
            auto const m = mother_of(x);
            return m && x.w().needle_worm_data_get_attack(data(x.state, m)).charging_attack < 0.1f;
        }

        void set_collide_with_terrain(ctx& x, bool v) {
            for (uint32_t i = 0; i < x.n; ++i)
                x.w().body_chunk_set_collide_with_terrain(x.ch[i], v ? 1 : 0);
        }

        // SmallNeedleWorm.HangOnMom: hanging from her tail segment
        void hang_on_mom(ctx& x) {
            auto& w = x.w();
            auto const m = mother_of(x);
            auto const held_mom = creature::grasped(w, x.c, 0);
            bool const ct = w.body_chunk_get_collide_with_terrain(x.ch[0]) != 0;
            // (and never into a pipe with her: dens.hpp takes what it holds for prey)
            if (!mother_attachable(x) || !consious(x.state, x.c) || creature::held(x.state, x.c) || held_mom != m || w.creature_get_going_home(x.c)
            || (!x.room.get_tile(x.main()).solid() && ct && x.rng.value() < 1.f / (consious(x.state, m) ? 400.f : 70.f))) {
                creature::release_grasp(x.state, x.c, 0);
                set_collide_with_terrain(x, true);
                return;
            }
            auto& mt = w.needle_worm_data_get_tail(data(x.state, m));
            uint32_t const seg = x.young().mom_tail_segment;
            if (!custom::dist_less(x.main(), mt.pos[seg], 30.f)) {
                vec2 const a = custom::dir_vec(x.main(), mt.pos[seg]) * (rw::distance(x.main(), mt.pos[seg]) - 30.f);
                x.main() += a * 0.95f;
                x.vel(0) += a * 0.95f;
                mt.pos[seg] -= a * 0.05f;
                mt.vel[seg] -= a * 0.05f;
            }
            set_collide_with_terrain(x, mt.rope_mode[chunk_count(false) + seg] > 0
                && (custom::dist_less(x.main(), mt.pos[seg], 50.f) || x.room.visual_contact(x.main(), mt.pos[seg])));
            if (x.rng.value() < 0.011111111f)
                x.x.flying = x.rng.value() < 0.5f ? 1.f : x.rng.value();
        }

        // SmallNeedleWorm.Act: and now and then onto mother's tail
        void small_act(ctx& x) {
            act(x);
            auto& w = x.w();
            if (!(x.rng.value() < 0.05882353f) || creature::grasped_object(w, x.c, 0) || !mother_attachable(x) || w.creature_get_going_home(x.c))
                return;
            auto const m = mother_of(x);
            if (!normal_flying_state(x.state, m))
                return;
            auto const& mt = w.needle_worm_data_get_tail(data(x.state, m));
            int32_t const last = int32_t(tail_count(false)) - 2;
            for (int32_t i = last; i >= 0; --i) {
                if (!(x.rng.value() < mathf::inverse_lerp(0.f, float(last), float(i))) || !custom::dist_less(x.main(), mt.pos[i], 25.f))
                    continue;
                bool free = true;
                rooms::for_each_creature(x.state, x.room.id, creature::type::needle_worm, [&](dcon::creature_id o) {
                    if (o != x.c && is_small(x.state, o) && creature::grasped(w, o, 0) == m
                    && w.needle_worm_data_get_young(data(x.state, o)).mom_tail_segment == uint8_t(i))
                        free = false;
                });
                if (free) {
                    x.young().mom_tail_segment = uint8_t(i);
                    creature::grab(x.state, x.c, 0, m, chunk_count(false) - 1);
                    return;
                }
            }
        }

        // SmallNeedleWorm.Update
        void small_update(ctx& x) {
            auto& w = x.w();
            auto& y = x.young();
            worm_update(x);
            for (uint32_t i = 0; i < x.total; ++i)
                x.add_seg_vel(i, custom::rnv(x.rng) * 4.f * mathf::pow(x.x.screaming, 0.2f));
            x.x.flying_this_frame = 0;
            if (creature::grasped_object(w, x.c, 0))
                hang_on_mom(x);
            else if (consious(x.state, x.c))
                small_act(x);
            bool const held = creature::held(x.state, x.c);
            if (!w.creature_get_dead(x.c) && held && y.scream_counter == 0.f)
                y.scream_counter = 0.01f;
            if (y.scream_counter > 0.f && !y.has_screamed) {
                float const prev = y.scream_counter;
                y.scream_counter += 0.005263158f;
                if (prev <= 0.2f && y.scream_counter > 0.2f)
                    creature::die(x.state, x.c);
                if (y.scream_counter > 1.f)
                    scream(x);
                else if (std::floor(prev * 4.f) != std::floor(y.scream_counter * 4.f) && held)
                    small_scream(x, true);
                if (auto const m = mother_of(x)) {
                    auto const cc = closest_creature(x);
                    auto& keep = w.needle_worm_data_get_big_ai(data(x.state, m)).keep_close_to;
                    if (!keep || (cc && x.rng.value() < 0.025f))
                        keep = cc;
                }
            }
            after_update(x);
        }

        // SocialEventRecognizer: the attack on a noodlefly (or on an
        // adult's young) lowers the adult's like of the attacker
        void social_event(sys::game_state& state, dcon::creature_id victim, dcon::creature_id attacker, float damage, bool was_dead, bool killed) {
            if (!creature::live(state, attacker) || attacker == victim)
                return;
            dcon::creature_id adult = victim;
            if (is_small(state, victim)) {
                adult = state.world.needle_worm_data_get_young(data(state, victim)).mother;
                if (!is_big_worm(state, adult) || state.world.creature_get_dead(adult))
                    return;
            }
            if (adult == attacker)
                return;
            float num = killed ? 0.9f : damage >= 0.5f ? 0.6f : 0.2f;
            if (was_dead)
                num /= 3.f;
            if (adult == victim)
                num /= 2.f;
            if (auto* e = social_of(state, adult, attacker, true)) {
                e->temp_like = mathf::clamp(e->temp_like - num, -1.f, 1.f);
                e->like = mathf::clamp(e->like - num * 0.1f, -1.f, 1.f);
            }
        }
    }

    char const* to_string(variant v) noexcept {
        switch (v) {
        case variant::big: return "big";
        case variant::small: return "small";
        }
        return "?";
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 at, variant v) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::needle_worm);
        auto const d = data(state, c);
        auto rng = rw::random(state.seed, 0, uint32_t(c.value));
        int32_t seed = int32_t(rng.next() & 0x7FFFFFFFu);
        if (state.spawn_seed >= 0)
            seed = state.spawn_seed;
        auto const view = world::get_room_view(state, room);
        // never inside terrain
        if (view.get_tile(at).solid()) {
            ivec2 const t0 = room_view::get_tile_position(at);
            for (int r = 1; r < 8 && view.get_tile(at).solid(); ++r)
                for (int dy = -r; dy <= r; ++dy)
                    for (int dx = -r; dx <= r; ++dx)
                        if (view.get_tile(at).solid() && !view.get_tile(t0 + ivec2{dx, dy}).solid())
                            at = room_view::middle_of_tile(t0 + ivec2{dx, dy});
        }
        bool const small = v == variant::small;
        physics::object_desc desc;
        desc.air_friction = 0.999f;
        desc.gravity = 0.9f;
        desc.bounce = 0.3f;
        desc.surface_friction = 0.4f;
        desc.water_friction = 0.96f;
        desc.buoyancy = 1.05f;
        auto const o = physics::create_object(state, room, desc);
        // NeedleWorm constructor: the chunks thickest a third of the way
        // down, a push connection between every pair
        uint32_t const n = chunk_count(small);
        float const num = small ? 0.7f : 1.f;
        dcon::body_chunk_id ch[max_chunks];
        float rads[max_chunks];
        for (uint32_t i = 0; i < n; ++i) {
            float const f = mathf::inverse_lerp(0.f, float(n - 1), float(i));
            float const t = mathf::lerp(0.6f, mathf::clamp01(std::sin(mathf::pow(f, 0.5f) * mathf::pi)), 0.5f + 0.5f * f);
            rads[i] = mathf::lerp(2.f, 5.f, t) * num;
            ch[i] = physics::add_chunk(state, o, at + vec2{0.f, -float(i) * 4.f}, rads[i], mathf::lerp(0.05f, 0.15f, t) * num);
        }
        for (uint32_t j = 0; j < n; ++j)
            for (uint32_t k = j + 1; k < n; ++k)
                physics::add_connection(state, o, ch[j], ch[k], rads[j] + rads[k], physics::connection_type::push, 1.f, -1.f);
        w.try_create_creature_body(c, o);
        auto& p = pl(state, c);
        p.variant = v;
        p.flying = 0.f;
        p.screaming = 0.f;
        p.extra_movement_force = 0.f;
        p.stuck_at_same_pos = 0.f;
        p.really_stuck_at_same_pos = 0.f;
        p.broken_line_of_sight = 0.f;
        p.segments_stuck_on_terrain = 0.f;
        p.crawl_sin = 0.f;
        p.look_dir = custom::rnv(rng);
        p.no_shortcuts = 0;
        p.fly_speed = 0.f;
        p.flying_this_frame = 0;
        p.at_dest_this_frame = 0;
        p.last_pos_counter = 0;
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_set_grasps(c, creature::grasp_pair{});
        w.creature_set_behavior(c, uint8_t(behavior::idle));
        w.creature_set_current_utility(c, 0.f);
        ivec2 const tile = room_view::get_tile_position(at);
        w.creature_set_destination(c, tile);
        tracker::clear(state, c);
        w.needle_worm_data_set_seed(d, seed);
        w.needle_worm_data_set_get_to_look_dir(d, p.look_dir);
        prev_positions pp;
        for (auto& q : pp.p)
            q = at;
        w.needle_worm_data_set_prev_positions(d, pp);
        w.needle_worm_data_set_tail(d, tail_state{});
        connection_ropes ropes;
        for (auto& r : ropes.r) {
            r = world::rope_state{};
            r.a = r.b = r.last_a = r.last_b = at;
            r.thickness = 0.1f;
        }
        w.needle_worm_data_set_ropes(d, ropes);
        w.needle_worm_data_set_attack(d, attack_state{});
        w.needle_worm_data_set_young(d, small_state{});
        ai_state ai;
        ai.idle_pos = tile;
        ai.last_assigned_idle_pos = tile;
        w.needle_worm_data_set_ai(d, ai);
        big_ai_state b;
        b.target_chunk = rng.range(0, 100);
        b.attack_from_pos = at;
        b.attack_target_pos = at;
        w.needle_worm_data_set_big_ai(d, b);
        w.needle_worm_data_set_social(d, social_memory{});
        {
            ctx x(state, c, view);
            reset_tail(x, at + vec2{0.f, -float(n) * 4.f});
        }
        needle_worm_graphics::init(state, c);
        needle_worm_graphics::reset_positions(state, c);
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        creature::release_target(state, c);
        creature::erase(state, c);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        // Creature.Update + PhysicalObject.Update
        creature::stun_countdown(state, c);
        physics::update_object(state, room, body(state, c), nullptr, nullptr);
        ctx x(state, c, room);
        // HealthState: badly hurt, it may die of it
        if (float const h = w.creature_get_health(c); h < 0.f && !w.creature_get_dead(c) && x.rng.value() < -h && x.rng.value() < 0.025f)
            creature::die(state, c);
        if (x.small)
            small_update(x);
        else
            big_update(x);
    }

    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) {
        // what holds it, what it hangs on to
        return !creature::holds(state.world, t, c) && !creature::holds(state.world, c, t);
    }

    void collide(sys::game_state&, dcon::creature_id, room_view const&, uint32_t, dcon::creature_id, uint32_t) {}

    void violence(sys::game_state& state, dcon::creature_id c, uint32_t k, vec2 momentum, float damage, float stun_bonus, creature::damage_type) {
        auto& w = state.world;
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        bool const small = is_small(state, c);
        bool const was_dead = w.creature_get_dead(c) != 0;
        // Creature.Violence (HealthState)
        auto const hit = chunk(state, c, k);
        w.body_chunk_get_vel(hit) += rw::clamp_magnitude(momentum / w.body_chunk_get_mass(hit), 10.f);
        float const dmg = damage / (small ? small_damage_resistance : big_damage_resistance);
        float st = (damage * 30.f + stun_bonus) / (small ? small_stun_resistance : big_stun_resistance);
        float& health = w.creature_get_health(c);
        st *= 1.5f + mathf::inverse_lerp(0.5f, 0.f, health) * rng.value();
        creature::stun(state, c, int32_t(st));
        health -= dmg;
        if (rng.value() < -health || health < -1.f || (health < 0.f && rng.value() < 0.33f) || dmg >= instant_death_damage_limit)
            creature::die(state, c);
        // who did it: a noodlefly's stab says so, else the closest slugcat
        // is blamed (the weapons don't tell their thrower here)
        auto& ai = w.needle_worm_data_get_ai(data(state, c));
        dcon::creature_id attacker = ai.attacker;
        ai.attacker = dcon::creature_id{};
        auto const room = creature::room_of(state, c);
        if (!creature::live(state, attacker)) {
            attacker = dcon::creature_id{};
            vec2 const at = w.body_chunk_get_pos(hit);
            float dst = float_max;
            rooms::for_each_creature(state, room, creature::type::slugcat, [&](dcon::creature_id s) {
                if (!creature::here(state, s, room))
                    return;
                float const dd = rw::distance(at, creature::main_pos(state, s));
                if (dd < dst) {
                    dst = dd;
                    attacker = s;
                }
            });
        }
        social_event(state, c, attacker, damage, was_dead, !was_dead && w.creature_get_dead(c));
    }

    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t) {
        auto& w = state.world;
        auto& a = w.needle_worm_data_get_attack(data(state, c));
        if (a.impale == t)
            a.impale = dcon::creature_id{};
        if (auto const g = creature::grasp_on(w, c, t); g >= 0) {
            creature::release_grasp(state, c, uint32_t(g));
            for (uint32_t i = 0; i < chunk_count(is_small(state, c)); ++i)
                w.body_chunk_set_collide_with_terrain(chunk(state, c, i), 1);
        }
    }

    void died(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        if (is_small(state, c)) {
            // SmallNeedleWorm.Die: the cry starts
            auto& y = w.needle_worm_data_get_young(data(state, c));
            if (y.scream_counter == 0.f)
                y.scream_counter = 0.01f;
        } else {
            auto& a = w.needle_worm_data_get_attack(data(state, c));
            a.impale = dcon::creature_id{};
            a.charging_attack = 0.f;
        }
    }

    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const view = world::get_room_view(state, room);
        ctx x(state, c, view);
        // NeedleWorm.SpitOutOfShortCut: out of the hole along its direction
        ivec2 const tile = room_view::get_tile_position(at);
        vec2 const a = rw::to_vec2(view.shortcut_entrance_hole_direction(tile));
        x.x.look_dir = a;
        w.needle_worm_data_set_get_to_look_dir(x.d, a);
        x.x.flying = 0.f;
        x.x.flying_this_frame = 0;
        for (uint32_t i = 0; i < x.n; ++i) {
            physics::hard_set_position(state, x.ch[i],
                room_view::middle_of_tile(tile) + a * custom::lerp_map(float(i), 0.f, float(x.n), 7.f, -2.f) * (x.small ? 0.3f : 1.f));
            x.vel(i) = a * custom::lerp_map(float(i), 0.f, float(x.n), 10.f, 1.f);
        }
        reset_tail(x, x.pos(x.n - 1));
        x.x.no_shortcuts = 80;
        auto& atk = x.atk();
        atk.in_wall = 0;
        atk.impale = dcon::creature_id{};
        atk.has_swish = 0;
        atk.swish_counter = 0;
        atk.charging_attack = 0.f;
        // NeedleWormAI.NewRoom
        auto& ai = x.ai();
        ai.in_room_counter = 0;
        ai.old_count = 0;
        ai.idle_pos = tile;
        ai.last_assigned_idle_pos = tile;
        ai.focus = dcon::creature_id{};
        x.big_ai().keep_close_to = dcon::creature_id{};
        auto& pp = w.needle_worm_data_get_prev_positions(x.d);
        for (auto& q : pp.p)
            q = x.main();
    }

    std::string describe(sys::game_state const& state, dcon::creature_id c) {
        auto const& w = state.world;
        static constexpr char const* behaviors[] = {"idle", "flee", "hunt", "attack", "faux attack", "get unstuck", "escape rain", "migrate"};
        auto const b = uint32_t(w.creature_get_behavior(c));
        std::string s = is_small(state, c) ? "baby noodlefly " : "noodlefly ";
        s += b < 8 ? behaviors[b] : "?";
        if (!is_small(state, c)) {
            auto const& a = w.needle_worm_data_get_attack(data(state, c));
            s += " ready " + std::to_string(a.attack_ready) + " charge " + std::to_string(a.charging_attack);
            if (a.in_wall)
                s += " stuck in wall";
            if (a.impale)
                s += " impaling";
        } else if (creature::grasped_object(w, c, 0)) {
            s += " on mother";
        }
        return s;
    }

    bool spawn_spot(room_view const& room, ivec2 tile) {
        return room.inside(tile.x, tile.y) && !room.get_tile(tile).solid() && pathing::terrain_proximity(room, tile) >= 3 && !pathing::narrow_space(room, tile);
    }

    void flying_weapon(sys::game_state& state, dcon::creature_id c, vec2 pos, vec2 vel) {
        if (is_small(state, c) || !normal_flying_state(state, c))
            return;
        auto const view = world::get_room_view(state, creature::room_of(state, c));
        if (!visual_contact(state, c, view, pos))
            return;
        ctx x(state, c, view);
        vec2 const dir = rw::normalized(vel);
        vec2 v = x.pos(1) - (pos + dir * 200.f);
        v.y *= 2.f;
        if (rw::magnitude(v) > 200.f)
            return;
        dodge(x, pos, dir);
    }

    //
    // The body for the graphics
    //

    vec2 segment_pos(sys::game_state const& state, dcon::creature_id c, uint32_t seg, float alpha) noexcept {
        auto const& w = state.world;
        uint32_t const n = chunk_count(is_small(state, c));
        if (seg < n) {
            auto const k = chunk(state, c, seg);
            return rw::lerp(w.body_chunk_get_last_pos(k), w.body_chunk_get_pos(k), alpha);
        }
        auto const& t = w.needle_worm_data_get_tail(data(state, c));
        return rw::lerp(t.last_pos[seg - n], t.pos[seg - n], alpha);
    }

    vec2 segment_dir(sys::game_state const& state, dcon::creature_id c, uint32_t seg, float alpha) noexcept {
        if (seg == 0)
            return custom::dir_vec(segment_pos(state, c, 1, alpha), segment_pos(state, c, 0, alpha));
        return custom::dir_vec(segment_pos(state, c, seg, alpha), segment_pos(state, c, seg - 1, alpha));
    }

    float segment_rad_for_collision(sys::game_state const& state, dcon::creature_id c, uint32_t seg) noexcept {
        if (seg < chunk_count(is_small(state, c)))
            return state.world.body_chunk_get_rad(chunk(state, c, seg));
        return 1.f;
    }

    namespace {
        // OnBody*'s segment pair and the fraction between them
        void on_body(sys::game_state const& state, dcon::creature_id c, float f, int32_t& a, int32_t& b, float& t) {
            int32_t const total = int32_t(total_segments(is_small(state, c)));
            f *= float(total) - 1.1f;
            a = custom::int_clamp(int32_t(std::floor(f)), 0, total - 1);
            b = custom::int_clamp(a + 1, 0, total - 1);
            t = mathf::inverse_lerp(float(a), float(b), f);
        }
    }

    vec2 on_body_pos(sys::game_state const& state, dcon::creature_id c, float f, float alpha) noexcept {
        int32_t a, b;
        float t;
        on_body(state, c, f, a, b, t);
        return rw::lerp(segment_pos(state, c, uint32_t(a), alpha), segment_pos(state, c, uint32_t(b), alpha), t);
    }

    vec2 on_body_dir(sys::game_state const& state, dcon::creature_id c, float f, float alpha) noexcept {
        int32_t a, b;
        float t;
        on_body(state, c, f, a, b, t);
        return rw::slerp(segment_dir(state, c, uint32_t(a), alpha), segment_dir(state, c, uint32_t(b), alpha), t);
    }

    float on_body_rad(sys::game_state const& state, dcon::creature_id c, float f) noexcept {
        int32_t a, b;
        float t;
        on_body(state, c, f, a, b, t);
        return mathf::lerp(segment_rad_for_collision(state, c, uint32_t(a)), segment_rad_for_collision(state, c, uint32_t(b)), t);
    }

    dcon::creature_id hanging_on_mother(sys::game_state const& state, dcon::creature_id c, uint32_t* seg) noexcept {
        auto const& w = state.world;
        if (!is_small(state, c))
            return dcon::creature_id{};
        auto const& y = w.needle_worm_data_get_young(data(state, c));
        auto const m = creature::grasped(w, c, 0);
        if (!m || m != y.mother || !is_big_worm(state, m))
            return dcon::creature_id{};
        if (seg)
            *seg = y.mom_tail_segment;
        return m;
    }

    //
    // The slugcat side
    //

    dcon::creature_id held_in_hand(sys::game_state const& state, dcon::player_id p, uint32_t hand) noexcept {
        if (!p)
            return dcon::creature_id{};
        auto const c = creature::grasped(state.world, player::creature_of(state, p), hand);
        return c && creature::type_of(state.world, c) == creature::type::needle_worm && is_small(state, c) ? c : dcon::creature_id{};
    }

    dcon::creature_id held_by(sys::game_state const& state, dcon::player_id p) noexcept {
        if (auto const c = held_in_hand(state, p, 0))
            return c;
        return held_in_hand(state, p, 1);
    }

    dcon::creature_id pickup_candidate(sys::game_state const& state, dcon::player_id p, room_view const& room, float& score) {
        auto const& w = state.world;
        score = float_max;
        vec2 const p0 = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
        int32_t const flip = w.player_get_flip_direction(p);
        dcon::creature_id result;
        rooms::for_each_creature(state, room.id, creature::type::needle_worm, [&](dcon::creature_id c) {
            if (!is_small(state, c) || creature::held(state, c) || w.creature_get_away(c) != 0)
                return;
            for (uint32_t i = 0; i < chunk_count(true); ++i) {
                auto const k = chunk(state, c, i);
                vec2 const cp = w.body_chunk_get_pos(k);
                float const rad = w.body_chunk_get_rad(k);
                if (!custom::dist_less(p0, cp, rad + 40.f))
                    continue;
                if (!custom::dist_less(p0, cp, rad + 20.f) && !room.visual_contact(p0, cp))
                    continue;
                float dist = rw::distance(p0, cp);
                if ((cp.x < p0.x) == (flip < 0))
                    dist -= 10.f;
                if (dist < score) {
                    result = c;
                    score = dist;
                }
            }
        });
        return result;
    }

    void grab(sys::game_state& state, dcon::player_id p, dcon::creature_id c, uint32_t hand) {
        // off its mother's tail first
        if (creature::grasped_object(state.world, c, 0))
            creature::release_grasp(state, c, 0);
        for (uint32_t i = 0; i < chunk_count(true); ++i)
            state.world.body_chunk_set_collide_with_terrain(chunk(state, c, i), 1);
        creature::grab(state, player::creature_of(state, p), hand, c, 0);
    }

    void release(sys::game_state& state, dcon::creature_id c) {
        uint32_t g = 0;
        if (auto const h = creature::holder(state, c, &g))
            creature::release_grasp(state, h, g);
    }

    void release_player(sys::game_state& state, dcon::player_id p) {
        for (uint32_t hand = 0; hand < 2; ++hand)
            if (held_in_hand(state, p, hand))
                creature::release_grasp(state, player::creature_of(state, p), hand);
    }

    void carry_update(sys::game_state& state, dcon::player_id p, uint32_t hand, vec2 hand_pos) {
        auto& w = state.world;
        auto const c = held_in_hand(state, p, hand);
        if (!c)
            return;
        auto const k = chunk(state, c, 0);
        w.body_chunk_set_last_pos(k, w.body_chunk_get_pos(k));
        w.body_chunk_set_pos(k, hand_pos);
        w.body_chunk_set_vel(k, w.body_chunk_get_vel(player::body_chunk(state, p, 0)));
    }

    void follow_player(sys::game_state& state, dcon::player_id p, dcon::room_id room, bool spit_out) {
        auto& w = state.world;
        for (uint32_t hand = 0; hand < 2; ++hand) {
            auto const c = held_in_hand(state, p, hand);
            if (!c)
                continue;
            w.physical_object_set_room_from_object_location(body(state, c), room);
            if (!spit_out)
                continue;
            vec2 const at = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
            for (uint32_t i = 0; i < chunk_count(true); ++i) {
                physics::hard_set_position(state, chunk(state, c, i), at);
                w.body_chunk_set_vel(chunk(state, c, i), w.body_chunk_get_vel(player::body_chunk(state, p, 0)));
            }
            auto const view = world::get_room_view(state, room);
            ctx x(state, c, view);
            reset_tail(x, at);
            tracker::clear(state, c);
            needle_worm_graphics::reset_positions(state, c);
        }
    }

    bool bit_by_player(sys::game_state& state, dcon::creature_id c, dcon::player_id p) {
        auto& w = state.world;
        {
            auto const view = world::get_room_view(state, creature::room_of(state, c));
            ctx x(state, c, view);
            scream(x);
        }
        creature::die(state, c);
        vec2 const mouth = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
        for (uint32_t i = 0; i < chunk_count(true); ++i) {
            auto const k = chunk(state, c, i);
            w.body_chunk_set_pos(k, mouth);
            w.body_chunk_set_last_pos(k, mouth);
        }
        auto& y = w.needle_worm_data_get_young(data(state, c));
        if (y.bites > 0)
            --y.bites;
        return y.bites < 1;
    }
}
