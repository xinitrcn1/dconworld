// Tentacle.cs and DaddyTentacle.cs. A tentacle is a list of room tiles
// (`segments`) from the body outwards, kept connected (every tile a four
// way neighbour of the previous one), and a few TentacleChunks following
// those tiles on Ropes. Its tip is sent to a grab destination by finding a
// tile path there (`grab_path`: a straight line when it can see it, else
// FindTentaclePath's greedy search through the room's tiles) and moving the
// segments along it; pulls straighten it, growth and shortening keep it
// near its ideal length. DaddyTentacle gives it its tasks: climbing (grips
// on the terrain to carry the body), hunting and grabbing the slugcat, and
// examining noises.

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

#include "daddy_internal.hpp"
#include "rooms.hpp"
#include "daddy_graphics.hpp"
#include "pathing.hpp"
#include "player.hpp"
#include "rot.hpp"

namespace daddy::detail {
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        constexpr float float_max = std::numeric_limits<float>::max();
        constexpr ivec2 four_directions_and_zero[5] = {{0, 0}, {-1, 0}, {0, -1}, {1, 0}, {0, 1}};

        // DaddyTentacle's Tentacle.TentacleProps(false, true, false, 0.5f, 0f,
        // 0f, 0f, 0f, 3.2f, 10f, 0.25f, 5f, 15, 60, 12, 20): a rope tentacle
        // without goal attraction (the tasks move the chunks)
        constexpr float mass_deterioration_per_chunk = 0.5f;
        constexpr float backtrack_speed = 3.2f;
        constexpr float chunk_velocity_cap = 10.f;
        constexpr float tile_tentacle_update_speed = 0.25f;
        constexpr float tile_tentacle_snap_with_path_distance = 5.f;
        constexpr int32_t segment_phase_through_terrain_frames = 15;
        constexpr int32_t max_pull_tiles_per_tick = 12;
        constexpr int32_t terrain_hits_before_phase = 20;

        bool has_any_solid(room_view const& room, ivec2 t) { return room.get_tile(t).solid(); }

        float float_dist(ivec2 a, ivec2 b) { return rw::magnitude(rw::to_vec2(a - b)); }

        // Custom.AreIntVectorsNeighbors / AreIntVectorsDiagonalNeighbors (sic)
        bool are_neighbors(ivec2 a, ivec2 b) {
            return a != b && (a.x == b.x || a.y == b.y) && std::abs(a.x - b.x) < 2 && std::abs(a.y - b.y) < 2;
        }
        bool are_diagonal_neighbors(ivec2 a, ivec2 b) {
            return a.x != b.x && a.y != b.y && (std::abs(a.x - b.x) < 2 || std::abs(a.y - b.y) < 2);
        }

        bool inside(room_view const& room, ivec2 t) { return room.inside(t.x, t.y); }

        // Room.TileRect(t).Shrink(1) clamp
        vec2 restrict_in_tile(vec2 v, ivec2 t, float shrink) {
            return vec2{mathf::clamp(v.x, float(t.x) * 20.f + shrink, float(t.x) * 20.f + 20.f - shrink),
                mathf::clamp(v.y, float(t.y) * 20.f + shrink, float(t.y) * 20.f + 20.f - shrink)};
        }

        //
        // SharedPhysics.TerrainCollisionData: VerticalCollision,
        // SlopesVertically, HorizontalCollision (goThroughFloors)
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
        // Tentacle
        //
        vec2 float_base(ctx& x, tentacle_work const& tw) { return x.pos(tw.t.connected_chunk); }
        tentacle_chunk& tip(tentacle_work& tw) { return tw.c.back(); }

        // Tentacle.grabDest
        std::optional<ivec2> grab_dest(tentacle_work const& tw) {
            if (tw.t.float_grab_dest)
                return room_view::get_tile_position(*tw.t.float_grab_dest);
            return std::nullopt;
        }

        // Tentacle.CheckIfAnySegmentsLeft
        void check_if_any_segments_left(ctx& x, tentacle_work& tw) {
            if (!tw.segments.empty())
                return;
            tw.segments.push_back(room_view::get_tile_position(float_base(x, tw)));
        }
        // Tentacle.BasePos
        ivec2 base_pos(ctx& x, tentacle_work& tw) {
            check_if_any_segments_left(x, tw);
            return tw.segments[0];
        }

        // TentacleChunk.currentSegment / StuckPos
        int32_t current_segment(tentacle_work const& tw, tentacle_chunk const& c) {
            int32_t const n = int32_t(tw.segments.size());
            return custom::int_clamp(int32_t(c.t_pos * float(n - 1)), 0, n - 1);
        }
        ivec2 stuck_pos(tentacle_work const& tw, tentacle_chunk const& c) { return tw.segments[size_t(current_segment(tw, c))]; }

        // Tentacle.Visual. The tiles a ray trace walks lie in the box of its
        // ends; when the nearest solid tile is further from `a` (the terrain
        // proximity map: a lower bound of the Manhattan distance) than that
        // box's far corner, nothing can be in the way
        bool visual(ctx& x, ivec2 a, ivec2 b) {
            int32_t const p = x.proximity(a);
            if (p > 0 && x.room.inside(b.x, b.y) && std::abs(b.x - a.x) + std::abs(b.y - a.y) < p)
                return true;
            return ray_trace_tiles_for_terrain(x.room, a, b);
        }

        // Rope.Update; a rope without bends whose tiles (the box of its old
        // and new ends, a tile wider) are all clear of the terrain finds no
        // corner to wrap, so it only takes its new ends
        void update_rope(ctx& x, world::rope_state& r, vec2 na, vec2 nb) {
            if (r.bend_count == 0) {
                ivec2 const t = room_view::get_tile_position(na);
                int32_t const p = x.proximity(t);
                if (p > 0) {
                    int32_t dx = 0, dy = 0;
                    for (vec2 const q : {r.a, r.b, nb}) {
                        ivec2 const u = room_view::get_tile_position(q);
                        dx = std::max(dx, std::abs(u.x - t.x));
                        dy = std::max(dy, std::abs(u.y - t.y));
                    }
                    if (dx + dy + 2 < p) {
                        r.last_a = r.a;
                        r.last_b = r.b;
                        r.a = na;
                        r.b = nb;
                        r.total_length = rw::distance(na, nb);
                        return;
                    }
                }
            }
            rot::update_rope(r, x.room, na, nb);
        }

        // TentacleChunk.RopeActive
        bool rope_active(tentacle_work const& tw, uint32_t i) {
            auto const& c = tw.c[i];
            return c.phase == -1.f && (i == 0 || tw.c[i - 1].phase == -1.f) && (tw.t.backtrack_from < int32_t(i) || tw.t.backtrack_from == -1);
        }

        // TentacleChunk.Reset
        void chunk_reset(ctx& x, tentacle_work& tw, tentacle_chunk& c) {
            c.trail = stuck_pos(tw, c);
            c.phase = -1.f;
            c.pos = room_view::middle_of_tile(stuck_pos(tw, c));
            c.last_pos = c.pos;
            c.vel = vec2{0.f, 0.f};
            // new Rope(room, FloatBase, FloatBase + (1, 1), 1f)
            vec2 const b = float_base(x, tw);
            world::rope_state r;
            r.a = r.last_a = b;
            r.b = r.last_b = b + vec2{1.f, 1.f};
            r.total_length = rw::distance(r.a, r.b);
            r.thickness = 1.f;
            c.rope = r;
        }

