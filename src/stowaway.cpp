// MoreSlugcats/StowawayBug.cs, StowawayBugAI.cs, StowawayBugState.cs: the
// stowaway. See stowaway.hpp.

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <span>
#include <string>
#include <vector>

#include "stowaway.hpp"
#include "daddy_internal.hpp"
#include "dens.hpp"
#include "items.hpp"
#include "particles.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "rain.hpp"
#include "rooms.hpp"
#include "rot.hpp"
#include "sound_types.hpp"
#include "tracker.hpp"

namespace stowaway {
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // CreatureTemplate (StaticWorld.InitStowaway): baseDamageResistance,
        // baseStunResistance (instantDeathDamageLimit: the default, none)
        constexpr float base_damage_resistance = 0.8f;
        constexpr float base_stun_resistance = 2.f;
        // StowawayBug.Update's `num`: how close to the maw a held creature
        // gets bitten to death
        constexpr float bite_reach = 30.f;
        // WaterDrip's colour (particles.cpp's water)
        constexpr rw::vec3 drip_color{0.62f, 0.72f, 0.85f};

        // Tentacle.TentacleProps(true, true, true, 0.5f, 0f, 0.5f, 0.05f,
        // 0.05f, 2.2f, 42f, 0.33333334f, 5f, 15, 60, 12, 20): a stiff rope
        // tentacle (always limp: goal attraction and alignment are never
        // used, nor is the grab path)
        constexpr float mass_deterioration_per_chunk = 0.5f;
        constexpr float backtrack_speed = 2.2f;
        constexpr float chunk_velocity_cap = 42.f;
        constexpr float tile_tentacle_update_speed = 0.33333334f;
        constexpr int32_t segment_phase_through_terrain_frames = 15;
        constexpr int32_t max_pull_tiles_per_tick = 12;
        constexpr int32_t terrain_hits_before_phase = 20;

        constexpr float float_max = std::numeric_limits<float>::max();
        constexpr ivec2 four_directions_and_zero[5] = {{0, 0}, {-1, 0}, {0, -1}, {1, 0}, {0, 1}};

        creature::stowaway_payload& pl(sys::game_state& state, dcon::creature_id c) { return state.world.creature_get_payload(c).stowaway; }
        creature::stowaway_payload const& pl(sys::game_state const& state, dcon::creature_id c) { return state.world.creature_get_payload(c).stowaway; }
        dcon::stowaway_data_id data(sys::game_state const& state, dcon::creature_id c) { return creature::stowaway_data(state.world, c); }
        ivec2 tile_of(vec2 p) { return room_view::get_tile_position(p); }

        bool has_any_solid(room_view const& room, ivec2 t) { return room.get_tile(t).solid(); }
        // Custom.AreIntVectorsNeighbors / AreIntVectorsDiagonalNeighbors (sic)
        bool are_neighbors(ivec2 a, ivec2 b) { return a != b && (a.x == b.x || a.y == b.y) && std::abs(a.x - b.x) < 2 && std::abs(a.y - b.y) < 2; }
        bool are_diagonal_neighbors(ivec2 a, ivec2 b) { return a.x != b.x && a.y != b.y && (std::abs(a.x - b.x) < 2 || std::abs(a.y - b.y) < 2); }

        //
        // SharedPhysics.TerrainCollisionData: VerticalCollision,
        // SlopesVertically, HorizontalCollision (as pole_mimic.cpp's)
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
        // Tentacle: one head while it updates (its tile segments a working
        // copy, its chunks in place)
        //
        struct head_ctx {
            room_view const& room;
            rw::rng& rng;
            head_state& h;
            std::vector<ivec2>& segments;
            std::span<tentacle_chunk> ch;
            // FloatBase: the connected chunk (mainBodyChunk, bodyChunks[0])
            vec2 base;
            std::vector<ivec2>& path;
            // AImap.getTerrainProximity of the room (null: none asked for)
            std::vector<uint16_t> const* prox = nullptr;

            int32_t proximity(ivec2 t) const {
                if (!prox || !room.inside(t.x, t.y))
                    return -1;
                return (*prox)[size_t(t.y) * size_t(room.width) + size_t(t.x)];
            }
        };

        // CheckIfAnySegmentsLeft, BasePos
        ivec2 base_pos(head_ctx& x) {
            if (x.segments.empty())
                x.segments.push_back(room_view::get_tile_position(x.base));
            return x.segments[0];
        }
        // TentacleChunk.currentSegment / StuckPos
        int32_t current_segment(head_ctx& x, tentacle_chunk const& c) {
            base_pos(x);
            int32_t const n = int32_t(x.segments.size());
            return custom::int_clamp(int32_t(c.t_pos * float(n - 1)), 0, n - 1);
        }
        ivec2 stuck_pos(head_ctx& x, tentacle_chunk const& c) { return x.segments[size_t(current_segment(x, c))]; }

        // Tentacle.Visual (the terrain proximity map rules out the ray
        // traces that can't meet a solid tile, as daddy_tentacle.cpp's)
        bool visual(head_ctx& x, ivec2 a, ivec2 b) {
            int32_t const p = x.proximity(a);
            if (p > 0 && x.room.inside(b.x, b.y) && std::abs(b.x - a.x) + std::abs(b.y - a.y) < p)
                return true;
            return daddy::detail::ray_trace_tiles_for_terrain(x.room, a, b);
        }
        int32_t ray_trace_tiles_list(ivec2 a, ivec2 b, std::vector<ivec2>& path) { return daddy::detail::ray_trace_tiles_list(a.x, a.y, b.x, b.y, path); }

