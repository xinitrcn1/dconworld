// MirosBird.cs (the body, its two BirdLegs and its Tentacle neck) and a
// simpler MirosBirdAI. Each bird is on its own: vanilla's MirosBirdAbstractAI
// moves a flock between rooms together; here a bird roams its room's floors
// and changes rooms only as migrate.hpp moves every creature. It notices what
// it Eats (the relationship table) when it sees it, runs at it with its head
// out and bites; the bite kills a slugcat (Violence 1.2 Bite) and the beak
// holds what it bit, carried home to its den.
//
// The body is four chunks (0 the main chunk, 1 the neck's base, 2 and 3 the
// hips) held up by the legs while they touch the ground, and chunk 4 the
// head, tied to the neck's tip. The neck is vanilla's tile Tentacle (its
// segments follow a tile path to where the head looks), ported here without
// a rope.

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <queue>
#include <vector>

#include "miros.hpp"
#include "body_parts.hpp"
#include "dens.hpp"
#include "migrate.hpp"
#include "miros_graphics.hpp"
#include "particles.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "rooms.hpp"
#include "tracker.hpp"

namespace miros {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;
    using creature::relationship;

    namespace {
        // CreatureTemplate: MirosBird
        constexpr float base_damage_resistance = 7.f;
        constexpr float base_stun_resistance = 2.75f;
        constexpr float visual_radius = 1800.f;
        // Tracker(AI, 4, 10, 160, ...): ticks a creature out of sight is remembered
        constexpr int32_t forget_time = 160;
        // BirdLeg.lowerLegLength, tighLenght
        constexpr float lower_leg_length = 55.f;
        constexpr float tigh_length = 35.f;
        // the neck: Tentacle(bodyChunks[1], 60) with TentacleProps(false,
        // false, true, 0.5f, 0.1f, 0.5f, 1.8f, 0.2f, 1.2f, 10f, 0.25f, 10f,
        // 15, 20, 20, 0)
        constexpr float neck_ideal_length = 60.f;
        constexpr float mass_deterioration_per_chunk = 0.5f;
        constexpr float pull_at_connection_chunk = 0.1f;
        constexpr float goal_attraction_speed_tip = 0.5f;
        constexpr float goal_attraction_speed = 1.8f;
        constexpr float align_to_segment_speed = 0.2f;
        constexpr float backtrack_speed = 1.2f;
        constexpr float chunk_velocity_cap = 10.f;
        constexpr float tile_tentacle_update_speed = 0.25f;
        constexpr float tile_tentacle_snap_with_path_distance = 10.f;
        constexpr int32_t segment_phase_through_terrain_frames = 15;
        constexpr int32_t max_pull_tiles_per_tick = 20;
        constexpr int32_t terrain_hits_before_phase = 0;
        // MirosBird.Head: bodyChunks[4]
        constexpr uint32_t head = 4;
        constexpr uint32_t chunk_count = 5;
        constexpr float float_max = std::numeric_limits<float>::max();
        constexpr ivec2 four_directions_and_zero[5] = {{0, 0}, {-1, 0}, {0, -1}, {1, 0}, {0, 1}};

        bool has_any_solid(room_view const& room, ivec2 t) { return room.get_tile(t).solid(); }
        float float_dist(ivec2 a, ivec2 b) { return rw::magnitude(rw::to_vec2(a - b)); }
        bool inside(room_view const& room, ivec2 t) { return room.inside(t.x, t.y); }
        // Custom.AreIntVectorsNeighbors / AreIntVectorsDiagonalNeighbors (sic)
        bool are_neighbors(ivec2 a, ivec2 b) {
            return a != b && (a.x == b.x || a.y == b.y) && std::abs(a.x - b.x) < 2 && std::abs(a.y - b.y) < 2;
        }
        bool are_diagonal_neighbors(ivec2 a, ivec2 b) {
            return a.x != b.x && a.y != b.y && (std::abs(a.x - b.x) < 2 || std::abs(a.y - b.y) < 2);
        }
        // Custom.FlattenVectorAlongAxis
        vec2 flatten_vector_along_axis(vec2 v, float axis, float fac) {
            v = body_parts::rotate_around_origo(v, axis);
            v.y *= fac;
            return body_parts::rotate_around_origo(v, -axis);
        }

        //
        // Room / SharedPhysics tile ray traces
        //
        // Room.RayTraceTilesList (fills `path` from the front, returns the count)
        int32_t ray_trace_tiles_list(int32_t x0, int32_t y0, int32_t x1, int32_t y1, std::vector<ivec2>& path) {
            int32_t num = std::abs(x1 - x0);
            int32_t num2 = std::abs(y1 - y0);
            int32_t x = x0, y = y0;
            int32_t i = std::min(1 + num + num2, 4096);
            int32_t const sx = x1 > x0 ? 1 : -1;
            int32_t const sy = y1 > y0 ? 1 : -1;
            int32_t e = num - num2;
            num *= 2;
            num2 *= 2;
            int32_t k = 0;
            while (i > 0) {
                if (int32_t(path.size()) <= k)
                    path.push_back(ivec2{x, y});
                else
                    path[size_t(k)] = ivec2{x, y};
                ++k;
                if (e > 0) {
                    x += sx;
                    e -= num2;
                } else {
                    y += sy;
                    e += num;
                }
                --i;
            }
            return k;
        }
        // Room.RayTraceTilesForTerrain: no solid tile on the way
        bool ray_trace_tiles_for_terrain(room_view const& room, ivec2 a, ivec2 b) {
            int32_t num = std::abs(b.x - a.x);
            int32_t num2 = std::abs(b.y - a.y);
            int32_t x = a.x, y = a.y;
            int32_t i = std::min(1 + num + num2, 4096);
            int32_t const sx = b.x > a.x ? 1 : -1;
            int32_t const sy = b.y > a.y ? 1 : -1;
            int32_t e = num - num2;
            num *= 2;
            num2 *= 2;
            while (i > 0) {
                if (room.get_tile(x, y).solid())
                    return false;
                if (e > 0) {
                    x += sx;
                    e -= num2;
                } else {
                    y += sy;
                    e += num;
                }
                --i;
            }
            return true;
        }
        // the tiles of the line from `a` to `b` in order (RayTracedTilesArray's
        // walk), until `f` returns true
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
        // Custom.RectCollision: where the segment from `last` to `pos` enters the rectangle
        vec2 rect_collision(vec2 pos, vec2 last, float left, float bottom, float right, float top) {
            vec2 const v1 = vertical_cross_point(last, pos, left);
            vec2 const v2 = vertical_cross_point(last, pos, right);
            vec2 v3 = horizontal_cross_point(last, pos, bottom);
            vec2 v4 = horizontal_cross_point(last, pos, top);
            if (last.x == pos.x) {
                v3 = vec2{last.x, bottom};
                v4 = vec2{last.x, top};
            }
            bool const in = last.x > left && last.x < right && last.y > bottom && last.y < top;
            float dst = in ? float_max : rw::distance(last, pos);
            vec2 out = pos;
            auto const take = [&](vec2 v) {
                out = v;
                dst = rw::distance(last, v);
            };
            if (v1.y >= bottom && v1.y <= top && custom::dist_less(last, v1, dst))
                take(v1);
            if (v2.y >= bottom && v2.y <= top && custom::dist_less(last, v2, dst))
                take(v2);
            if (v3.x >= left && v3.x <= right && custom::dist_less(last, v3, dst))
                take(v3);
            if (v4.x >= left && v4.x <= right && custom::dist_less(last, v4, dst))
                take(v4);
            return out;
        }
        // SharedPhysics.ExactTerrainRayTracePos
        std::optional<vec2> exact_terrain_ray_trace_pos(room_view const& room, vec2 a, vec2 b) {
            ivec2 hit{0, 0};
            if (!walk_tiles(a, b, [&](ivec2 t) { return room.get_tile(t).solid() ? (hit = t, true) : false; }))
                return std::nullopt;
            vec2 const m = room_view::middle_of_tile(hit);
            return rect_collision(b, a, m.x - 10.f, m.y - 10.f, m.x + 10.f, m.y + 10.f);
        }