        void phase_to_segment(tentacle_chunk& c) {
            c.phase_from = c.pos;
            c.phase = 0.f;
        }

        // TentacleChunk.Update
        void chunk_update(ctx& x, tentacle_work& tw, uint32_t i) {
            auto& c = tw.c[i];
            auto& t = tw.t;
            float const n = float(tw.c.size());
            c.last_pos = c.pos;
            update_rope(x, c.rope, i == 0 ? float_base(x, tw) : tw.c[i - 1].pos, c.pos);
            if (!rope_active(tw, i) || c.phase > -1.f || (i > 0 && tw.c[i - 1].phase > -1.f) || c.rope.total_length > t.ideal_length / n * 5.f)
                c.rope.bend_count = 0;
            if (c.phase == -1.f) {
                if (c.phases_to_same_location > 0 && tw.segments[size_t(current_segment(tw, c))] == c.after_phase_stuck_pos) {
                    vec2 const m = room_view::middle_of_tile(stuck_pos(tw, c));
                    c.pos = vec2{mathf::clamp(c.pos.x, m.x - 9.f, m.x + 9.f), mathf::clamp(c.pos.y, m.y - 9.f, m.y + 9.f)};
                }
                float const ideal = t.ideal_length / n * mathf::lerp(1.f, 0.1f, t.retract_fac);
                if (i == 0) {
                    vec2 a = custom::dir_vec(c.pos, float_base(x, tw));
                    float num = rw::distance(c.pos, float_base(x, tw));
                    if (rope_active(tw, i)) {
                        a = custom::dir_vec(c.pos, rot::rope_b_connect(c.rope));
                        num = c.rope.total_length;
                    }
                    if (num > ideal) {
                        // (pullAtConnectionChunk is 0: the body isn't pulled)
                        c.pos -= a * (ideal - num);
                        c.vel -= a * (ideal - num);
                    }
                    c.stretched_fac = t.ideal_length / n / mathf::max(1.f, num);
                } else {
                    auto& prev = tw.c[i - 1];
                    vec2 a = custom::dir_vec(c.pos, prev.pos);
                    float num = rw::distance(c.pos, prev.pos);
                    if (rope_active(tw, i)) {
                        a = custom::dir_vec(c.pos, rot::rope_b_connect(c.rope));
                        num = c.rope.total_length;
                    }
                    if (num > ideal) {
                        c.pos -= a * (ideal - num) * (1.f - mass_deterioration_per_chunk);
                        c.vel -= a * (ideal - num) * (1.f - mass_deterioration_per_chunk);
                        if (rope_active(tw, i))
                            a = custom::dir_vec(rot::rope_a_connect(c.rope), prev.pos);
                        prev.pos += a * (ideal - num) * mass_deterioration_per_chunk;
                        prev.vel += a * (ideal - num) * mass_deterioration_per_chunk;
                    }
                    c.stretched_fac = t.ideal_length / n / mathf::max(1.f, num);
                }
                if (stuck_pos(tw, c) != c.trail)
                    c.trail = stuck_pos(tw, c);
                ivec2 const here = room_view::get_tile_position(c.pos);
                if (t.backtrack_from == -1 && !visual(x, here, tw.segments[size_t(current_segment(tw, c))]))
                    t.backtrack_from = int32_t(i);
                if (t.backtrack_from != -1 && int32_t(i) >= t.backtrack_from) {
                    bool flag = false;
                    int32_t const cs = current_segment(tw, c);
                    // (each ray is traced once: segment k's is segment k + 1's previous)
                    bool prev_seen = cs >= 1 && visual(x, here, tw.segments[0]);
                    for (int32_t k = 1; k <= cs && !flag; ++k) {
                        bool const seen = visual(x, here, tw.segments[size_t(k)]);
                        bool const was = prev_seen;
                        prev_seen = seen;
                        if (!seen && was) {
                            c.vel += rw::clamp_magnitude(room_view::middle_of_tile(tw.segments[size_t(k - 1)]) - c.pos, 20.f) * backtrack_speed / 20.f;
                            flag = true;
                        }
                    }
                    if (!flag) {
                        vec2 const fb = i > 0 ? tw.c[i - 1].pos : float_base(x, tw);
                        c.vel += rw::clamp_magnitude(fb - c.pos, 20.f) * backtrack_speed / 20.f;
                        if (i > 0)
                            tw.c[i - 1].vel += rw::clamp_magnitude(fb - c.pos, 20.f) * backtrack_speed / 20.f;
                    }
                    if (!flag && (c.contact_point.x != 0 || c.contact_point.y != 0)) {
                        ++c.phase_attempts;
                        if (c.phase_attempts > terrain_hits_before_phase) {
                            phase_to_segment(c);
                            c.phase_attempts = 0;
                        }
                    }
                } else {
                    c.phase_attempts = 0;
                    if (t.limp) {
                        for (int32_t k = current_segment(tw, c); k >= 0; --k) {
                            ivec2 const s = tw.segments[size_t(k)];
                            if (visual(x, here, s)) {
                                c.vel += rw::clamp_magnitude(room_view::middle_of_tile(s) - c.pos, 20.f) * 0.005f;
                                break;
                            }
                        }
                    }
                    // (goalAttractionSpeed and alignToSegmentSpeed are 0)
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
                    c.pos = rw::lerp(c.phase_from, room_view::middle_of_tile(stuck_pos(tw, c)), mathf::min(1.f, c.phase));
                c.vel = vec2{0.f, 0.f};
                if (c.phase >= 1.f) {
                    c.phase = -1.f;
                    if (c.after_phase_stuck_pos == stuck_pos(tw, c))
                        ++c.phases_to_same_location;
                    else
                        c.phases_to_same_location = 0;
                    c.after_phase_stuck_pos = stuck_pos(tw, c);
                }
                if (i > 0 && tw.c[i - 1].phase == -1.f) {
                    auto& prev = tw.c[i - 1];
                    vec2 const a = custom::dir_vec(c.pos, prev.pos);
                    float const num = rw::distance(c.pos, prev.pos);
                    float const ideal = t.ideal_length / n * mathf::lerp(1.f, 0.1f, t.retract_fac);
                    if (num > ideal) {
                        prev.pos += a * (ideal - num) * mass_deterioration_per_chunk;
                        prev.vel += a * (ideal - num) * mass_deterioration_per_chunk;
                    }
                    c.stretched_fac = ideal / num;
                }
            }
        }

        // Tentacle.MoveSegment
        bool move_segment(ctx& x, tentacle_work& tw, int32_t s, ivec2 dest, bool allow_occupied, bool allow_solid) {
            auto& seg = tw.segments;
            if (s == 0 || seg[size_t(s)] == dest || (!are_neighbors(seg[size_t(s - 1)], dest) && seg[size_t(s - 1)] != dest) || (!allow_solid && has_any_solid(x.room, dest)))
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
                for (auto const& c : cand) {
                    float num2 = 1.f;
                    if (has_any_solid(x.room, c))
                        num2 += 1000.f;
                    for (auto const& q : seg)
                        if (c == q)
                            num2 += 1.f;
                    if (num2 < num) {
                        best = c;
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
        void shorten(tentacle_work& tw, int32_t from, int32_t to, std::vector<ivec2> const& straight, int32_t count) {
            tw.segments.erase(tw.segments.begin() + from + 1, tw.segments.begin() + to);
            for (int32_t i = count - 2; i > 0; --i)
                tw.segments.insert(tw.segments.begin() + from + 1, straight[size_t(i)]);
        }

        // Tentacle.PullAtTentacle
        void pull_at_tentacle(ctx& x, tentacle_work& tw, int32_t start, bool only_simple_pull, std::vector<ivec2>& path) {
            auto& t = tw.t;
            auto& seg = tw.segments;
            if (!only_simple_pull) {
                while (t.pull_counter > 0) {
                    bool flag = false;
                    for (int32_t num = start; num < int32_t(seg.size()) - 3 && !flag; ++num) {
                        for (int32_t i = int32_t(seg.size()) - 1; i >= num + 3; --i) {
                            int32_t const num2 = i - num - custom::manhattan_distance(seg[size_t(num)], seg[size_t(i)]);
                            if (num2 > 0 && num2 <= t.pull_counter && visual(x, seg[size_t(num)], seg[size_t(i)])) {
                                int32_t const num3 = ray_trace_tiles_list(seg[size_t(num)].x, seg[size_t(num)].y, seg[size_t(i)].x, seg[size_t(i)].y, path);
                                bool flag2 = true;
                                if (num2 > 1 && num2 <= t.pulls_this_tick) {
                                    for (int32_t j = 1; j < num3 - 1; ++j) {
                                        for (int32_t k = num + 1; k < i; ++k) {
                                            if (custom::manhattan_distance(seg[size_t(k)], path[size_t(j)]) > 2 && !visual(x, seg[size_t(k)], path[size_t(j)])) {
                                                flag2 = false;
                                                break;
                                            }
                                        }
                                    }
                                }
                                if (flag2) {
                                    t.pulls_this_tick -= num2;
                                    t.pull_counter -= num2;
                                    shorten(tw, num, i, path, num3);
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
        // back to the start over every tile (solid ones at 1000 extra), the
        // path running from the start up to the goal's neighbour. The open
        // list is a heap on (heuristic, insertion order), which picks what
        // vanilla's first-lowest scan of its List does
        int32_t find_tentacle_path(ctx& x, ivec2 start, ivec2 goal, std::vector<ivec2>& path) {
            struct cell {
                ivec2 pos;
                int32_t parent;
            };
            static thread_local std::vector<cell> cells;
            static thread_local std::vector<uint32_t> visited;
            static thread_local uint32_t stamp = 0;
            int32_t const w = x.room.width, h = x.room.height;
            size_t const size = size_t(w) * size_t(h);
            if (visited.size() < size)
                visited.assign(size, 0);
            if (++stamp == 0) {
                std::fill(visited.begin(), visited.end(), 0);
                stamp = 1;
            }
            using node = std::pair<float, uint32_t>; // heuristic, cell (insertion order)
            std::priority_queue<node, std::vector<node>, std::greater<>> open;
            cells.clear();
            cells.push_back(cell{goal, -1});
            open.push({0.f, 0});
            int32_t found = -1;
            vec2 const s = rw::to_vec2(start);
            while (!open.empty() && found < 0) {
                uint32_t const ci = open.top().second;
                open.pop();
                ivec2 const p = cells[ci].pos;
                for (auto const d : custom::four_directions) {
                    ivec2 const q = p + d;
                    if (q.x < 0 || q.x >= w || q.y < 0 || q.y >= h)
                        continue;
                    uint32_t& v = visited[size_t(q.x) * size_t(h) + size_t(q.y)];
                    if (v == stamp)
                        continue;
                    v = stamp;
                    float num2 = rw::distance(rw::to_vec2(q), s) + (has_any_solid(x.room, q) ? 1000.f : 0.f);
                    cells.push_back(cell{q, int32_t(ci)});
                    if (q == start) {
                        found = int32_t(cells.size() - 1);
                        break;
                    }
                    open.push({num2, uint32_t(cells.size() - 1)});
                }
            }
            int32_t n = 0;
            for (int32_t k = found; k >= 0 && cells[size_t(k)].parent >= 0; k = cells[size_t(k)].parent) {
                if (int32_t(path.size()) <= n)
                    path.push_back(cells[size_t(k)].pos);
                else
                    path[size_t(n)] = cells[size_t(k)].pos;
                ++n;
            }
            return n;
        }

        // Tentacle.MoveAlignedSegmentsWithPath
        void move_aligned_segments_with_path(ctx& x, tentacle_work& tw, std::vector<ivec2>& path) {
            auto& seg = tw.segments;
            auto& gp = tw.grab_path;
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
            tw.t.pull_counter += num3 - 1;
            pull_at_tentacle(x, tw, num, true, path);
        }

        // Tentacle.FindGrabPath
        void find_grab_path(ctx& x, tentacle_work& tw, std::vector<ivec2>& path) {
            ivec2 const g = *grab_dest(tw);
            ivec2 const b = base_pos(x, tw);
            int32_t num;
            if (!inside(x.room, g) || !inside(x.room, b) || visual(x, b, g))
                num = ray_trace_tiles_list(b.x, b.y, g.x, g.y, tw.grab_path);
            else
                num = find_tentacle_path(x, b, g, tw.grab_path);
            if (int32_t(tw.grab_path.size()) > num)
                tw.grab_path.resize(size_t(num));
            move_aligned_segments_with_path(x, tw, path);
        }

        // Tentacle.MoveGrabDest
        void move_grab_dest(ctx& x, tentacle_work& tw, vec2 to, std::vector<ivec2>& path) {
            if (auto const g = grab_dest(tw); g && *g == room_view::get_tile_position(to))
                return;
            tw.t.float_grab_dest = to;
            find_grab_path(x, tw, path);
        }

        // Tentacle.Grow
        void grow(ctx& x, tentacle_work& tw, int32_t add) {
            auto& seg = tw.segments;
            for (int32_t i = 0; i < add; ++i) {
                float num = float_max;
                ivec2 item = seg.back();
                for (auto const d : four_directions_and_zero) {
                    float num2 = x.rng.value();
                    ivec2 const c = seg.back() + d;
                    if (has_any_solid(x.room, c))
                        num2 += 10000.f;
                    for (auto const& q : seg)
                        if (q == c)
                            num2 += 1.f;
                    if (num2 < num) {
                        num = num2;
                        item = c;
                    }
                }
                seg.push_back(item);
            }
        }

        // Tentacle.MoveBase
        void move_base(ctx& x, tentacle_work& tw, ivec2 to, std::vector<ivec2>& path) {
            auto& seg = tw.segments;
            if (has_any_solid(x.room, to) || to == base_pos(x, tw))
                return;
            if (are_neighbors(seg[0], to)) {
                int32_t num = -1;
                for (int32_t i = 1; i < int32_t(seg.size()) - 1; ++i)
                    if (seg[size_t(i)] == to)
                        num = i;
                if (num > 0) {
                    seg.erase(seg.begin(), seg.begin() + num);
                    grow(x, tw, num);
                } else {
                    seg.insert(seg.begin(), to);
                    ++tw.t.pull_counter;
                    if (tw.t.pull_counter > 4)
                        pull_at_tentacle(x, tw, 0, false, path);
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
                tw.t.pull_counter += num2 - 1;
                if (tw.t.pull_counter > 4)
                    pull_at_tentacle(x, tw, 0, false, path);
            }
            if (grab_dest(tw))
                find_grab_path(x, tw, path);
        }

        // Tentacle.AlignWithGrabPath
        void align_with_grab_path(ctx& x, tentacle_work& tw, std::vector<ivec2>& path) {
            auto& seg = tw.segments;
            auto& gp = tw.grab_path;
            bool flag = false;
            for (int32_t i = std::min(int32_t(seg.size()) - 2, int32_t(gp.size()) - 1); i > 0; --i) {
                if (i >= int32_t(seg.size()) || i >= int32_t(gp.size()))
                    continue;
                if (seg[size_t(i)] != gp[size_t(i)] && visual(x, seg[size_t(i)], gp[size_t(i)])) {
                    ray_trace_tiles_list(seg[size_t(i)].x, seg[size_t(i)].y, gp[size_t(i)].x, gp[size_t(i)].y, path);
                    if (i + 1 < int32_t(seg.size()) && move_segment(x, tw, i + 1, path[1], false, false))
                        flag = true;
                }
            }
            if (!flag) {
                for (int32_t k = 1; k < int32_t(seg.size()) && k < int32_t(gp.size()); ++k) {
                    if (seg[size_t(k)] != gp[size_t(k)]) {
                        move_segment(x, tw, k, gp[size_t(k)], true, false);
                        return;
                    }
                }
            }
        }

        // Tentacle.CurrentLength
        float current_length(tentacle_work const& tw) {
            float num = float(tw.segments.size()) * 20.f;
            for (size_t i = 0; i + 2 < tw.segments.size(); ++i)
                if (are_diagonal_neighbors(tw.segments[i], tw.segments[i + 2]))
                    num -= 5.9000006f;
            return num;
        }

        // Tentacle.AdjustLength (`shorten` is off for daddies)
        void adjust_length(ctx& x, tentacle_work& tw, std::vector<ivec2>& path) {
            float const num = current_length(tw);
            int32_t const num3 = int32_t(tw.t.ideal_length / 20.f - num / 20.f);
            if (num3 > 0 && (tw.grab_path.empty() || tw.segments.size() < tw.grab_path.size())) {
                grow(x, tw, 1);
                return;
            }
            if (num3 < 0) {
                ++tw.t.pull_counter;
                pull_at_tentacle(x, tw, 0, false, path);
            }
        }

        // DaddyTentacle.GravityDirection
        ivec2 gravity_direction(ctx& x, tentacle_work& tw) {
            if (x.rng.value() >= 0.5f)
                return ivec2{0, -1};
            return ivec2{tip(tw).pos.x < float_base(x, tw).x ? -1 : 1, -1};
        }

        // Tentacle.Gravity
        void gravity(ctx& x, tentacle_work& tw) {
            ivec2 const b = gravity_direction(x, tw);
            bool flag = false;
            for (int32_t i = 0; i < int32_t(tw.segments.size()) && !flag; ++i)
                if (move_segment(x, tw, i, tw.segments[size_t(i)] + b, false, false))
                    flag = true;
            if (!flag)
                for (int32_t i = 0; i < int32_t(tw.segments.size()); ++i)
                    if (move_segment(x, tw, i, tw.segments[size_t(i)] + b, true, false))
                        break;
        }

        // Tentacle.Update
        void base_update(ctx& x, tentacle_work& tw) {
            auto& path = x.scratch;
            move_base(x, tw, room_view::get_tile_position(float_base(x, tw)), path);
            tw.t.update_counter += tile_tentacle_update_speed;
            if (tw.t.update_counter >= 1.f) {
                tw.t.pulls_this_tick = max_pull_tiles_per_tick;
                if (tw.t.limp)
                    gravity(x, tw);
                else if (!tw.grab_path.empty())
                    align_with_grab_path(x, tw, path);
                adjust_length(x, tw, path);
                tw.t.update_counter -= 1.f;
            }
            tw.t.backtrack_from = -1;
            for (uint32_t i = 0; i < tw.c.size(); ++i)
                chunk_update(x, tw, i);
        }

        // Tentacle.PushChunksApart
        void push_chunks_apart(tentacle_work& tw, uint32_t a, uint32_t b) {
            auto& ca = tw.c[a];
            auto& cb = tw.c[b];
            vec2 const d = custom::dir_vec(ca.pos, cb.pos);
            float const num = rw::distance(ca.pos, cb.pos);
            if (num < 10.f) {
                ca.pos -= d * (10.f - num) * 0.5f;
                ca.vel -= d * (10.f - num) * 0.5f;
                cb.pos += d * (10.f - num) * 0.5f;
                cb.vel += d * (10.f - num) * 0.5f;
            }
        }

        //
        // DaddyTentacle
        //
        dcon::body_chunk_id player_chunk(ctx& x, uint8_t player, uint8_t chunk) {
            auto const p = player_of(x.state, player);
            return p ? player::body_chunk(x.state, p, chunk) : dcon::body_chunk_id{};
        }

        void release_grab(ctx& x, tentacle_work& tw) {
            if (auto const c = player_chunk(x, tw.t.grab_player, tw.t.grab_chunk))
                x.state.play_sound(sound::sound_id::daddy_and_bro_tentacle_release_creature, x.room.id, x.state.world.body_chunk_get_pos(c));
            tw.t.grab_player = 0;
        }

        // DaddyTentacle.IsCreatureCaughtEnough (a slugcat's bodySize is 1)
        bool is_creature_caught_enough(ctx& x, uint8_t player) {
            int32_t num = 0;
            for (auto const& o : x.tents)
                if (o.t.grab_player == player)
                    ++num;
            return float(num) >= 1.f * (x.size_class ? 1.5f : 2.5f);
        }

        // DaddyTentacle.CollideWithCreature
        void collide_with_creature(ctx& x, tentacle_work& tw, uint32_t j, dcon::body_chunk_id cc) {
            if (tw.t.backtrack_from > -1 && tw.t.backtrack_from <= int32_t(j))
                return;
            auto& w = x.state.world;
            auto& tc = tw.c[j];
            vec2& cp = w.body_chunk_get_pos(cc);
            float const num = rw::distance(tc.pos, cp);
            float const num2 = (tc.rad + w.body_chunk_get_rad(cc)) / 4.f;
            vec2 const a = custom::dir_vec(tc.pos, cp);
            float const m = w.body_chunk_get_mass(cc);
            float const num3 = m / (m + 0.01f);
            constexpr float d = 0.8f;
            tc.pos += a * (num - num2) * num3 * d;
            tc.vel += a * (num - num2) * num3 * d;
            cp -= a * (num - num2) * (1.f - num3) * d;
            w.body_chunk_get_vel(cc) -= a * (num - num2) * (1.f - num3) * d;
        }

        // DaddyTentacle.Touch: over the slugcats in the room (the creatures
        // here the tentacles feel)
        void touch(ctx& x, tentacle_work& tw) {
            auto& w = x.state.world;
            auto& t = tw.t;
            bool flag = false;
            bool done = false;
            rooms::for_each_player(x.state, x.room.id, [&](dcon::player_id p) {
                if (done || !player_here(x.state, p, x.room.id))
                    return;
                uint8_t const ref = player_ref(p);
                for (uint32_t j = 0; j < tw.c.size(); ++j) {
                    for (uint32_t k = 0; k < 2; ++k) {
                        auto const pc = player::body_chunk(x.state, p, k);
                        if (!custom::dist_less(tw.c[j].pos, w.body_chunk_get_pos(pc), tw.c[j].rad + w.body_chunk_get_rad(pc)))
                            continue;
                        if (x.z.eyes_closed < 1 || x.rng.value() < 0.05f) {
                            see_player(x, p);
                            if (x.state.viewed(x.room.id))
                                daddy_graphics::feel_something(x.state, x.d, ref, tw.c[j].pos);
                        }
                        collide_with_creature(x, tw, j, pc);
                        if (!t.needed_for_locomotion && t.grab_player == 0 && j == tw.c.size() - 1 && (x.size_class || x.z.digesting_counter <= 0)
                        && (x.z.eyes_closed < 1 || x.rng.value() < (x.size_class ? 0.5f : 0.15f))
                        && (t.current == task::hunt || !is_creature_caught_enough(x, ref))) {
                            flag = true;
                            if (rw::distance(tw.c[j].vel, w.body_chunk_get_vel(pc)) >= mathf::lerp(1.f, 8.f, t.sticky))
                                break;
                            // (it eats slugcats: the relationship is Eats)
                            bool flag3 = x.seen_player == ref;
                            for (uint32_t m = 0; m < tw.c.size() && flag3; ++m)
                                if (tw.c[m].phase > -1.f || x.room.get_tile(tw.c[m].pos).solid())
                                    flag3 = false;
                            if (flag3) {
                                t.grab_player = ref;
                                t.grab_chunk = uint8_t(k);
                                x.state.play_sound(sound::sound_id::daddy_and_bro_tentacle_grab_creature, x.room.id, tw.c[j].pos);
                                switch_task(x, tw, task::grabbing);
                                done = true;
                                return;
                            }
                            break;
                        }
                        if (t.needed_for_locomotion || (t.current != task::locomotion && t.current != task::examine_sound) || is_creature_caught_enough(x, ref))
                            break;
                        if (x.seen_player != ref)
                            break;
                        bool flag4 = false;
                        for (auto const& o : x.tents)
                            flag4 = flag4 || o.t.hunt_player == ref;
                        if (!flag4) {
                            t.hunt_player = ref;
                            if (auto* s = source(x.a, t.check_sound))
                                s->slated_for_deletion = 1;
                            t.check_sound = 0;
                            switch_task(x, tw, task::hunt);
                        }
                        break;
                    }
                }
            });
            if (done)
                return;
            if (flag)
                t.sticky = mathf::min(1.f, t.sticky + 0.033333335f);
            else
                t.sticky = mathf::max(0.f, t.sticky - 0.016666668f);
        }

        // DaddyTentacle.LookForCreaturesToHunt (PreyTracker: the one slugcat)
        void look_for_creatures_to_hunt(ctx& x, tentacle_work& tw) {
            if (tw.t.needed_for_locomotion || x.seen_player == 0)
                return;
            dcon::player_id const p = player_of(x.state, x.seen_player);
            // GetTrackedPrey(Random.Range(0, TotalTrackedPrey))
            x.rng.range(0, 1);
            uint8_t const ref = x.seen_player;
            for (auto const& o : x.tents)
                if (o.t.hunt_player == ref)
                    return;
            if (is_creature_caught_enough(x, ref))
                return;
            if (!p || !player_here(x.state, p, x.room.id))
                return;
            if (rw::distance(room_view::middle_of_tile(best_guess(x)), float_base(x, tw)) > tw.t.ideal_length + 40.f)
                return;
            if (auto* s = source(x.a, tw.t.check_sound))
                s->slated_for_deletion = 1;
            tw.t.check_sound = 0;
            tw.t.hunt_player = ref;
            switch_task(x, tw, task::hunt);
        }

        // DaddyTentacle.LookForSoundsToExamine
        void look_for_sounds_to_examine(ctx& x, tentacle_work& tw) {
            auto& nt = x.a.noise;
            if (!x.size_class || tw.t.needed_for_locomotion || nt.count == 0)
                return;
            int32_t num = std::numeric_limits<int32_t>::max();
            int32_t num2 = -1;
            for (int32_t i = 0; i < nt.count; ++i) {
                auto const& s = nt.sources[i];
                if (custom::dist_less(s.pos, float_base(x, tw), tw.t.ideal_length * 0.85f) && s.age < num && s.player == 0) {
                    bool flag = false;
                    for (auto const& o : x.tents)
                        flag = flag || o.t.check_sound == s.id;
                    if (!flag) {
                        num = s.age;
                        num2 = i;
                    }
                }
            }
            if (num2 > -1) {
                tw.t.check_sound = nt.sources[num2].id;
                switch_task(x, tw, task::examine_sound);
            }
        }

        // DaddyTentacle.ConsiderGrabPos / GrabPosScore
        float grab_pos_score(ctx& x, tentacle_work& tw, vec2 test, vec2 ideal) {
            float num = 100.f / rw::distance(test, ideal);
            if (auto const g = grab_dest(tw); g && room_view::get_tile_position(test) == *g)
                num *= 1.5f;
            for (auto const d : custom::four_directions)
                if (x.room.get_tile(test + rw::to_vec2(d) * 20.f).solid()) {
                    num *= 2.f;
                    break;
                }
            return num;
        }
        void consider_grab_pos(ctx& x, tentacle_work& tw, vec2 test, vec2 ideal) {
            if (grab_pos_score(x, tw, test, ideal) > grab_pos_score(x, tw, tw.t.preliminary_grab_dest, ideal))
                tw.t.preliminary_grab_dest = test;
        }

        // DaddyTentacle.SecondaryGrabPosScore / ConsiderSecondaryGrabPos
        float secondary_grab_pos_score(ctx& x, tentacle_work& tw, ivec2 test) {
            auto const g = grab_dest(tw);
            if (!g)
                return 0.f;
            if (float_dist(test, base_pos(x, tw)) < 7.f)
                return 0.f;
            float const num = tw.t.ideal_length - float(tw.grab_path.size()) * 20.f;
            vec2 const fg = *tw.t.float_grab_dest;
            if (rw::distance(room_view::middle_of_tile(test), fg) > num)
                return 0.f;
            if (!visual(x, *g, test))
                return 0.f;
            float num2 = 0.f;
            for (auto const d : custom::eight_directions)
                if (x.room.get_tile(test + d).solid())
                    num2 += 1.f;
            if (x.room.get_tile(test).any_beam())
                num2 += 1.f;
            if (num2 > 0.f && test == tw.t.secondary_grab_pos)
                num2 += 1.f;
            if (num2 == 0.f)
                return 0.f;
            num2 += float_dist(test, base_pos(x, tw)) / 10.f;
            return num2 / (1.f + std::fabs(num * 0.75f - rw::distance(room_view::middle_of_tile(test), fg))
                + rw::distance(room_view::middle_of_tile(test), room_view::middle_of_tile(tw.segments.back())));
        }
        void consider_secondary_grab_pos(ctx& x, tentacle_work& tw, ivec2 test) {
            if (x.room.get_tile(test).solid())
                return;
            if (secondary_grab_pos_score(x, tw, test) > secondary_grab_pos_score(x, tw, tw.t.secondary_grab_pos)) {
                tw.t.secondary_grab_back_track_counter = 0;
                tw.t.secondary_grab_pos = test;
            }
        }

        // DaddyTentacle.StickToTerrain
        void stick_to_terrain(ctx& x, tentacle_work& tw, uint32_t ci) {
            auto& chunk = tw.c[ci];
            if (tw.t.float_grab_dest && !custom::dist_less(chunk.pos, *tw.t.float_grab_dest, 200.f))
                return;
            vec2 const mid = room_view::middle_of_tile(chunk.pos);
            int32_t const num = int32_t(mathf::sign(chunk.pos.x - mid.x));
            vec2 v{0.f, 0.f};
            ivec2 const tp = room_view::get_tile_position(chunk.pos);
            for (auto const d : custom::eight_directions_diagonals_last) {
                if (x.room.get_tile(tp + ivec2{d.x * num, d.y}).solid()) {
                    if (d.x != 0)
                        v.x = mid.x + float(d.x * num) * (20.f - chunk.rad);
                    if (d.y != 0)
                        v.y = mid.y + float(d.y) * (20.f - chunk.rad);
                    break;
                }
            }
            auto const tile = x.room.get_tile(chunk.pos);
            if (v.x == 0.f && tile.vertical_beam())
                v.x = mid.x;
            if (v.y == 0.f && tile.horizontal_beam())
                v.y = mid.y;
            int32_t const n = int32_t(tw.c.size());
            if (int32_t(ci) > n / 2) {
                float const vol = mathf::inverse_lerp(float(n / 2), float(n - 1), float(ci));
                if (v.x != 0.f || v.y != 0.f) {
                    if (chunk.stick_sounds > 10)
                        x.state.play_sound(sound::sound_id::daddy_and_bro_tentacle_grab_terrain, x.room.id, chunk.pos, vol);
                    if (chunk.stick_sounds > 0)
                        chunk.stick_sounds = 0;
                    else
                        --chunk.stick_sounds;
                } else {
                    if (chunk.stick_sounds < -10)
                        x.state.play_sound(sound::sound_id::daddy_and_bro_tentacle_release_terrain, x.room.id, chunk.pos, vol);
                    if (chunk.stick_sounds < 0)
                        chunk.stick_sounds = 0;
                    else
                        ++chunk.stick_sounds;
                }
            }
            if (v.x != 0.f) {
                chunk.vel.x += (v.x - chunk.pos.x) * 0.1f;
                chunk.vel.y *= 0.9f;
            }
            if (v.y != 0.f) {
                chunk.vel.y += (v.y - chunk.pos.y) * 0.1f;
                chunk.vel.x *= 0.9f;
            }
            if (v.x != 0.f || v.y != 0.f)
                tw.t.chunks_gripping += 1.f / float(n);
        }

        // DaddyTentacle.Climb
        void climb(ctx& x, tentacle_work& tw, std::vector<ivec2>& path) {
            auto& t = tw.t;
            vec2 const fb = float_base(x, tw);
            float const k = custom::lerp_map(float(x.z.stuck_counter), 50.f, 200.f, 0.5f, 0.95f);
            vec2 const dir = rw::slerp(t.tentacle_dir, x.z.move_direction, k);
            t.ideal_grab_pos = fb + dir * t.ideal_length * 0.7f;
            vec2 const rnd = custom::rnv(x.rng);
            vec2 const v = fb + rw::slerp(dir, rnd, mathf::inverse_lerp(20.f, 200.f, float(t.found_no_grab_pos))) * t.ideal_length
                * custom::lerp_map(float(std::max(t.found_no_grab_pos, x.z.stuck_counter)), 20.f, 200.f, 0.7f, 1.2f);
            static thread_local std::vector<ivec2> rays;
            ray_traced_tiles(fb, v, rays);
            bool flag = false;
            for (size_t j = 0; j + 1 < rays.size(); ++j) {
                if (x.room.get_tile(rays[j + 1]).solid()) {
                    consider_grab_pos(x, tw, restrict_in_tile(v, rays[j], 1.f), t.ideal_grab_pos);
                    flag = true;
                    break;
                }
                if (x.room.get_tile(rays[j]).any_beam()) {
                    consider_grab_pos(x, tw, room_view::middle_of_tile(rays[j]), t.ideal_grab_pos);
                    flag = true;
                }
            }
            if (flag)
                t.found_no_grab_pos = 0;
            else
                ++t.found_no_grab_pos;
            bool const flag2 = t.secondary_grab_back_track_counter < 200 && secondary_grab_pos_score(x, tw, t.secondary_grab_pos) > 0.f;
            for (uint32_t i = 0; i < tw.c.size(); ++i) {
                if (t.backtrack_from != -1 && t.backtrack_from <= int32_t(i))
                    continue;
                stick_to_terrain(x, tw, i);
                if (!t.float_grab_dest)
                    continue;
                auto& c = tw.c[i];
                vec2 const fg = *t.float_grab_dest;
                if (!t.at_grab_dest && custom::dist_less(c.pos, fg, 20.f))
                    t.at_grab_dest = 1;
                int32_t const cs = current_segment(tw, c);
                if (cs <= int32_t(tw.grab_path.size()) || !flag2) {
                    c.vel += rw::clamp_magnitude(fg - c.pos, 20.f) / 20.f * 1.2f;
                } else if (i > 1 && tw.segments.size() > tw.grab_path.size() && flag2) {
                    float const num = mathf::inverse_lerp(float(tw.grab_path.size()), float(tw.segments.size()), float(cs));
                    vec2 a = custom::dir_vec(tw.c[i - 2].pos, c.pos) * (1.f - num) * 0.6f;
                    a += custom::dir_vec(c.pos, room_view::middle_of_tile(*grab_dest(tw))) * mathf::pow(1.f - num, 4.f) * 2.f;
                    a += custom::dir_vec(c.pos, room_view::middle_of_tile(t.secondary_grab_pos)) * mathf::pow(num, 4.f) * 2.f;
                    a += custom::dir_vec(c.pos, fb) * std::sin(num * mathf::pi) * 0.3f;
                    c.vel += rw::normalized(a) * 1.2f;
                    if (i == tw.c.size() - 1)
                        c.vel += rw::clamp_magnitude(room_view::middle_of_tile(t.secondary_grab_pos) - c.pos, 20.f) / 20.f * 4.2f;
                }
            }
            if (auto const g = grab_dest(tw)) {
                int32_t const dx = x.rng.range(-20, 21);
                int32_t const dy = x.rng.range(-20, 21);
                consider_secondary_grab_pos(x, tw, *g + ivec2{dx, dy});
            }
            if (!grab_dest(tw) || !t.at_grab_dest)
                update_climb_grab_pos(x, tw);
        }

        // the chunks of a hunting or examining tentacle go for the grab
        // destination when they see it, else along their segments
        void reach(ctx& x, tentacle_work& tw) {
            for (uint32_t l = 0; l < tw.c.size(); ++l) {
                if (tw.t.backtrack_from != -1 && tw.t.backtrack_from <= int32_t(l))
                    continue;
                auto& c = tw.c[l];
                if (tw.t.float_grab_dest && x.room.visual_contact(c.pos, *tw.t.float_grab_dest))
                    c.vel += rw::clamp_magnitude(*tw.t.float_grab_dest - c.pos, 20.f) / 20.f * 1.2f;
                else
                    c.vel += rw::clamp_magnitude(room_view::middle_of_tile(tw.segments[size_t(current_segment(tw, c))]) - c.pos, 20.f) / 20.f * 1.2f;
            }
        }

        // DaddyTentacle.Hunt
        void hunt(ctx& x, tentacle_work& tw, std::vector<ivec2>& path) {
            auto& t = tw.t;
            dcon::player_id const p = player_of(x.state, t.hunt_player);
            if (!p || x.seen_player != t.hunt_player || !player_here(x.state, p, x.room.id)) {
                switch_task(x, tw, task::locomotion);
                return;
            }
            if (x.sees_player)
                move_grab_dest(x, tw, x.state.world.body_chunk_get_pos(player::body_chunk(x.state, p, 0)), path);
            else
                move_grab_dest(x, tw, room_view::middle_of_tile(best_guess(x)), path);
            if (float(tw.grab_path.size()) * 20.f > t.ideal_length || t.needed_for_locomotion) {
                float num = float_max;
                int32_t num2 = -1;
                for (int32_t k = 0; k < int32_t(x.tents.size()); ++k) {
                    auto const& o = x.tents[size_t(k)].t;
                    if (o.current == task::locomotion && !o.needed_for_locomotion && (o.ideal_length > t.ideal_length || t.needed_for_locomotion) && !o.at_grab_dest
                    && std::fabs(o.ideal_length - float(tw.grab_path.size()) * 20.f) < num) {
                        num = std::fabs(o.ideal_length - float(tw.grab_path.size()) * 20.f);
                        num2 = k;
                    }
                }
                if (num2 > -1) {
                    x.tents[size_t(num2)].t.hunt_player = t.hunt_player;
                    x.tents[size_t(num2)].t.current = task::hunt;
                    t.hunt_player = 0;
                    update_climb_grab_pos(x, tw);
                    return;
                }
            }
            if (rw::distance(room_view::middle_of_tile(best_guess(x)), float_base(x, tw)) > t.ideal_length * 1.5f) {
                t.hunt_player = 0;
                update_climb_grab_pos(x, tw);
                return;
            }
            reach(x, tw);
        }

        // DaddyTentacle.ExamineSound
        void examine_sound(ctx& x, tentacle_work& tw, std::vector<ivec2>& path) {
            auto& t = tw.t;
            auto* s = source(x.a, t.check_sound);
            if (!s || !custom::dist_less(s->pos, float_base(x, tw), t.ideal_length * 1.1f) || s->slated_for_deletion) {
                switch_task(x, tw, task::locomotion);
                return;
            }
            --t.sound_check_timer;
            if (t.sound_check_timer < 1 || !t.examine_sound_pos || custom::dist_less(*t.examine_sound_pos, tip(tw).pos, 20.f)) {
                if (t.examine_sound_pos) {
                    t.sound_check_timer = x.rng.range(40, 180);
                    ++t.sound_check_counter;
                    if (t.sound_check_counter > 17) {
                        s->slated_for_deletion = 1;
                        switch_task(x, tw, task::locomotion);
                        return;
                    }
                    t.examine_sound_pos.reset();
                }
                static thread_local std::vector<ivec2> rays;
                ray_traced_tiles(s->pos, s->pos + custom::rnv(x.rng) * 150.f, rays);
                size_t num = rays.size();
                for (size_t i = 0; i + 1 < rays.size(); ++i)
                    if (x.room.get_tile(rays[i + 1]).solid()) {
                        num = i;
                        break;
                    }
                if (rays.size() > num + 1)
                    rays.resize(num + 1);
                while (!rays.empty()) {
                    ivec2 const r = rays.back();
                    // (the 5% beam roll only happens when the proximity test fails)
                    if (x.proximity(r) < 2 || (x.room.get_tile(r).any_beam() && x.rng.value() < 0.05f)) {
                        vec2 const m = room_view::middle_of_tile(r);
                        t.examine_sound_pos = restrict_in_tile(m + custom::dir_vec(float_base(x, tw), m) * 20.f, r, 1.f);
                        break;
                    }
                    rays.pop_back();
                }
            }
            if (t.examine_sound_pos)
                move_grab_dest(x, tw, *t.examine_sound_pos, path);
            reach(x, tw);
        }
    }

    // Room.RayTraceTilesList
    int32_t ray_trace_tiles_list(int32_t x0, int32_t y0, int32_t x1, int32_t y1, std::vector<ivec2>& path) {
        int32_t num = std::abs(x1 - x0);
        int32_t num2 = std::abs(y1 - y0);
        int32_t x = x0, y = y0;
        // (bounded: a chunk flung far out of the room can't stall the tick)
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

    // Room.RayTraceTilesForTerrain / SharedPhysics.RayTraceTilesForTerrain
    bool ray_trace_tiles_for_terrain(room_view const& room, ivec2 a, ivec2 b) noexcept {
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

    // SharedPhysics.RayTracedTilesArray
    void ray_traced_tiles(vec2 a, vec2 b, std::vector<ivec2>& out) {
        out.clear();
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
            out.push_back(ivec2{x, y});
            if (t_next_v < t_next_h) {
                y += y_inc;
                t_next_v += dt_dy;
            } else {
                x += x_inc;
                t_next_h += dt_dx;
            }
        }
    }

    void tentacle_reset(ctx& x, tentacle_work& tw, vec2 at) {
        tw.segments.assign(1, room_view::get_tile_position(at));
        for (auto& c : tw.c) {
            c.pos = at;
            c.vel = vec2{0.f, 0.f};
            chunk_reset(x, tw, c);
        }
    }

    void tentacle_new_room(ctx& x, tentacle_work& tw) {
        // Tentacle.NewRoom
        tw.segments.assign(size_t(int32_t(tw.t.ideal_length / 20.f)), room_view::get_tile_position(x.pos(0)));
        for (auto& c : tw.c)
            chunk_reset(x, tw, c);
        tw.grab_path.clear();
        // DaddyTentacle.NewRoom
        switch_task(x, tw, task::locomotion);
    }

    void tentacle_place(ctx& x, tentacle_work& tw, vec2 middle) {
        vec2 const fb = float_base(x, tw);
        ivec2 const a = room_view::get_tile_position(fb);
        ivec2 const b = room_view::get_tile_position(fb + custom::dir_vec(middle, fb) * tw.t.ideal_length);
        std::vector<ivec2> list;
        int32_t const n = ray_trace_tiles_list(a.x, a.y, b.x, b.y, list);
        list.resize(size_t(n));
        for (size_t j = 1; j < list.size(); ++j)
            if (x.room.get_tile(list[j]).solid()) {
                list.resize(j);
                break;
            }
        tw.segments = list;
        for (auto& c : tw.c)
            chunk_reset(x, tw, c);
        // (vanilla hands the segment list itself to MoveGrabDest as its
        // scratch path, so the first ray trace overwrites the base tiles: the
        // tentacle starts out folded back and straightens itself out)
        move_grab_dest(x, tw, room_view::middle_of_tile(list.back()), tw.segments);
    }

    void switch_task(ctx& x, tentacle_work& tw, task nt) {
        auto& t = tw.t;
        if (nt != task::hunt)
            t.hunt_player = 0;
        if (nt != task::examine_sound) {
            t.check_sound = 0;
            t.examine_sound_pos.reset();
        }
        if (nt != task::grabbing && t.grab_player != 0)
            release_grab(x, tw);
        if (nt == task::locomotion && t.current != task::locomotion)
            update_climb_grab_pos(x, tw);
        if (nt == task::examine_sound) {
            t.sound_check_timer = 0;
            t.sound_check_counter = 0;
        }
        t.current = nt;
    }

    void update_climb_grab_pos(ctx& x, tentacle_work& tw) {
        if (tw.t.hunt_player != 0)
            return;
        move_grab_dest(x, tw, tw.t.preliminary_grab_dest, x.scratch);
    }

    float release_score(tentacle_work const& tw) {
        float num = float_max;
        for (size_t i = tw.c.size() / 2; i < tw.c.size(); ++i)
            if (custom::dist_less(tw.c[i].pos, tw.t.ideal_grab_pos, num))
                num = rw::distance(tw.c[i].pos, tw.t.ideal_grab_pos);
        return num;
    }

    // DaddyTentacle.Update
    void tentacle_update(ctx& x, tentacle_work& tw) {
        auto& w = x.state.world;
        auto& t = tw.t;
        // the part's stun and health
        int32_t& stun = w.creature_part_get_stun(tw.part);
        base_update(x, tw);
        auto const gp = player_of(x.state, t.grab_player);
        if (t.grab_player != 0 && (!gp || !player_here(x.state, gp, x.room.id))) {
            stun = 10;
            t.grab_player = 0;
        }
        if (x.dead()) {
            t.needed_for_locomotion = 1;
            t.grab_player = 0;
            t.limp = 1;
        }
        if (stun > 0) {
            --stun;
            t.grab_player = 0;
        }
        float const health = w.creature_part_get_health(tw.part);
        if (mathf::pow(x.rng.value(), 0.35f) > health) {
            float const r = x.rng.value();
            stun = std::max(stun, int32_t(mathf::lerp(-4.f, 14.f, mathf::pow(r, 0.5f + 20.f * mathf::max(0.f, health)))));
        }
        if (t.grab_player != 0) {
            auto const pc = player_chunk(x, t.grab_player, t.grab_chunk);
            auto& tp = tip(tw);
            vec2& cp = w.body_chunk_get_pos(pc);
            float const num = rw::distance(tp.pos, cp);
            float const num2 = (tp.rad + w.body_chunk_get_rad(pc)) / 4.f;
            vec2 const a = custom::dir_vec(tp.pos, cp);
            float const m = w.body_chunk_get_mass(pc);
            float const num3 = m / (m + 0.01f);
            tp.pos += a * (num - num2) * num3;
            tp.vel += a * (num - num2) * num3;
            cp -= a * (num - num2) * (1.f - num3);
            w.body_chunk_get_vel(pc) -= a * (num - num2) * (1.f - num3);
            // (Player.GraspWiggle is a Remix option: a held slugcat can't wiggle free)
        }
        t.limp = (!x.consious() || stun > 0) ? 1 : 0;
        for (auto& c : tw.c) {
            c.vel *= 0.9f;
            if (t.limp)
                c.vel.y -= 0.5f;
            if (stun > 0 && !x.dead())
                c.vel += custom::rnv(x.rng) * 10.f;
        }
        if (t.limp) {
            for (auto& c : tw.c)
                c.vel.y -= 0.7f;
            return;
        }
        t.at_grab_dest = 0;
        if (t.backtrack_from > -1) {
            ++t.secondary_grab_back_track_counter;
            if (!t.last_back_track)
                t.secondary_grab_back_track_counter += 20;
        }
        t.last_back_track = t.backtrack_from > -1 ? 1 : 0;
        vec2 v = x.pos(0);
        for (uint32_t k = 1; k < x.n; ++k)
            v += x.pos(k);
        v /= float(x.n);
        t.away_from_body_rotation = custom::aim(v, float_base(x, tw));
        t.chunks_gripping = 0.f;
        if (!t.needed_for_locomotion) {
            if (t.current != task::grabbing) {
                look_for_creatures_to_hunt(x, tw);
                if (t.hunt_player == 0 && t.check_sound == 0)
                    look_for_sounds_to_examine(x, tw);
            }
        } else if (t.current != task::locomotion) {
            switch_task(x, tw, task::locomotion);
        }
        if (t.current == task::hunt && (t.hunt_player == 0 || x.seen_player != t.hunt_player))
            switch_task(x, tw, task::locomotion);
        else if (t.current != task::hunt && t.hunt_player != 0)
            t.hunt_player = 0;
        if (t.current == task::examine_sound) {
            auto const* s = source(x.a, t.check_sound);
            if (!s || s->slated_for_deletion)
                switch_task(x, tw, task::locomotion);
        } else if (t.check_sound != 0) {
            t.check_sound = 0;
        }
        if (t.current == task::grabbing && (t.grab_player == 0 || !player_here(x.state, player_of(x.state, t.grab_player), x.room.id)))
            switch_task(x, tw, task::locomotion);
        else if (t.current != task::grabbing && t.grab_player != 0)
            release_grab(x, tw);

        auto& path = x.scratch;
        if (t.current == task::locomotion) {
            climb(x, tw, path);
        } else if (t.current == task::hunt) {
            hunt(x, tw, path);
        } else if (t.current == task::examine_sound) {
            examine_sound(x, tw, path);
        } else if (t.current == task::grabbing) {
            vec2 const gpos = w.body_chunk_get_pos(player_chunk(x, t.grab_player, t.grab_chunk));
            move_grab_dest(x, tw, v + custom::dir_vec(v, gpos) * 20.f, path);
            vec2 p = v;
            bool flag2 = x.room.visual_contact(gpos, v);
            for (int32_t l = int32_t(tw.c.size()) - 1; l >= 0; --l) {
                vec2 p2 = float_base(x, tw);
                if (l > 0) {
                    p2 = tw.c[size_t(l - 1)].pos;
                    if (!flag2 && !x.room.visual_contact(gpos, tw.c[size_t(l - 1)].pos)) {
                        p = tw.c[size_t(l)].pos;
                        flag2 = true;
                    }
                }
                auto& c = tw.c[size_t(l)];
                c.vel += custom::dir_vec(c.pos, p2) * 1.2f;
                if (c.phase > -1.f || x.room.get_tile(c.pos).solid()) {
                    release_grab(x, tw);
                    switch_task(x, tw, task::locomotion);
                    break;
                }
            }
            if (t.current == task::grabbing) {
                auto const pc = player_chunk(x, t.grab_player, t.grab_chunk);
                vec2 const tp2 = tw.c.size() >= 2 ? tw.c[tw.c.size() - 2].pos : float_base(x, tw);
                vec2 const pull = rw::slerp(custom::dir_vec(gpos, p), custom::dir_vec(tip(tw).pos, tp2), 0.5f)
                    * custom::lerp_map(float(tw.grab_path.size()), 3.f, 18.f, 0.65f, 0.25f) * (x.size_class ? 1.f : 0.45f);
                w.body_chunk_get_vel(pc) += pull / w.body_chunk_get_mass(pc);
            }
        }
        for (uint32_t m = 0; m < tw.c.size(); ++m) {
            float const num4 = tw.c.size() > 1 ? float(m) / float(tw.c.size() - 1) : 0.f;
            if (num4 < 0.2f)
                tw.c[m].vel += custom::deg_to_vec(t.away_from_body_rotation) * mathf::inverse_lerp(0.2f, 0.f, num4) * 5.f;
            for (uint32_t n2 = m + 1; n2 < tw.c.size(); ++n2)
                push_chunks_apart(tw, m, n2);
        }
        touch(x, tw);
    }
}
