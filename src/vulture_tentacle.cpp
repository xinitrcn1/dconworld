// Tentacle.cs (the neck and the wings: a list of room tiles from the body
// out to where the tentacle reaches, `segments`, kept four way connected,
// with TentacleChunks following them) and VultureTentacle.cs (the wings'
// climbing and flying), for vulture.cpp. Ported from the same code as the
// miros bird's neck and the rain deer's legs, generalised over the props.

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

#include "vulture_internal.hpp"
#include "daddy_internal.hpp"
#include "body_parts.hpp"

namespace vulture::detail {
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;
    using daddy::detail::ray_trace_tiles_for_terrain;
    using daddy::detail::ray_trace_tiles_list;

    namespace {
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
        // Custom.RectCollision: where the segment from `last` to `pos` meets
        // the rectangle's border (`pos` when it doesn't)
        vec2 rect_collision(vec2 pos, vec2 last, float left, float bottom, float right, float top, vec2* upper_right = nullptr) {
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
            vec2 ur{0.f, 0.f};
            auto const take = [&](vec2 v, vec2 side) {
                out = v;
                ur = side;
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
            if (upper_right)
                *upper_right = ur;
            return out;
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
    }

    //
    // SharedPhysics
    //
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

    std::optional<ivec2> first_solid(room_view const& room, vec2 a, vec2 b) {
        ivec2 hit{0, 0};
        if (walk_tiles(a, b, [&](ivec2 t) { return room.get_tile(t).solid() ? (hit = t, true) : false; }))
            return hit;
        return std::nullopt;
    }

    vec2 beam_end(room_view const& room, vec2 from, vec2 dir, bool* hit_terrain) {
        // Custom.RectCollision(pos, pos + dir * 100000, RoomRect.Grow(200)).GetCorner(D)
        vec2 corner = rect_collision(from + dir * 100000.f, from, -200.f, -200.f, room.pixel_width() + 200.f, room.pixel_height() + 200.f);
        if (hit_terrain)
            *hit_terrain = false;
        if (auto const t = first_solid(room, from, corner)) {
            vec2 const m = room_view::middle_of_tile(*t);
            corner = rect_collision(from, corner, m.x - 10.f, m.y - 10.f, m.x + 10.f, m.y + 10.f);
            if (hit_terrain)
                *hit_terrain = true;
        }
        return corner;
    }

    bool exact_terrain_ray_trace(room_view const& room, vec2 a, vec2 b, vec2& at, vec2& into) {
        ivec2 hit{0, 0};
        if (!walk_tiles(a, b, [&](ivec2 t) { return room.get_tile(t).solid() ? (hit = t, true) : false; }))
            return false;
        vec2 const m = room_view::middle_of_tile(hit);
        into = vec2{0.f, 0.f};
        at = rect_collision(b, a, m.x - 10.f, m.y - 10.f, m.x + 10.f, m.y + 10.f, &into);
        return true;
    }

    //
    // Tentacle
    //
    namespace {
        std::optional<ivec2> grab_dest(tent const& tn) {
            if (tn.t->float_grab_dest)
                return room_view::get_tile_position(*tn.t->float_grab_dest);
            return std::nullopt;
        }
        int32_t current_segment(tent const& tn, tentacle_chunk const& c) {
            int32_t const k = int32_t(tn.seg.size());
            return custom::int_clamp(int32_t(c.t_pos * float(k - 1)), 0, k - 1);
        }
        ivec2 stuck_pos(tent const& tn, tentacle_chunk const& c) { return tn.seg[size_t(current_segment(tn, c))]; }
        bool visual(ctx& x, ivec2 a, ivec2 b) { return ray_trace_tiles_for_terrain(x.room, a, b); }

        // TentacleChunk.Reset
        void chunk_reset(tent& tn, tentacle_chunk& c) {
            c.phase = -1.f;
            c.pos = room_view::middle_of_tile(stuck_pos(tn, c));
            c.last_pos = c.pos;
            c.vel = vec2{0.f, 0.f};
        }

        // TentacleChunk.Update (no rope: RopeActive is false)
        void chunk_update(ctx& x, tent& tn, uint32_t i) {
            auto& t = *tn.t;
            auto const& tp = *tn.tp;
            auto& c = tn.c[i];
            float const n = float(tn.c.size());
            c.last_pos = c.pos;
            float const ideal = t.ideal_length / n * mathf::lerp(1.f, 0.1f, t.retract_fac);
            if (c.phase == -1.f) {
                if (c.phases_to_same_location > 0 && stuck_pos(tn, c) == c.after_phase_stuck_pos) {
                    vec2 const m = room_view::middle_of_tile(stuck_pos(tn, c));
                    c.pos = vec2{mathf::clamp(c.pos.x, m.x - 9.f, m.x + 9.f), mathf::clamp(c.pos.y, m.y - 9.f, m.y + 9.f)};
                }
                if (i == 0) {
                    vec2 const fb = float_base(x, tn);
                    vec2 const a = custom::dir_vec(c.pos, fb);
                    float const num = rw::distance(c.pos, fb);
                    if (num > ideal) {
                        c.pos -= a * (ideal - num) * (1.f - tp.pull_at_connection_chunk);
                        c.vel -= a * (ideal - num) * (1.f - tp.pull_at_connection_chunk);
                        if (tp.pull_at_connection_chunk > 0.f) {
                            x.pos(t.connected_chunk) += a * (ideal - num) * tp.pull_at_connection_chunk;
                            x.vel(t.connected_chunk) += a * (ideal - num) * tp.pull_at_connection_chunk;
                        }
                    }
                    c.stretched_fac = t.ideal_length / n / mathf::max(1.f, num);
                } else {
                    auto& prev = tn.c[i - 1];
                    vec2 const a = custom::dir_vec(c.pos, prev.pos);
                    float const num = rw::distance(c.pos, prev.pos);
                    if (num > ideal) {
                        c.pos -= a * (ideal - num) * (1.f - tp.mass_deterioration_per_chunk);
                        c.vel -= a * (ideal - num) * (1.f - tp.mass_deterioration_per_chunk);
                        prev.pos += a * (ideal - num) * tp.mass_deterioration_per_chunk;
                        prev.vel += a * (ideal - num) * tp.mass_deterioration_per_chunk;
                    }
                    c.stretched_fac = t.ideal_length / n / mathf::max(1.f, num);
                }
                ivec2 const here = room_view::get_tile_position(c.pos);
                int32_t const cs = current_segment(tn, c);
                if (t.backtrack_from == -1 && !visual(x, here, tn.seg[size_t(cs)]))
                    t.backtrack_from = int32_t(i);
                if (t.backtrack_from != -1 && int32_t(i) >= t.backtrack_from) {
                    bool flag = false;
                    for (int32_t k = 1; k <= cs && !flag; ++k) {
                        if (!visual(x, here, tn.seg[size_t(k)]) && visual(x, here, tn.seg[size_t(k - 1)])) {
                            c.vel += rw::clamp_magnitude(room_view::middle_of_tile(tn.seg[size_t(k - 1)]) - c.pos, 20.f) * tp.backtrack_speed / 20.f;
                            flag = true;
                        }
                    }
                    if (!flag) {
                        vec2 const fb = i > 0 ? tn.c[i - 1].pos : float_base(x, tn);
                        c.vel += rw::clamp_magnitude(fb - c.pos, 20.f) * tp.backtrack_speed / 20.f;
                        if (i > 0)
                            tn.c[i - 1].vel += rw::clamp_magnitude(fb - c.pos, 20.f) * tp.backtrack_speed / 20.f;
                    }
                    if (!flag && (c.contact_point.x != 0 || c.contact_point.y != 0)) {
                        ++c.phase_attempts;
                        if (c.phase_attempts > tp.terrain_hits_before_phase) {
                            // PhaseToSegment
                            c.phase_from = c.pos;
                            c.phase = 0.f;
                            c.phase_attempts = 0;
                        }
                    }
                } else {
                    c.phase_attempts = 0;
                    if (t.limp) {
                        for (int32_t k = cs; k >= 0; --k) {
                            ivec2 const s = tn.seg[size_t(k)];
                            if (visual(x, here, s)) {
                                c.vel += rw::clamp_magnitude(room_view::middle_of_tile(s) - c.pos, 20.f) * 0.005f;
                                break;
                            }
                        }
                    } else {
                        if (t.float_grab_dest)
                            c.vel += rw::clamp_magnitude(*t.float_grab_dest - c.pos, 20.f) * (i == tn.c.size() - 1 ? tp.goal_attraction_speed_tip : tp.goal_attraction_speed) / 20.f;
                        c.vel += rw::clamp_magnitude(room_view::middle_of_tile(tn.seg[size_t(cs)]) - c.pos, 20.f) * tp.align_to_segment_speed / 20.f;
                    }
                }
                c.vel = rw::clamp_magnitude(c.vel, tp.chunk_velocity_cap);
                c.pos += c.vel;
                if (c.collide_with_terrain) {
                    c.last_contact_point = c.contact_point;
                    terrain_collision cd{c.pos, c.pos - c.vel, c.vel, c.rad};
                    vertical_collision(x.room, cd);
                    slopes_vertically(x.room, cd);
                    horizontal_collision(x.room, cd);
                    c.contact_point = cd.contact;
                    c.pos = cd.pos;
                    c.vel = cd.vel;
                }
            } else {
                c.phase += 1.f / float(tp.segment_phase_through_terrain_frames);
                if (c.phase <= 1.f)
                    c.pos = rw::lerp(c.phase_from, room_view::middle_of_tile(stuck_pos(tn, c)), mathf::min(1.f, c.phase));
                c.vel = vec2{0.f, 0.f};
                if (c.phase >= 1.f) {
                    c.phase = -1.f;
                    if (c.after_phase_stuck_pos == stuck_pos(tn, c))
                        ++c.phases_to_same_location;
                    else
                        c.phases_to_same_location = 0;
                    c.after_phase_stuck_pos = stuck_pos(tn, c);
                }
                if (i > 0 && tn.c[i - 1].phase == -1.f) {
                    auto& prev = tn.c[i - 1];
                    vec2 const a = custom::dir_vec(c.pos, prev.pos);
                    float const num = rw::distance(c.pos, prev.pos);
                    if (num > ideal) {
                        prev.pos += a * (ideal - num) * tp.mass_deterioration_per_chunk;
                        prev.vel += a * (ideal - num) * tp.mass_deterioration_per_chunk;
                    }
                }
            }
        }

        // Tentacle.MoveSegment
        bool move_segment(ctx& x, tent& tn, int32_t s, ivec2 dest, bool allow_occupied, bool allow_solid) {
            auto& seg = tn.seg;
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
        void shorten(tent& tn, int32_t from, int32_t to, std::vector<ivec2> const& straight, int32_t count) {
            tn.seg.erase(tn.seg.begin() + from + 1, tn.seg.begin() + to);
            for (int32_t i = count - 2; i > 0; --i)
                tn.seg.insert(tn.seg.begin() + from + 1, straight[size_t(i)]);
        }

        // Tentacle.PullAtTentacle
        void pull_at_tentacle(ctx& x, tent& tn, int32_t start, bool only_simple_pull, std::vector<ivec2>& path) {
            auto& t = *tn.t;
            auto& seg = tn.seg;
            if (!only_simple_pull) {
                while (t.pull_counter > 0) {
                    bool flag = false;
                    for (int32_t num = start; num < int32_t(seg.size()) - 3 && !flag; ++num) {
                        for (int32_t i = int32_t(seg.size()) - 1; i >= num + 3; --i) {
                            int32_t const num2 = i - num - custom::manhattan_distance(seg[size_t(num)], seg[size_t(i)]);
                            if (num2 > 0 && num2 <= t.pull_counter && visual(x, seg[size_t(num)], seg[size_t(i)])) {
                                int32_t const num3 = ray_trace_tiles_list(seg[size_t(num)].x, seg[size_t(num)].y, seg[size_t(i)].x, seg[size_t(i)].y, path);
                                bool flag2 = true;
                                if (num2 > 1 && num2 <= t.pulls_this_tick)
                                    for (int32_t j = 1; j < num3 - 1 && flag2; ++j)
                                        for (int32_t k = num + 1; k < i; ++k)
                                            if (custom::manhattan_distance(seg[size_t(k)], path[size_t(j)]) > 2 && !visual(x, seg[size_t(k)], path[size_t(j)])) {
                                                flag2 = false;
                                                break;
                                            }
                                if (flag2) {
                                    t.pulls_this_tick -= num2;
                                    t.pull_counter -= num2;
                                    shorten(tn, num, i, path, num3);
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
            while (t.pull_counter > 0 && t.pulls_this_tick > 0 && seg.size() > 1) {
                seg.pop_back();
                --t.pull_counter;
                --t.pulls_this_tick;
            }
        }

        // Tentacle.FindTentaclePath: a greedy best first search from the goal
        // back to the start over every tile (solid ones at 1000 extra)
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
        void move_aligned_segments_with_path(ctx& x, tent& tn, std::vector<ivec2>& path) {
            auto& seg = tn.seg;
            auto& gp = tn.gp;
            int32_t const lim = std::min(int32_t(seg.size()) - 1, int32_t(gp.size()) - 1);
            int32_t num = lim;
            int32_t k = 0;
            while (k < lim && float_dist(seg[size_t(k + 1)], gp[size_t(k + 1)]) <= tn.tp->tile_tentacle_snap_with_path_distance
                && visual(x, seg[size_t(k + 1)], gp[size_t(k + 1)])) {
                num = k + 1;
                seg[size_t(k)] = gp[size_t(k)];
                ++k;
            }
            if (num < 0)
                return;
            int32_t const num3 = ray_trace_tiles_list(gp[size_t(num)].x, gp[size_t(num)].y, seg[size_t(num)].x, seg[size_t(num)].y, path);
            for (int32_t i = num3 - 2; i >= 0; --i)
                seg.insert(seg.begin() + num, path[size_t(i)]);
            tn.t->pull_counter += num3 - 1;
            pull_at_tentacle(x, tn, num, true, path);
        }

        // Tentacle.FindGrabPath
        void find_grab_path(ctx& x, tent& tn, std::vector<ivec2>& path) {
            ivec2 const g = *grab_dest(tn);
            ivec2 const b = base_pos(x, tn);
            int32_t num;
            if (!inside(x.room, g) || !inside(x.room, b) || visual(x, b, g))
                num = ray_trace_tiles_list(b.x, b.y, g.x, g.y, tn.gp);
            else
                num = find_tentacle_path(x, b, g, tn.gp);
            if (int32_t(tn.gp.size()) > num)
                tn.gp.resize(size_t(num));
            move_aligned_segments_with_path(x, tn, path);
        }

        // Tentacle.Grow
        void grow(ctx& x, tent& tn, int32_t add) {
            auto& seg = tn.seg;
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
        void move_base(ctx& x, tent& tn, ivec2 to, std::vector<ivec2>& path) {
            auto& seg = tn.seg;
            if (has_any_solid(x.room, to) || to == base_pos(x, tn))
                return;
            if (are_neighbors(seg[0], to)) {
                int32_t num = -1;
                for (int32_t i = 1; i < int32_t(seg.size()) - 1; ++i)
                    if (seg[size_t(i)] == to)
                        num = i;
                if (num > 0) {
                    seg.erase(seg.begin(), seg.begin() + num);
                    grow(x, tn, num);
                } else {
                    seg.insert(seg.begin(), to);
                    ++tn.t->pull_counter;
                    if (tn.t->pull_counter > 4)
                        pull_at_tentacle(x, tn, 0, false, path);
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
                tn.t->pull_counter += num2 - 1;
                if (tn.t->pull_counter > 4)
                    pull_at_tentacle(x, tn, 0, false, path);
            }
            if (grab_dest(tn))
                find_grab_path(x, tn, path);
        }

        // Tentacle.AlignWithGrabPath
        void align_with_grab_path(ctx& x, tent& tn, std::vector<ivec2>& path) {
            auto& seg = tn.seg;
            auto& gp = tn.gp;
            bool flag = false;
            for (int32_t i = std::min(int32_t(seg.size()) - 2, int32_t(gp.size()) - 1); i > 0; --i) {
                if (i >= int32_t(seg.size()) || i >= int32_t(gp.size()))
                    continue;
                if (seg[size_t(i)] != gp[size_t(i)] && visual(x, seg[size_t(i)], gp[size_t(i)])) {
                    int32_t const k = ray_trace_tiles_list(seg[size_t(i)].x, seg[size_t(i)].y, gp[size_t(i)].x, gp[size_t(i)].y, path);
                    if (k > 1 && move_segment(x, tn, i + 1, path[1], false, false))
                        flag = true;
                }
            }
            if (!flag)
                for (int32_t k = 1; k < int32_t(seg.size()) && k < int32_t(gp.size()); ++k)
                    if (seg[size_t(k)] != gp[size_t(k)]) {
                        move_segment(x, tn, k, gp[size_t(k)], true, false);
                        return;
                    }
        }

        // Tentacle.CurrentLength
        float current_length(tent const& tn) {
            float num = float(tn.seg.size()) * 20.f;
            for (size_t i = 0; i + 2 < tn.seg.size(); ++i)
                if (are_diagonal_neighbors(tn.seg[i], tn.seg[i + 2]))
                    num -= 5.9000006f;
            return num;
        }

        // Tentacle.AdjustLength (tProps.shorten: no longer than the grab path)
        void adjust_length(ctx& x, tent& tn, std::vector<ivec2>& path) {
            float const num = current_length(tn);
            float num2 = tn.t->ideal_length;
            if (!tn.gp.empty())
                num2 = mathf::min(tn.t->ideal_length, float(tn.gp.size()) * 20.f);
            int32_t const num3 = int32_t(num2 / 20.f - num / 20.f);
            if (num3 > 0 && (tn.gp.empty() || tn.seg.size() < tn.gp.size())) {
                grow(x, tn, 1);
                return;
            }
            if (num3 < 0) {
                ++tn.t->pull_counter;
                pull_at_tentacle(x, tn, 0, false, path);
            }
        }

        // Tentacle.Gravity with GravityDirection (Tentacle's: straight down
        // or a diagonal; VultureTentacle's: down or towards its side)
        void gravity(ctx& x, tent& tn, std::optional<float> wing_dir) {
            ivec2 b{0, -1};
            if (wing_dir) {
                if (x.rng.value() < 0.5f)
                    b = ivec2{int32_t(*wing_dir), -1};
            } else if (x.rng.value() < 0.5f) {
                b = ivec2{x.rng.value() < 0.5f ? -1 : 1, -1};
            }
            bool flag = false;
            for (int32_t i = 0; i < int32_t(tn.seg.size()) && !flag; ++i)
                if (move_segment(x, tn, i, tn.seg[size_t(i)] + b, false, false))
                    flag = true;
            if (!flag)
                for (int32_t i = 0; i < int32_t(tn.seg.size()); ++i)
                    if (move_segment(x, tn, i, tn.seg[size_t(i)] + b, true, false))
                        break;
        }

        void tentacle_update_impl(ctx& x, tent& tn, std::optional<float> wing_dir) {
            auto& path = x.scratch;
            auto& t = *tn.t;
            base_pos(x, tn);
            move_base(x, tn, room_view::get_tile_position(float_base(x, tn)), path);
            t.update_counter += tn.tp->tile_tentacle_update_speed;
            if (t.update_counter >= 1.f) {
                t.pulls_this_tick = tn.tp->max_pull_tiles_per_tick;
                if (t.limp)
                    gravity(x, tn, wing_dir);
                else if (!tn.gp.empty())
                    align_with_grab_path(x, tn, path);
                adjust_length(x, tn, path);
                t.update_counter -= 1.f;
            }
            base_pos(x, tn);
            t.backtrack_from = -1;
            for (uint32_t i = 0; i < tn.c.size(); ++i)
                chunk_update(x, tn, i);
        }

        // Tentacle.PushChunksApart
        void push_chunks_apart(tent& tn, uint32_t a, uint32_t b) {
            vec2 const a2 = custom::dir_vec(tn.c[a].pos, tn.c[b].pos);
            float const num = rw::distance(tn.c[a].pos, tn.c[b].pos);
            constexpr float num2 = 10.f;
            if (num < num2) {
                tn.c[a].pos -= a2 * (num2 - num) * 0.5f;
                tn.c[a].vel -= a2 * (num2 - num) * 0.5f;
                tn.c[b].pos += a2 * (num2 - num) * 0.5f;
                tn.c[b].vel += a2 * (num2 - num) * 0.5f;
            }
        }
    }

    vec2 float_base(ctx& x, tent const& tn) { return x.pos(tn.t->connected_chunk); }

    ivec2 base_pos(ctx& x, tent& tn) {
        if (tn.seg.empty())
            tn.seg.push_back(room_view::get_tile_position(float_base(x, tn)));
        return tn.seg[0];
    }

    void tentacle_update(ctx& x, tent& tn) { tentacle_update_impl(x, tn, std::nullopt); }

    void tentacle_new_room(ctx& x, tent& tn) {
        tn.seg.assign(size_t(std::max(1, int32_t(tn.t->ideal_length / 20.f))), room_view::get_tile_position(x.pos(0)));
        tn.gp.clear();
        for (auto& c : tn.c)
            chunk_reset(tn, c);
    }

    void tentacle_reset(ctx&, tent& tn, vec2 at) {
        tn.seg.assign(1, room_view::get_tile_position(at));
        for (auto& c : tn.c) {
            c.pos = at;
            c.vel = vec2{0.f, 0.f};
            chunk_reset(tn, c);
        }
    }

    void move_grab_dest(ctx& x, tent& tn, vec2 to) {
        if (auto const g = grab_dest(tn); g && *g == room_view::get_tile_position(to))
            return;
        tn.t->float_grab_dest = to;
        find_grab_path(x, tn, x.scratch);
    }

    //
    // VultureTentacle
    //
    float tentacle_dir(ctx const& x, uint32_t number) {
        if (number == 0)
            return -1.f;
        if (!x.miros())
            return 1.f;
        if (number == 1)
            return 1.f;
        if (number == 2)
            return -4.f;
        return 4.f;
    }

    bool has_any_grip(wing_work const& ww) { return ww.w.attached_at_tip || ww.w.segments_gripping_terrain > 0; }

    float tentacle_contour(variant v, float flying_mode, float x) {
        float const num = mathf::lerp(0.45f, 0.1f, flying_mode);
        float const num2 = mathf::lerp(0.51f, 0.25f, flying_mode);
        float const num3 = mathf::lerp(0.85f, 0.4f, flying_mode);
        float const num4 = mathf::lerp(6.5f, 5.5f, flying_mode);
        float const num5 = mathf::lerp(0.5f, 0.35f, flying_mode);
        float const num6 = mathf::lerp(0.85f, 0.f, flying_mode);
        float const num7 = num6 + (1.f - num6) * std::cos(mathf::inverse_lerp(num2, 1.2f, x) * mathf::pi * 0.5f);
        float const num8 = v == variant::king ? 1.2f : 1.f;
        if (x < num)
            return num4 * num5 * num8;
        if (x < num2)
            return num4 * mathf::lerp(num5, 1.f, custom::s_curve(mathf::inverse_lerp(num, num2, x), 0.1f)) * num8;
        if (x < num3)
            return num4 * num7 * num8;
        return num4 * mathf::lerp(0.5f, 1.f, std::cos(mathf::pow(mathf::inverse_lerp(num3, 1.f, x), 4.f) * mathf::pi * 0.5f)) * num7 * num8;
    }

    float feather_contour(float x, float k) {
        float num = mathf::lerp(0.2f, 1.f, custom::s_curve(mathf::pow(x, 1.5f), 0.1f));
        if (mathf::pow(x, 1.5f) > 0.5f)
            num *= std::sqrt(1.f - mathf::pow(mathf::inverse_lerp(0.5f, 1.f, mathf::pow(x, 1.5f)), 4.5f));
        float num2 = 1.f;
        num2 *= mathf::pow(std::sin(mathf::pow(x, 0.5f) * mathf::pi), 0.7f);
        if (x < 0.3f)
            num2 *= mathf::lerp(0.7f, 1.f, custom::s_curve(mathf::inverse_lerp(0.f, 0.3f, x), 0.5f));
        return mathf::lerp(num * 0.5f, num2, k);
    }

    float feather_width(float x) { return mathf::pow(std::sin(mathf::inverse_lerp(-0.45f, 1.f, x) * mathf::pi), 2.6f); }

    wing_work& ctx::other_wing(wing_work const& ww) {
        uint32_t const n = ww.number;
        uint32_t o;
        if (!miros())
            o = 1 - std::min(n, 1u);
        else
            o = n % 2 == 0 ? n + 1 : n - 1;
        o = std::min<uint32_t>(o, wing_count() - 1);
        return wings[o];
    }

    void switch_mode(ctx& x, wing_work& ww, wing_mode m) {
        ww.w.mode = m;
        if (m == wing_mode::fly) {
            if (x.miros())
                release_grip(x, ww);
            ww.w.t.float_grab_dest = std::nullopt;
        }
    }

    void release_grip(ctx& x, wing_work& ww) {
        if (x.other_wing(ww).w.grab_delay < 1)
            ww.w.grab_delay = 10;
        ww.w.t.float_grab_dest = std::nullopt;
    }

    float support(ctx& x, wing_work const& ww) {
        if (x.state.world.creature_part_get_stun(ww.part) > 0)
            return 0.f;
        if (ww.w.mode == wing_mode::climb)
            return mathf::clamp((has_any_grip(ww) ? (x.miros() ? 4.f : 0.5f) : 0.f) + float(ww.w.segments_gripping_terrain) / float(ww.w.chunk_count), 0.f, 1.f);
        return x.miros() ? 1.2f : 0.5f;
    }

    float release_score(ctx&, wing_work const& ww) {
        if (ww.w.mode != wing_mode::climb)
            return std::numeric_limits<float>::lowest();
        float num = rw::distance(ww.w.c[ww.w.chunk_count - 1].pos, ww.w.desired_grab_pos);
        if (!ww.w.t.float_grab_dest)
            num *= 2.f;
        return num;
    }

    bool wing_space(ctx& x, wing_work& ww) {
        vec2 const base = x.pos(ww.w.t.connected_chunk);
        float const dir = tentacle_dir(x, ww.number);
        for (int32_t i = -1; i <= 1; ++i)
            if (!ray_trace_tiles_for_terrain(x.room, room_view::get_tile_position(base),
                    room_view::get_tile_position(base + vec2{dir * ww.w.t.ideal_length, 50.f * float(i)})))
                return false;
        return true;
    }

    void wing_damage(ctx& x, wing_work& ww, float damage, float stun_bonus) {
        damage /= 2.7f;
        stun_bonus /= 1.2f;
        int32_t& st = x.wing_stun_ref(ww);
        st = std::max(st, int32_t(damage * 30.f + stun_bonus));
        x.wing_health(ww) -= damage;
    }

    namespace {
        // VultureTentacle.UpdateDesiredGrabPos
        void update_desired_grab_pos(ctx& x, wing_work& ww) {
            float const dir = tentacle_dir(x, ww.number);
            vec2 const side = rw::normalized(vec2{dir, -0.8f});
            if (x.p.hover_still)
                ww.w.desired_grab_pos = x.pos(0) + side * ww.w.t.ideal_length * 0.7f;
            else
                ww.w.desired_grab_pos = x.pos(0) + rw::slerp(x.p.move_direction, side, 0.3f) * ww.w.t.ideal_length * 0.7f;
        }

        // VultureTentacle.GripPointAttractiveness
        float grip_point_attractiveness(ctx& x, wing_work const& ww, ivec2 pos) {
            float const d = float_dist(room_view::get_tile_position(ww.w.desired_grab_pos), pos);
            if (x.room.get_tile(pos).solid())
                return 100.f / d;
            return 65.f / d;
        }

        // VultureTentacle.ClosestSolid
        std::optional<ivec2> closest_solid(ctx& x, tent& tn, ivec2 goal, int32_t max_distance, float max_dist_from_base) {
            if (x.room.get_tile(goal).solid())
                return goal;
            ivec2 const b = base_pos(x, tn);
            for (int32_t i = 1; i <= max_distance; ++i)
                for (auto const d : custom::eight_directions) {
                    ivec2 const t = goal + d * i;
                    if (x.room.get_tile(t).solid() && float_dist(b, t) < max_dist_from_base)
                        return t;
                }
            return std::nullopt;
        }

        // VultureTentacle.FindGrabPos
        void find_grab_pos(ctx& x, wing_work& ww) {
            auto& w = ww.w;
            auto& tn = ww.tn;
            auto& path = x.scratch;
            if (w.grab_delay > 0) {
                --w.grab_delay;
                return;
            }
            if (auto const iv = closest_solid(x, tn, room_view::get_tile_position(w.desired_grab_pos), 8, 8.f)) {
                ivec2 const b = base_pos(x, tn);
                auto const hit = first_solid(x.room, room_view::middle_of_tile(b), room_view::middle_of_tile(*iv));
                ivec2 const iv2 = hit ? *hit : *iv;
                auto const gd = grab_dest(tn);
                if (!gd || grip_point_attractiveness(x, ww, iv2) > grip_point_attractiveness(x, ww, *gd)) {
                    vec2 const m = room_view::middle_of_tile(iv2);
                    vec2 const fb = float_base(x, tn);
                    move_grab_dest(x, tn, vec2{mathf::clamp(fb.x, m.x - 11.f, m.x + 11.f), mathf::clamp(fb.y, m.y - 11.f, m.y + 11.f)});
                }
            }
            vec2 const p = w.desired_grab_pos + custom::deg_to_vec(x.rng.value() * 360.f) * x.rng.value() * w.t.ideal_length;
            ivec2 const b = base_pos(x, tn);
            ivec2 const pt = room_view::get_tile_position(p);
            int32_t const num = ray_trace_tiles_list(b.x, b.y, pt.x, pt.y, path);
            for (int32_t i = 0; i < num && !x.room.get_tile(path[size_t(i)]).solid(); ++i) {
                auto const tl = x.room.get_tile(path[size_t(i)]);
                if (tl.any_beam()) {
                    auto const gd = grab_dest(tn);
                    if (!gd || grip_point_attractiveness(x, ww, path[size_t(i)]) > grip_point_attractiveness(x, ww, *gd)) {
                        move_grab_dest(x, tn, room_view::middle_of_tile(path[size_t(i)]));
                        return;
                    }
                }
            }
        }

        // VultureTentacle.GripTerrain
        bool grip_terrain(ctx& x, wing_work& ww, uint32_t i) {
            auto& c = ww.w.c[i];
            ivec2 const tp = room_view::get_tile_position(c.pos);
            for (auto const d : custom::four_directions) {
                if (x.room.get_tile(tp + d).solid()) {
                    c.vel *= 0.25f;
                    c.vel += rw::to_vec2(d) * 0.8f;
                    return c.contact_point.x != 0 || c.contact_point.y != 0;
                }
            }
            auto const tl = x.room.get_tile(c.pos);
            if (tl.horizontal_beam()) {
                c.vel *= 0.25f;
                c.vel.y += (room_view::middle_of_tile(c.pos).y - c.pos.y) * 0.3f;
                return true;
            }
            if (tl.vertical_beam()) {
                c.vel *= 0.25f;
                c.vel.x += (room_view::middle_of_tile(c.pos).x - c.pos.x) * 0.3f;
                return true;
            }
            return false;
        }
    }

    void wing_update(ctx& x, wing_work& ww) {
        auto& w = ww.w;
        auto& tn = ww.tn;
        float const dir = tentacle_dir(x, ww.number);
        uint32_t const n = w.chunk_count;
        // Tentacle.Update (VultureTentacle's GravityDirection)
        tentacle_update_impl(x, tn, dir);
        if (x.w().creature_get_entering_shortcut(x.c).x >= 0) {
            w.t.retract_fac = mathf::min(0.f, w.t.retract_fac - 0.1f);
            vec2 const hole = room_view::middle_of_tile(x.w().creature_get_entering_shortcut(x.c));
            for (uint32_t i = 0; i < n; ++i)
                w.c[i].vel += rw::clamp_magnitude(hole - w.c[i].pos, 50.f) / 10.f;
            if (tn.seg.size() > 1)
                tn.seg.pop_back();
            return;
        }
        w.attached_at_tip = 0;
        w.t.ideal_length = mathf::lerp(20.f * (x.king() ? 9.f : 7.f), 20.f * (x.king() ? 13.5f : 11.f), w.flying_mode);
        int32_t& stun = x.wing_stun_ref(ww);
        if (stun > 0)
            --stun;
        float const health = x.wing_health(ww);
        if (mathf::pow(x.rng.value(), 0.25f) > 2.f * health)
            stun = std::max(stun, int32_t(mathf::lerp(-2.f, 12.f, mathf::pow(x.rng.value(), 0.5f + 20.f * mathf::max(0.f, health)))));
        w.t.limp = (!x.awake() || stun > 0) ? 1 : 0;
        if (w.t.limp) {
            w.t.float_grab_dest = std::nullopt;
            for (uint32_t j = 0; j < n; ++j) {
                w.c[j].vel *= 0.9f;
                w.c[j].vel.y -= 0.5f;
            }
        }
        auto& other = x.other_wing(ww);
        for (uint32_t k = 0; k < n; ++k) {
            auto& c = w.c[k];
            c.rad = tentacle_contour(x.v, w.flying_mode, c.t_pos);
            if (w.t.backtrack_from == -1 || int32_t(k) < w.t.backtrack_from) {
                if (k > 1 && custom::dist_less(c.pos, w.c[k - 2].pos, 30.f))
                    c.vel -= custom::dir_vec(c.pos, w.c[k - 2].pos) * (30.f - rw::distance(c.pos, w.c[k - 2].pos)) * 0.1f;
                else if (k <= 1)
                    c.vel = custom::dir_vec(x.pos(other.w.t.connected_chunk), x.pos(w.t.connected_chunk)) * (k == 0 ? 2.f : 1.2f);
            }
            if (x.room.point_submerged(c.pos))
                c.vel *= 0.5f;
            // (the Vulture_Tentacle_Collide_Terrain sound has no sample here)
        }
        if (!w.t.limp) {
            if (w.mode == wing_mode::climb) {
                auto& tip = w.c[n - 1];
                if (w.t.float_grab_dest && custom::dist_less(tip.pos, *w.t.float_grab_dest, 40.f) && w.t.backtrack_from == -1) {
                    tip.pos = *w.t.float_grab_dest;
                    tip.vel = vec2{0.f, 0.f};
                    w.attached_at_tip = 1;
                }
                w.flying_mode = mathf::clamp(w.flying_mode - 0.025f, 0.f, 1.f);
                tip.collide_with_terrain = w.attached_at_tip ? 0 : 1;
                update_desired_grab_pos(x, ww);
                bool flag = ww.number == 0;
                if (x.miros())
                    flag = ww.number % 2 == 0;
                uint32_t const body_chunk = flag ? 3 : 2;
                w.segments_gripping_terrain = 0;
                float const sup = support(x, ww);
                for (uint32_t l = 0; l < n; ++l) {
                    auto& c = w.c[l];
                    c.vel *= mathf::lerp(0.95f, 0.85f, sup);
                    if (w.attached_at_tip && (w.t.backtrack_from == -1 || int32_t(l) < w.t.backtrack_from) && grip_terrain(x, ww, l)) {
                        ++w.segments_gripping_terrain;
                        for (int32_t m = int32_t(l) - 1; m > 0; --m)
                            push_chunks_apart(tn, l, uint32_t(m));
                    } else {
                        c.vel.y += 0.1f;
                        c.vel += x.vel(w.t.connected_chunk) * 0.1f;
                        if (!has_any_grip(ww)) {
                            if (w.t.float_grab_dest)
                                c.vel += custom::dir_vec(c.pos, *w.t.float_grab_dest) * 0.3f;
                            else
                                c.vel += custom::dir_vec(c.pos, w.desired_grab_pos + custom::dir_vec(float_base(x, tn), w.desired_grab_pos) * 70.f) * 0.6f;
                        }
                    }
                    c.vel += custom::dir_vec(x.pos(body_chunk), c.pos) * 0.5f / (float(l) + 1.f);
                }
                if (w.attached_at_tip) {
                    w.frames_without_reaching = 0;
                    auto const gd = grab_dest(tn);
                    if (gd && ray_trace_tiles_for_terrain(x.room, base_pos(x, tn), *gd)) {
                        vec2 const cp = x.pos(w.t.connected_chunk);
                        if (!custom::dist_less(tip.pos, cp, w.t.ideal_length)) {
                            vec2 const a = custom::dir_vec(tip.pos, cp);
                            float const num = rw::distance(tip.pos, cp);
                            float const num2 = w.t.ideal_length * 0.9f;
                            x.pos(w.t.connected_chunk) += a * (num2 - num) * 0.2f;
                            x.vel(w.t.connected_chunk) += a * (num2 - num) * 0.2f;
                        }
                        if (!custom::dist_less(tip.pos, x.pos(w.t.connected_chunk), w.t.ideal_length * 0.9f))
                            x.b.hanging_in_tentacle = 1;
                    }
                    // (Vulture_Tentacle_Grab_Terrain has no sample here)
                    w.play_grab_sound = 0;
                } else {
                    w.play_grab_sound = 1;
                    find_grab_pos(x, ww);
                    ++w.frames_without_reaching;
                    if (w.frames_without_reaching > 60 && !w.t.float_grab_dest) {
                        w.frames_without_reaching = 0;
                        switch_mode(x, ww, wing_mode::fly);
                    }
                }
                if (other.w.mode == wing_mode::fly) {
                    ++w.other_tentacle_is_flying;
                    if (!has_any_grip(ww)
                        && ((w.other_tentacle_is_flying > 30 && x.proximity(base_pos(x, tn)) >= 3) || w.other_tentacle_is_flying > 100)) {
                        switch_mode(x, ww, wing_mode::fly);
                        w.other_tentacle_is_flying = 0;
                    }
                } else {
                    w.other_tentacle_is_flying = 0;
                }
            } else {
                bool flag2 = false;
                w.flying_mode = mathf::clamp(w.flying_mode + 0.05f, 0.f, 1.f);
                bool flag3 = ww.number == 0;
                if (x.miros())
                    flag3 = ww.number % 2 == 0;
                vec2 const cp = x.pos(w.t.connected_chunk);
                vec2 const other_base = x.pos(flag3 ? 3 : 2);
                for (uint32_t k = 0; k < n; ++k) {
                    auto& c = w.c[k];
                    c.vel *= 0.95f;
                    c.vel.x += dir * 0.6f;
                    vec2 v = cp - custom::dir_vec(cp, other_base) * w.t.ideal_length * c.t_pos;
                    v = rw::lerp(v, cp + vec2{dir * w.t.ideal_length * c.t_pos, 0.f}, 0.5f);
                    vec2 const a2 = custom::perpendicular_vector(rw::normalized(cp - v)) * (flag3 ? -1.f : 1.f);
                    v += a2 * std::sin(mathf::pi * 2.f * (x.p.wing_flap - c.t_pos * 0.5f)) * mathf::lerp(200.f, 600.f, x.p.wing_flap_amplitude);
                    c.vel += rw::clamp_magnitude(v - c.pos, 30.f) / 30.f * 5.f * mathf::lerp(0.2f, 1.f, x.p.wing_flap_amplitude);
                    if (c.contact_point.x != 0 || c.contact_point.y != 0)
                        flag2 = true;
                }
                float num3 = 0.5f;
                if (x.miros())
                    num3 = 1.4f / float(x.wing_count());
                float const flap = std::sin(mathf::pi * 2.f * x.p.wing_flap);
                float const amp = mathf::lerp(0.5f, 1.f, x.p.wing_flap_amplitude);
                x.vel(1).y += mathf::pow(num3 + num3 * flap, 2.f) * 5.6f * amp;
                x.vel(1).x += (num3 + num3 * flap) * -2.6f * dir * amp;
                if (x.state.world.creature_part_get_stun(other.part) > 0 && stun < 1)
                    for (uint32_t k = 0; k < 4; ++k)
                        x.vel(k) += custom::dir_vec(w.c[n - 1].pos, x.pos(k)) * mathf::pow(num3 + num3 * flap, 2.f) * 0.4f * amp;
                if (flag2)
                    ++w.frames_of_hitting_terrain;
                else
                    --w.frames_of_hitting_terrain;
                w.frames_of_hitting_terrain = custom::int_clamp(w.frames_of_hitting_terrain, 0, 30);
                if (w.frames_of_hitting_terrain >= 30) {
                    w.frames_of_hitting_terrain = 0;
                    switch_mode(x, ww, wing_mode::climb);
                } else if (other.w.mode == wing_mode::climb) {
                    update_desired_grab_pos(x, ww);
                    find_grab_pos(x, ww);
                    if (w.t.float_grab_dest)
                        switch_mode(x, ww, wing_mode::climb);
                }
            }
        }
        // (the wings' woosh loop has no sample here)
    }
}