        //
        // SharedPhysics.TerrainCollisionData: VerticalCollision,
        // SlopesVertically, HorizontalCollision (for the neck's chunks)
        //
        struct terrain_collision {
            vec2 pos;
            vec2 last_pos;
            vec2 vel;
            float rad;
            ivec2 contact{0, 0};
        };
        void vertical_collision(room_view const& room, terrain_collision& cd) {
            ivec2 const tp = room_view::get_tile_position(cd.last_pos);
            bool const last_solid = room.get_tile(cd.last_pos).solid();
            if (cd.vel.y > 0.f) {
                int32_t const y = room_view::get_tile_position(vec2{0.f, cd.pos.y + cd.rad + 0.01f}).y;
                int32_t const y2 = room_view::get_tile_position(vec2{0.f, cd.last_pos.y + cd.rad}).y;
                int32_t const x = room_view::get_tile_position(vec2{cd.pos.x - cd.rad + 1.f, 0.f}).x;
                int32_t const x2 = room_view::get_tile_position(vec2{cd.pos.x + cd.rad - 1.f, 0.f}).x;
                for (int32_t i = y2; i <= y; ++i)
                    for (int32_t j = x; j <= x2; ++j)
                        if (room.get_tile(j, i).solid() && !room.get_tile(j, i - 1).solid() && (tp.y < i || last_solid)) {
                            cd.pos.y = float(i) * 20.f - cd.rad;
                            cd.vel.y = 0.f;
                            cd.contact.y = 1;
                            return;
                        }
            } else if (cd.vel.y < 0.f) {
                int32_t const y = room_view::get_tile_position(vec2{0.f, cd.pos.y - cd.rad - 0.01f}).y;
                int32_t const y2 = room_view::get_tile_position(vec2{0.f, cd.last_pos.y - cd.rad}).y;
                int32_t const x = room_view::get_tile_position(vec2{cd.pos.x - cd.rad + 1.f, 0.f}).x;
                int32_t const x2 = room_view::get_tile_position(vec2{cd.pos.x + cd.rad - 1.f, 0.f}).x;
                for (int32_t i = y2; i >= y; --i)
                    for (int32_t j = x; j <= x2; ++j)
                        if (room.get_tile(j, i).solid() && !room.get_tile(j, i + 1).solid() && (tp.y > i || last_solid)) {
                            cd.pos.y = (float(i) + 1.f) * 20.f + cd.rad;
                            cd.vel.y = 0.f;
                            cd.contact.y = -1;
                            return;
                        }
            }
        }
        void slopes_vertically(room_view const& room, terrain_collision& cd) {
            using world::slope_direction;
            ivec2 const tp = room_view::get_tile_position(cd.pos);
            ivec2 b{0, 0};
            slope_direction a = room.identify_slope(cd.pos);
            vec2 const mid = room_view::middle_of_tile(cd.pos);
            if (room.get_tile(cd.pos).t != world::terrain::slope) {
                if (room.identify_slope(tp.x - 1, tp.y) != slope_direction::broken && cd.pos.x - cd.rad <= mid.x - 10.f) {
                    a = room.identify_slope(tp.x - 1, tp.y);
                    b.x = -1;
                } else if (room.identify_slope(tp.x + 1, tp.y) != slope_direction::broken && cd.pos.x + cd.rad >= mid.x + 10.f) {
                    a = room.identify_slope(tp.x + 1, tp.y);
                    b.x = 1;
                } else if (cd.pos.y - cd.rad < mid.y - 10.f) {
                    if (room.identify_slope(tp.x, tp.y - 1) != slope_direction::broken) {
                        a = room.identify_slope(tp.x, tp.y - 1);
                        b.y = -1;
                    }
                } else if (cd.pos.y + cd.rad > mid.y + 10.f && room.identify_slope(tp.x, tp.y + 1) != slope_direction::broken) {
                    a = room.identify_slope(tp.x, tp.y + 1);
                    b.y = 1;
                }
            }
            if (a == slope_direction::broken)
                return;
            vec2 const v = room_view::middle_of_tile(tp + b);
            float num;
            int32_t num2;
            if (a == slope_direction::up_left) {
                num = cd.pos.x - (v.x - 10.f) + (v.y - 10.f);
                num2 = -1;
            } else if (a == slope_direction::up_right) {
                num = 20.f - (cd.pos.x - (v.x - 10.f)) + (v.y - 10.f);
                num2 = -1;
            } else if (a == slope_direction::down_left) {
                num = 20.f - (cd.pos.x - (v.x - 10.f)) + (v.y - 10.f);
                num2 = 1;
            } else {
                num = cd.pos.x - (v.x - 10.f) + (v.y - 10.f);
                num2 = 1;
            }
            if (num2 == -1 && cd.pos.y <= num + cd.rad + cd.rad)
                cd.pos.y = num + cd.rad + cd.rad;
            else if (num2 == 1 && cd.pos.y >= num - cd.rad - cd.rad)
                cd.pos.y = num - cd.rad - cd.rad;
        }
        void horizontal_collision(room_view const& room, terrain_collision& cd) {
            ivec2 const tp = room_view::get_tile_position(cd.last_pos);
            bool const last_solid = room.get_tile(cd.last_pos).solid();
            if (cd.vel.x > 0.f) {
                int32_t const x = room_view::get_tile_position(vec2{cd.pos.x + cd.rad + 0.01f, 0.f}).x;
                int32_t const x2 = room_view::get_tile_position(vec2{cd.last_pos.x + cd.rad, 0.f}).x;
                int32_t const y = room_view::get_tile_position(vec2{0.f, cd.pos.y + cd.rad - 1.f}).y;
                int32_t const y2 = room_view::get_tile_position(vec2{0.f, cd.pos.y - cd.rad + 1.f}).y;
                for (int32_t i = x2; i <= x; ++i)
                    for (int32_t j = y2; j <= y; ++j)
                        if (room.get_tile(i, j).solid() && !room.get_tile(i - 1, j).solid() && (tp.x < i || last_solid)) {
                            cd.pos.x = float(i) * 20.f - cd.rad;
                            cd.vel.x = 0.f;
                            cd.contact.x = 1;
                            return;
                        }
            } else if (cd.vel.x < 0.f) {
                int32_t const x = room_view::get_tile_position(vec2{cd.pos.x - cd.rad - 0.01f, 0.f}).x;
                int32_t const x2 = room_view::get_tile_position(vec2{cd.last_pos.x - cd.rad, 0.f}).x;
                int32_t const y = room_view::get_tile_position(vec2{0.f, cd.pos.y + cd.rad - 1.f}).y;
                int32_t const y2 = room_view::get_tile_position(vec2{0.f, cd.pos.y - cd.rad + 1.f}).y;
                for (int32_t i = x2; i >= x; --i)
                    for (int32_t j = y2; j <= y; ++j)
                        if (room.get_tile(i, j).solid() && !room.get_tile(i + 1, j).solid() && (tp.x > i || last_solid)) {
                            cd.pos.x = (float(i) + 1.f) * 20.f + cd.rad;
                            cd.vel.x = 0.f;
                            cd.contact.x = -1;
                            return;
                        }
            }
        }

        //
        // Update's working set
        //
        struct ctx {
            sys::game_state& state;
            dcon::creature_id c;
            room_view const& room;
            rw::rng rng;
            creature::miros_payload& p;
            dcon::miros_data_id d;
            leg_pair& legs;
            neck_state& n;
            dcon::body_chunk_id ch[chunk_count];
            bool viewed;
            // the neck's tile lists while it updates, and the `ref path` scratch
            std::vector<ivec2> seg;
            std::vector<ivec2> gp;
            std::vector<ivec2> scratch;
            // the path field to the destination, once asked for
            std::optional<pathing::field_id> field;

            // `salt` tells apart the ctxs made for one bird in one tick
            ctx(sys::game_state& s, dcon::creature_id cc, room_view const& r, uint32_t salt)
                : state(s), c(cc), room(r), rng(rw::random(s.seed, s.tick, rw::ids(uint32_t(cc.value), salt))), p(s.world.creature_get_payload(cc).miros), d(creature::miros_data(s.world, cc)),
                  legs(s.world.miros_data_get_legs(d)), n(s.world.miros_data_get_neck(d)), viewed(s.viewed(r.id)) {
                for (uint32_t i = 0; i < chunk_count; ++i)
                    ch[i] = chunk(s, cc, i);
            }
            auto& w() { return state.world; }
            vec2& pos(uint32_t i) { return state.world.body_chunk_get_pos(ch[i]); }
            vec2& vel(uint32_t i) { return state.world.body_chunk_get_vel(ch[i]); }
            float mass(uint32_t i) { return state.world.body_chunk_get_mass(ch[i]); }
            bool awake() { return consious(state, c); }
            float gravity() { return physics::gravity(state, body(state, c), room); }
            dcon::creature_id grasped() { return creature::grasped(state.world, c, 0); }
            ivec2& destination() { return state.world.creature_get_destination(c); }
            // abstractCreature.pos: the main chunk's tile
            ivec2 at() { return room_view::get_tile_position(pos(0)); }
            pathing::field_id path() {
                if (!field)
                    field = pathing::field_to(state, room.id, destination(), walker);
                return *field;
            }
            neck_chunk& tip() { return n.c[neck_chunks - 1]; }
            float run_mode() { return (legs.l[0].run_mode + legs.l[1].run_mode) / 2.f; }
        };

        void load_neck(ctx& x) {
            x.seg.assign(x.n.segments.v, x.n.segments.v + x.n.segments.n);
            x.gp.assign(x.n.grab_path.v, x.n.grab_path.v + x.n.grab_path.n);
        }
        void store_neck(ctx& x) {
            auto put = [](tile_list& l, std::vector<ivec2> const& v) {
                l.n = uint8_t(std::min<size_t>(v.size(), max_neck_tiles));
                std::copy(v.begin(), v.begin() + l.n, l.v);
            };
            put(x.n.segments, x.seg);
            put(x.n.grab_path, x.gp);
        }

        // the floor tile under the body (where its feet are): the path runs
        // from there (see `walker`)
        ivec2 ground_tile(room_view const& room, ivec2 t) {
            t.x = custom::int_clamp(t.x, 0, room.width - 1);
            t.y = custom::int_clamp(t.y, 0, room.height - 1);
            for (int32_t y = t.y; y >= 0 && y > t.y - 10; --y)
                if (pathing::accessible(room, ivec2{t.x, y}, walker))
                    return ivec2{t.x, y};
            return t;
        }
        ivec2 ground(ctx& x) { return ground_tile(x.room, x.at()); }

        // a floor tile with room above for its body
        bool open_floor(room_view const& room, ivec2 t) {
            if (!inside(room, t) || !pathing::accessible(room, t, walker))
                return false;
            for (int32_t k = 1; k <= 4; ++k)
                if (has_any_solid(room, t + ivec2{0, k}))
                    return false;
            return true;
        }

        //
        // The neck: Tentacle
        //
        vec2 float_base(ctx& x) { return x.pos(1); }

        std::optional<ivec2> grab_dest(ctx& x) {
            if (x.n.float_grab_dest)
                return room_view::get_tile_position(*x.n.float_grab_dest);
            return std::nullopt;
        }
        // Tentacle.CheckIfAnySegmentsLeft / BasePos
        ivec2 base_pos(ctx& x) {
            if (x.seg.empty())
                x.seg.push_back(room_view::get_tile_position(float_base(x)));
            return x.seg[0];
        }
        // TentacleChunk.currentSegment / StuckPos
        int32_t current_segment(ctx& x, neck_chunk const& c) {
            int32_t const k = int32_t(x.seg.size());
            return custom::int_clamp(int32_t(c.t_pos * float(k - 1)), 0, k - 1);
        }
        ivec2 stuck_pos(ctx& x, neck_chunk const& c) { return x.seg[size_t(current_segment(x, c))]; }
        // Tentacle.Visual
        bool visual(ctx& x, ivec2 a, ivec2 b) { return ray_trace_tiles_for_terrain(x.room, a, b); }

        // TentacleChunk.Reset
        void chunk_reset(ctx& x, neck_chunk& c) {
            c.phase = -1.f;
            c.pos = room_view::middle_of_tile(stuck_pos(x, c));
            c.last_pos = c.pos;
            c.vel = vec2{0.f, 0.f};
        }