        // Rope.Update (a rope in the clear only takes its new ends)
        void update_rope(head_ctx& x, world::rope_state& r, vec2 na, vec2 nb) {
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
        bool rope_active(head_ctx const& x, uint32_t i) {
            auto const& c = x.ch[i];
            return c.phase == -1.f && (i == 0 || x.ch[i - 1].phase == -1.f) && (x.h.backtrack_from < int32_t(i) || x.h.backtrack_from == -1);
        }

        // TentacleChunk.Reset
        void chunk_reset(head_ctx& x, tentacle_chunk& c) {
            c.trail = stuck_pos(x, c);
            c.phase = -1.f;
            c.pos = room_view::middle_of_tile(stuck_pos(x, c));
            c.last_pos = c.pos;
            c.vel = vec2{0.f, 0.f};
            // new Rope(room, FloatBase, FloatBase + (1, 1), 1f)
            world::rope_state r;
            r.a = r.last_a = x.base;
            r.b = r.last_b = x.base + vec2{1.f, 1.f};
            r.total_length = rw::distance(r.a, r.b);
            r.thickness = 1.f;
            c.rope = r;
        }

        void phase_to_segment(tentacle_chunk& c) {
            c.phase_from = c.pos;
            c.phase = 0.f;
        }

        // TentacleChunk.Update (tProps.stiff: always held at its ideal
        // distance; pullAtConnectionChunk 0: the body isn't pulled)
        void chunk_update(head_ctx& x, uint32_t i) {
            auto& c = x.ch[i];
            auto& t = x.h;
            float const n = float(x.ch.size());
            c.last_pos = c.pos;
            update_rope(x, c.rope, i == 0 ? x.base : x.ch[i - 1].pos, c.pos);
            if (!rope_active(x, i) || c.phase > -1.f || (i > 0 && x.ch[i - 1].phase > -1.f) || c.rope.total_length > t.ideal_length / n * 5.f)
                c.rope.bend_count = 0;
            float const ideal = t.ideal_length / n * mathf::lerp(1.f, 0.1f, t.retract_fac);
            if (c.phase == -1.f) {
                if (c.phases_to_same_location > 0 && stuck_pos(x, c) == c.after_phase_stuck_pos) {
                    vec2 const m = room_view::middle_of_tile(stuck_pos(x, c));
                    c.pos = vec2{mathf::clamp(c.pos.x, m.x - 9.f, m.x + 9.f), mathf::clamp(c.pos.y, m.y - 9.f, m.y + 9.f)};
                }
                if (i == 0) {
                    vec2 a = custom::dir_vec(c.pos, x.base);
                    float num = rw::distance(c.pos, x.base);
                    if (rope_active(x, i)) {
                        a = custom::dir_vec(c.pos, rot::rope_b_connect(c.rope));
                        num = c.rope.total_length;
                    }
                    c.pos -= a * (ideal - num);
                    c.vel -= a * (ideal - num);
                    c.stretched_fac = t.ideal_length / n / mathf::max(1.f, num);
                } else {
                    auto& prev = x.ch[i - 1];
                    vec2 a = custom::dir_vec(c.pos, prev.pos);
                    float num = rw::distance(c.pos, prev.pos);
                    if (rope_active(x, i)) {
                        a = custom::dir_vec(c.pos, rot::rope_b_connect(c.rope));
                        num = c.rope.total_length;
                    }
                    c.pos -= a * (ideal - num) * (1.f - mass_deterioration_per_chunk);
                    c.vel -= a * (ideal - num) * (1.f - mass_deterioration_per_chunk);
                    if (rope_active(x, i))
                        a = custom::dir_vec(rot::rope_a_connect(c.rope), prev.pos);
                    prev.pos += a * (ideal - num) * mass_deterioration_per_chunk;
                    prev.vel += a * (ideal - num) * mass_deterioration_per_chunk;
                    c.stretched_fac = t.ideal_length / n / mathf::max(1.f, num);
                }
                if (stuck_pos(x, c) != c.trail)
                    c.trail = stuck_pos(x, c);
                ivec2 const here = room_view::get_tile_position(c.pos);
                int32_t const cs = current_segment(x, c);
                if (t.backtrack_from == -1 && !visual(x, here, x.segments[size_t(cs)]))
                    t.backtrack_from = int32_t(i);
                if (t.backtrack_from != -1 && int32_t(i) >= t.backtrack_from) {
                    bool flag = false;
                    bool prev_seen = cs >= 1 && visual(x, here, x.segments[0]);
                    for (int32_t k = 1; k <= cs && !flag; ++k) {
                        bool const seen = visual(x, here, x.segments[size_t(k)]);
                        bool const was = prev_seen;
                        prev_seen = seen;
                        if (!seen && was) {
                            c.vel += rw::clamp_magnitude(room_view::middle_of_tile(x.segments[size_t(k - 1)]) - c.pos, 20.f) * backtrack_speed / 20.f;
                            flag = true;
                        }
                    }
                    if (!flag) {
                        vec2 const fb = i > 0 ? x.ch[i - 1].pos : x.base;
                        c.vel += rw::clamp_magnitude(fb - c.pos, 20.f) * backtrack_speed / 20.f;
                        if (i > 0)
                            x.ch[i - 1].vel += rw::clamp_magnitude(fb - c.pos, 20.f) * backtrack_speed / 20.f;
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
                    // (always limp: drawn gently towards the closest
                    // segment in sight)
                    for (int32_t k = cs; k >= 0; --k) {
                        ivec2 const s = x.segments[size_t(k)];
                        if (visual(x, here, s)) {
                            c.vel += rw::clamp_magnitude(room_view::middle_of_tile(s) - c.pos, 20.f) * 0.005f;
                            break;
                        }
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
                if (i > 0 && x.ch[i - 1].phase == -1.f) {
                    auto& prev = x.ch[i - 1];
                    vec2 const a = custom::dir_vec(c.pos, prev.pos);
                    float const num = rw::distance(c.pos, prev.pos);
                    prev.pos += a * (ideal - num) * mass_deterioration_per_chunk;
                    prev.vel += a * (ideal - num) * mass_deterioration_per_chunk;
                    c.stretched_fac = num > 0.f ? ideal / num : 1.f;
                }
            }
        }

        // Tentacle.MoveSegment
        bool move_segment(head_ctx& x, int32_t s, ivec2 dest, bool allow_occupied, bool allow_solid) {
            auto& seg = x.segments;
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
        void shorten(head_ctx& x, int32_t from, int32_t to, std::vector<ivec2> const& straight, int32_t count) {
            x.segments.erase(x.segments.begin() + from + 1, x.segments.begin() + to);
            for (int32_t i = count - 2; i > 0; --i)
                x.segments.insert(x.segments.begin() + from + 1, straight[size_t(i)]);
        }

        // Tentacle.PullAtTentacle
        void pull_at_tentacle(head_ctx& x, int32_t start, bool only_simple_pull, std::vector<ivec2>& path) {
            auto& t = x.h;
            auto& seg = x.segments;
            if (!only_simple_pull) {
                while (t.pull_counter > 0) {
                    bool flag = false;
                    for (int32_t num = start; num < int32_t(seg.size()) - 3 && !flag; ++num) {
                        for (int32_t i = int32_t(seg.size()) - 1; i >= num + 3; --i) {
                            int32_t const num2 = i - num - custom::manhattan_distance(seg[size_t(num)], seg[size_t(i)]);
                            if (num2 > 0 && num2 <= t.pull_counter && visual(x, seg[size_t(num)], seg[size_t(i)])) {
                                int32_t const num3 = ray_trace_tiles_list(seg[size_t(num)], seg[size_t(i)], path);
                                bool flag2 = true;
                                if (num2 > 1 && num2 <= t.pulls_this_tick) {
                                    for (int32_t j = 1; j < num3 - 1; ++j)
                                        for (int32_t k = num + 1; k < i; ++k)
                                            if (custom::manhattan_distance(seg[size_t(k)], path[size_t(j)]) > 2 && !visual(x, seg[size_t(k)], path[size_t(j)])) {
                                                flag2 = false;
                                                break;
                                            }
                                }
                                if (flag2) {
                                    t.pulls_this_tick -= num2;
                                    t.pull_counter -= num2;
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
            while (t.pull_counter > 0 && t.pulls_this_tick > 0 && seg.size() > 1) {
                seg.pop_back();
                --t.pull_counter;
                --t.pulls_this_tick;
            }
        }

        // Tentacle.FindTentaclePath: a greedy best first search from the
        // goal back to the start over every tile (solid ones at 1000 extra),
        // the open list a heap on (heuristic, insertion order)
        int32_t find_tentacle_path(head_ctx& x, ivec2 start, ivec2 goal, std::vector<ivec2>& path) {
            struct cell {
                ivec2 pos;
                int32_t parent;
            };
            int32_t const w = x.room.width, h = x.room.height;
            std::vector<cell> cells;
            std::vector<uint8_t> visited(size_t(w) * size_t(h), 0);
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

        // Tentacle.Grow
        void grow(head_ctx& x, int32_t add) {
            auto& seg = x.segments;
            base_pos(x);
            for (int32_t i = 0; i < add; ++i) {
                float num = float_max;
                ivec2 item = seg.back();
                for (auto const dd : four_directions_and_zero) {
                    float num2 = x.rng.value();
                    ivec2 const c = seg.back() + dd;
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

        // Tentacle.MoveBase (no grab destination: no grab path to find)
        void move_base(head_ctx& x, ivec2 to, std::vector<ivec2>& path) {
            auto& seg = x.segments;
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
                    ++x.h.pull_counter;
                    if (x.h.pull_counter > 4)
                        pull_at_tentacle(x, 0, false, path);
                }
            } else {
                int32_t num2;
                if (!x.room.inside(seg[0].x, seg[0].y) || !x.room.inside(to.x, to.y) || visual(x, seg[0], to))
                    num2 = ray_trace_tiles_list(seg[0], to, path);
                else
                    num2 = find_tentacle_path(x, seg[0], to, path);
                ivec2 b = seg[0];
                for (int32_t j = 1; j < num2; ++j) {
                    seg.insert(seg.begin(), seg[0] + (path[size_t(j)] - b));
                    b = path[size_t(j)];
                }
                x.h.pull_counter += num2 - 1;
                if (x.h.pull_counter > 4)
                    pull_at_tentacle(x, 0, false, path);
            }
        }

        // Tentacle.CurrentLength
        float current_length(head_ctx const& x) {
            float num = float(x.segments.size()) * 20.f;
            for (size_t i = 0; i + 2 < x.segments.size(); ++i)
                if (are_diagonal_neighbors(x.segments[i], x.segments[i + 2]))
                    num -= 5.9000006f;
            return num;
        }

        // Tentacle.AdjustLength (the grab path is always empty)
        void adjust_length(head_ctx& x, std::vector<ivec2>& path) {
            float const num = current_length(x);
            int32_t const num3 = int32_t(x.h.ideal_length / 20.f - num / 20.f);
            if (num3 > 0) {
                grow(x, 1);
                return;
            }
            if (num3 < 0) {
                ++x.h.pull_counter;
                pull_at_tentacle(x, 0, false, path);
            }
        }

        // Tentacle.Gravity with Tentacle.GravityDirection
        void gravity(head_ctx& x) {
            ivec2 b{0, -1};
            if (x.rng.value() < 0.5f)
                b = ivec2{x.rng.value() < 0.5f ? -1 : 1, -1};
            bool flag = false;
            for (int32_t i = 0; i < int32_t(x.segments.size()) && !flag; ++i)
                if (move_segment(x, i, x.segments[size_t(i)] + b, false, false))
                    flag = true;
            if (!flag)
                for (int32_t i = 0; i < int32_t(x.segments.size()); ++i)
                    if (move_segment(x, i, x.segments[size_t(i)] + b, true, false))
                        break;
        }

        // Tentacle.Update (limp: Gravity)
        void tentacle_update(head_ctx& x) {
            auto& path = x.path;
            move_base(x, room_view::get_tile_position(x.base), path);
            x.h.update_counter += tile_tentacle_update_speed;
            if (x.h.update_counter >= 1.f) {
                x.h.pulls_this_tick = max_pull_tiles_per_tick;
                if (x.h.limp)
                    gravity(x);
                adjust_length(x, path);
                x.h.update_counter -= 1.f;
            }
            x.h.backtrack_from = -1;
            for (uint32_t i = 0; i < x.ch.size(); ++i)
                chunk_update(x, i);
        }

        // Tentacle.NewRoom (the owner's firstChunk is the connected chunk)
        void tentacle_new_room(head_ctx& x) {
            x.segments.assign(size_t(std::max(0, int32_t(x.h.ideal_length / 20.f))), room_view::get_tile_position(x.base));
            for (auto& c : x.ch)
                chunk_reset(x, c);
        }

        //
        // The stowaway while it updates: its heads' segments as working
        // copies, its chunks and feelers in place
        //
        struct ctx {
            sys::game_state& state;
            dcon::creature_id c;
            room_view const& room;
            rw::rng rng;
            creature::stowaway_payload& p;
            dcon::stowaway_data_id d;
            body_state& b;
            std::vector<ivec2> segs[head_count];
            // the `ref List<IntVector2> path` scratch list
            std::vector<ivec2> path;
            std::span<tentacle_chunk> ch;
            std::span<feeler_point> fe;
            dcon::body_chunk_id k0, k1;
            std::vector<uint16_t> const* prox = nullptr;

            // (keyed by the constructing call site too: several ctxs of one
            // creature in one tick roll apart)
            ctx(sys::game_state& s, dcon::creature_id id, room_view const& r, std::source_location const l = std::source_location::current())
                : state(s), c(id), room(r), rng(rw::random(s.seed, s.tick, uint32_t(id.value), l)), p(pl(s, id)), d(data(s, id)), b(s.world.stowaway_data_get_body(d)) {
                auto& w = state.world;
                auto const all = w.stowaway_data_get_segments(d);
                uint32_t at = 0;
                for (uint32_t i = 0; i < head_count; ++i) {
                    uint32_t const n = b.heads[i].seg_count;
                    if (at + n <= all.size())
                        segs[i].assign(all.begin() + at, all.begin() + at + n);
                    at += n;
                }
                refresh_spans();
                k0 = creature::chunk(state, c, 0);
                k1 = creature::chunk(state, c, 1);
            }
            void refresh_spans() {
                auto& w = state.world;
                auto const cs = w.stowaway_data_get_chunks(d);
                ch = std::span<tentacle_chunk>{cs.begin(), cs.size()};
                auto const fs = w.stowaway_data_get_feelers(d);
                fe = std::span<feeler_point>{fs.begin(), fs.size()};
            }
            void store() {
                std::vector<ivec2> all;
                for (uint32_t i = 0; i < head_count; ++i) {
                    b.heads[i].seg_count = uint16_t(std::min<size_t>(segs[i].size(), 0xFFFF));
                    all.insert(all.end(), segs[i].begin(), segs[i].begin() + b.heads[i].seg_count);
                }
                state.world.stowaway_data_get_segments(d).load_range(all.data(), all.data() + all.size());
            }
            bool heads_ok() const { return ch.size() == head_count * head_chunks; }
            std::span<tentacle_chunk> head(uint32_t i) { return ch.subspan(size_t(i) * head_chunks, head_chunks); }
            tentacle_chunk& tip(uint32_t i) { return ch[size_t(i) * head_chunks + head_chunks - 1]; }
            vec2& pos(dcon::body_chunk_id k) { return state.world.body_chunk_get_pos(k); }
            vec2& vel(dcon::body_chunk_id k) { return state.world.body_chunk_get_vel(k); }
            head_ctx head_ctx_of(uint32_t i) { return head_ctx{room, rng, b.heads[i], segs[i], head(i), pos(k0), path, prox}; }
            behavior beh() const { return behavior(state.world.creature_get_behavior(c)); }
            void set_beh(behavior v) { state.world.creature_set_behavior(c, uint8_t(v)); }
        };

        //
        // Grasps: heads 0 and 1 are the creature's grasps, head 2's is kept
        // in the body state (counted in its body's grabbed_by_count)
        //
        struct held_ref {
            dcon::creature_id who;
            uint8_t chunk = 0;
        };
        held_ref grasp_of(sys::game_state const& state, dcon::creature_id c, body_state const& b, uint32_t i) {
            auto const& w = state.world;
            if (i < 2)
                return held_ref{creature::grasped(w, c, i), creature::grasp_chunk(w, c, i)};
            return held_ref{b.extra_grasp, b.extra_grasp_chunk};
        }
        void release_head(sys::game_state& state, dcon::creature_id c, body_state& b, uint32_t i) {
            auto& w = state.world;
            if (i < 2) {
                creature::release_grasp(state, c, i);
                return;
            }
            auto const t = b.extra_grasp;
            b.extra_grasp = dcon::creature_id{};
            b.extra_grasp_chunk = 0;
            if (t && w.creature_is_valid(t))
                if (auto const o = w.creature_get_body_from_creature_body(t)) {
                    uint8_t& n = w.physical_object_get_grabbed_by_count(o);
                    if (n > 0)
                        --n;
                }
        }
        void grab_head(sys::game_state& state, dcon::creature_id c, body_state& b, uint32_t i, dcon::creature_id t, uint32_t k) {
            auto& w = state.world;
            if (i < 2) {
                creature::grab(state, c, i, t, k);
                return;
            }
            release_head(state, c, b, 2);
            b.extra_grasp = t;
            b.extra_grasp_chunk = uint8_t(k);
            if (auto const o = creature::body_of(state, t))
                ++w.physical_object_get_grabbed_by_count(o);
        }
        // Creature.LoseAllGrasps
        void lose_all_grasps(sys::game_state& state, dcon::creature_id c, body_state& b) {
            for (uint32_t i = 0; i < head_count; ++i)
                release_head(state, c, b, i);
        }
        bool any_grasp(sys::game_state const& state, dcon::creature_id c, body_state const& b) {
            for (uint32_t i = 0; i < head_count; ++i)
                if (grasp_of(state, c, b, i).who)
                    return true;
            return false;
        }

        // a creature's chunk out in the room (none for one gone or out of reach)
        dcon::body_chunk_id target_chunk(sys::game_state const& state, dcon::room_id room, dcon::creature_id t, uint32_t k) {
            if (!t || !creature::here(state, t, room) || k >= creature::chunk_count_of(state, t))
                return dcon::body_chunk_id{};
            return creature::chunk_of(state, t, k);
        }

        bool eating(body_state const& b, dcon::creature_id t) {
            for (uint32_t i = 0; i < b.eat_count; ++i)
                if (b.eat[i].who == t)
                    return true;
            return false;
        }
        void remove_eat(body_state& b, uint32_t i) {
            for (uint32_t j = i; j + 1 < b.eat_count; ++j)
                b.eat[j] = b.eat[j + 1];
            if (b.eat_count > 0)
                --b.eat_count;
        }

        // StowawayBugAI.WantToEat
        bool want_to_eat(creature::tmpl t) {
            using creature::tmpl;
            switch (t) {
            case tmpl::deer:
            case tmpl::brother_long_legs:
            case tmpl::daddy_long_legs:
            case tmpl::terror_long_legs:
            case tmpl::garbage_worm:
            case tmpl::vulture:
            case tmpl::king_vulture:
            case tmpl::miros_vulture:
            case tmpl::miros_bird:
            case tmpl::pole_mimic:
            case tmpl::tentacle_plant:
            case tmpl::overseer:
            case tmpl::stowaway_bug:
                return false;
            default:
                return true;
            }
        }

        // the room's rain cycle timer (RainCycle.timer) and finished cycles
        int32_t cycle_timer(sys::game_state const& state, dcon::room_id room) {
            auto const* cy = rain::of_room(state, room);
            return cy ? cy->timer : 0;
        }
        uint32_t cycle_number(sys::game_state const& state, dcon::room_id room) {
            auto const* cy = rain::of_room(state, room);
            return cy ? cy->cycles : 0u;
        }

        // StowawayBugState.CurrentlyDigesting (sic: true once it is over)
        bool currently_digesting(ctx const& x) { return cycle_timer(x.state, x.room.id) > x.b.digestion_time_start + x.b.digestion_length; }
        // StowawayBugState.StartDigestion
        void start_digestion(ctx& x) {
            x.b.digestion_time_start = cycle_timer(x.state, x.room.id);
            x.b.digestion_length = x.rng.range(1000, 3000);
        }
        // StowawayBugState.AwakeThisCycle (ID.RandomSeed * 42 % 13, C#'s
        // wrapping int product and signed remainder)
        bool awake_this_cycle(ctx& x, uint32_t cycle) {
            int32_t const seed = x.state.world.stowaway_data_get_seed(x.d);
            int32_t const a = int32_t(uint32_t(seed) * 42u) % 13;
            if (a != int32_t(cycle % 32u))
                return false;
            x.b.digestion_time_start = 0;
            x.b.digestion_length = x.rng.range(100, 6000);
            return true;
        }
        // StowawayBugAI's constructor: asleep unless it wakes this cycle or
        // doesn't point down (rolled again each new cycle here: it stays in
        // its room through the cycles)
        void ai_wake_roll(ctx& x) {
            uint32_t const cycle = cycle_number(x.state, x.room.id);
            x.p.cycle_seen = cycle;
            x.p.active_this_cycle = 1;
            x.set_beh(behavior::hidden);
            if (!awake_this_cycle(x, cycle)) {
                x.p.active_this_cycle = 0;
                x.set_beh(behavior::sleeping);
            }
            if (x.b.placed_direction.y > -0.3f) {
                x.p.active_this_cycle = 1;
                x.set_beh(behavior::idle);
            }
        }

        //
        // StowawayBugAI: Tracker(10, 3, 450, .., 20), PreyTracker(1, 8, 0,
        // 2000), ThreatTracker(3), RainTracker; it sees nothing (VisualScore
        // 0): what its feelers or heads touch it notices (SeeCreature)
        //
        tracker::params const& tracker_params() {
            static tracker::params const p = [] {
                tracker::params q;
                q.see_around_corners = 10;
                q.frames_to_remember = 450;
                q.ghost_dismissal_range = 20;
                q.prey_max = 1;
                q.persistance_bias = 8.f;
                q.sure_to_get = 0.f;
                q.sure_to_lose = 2000.f;
                q.give_up_unreachable = -1;
                q.threat_max = 3;
                return q;
            }();
            return p;
        }
        bool hook_sees(void*, dcon::body_chunk_id) { return false; }
        bool hook_sees_point(void*, vec2) { return false; }
        tracker::hooks ai_hooks() {
            tracker::hooks h;
            h.sees = hook_sees;
            h.sees_point = hook_sees_point;
            h.module = tracker::module_without_aggression;
            return h;
        }
        // Tracker.SeeCreature
        void see(ctx& x, dcon::creature_id t) {
            if (t && t != x.c)
                tracker::see(x.state, x.c, x.room, t, ai_hooks());
        }

        // StowawayBugAI.Update (Saint's tongue, LickedByPlayer, isn't ported)
        void ai_update(ctx& x) {
            auto& w = x.state.world;
            x.p.focus = dcon::creature_id{};
            tracker::update(x.state, x.c, x.room, tile_of(x.pos(x.k0)), tracker_params(), ai_hooks());
            behavior b = x.beh();
            if (b == behavior::sleeping) {
                if (x.p.any_tentacle_pulled && x.rng.value() < 0.001f && !currently_digesting(x))
                    b = behavior::idle;
                if (x.p.active_this_cycle && !currently_digesting(x))
                    b = behavior::idle;
                x.set_beh(b);
                return;
            }
            auto const& ts = tracker::of(x.state, x.c);
            auto const* prey = tracker::prey(ts);
            if (b != behavior::hidden && b != behavior::escape_rain && prey)
                x.p.focus = prey->who;
            // UtilityComparer: threat (weight 1), prey (0.5), rain (0.8); the
            // first of the highest wins
            float const u[3] = {
                tracker::threat_utility(ts),
                tracker::prey_utility(x.state, x.c, x.room, tile_of(x.pos(x.k0)), tracker_params()) * 0.5f,
                dens::rain_utility(x.state, x.c, x.room.id) * 0.8f,
            };
            int32_t best = 0;
            for (int32_t i = 1; i < 3; ++i)
                if (u[i] > u[best])
                    best = i;
            float const utility = u[best];
            if (best == 0)
                b = behavior::hidden;
            else if (best == 2 && !prey)
                b = behavior::escape_rain;
            else if (x.p.hunt_delay > 0)
                b = behavior::idle;
            else if (best == 1) {
                if (!prey)
                    b = behavior::idle;
                else
                    b = behavior::attacking;
            }
            if (utility < 0.1f)
                b = behavior::idle;
            x.set_beh(b);
            w.creature_set_current_utility(x.c, utility);
        }

        // StowawayBug.Act
        void act(ctx& x) {
            auto& w = x.state.world;
            auto& p = x.p;
            auto& b = x.b;
            ai_update(x);
            b.last_direction = b.current_direction;
            vec2 pt = x.pos(x.k1);
            if (p.digest_prey > 0.f)
                x.set_beh(behavior::sleeping);
            behavior const bh = x.beh();
            if (bh == behavior::sleeping || bh == behavior::hidden || bh == behavior::escape_rain) {
                pt = x.pos(x.k0) + b.placed_direction * 20.f;
                p.sleep_scale = mathf::min(1.f, p.sleep_scale + 0.01f);
                if (bh == behavior::sleeping)
                    p.tentacles_withdrawn = 1.f;
                else
                    p.tentacles_withdrawn = mathf::min(1.f, p.tentacles_withdrawn + 0.09f);
                p.maw_open = 0;
            } else if (bh == behavior::idle && b.placed_direction.y <= 0.3f) {
                p.maw_open = 0;
                pt = x.pos(x.k0) + b.goal_direction * 20.f;
                b.goal_direction = rw::lerp(b.goal_direction, b.placed_direction, 0.01f);
                if (any_grasp(x.state, x.c, b))
                    p.maw_open = 1;
                if (p.biting > 0.f)
                    p.maw_open = 1;
                for (uint32_t i = 0; i < head_count; ++i)
                    if (b.heads[i].retract_fac < 0.95f)
                        p.maw_open = 1;
            } else if (bh == behavior::attacking || b.placed_direction.y > 0.3f) {
                p.maw_open = 1;
                auto const* prey = tracker::prey(tracker::of(x.state, x.c));
                if (prey && creature::here(x.state, prey->who, x.room.id)) {
                    bool flag = true;
                    for (uint32_t j = 0; j < head_count; ++j)
                        if (b.heads[j].fired || b.heads[j].cooldown > 0.f) {
                            flag = false;
                            break;
                        }
                    if (p.maw_open && (flag || x.rng.value() < 0.4f)) {
                        for (uint32_t k = 0; k < head_count; ++k) {
                            auto& h = b.heads[k];
                            if (!h.fired && p.spit_cooldown < 0 && !grasp_of(x.state, x.c, b, k).who) {
                                h.fired = 1;
                                h.retract_fac = 0.f;
                                h.cooldown = float(x.rng.range(20, 40));
                                p.spit_cooldown = x.rng.range(40, 60);
                                // aimed where the prey (DangerPos: its main
                                // chunk) will be, by its first chunk's speed
                                vec2 const danger = creature::main_pos(x.state, prey->who);
                                vec2 const pv = w.body_chunk_get_vel(creature::chunk_of(x.state, prey->who, 0));
                                float const lead = mathf::lerp(10.f, 45.f, x.rng.value());
                                x.tip(k).vel = custom::dir_vec(x.tip(k).pos, danger + pv * lead) * 45.f;
                                // (Big_Spider_Spit: no sample id)
                                x.state.play_sound(sound::sound_id::red_lizard_spit_hit_npc, x.room.id, x.pos(x.k0));
                                break;
                            }
                        }
                    }
                }
                if (p.focus) {
                    vec2 aim = x.pos(x.k1);
                    if (auto const* r = tracker::find(tracker::of(x.state, x.c), p.focus))
                        aim = room_view::middle_of_tile(tracker::best_guess(*r));
                    pt = x.pos(x.k0) + custom::dir_vec(x.pos(x.k0), aim) * 20.f;
                    b.goal_direction = custom::dir_vec(x.pos(x.k0), pt);
                } else {
                    pt = x.pos(x.k0) + b.goal_direction * 20.f;
                }
            }
            if (bh != behavior::sleeping && bh != behavior::hidden && bh != behavior::escape_rain) {
                p.tentacles_withdrawn = mathf::max(0.f, p.tentacles_withdrawn - 0.11f);
                p.sleep_scale = mathf::max(0.f, p.sleep_scale - 0.07f);
            }
            if (p.any_tentacle_pulled)
                p.tentacles_withdrawn = mathf::min(1.f, p.tentacles_withdrawn + 0.009f);
            x.vel(x.k1) += custom::dir_vec(x.pos(x.k1), pt);
            b.current_direction = custom::dir_vec(x.pos(x.k0), x.pos(x.k1));
            if (rw::distance(b.current_direction, b.goal_direction) < 0.01f) {
                vec2 const v = b.placed_direction * 10.f + custom::rnv(x.rng) * 8.f;
                b.goal_direction = custom::dir_vec(x.pos(x.k0), x.pos(x.k0) + v);
            }
        }

        // a WaterDrip (cosmetic: only where a camera looks; the rolls are
        // made either way)
        void drip(ctx& x, vec2 at, vec2 v) { particles::spawn(x.state, x.room.id, particles::kind::drop, at, v, 1.f, 2.f, drip_color, 0.85f); }

        // what it swallowed is gone (a slugcat eaten)
        void swallow(ctx& x, dcon::creature_id t) {
            auto& w = x.state.world;
            creature::release_target(x.state, t, x.room.id);
            if (auto const pp = creature::player_of(x.state, t)) {
                items::release_all(x.state, pp);
                creature::kill(x.state, t, creature::tmpl::stowaway_bug);
                w.player_set_eaten(pp, 1);
                return;
            }
            creature::kill(x.state, t, creature::tmpl::stowaway_bug);
            x.state.defer(x.room.id, [t](sys::game_state& st) {
                if (st.world.creature_is_valid(t))
                    creature::destroy(st, t);
            });
        }

        // StowawayBug.Eat: what it killed pulled into its mouth, dying half
        // way (its chunk connections turning Pull and its collision layer
        // aren't ported: it doesn't collide with the stowaway, collides)
        void eat(ctx& x) {
            auto& w = x.state.world;
            vec2 const pos = x.pos(x.k0);
            int32_t i = int32_t(x.b.eat_count) - 1;
            while (i >= 0) {
                if (i >= int32_t(x.b.eat_count)) {
                    i = int32_t(x.b.eat_count) - 1;
                    continue;
                }
                eat_object& e = x.b.eat[i];
                dcon::creature_id const t = e.who;
                auto const gk = target_chunk(x.state, x.room.id, t, e.chunk);
                if (!gk) {
                    remove_eat(x.b, uint32_t(i));
                    --i;
                    continue;
                }
                if (e.progression > 1.f) {
                    tracker::forget(x.state, x.c, t);
                    remove_eat(x.b, uint32_t(i));
                    swallow(x, t);
                    --i;
                    continue;
                }
                float const prog = e.progression;
                e.progression += 0.0125f;
                if (prog <= 0.5f && e.progression > 0.5f)
                    creature::kill(x.state, t, creature::tmpl::stowaway_bug);
                float const d = e.distance * (1.f - e.progression);
                {
                    vec2& gp = w.body_chunk_get_pos(gk);
                    w.body_chunk_set_vel(gk, vec2{0.f, 0.f});
                    gp = pos + custom::dir_vec(pos, gp) * d;
                }
                uint32_t const n = creature::chunk_count_of(x.state, t);
                for (uint32_t k = 0; k < n; ++k) {
                    auto const ck = creature::chunk_of(x.state, t, k);
                    vec2& cp = w.body_chunk_get_pos(ck);
                    w.body_chunk_get_vel(ck) *= 1.f - e.progression;
                    cp = rw::lerp(cp, pos + custom::dir_vec(pos, cp) * d, e.progression);
                }
                --i;
            }
        }

        // StowawayBug.Carry: the held chunk and the head's tip together, the
        // chunk pulled up to the body as the head retracts
        void carry(ctx& x, uint32_t head, dcon::creature_id t, dcon::body_chunk_id g) {
            auto& w = x.state.world;
            auto& tp = x.tip(head);
            vec2& gp = w.body_chunk_get_pos(g);
            vec2& gv = w.body_chunk_get_vel(g);
            vec2 const a = custom::dir_vec(gp, tp.pos);
            float const num = rw::distance(gp, tp.pos);
            float const m = w.body_chunk_get_mass(g);
            float const num2 = m / (m + 0.1f);
            gp += a * num * (1.f - num2);
            gv += a * num * (1.f - num2);
            tp.pos -= a * num * num2;
            tp.vel -= a * num * num2;
            gv.x *= 0.95f;
            gv += custom::dir_vec(gp, x.pos(x.k0)) * (x.b.heads[head].retract_fac * (creature::mass_of(x.state, t) * 4.f));
        }

        // a head's tip touching creatures: stung, the edible held
        void head_sting(ctx& x, uint32_t k) {
            vec2 const tp = x.tip(k).pos;
            auto& w = x.state.world;
            rooms::for_each_creature(x.state, x.room.id, [&](dcon::creature_id t) {
                if (grasp_of(x.state, x.c, x.b, k).who || t == x.c || !creature::here(x.state, t, x.room.id) || eating(x.b, t))
                    return;
                uint32_t const n = creature::chunk_count_of(x.state, t);
                for (uint32_t j = 0; j < n && !grasp_of(x.state, x.c, x.b, k).who; ++j) {
                    auto const ck = creature::chunk_of(x.state, t, j);
                    if (!custom::dist_less(w.body_chunk_get_pos(ck), tp, w.body_chunk_get_rad(ck) + 16.f))
                        continue;
                    // (Dart_Maggot_Stick_In_Creature: no sample id; the
                    // CreatureSpasmer is cosmetic)
                    x.b.heads[k].cooldown = 0.f;
                    creature::violence_on(x.state, t, j, vec2{0.f, 0.f}, creature::is_slugcat(w, t) ? 0.f : 0.2f, 90.f, creature::damage_type::bite,
                        creature::tmpl::stowaway_bug);
                    if (want_to_eat(creature::template_of(x.state, t)) && creature::here(x.state, t, x.room.id))
                        grab_head(x.state, x.c, x.b, k, t, j);
                }
            });
        }

        // the heads: stinging, holding, retracting, their Tentacles
        void update_heads(ctx& x, bool& flag, bool& flag2) {
            auto& w = x.state.world;
            bool const awake = creature::consious(x.state, x.c);
            bool const dead = w.creature_get_dead(x.c) != 0;
            float const g = w.physical_object_get_g(creature::body(x.state, x.c));
            for (uint32_t k = 0; k < head_count; ++k) {
                auto& h = x.b.heads[k];
                if (h.retract_fac != 1.f) {
                    h.limp = 1;
                    if (h.cooldown > 0.f && h.retract_fac < 0.3f && awake)
                        head_sting(x, k);
                }
                held_ref const gr = grasp_of(x.state, x.c, x.b, k);
                if (gr.who) {
                    auto const gk = target_chunk(x.state, x.room.id, gr.who, gr.chunk);
                    // (Spear_Dislodged_From_Creature: no sample id)
                    if (!gk) {
                        release_head(x.state, x.c, x.b, k);
                    } else if (dead) {
                        h.retract_fac = 0.f;
                        release_head(x.state, x.c, x.b, k);
                    } else if (w.creature_get_entering_shortcut(gr.who).x >= 0) {
                        h.retract_fac = 0.f;
                        release_head(x.state, x.c, x.b, k);
                    } else if (rw::distance(x.tip(k).pos, x.pos(x.k0)) > x.b.head_length * 6.f) {
                        h.retract_fac = 0.f;
                        release_head(x.state, x.c, x.b, k);
                    } else if (rw::distance(x.tip(k).pos, x.pos(x.k0)) > 96.f && h.retract_fac >= 0.95f) {
                        h.retract_fac = 0.f;
                        release_head(x.state, x.c, x.b, k);
                    } else {
                        see(x, gr.who);
                        x.p.any_tentacle_pulled = 1;
                        carry(x, k, gr.who, gk);
                        flag = true;
                        if (rw::distance(x.tip(k).pos, x.pos(x.k1)) < bite_reach)
                            flag2 = true;
                        // (Player.GraspWiggle, a Remix option, is off: a
                        // slugcat doesn't wiggle free)
                    }
                }
                auto hc = x.head(k);
                for (auto& tc : hc) {
                    tc.vel.y -= g * (1.f - h.retract_fac);
                    tc.vel *= mathf::lerp(1.f, 0.95f, h.retract_fac);
                }
                if (h.retract_fac >= 0.98f) {
                    vec2 const dir = custom::dir_vec(x.pos(x.k1), x.pos(x.k0));
                    for (uint32_t m = head_chunks - 1; m > 1; --m)
                        hc[m - 1].pos = hc[m].pos + dir * 3.f;
                }
                h.ideal_length = x.b.head_length * (1.f - h.retract_fac);
                head_ctx hx = x.head_ctx_of(k);
                tentacle_update(hx);
            }
        }

        // the bites: a held creature at the maw bitten to death and eaten
        // (StowawayBugGraphics.Bite / KillerBite, here the simulation's)
        void update_bite(ctx& x, bool flag, bool flag2) {
            auto& p = x.p;
            if (!((x.rng.value() < 0.02f || flag2) && flag))
                return;
            if (p.biting != 0.f || !p.maw_open)
                return;
            p.biting = 0.1f;
            if (flag2)
                p.bite_is_killer = 1;
            x.state.play_sound(sound::sound_id::lizard_jaws_shut_miss_creature, x.room.id, x.pos(x.k0));
            for (int32_t n = x.rng.range(1, 5); n > 0; --n)
                drip(x, x.pos(x.k1), custom::dir_vec(x.pos(x.k0), x.pos(x.k1)) * 10.f + custom::rnv(x.rng));
            if (!flag2)
                return;
            for (uint32_t k = 0; k < head_count; ++k) {
                held_ref const gr = grasp_of(x.state, x.c, x.b, k);
                auto const gk = target_chunk(x.state, x.room.id, gr.who, gr.chunk);
                if (!gk || rw::distance(x.tip(k).pos, x.pos(x.k1)) >= bite_reach)
                    continue;
                creature::violence_on(x.state, gr.who, gr.chunk, vec2{0.f, 0.f}, 4.f, 200.f, creature::damage_type::bite, creature::tmpl::stowaway_bug);
                if (creature::dead(x.state, gr.who)) {
                    p.digest_prey += 0.01f;
                    for (int32_t n = x.rng.range(4, 8); n > 0; --n)
                        drip(x, x.pos(x.k1), custom::rnv(x.rng));
                    if (x.b.eat_count < max_eat)
                        x.b.eat[x.b.eat_count++] = eat_object{gr.who, gr.chunk, rw::distance(x.pos(x.k0), x.state.world.body_chunk_get_pos(gk)), 0.f};
                    lose_all_grasps(x.state, x.c, x.b);
                    start_digestion(x);
                    x.state.play_sound(sound::sound_id::bro_digestion_init, x.room.id, x.pos(x.k0));
                } else {
                    x.b.heads[k].retract_fac = 0.f;
                }
                x.state.play_sound(sound::sound_id::lizard_jaws_grab_player, x.room.id, x.pos(x.k0));
            }
        }

        // the feelers: strings of points hanging from the maw, stretched
        // while it is awake; a creature at a tip is noticed
        void update_feelers(ctx& x) {
            auto& w = x.state.world;
            float const num6 = mathf::lerp(10.f, 1.f, x.p.tentacles_withdrawn);
            vec2 const attach = attach_pos(x.state, x.c, 1.f);
            x.p.any_tentacle_pulled = 0;
            uint32_t at = 0;
            uint32_t const count = x.b.feeler_count;
            for (uint32_t f = 0; f < count; ++f) {
                uint32_t const n = x.b.feeler_len[f];
                if (at + n > x.fe.size())
                    break;
                auto pts = x.fe.subspan(at, n);
                at += n;
                for (uint32_t j = 0; j < n; ++j) {
                    float const t = float(j) / float(std::max(1u, n - 1));
                    auto& q = pts[j];
                    q.last_pos = q.pos;
                    q.pos += q.vel;
                    if (x.room.point_submerged(q.pos)) {
                        q.vel *= custom::lerp_map(rw::magnitude(q.vel), 1.f, 10.f, 1.f, 0.5f, mathf::lerp(1.4f, 0.4f, t));
                        q.vel += custom::rnv(x.rng) * 0.2f;
                    } else {
                        q.vel *= 0.999f;
                        q.vel.y -= x.room.gravity * 0.6f;
                        terrain_collision cd{q.pos, q.last_pos, q.vel, 1.f};
                        horizontal_collision(x.room, cd);
                        vertical_collision(x.room, cd);
                        slopes_vertically(x.room, cd);
                        q.pos = cd.pos;
                        q.vel = cd.vel;
                    }
                }
                for (uint32_t j = 0; j < n; ++j) {
                    if (j > 0) {
                        vec2 nr = rw::normalized(pts[j].pos - pts[j - 1].pos);
                        float const num10 = rw::distance(pts[j].pos, pts[j - 1].pos);
                        pts[j].pos += nr * (num6 - num10) * 0.5f;
                        pts[j].vel += nr * (num6 - num10) * 0.5f;
                        pts[j - 1].pos -= nr * (num6 - num10) * 0.5f;
                        pts[j - 1].vel -= nr * (num6 - num10) * 0.5f;
                        if (j > 1) {
                            nr = rw::normalized(pts[j].pos - pts[j - 2].pos);
                            pts[j].vel += nr * 0.2f;
                            pts[j - 2].vel -= nr * 0.2f;
                        }
                    } else {
                        pts[j].pos = attach;
                        pts[j].vel = vec2{0.f, 0.f};
                    }
                }
                if (n > 0 && x.rng.value() < 1.f / float(count)) {
                    vec2 const tip = pts[n - 1].pos;
                    // (not itself: Tracker.SeeCreature doesn't track its own)
                    rooms::for_each_creature(x.state, x.room.id, [&](dcon::creature_id t) {
                        if (t == x.c || !creature::here(x.state, t, x.room.id))
                            return;
                        uint32_t const m = creature::chunk_count_of(x.state, t);
                        for (uint32_t k = 0; k < m; ++k) {
                            auto const ck = creature::chunk_of(x.state, t, k);
                            if (custom::dist_less(w.body_chunk_get_pos(ck), tip, w.body_chunk_get_rad(ck) + 10.f)) {
                                see(x, t);
                                x.p.any_tentacle_pulled = 1;
                            }
                        }
                    });
                }
            }
        }

        // the part of StowawayBugGraphics.Update the simulation reads
        // (DrawSprites and the AI depend on it; vanilla runs it only with a
        // graphics module)
        void module_update(ctx& x) {
            auto& p = x.p;
            if (p.digest_prey > 0.f)
                p.digest_prey += 0.001f;
            if (p.digest_prey > 1.f)
                p.digest_prey = 0.f;
            if (!creature::consious(x.state, x.c))
                p.mouth_open = mathf::lerp(p.mouth_open, 0.45f, 0.03f);
            else if (p.maw_open || p.digest_prey > 0.f)
                p.mouth_open += 0.05f;
            else
                p.mouth_open -= 0.01f;
            p.mouth_open = mathf::clamp(p.mouth_open, 0.f, 1.f);
            if (p.biting > 0.f)
                p.biting += 0.15f;
            if (p.biting > 2.f && p.digest_prey == 0.f) {
                p.biting = 0.f;
                p.bite_is_killer = 0;
            }
            if (p.digest_prey > 0.f)
                p.biting = 1.f;
        }

        // StowawayBug.bodySetup: how long the feelers and heads are (by the
        // open tiles below it), the teeth, the start direction
        void body_setup(ctx& x) {
            auto& w = x.state.world;
            auto& b = x.b;
            ivec2 const t0 = tile_of(b.home_pos);
            // RayTraceTilesList straight down to the bottom row
            int32_t num = 0;
            b.color_pick_pos = room_view::middle_of_tile(t0);
            for (int32_t y = t0.y; y >= 0; --y) {
                if (num <= 10)
                    b.color_pick_pos = room_view::middle_of_tile(t0.x, y);
                if (x.room.get_tile(t0.x, y).solid())
                    break;
                ++num;
            }
            if (num < 5)
                num = 5;
            else
                num += int32_t(float(num) / 1.75f);
            // SetStartDirection
            vec2 const dir = custom::dir_vec(b.home_pos, b.aim_pos);
            b.placed_direction = b.goal_direction = b.current_direction = b.last_direction = dir;
            rw::unity_random r{w.stowaway_data_get_seed(x.d)};
            if (dir.y > -0.3f)
                num = r.range(11, 21);
            if (dir.y > 0.25f)
                num = r.range(1, 10);
            int32_t const num2 = r.range(7, 12);
            x.p.hunt_delay = r.range(97, 126);
            b.feeler_count = uint8_t(std::min<int32_t>(num2, int32_t(max_feelers)));
            uint32_t total = 0;
            for (uint32_t i = 0; i < b.feeler_count; ++i) {
                b.feeler_len[i] = uint8_t(std::clamp(r.range(int32_t(float(num) / 2.f), num), 0, 255));
                total += b.feeler_len[i];
            }
            b.teeth_count = r.range(3, 7);
            b.teeth_seed = r.range(1, 99);
            b.head_length = mathf::clamp(float(num), 10.f, 25.f) * 15.f;
            for (auto& h : b.heads) {
                h = head_state{};
                h.retract_fac = 1.f;
                h.limp = 1;
                h.ideal_length = b.head_length;
            }
            auto chunks = w.stowaway_data_get_chunks(x.d);
            chunks.resize(head_count * head_chunks);
            for (uint32_t j = 0; j < head_count; ++j)
                for (uint32_t k = 0; k < head_chunks; ++k) {
                    tentacle_chunk tc;
                    tc.t_pos = float(k + 1) / float(head_chunks);
                    tc.rad = mathf::lerp(3.f, 5.f, float(k) / float(head_chunks - 1));
                    chunks.at(j * head_chunks + k) = tc;
                }
            auto feelers = w.stowaway_data_get_feelers(x.d);
            feelers.resize(total);
            x.refresh_spans();
        }

        // StowawayBug.NewRoom: bodySetup, ResetTentacles, the heads' NewRoom,
        // the body pushed out along its direction
        void new_room(ctx& x) {
            body_setup(x);
            vec2 const fc = x.pos(x.k0);
            for (auto& q : x.fe) {
                q.pos = fc + custom::rnv(x.rng);
                q.last_pos = q.pos;
                q.vel = vec2{0.f, 0.f};
            }
            for (uint32_t i = 0; i < head_count; ++i) {
                x.segs[i].clear();
                head_ctx hx = x.head_ctx_of(i);
                tentacle_new_room(hx);
            }
            vec2 const p = x.pos(x.k0) + x.b.placed_direction * 8.f;
            physics::hard_set_position(x.state, x.k0, p);
            physics::hard_set_position(x.state, x.k1, p);
            x.state.world.body_chunk_set_vel(x.k0, vec2{0.f, 0.f});
            x.state.world.body_chunk_set_vel(x.k1, vec2{0.f, 0.f});
        }

        // its home and aim for a spot in the room: hanging from the ceiling
        // above, facing down (the Stowaway placed object's stand-in)
        void ceiling_spot(room_view const& room, vec2 at, vec2& home, vec2& aim) {
            ivec2 t = tile_of(at);
            if (room.get_tile(t).solid()) {
                ivec2 const t0 = t;
                int32_t best = 1 << 30;
                for (int32_t dy = -6; dy <= 6; ++dy)
                    for (int32_t dx = -6; dx <= 6; ++dx) {
                        ivec2 const q = t0 + ivec2{dx, dy};
                        if (room.inside(q.x, q.y) && !room.get_tile(q).solid() && dx * dx + dy * dy < best) {
                            best = dx * dx + dy * dy;
                            t = q;
                        }
                    }
            }
            for (int32_t k = 0; k < 40 && room.inside(t.x, t.y + 1) && !room.get_tile(t.x, t.y + 1).solid(); ++k)
                ++t.y;
            home = room_view::middle_of_tile(t) + vec2{0.f, 5.f};
            aim = home + vec2{0.f, -20.f};
        }

        // placed at `home` aiming at `aim`: the body there, set up, awake or
        // asleep for this cycle
        void place(sys::game_state& state, dcon::creature_id c, room_view const& room, vec2 home, vec2 aim) {
            auto& w = state.world;
            {
                auto& b = w.stowaway_data_get_body(data(state, c));
                b.home_pos = home;
                b.aim_pos = aim;
                b.original_pos = home;
            }
            for (uint32_t i = 0; i < 2; ++i) {
                auto const k = creature::chunk(state, c, i);
                physics::hard_set_position(state, k, home);
                w.body_chunk_set_vel(k, vec2{0.f, 0.f});
            }
            ctx x(state, c, room);
            x.prox = &pathing::ai_terrain_proximity_map(state, room.id);
            lose_all_grasps(state, c, x.b);
            x.b.eat_count = 0;
            new_room(x);
            if (!w.creature_get_dead(c))
                ai_wake_roll(x);
            x.store();
            tracker::clear(state, c);
        }

        // Creature.Update's HealthState: hurt, it may die or faint, else heals
        void health_update(sys::game_state& state, dcon::creature_id c) {
            auto& w = state.world;
            auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
            if (w.creature_get_dead(c))
                return;
            float& health = w.creature_get_health(c);
            if (health < 0.f && rng.value() < -health && rng.value() < 1.f / float(consious(state, c) ? 80 : 800))
                creature::die(state, c);
            if (rng.value() < 0.033333335f && (rng.value() * 0.2f > health || rng.value() < -health))
                stowaway::stun(state, c, rng.range(1, rng.range(1, 27 - custom::int_clamp(int32_t(20.f * health), 0, 10))));
            if (health > 0.f && health < 1.f && rng.value() < 0.01f)
                health = mathf::min(1.f, health + 1.f / mathf::lerp(550.f, 70.f, health));
        }
    }

    float rad(float f) {
        if (f < 0.5f)
            return custom::lerp_map(f, 0.f, 0.09f, 5.8f, 0.001f);
        return custom::lerp_map(f, 0.85f, 1.f, 0.001f, 0.01f);
    }

    vec2 attach_pos(sys::game_state const& state, dcon::creature_id c, float alpha) {
        auto const& w = state.world;
        auto const k0 = creature::chunk(state, c, 0), k1 = creature::chunk(state, c, 1);
        vec2 const v = custom::dir_vec(w.body_chunk_get_last_pos(k0), w.body_chunk_get_last_pos(k1));
        vec2 const v2 = custom::dir_vec(w.body_chunk_get_pos(k0), w.body_chunk_get_pos(k1));
        vec2 const s = rw::slerp(v, v2, alpha);
        return rw::lerp(w.body_chunk_get_last_pos(k1), w.body_chunk_get_pos(k1), alpha) + s * 17.f;
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::stowaway);
        auto const d = data(state, c);
        auto rng = rw::random(state.seed, 0, uint32_t(c.value));
        int32_t seed = int32_t(rng.next() & 0x7FFFFFFFu);
        if (state.spawn_seed >= 0)
            seed = state.spawn_seed;
        physics::object_desc desc;
        desc.gravity = 0.9f;
        desc.air_friction = 0.999f;
        desc.bounce = 0.1f;
        desc.surface_friction = 0.4f;
        desc.water_friction = 0.96f;
        desc.buoyancy = 0.95f;
        auto const o = physics::create_object(state, room, desc);
        // two chunks: the anchor (7.5, 0.2, no terrain) and the maw (7, 0.5)
        auto const k0 = physics::add_chunk(state, o, at, 7.5f, 0.2f);
        w.body_chunk_set_collide_with_terrain(k0, false);
        auto const k1 = physics::add_chunk(state, o, at, 7.f, 0.5f);
        physics::add_connection(state, o, k0, k1, 14.f, physics::connection_type::normal, 1.f, 0.5f);
        w.try_create_creature_body(c, o);
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_set_grasps(c, creature::grasp_pair{});
        w.creature_set_behavior(c, uint8_t(behavior::idle));
        w.creature_set_current_utility(c, 0.f);
        w.creature_set_destination(c, tile_of(at));
        auto& p = pl(state, c);
        {
            uint16_t const keep = p.data;
            p = creature::stowaway_payload{};
            p.data = keep;
        }
        w.stowaway_data_set_seed(d, seed);
        w.stowaway_data_set_body(d, body_state{});
        w.stowaway_data_set_graphics(d, graphics_state{});
        tracker::clear(state, c);
        auto const view = world::get_room_view(state, room);
        vec2 home, aim;
        ceiling_spot(view, at, home, aim);
        place(state, c, view, home, aim);
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        auto& b = state.world.stowaway_data_get_body(data(state, c));
        lose_all_grasps(state, c, b);
        creature::release_target(state, c);
        creature::erase(state, c);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        health_update(state, c);
        // Creature.Update + PhysicalObject.Update
        creature::stun_countdown(state, c);
        physics::update_object(state, room, body(state, c), nullptr, nullptr);
        ctx x(state, c, room);
        if (!x.heads_ok())
            return;
        x.prox = &pathing::ai_terrain_proximity_map(state, room.id);
        auto& p = x.p;
        bool const dead = w.creature_get_dead(c) != 0;
        // a new cycle: awake or asleep through it
        if (!dead && cycle_number(state, room.id) != p.cycle_seen)
            ai_wake_roll(x);
        eat(x);
        --p.spit_cooldown;
        --p.hunt_delay;
        if (creature::consious(state, c)) {
            act(x);
            for (uint32_t i = 0; i < head_count; ++i) {
                auto& h = x.b.heads[i];
                h.cooldown -= 0.1f;
                if (h.fired && h.retract_fac < 1.f && p.spit_cooldown <= 0)
                    h.retract_fac = mathf::clamp01(h.retract_fac + (grasp_of(state, c, x.b, i).who ? 0.0025f : 0.0055f));
                if (h.fired && h.retract_fac == 1.f) {
                    h.fired = 0;
                    h.cooldown = float(x.rng.range(12, 20));
                }
            }
        } else {
            if (!dead && x.beh() == behavior::sleeping)
                x.set_beh(behavior::idle);
            if (dead)
                x.set_beh(behavior::idle);
            p.maw_open = 0;
            for (auto& h : x.b.heads) {
                h.fired = 1;
                h.limp = 1;
                h.retract_fac = 0.f;
            }
        }
        bool flag = false, flag2 = false;
        update_heads(x, flag, flag2);
        update_bite(x, flag, flag2);
        // the anchor at its home (drawn up into the ceiling asleep)
        w.body_chunk_set_vel(x.k0, vec2{0.f, 0.f});
        x.pos(x.k0) = rw::lerp(x.b.home_pos, x.b.home_pos - x.b.placed_direction * 25.f, p.sleep_scale);
        update_feelers(x);
        float num13 = 0.008f;
        if (p.active_this_cycle)
            num13 = 0.095f;
        if (p.maw_open)
            num13 = 0.2f;
        if (x.rng.value() < num13) {
            float const dd = p.maw_open ? 15.f : 2.f;
            drip(x, x.pos(x.k1) + custom::rnv(x.rng) * dd, vec2{0.f, 1.f});
        }
        x.vel(x.k1) += vec2{0.f, room.gravity * mathf::inverse_lerp(0.01f, 0.25f, x.b.placed_direction.y)};
        x.vel(x.k1) += custom::dir_vec(x.pos(x.k1), x.pos(x.k0) + x.b.placed_direction * 10.f) * 0.33f;
        x.vel(x.k1) *= 0.92f;
        module_update(x);
        x.store();
    }

    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) {
        auto const& w = state.world;
        if (pl(state, c).maw_open)
            return false;
        auto const& b = w.stowaway_data_get_body(data(state, c));
        if (eating(b, t))
            return false;
        for (uint32_t i = 0; i < head_count; ++i)
            if (grasp_of(state, c, b, i).who == t)
                return false;
        return true;
    }

    void collide(sys::game_state&, dcon::creature_id, room_view const&, uint32_t, dcon::creature_id, uint32_t) {}

    void stun(sys::game_state& state, dcon::creature_id c, int32_t st) {
        auto& w = state.world;
        auto& p = pl(state, c);
        if (!p.active_this_cycle) {
            p.active_this_cycle = 1;
            w.creature_set_behavior(c, uint8_t(behavior::hidden));
            st /= 3;
            if (st < 20)
                return;
        } else {
            p.maw_open = 1;
            if (st < 60)
                st = 60;
        }
        int32_t& s = w.creature_get_stun(c);
        s = std::max(s, st);
        lose_all_grasps(state, c, w.stowaway_data_get_body(data(state, c)));
    }

    void violence(sys::game_state& state, dcon::creature_id c, uint32_t k, vec2 momentum, float damage, float stun_bonus, creature::damage_type) {
        auto& w = state.world;
        auto rng = rw::random(state.seed, state.tick, rw::ids(uint32_t(c.value), k));
        if (k >= 2)
            k = 0;
        // Creature.Violence (HealthState; the appendages, the heads, aren't
        // hit: StowawayBug.Violence's 0.05 for them doesn't come up)
        auto const hit = chunk(state, c, k);
        w.body_chunk_get_vel(hit) += rw::clamp_magnitude(momentum / w.body_chunk_get_mass(hit), 10.f);
        float const dmg = damage / base_damage_resistance;
        float st = (damage * 30.f + stun_bonus) / base_stun_resistance;
        float& health = w.creature_get_health(c);
        st *= 1.5f + mathf::inverse_lerp(0.5f, 0.f, health) * rng.value();
        stowaway::stun(state, c, int32_t(st));
        health -= dmg;
        if (rng.value() < -health || health < -1.f || (health < 0.f && rng.value() < 0.33f))
            creature::die(state, c);
    }

    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t) {
        auto& b = state.world.stowaway_data_get_body(data(state, c));
        for (uint32_t i = 0; i < head_count; ++i)
            if (grasp_of(state, c, b, i).who == t)
                release_head(state, c, b, i);
        for (int32_t i = int32_t(b.eat_count) - 1; i >= 0; --i)
            if (b.eat[i].who == t)
                remove_eat(b, uint32_t(i));
        auto& p = pl(state, c);
        if (p.focus == t)
            p.focus = dcon::creature_id{};
    }

    void died(sys::game_state&, dcon::creature_id) {}

    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, vec2 at) {
        auto const view = world::get_room_view(state, room);
        ivec2 const t = tile_of(at);
        ivec2 const d = view.shortcut_entrance_hole_direction(t);
        vec2 home, aim;
        if (d.x != 0 || d.y != 0) {
            // SpitOutOfShortCut: its home out of the hole, aiming out of it
            vec2 const dir = rw::to_vec2(d);
            home = room_view::middle_of_tile(t) + dir * 40.f;
            aim = home + dir * 20.f;
        } else {
            ceiling_spot(view, at, home, aim);
        }
        place(state, c, view, home, aim);
    }

    std::string describe(sys::game_state const& state, dcon::creature_id c) {
        static constexpr char const* names[] = {"idle", "escape rain", "hidden", "attacking", "digesting", "sleeping"};
        auto const& w = state.world;
        auto const& p = pl(state, c);
        auto const& b = w.stowaway_data_get_body(data(state, c));
        uint32_t const bh = w.creature_get_behavior(c);
        std::string s = "stowaway at " + std::to_string(int(b.home_pos.x)) + "," + std::to_string(int(b.home_pos.y)) + " behavior " + (bh < 6 ? names[bh] : "?");
        if (!p.active_this_cycle)
            s += " dormant";
        if (p.maw_open)
            s += " maw open";
        int32_t fired = 0;
        for (auto const& h : b.heads)
            fired += h.fired ? 1 : 0;
        if (fired)
            s += " heads out " + std::to_string(fired);
        if (any_grasp(state, c, b))
            s += " holding";
        if (b.eat_count)
            s += " eating";
        if (p.digest_prey > 0.f)
            s += " digesting";
        return s;
    }

    bool spawn_spot(room_view const& room, ivec2 tile) {
        // under a ceiling (within three tiles), with four open tiles below
        if (!room.inside(tile.x, tile.y) || room.get_tile(tile).solid())
            return false;
        bool ceiling = false;
        for (int32_t k = 1; k <= 3 && !ceiling; ++k)
            ceiling = room.get_tile(tile + ivec2{0, k}).solid();
        if (!ceiling)
            return false;
        for (int32_t k = 1; k <= 4; ++k)
            if (!room.inside(tile.x, tile.y - k) || room.get_tile(tile + ivec2{0, -k}).solid())
                return false;
        return true;
    }
}