        // TentacleChunk.Update
        void chunk_update(ctx& x, uint32_t i) {
            auto& n = x.n;
            auto& c = n.c[i];
            c.last_pos = c.pos;
            float const ideal = neck_ideal_length / float(neck_chunks) * mathf::lerp(1.f, 0.1f, n.retract_fac);
            if (c.phase == -1.f) {
                if (c.phases_to_same_location > 0 && stuck_pos(x, c) == c.after_phase_stuck_pos) {
                    vec2 const m = room_view::middle_of_tile(stuck_pos(x, c));
                    c.pos = vec2{mathf::clamp(c.pos.x, m.x - 9.f, m.x + 9.f), mathf::clamp(c.pos.y, m.y - 9.f, m.y + 9.f)};
                }
                if (i == 0) {
                    vec2 const a = custom::dir_vec(c.pos, float_base(x));
                    float const num = rw::distance(c.pos, float_base(x));
                    if (num > ideal) {
                        c.pos -= a * (ideal - num) * (1.f - pull_at_connection_chunk);
                        c.vel -= a * (ideal - num) * (1.f - pull_at_connection_chunk);
                        x.pos(1) += a * (ideal - num) * pull_at_connection_chunk;
                        x.vel(1) += a * (ideal - num) * pull_at_connection_chunk;
                    }
                    c.stretched_fac = neck_ideal_length / float(neck_chunks) / mathf::max(1.f, num);
                } else {
                    auto& prev = n.c[i - 1];
                    vec2 const a = custom::dir_vec(c.pos, prev.pos);
                    float const num = rw::distance(c.pos, prev.pos);
                    if (num > ideal) {
                        c.pos -= a * (ideal - num) * (1.f - mass_deterioration_per_chunk);
                        c.vel -= a * (ideal - num) * (1.f - mass_deterioration_per_chunk);
                        prev.pos += a * (ideal - num) * mass_deterioration_per_chunk;
                        prev.vel += a * (ideal - num) * mass_deterioration_per_chunk;
                    }
                    c.stretched_fac = neck_ideal_length / float(neck_chunks) / mathf::max(1.f, num);
                }
                ivec2 const here = room_view::get_tile_position(c.pos);
                int32_t const cs = current_segment(x, c);
                if (n.backtrack_from == -1 && !visual(x, here, x.seg[size_t(cs)]))
                    n.backtrack_from = int32_t(i);
                if (n.backtrack_from != -1 && int32_t(i) >= n.backtrack_from) {
                    bool flag = false;
                    for (int32_t k = 1; k <= cs && !flag; ++k) {
                        if (!visual(x, here, x.seg[size_t(k)]) && visual(x, here, x.seg[size_t(k - 1)])) {
                            c.vel += rw::clamp_magnitude(room_view::middle_of_tile(x.seg[size_t(k - 1)]) - c.pos, 20.f) * backtrack_speed / 20.f;
                            flag = true;
                        }
                    }
                    if (!flag) {
                        vec2 const fb = i > 0 ? n.c[i - 1].pos : float_base(x);
                        c.vel += rw::clamp_magnitude(fb - c.pos, 20.f) * backtrack_speed / 20.f;
                        if (i > 0)
                            n.c[i - 1].vel += rw::clamp_magnitude(fb - c.pos, 20.f) * backtrack_speed / 20.f;
                    }
                    if (!flag && (c.contact_point.x != 0 || c.contact_point.y != 0)) {
                        ++c.phase_attempts;
                        if (c.phase_attempts > terrain_hits_before_phase) {
                            // PhaseToSegment
                            c.phase_from = c.pos;
                            c.phase = 0.f;
                            c.phase_attempts = 0;
                        }
                    }
                } else {
                    c.phase_attempts = 0;
                    if (n.limp) {
                        for (int32_t k = cs; k >= 0; --k) {
                            ivec2 const s = x.seg[size_t(k)];
                            if (visual(x, here, s)) {
                                c.vel += rw::clamp_magnitude(room_view::middle_of_tile(s) - c.pos, 20.f) * 0.005f;
                                break;
                            }
                        }
                    } else {
                        if (n.float_grab_dest)
                            c.vel += rw::clamp_magnitude(*n.float_grab_dest - c.pos, 20.f) * (i == neck_chunks - 1 ? goal_attraction_speed_tip : goal_attraction_speed) / 20.f;
                        c.vel += rw::clamp_magnitude(room_view::middle_of_tile(x.seg[size_t(cs)]) - c.pos, 20.f) * align_to_segment_speed / 20.f;
                    }
                }
                c.vel = rw::clamp_magnitude(c.vel, chunk_velocity_cap);
                c.pos += c.vel;
                c.last_contact_point = c.contact_point;
                terrain_collision cd{c.pos, c.pos - c.vel, c.vel, c.rad};
                vertical_collision(x.room, cd);
                slopes_vertically(x.room, cd);
                horizontal_collision(x.room, cd);
                c.contact_point = cd.contact;
                c.pos = cd.pos;
                c.vel = cd.vel;
            } else {
                c.phase += 1.f / float(segment_phase_through_terrain_frames);
                if (c.phase <= 1.f)
                    c.pos = rw::lerp(c.phase_from, room_view::middle_of_tile(stuck_pos(x, c)), mathf::min(1.f, c.phase));
                c.vel = vec2{0.f, 0.f};
                if (c.phase >= 1.f) {
                    c.phase = -1.f;
                    if (c.after_phase_stuck_pos == stuck_pos(x, c))
                        ++c.phases_to_same_location;
                    else
                        c.phases_to_same_location = 0;
                    c.after_phase_stuck_pos = stuck_pos(x, c);
                }
                if (i > 0 && n.c[i - 1].phase == -1.f) {
                    auto& prev = n.c[i - 1];
                    vec2 const a = custom::dir_vec(c.pos, prev.pos);
                    float const num = rw::distance(c.pos, prev.pos);
                    if (num > ideal) {
                        prev.pos += a * (ideal - num) * mass_deterioration_per_chunk;
                        prev.vel += a * (ideal - num) * mass_deterioration_per_chunk;
                    }
                    c.stretched_fac = ideal / mathf::max(0.01f, num);
                }
            }
        }

        // Tentacle.MoveSegment
        bool move_segment(ctx& x, int32_t s, ivec2 dest, bool allow_occupied, bool allow_solid) {
            auto& seg = x.seg;
            if (s == 0 || s >= int32_t(seg.size()) || seg[size_t(s)] == dest || (!are_neighbors(seg[size_t(s - 1)], dest) && seg[size_t(s - 1)] != dest)
                || (!allow_solid && has_any_solid(x.room, dest)))
                return false;
            if (dest == seg[size_t(s - 1)]) {
                if (!allow_occupied)
                    return false;
                seg.insert(seg.begin() + s, dest);
                seg.pop_back();
            } else if (std::abs(seg[size_t(s)].x - dest.x) > 1 || std::abs(seg[size_t(s)].y - dest.y) > 1) {
                if (!allow_occupied)
                    return false;
                ivec2 const prev = seg[size_t(s - 1)];
                seg.insert(seg.begin() + s, prev);
                seg.insert(seg.begin() + s, dest);
                seg.pop_back();
                seg.pop_back();
            } else {
                if (!allow_occupied)
                    for (auto const& q : seg)
                        if (q == dest)
                            return false;
                ivec2 const cand[2] = {ivec2{seg[size_t(s)].x, dest.y}, ivec2{dest.x, seg[size_t(s)].y}};
                ivec2 best = cand[0];
                float num = float_max;
                for (auto const& cc : cand) {
                    float num2 = 1.f;
                    if (has_any_solid(x.room, cc))
                        num2 += 1000.f;
                    for (auto const& q : seg)
                        if (cc == q)
                            num2 += 1.f;
                    if (num2 < num) {
                        best = cc;
                        num = num2;
                    }
                }
                if (!allow_occupied)
                    for (auto const& q : seg)
                        if (q == best)
                            return false;
                seg.insert(seg.begin() + s, best);
                seg.insert(seg.begin() + s, dest);
                seg.pop_back();
                seg.pop_back();
            }
            return true;
        }

        // Tentacle.Shorten
        void shorten(ctx& x, int32_t from, int32_t to, std::vector<ivec2> const& straight, int32_t count) {
            x.seg.erase(x.seg.begin() + from + 1, x.seg.begin() + to);
            for (int32_t i = count - 2; i > 0; --i)
                x.seg.insert(x.seg.begin() + from + 1, straight[size_t(i)]);
        }

        // Tentacle.PullAtTentacle
        void pull_at_tentacle(ctx& x, int32_t start, bool only_simple_pull, std::vector<ivec2>& path) {
            auto& n = x.n;
            auto& seg = x.seg;
            if (!only_simple_pull) {
                while (n.pull_counter > 0) {
                    bool flag = false;
                    for (int32_t num = start; num < int32_t(seg.size()) - 3 && !flag; ++num) {
                        for (int32_t i = int32_t(seg.size()) - 1; i >= num + 3; --i) {
                            int32_t const num2 = i - num - custom::manhattan_distance(seg[size_t(num)], seg[size_t(i)]);
                            if (num2 > 0 && num2 <= n.pull_counter && visual(x, seg[size_t(num)], seg[size_t(i)])) {
                                int32_t const num3 = ray_trace_tiles_list(seg[size_t(num)].x, seg[size_t(num)].y, seg[size_t(i)].x, seg[size_t(i)].y, path);
                                bool flag2 = true;
                                if (num2 > 1 && num2 <= n.pulls_this_tick)
                                    for (int32_t j = 1; j < num3 - 1; ++j)
                                        for (int32_t k = num + 1; k < i; ++k)
                                            if (custom::manhattan_distance(seg[size_t(k)], path[size_t(j)]) > 2 && !visual(x, seg[size_t(k)], path[size_t(j)])) {
                                                flag2 = false;
                                                break;
                                            }
                                if (flag2) {
                                    n.pulls_this_tick -= num2;
                                    n.pull_counter -= num2;
                                    shorten(x, num, i, path, num3);
                                    flag = true;
                                    break;
                                }
                            }
                        }
                    }
                    if (!flag)
                        break;
                }
            }
            while (n.pull_counter > 0 && n.pulls_this_tick > 0 && seg.size() > 1) {
                seg.pop_back();
                --n.pull_counter;
                --n.pulls_this_tick;
            }
        }

        // Tentacle.FindTentaclePath: a greedy best first search from the goal
        // back to the start over every tile (solid ones at 1000 extra); the
        // open list is a heap on (heuristic, insertion order), the pick of
        // vanilla's first-lowest scan of its List
        int32_t find_tentacle_path(ctx& x, ivec2 start, ivec2 goal, std::vector<ivec2>& path) {
            struct cell {
                ivec2 pos;
                int32_t parent;
            };
            int32_t const w = x.room.width, h = x.room.height;
            std::vector<uint8_t> visited(size_t(w) * size_t(h), 0);
            std::vector<cell> cells;
            using node = std::pair<float, uint32_t>;
            std::priority_queue<node, std::vector<node>, std::greater<>> open;
            cells.push_back(cell{goal, -1});
            open.push({0.f, 0});
            int32_t found = -1;
            vec2 const s = rw::to_vec2(start);
            while (!open.empty() && found < 0) {
                uint32_t const ci = open.top().second;
                open.pop();
                ivec2 const p = cells[ci].pos;
                for (auto const dd : custom::four_directions) {
                    ivec2 const q = p + dd;
                    if (q.x < 0 || q.x >= w || q.y < 0 || q.y >= h)
                        continue;
                    uint8_t& v = visited[size_t(q.x) * size_t(h) + size_t(q.y)];
                    if (v)
                        continue;
                    v = 1;
                    float const num2 = rw::distance(rw::to_vec2(q), s) + (has_any_solid(x.room, q) ? 1000.f : 0.f);
                    cells.push_back(cell{q, int32_t(ci)});
                    if (q == start) {
                        found = int32_t(cells.size() - 1);
                        break;
                    }
                    open.push({num2, uint32_t(cells.size() - 1)});
                }
            }
            int32_t k = 0;
            for (int32_t i = found; i >= 0 && cells[size_t(i)].parent >= 0; i = cells[size_t(i)].parent) {
                if (int32_t(path.size()) <= k)
                    path.push_back(cells[size_t(i)].pos);
                else
                    path[size_t(k)] = cells[size_t(i)].pos;
                ++k;
            }
            return k;
        }

        // Tentacle.MoveAlignedSegmentsWithPath
        void move_aligned_segments_with_path(ctx& x, std::vector<ivec2>& path) {
            auto& seg = x.seg;
            auto& gp = x.gp;
            int32_t const lim = std::min(int32_t(seg.size()) - 1, int32_t(gp.size()) - 1);
            int32_t num = lim;
            int32_t k = 0;
            while (k < lim && float_dist(seg[size_t(k + 1)], gp[size_t(k + 1)]) <= tile_tentacle_snap_with_path_distance && visual(x, seg[size_t(k + 1)], gp[size_t(k + 1)])) {
                num = k + 1;
                seg[size_t(k)] = gp[size_t(k)];
                ++k;
            }
            if (num < 0)
                return;
            int32_t const num3 = ray_trace_tiles_list(gp[size_t(num)].x, gp[size_t(num)].y, seg[size_t(num)].x, seg[size_t(num)].y, path);
            for (int32_t i = num3 - 2; i >= 0; --i)
                seg.insert(seg.begin() + num, path[size_t(i)]);
            x.n.pull_counter += num3 - 1;
            pull_at_tentacle(x, num, true, path);
        }

        // Tentacle.FindGrabPath
        void find_grab_path(ctx& x, std::vector<ivec2>& path) {
            ivec2 const g = *grab_dest(x);
            ivec2 const b = base_pos(x);
            int32_t num;
            if (!inside(x.room, g) || !inside(x.room, b) || visual(x, b, g))
                num = ray_trace_tiles_list(b.x, b.y, g.x, g.y, x.gp);
            else
                num = find_tentacle_path(x, b, g, x.gp);
            if (int32_t(x.gp.size()) > num)
                x.gp.resize(size_t(num));
            move_aligned_segments_with_path(x, path);
        }

        // Tentacle.MoveGrabDest
        void move_grab_dest(ctx& x, vec2 to) {
            if (auto const g = grab_dest(x); g && *g == room_view::get_tile_position(to))
                return;
            x.n.float_grab_dest = to;
            find_grab_path(x, x.scratch);
        }

        // Tentacle.Grow
        void grow(ctx& x, int32_t add) {
            auto& seg = x.seg;
            for (int32_t i = 0; i < add; ++i) {
                float num = float_max;
                ivec2 item = seg.back();
                for (auto const dd : four_directions_and_zero) {
                    float num2 = x.rng.value();
                    ivec2 const cc = seg.back() + dd;
                    if (has_any_solid(x.room, cc))
                        num2 += 10000.f;
                    for (auto const& q : seg)
                        if (q == cc)
                            num2 += 1.f;
                    if (num2 < num) {
                        num = num2;
                        item = cc;
                    }
                }
                seg.push_back(item);
            }
        }

        // Tentacle.MoveBase
        void move_base(ctx& x, ivec2 to, std::vector<ivec2>& path) {
            auto& seg = x.seg;
            if (has_any_solid(x.room, to) || to == base_pos(x))
                return;
            if (are_neighbors(seg[0], to)) {
                int32_t num = -1;
                for (int32_t i = 1; i < int32_t(seg.size()) - 1; ++i)
                    if (seg[size_t(i)] == to)
                        num = i;
                if (num > 0) {
                    seg.erase(seg.begin(), seg.begin() + num);
                    grow(x, num);
                } else {
                    seg.insert(seg.begin(), to);
                    ++x.n.pull_counter;
                    if (x.n.pull_counter > 4)
                        pull_at_tentacle(x, 0, false, path);
                }
            } else {
                int32_t num2;
                if (!inside(x.room, seg[0]) || !inside(x.room, to) || visual(x, seg[0], to))
                    num2 = ray_trace_tiles_list(seg[0].x, seg[0].y, to.x, to.y, path);
                else
                    num2 = find_tentacle_path(x, seg[0], to, path);
                ivec2 b = seg[0];
                for (int32_t j = 1; j < num2; ++j) {
                    seg.insert(seg.begin(), seg[0] + (path[size_t(j)] - b));
                    b = path[size_t(j)];
                }
                x.n.pull_counter += num2 - 1;
                if (x.n.pull_counter > 4)
                    pull_at_tentacle(x, 0, false, path);
            }
            if (grab_dest(x))
                find_grab_path(x, path);
        }

        // Tentacle.AlignWithGrabPath
        void align_with_grab_path(ctx& x, std::vector<ivec2>& path) {
            auto& seg = x.seg;
            auto& gp = x.gp;
            bool flag = false;
            for (int32_t i = std::min(int32_t(seg.size()) - 2, int32_t(gp.size()) - 1); i > 0; --i) {
                if (i >= int32_t(seg.size()) || i >= int32_t(gp.size()))
                    continue;
                if (seg[size_t(i)] != gp[size_t(i)] && visual(x, seg[size_t(i)], gp[size_t(i)])) {
                    int32_t const k = ray_trace_tiles_list(seg[size_t(i)].x, seg[size_t(i)].y, gp[size_t(i)].x, gp[size_t(i)].y, path);
                    if (k > 1 && move_segment(x, i + 1, path[1], false, false))
                        flag = true;
                }
            }
            if (!flag)
                for (int32_t k = 1; k < int32_t(seg.size()) && k < int32_t(gp.size()); ++k)
                    if (seg[size_t(k)] != gp[size_t(k)]) {
                        move_segment(x, k, gp[size_t(k)], true, false);
                        return;
                    }
        }

        // Tentacle.CurrentLength
        float current_length(ctx& x) {
            float num = float(x.seg.size()) * 20.f;
            for (size_t i = 0; i + 2 < x.seg.size(); ++i)
                if (are_diagonal_neighbors(x.seg[i], x.seg[i + 2]))
                    num -= 5.9000006f;
            return num;
        }

        // Tentacle.AdjustLength (tProps.shorten: no longer than the grab path)
        void adjust_length(ctx& x, std::vector<ivec2>& path) {
            float const num = current_length(x);
            float num2 = neck_ideal_length;
            if (!x.gp.empty())
                num2 = mathf::min(neck_ideal_length, float(x.gp.size()) * 20.f);
            int32_t const num3 = int32_t(num2 / 20.f - num / 20.f);
            if (num3 > 0 && (x.gp.empty() || x.seg.size() < x.gp.size())) {
                grow(x, 1);
                return;
            }
            if (num3 < 0) {
                ++x.n.pull_counter;
                pull_at_tentacle(x, 0, false, path);
            }
        }

        // Tentacle.Gravity, GravityDirection
        void neck_gravity(ctx& x) {
            ivec2 b{0, -1};
            if (x.rng.value() < 0.5f)
                b = ivec2{x.rng.value() < 0.5f ? -1 : 1, -1};
            bool flag = false;
            for (int32_t i = 0; i < int32_t(x.seg.size()) && !flag; ++i)
                if (move_segment(x, i, x.seg[size_t(i)] + b, false, false))
                    flag = true;
            if (!flag)
                for (int32_t i = 0; i < int32_t(x.seg.size()); ++i)
                    if (move_segment(x, i, x.seg[size_t(i)] + b, true, false))
                        break;
        }

        // Tentacle.Update
        void tentacle_update(ctx& x) {
            auto& path = x.scratch;
            move_base(x, room_view::get_tile_position(float_base(x)), path);
            x.n.update_counter += tile_tentacle_update_speed;
            if (x.n.update_counter >= 1.f) {
                x.n.pulls_this_tick = max_pull_tiles_per_tick;
                if (x.n.limp)
                    neck_gravity(x);
                else if (!x.gp.empty())
                    align_with_grab_path(x, path);
                adjust_length(x, path);
                x.n.update_counter -= 1.f;
            }
            base_pos(x);
            x.n.backtrack_from = -1;
            for (uint32_t i = 0; i < neck_chunks; ++i)
                chunk_update(x, i);
        }

        // Tentacle.NewRoom: the segments at the main chunk's tile, the chunks reset
        void neck_new_room(ctx& x) {
            x.seg.assign(size_t(neck_ideal_length / 20.f), room_view::get_tile_position(x.pos(0)));
            x.gp.clear();
            for (auto& c : x.n.c)
                chunk_reset(x, c);
        }
        // Tentacle.Reset
        void neck_reset(ctx& x, vec2 at) {
            x.seg.assign(1, room_view::get_tile_position(at));
            for (auto& c : x.n.c) {
                c.pos = at;
                c.vel = vec2{0.f, 0.f};
                chunk_reset(x, c);
            }
        }

        //
        // AI: a simpler MirosBirdAI
        //
        // the tracker's settings (MirosBirdAI's constructor)
        tracker::params const& tracker_params() {
            static tracker::params const p = [] {
                tracker::params q;
                q.see_around_corners = 4;
                q.frames_to_remember = forget_time;
                q.ghost_dismissal_range = 5;
                q.prey_max = 5;
                q.persistance_bias = 1.f;
                q.sure_to_get = 5.f;
                q.sure_to_lose = 15.f;
                q.threat_max = 3;
                return q;
            }();
            return p;
        }

        // MirosBirdAI.VisualScore > 0 and the room's visual contact, from the head
        bool visual_contact(ctx& x, vec2 look) {
            vec2 const eye = x.pos(head);
            if (!custom::dist_less(eye, look, visual_radius))
                return false;
            float score = mathf::inverse_lerp(visual_radius, 0.f, rw::distance(eye, look));
            score -= mathf::inverse_lerp(0.7f, 0.3f, rw::dot(rw::normalized(x.tip().pos - look), rw::normalized(x.tip().pos - eye)));
            return score > 0.f && x.room.visual_contact(eye, look);
        }

        // IUseARelationshipTracker.ModuleToTrackRelationship
        creature::track_module module_of(relationship r) noexcept {
            if (r.type == creature::rel::afraid)
                return creature::track_module::threat;
            if (r.type == creature::rel::eats)
                return creature::track_module::prey;
            return creature::track_module::none;
        }

        bool hook_sees(void* u, dcon::body_chunk_id k) {
            auto& x = *static_cast<ctx*>(u);
            return visual_contact(x, x.state.world.body_chunk_get_pos(k));
        }
        bool hook_sees_point(void* u, vec2 p) {
            return visual_contact(*static_cast<ctx*>(u), p);
        }
        bool hook_reach(void* u, ivec2 t) {
            auto& x = *static_cast<ctx*>(u);
            if (!inside(x.room, t) || !pathing::accessible(x.room, t, walker))
                return false;
            return pathing::distance(x.state, x.path(), t) != pathing::unreachable;
        }
        tracker::hooks make_hooks(ctx& x) {
            tracker::hooks h;
            h.user = &x;
            h.sees = hook_sees;
            h.sees_point = hook_sees_point;
            h.module = module_of;
            h.reach = hook_reach;
            return h;
        }

        // MirosBirdAI.DoIWantToBiteCreature
        bool wants_to_bite(sys::game_state const& state, dcon::creature_id t) {
            auto const tt = creature::template_of(state, t);
            return tt != creature::tmpl::miros_bird && !creature::small_creature(tt);
        }

        // the roaming spot (not vanilla: there its flock's abstract AI moves it about)
        void roam(ctx& x, ivec2 from) {
            auto& w = x.w();
            ivec2& spot = w.miros_data_get_idle_pos(x.d);
            int32_t& counter = w.miros_data_get_idle_counter(x.d);
            --counter;
            if (custom::manhattan_distance(from, spot) < 3 && counter > 160)
                counter = x.rng.range(20, 160);
            if (counter <= 0 || !open_floor(x.room, spot)) {
                // a couple of random floor spots a tick, until one is reachable
                for (int32_t k = 0; k < 2; ++k) {
                    ivec2 const t = ground_tile(x.room, ivec2{x.rng.range(0, x.room.width), x.rng.range(0, x.room.height)});
                    if (!open_floor(x.room, t))
                        continue;
                    if (pathing::distance(x.state, pathing::field_to(x.state, x.room.id, t, walker), from) == pathing::unreachable)
                        continue;
                    spot = t;
                    counter = x.rng.range(200, 800);
                    break;
                }
            }
            x.destination() = open_floor(x.room, spot) ? spot : from;
        }

        // MirosBirdAI.Update
        void ai_update(ctx& x) {
            auto& w = x.w();
            ivec2 const from = ground(x);
            tracker::hooks const h = make_hooks(x);
            tracker::update(x.state, x.c, x.room, from, tracker_params(), h);
            auto const& ts = tracker::of(x.state, x.c);
            // UtilityComparer: threat (0.9), prey (0.5), rain (0.9)
            auto const* prey_rep = tracker::prey(ts);
            float utility = tracker::threat_utility(ts) * 0.9f;
            behavior b = behavior::flee;
            if (float const hunt = prey_rep ? tracker::prey_utility(x.state, x.c, x.room, from, tracker_params()) * 0.5f : 0.f; hunt > utility) {
                utility = hunt;
                b = behavior::hunt;
            }
            if (float const rain = dens::rain_utility(x.state, x.c, x.room.id) * 0.9f; rain > utility) {
                utility = rain;
                b = behavior::escape_rain;
            }
            if (utility < 0.1f || (b == behavior::flee && !tracker::threat(ts)))
                b = behavior::idle;
            if (x.grasped() && b != behavior::flee && b != behavior::escape_rain)
                b = behavior::return_prey;
            w.creature_set_behavior(x.c, uint8_t(b));
            w.creature_set_current_utility(x.c, utility);
            switch (b) {
            case behavior::idle:
                migrate::idle_roll(x.state, x.c, x.room);
                if (auto const t = migrate::travel_tile(x.state, x.c, x.room)) {
                    x.destination() = *t;
                    break;
                }
                roam(x, from);
                break;
            case behavior::flee:
                w.miros_data_set_focus(x.d, ts.threat);
                x.destination() = tracker::flee_to(x.state, x.c, x.room, from, 5, 30, tracker_params(), h);
                break;
            case behavior::escape_rain:
            case behavior::return_prey:
                // DenFinder: the den (dens.hpp)
                if (auto const t = dens::home_tile(x.state, x.c, x.room))
                    x.destination() = *t;
                break;
            case behavior::hunt:
                w.miros_data_set_focus(x.d, prey_rep->who);
                x.destination() = tracker::best_guess(*prey_rep);
                break;
            }
            // (a field asked for before the destination changed is stale)
            x.field.reset();
        }

        // CreatureLooker.lookCreature: the creature it focuses on, if it
        // still tracks it
        creature::tracked const* look_creature(ctx& x) {
            auto const f = x.w().miros_data_get_focus(x.d);
            if (!f)
                return nullptr;
            return tracker::find(tracker::of(x.state, x.c), f);
        }

        //
        // MirosBird
        //
        // PhysicalObject.WeightedPush
        void weighted_push(ctx& x, uint32_t a, uint32_t b, vec2 dir, float frc) {
            float const num = x.mass(b) / (x.mass(a) + x.mass(b));
            x.vel(a) += dir * frc * num;
            x.vel(b) -= dir * frc * (1.f - num);
        }

        // MirosBird.RunCycle
        float run_cycle(ctx& x, float spot, float time_stacker) {
            float num = mathf::lerp(x.p.last_run_cycle, x.p.run_cycle, time_stacker) + spot;
            if (num < 0.f)
                num += std::floor(std::fabs(num) + 1.f);
            return num - std::floor(num);
        }

        // MirosBird.Stun (+ Creature.Stun)
        void stun(sys::game_state& state, dcon::creature_id c, int32_t st) {
            auto& w = state.world;
            if (rw::random(state.seed, state.tick, uint32_t(c.value)).value() < mathf::inverse_lerp(float(st), 0.f, 30.f))
                creature::release_all(state, c, creature::room_of(state, c));
            w.creature_set_stun(c, std::max(w.creature_get_stun(c), st));
        }

        // MirosBird.CheckFlip: the hips swapped when the body turned over
        void check_flip(ctx& x) {
            auto& w = x.w();
            if (custom::distance_to_line(x.pos(3), x.pos(1), x.pos(0)) >= 0.f)
                return;
            auto const a = x.ch[2], b = x.ch[3];
            std::swap(w.body_chunk_get_pos(a), w.body_chunk_get_pos(b));
            std::swap(w.body_chunk_get_vel(a), w.body_chunk_get_vel(b));
            std::swap(w.body_chunk_get_last_pos(a), w.body_chunk_get_last_pos(b));
            std::swap(w.body_chunk_get_last_last_pos(a), w.body_chunk_get_last_last_pos(b));
        }

        // MirosBird.JawSlamShut: the beak snaps shut on what is in front of it
        void jaw_slam_shut(ctx& x) {
            auto& w = x.w();
            vec2 const a = custom::dir_vec(x.tip().pos, x.pos(head));
            x.tip().vel -= a * 10.f;
            x.tip().pos += a * 20.f;
            x.pos(head) += a * 20.f;
            // (no snap sounds: the Miros_Beak_Snap ids aren't in the sound table)
            rooms::for_each_creature(x.state, x.room.id, [&](dcon::creature_id t) {
                if (x.grasped() || t == x.c || !creature::here(x.state, t, x.room.id) || !wants_to_bite(x.state, t) || w.creature_get_entering_shortcut(t).x >= 0)
                    return;
                uint32_t const m = creature::chunk_count_of(x.state, t);
                for (uint32_t k = 0; k < m; ++k) {
                    auto const kc = creature::chunk_of(x.state, t, k);
                    vec2 const kp = w.body_chunk_get_pos(kc);
                    if (custom::dist_less(x.pos(head) + a * 20.f, kp, 20.f + w.body_chunk_get_rad(kc)) && x.room.visual_contact(x.pos(head), kp)) {
                        creature::grab(x.state, x.c, 0, t, k);
                        x.p.jaw_open = 0.15f;
                        x.p.jaw_vel = 0.f;
                        creature::violence_on(x.state, t, k, custom::dir_vec(x.pos(head), kp) * 4.f, 1.2f, 0.f, creature::damage_type::bite, creature::tmpl::miros_bird);
                        break;
                    }
                }
                // (a daddy's tentacles bitten aren't stunned)
            });
        }

        // MirosBird.BirdLeg's sparks (cosmetic: rolled from the graphics
        // module's random, only where a camera looks)
        void sparks(ctx& x, leg& l, vec2 last_ps, vec2 ps, bool big) {
            if (!x.viewed)
                return;
            // per leg, the slip's apart from the step's
            auto g = rw::random(x.state.seed, x.state.tick, rw::ids(uint32_t(x.c.value), uint32_t(&l - x.legs.l) * 2u + (big ? 1u : 0u)));
            float const dist = rw::distance(last_ps, ps);
            int32_t const count = big ? 6 + int32_t(dist / 5.f) : 1 + int32_t(dist / 12.f);
            vec2 const knee = l.joints[1].pos;
            for (int32_t i = 0; i < count; ++i) {
                vec2 const at = rw::lerp(last_ps, ps, g.value()) + custom::dir_vec(ps, knee);
                vec2 const v = custom::rnv(g) * (big ? 12.f : 6.f) + (ps - last_ps) * 0.4f;
                float const life = big ? mathf::lerp(11.f, 60.f, g.value()) : mathf::lerp(5.f, 18.f, g.value());
                particles::spawn(x.state, x.room.id, particles::kind::spark, at, v, 2.f, life / 40.f, vec3{1.f, 1.f, 0.8f});
            }
            l.light_up = big ? g.range(3, 6) + int32_t(custom::lerp_map(dist, 20.f, 40.f, 1.f, 3.f)) : g.range(2, 4) + int32_t(custom::lerp_map(dist, 10.f, 40.f, 0.f, 3.f));
            l.light_up_pos1 = last_ps;
            l.light_up_pos2 = ps;
        }

        // BirdLeg.ConnectionSide, ConnectionPos
        vec2 connection_pos(ctx& x, uint32_t i) {
            auto const& l = x.legs.l[i];
            float const side = mathf::lerp(0.5f + l.flip * 0.5f, 0.5f + 0.5f * std::sin(run_cycle(x, float(i) * 0.5f, 1.f) * mathf::pi * 2.f), l.run_mode);
            return rw::lerp(x.pos(2), x.pos(3), side);
        }

        // BirdLeg.Slip
        void slip(ctx& x, leg& l, vec2 last_ps, vec2 ps) {
            vec2 const md = rw::normalized(x.p.move_dir);
            x.pos(0) += md * 12.f;
            x.vel(0) += md * 12.f;
            x.pos(1) += md * 12.f;
            x.vel(1) += md * 12.f;
            sparks(x, l, last_ps, ps, true);
        }

        // BirdLeg.RunMode: the legs spring off the ground in turn
        void leg_run_mode(ctx& x, uint32_t i, ivec2) {
            auto& l = x.legs.l[i];
            vec2 const md = x.p.move_dir;
            l.flip = mathf::lerp(l.flip, x.p.body_flip, 0.2f);
            vec2 const conn = connection_pos(x, i);
            vec2 const v = rw::slerp(custom::dir_vec(x.pos(1), x.pos(0)), vec2{0.f, -1.f}, 0.75f);
            vec2 v2 = custom::deg_to_vec(360.f * run_cycle(x, float(i) * 0.5f, 1.f));
            v2 = flatten_vector_along_axis(v2, custom::vec_to_deg(v) + l.flip * 30.f, 0.3f);
            vec2 v3 = conn + v * 20.f + v2 * 35.f + custom::perpendicular_vector(v) * l.flip * 10.f;
            if (!custom::dist_less(conn, v3, tigh_length))
                v3 = conn + custom::dir_vec(conn, v3) * tigh_length;
            if (auto const hit = exact_terrain_ray_trace_pos(x.room, conn, v3))
                v3 = *hit + custom::dir_vec(*hit, conn);
            v2 = custom::deg_to_vec(360.f * run_cycle(x, float(i) * 0.5f + std::sin(run_cycle(x, float(i) * 0.5f, 1.f) * mathf::pi * 2.f) * 0.01f * l.flip, 1.f));
            v2 = flatten_vector_along_axis(v2, custom::vec_to_deg(v), 0.8f);
            vec2 const v5 = v3 + custom::dir_vec(v3, x.pos(0) + v * 50.f + v2 * 55.f) * lower_leg_length;
            vec2 v6 = v5;
            if (auto const hit = exact_terrain_ray_trace_pos(x.room, v3, v5)) {
                v6 = *hit;
                l.spring_power = 1.f - rw::distance(v3, v6) / lower_leg_length;
                l.spring_dir = custom::dir_vec(v5, v3);
                l.forward_power = mathf::inverse_lerp(-0.5f, 0.5f, rw::dot(rw::normalized(md), l.spring_dir));
                if (l.last_spring_pow > 0.f) {
                    float const dd = mathf::clamp(l.spring_power - l.last_spring_pow, 0.f, 0.2f);
                    x.pos(0) += l.spring_dir * dd * lower_leg_length;
                    x.vel(0) += l.spring_dir * dd * lower_leg_length;
                    if (l.foot_secure_pos) {
                        vec2 const fs = *l.foot_secure_pos;
                        if (l.foot_secure_frames > 4 && !custom::dist_less(v6, fs, 28.f) && v6.y == fs.y && (v6.x - fs.x < 0.f) != (md.x < 0.f)) {
                            slip(x, l, fs, v6);
                            l.foot_secure_pos = v6;
                        } else {
                            v6 = fs;
                        }
                    }
                } else {
                    l.foot_secure_pos = v6;
                }
                float const a = rw::distance(conn, v3);
                v3 = rw::lerp(custom::inverse_kinematic(conn, v6, a, lower_leg_length, -l.flip), v3, 0.5f);
                l.ground_contact = 1;
            } else {
                l.foot_secure_pos = std::nullopt;
            }
            auto& hip = l.joints[0];
            auto& knee = l.joints[1];
            auto& foot = l.joints[2];
            float const speed = custom::lerp_map(l.run_mode, 0.5f, 1.f, 5.f, 40.f);
            hip.last_pos = hip.pos;
            hip.pos = conn;
            knee.last_pos = knee.pos;
            knee.pos += x.vel(0);
            knee.pos += rw::clamp_magnitude(v3 - knee.pos, speed);
            foot.last_pos = foot.pos;
            foot.pos += x.vel(0);
            foot.pos += rw::clamp_magnitude(v6 - foot.pos, speed);
            l.move_from_pos = foot.pos;
            l.move_progress = 0.f;
            l.move_to_pos = std::nullopt;
        }

        // BirdLeg.StandMode: a step at a time, the foot set down where it reaches the ground
        void leg_stand_mode(ctx& x, uint32_t i, ivec2 from) {
            auto& l = x.legs.l[i];
            vec2 const md = x.p.move_dir;
            l.flip = mathf::lerp(l.flip, -1.f + 2.f * float(i), 0.2f);
            auto& hip = l.joints[0];
            auto& knee = l.joints[1];
            auto& foot = l.joints[2];
            hip.last_pos = hip.pos;
            hip.pos = connection_pos(x, i);
            vec2 const a = rw::slerp(custom::dir_vec(x.pos(1), x.pos(0)), vec2{0.f, -1.f}, 0.75f);
            vec2 v = hip.pos + rw::normalized(rw::normalized(a * 1.8f + custom::dir_vec(x.pos(0), hip.pos)) * 0.7f + vec2{md.x, md.y * 0.4f}) * (tigh_length + lower_leg_length);
            foot.last_pos = foot.pos;
            if (l.move_to_pos) {
                l.move_progress = mathf::min(1.f, l.move_progress + 0.1f);
                v = rw::lerp(l.move_from_pos, *l.move_to_pos, l.move_progress);
                foot.pos = v;
                if (l.move_progress >= 1.f) {
                    l.foot_secure_pos = l.move_to_pos;
                    l.move_to_pos = std::nullopt;
                    l.move_progress = 0.f;
                }
            } else if (l.foot_secure_pos) {
                l.forward_power = custom::lerp_map(rw::dot(custom::dir_vec(foot.pos, x.pos(0)), md), -1.f, 1.f, 0.f, 0.2f);
                l.ground_contact = 1;
                foot.pos = *l.foot_secure_pos;
                if (!custom::dist_less(hip.pos, *l.foot_secure_pos, tigh_length + lower_leg_length + (x.legs.l[1 - i].ground_contact ? 0.f : 20.f)))
                    l.foot_secure_pos = std::nullopt;
            } else {
                foot.pos += rw::clamp_magnitude(v - foot.pos, 10.f);
                l.move_from_pos = foot.pos;
                l.move_progress = 0.f;
                if (auto const hit = exact_terrain_ray_trace_pos(x.room, hip.pos, v)) {
                    l.move_to_pos = *hit;
                    l.ground_contact = 1;
                }
            }
            vec2 const a2 = custom::inverse_kinematic(hip.pos, foot.pos, tigh_length, lower_leg_length, -l.flip);
            if (auto const hit = exact_terrain_ray_trace_pos(x.room, a2, foot.pos))
                foot.pos = *hit;
            knee.last_pos = knee.pos;
            knee.pos += rw::clamp_magnitude(rw::lerp(a2, custom::inverse_kinematic(hip.pos, foot.pos, tigh_length, lower_leg_length, -l.flip), 0.5f) - knee.pos, 40.f);
            if (auto const hit = exact_terrain_ray_trace_pos(x.room, hip.pos, knee.pos)) {
                l.forward_power = mathf::max(l.forward_power, 0.08f);
                knee.pos = *hit;
            }
            // at the destination with both feet down on one side: the
            // trailing one steps over
            auto& l0 = x.legs.l[0];
            auto& l1 = x.legs.l[1];
            if (i == 1 && l.foot_secure_pos && l0.foot_secure_pos && custom::manhattan_distance(x.destination(), from) < 4
                && (l0.joints[2].pos.x < x.pos(0).x) == (l1.joints[2].pos.x < x.pos(0).x)) {
                auto& bl = x.legs.l[(md.x < 0.f) == (l0.joints[2].pos.x < l1.joints[2].pos.x) ? 0 : 1];
                if (bl.foot_secure_pos) {
                    bl.move_from_pos = *bl.foot_secure_pos;
                    bl.move_progress = 0.f;
                    bl.foot_secure_pos = std::nullopt;
                }
            }
        }

        // BirdLeg.Update
        void leg_update(ctx& x, uint32_t i, ivec2 from) {
            auto& l = x.legs.l[i];
            vec2 const md = x.p.move_dir;
            l.last_foot_secure_pos = l.foot_secure_pos;
            l.last_spring_pow = l.spring_power;
            l.spring_power = 0.f;
            l.forward_power = 0.f;
            l.ground_contact = 0;
            l.last_run_mode = l.run_mode;
            if (std::fabs(x.p.body_flip) > (md.y < -0.1f ? 0.1f : 0.3f) && std::fabs(x.vel(0).x) > 3.f && (std::fabs(md.x) > 0.5f || md.y < -0.1f)
                && custom::manhattan_distance(x.destination(), from) > 3)
                l.run_mode = mathf::min(1.f, l.run_mode + 0.025f);
            else
                l.run_mode = mathf::max(0.f, l.run_mode - 0.1f);
            if (l.run_mode > (l.mode_conservative_bias ? 0.4f : 0.6f)) {
                leg_run_mode(x, i, from);
                l.mode_conservative_bias = 1;
            } else {
                leg_stand_mode(x, i, from);
                l.mode_conservative_bias = 0;
            }
            if (l.foot_secure_pos && l.last_foot_secure_pos && *l.foot_secure_pos == *l.last_foot_secure_pos)
                ++l.foot_secure_frames;
            else
                l.foot_secure_frames = 0;
            auto& knee = l.joints[1];
            auto& foot = l.joints[2];
            // (the piston sounds and their noises aren't ported: no Miros sound ids)
            if (!l.foot_secure_pos && l.last_foot_secure_pos && (foot.pos.x - l.last_foot_secure_pos->x < 0.f) != (md.x < 0.f)
                && !custom::dist_less(foot.pos, *l.last_foot_secure_pos, 18.f) && !custom::dist_less(foot.pos, foot.last_pos, 18.f))
                sparks(x, l, *l.last_foot_secure_pos, foot.pos, false);
            if (l.foot_secure_pos && !l.last_foot_secure_pos && !custom::dist_less(foot.pos, foot.last_pos, 60.f))
                sparks(x, l, foot.pos, foot.pos, false);
            if (l.light_up > 0)
                --l.light_up;
            auto const& hip = l.joints[0];
            if (!custom::dist_less(knee.pos, hip.pos, tigh_length))
                knee.pos = hip.pos + custom::dir_vec(hip.pos, knee.pos) * tigh_length;
            float const reach = lower_leg_length + (l.ground_contact ? 20.f : 0.f);
            if (!custom::dist_less(knee.pos, foot.pos, reach))
                foot.pos = knee.pos + custom::dir_vec(knee.pos, foot.pos) * reach;
        }

        // BirdLeg.Reset
        void leg_reset(ctx& x, uint32_t i) {
            auto& l = x.legs.l[i];
            vec2 const conn = connection_pos(x, i);
            for (auto& j : l.joints)
                j.pos = j.last_pos = conn;
            l.foot_secure_pos = std::nullopt;
            l.last_foot_secure_pos = std::nullopt;
            l.move_to_pos = std::nullopt;
        }

        // MirosBird.FollowPath over the floor field (from the floor tile
        // under the main chunk, else under another chunk), the body steered
        // at the path `body_lift` up, looking ahead while the path is in sight
        void follow_path(ctx& x) {
            auto const f = x.path();
            auto const step_from = [&](ivec2 t) -> std::optional<ivec2> {
                if (!inside(x.room, t) || !pathing::accessible(x.room, t, walker))
                    return std::nullopt;
                return pathing::next_step(x.state, f, t);
            };
            uint32_t num = 0;
            ivec2 start = ground(x);
            auto step = step_from(start);
            for (uint32_t i = 1; i < 4 && !step; ++i) {
                start = ground_tile(x.room, room_view::get_tile_position(x.pos(i)));
                step = step_from(start);
                if (step)
                    num = i;
            }
            // (vanilla's last resort, the most accessible tile around, has no
            // counterpart over the floor field: it keeps pushing up)
            if (!step)
                return;
            vec2 const lift{0.f, body_lift};
            vec2 md = custom::dir_vec(x.pos(num), room_view::middle_of_tile(*step) + lift);
            vec2 const s = room_view::middle_of_tile(start);
            vec2 b = room_view::middle_of_tile(*step);
            ivec2 cur = *step;
            for (int32_t k = 0; k < 10; ++k) {
                auto const next = step_from(cur);
                if (!next || !ray_trace_tiles_for_terrain(x.room, start, *next) || rw::distance(s, b) > rw::distance(s, room_view::middle_of_tile(*next)))
                    break;
                b = room_view::middle_of_tile(*next);
                md += custom::dir_vec(x.pos(num), b + lift);
                cur = *next;
            }
            x.p.move_dir = rw::normalized(md);
        }

        // MirosBird.UpdateNeck
        void update_neck(ctx& x) {
            auto& n = x.n;
            tentacle_update(x);
            vec2 const a = x.p.move_dir;
            for (uint32_t i = 0; i < neck_chunks; ++i) {
                auto& c = n.c[i];
                float const t = float(i) / float(neck_chunks - 1);
                c.vel *= 0.95f;
                c.vel.y -= n.limp ? 0.7f : 0.1f;
                if (n.backtrack_from == -1 || n.backtrack_from > int32_t(i))
                    c.vel += custom::dir_vec(x.pos(0), x.pos(1)) * mathf::lerp(3.f, 1.f, t);
                c.vel -= x.vel(1);
                c.vel *= 0.75f;
                c.vel += x.vel(1);
            }
            bool const awake = x.awake();
            n.limp = awake ? 0 : 1;
            float const num = n.backtrack_from == -1 ? 0.5f : 0.f;
            auto& tip = x.tip();
            auto& mid = n.c[neck_chunks - 2];
            auto const held = x.grasped();
            if (!held) {
                vec2 const a2 = custom::dir_vec(x.pos(head), tip.pos);
                float const num2 = rw::distance(x.pos(head), tip.pos);
                x.pos(head) -= (6.f - num2) * a2 * (1.f - num);
                x.vel(head) -= (6.f - num2) * a2 * (1.f - num);
                tip.pos += (6.f - num2) * a2 * num;
                tip.vel += (6.f - num2) * a2 * num;
                x.vel(head) += custom::dir_vec(mid.pos, x.pos(head)) * 6.f * (1.f - num);
                x.vel(head) += custom::dir_vec(tip.pos, x.pos(head)) * 6.f * (1.f - num);
                tip.vel -= custom::dir_vec(mid.pos, x.pos(head)) * 6.f * num;
                mid.vel -= custom::dir_vec(mid.pos, x.pos(head)) * 6.f * num;
            }
            if (!awake) {
                n.retract_fac = 0.5f;
                n.float_grab_dest = std::nullopt;
                return;
            }
            x.vel(head).y += x.gravity();
            vec2 vector;
            if (auto const* look = look_creature(x); !look)
                vector = x.pos(0) + a * 400.f;
            else if (look->visual && creature::here(x.state, look->who, x.room.id))
                vector = creature::main_pos(x.state, look->who);
            else
                vector = room_view::middle_of_tile(tracker::best_guess(*look));
            float const num3 = x.run_mode();
            n.retract_fac = mathf::clamp01(mathf::lerp(0.5f, 0.8f, num3));
            vector = rw::lerp(vector, x.pos(0) + a * 200.f, mathf::pow(num3, 6.f));
            if ((custom::dist_less(vector, x.pos(0), 220.f) && !x.room.visual_contact(vector, x.pos(head))) || num3 > 0.5f)
                move_grab_dest(x, vector);
            else if (n.backtrack_from == -1)
                n.float_grab_dest = std::nullopt;
            vec2 const a3 = custom::dir_vec(x.pos(head), vector);
            if (!held) {
                tip.vel += a3 * num * 1.2f;
                mid.vel -= a3 * 0.5f * num;
                x.vel(head) += a3 * 6.f * (1.f - num);
            } else {
                tip.vel += a3 * 2.f * num;
                mid.vel -= a3 * 2.f * num;
                if (creature::here(x.state, held, x.room.id) && creature::grasp_chunk(x.w(), x.c, 0) < creature::chunk_count_of(x.state, held)) {
                    auto const gc = creature::chunk_of(x.state, held, creature::grasp_chunk(x.w(), x.c, 0));
                    x.w().body_chunk_get_vel(gc) += a3 / x.w().body_chunk_get_mass(gc);
                }
            }
            if (custom::dist_less(x.pos(head), vector, 80.f * mathf::inverse_lerp(1.f, 0.5f, x.p.jaw_open)))
                for (auto& c : n.c)
                    c.vel -= a3 * mathf::inverse_lerp(80.f, 20.f, rw::distance(x.pos(head), vector)) * 8.f * num;
        }

        // MirosBird.Carry: what the beak holds, dragged along
        void carry(ctx& x) {
            auto& w = x.w();
            if (!x.awake()) {
                creature::release_all(x.state, x.c, x.room.id);
                return;
            }
            auto const t = x.grasped();
            uint32_t const k = creature::grasp_chunk(w, x.c, 0);
            if (!creature::here(x.state, t, x.room.id) || k >= creature::chunk_count_of(x.state, t)) {
                creature::release_all(x.state, x.c, x.room.id);
                return;
            }
            if (x.rng.value() < 0.008333334f && creature::static_relationship(x.state, x.c, t).type != creature::rel::eats) {
                creature::release_all(x.state, x.c, x.room.id);
                return;
            }
            auto const gc = creature::chunk_of(x.state, t, k);
            vec2& gp = w.body_chunk_get_pos(gc);
            vec2& gv = w.body_chunk_get_vel(gc);
            float const gm = w.body_chunk_get_mass(gc);
            float num = gm / (gm + x.mass(head));
            float num2 = gm / (gm + x.mass(0));
            if (x.n.backtrack_from != -1) {
                num = 0.f;
                num2 = 0.f;
            }
            auto& tip = x.tip();
            if (!custom::dist_less(gp, tip.pos, 20.f)) {
                vec2 const a = custom::dir_vec(gp, tip.pos);
                float const num3 = rw::distance(gp, tip.pos);
                gp -= (20.f - num3) * a * (1.f - num);
                gv -= (20.f - num3) * a * (1.f - num);
                tip.pos += (20.f - num3) * a * num;
                tip.vel += (20.f - num3) * a * num;
            }
            x.pos(head) = rw::lerp(tip.pos, gp, 0.1f);
            x.vel(head) = tip.vel;
            constexpr float num4 = 40.f;
            if (!custom::dist_less(x.pos(0), gp, num4)) {
                if (!custom::dist_less(x.pos(0), gp, num4 * 3.f)) {
                    creature::release_all(x.state, x.c, x.room.id);
                    return;
                }
                vec2 const a2 = custom::dir_vec(gp, x.pos(0));
                float const num5 = rw::distance(gp, x.pos(0));
                gp -= (num4 - num5) * a2 * (1.f - num2);
                gv -= (num4 - num5) * a2 * (1.f - num2);
                x.pos(0) += (num4 - num5) * a2 * num2;
                x.vel(0) += (num4 - num5) * a2 * num2;
            }
            if (w.creature_get_entering_shortcut(t).x >= 0)
                creature::release_all(x.state, x.c, x.room.id);
        }

        // MirosBird.Act
        void act(ctx& x) {
            auto& p = x.p;
            auto& rng = x.rng;
            p.last_jaw_open = p.jaw_open;
            if (x.grasped()) {
                p.jaw_open = 0.15f;
            } else if (p.jaw_slam_pause > 0) {
                --p.jaw_slam_pause;
            } else {
                if (p.jaw_vel == 0.f)
                    p.jaw_vel = 0.15f;
                p.jaw_open += p.jaw_vel;
                if (p.jaw_keep_open_pause > 0) {
                    --p.jaw_keep_open_pause;
                    p.jaw_open = mathf::clamp(mathf::lerp(p.jaw_open, p.keep_jaw_open_pos, rng.value() * 0.5f), 0.f, 1.f);
                } else if (rng.value() < 1.f / 40.f) {
                    p.jaw_keep_open_pause = int16_t(rng.range(10, rng.range(10, 60)));
                    p.keep_jaw_open_pos = rng.value() < 0.5f ? 0.f : 1.f;
                    p.jaw_vel = mathf::lerp(-0.4f, 0.4f, rng.value());
                    p.jaw_open = mathf::clamp(p.jaw_open, 0.f, 1.f);
                } else if (p.jaw_open <= 0.f) {
                    p.jaw_open = 0.f;
                    if (p.jaw_vel < -0.4f)
                        jaw_slam_shut(x);
                    p.jaw_vel = 0.15f;
                    p.jaw_slam_pause = 5;
                } else if (p.jaw_open >= 1.f) {
                    p.jaw_open = 1.f;
                    p.jaw_vel = -0.5f;
                }
            }
            // stuck: back and forth over the same tiles
            ivec2 const at = x.at();
            ivec2 const from = ground(x);
            int32_t num = 0;
            if (custom::manhattan_distance(from, x.destination()) > 3) {
                auto& past = x.w().miros_data_get_past(x.d);
                for (int32_t i = std::min(past.count, past_positions::kept - 1); i > 0; --i)
                    past.tile[i] = past.tile[i - 1];
                past.tile[0] = at;
                past.count = std::min(past.count + 1, past_positions::kept);
                for (int32_t i = 20; i < past.count; ++i)
                    if (float_dist(at, past.tile[i]) < 4.f)
                        ++num;
            }
            int32_t stuck = p.stuck_counter;
            stuck = num > 10 ? stuck + 1 : stuck - 2;
            stuck = custom::int_clamp(stuck, 0, 200);
            p.stuck_counter = int16_t(stuck);
            if (stuck > 100) {
                if (rng.value() < 0.033333335f)
                    x.legs.l[rng.range(0, 2)].foot_secure_pos = std::nullopt;
                for (uint32_t j = 0; j < chunk_count; ++j)
                    x.vel(j) += custom::rnv(rng) * custom::lerp_map(float(stuck), 100.f, 200.f, 0.f, 3.f);
            }
            p.last_run_cycle = p.run_cycle;
            p.run_cycle += mathf::sign(p.body_flip)
                / mathf::lerp(mathf::lerp(30.f, 40.f, mathf::pow(mathf::max(x.legs.l[0].spring_power, x.legs.l[1].spring_power), 0.3f)),
                    custom::lerp_map(std::fabs(x.vel(0).x), 2.f, 10.f, 50.f, 20.f), 1.f);

            ai_update(x);

            // moveDir: at the destination straight at it (a pipe home: down
            // into it), else along the path
            p.move_dir = vec2{0.f, 1.f};
            ivec2 const dest = x.destination();
            bool const home = x.w().creature_get_going_home(x.c) != 0;
            vec2 const target = room_view::middle_of_tile(dest) + (home ? vec2{0.f, 0.f} : vec2{0.f, body_lift});
            if (inside(x.room, dest) && (custom::dist_less(x.pos(0), room_view::middle_of_tile(dest), 30.f) || custom::manhattan_distance(from, dest) <= (home ? 3 : 1)))
                p.move_dir = rw::clamp_magnitude(target - x.pos(0), 30.f) / 30.f;
            else
                follow_path(x);
            vec2 const md = p.move_dir;

            p.last_body_flip = p.body_flip;
            if (std::fabs(md.x) < 0.3f)
                p.body_flip *= 0.7f;
            else if (md.x < 0.f)
                p.body_flip = mathf::max(-1.f, p.body_flip - 0.1f);
            else
                p.body_flip = mathf::min(1.f, p.body_flip + 0.1f);
            float num2 = 0.f;
            float const num3 = p.forward_power;
            p.forward_power = 0.f;
            bool both = true;
            for (auto const& l : x.legs.l) {
                if (l.ground_contact) {
                    num2 = 1.f;
                    p.forward_power += l.forward_power / 2.f;
                } else {
                    both = false;
                }
            }
            if (!both && custom::manhattan_distance(from, dest) < 6)
                p.weight_down_to_stand_on_both_legs = mathf::min(1.f, p.weight_down_to_stand_on_both_legs + 0.033333335f);
            else if (std::fabs(md.x) > 0.3f)
                p.weight_down_to_stand_on_both_legs = mathf::max(0.f, p.weight_down_to_stand_on_both_legs - 0.1f);
            // (vanilla: any open tile in the main chunk's column down to 3 below)
            bool open = false;
            for (int32_t y = at.y; y >= at.y - 3 && !open; --y)
                open = !has_any_solid(x.room, ivec2{at.x, y});
            if (!open)
                num2 = 0.f;
            p.forward_power = mathf::pow(p.forward_power, 0.4f);
            p.forward_power = custom::lerp_map(float(stuck), 100.f, 200.f, p.forward_power, 1.5f);
            float const g = x.gravity();
            float const rm = x.run_mode();
            for (uint32_t k = 0; k < chunk_count; ++k) {
                x.vel(k) *= mathf::lerp(1.f, 0.85f, mathf::pow(num2, 0.5f));
                x.vel(k).y += g * mathf::pow(num2, 0.5f) * mathf::inverse_lerp(1.f, 0.5f, p.weight_down_to_stand_on_both_legs * (1.f - rm));
                x.vel(k) += md * mathf::max(p.forward_power, num3 * 0.5f) * 2.6f;
            }
            x.vel(head) += md * p.forward_power * 1.5f;
            float const up = rw::dot(custom::dir_vec(x.pos(0), x.pos(1)), vec2{0.f, 1.f});
            weighted_push(x, 1, 2, vec2{md.x, 0.f}, custom::lerp_map(up, 0.f, 1.f, 0.f, 1.f));
            weighted_push(x, 1, 2, vec2{0.f, 1.f}, custom::lerp_map(up, -1.f, 1.f, 8.f, 0.f) * num2);
            // (enterRoomHalf's push out over the room's edge isn't ported: it
            // leaves rooms through their pipes like everything else here)
        }

        void set_up(ctx& x, vec2 at) {
            x.p.move_dir = vec2{0.f, 1.f};
            neck_reset(x, at);
            neck_new_room(x);
            for (uint32_t i = 0; i < 2; ++i)
                leg_reset(x, i);
            x.w().miros_data_get_past(x.d).count = 0;
            x.w().miros_data_set_idle_counter(x.d, 0);
            store_neck(x);
        }
    }

    // MirosBird.Update
    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        // Creature.Update + PhysicalObject.Update
        creature::stun_countdown(state, c);
        physics::update_object(state, room, body(state, c), nullptr, nullptr);
        // (the light and heavy terrain impact sounds aren't in the sound table)
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        float const health = w.creature_get_health(c);
        if (!w.creature_get_dead(c) && health < 0.f && rng.value() < -health && rng.value() < 0.025f)
            creature::die(state, c);
        ctx x{state, c, room, 0};
        load_neck(x);
        check_flip(x);
        update_neck(x);
        if (x.awake())
            act(x);
        ivec2 const from = ground(x);
        for (uint32_t i = 0; i < 2; ++i)
            leg_update(x, i, from);
        if (x.grasped())
            carry(x);
        store_neck(x);
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 pos) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::miros);
        auto rng = rw::random(state.seed, 0, uint32_t(c.value));
        auto const view = world::get_room_view(state, room);
        physics::object_desc desc;
        desc.air_friction = 0.999f;
        desc.gravity = 0.9f;
        desc.bounce = 0.1f;
        desc.surface_friction = 0.4f;
        desc.water_friction = 0.96f;
        desc.buoyancy = 0.95f;
        auto const o = physics::create_object(state, room, desc);
        // on its legs over the floor it was put on
        if (!view.get_tile(pos + vec2{0.f, 60.f}).solid() && !view.get_tile(pos + vec2{0.f, 30.f}).solid())
            pos.y += 60.f;
        // the chunks: main 18, neck base 8, hips 7 (masses rad / 40 * 8), head 9 (0.6)
        constexpr float rads[4] = {18.f, 8.f, 7.f, 7.f};
        vec2 const offs[4] = {vec2{0.f, 0.f}, vec2{0.f, 25.f}, vec2{-17.f, 0.f}, vec2{17.f, 0.f}};
        dcon::body_chunk_id ks[5];
        for (uint32_t i = 0; i < 4; ++i)
            ks[i] = physics::add_chunk(state, o, pos + offs[i], rads[i], rads[i] / 40.f * 8.f);
        ks[4] = physics::add_chunk(state, o, pos + vec2{0.f, 40.f}, 9.f, 0.6f);
        w.body_chunk_set_go_through_floors(ks[4], 1);
        for (uint32_t i = 0; i < 4; ++i)
            for (uint32_t j = i + 1; j < 4; ++j)
                physics::add_connection(state, o, ks[i], ks[j], rw::distance(offs[i], offs[j]), physics::connection_type::normal, 1.f, -1.f);
        w.try_create_creature_body(c, o);
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_set_behavior(c, uint8_t(behavior::idle));
        w.creature_set_current_utility(c, 0.f);
        w.creature_set_destination(c, ground_tile(view, room_view::get_tile_position(pos)));
        auto& p = w.creature_get_payload(c).miros;
        uint16_t const data = p.data;
        p = creature::miros_payload{};
        p.data = data;
        p.move_dir = vec2{0.f, 1.f};
        auto const d = creature::miros_data(w, c);
        w.miros_data_set_seed(d, int32_t(rng.next()));
        w.miros_data_set_legs(d, leg_pair{});
        w.miros_data_set_neck(d, neck_state{});
        w.miros_data_set_past(d, past_positions{});
        w.miros_data_set_focus(d, dcon::creature_id{});
        w.miros_data_set_idle_pos(d, ivec2{-1, -1});
        w.miros_data_set_idle_counter(d, 0);
        auto& n = w.miros_data_get_neck(d);
        for (uint32_t i = 0; i < neck_chunks; ++i) {
            n.c[i].t_pos = float(i + 1) / float(neck_chunks);
            n.c[i].rad = i == neck_chunks - 1 ? 5.f : 3.f;
        }
        tracker::clear(state, c);
        {
            ctx x{state, c, view, 1};
            set_up(x, pos);
        }
        miros_graphics::init(state, c);
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        creature::release_all(state, c, creature::room_of(state, c));
        creature::release_target(state, c);
        creature::erase(state, c);
    }

    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) {
        // what the beak holds is kept by the carry
        return !creature::holds(state.world, c, t) && !creature::holds(state.world, t, c);
    }

    // (Creature.Collide: nothing of its own)
    void collide(sys::game_state&, dcon::creature_id, room_view const&, uint32_t, dcon::creature_id, uint32_t) {}

    // Creature.Violence (MirosBird has no Violence of its own) with its Stun
    void violence(sys::game_state& state, dcon::creature_id c, uint32_t k, vec2 momentum, float damage, float stun_bonus, creature::damage_type) {
        auto& w = state.world;
        auto rng = rw::random(state.seed, state.tick, rw::ids(uint32_t(c.value), k));
        auto const hit = chunk(state, c, std::min(k, chunk_count - 1));
        w.body_chunk_get_vel(hit) += rw::clamp_magnitude(momentum / w.body_chunk_get_mass(hit), 10.f);
        float const dmg = damage / base_damage_resistance;
        float st = (damage * 30.f + stun_bonus) / base_stun_resistance;
        float& health = w.creature_get_health(c);
        st *= 1.5f + mathf::inverse_lerp(0.5f, 0.f, health) * rng.value();
        stun(state, c, int32_t(st));
        health -= dmg;
        // quickDeath
        if (rng.value() < -health || health < -1.f || (health < 0.f && rng.value() < 0.33f))
            creature::die(state, c);
    }

    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t) {
        if (creature::holds(state.world, c, t))
            creature::release_grasp(state, c, uint32_t(creature::grasp_on(state.world, c, t)));
    }

    // (Creature.Die: the neck goes limp on the next update)
    void died(sys::game_state&, dcon::creature_id) {}

    // MirosBird.SpitOutOfShortCut + NewRoom
    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const view = world::get_room_view(state, room);
        ivec2 const t = room_view::get_tile_position(at);
        vec2 const a = rw::to_vec2(view.shortcut_entrance_hole_direction(t));
        for (uint32_t i = 0; i < chunk_count; ++i) {
            auto const k = chunk(state, c, i);
            w.body_chunk_set_pos(k, at - a * (-1.5f + float(i)) * 15.f);
            w.body_chunk_set_last_pos(k, at);
            w.body_chunk_set_last_last_pos(k, at);
            w.body_chunk_set_vel(k, a * 8.f);
        }
        ctx x{state, c, view, 2};
        set_up(x, at);
        w.miros_data_set_focus(x.d, dcon::creature_id{});
    }

    std::string describe(sys::game_state const& state, dcon::creature_id c) {
        auto const& w = state.world;
        vec2 const p = w.body_chunk_get_pos(chunk(state, c, 0));
        auto const held = creature::grasped(w, c, 0);
        return "miros bird at " + std::to_string(int(p.x)) + "," + std::to_string(int(p.y)) + " behavior " + std::to_string(int(w.creature_get_behavior(c)))
            + (held ? std::string{" holding a "} + creature::to_string(w.creature_get_type(held)) : std::string{});
    }

    bool spawn_spot(room_view const& room, ivec2 tile) {
        // a floor tile with room above and to the sides for its tall body
        if (!open_floor(room, tile))
            return false;
        for (int32_t dx = -1; dx <= 1; ++dx)
            for (int32_t dy = 1; dy <= 5; ++dy)
                if (has_any_solid(room, tile + ivec2{dx, dy}))
                    return false;
        return true;
    }
}
