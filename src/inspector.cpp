// Downpour's Inspector.cs and InspectorAI.cs: the inspector. See inspector.hpp.
//
// The heads are Tentacles (Tentacle.cs) with the same TentacleProps as the
// monster kelp's, so their tile and chunk code is tentacle_plant.cpp's,
// over a head's working copy (`tent`) instead of the plant's.

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <string>
#include <vector>

#include "inspector.hpp"
#include "daddy_internal.hpp"
#include "dens.hpp"
#include "inspector_graphics.hpp"
#include "items.hpp"
#include "migrate.hpp"
#include "particles.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "rooms.hpp"
#include "rot.hpp"
#include "sound_types.hpp"
#include "tracker.hpp"

namespace inspector {
    using creature::relationship;
    using creature::rel;
    using creature::tmpl;
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;
    using daddy::detail::ray_trace_tiles_for_terrain;
    using daddy::detail::ray_trace_tiles_list;

    namespace {
        constexpr float float_max = std::numeric_limits<float>::max();
        constexpr ivec2 four_directions_and_zero[5] = {{0, 0}, {-1, 0}, {0, -1}, {1, 0}, {0, 1}};

        // the heads' Tentacle.TentacleProps(false, true, true, 0.5f, 0f,
        // 0.5f, 0.05f, 0.05f, 2.2f, 12f, 0.33333334f, 5f, 15, 60, 12, 20)
        constexpr float mass_deterioration_per_chunk = 0.5f;
        constexpr float goal_attraction_speed_tip = 0.5f;
        constexpr float goal_attraction_speed = 0.05f;
        constexpr float align_to_segment_speed = 0.05f;
        constexpr float backtrack_speed = 2.2f;
        constexpr float chunk_velocity_cap = 12.f;
        constexpr float tile_tentacle_update_speed = 0.33333334f;
        constexpr float tile_tentacle_snap_with_path_distance = 5.f;
        constexpr int32_t segment_phase_through_terrain_frames = 15;
        constexpr int32_t max_pull_tiles_per_tick = 12;
        constexpr int32_t terrain_hits_before_phase = 20;

        // the template (StaticWorld.InitInspector): baseDamageResistance,
        // baseStunResistance (instantDeathDamageLimit: the default, none)
        constexpr float base_damage_resistance = 1.f;
        constexpr float base_stun_resistance = 5.f;
        // Corridor 1, Air 1, Floor, Wall, Ceiling 100 (the dearest a
        // resistance can be here)
        constexpr pathing::movement movement_kind{{255, 10, 10, 255, 255, 10, 0}, 0};

        body_payload& pl(sys::game_state& state, dcon::creature_id c) { return state.world.creature_get_payload(c).inspector; }
        body_payload const& pl(sys::game_state const& state, dcon::creature_id c) { return state.world.creature_get_payload(c).inspector; }
        ivec2 tile_of(vec2 p) { return room_view::get_tile_position(p); }

        // Mathf.DeltaAngle
        float delta_angle(float current, float target) {
            float t = target - current;
            t = mathf::clamp(t - std::floor(t / 360.f) * 360.f, 0.f, 360.f);
            if (t > 180.f)
                t -= 360.f;
            return t;
        }
        // Custom.Angle (of the two vectors from the origin, signed)
        float custom_angle(vec2 a, vec2 b) { return rw::angle(a, b) * mathf::sign(custom::distance_to_line(a, b, -b)); }
        // Vector2.MoveTowards
        vec2 move_towards(vec2 cur, vec2 target, float max_delta) {
            vec2 const d = target - cur;
            float const m = rw::magnitude(d);
            if (m <= max_delta || m == 0.f)
                return target;
            return cur + d / m * max_delta;
        }

        // the inspector while it updates
        struct ctx {
            sys::game_state& state;
            dcon::creature_id c;
            room_view const& room;
            rw::rng rng;
            body_payload& p;
            dcon::inspector_data_id d;
            ai_state& ai;
            dcon::body_chunk_id k;
            dcon::creature_part_id part[head_count];
            dcon::inspector_head_id hid[head_count];
            head* h[head_count];
            std::vector<uint16_t> const* prox;

            // `salt` tells apart the ctxs made for one inspector in one tick
            ctx(sys::game_state& s, dcon::creature_id id, room_view const& r, uint32_t salt)
                : state(s), c(id), room(r), rng(rw::random(s.seed, s.tick, rw::ids(uint32_t(id.value), salt))), p(pl(s, id)), d(data_of(s.world, id)),
                  ai(s.world.inspector_data_get_ai(d)), k(creature::chunk(s, id, 0)), prox(&pathing::ai_terrain_proximity_map(s, r.id)) {
                for (uint32_t i = 0; i < head_count; ++i) {
                    part[i] = creature::part(s.world, id, i);
                    hid[i] = head_data(s.world, part[i]);
                    h[i] = &s.world.inspector_head_get_t(hid[i]);
                }
            }
            vec2& pos() { return state.world.body_chunk_get_pos(k); }
            vec2& vel() { return state.world.body_chunk_get_vel(k); }
            head_chunk& tip(uint32_t i) { return h[i]->c[head_chunks - 1]; }
            float health(uint32_t i) const { return state.world.creature_part_get_health(part[i]); }
            bool dead() const { return state.world.creature_get_dead(c) != 0; }
            bool consious() const { return creature::consious(state, c); }
            behavior current() const { return behavior(state.world.creature_get_behavior(c)); }
            void set_current(behavior b) { state.world.creature_set_behavior(c, uint8_t(b)); }
            int32_t proximity(ivec2 q) const {
                if (!room.inside(q.x, q.y))
                    return -1;
                return (*prox)[size_t(q.y) * size_t(room.width) + size_t(q.x)];
            }
            // Inspector.getHeadDirection
            vec2 head_dir(uint32_t i) const { return custom::dir_vec(h[i]->c[head_chunks - 2].last_pos, h[i]->c[head_chunks - 1].last_pos); }
        };

        //
        // A head's Tentacle (tentacle_plant.cpp's port over one head)
        //
        std::vector<ivec2>& segments_buffer() {
            static thread_local std::vector<ivec2> buf;
            return buf;
        }
        std::vector<ivec2>& grab_path_buffer() {
            static thread_local std::vector<ivec2> buf;
            return buf;
        }
        std::vector<ivec2>& scratch_buffer() {
            static thread_local std::vector<ivec2> buf;
            return buf;
        }

        struct tent {
            ctx& x;
            uint32_t i;
            head& t;
            std::vector<ivec2>& segments;
            std::vector<ivec2>& grab_path;
            std::vector<ivec2>& scratch;

            tent(ctx& cx, uint32_t index)
                : x(cx), i(index), t(*cx.h[index]), segments(segments_buffer()), grab_path(grab_path_buffer()), scratch(scratch_buffer()) {
                auto& w = x.state.world;
                auto const segs = w.inspector_head_get_segments(x.hid[i]);
                segments.assign(segs.begin(), segs.end());
                auto const gp = w.inspector_head_get_grab_path(x.hid[i]);
                grab_path.assign(gp.begin(), gp.end());
            }
            void store() {
                auto& w = x.state.world;
                w.inspector_head_get_segments(x.hid[i]).load_range(segments.data(), segments.data() + segments.size());
                w.inspector_head_get_grab_path(x.hid[i]).load_range(grab_path.data(), grab_path.data() + grab_path.size());
            }
        };

        bool has_any_solid(room_view const& room, ivec2 q) { return room.get_tile(q).solid(); }
        float float_dist(ivec2 a, ivec2 b) { return rw::magnitude(rw::to_vec2(a - b)); }
        bool inside(room_view const& room, ivec2 q) { return room.inside(q.x, q.y); }
        bool are_neighbors(ivec2 a, ivec2 b) {
            return a != b && (a.x == b.x || a.y == b.y) && std::abs(a.x - b.x) < 2 && std::abs(a.y - b.y) < 2;
        }
        bool are_diagonal_neighbors(ivec2 a, ivec2 b) {
            return a.x != b.x && a.y != b.y && (std::abs(a.x - b.x) < 2 || std::abs(a.y - b.y) < 2);
        }

        // SharedPhysics.TerrainCollisionData (as tentacle_plant.cpp's)
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

        // Tentacle.FloatBase: the body
        vec2 float_base(tent& tw) { return tw.x.pos(); }
        std::optional<ivec2> grab_dest(tent const& tw) {
            if (tw.t.float_grab_dest)
                return room_view::get_tile_position(*tw.t.float_grab_dest);
            return std::nullopt;
        }
        // Tentacle.CheckIfAnySegmentsLeft, BasePos
        ivec2 base_pos(tent& tw) {
            if (tw.segments.empty())
                tw.segments.push_back(room_view::get_tile_position(float_base(tw)));
            return tw.segments[0];
        }
        int32_t current_segment(tent const& tw, head_chunk const& c) {
            int32_t const n = int32_t(tw.segments.size());
            return custom::int_clamp(int32_t(c.t_pos * float(n - 1)), 0, n - 1);
        }
        ivec2 stuck_pos(tent const& tw, head_chunk const& c) { return tw.segments[size_t(current_segment(tw, c))]; }

        // Tentacle.Visual (nothing can be in the way within the terrain
        // proximity of `a`)
        bool visual(tent& tw, ivec2 a, ivec2 b) {
            int32_t const p = tw.x.proximity(a);
            if (p > 0 && tw.x.room.inside(b.x, b.y) && std::abs(b.x - a.x) + std::abs(b.y - a.y) < p)
                return true;
            return ray_trace_tiles_for_terrain(tw.x.room, a, b);
        }

        // Rope.Update (a rope without bends, far from the terrain, only
        // takes its new ends)
        void update_rope(tent& tw, world::rope_state& r, vec2 na, vec2 nb) {
            if (r.bend_count == 0) {
                ivec2 const t = room_view::get_tile_position(na);
                int32_t const p = tw.x.proximity(t);
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
            rot::update_rope(r, tw.x.room, na, nb);
        }

        bool rope_active(tent const& tw, uint32_t i) {
            auto const& c = tw.t.c[i];
            return c.phase == -1.f && (i == 0 || tw.t.c[i - 1].phase == -1.f) && (tw.t.backtrack_from < int32_t(i) || tw.t.backtrack_from == -1);
        }

        // TentacleChunk.Reset
        void chunk_reset(tent& tw, head_chunk& c) {
            c.trail = stuck_pos(tw, c);
            c.phase = -1.f;
            c.pos = room_view::middle_of_tile(stuck_pos(tw, c));
            c.last_pos = c.pos;
            c.vel = vec2{0.f, 0.f};
            vec2 const b = float_base(tw);
            world::rope_state r;
            r.a = r.last_a = b;
            r.b = r.last_b = b + vec2{1.f, 1.f};
            r.total_length = rw::distance(r.a, r.b);
            r.thickness = 1.f;
            c.rope = r;
        }

        // TentacleChunk.Update
        void chunk_update(tent& tw, uint32_t i) {
            auto& c = tw.t.c[i];
            auto& t = tw.t;
            constexpr float n = float(head_chunks);
            c.last_pos = c.pos;
            update_rope(tw, c.rope, i == 0 ? float_base(tw) : t.c[i - 1].pos, c.pos);
            if (!rope_active(tw, i) || c.phase > -1.f || (i > 0 && t.c[i - 1].phase > -1.f) || c.rope.total_length > head_length / n * 5.f)
                c.rope.bend_count = 0;
            if (c.phase == -1.f) {
                if (c.phases_to_same_location > 0 && tw.segments[size_t(current_segment(tw, c))] == c.after_phase_stuck_pos) {
                    vec2 const m = room_view::middle_of_tile(stuck_pos(tw, c));
                    c.pos = vec2{mathf::clamp(c.pos.x, m.x - 9.f, m.x + 9.f), mathf::clamp(c.pos.y, m.y - 9.f, m.y + 9.f)};
                }
                float const ideal = head_length / n * mathf::lerp(1.f, 0.1f, t.retract_fac);
                if (i == 0) {
                    vec2 a = custom::dir_vec(c.pos, float_base(tw));
                    float num = rw::distance(c.pos, float_base(tw));
                    if (rope_active(tw, i)) {
                        a = custom::dir_vec(c.pos, rot::rope_b_connect(c.rope));
                        num = c.rope.total_length;
                    }
                    if (num > ideal) {
                        // (pullAtConnectionChunk 0: the body isn't pulled)
                        c.pos -= a * (ideal - num);
                        c.vel -= a * (ideal - num);
                    }
                    c.stretched_fac = head_length / n / mathf::max(1.f, num);
                } else {
                    auto& prev = t.c[i - 1];
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
                    c.stretched_fac = head_length / n / mathf::max(1.f, num);
                }
                if (stuck_pos(tw, c) != c.trail)
                    c.trail = stuck_pos(tw, c);
                ivec2 const here = room_view::get_tile_position(c.pos);
                if (t.backtrack_from == -1 && !visual(tw, here, tw.segments[size_t(current_segment(tw, c))]))
                    t.backtrack_from = int32_t(i);
                if (t.backtrack_from != -1 && int32_t(i) >= t.backtrack_from) {
                    bool flag = false;
                    int32_t const cs = current_segment(tw, c);
                    bool prev_seen = cs >= 1 && visual(tw, here, tw.segments[0]);
                    for (int32_t k = 1; k <= cs && !flag; ++k) {
                        bool const seen = visual(tw, here, tw.segments[size_t(k)]);
                        bool const was = prev_seen;
                        prev_seen = seen;
                        if (!seen && was) {
                            c.vel += rw::clamp_magnitude(room_view::middle_of_tile(tw.segments[size_t(k - 1)]) - c.pos, 20.f) * backtrack_speed / 20.f;
                            flag = true;
                        }
                    }
                    if (!flag) {
                        vec2 const fb = i > 0 ? t.c[i - 1].pos : float_base(tw);
                        c.vel += rw::clamp_magnitude(fb - c.pos, 20.f) * backtrack_speed / 20.f;
                        if (i > 0)
                            t.c[i - 1].vel += rw::clamp_magnitude(fb - c.pos, 20.f) * backtrack_speed / 20.f;
                    }
                    if (!flag && (c.contact_point.x != 0 || c.contact_point.y != 0)) {
                        ++c.phase_attempts;
                        if (c.phase_attempts > terrain_hits_before_phase) {
                            c.phase_from = c.pos;
                            c.phase = 0.f;
                            c.phase_attempts = 0;
                        }
                    }
                } else {
                    c.phase_attempts = 0;
                    if (t.limp) {
                        for (int32_t k = current_segment(tw, c); k >= 0; --k) {
                            ivec2 const s = tw.segments[size_t(k)];
                            if (visual(tw, here, s)) {
                                c.vel += rw::clamp_magnitude(room_view::middle_of_tile(s) - c.pos, 20.f) * 0.005f;
                                break;
                            }
                        }
                    } else {
                        if (t.float_grab_dest) {
                            float const s = i == head_chunks - 1 ? goal_attraction_speed_tip : goal_attraction_speed;
                            c.vel += rw::clamp_magnitude(*t.float_grab_dest - c.pos, 20.f) * s / 20.f;
                        }
                        c.vel += rw::clamp_magnitude(room_view::middle_of_tile(tw.segments[size_t(current_segment(tw, c))]) - c.pos, 20.f) * align_to_segment_speed / 20.f;
                    }
                }
                c.vel = rw::clamp_magnitude(c.vel, chunk_velocity_cap);
                c.pos += c.vel;
                c.last_contact_point = c.contact_point;
                terrain_collision cd{c.pos, c.pos - c.vel, c.vel, c.rad};
                vertical_collision(tw.x.room, cd);
                slopes_vertically(tw.x.room, cd);
                horizontal_collision(tw.x.room, cd);
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
                if (i > 0 && t.c[i - 1].phase == -1.f) {
                    auto& prev = t.c[i - 1];
                    vec2 const a = custom::dir_vec(c.pos, prev.pos);
                    float const num = rw::distance(c.pos, prev.pos);
                    float const ideal = head_length / n * mathf::lerp(1.f, 0.1f, t.retract_fac);
                    if (num > ideal) {
                        prev.pos += a * (ideal - num) * mass_deterioration_per_chunk;
                        prev.vel += a * (ideal - num) * mass_deterioration_per_chunk;
                    }
                    c.stretched_fac = ideal / mathf::max(num, 0.0001f);
                }
            }
        }

        // Tentacle.MoveSegment
        bool move_segment(tent& tw, int32_t s, ivec2 dest, bool allow_occupied, bool allow_solid) {
            auto& seg = tw.segments;
            if (s == 0 || s >= int32_t(seg.size()) || seg[size_t(s)] == dest || (!are_neighbors(seg[size_t(s - 1)], dest) && seg[size_t(s - 1)] != dest)
            || (!allow_solid && has_any_solid(tw.x.room, dest)))
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
                    if (has_any_solid(tw.x.room, cc))
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
        void shorten(tent& tw, int32_t from, int32_t to, std::vector<ivec2> const& straight, int32_t count) {
            tw.segments.erase(tw.segments.begin() + from + 1, tw.segments.begin() + to);
            for (int32_t i = count - 2; i > 0; --i)
                tw.segments.insert(tw.segments.begin() + from + 1, straight[size_t(i)]);
        }

        // Tentacle.PullAtTentacle
        void pull_at_tentacle(tent& tw, int32_t start, bool only_simple_pull, std::vector<ivec2>& path) {
            auto& t = tw.t;
            auto& seg = tw.segments;
            if (!only_simple_pull) {
                while (t.pull_counter > 0) {
                    bool flag = false;
                    for (int32_t num = start; num < int32_t(seg.size()) - 3 && !flag; ++num) {
                        for (int32_t i = int32_t(seg.size()) - 1; i >= num + 3; --i) {
                            int32_t const num2 = i - num - custom::manhattan_distance(seg[size_t(num)], seg[size_t(i)]);
                            if (num2 > 0 && num2 <= t.pull_counter && visual(tw, seg[size_t(num)], seg[size_t(i)])) {
                                int32_t const num3 = ray_trace_tiles_list(seg[size_t(num)].x, seg[size_t(num)].y, seg[size_t(i)].x, seg[size_t(i)].y, path);
                                bool flag2 = true;
                                if (num2 > 1 && num2 <= t.pulls_this_tick) {
                                    for (int32_t j = 1; j < num3 - 1; ++j) {
                                        for (int32_t k = num + 1; k < i; ++k) {
                                            if (custom::manhattan_distance(seg[size_t(k)], path[size_t(j)]) > 2 && !visual(tw, seg[size_t(k)], path[size_t(j)])) {
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

        // Tentacle.FindTentaclePath (a heap on heuristic and insertion order)
        int32_t find_tentacle_path(tent& tw, ivec2 start, ivec2 goal, std::vector<ivec2>& path) {
            struct cell {
                ivec2 pos;
                int32_t parent;
            };
            static thread_local std::vector<cell> cells;
            static thread_local std::vector<uint32_t> visited;
            static thread_local uint32_t stamp = 0;
            int32_t const w = tw.x.room.width, h = tw.x.room.height;
            size_t const size = size_t(w) * size_t(h);
            if (visited.size() < size)
                visited.assign(size, 0);
            if (++stamp == 0) {
                std::fill(visited.begin(), visited.end(), 0);
                stamp = 1;
            }
            using node = std::pair<float, uint32_t>;
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
                for (auto const dd : custom::four_directions) {
                    ivec2 const q = p + dd;
                    if (q.x < 0 || q.x >= w || q.y < 0 || q.y >= h)
                        continue;
                    uint32_t& v = visited[size_t(q.x) * size_t(h) + size_t(q.y)];
                    if (v == stamp)
                        continue;
                    v = stamp;
                    float const num2 = rw::distance(rw::to_vec2(q), s) + (has_any_solid(tw.x.room, q) ? 1000.f : 0.f);
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
        void move_aligned_segments_with_path(tent& tw, std::vector<ivec2>& path) {
            auto& seg = tw.segments;
            auto& gp = tw.grab_path;
            int32_t const lim = std::min(int32_t(seg.size()) - 1, int32_t(gp.size()) - 1);
            int32_t num = lim;
            int32_t k = 0;
            while (k < lim && float_dist(seg[size_t(k + 1)], gp[size_t(k + 1)]) <= tile_tentacle_snap_with_path_distance && visual(tw, seg[size_t(k + 1)], gp[size_t(k + 1)])) {
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
            pull_at_tentacle(tw, num, true, path);
        }

        // Tentacle.FindGrabPath
        void find_grab_path(tent& tw, std::vector<ivec2>& path) {
            ivec2 const g = *grab_dest(tw);
            ivec2 const b = base_pos(tw);
            int32_t num;
            if (!inside(tw.x.room, g) || !inside(tw.x.room, b) || visual(tw, b, g))
                num = ray_trace_tiles_list(b.x, b.y, g.x, g.y, tw.grab_path);
            else
                num = find_tentacle_path(tw, b, g, tw.grab_path);
            if (int32_t(tw.grab_path.size()) > num)
                tw.grab_path.resize(size_t(num));
            move_aligned_segments_with_path(tw, path);
        }

        // Tentacle.Grow
        void grow(tent& tw, int32_t add) {
            auto& seg = tw.segments;
            for (int32_t i = 0; i < add; ++i) {
                float num = float_max;
                ivec2 item = seg.back();
                for (auto const dd : four_directions_and_zero) {
                    float num2 = tw.x.rng.value();
                    ivec2 const cc = seg.back() + dd;
                    if (has_any_solid(tw.x.room, cc))
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
        void move_base(tent& tw, ivec2 to, std::vector<ivec2>& path) {
            auto& seg = tw.segments;
            if (has_any_solid(tw.x.room, to) || to == base_pos(tw))
                return;
            if (are_neighbors(seg[0], to)) {
                int32_t num = -1;
                for (int32_t i = 1; i < int32_t(seg.size()) - 1; ++i)
                    if (seg[size_t(i)] == to)
                        num = i;
                if (num > 0) {
                    seg.erase(seg.begin(), seg.begin() + num);
                    grow(tw, num);
                } else {
                    seg.insert(seg.begin(), to);
                    ++tw.t.pull_counter;
                    if (tw.t.pull_counter > 4)
                        pull_at_tentacle(tw, 0, false, path);
                }
            } else {
                int32_t num2;
                if (!inside(tw.x.room, seg[0]) || !inside(tw.x.room, to) || visual(tw, seg[0], to))
                    num2 = ray_trace_tiles_list(seg[0].x, seg[0].y, to.x, to.y, path);
                else
                    num2 = find_tentacle_path(tw, seg[0], to, path);
                ivec2 b = seg[0];
                for (int32_t j = 1; j < num2; ++j) {
                    seg.insert(seg.begin(), seg[0] + (path[size_t(j)] - b));
                    b = path[size_t(j)];
                }
                tw.t.pull_counter += num2 - 1;
                if (tw.t.pull_counter > 4)
                    pull_at_tentacle(tw, 0, false, path);
            }
            if (grab_dest(tw))
                find_grab_path(tw, path);
        }

        // Tentacle.AlignWithGrabPath
        void align_with_grab_path(tent& tw, std::vector<ivec2>& path) {
            auto& seg = tw.segments;
            auto& gp = tw.grab_path;
            bool flag = false;
            for (int32_t i = std::min(int32_t(seg.size()) - 2, int32_t(gp.size()) - 1); i > 0; --i) {
                if (i >= int32_t(seg.size()) || i >= int32_t(gp.size()))
                    continue;
                if (seg[size_t(i)] != gp[size_t(i)] && visual(tw, seg[size_t(i)], gp[size_t(i)])) {
                    ray_trace_tiles_list(seg[size_t(i)].x, seg[size_t(i)].y, gp[size_t(i)].x, gp[size_t(i)].y, path);
                    if (path.size() > 1 && i + 1 < int32_t(seg.size()) && move_segment(tw, i + 1, path[1], false, false))
                        flag = true;
                }
            }
            if (!flag) {
                for (int32_t k = 1; k < int32_t(seg.size()) && k < int32_t(gp.size()); ++k) {
                    if (seg[size_t(k)] != gp[size_t(k)]) {
                        move_segment(tw, k, gp[size_t(k)], true, false);
                        return;
                    }
                }
            }
        }

        // Tentacle.CurrentLength
        float current_length(tent const& tw) {
            float num = float(tw.segments.size()) * 20.f;
            for (size_t i = 0; i + 2 < tw.segments.size(); ++i)
                if (are_diagonal_neighbors(tw.segments[i], tw.segments[i + 2]))
                    num -= 5.9000006f;
            return num;
        }

        // Tentacle.AdjustLength (`shorten` on)
        void adjust_length(tent& tw, std::vector<ivec2>& path) {
            float const num = current_length(tw);
            float num2 = head_length;
            if (!tw.grab_path.empty())
                num2 = mathf::min(head_length, float(tw.grab_path.size()) * 20.f);
            int32_t const num3 = int32_t(num2 / 20.f - num / 20.f);
            if (num3 > 0 && (tw.grab_path.empty() || tw.segments.size() < tw.grab_path.size())) {
                grow(tw, 1);
                return;
            }
            if (num3 < 0) {
                ++tw.t.pull_counter;
                pull_at_tentacle(tw, 0, false, path);
            }
        }

        // Tentacle.GravityDirection, Gravity
        void gravity(tent& tw) {
            ivec2 b{0, -1};
            if (tw.x.rng.value() < 0.5f)
                b = ivec2{tw.x.rng.value() < 0.5f ? -1 : 1, -1};
            bool flag = false;
            for (int32_t i = 0; i < int32_t(tw.segments.size()) && !flag; ++i)
                if (move_segment(tw, i, tw.segments[size_t(i)] + b, false, false))
                    flag = true;
            if (!flag)
                for (int32_t i = 0; i < int32_t(tw.segments.size()); ++i)
                    if (move_segment(tw, i, tw.segments[size_t(i)] + b, true, false))
                        break;
        }

        // Tentacle.Update
        void tentacle_update(tent& tw) {
            auto& path = tw.scratch;
            move_base(tw, room_view::get_tile_position(float_base(tw)), path);
            tw.t.update_counter += tile_tentacle_update_speed;
            if (tw.t.update_counter >= 1.f) {
                tw.t.pulls_this_tick = max_pull_tiles_per_tick;
                if (tw.t.limp)
                    gravity(tw);
                else if (!tw.grab_path.empty())
                    align_with_grab_path(tw, path);
                adjust_length(tw, path);
                tw.t.update_counter -= 1.f;
            }
            tw.t.backtrack_from = -1;
            for (uint32_t i = 0; i < head_chunks; ++i)
                chunk_update(tw, i);
        }

        // Tentacle.NewRoom: every segment on the body's tile, the chunks on them
        void tentacle_new_room(tent& tw) {
            tw.segments.assign(size_t(head_length / 20.f), room_view::get_tile_position(tw.x.pos()));
            for (uint32_t i = 0; i < head_chunks; ++i)
                chunk_reset(tw, tw.t.c[i]);
            tw.grab_path.clear();
        }

        // Tentacle.Reset(resetPos)
        void tentacle_reset(tent& tw, vec2 at) {
            tw.segments.assign(1, room_view::get_tile_position(at));
            for (uint32_t i = 0; i < head_chunks; ++i) {
                tw.t.c[i].pos = at;
                tw.t.c[i].vel = vec2{0.f, 0.f};
                chunk_reset(tw, tw.t.c[i]);
            }
        }

        //
        // What the heads hold
        //
        bool is_spear(world::item_type t) {
            return t == world::item_type::spear || t == world::item_type::explosive_spear || t == world::item_type::electric_spear;
        }
        // the chunk a hold is on, when it is still in the room
        dcon::body_chunk_id hold_chunk(ctx const& x, hold const& hd) {
            auto const& w = x.state.world;
            if (hd.kind == hold_kind::creature) {
                if (!creature::here(x.state, hd.creature, x.room.id) || hd.chunk >= creature::chunk_count_of(x.state, hd.creature))
                    return dcon::body_chunk_id{};
                return creature::chunk_of(x.state, hd.creature, hd.chunk);
            }
            if (hd.kind == hold_kind::item) {
                if (!w.item_is_valid(hd.item) || rooms::room_of(x.state, hd.item) != x.room.id)
                    return dcon::body_chunk_id{};
                return items::chunk(x.state, hd.item);
            }
            return dcon::body_chunk_id{};
        }
        bool hold_is_weapon(ctx const& x, hold const& hd) {
            return hd.kind == hold_kind::item && x.state.world.item_is_valid(hd.item) && items::is_weapon(x.state.world.item_get_type(hd.item));
        }
        // Inspector.HeadWeaponized
        bool weaponized(ctx const& x, uint32_t i) {
            if (x.health(i) <= 0.f)
                return false;
            return hold_is_weapon(x, x.h[i]->want) || hold_is_weapon(x, x.h[i]->grab);
        }
        // Inspector.HeadsCrippled
        bool crippled(ctx const& x, uint32_t i) { return x.h[i]->limp != 0; }
        // Inspector.isOtherHeadsGoalChunk
        bool other_heads_goal(ctx const& x, uint32_t i, hold const& hd) {
            for (uint32_t j = 0; j < head_count; ++j)
                if (j != i && (x.h[j]->want == hd || x.h[j]->grab == hd))
                    return true;
            return false;
        }
        hold creature_hold(dcon::creature_id t) {
            hold hd;
            hd.kind = hold_kind::creature;
            hd.creature = t;
            hd.chunk = 0;
            return hd;
        }
        hold item_hold(dcon::item_id it) {
            hold hd;
            hd.kind = hold_kind::item;
            hd.item = it;
            return hd;
        }

        // InspectorAI.OrderAHeadToGrabObject (its firstChunk)
        void order_a_head_to_grab(ctx& x, hold const& obj) {
            auto const k = hold_chunk(x, obj);
            if (!k)
                return;
            vec2 const op = x.state.world.body_chunk_get_pos(k);
            uint32_t num = 0;
            for (uint32_t i = 0; i < head_count; ++i)
                if (!crippled(x, i) && !weaponized(x, i) && rw::distance(x.tip(i).pos, op) < rw::distance(x.tip(num).pos, op))
                    num = i;
            if (!x.h[num]->want.valid() && !x.h[num]->grab.valid())
                x.h[num]->want = obj;
        }

        // InspectorAI.GrabAttentionWithObject
        void grab_attention(ctx& x, dcon::creature_id t) {
            if (x.p.active_eye != -1) {
                x.h[uint32_t(x.p.active_eye)]->curiosity = t;
                x.ai.attention_grabber_timer = float(attention_delay_max);
            }
        }

        // Creature.AllGraspsLetGoOfThisObject: what holds `t` lets go of it
        void all_grasps_let_go(ctx& x, dcon::creature_id t) {
            dcon::creature_id holders[8];
            uint32_t grasps[8];
            uint32_t n = 0;
            creature::for_each_holder(x.state, x.room.id, t, [&](dcon::creature_id hc, uint32_t g) {
                if (n < 8) {
                    holders[n] = hc;
                    grasps[n] = g;
                    ++n;
                }
            });
            for (uint32_t i = 0; i < n; ++i)
                creature::release_grasp(x.state, holders[i], grasps[i]);
        }

        //
        // InspectorAI: Tracker(10, 10, -1, .., 10), PreyTracker(4, 0.6, 60,
        // 300) never giving up on prey, ThreatTracker(3)
        //
        tracker::params const& tracker_params() {
            static tracker::params const p = [] {
                tracker::params q;
                q.see_around_corners = 10;
                q.frames_to_remember = -1;
                q.ghost_dismissal_range = 10;
                q.prey_max = 4;
                q.persistance_bias = 0.6f;
                q.sure_to_get = 60.f;
                q.sure_to_lose = 300.f;
                q.give_up_unreachable = -1;
                q.threat_max = 3;
                return q;
            }();
            return p;
        }

        // the long legs and the inspectors (StaticWorld: the Inspector's
        // ancestor is the DaddyLongLegs' template)
        bool long_legs_ancestor(tmpl t) {
            return t == tmpl::daddy_long_legs || t == tmpl::brother_long_legs || t == tmpl::terror_long_legs || t == tmpl::hunter_daddy || t == tmpl::inspector;
        }

        // ArtificialIntelligence.VisualContact with InspectorAI.VisualScore:
        // only the active eye sees, what it faces, from its tip
        bool visual_contact(ctx& x, vec2 look) {
            if (x.p.active_eye < 0)
                return false;
            uint32_t const e = uint32_t(x.p.active_eye);
            if (x.health(e) <= 0.f)
                return false;
            auto const& t = *x.h[e];
            float const current = custom_angle(t.c[head_chunks - 2].pos, t.c[head_chunks - 1].pos);
            float const target = custom_angle(t.c[head_chunks - 1].pos, look);
            if (mathf::inverse_lerp(50.f, 30.f, delta_angle(current, target)) <= 0.f)
                return false;
            return x.room.visual_contact(t.c[head_chunks - 1].pos, look);
        }
        bool hook_sees(void* u, dcon::body_chunk_id k) {
            auto& x = *static_cast<ctx*>(u);
            return visual_contact(x, x.state.world.body_chunk_get_pos(k));
        }
        bool hook_sees_point(void* u, vec2 p) { return visual_contact(*static_cast<ctx*>(u), p); }
        // PathFinder.CoordinateReachableAndGetbackable
        bool reachable(ctx& x, ivec2 t) {
            if (!x.room.inside(t.x, t.y) || !pathing::accessible(x.room, t, movement_kind))
                return false;
            auto const f = pathing::field_to(x.state, x.room.id, t, movement_kind);
            return pathing::distance(x.state, f, tile_of(x.pos())) != pathing::unreachable;
        }
        bool hook_reach(void* u, ivec2 t) { return reachable(*static_cast<ctx*>(u), t); }
        // ModuleToTrackRelationship is the Tracker for everything: only what
        // it explicitly hunts (Eats) goes to the PreyTracker
        creature::track_module hook_module(relationship r) { return r.type == rel::eats ? creature::track_module::prey : creature::track_module::none; }
        // ArtificialIntelligence.CreatureSpotted: CreatureLooker.ReevaluateLookObject(rep, 4)
        void hook_spotted(void* u, creature::tracked const& r, bool) {
            auto& x = *static_cast<ctx*>(u);
            x.ai.spotted = r.who;
        }

        // the head holding `t`, if any
        int32_t head_holding(ctx const& x, dcon::creature_id t) {
            for (uint32_t j = 0; j < head_count; ++j)
                if (!crippled(x, j) && x.h[j]->grab.kind == hold_kind::creature && x.h[j]->grab.creature == t)
                    return int32_t(j);
            return -1;
        }

        // InspectorAI.CreateTrackedCreatureState + UpdateDynamicRelationship
        relationship hook_relate(void* u, creature::tracked const& r) {
            auto& x = *static_cast<ctx*>(u);
            auto& w = x.state.world;
            relationship cur = r.rel;
            tmpl const tt = creature::template_of(x.state, r.who);
            // (a SimpleCreatureRepresentation: its relationship as it is)
            if (creature::small_creature(tt))
                return cur;
            // CreateTrackedCreatureState, the first time (state bit 1: done;
            // bit 2: Violence's AddPrey waiting)
            if ((r.state & 1) == 0) {
                if (auto* q = tracker::find(tracker::of(x.state, x.c), r.who))
                    q->state |= 1;
                cur.type = long_legs_ancestor(tt) ? rel::attacks : rel::uncomfortable;
                if (!creature::dead(x.state, r.who))
                    grab_attention(x, r.who);
                cur.intensity = 1.f;
                x.ai.first_time_attention_grabber = r.who;
                return cur;
            }
            // something that hurt it (Violence's preyTracker.AddPrey)
            if ((r.state & 2) != 0) {
                if (auto* q = tracker::find(tracker::of(x.state, x.c), r.who))
                    q->state &= uint8_t(~2u);
                cur.type = rel::eats;
                cur.intensity = 1.f;
            }
            auto const* prey = tracker::prey(tracker::of(x.state, x.c));
            if (prey && prey->who == r.who && cur.type == rel::uncomfortable) {
                cur.type = rel::eats;
                cur.intensity = 1.f;
            }
            bool const here = creature::here(x.state, r.who, x.room.id);
            vec2 const body = x.pos();
            vec2 const seen_at = rw::to_vec2(tracker::best_guess(r)) * 20.f;
            if (cur.type == rel::uncomfortable || tt == tmpl::inspector) {
                if (r.visual) {
                    if (here && !creature::dead(x.state, r.who)) {
                        // (no neurons to hold, no dangerous throw locations)
                        cur.intensity = 1.f;
                    }
                    if (cur.intensity < 0.5f && x.rng.value() < 0.02f)
                        cur.intensity = 1.f;
                    if (rw::distance(body, seen_at) < 100.f && cur.intensity + 0.09f < 1.f)
                        cur.intensity += 0.09f;
                }
                if (cur.intensity > 0.f && r.visual)
                    cur.intensity -= 0.006f;
                if (cur.intensity > 0.f && !r.visual)
                    cur.intensity -= 0.01f;
                if (cur.intensity < 0.f)
                    cur.intensity = 0.f;
            } else if (cur.type == rel::attacks && r.visual) {
                if (x.p.active_eye != -1 && rw::distance(x.tip(uint32_t(x.p.active_eye)).pos, seen_at) < 500.f) {
                    x.p.anger += 0.19f;
                    if (x.p.anger > 1.f) {
                        if (x.p.anger > 2.f)
                            x.p.anger = 2.f;
                        cur.intensity += 0.08f;
                        if (cur.intensity > 1.f) {
                            cur.intensity = 1.f;
                            x.set_current(behavior::escape_rain);
                            x.ai.new_idle_pos_counter = x.rng.range(300, 400);
                        }
                    }
                } else {
                    x.p.anger -= 0.001f;
                    if (x.p.anger < 0.f)
                        x.p.anger = 0.f;
                }
            } else if (cur.type == rel::eats && r.visual && here) {
                if (creature::dead(x.state, r.who) && x.p.anger <= 0.f) {
                    cur.type = rel::uncomfortable;
                    cur.intensity = 1.f;
                }
                x.p.anger += 0.19f;
                if (x.p.anger > 1.f)
                    x.p.anger = 1.f;
                cur.intensity += 0.05f;
                if (cur.intensity >= 1.f) {
                    if (!long_legs_ancestor(tt)) {
                        int32_t const num2 = head_holding(x, r.who);
                        if (num2 == -1) {
                            order_a_head_to_grab(x, creature_hold(r.who));
                        } else {
                            auto& tp = x.tip(uint32_t(num2));
                            auto const k0 = creature::chunk_of(x.state, r.who, 0);
                            vec2& fp = w.body_chunk_get_pos(k0);
                            vec2& fv = w.body_chunk_get_vel(k0);
                            float const tm = rw::magnitude(tp.vel);
                            if (tm < 1.f || tm > 4.f) {
                                tp.vel *= 1.2f;
                                tp.vel += vec2{float(x.rng.range(-18, 18)), float(x.rng.range(-18, 18))};
                                fp = tp.pos;
                                fv = tp.vel;
                            }
                            float const target = custom::vec_to_deg(custom::dir_vec(body, fp));
                            // (no DangerousThrowLocations: flag stays false, num4 35)
                            constexpr float num4 = 35.f;
                            ivec2 const from{int32_t(fp.x / 20.f), int32_t(fp.y / 20.f)};
                            ivec2 const to{int32_t((fp.x + fv.x * 3.f) / 20.f), int32_t((fp.y + fv.y * 3.f) / 20.f)};
                            float const fm = rw::magnitude(fv);
                            float const dang = delta_angle(custom::vec_to_deg(fv), target);
                            bool fling = false;
                            if (!ray_trace_tiles_for_terrain(x.room, from, to) && (fm > 50.f || (x.rng.value() < 0.5f && dang < num4 && fm > 30.f)))
                                fling = true;
                            else if (dang < num4 && fm > 20.f)
                                fling = true;
                            if (fling) {
                                cur.intensity = 0.f;
                                x.h[uint32_t(num2)]->want = hold{};
                                x.h[uint32_t(num2)]->grab = hold{};
                                // (no Vulture_Peck sound id)
                            }
                        }
                    } else {
                        cur.intensity += 0.03f;
                        if (cur.intensity > 1.f)
                            cur.intensity = 1.f;
                    }
                }
            }
            return cur;
        }

        tracker::hooks make_hooks(ctx& x) {
            tracker::hooks h;
            h.user = &x;
            h.sees = hook_sees;
            h.sees_point = hook_sees_point;
            h.relate = hook_relate;
            h.module = hook_module;
            h.spotted = hook_spotted;
            h.reach = hook_reach;
            return h;
        }

        // CreatureLooker.ReevaluateLookObject (scaryness: 1)
        void reevaluate_look(ctx& x, dcon::creature_id favor, float bonus) {
            auto const& ts = tracker::of(x.state, x.c);
            x.ai.look_creature = dcon::creature_id{};
            float best = 0.f;
            bool const flag = x.rng.value() < 0.5f && !favor;
            ivec2 const at = tile_of(x.pos());
            tmpl const me = creature::template_of(x.state, x.c);
            for (uint32_t i = 0; i < ts.count; ++i) {
                auto const& r = ts.reps[i];
                if (!creature::live(x.state, r.who))
                    continue;
                tmpl const tt = creature::template_of(x.state, r.who);
                relationship const st = creature::static_relationship(me, tt);
                float num2 = 1.f + mathf::lerp(creature::body_size(tt), 1.f, 0.5f) * st.intensity * tracker::chance_of_finding(r, tracker_params().see_around_corners);
                if (flag)
                    num2 += x.rng.value();
                if (r.visual && creature::here(x.state, r.who, x.room.id))
                    num2 *= 1.f + rw::magnitude(x.state.world.body_chunk_get_vel(creature::chunk_of(x.state, r.who, creature::main_chunk_of(x.state, r.who))));
                num2 /= mathf::lerp(rw::magnitude(rw::to_vec2(tracker::best_guess(r) - at)), 1.f, 0.6f);
                if (r.who == favor)
                    num2 *= bonus;
                if (st.type == rel::ignores)
                    num2 /= 2.f;
                if (num2 > best) {
                    best = num2;
                    x.ai.look_creature = r.who;
                }
            }
        }
        // CreatureLooker.Update (chanceToLookAtNothing 0.2, 20 frames)
        void looker_update(ctx& x) {
            if (x.ai.look_creature && !tracker::find(tracker::of(x.state, x.c), x.ai.look_creature))
                x.ai.look_creature = dcon::creature_id{};
            if (x.ai.look_focus_delay > 0)
                --x.ai.look_focus_delay;
            if (x.ai.look_focus_delay < 1 && x.rng.value() < 1.f / 20.f) {
                x.ai.look_focus_delay = 20;
                if (x.rng.value() > 0.2f)
                    reevaluate_look(x, dcon::creature_id{}, 0.f);
                else
                    x.ai.look_creature = dcon::creature_id{};
            }
        }

        //
        // The head goals
        //
        // InspectorAI.PopulateHeadGoals
        void populate_head_goals(ctx& x, int32_t n) {
            auto goals = x.state.world.inspector_data_get_head_goals(x.d);
            if (goals.size() >= 500)
                return;
            for (int32_t i = 0; i < n; ++i) {
                ivec2 const t{x.rng.range(0, x.room.width), x.rng.range(0, x.room.height)};
                if (!x.room.get_tile(t).solid() && goals.size() < max_head_goals)
                    goals.push_back(rw::to_vec2(t) * 20.f);
            }
        }
        // InspectorAI.PopulateInspectionModeHeadGoals
        void populate_inspection_goals(ctx& x, int32_t n) {
            auto goals = x.state.world.inspector_data_get_head_goals(x.d);
            ivec2 const at = tile_of(x.pos());
            for (int32_t i = 0; i < n; ++i) {
                ivec2 t = at;
                t.x += x.rng.range(-20, 20);
                t.y += x.rng.range(-20, 20);
                if (x.room.inside(t.x, t.y) && !x.room.get_tile(t).solid() && goals.size() < max_head_goals)
                    goals.push_back(rw::to_vec2(t) * 20.f);
            }
        }
        // (PopulateHeadSpecialGoals: around zap coils and coral, none here)

        // InspectorAI.HeadGoalInterest
        float head_goal_interest(ctx& x, uint32_t i, vec2 goal) {
            vec2 a = x.tip(i).pos;
            a = rw::lerp(a, x.pos(), mathf::lerp(0.4f, 0.9f, x.rng.value()));
            a += x.vel() * 10.f;
            float num = mathf::inverse_lerp(800.f, 60.f, rw::distance(a, goal));
            num = mathf::lerp(num, 0.f, mathf::inverse_lerp(0.f, 100.f, rw::distance(x.pos(), goal)));
            if (x.ai.look_creature) {
                if (auto const* r = tracker::find(tracker::of(x.state, x.c), x.ai.look_creature)) {
                    vec2 const b = rw::to_vec2(tracker::best_guess(*r)) * 20.f;
                    float num2 = mathf::inverse_lerp(800.f, 10.f, rw::distance(goal, b));
                    if (r->visual)
                        num2 *= 1.7f;
                    num = mathf::lerp(num, num2, (500.f - float(r->ticks_since_seen)) / 500.f);
                }
            }
            for (uint32_t j = 0; j < head_count; ++j)
                if (goal == x.h[j]->goal)
                    num = -float_max;
            return num;
        }
        // InspectorAI.HeadFindNewMoreInterestingGoal
        void head_find_new_goal(ctx& x, uint32_t i) {
            auto goals = x.state.world.inspector_data_get_head_goals(x.d);
            if (goals.size() == 0)
                return;
            for (int32_t n = 0; n < 10; ++n) {
                vec2 const v = goals.at(uint32_t(x.rng.range(0, int32_t(goals.size()))));
                bool look = false;
                if (x.ai.look_creature && creature::here(x.state, x.ai.look_creature, x.room.id)) {
                    auto const* r = tracker::find(tracker::of(x.state, x.c), x.ai.look_creature);
                    if (r && r->visual && r->rel.intensity > 0.6f) {
                        vec2 const lp = x.state.world.body_chunk_get_pos(creature::chunk_of(x.state, r->who, 0));
                        look = rw::distance(lp, x.tip(i).pos) < 60.f || rw::distance(lp, x.pos()) < 128.f;
                    }
                }
                if (look) {
                    x.h[i]->curiosity = x.ai.look_creature;
                } else if (head_goal_interest(x, i, x.h[i]->goal) < head_goal_interest(x, i, v)) {
                    x.h[i]->curiosity = dcon::creature_id{};
                    x.h[i]->goal = v;
                    break;
                }
            }
        }

        //
        // InspectorAI.Update
        //
        void ai_update(ctx& x) {
            auto& w = x.state.world;
            auto& ai = x.ai;
            auto& p = x.p;
            ivec2 const at = tile_of(x.pos());
            {
                tracker::hooks const h = make_hooks(x);
                tracker::update(x.state, x.c, x.room, at, tracker_params(), h);
            }
            if (ai.spotted) {
                reevaluate_look(x, ai.spotted, 4.f);
                ai.spotted = dcon::creature_id{};
            }
            p.anger = mathf::clamp(p.anger - 0.06f, 0.f, 1.f);
            looker_update(x);
            if (ai.attention_grabber_timer > 0.f) {
                ai.attention_grabber_timer -= 1.f;
                if (ai.attention_grabber_timer == 0.f) {
                    for (uint32_t i = 0; i < head_count; ++i)
                        if (!weaponized(x, i))
                            x.h[i]->curiosity = ai.first_time_attention_grabber;
                    ai.first_time_attention_grabber = dcon::creature_id{};
                }
            }
            if (w.inspector_data_get_head_goals(x.d).size() == 0) {
                populate_inspection_goals(x, 20);
                for (uint32_t i = 0; i < head_count; ++i)
                    head_find_new_goal(x, i);
            } else if (x.current() != behavior::inspect_area) {
                populate_head_goals(x, 10);
            }
            if (ai.head_idler <= 0) {
                head_find_new_goal(x, uint32_t(x.rng.range(0, int32_t(head_count))));
                ai.head_idler = x.rng.range(200, 500);
            }
            --ai.head_idler;
            if (x.current() != behavior::idle)
                --ai.head_idler;
            // UtilityComparer: the ThreatTracker (weight 1), PreyTracker (4),
            // RainTracker (10)
            auto const& ts = tracker::of(x.state, x.c);
            float const threat = tracker::threat_utility(ts);
            float const prey = tracker::prey_utility(x.state, x.c, x.room, at, tracker_params()) * 4.f;
            float const rain = dens::rain_utility(x.state, x.c, x.room.id) * 10.f;
            float highest = threat;
            int32_t best_module = 0;
            if (prey > highest) {
                highest = prey;
                best_module = 1;
            }
            if (rain > highest) {
                highest = rain;
                best_module = 2;
            }
            w.creature_set_current_utility(x.c, highest);
            behavior b = x.current();
            if (highest > 0.3f && best_module == 2)
                b = behavior::escape_rain;
            else if (highest > 0.1f && best_module == 1 && tracker::prey(ts))
                b = behavior::hunt;
            else if (b != behavior::idle && b != behavior::inspect_area) {
                b = behavior::idle;
                ai.new_idle_pos_counter = 1;
            }
            x.set_current(b);
            ivec2& dest = w.creature_get_destination(x.c);
            bool all_crippled = true;
            for (uint32_t i = 0; i < head_count; ++i)
                all_crippled = all_crippled && crippled(x, i);
            if (b == behavior::escape_rain || all_crippled) {
                if (auto const den = dens::home_tile(x.state, x.c, x.room))
                    dest = *den;
                return;
            }
            if (b == behavior::hunt) {
                if (auto const* pr = tracker::prey(ts)) {
                    ivec2 const g = tracker::best_guess(*pr);
                    if (reachable(x, g))
                        dest = g;
                }
                return;
            }
            if (b == behavior::idle) {
                // (the abstract AI's roaming: a trip to another room now and then)
                migrate::idle_roll(x.state, x.c, x.room);
                if (auto const t = migrate::travel_tile(x.state, x.c, x.room)) {
                    dest = *t;
                    return;
                }
                if (ai.new_idle_pos_counter < 1 && x.proximity(at) > 4 && rw::magnitude(rw::to_vec2(dest - at)) < 80.f) {
                    // inspection mode
                    w.inspector_data_get_head_goals(x.d).clear();
                    populate_inspection_goals(x, 50);
                    x.set_current(behavior::inspect_area);
                    ai.new_idle_pos_counter = x.rng.range(1400, 2500);
                } else {
                    --ai.new_idle_pos_counter;
                    if (ai.new_idle_pos_counter < 1 || !reachable(x, dest)) {
                        ivec2 const t{x.rng.range(0, x.room.width), x.rng.range(0, x.room.height)};
                        if (reachable(x, t)) {
                            dest = t;
                            ai.new_idle_pos_counter = x.rng.range(300, 2000);
                        }
                    } else if (x.proximity(dest) < 1) {
                        ivec2 const t = dest + custom::four_directions[x.rng.range(0, 4)];
                        if (reachable(x, t) && x.proximity(t) > x.proximity(dest))
                            dest = t;
                    }
                    if (rw::magnitude(rw::to_vec2(dest - at)) < 3.f && x.room.visual_contact(room_view::middle_of_tile(at), room_view::middle_of_tile(dest))) {
                        ivec2 const t = dest + ivec2{x.rng.range(-20, 21), x.rng.range(-20, 21)};
                        if (reachable(x, t) && rw::magnitude(rw::to_vec2(t - at)) >= 7.f) {
                            dest = t;
                            return;
                        }
                    }
                    // (no zap coils to keep away from)
                }
            }
            if (x.current() == behavior::inspect_area) {
                --ai.new_idle_pos_counter;
                if (ai.new_idle_pos_counter < 1) {
                    x.set_current(behavior::idle);
                    w.inspector_data_get_head_goals(x.d).clear();
                    populate_head_goals(x, 50);
                    ai.new_idle_pos_counter = 1;
                }
            }
        }

        //
        // Inspector.Act
        //
        // a weapon a head reaching for something would rather take
        void nearer_weapon(ctx& x, uint32_t l, vec2 want_pos) {
            auto& w = x.state.world;
            vec2 const tip = x.tip(l).pos;
            vec2 const body = x.pos();
            float best = rw::distance(tip, want_pos);
            dcon::item_id pick;
            rooms::for_each_item(x.state, x.room.id, [&](dcon::item_id it) {
                if (!items::is_weapon(w.item_get_type(it)) || w.item_get_mode(it) == world::weapon_mode::thrown || w.item_get_grabbed_by(it))
                    return;
                // (only what lies free: no pulling spears out of walls)
                if (w.item_get_mode(it) != world::weapon_mode::free)
                    return;
                vec2 const ip = w.body_chunk_get_pos(items::chunk(x.state, it));
                if (rw::distance(ip, body) >= 400.f || rw::distance(ip, tip) <= 10.f || rw::distance(tip, ip) >= best)
                    return;
                if (other_heads_goal(x, l, item_hold(it)) || !visual_contact(x, ip))
                    return;
                best = rw::distance(tip, ip);
                pick = it;
            });
            if (pick)
                x.h[l]->want = item_hold(pick);
        }

        void throw_weapon(ctx& x, uint32_t l, ivec2 dir) {
            auto& w = x.state.world;
            auto& hd = x.h[l]->grab;
            auto const it = hd.item;
            auto const k = items::chunk(x.state, it);
            vec2 const at = w.body_chunk_get_pos(k);
            // Weapon.Thrown(this, pos, pos - dir * 15, dir, 1): along x when
            // there is one, else along y, at 40
            vec2 const d = dir.x != 0 ? vec2{float(dir.x), 0.f} : vec2{0.f, float(dir.y)};
            items::creature_throw(x.state, it, x.c, at, at, d, 40.f);
            hd = hold{};
            x.h[l]->want = hold{};
        }

        void act(ctx& x) {
            auto& w = x.state.world;
            auto& p = x.p;
            ai_update(x);
            vec2 const body = x.pos();
            for (uint32_t l = 0; l < head_count; ++l) {
                auto& t = *x.h[l];
                auto& tip = x.tip(l);
                if (x.health(l) <= 0.f) {
                    t.limp = 1;
                    t.want = hold{};
                    t.grab = hold{};
                    for (uint32_t m = 0; m < head_chunks; ++m) {
                        t.c[m].vel *= 0.9f;
                        if (t.c[m].vel.y > -3.f)
                            t.c[m].vel.y += -0.2f * x.room.gravity;
                    }
                } else if (t.want.valid()) {
                    auto const k = hold_chunk(x, t.want);
                    if (!k) {
                        t.want = hold{};
                        t.float_grab_dest = t.goal;
                    } else {
                        vec2& wp = w.body_chunk_get_pos(k);
                        t.float_grab_dest = wp;
                        bool const weap = weaponized(x, l);
                        if (rw::distance(tip.pos, wp) < 25.f) {
                            t.grab = t.want;
                            if (t.grab.kind == hold_kind::item) {
                                // (Spear.PulledOutOfStuckObject, PickedUp,
                                // ChangeMode(Free): it only takes free ones)
                                x.state.play_sound(sound::sound_id::slugcat_pick_up_spear, x.room.id, wp);
                            }
                            // (no Vulture_Grab_Player/NPC sound ids)
                            t.want = hold{};
                            wp = tip.pos + x.head_dir(l) * 5.f;
                            w.body_chunk_get_vel(k) *= 0.f;
                            t.float_grab_dest = t.goal;
                        } else if (weap && rw::distance(body, wp) > 800.f) {
                            t.want = hold{};
                        } else if (!weap && rw::distance(tip.pos, wp) > 1300.f) {
                            t.want = hold{};
                        } else {
                            nearer_weapon(x, l, wp);
                        }
                    }
                } else if (t.grab.valid() && !hold_chunk(x, t.grab)) {
                    t.grab = hold{};
                    t.float_grab_dest = t.goal;
                } else if (t.grab.valid()) {
                    auto const k = hold_chunk(x, t.grab);
                    if (t.grab.kind == hold_kind::creature) {
                        all_grasps_let_go(x, t.grab.creature);
                    } else if (auto const by = w.item_get_grabbed_by(t.grab.item)) {
                        creature::release_grasp(x.state, by, w.item_get_grabbed_hand(t.grab.item));
                    }
                    t.want = hold{};
                    vec2& gp = w.body_chunk_get_pos(k);
                    gp = tip.pos + x.head_dir(l) * 25.f;
                    w.body_chunk_get_vel(k) *= 0.f;
                    bool const weap = weaponized(x, l);
                    if (!weap)
                        t.float_grab_dest = rw::lerp(tip.pos, body, 0.24f);
                    if (!weap && x.rng.value() < 0.1f && rw::distance(tip.pos, body) < 80.f) {
                        // let go (its chunks at the held one's speed, nought)
                        vec2 const v = rw::lerp(w.body_chunk_get_vel(k) * 0.001f, w.body_chunk_get_vel(k), p.anger);
                        if (t.grab.kind == hold_kind::creature) {
                            uint32_t const n = creature::chunk_count_of(x.state, t.grab.creature);
                            for (uint32_t m = 0; m < n; ++m)
                                w.body_chunk_set_vel(creature::chunk_of(x.state, t.grab.creature, m), v);
                        } else {
                            w.body_chunk_set_vel(k, v);
                        }
                        t.grab = hold{};
                    } else if (weap) {
                        auto const it = t.grab.item;
                        if (is_spear(w.item_get_type(it)))
                            w.item_set_rotation(it, custom::perpendicular_vector(x.head_dir(l)));
                        auto const* prey = tracker::prey(tracker::of(x.state, x.c));
                        if (prey) {
                            vec2 const v2 = rw::to_vec2(tracker::best_guess(*prey)) * 20.f;
                            // (its last seen coordinate is in this room: the
                            // tracker keeps no other)
                            vec2 const a = custom::dir_vec(gp, v2);
                            if (!t.float_grab_dest) {
                                tip.vel += a * 40.f;
                                if (rw::distance(gp, body) > 80.f && rw::distance(gp, v2) < rw::distance(v2, body)) {
                                    if (rw::magnitude(tip.vel) > 25.f) {
                                        vec2 const a2 = rw::normalized(a) * 2.f;
                                        ivec2 const dir = custom::clamp_at_one(ivec2{int32_t(a2.x), int32_t(a2.y)});
                                        if (dir.x != 0 || dir.y != 0)
                                            throw_weapon(x, l, dir);
                                    } else {
                                        t.float_grab_dest = body;
                                    }
                                }
                            } else if ((rw::distance(gp, body) > 80.f && ray_trace_tiles_for_terrain(x.room, tile_of(tip.pos), tile_of(gp))
                                           && rw::distance(gp, v2) > rw::distance(v2, body))
                                || rw::distance(gp, v2) < 10.f) {
                                t.float_grab_dest = std::nullopt;
                                for (uint32_t m = head_chunks - 5; m < head_chunks; ++m)
                                    t.c[m].vel += a * 80.f;
                            } else {
                                t.float_grab_dest = body + a * -80.f;
                            }
                        } else if (x.rng.value() < 0.01f) {
                            t.grab = hold{};
                        }
                    }
                } else if (t.curiosity && creature::here(x.state, t.curiosity, x.room.id)) {
                    if (creature::is(w, t.curiosity, creature::type::inspector)) {
                        // the other inspector's same head's tip
                        auto const oh = head_of(x.state, t.curiosity, l);
                        vec2 const ot = w.inspector_head_get_t(oh).c[head_chunks - 1].pos;
                        t.float_grab_dest = rw::lerp(tip.pos, ot, 0.25f);
                    } else {
                        t.float_grab_dest = rw::lerp(body, w.body_chunk_get_pos(creature::chunk_of(x.state, t.curiosity, 0)), 0.8f);
                    }
                } else {
                    t.float_grab_dest = t.goal;
                }
            }
            // the path (StandardPather.FollowPath)
            ivec2 const here = tile_of(body);
            ivec2 const dest = w.creature_get_destination(x.c);
            ivec2 step = here;
            if (x.room.inside(dest.x, dest.y) && pathing::accessible(x.room, dest, movement_kind)) {
                auto const f = pathing::field_to(x.state, x.room.id, dest, movement_kind);
                if (auto const s = pathing::next_step(x.state, f, here))
                    step = *s;
            }
            if (creature::held(x.state, x.c)) {
                x.vel() += vec2{mathf::lerp(-1.f, 2.f, x.rng.value()) / 2.f, mathf::lerp(-1.f, 2.f, x.rng.value()) / 2.f};
            } else {
                p.goal_pos = rw::to_vec2(step) * 20.f;
                vec2 v3 = custom::dir_vec(body, p.goal_pos) * 0.3f;
                for (uint32_t i = 0; i < head_count; ++i)
                    v3 = rw::lerp(v3, custom::dir_vec(body, x.tip(i).pos), mathf::inverse_lerp(100.f, 10.f, rw::distance(x.tip(i).pos, p.goal_pos)));
                float const t = mathf::inverse_lerp(2.f, 16.f, float(p.wing_number));
                if (rw::distance(body, p.goal_pos) > 15.f) {
                    p.flying_power = rw::lerp(p.flying_power, v3, t);
                } else if (rw::magnitude(p.flying_power) > 1.f) {
                    p.flying_power *= 0.8f;
                } else {
                    p.flying_power *= 0.f;
                    x.vel() = move_towards(x.vel(), rw::normalized(x.vel()), 0.24f);
                }
            }
            physics::set_go_through_floors(x.state, creature::body(x.state, x.c), step.y < here.y);
            // (squeezing through pipes: dens.cpp pulls it in; no zap coils)
            p.squeeze = 0;
            vec2 b{0.f, 0.f};
            if (x.room.gravity > 0.f)
                b.y += x.room.gravity * 0.9f;
            float const dd = 1.f + mathf::clamp(p.anger, 0.f, 2.f) * 3.f + dens::rain_utility(x.state, x.c, x.room.id);
            x.vel() += p.flying_power * dd / 30.f + b;
        }

        // Creature.Update's HealthState: hurt, it may die, else heals
        void health_update(sys::game_state& state, dcon::creature_id c, rw::rng& rng) {
            auto& w = state.world;
            if (w.creature_get_dead(c))
                return;
            float& health = w.creature_get_health(c);
            if (health < 0.f && rng.value() < -health && rng.value() < 1.f / float(creature::consious(state, c) ? 80 : 800))
                creature::die(state, c);
            if (rng.value() < 0.033333335f && (rng.value() * 0.2f > health || rng.value() < -health))
                creature::stun(state, c, rng.range(1, rng.range(1, 27 - custom::int_clamp(int32_t(20.f * health), 0, 10))));
            if (health > 0.f && health < 1.f && rng.value() < 0.01f)
                health = mathf::min(1.f, health + 1.f / mathf::lerp(550.f, 70.f, health));
        }

        // Inspector.NewRoom (the lights, neurons and projection circles
        // aside) with InspectorAI.NewRoom: the heads start over
        void new_room(ctx& x) {
            for (uint32_t i = 0; i < head_count; ++i) {
                tent tw(x, i);
                tentacle_new_room(tw);
                tw.store();
            }
            tracker::clear(x.state, x.c);
            x.ai.look_creature = dcon::creature_id{};
            x.ai.spotted = dcon::creature_id{};
            // (the old room's head goals are dropped: vanilla keeps them)
            x.state.world.inspector_data_get_head_goals(x.d).clear();
        }

        // InspectorState / InitiateGraphicsModule's ownerIterator
        int8_t owner_of(sys::game_state const& state, dcon::room_id room) {
            auto const r = room ? state.world.room_get_region_from_region_membership(room) : dcon::region_id{};
            if (!r)
                return -1;
            auto const n = state.to_string_view(state.world.region_get_name(r));
            if (n == "SS")
                return 0;
            if (n == "MS" || n == "DM")
                return 1;
            return -1;
        }
    }

    dcon::inspector_head_id head_of(sys::game_state const& state, dcon::creature_id c, uint32_t i) noexcept {
        return head_data(state.world, creature::part(state.world, c, i));
    }

    float rad(float f) noexcept {
        if (f < 0.5f)
            return custom::lerp_map(f, 0.f, 0.14f, 5.8f, 0.01f);
        return custom::lerp_map(f, 0.85f, 1.f, 0.01f, 2.6f);
    }

    vec3 true_color(sys::game_state const& state, dcon::creature_id c) noexcept {
        auto const& p = pl(state, c);
        vec3 col{0.44705883f, 0.9019608f, 0.76862746f};
        if (p.owner_iterator == 1)
            col = vec3{1.f, 0.8f, 0.3f};
        else if (p.owner_iterator == 2)
            col = vec3{0.f, 1.f, 0.f};
        else if (p.owner_iterator == 3)
            col = vec3{1.f, 0.2f, 0.f};
        vec3 hsl = custom::rgb2hsl(col);
        hsl.y = 0.9f;
        rw::unity_random r{int32_t(uint32_t(state.world.inspector_data_get_seed(data_of(state.world, c))) + 20u)};
        hsl.x += r.value() / 20.f;
        return custom::hsl2rgb(hsl.x, hsl.y, hsl.z);
    }

    vec3 body_color(sys::game_state const& state, dcon::creature_id c, float roll, float* alpha) noexcept {
        auto const& p = pl(state, c);
        if (alpha)
            *alpha = 1.f;
        vec3 const tc = true_color(state, c);
        vec3 const red{1.f, 0.f, 0.f};
        if (state.world.creature_get_stun(c) <= 0 && !creature::held(state, c))
            return tc + (red - tc) * mathf::clamp01(p.anger * 0.78f);
        if (roll > 0.7f) {
            if (alpha)
                *alpha = 0.f;
            return vec3{0.f, 0.f, 0.f};
        }
        return tc + (red - tc) * 0.86f;
    }

    float attention_flicker(sys::game_state const& state, dcon::creature_id c) noexcept {
        float const t = state.world.inspector_data_get_ai(data_of(state.world, c)).attention_grabber_timer;
        return 6.f + std::sin(t * 4.f) * 6.f * t / float(attention_delay_max);
    }

    float glow_mix(sys::game_state const& state, dcon::creature_id c) noexcept {
        auto const& ai = state.world.inspector_data_get_ai(data_of(state.world, c));
        float const t = ai.attention_grabber_timer;
        float num4 = (1.f + std::sin(t / 2.f) / 2.f) * t / (float(attention_delay_max) * 2.f);
        if (behavior(state.world.creature_get_behavior(c)) == behavior::inspect_area) {
            float const b = mathf::max(0.15f, (0.25f + std::sin(float(ai.new_idle_pos_counter)) / 5.f) * (float(ai.new_idle_pos_counter) / 2500.f));
            num4 = mathf::max(num4, b);
        }
        if (!creature::consious(state, c))
            num4 = 0.f;
        return num4;
    }

    bool head_weaponized(sys::game_state const& state, dcon::creature_id c, uint32_t i) noexcept {
        auto const& w = state.world;
        auto const part = creature::part(w, c, i);
        if (w.creature_part_get_health(part) <= 0.f)
            return false;
        auto const& t = w.inspector_head_get_t(head_data(w, part));
        auto const weapon = [&](hold const& hd) { return hd.kind == hold_kind::item && w.item_is_valid(hd.item) && items::is_weapon(w.item_get_type(hd.item)); };
        return weapon(t.want) || weapon(t.grab);
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::inspector);
        auto const d = data_of(w, c);
        auto rng = rw::random(state.seed, 0, uint32_t(c.value));
        int32_t seed = int32_t(rng.next() & 0x7FFFFFFFu);
        if (state.spawn_seed >= 0)
            seed = state.spawn_seed;
        physics::object_desc desc;
        desc.gravity = 0.9f;
        desc.air_friction = 0.99f;
        desc.bounce = 0.3f;
        desc.surface_friction = 0.4f;
        desc.water_friction = 0.92f;
        desc.buoyancy = 0.01f;
        auto const o = physics::create_object(state, room, desc);
        // never inside terrain
        auto const view = world::get_room_view(state, room);
        if (view.get_tile(at).solid()) {
            ivec2 const t0 = tile_of(at);
            int best = 1 << 30;
            for (int dy = -8; dy <= 8; ++dy)
                for (int dx = -8; dx <= 8; ++dx) {
                    ivec2 const t = t0 + ivec2{dx, dy};
                    if (view.inside(t.x, t.y) && !view.get_tile(t).solid() && dx * dx + dy * dy < best) {
                        best = dx * dx + dy * dy;
                        at = room_view::middle_of_tile(t);
                    }
                }
        }
        // BodyChunk(this, 0, .., 2.6f, 2.15f): radius 2.6, mass 2.15
        physics::add_chunk(state, o, at, 2.6f, 2.15f);
        w.try_create_creature_body(c, o);
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_set_grasps(c, creature::grasp_pair{});
        w.creature_set_behavior(c, uint8_t(behavior::idle));
        w.creature_set_current_utility(c, 0.f);
        w.creature_set_destination(c, tile_of(at));
        auto& p = pl(state, c);
        uint16_t const keep = p.data;
        p = body_payload{};
        p.data = keep;
        p.squeeze_fac = 1.f;
        p.goal_pos = at;
        p.active_eye = -1;
        p.owner_iterator = owner_of(state, room);
        {
            // InspectorState: Wingnumber 3 + floor(SeededRandom(seed) * 8)
            rw::unity_random r{seed};
            p.wing_number = uint8_t(std::min<int32_t>(3 + int32_t(std::floor(r.value() * 8.f)), int32_t(max_wings)));
        }
        w.inspector_data_set_seed(d, seed);
        w.inspector_data_set_ai(d, ai_state{});
        w.inspector_data_get_head_goals(d).clear();
        w.inspector_data_set_graphics(d, graphics_state{});
        w.inspector_data_get_mycelium_points(d).clear();
        // the heads: TentacleChunk(i, (i + 1) / 8, Lerp(3, 5, i / 7))
        for (uint32_t i = 0; i < head_count; ++i) {
            auto const part = creature::add_part(state, c, creature::part_kind::inspector_head, uint8_t(i));
            auto const hd = w.create_inspector_head();
            w.creature_part_get_payload(part).inspector_head = head_payload{uint16_t(hd.index())};
            w.creature_part_set_health(part, head_health);
            head t;
            for (uint32_t j = 0; j < head_chunks; ++j) {
                t.c[j] = head_chunk{};
                t.c[j].t_pos = float(j + 1) / float(head_chunks);
                t.c[j].rad = mathf::lerp(3.f, 5.f, float(j) / float(head_chunks - 1));
            }
            t.goal = at;
            w.inspector_head_set_t(hd, t);
            w.inspector_head_get_segments(hd).clear();
            w.inspector_head_get_grab_path(hd).clear();
            w.inspector_head_set_graphics(hd, head_graphics{});
        }
        tracker::clear(state, c);
        {
            ctx x(state, c, view, 2);
            new_room(x);
        }
        inspector_graphics::reset_positions(state, c);
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        creature::release_target(state, c);
        creature::erase(state, c);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        ctx x(state, c, room, 0);
        auto& p = x.p;
        health_update(state, c, x.rng);
        // Creature.Update + PhysicalObject.Update
        creature::stun_countdown(state, c);
        physics::update_object(state, room, creature::body(state, c), nullptr, nullptr);
        if (p.squeeze || w.creature_get_entering_shortcut(c).x >= 0)
            p.squeeze_fac = mathf::min(1.f, p.squeeze_fac + 0.02f);
        else
            p.squeeze_fac = mathf::max(0.f, p.squeeze_fac - 0.033333335f);
        if (p.squeeze_fac > 0.8f) {
            float const f = custom::lerp_map(p.squeeze_fac, 0.8f, 1.f, 0.f, 0.5f);
            for (uint32_t i = 0; i < head_count; ++i)
                for (uint32_t j = 0; j < head_chunks; ++j)
                    x.h[i]->c[j].pos = rw::lerp(x.h[i]->c[j].pos, x.pos(), f);
        }
        p.lightpulse += 0.03f;
        bool const awake = x.consious();
        for (uint32_t k = 0; k < head_count; ++k) {
            tent tw(x, k);
            tentacle_update(tw);
            tw.store();
            x.h[k]->limp = awake ? 0 : 1;
            // InspectorGraphics' blinks (here with the body: the active eye
            // reads them)
            float& bl = x.h[k]->blink;
            if (bl > 0.f) {
                bl += 0.1f;
                if (bl > 1.f)
                    bl = 0.f;
            } else if (x.rng.value() < 0.01f) {
                bl = 0.01f;
            }
        }
        p.last_dying = p.dying;
        if (x.dead()) {
            p.squeeze_fac = 0.2f;
            x.vel() += custom::rnv(x.rng) * x.rng.value() * 4.f * p.dying;
            x.pos() += custom::rnv(x.rng) * x.rng.value() * 4.f * p.dying;
            if (p.last_dying == 0.f) {
                // (no Overseer_Death sound; the ShockWaves as flashes, no
                // OverseerCarcass eyes left behind)
                vec3 const col = body_color(state, c, 0.f);
                for (uint32_t l = 0; l < head_count; ++l) {
                    uint32_t const n = uint32_t(x.rng.range(0, int32_t(head_chunks)));
                    particles::spawn(state, room.id, particles::kind::flash, x.h[l]->c[n].pos, vec2{0.f, 0.f}, float(x.rng.range(40, 160)), 0.12f, col);
                }
            }
            p.dying = mathf::min(1.f, p.dying + 0.16666667f);
            if (p.dying >= 1.f && !p.destroy_queued) {
                p.destroy_queued = 1;
                state.defer(room.id, [c](sys::game_state& st) {
                    if (st.world.creature_is_valid(c) && creature::is(st.world, c, creature::type::inspector))
                        creature::destroy(st, c);
                });
            }
            return;
        }
        // (no NoLeviathanStrandingZones)
        if (awake) {
            if (x.rng.value() > 0.6f) {
                p.active_eye = int8_t(x.rng.range(0, int32_t(head_count)));
                if (x.h[uint32_t(p.active_eye)]->blink > 0.f || crippled(x, uint32_t(p.active_eye)))
                    p.active_eye = -1;
            }
            act(x);
            return;
        }
        for (uint32_t i = 0; i < head_count; ++i) {
            x.h[i]->grab = hold{};
            x.h[i]->want = hold{};
        }
        p.active_eye = -1;
    }

    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) {
        auto const& w = state.world;
        for (uint32_t i = 0; i < head_count; ++i) {
            auto const& h = w.inspector_head_get_t(head_of(state, c, i));
            if (h.grab.kind == hold_kind::creature && h.grab.creature == t)
                return false;
        }
        return true;
    }

    void collide(sys::game_state& state, dcon::creature_id c, room_view const& room, uint32_t, dcon::creature_id t, uint32_t) {
        if (state.world.creature_get_dead(c) || !creature::live(state, t))
            return;
        ctx x(state, c, room, 3);
        tracker::hooks const h = make_hooks(x);
        tracker::see(state, c, room, t, h);
        if (x.p.active_eye != -1)
            x.h[uint32_t(x.p.active_eye)]->curiosity = t;
    }

    void violence(sys::game_state& state, dcon::creature_id c, creature::hit_part const& part, vec2 momentum, float damage, float stun_bonus,
        creature::damage_type type) {
        auto& w = state.world;
        if (type == creature::damage_type::electric)
            return;
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        auto const room = creature::room_of(state, c);
        vec2 const at = w.body_chunk_get_pos(creature::chunk(state, c, 0));
        // the source (rwcpp's Violence carries none): a bite is the closest
        // creature's, anything else a weapon the closest slugcat threw
        bool const bite = type == creature::damage_type::bite;
        bool const weapon = type == creature::damage_type::stab || type == creature::damage_type::blunt;
        if (bite || weapon) {
            dcon::creature_id src;
            float best = float_max;
            if (room)
                rooms::for_each_creature(state, room, [&](dcon::creature_id o) {
                    if (o == c || w.creature_get_away(o) != 0 || (weapon && !creature::is_slugcat(w, o)))
                        return;
                    float const dst = rw::distance(creature::main_pos(state, o), at);
                    if (dst < best) {
                        best = dst;
                        src = o;
                    }
                });
            if (bite)
                momentum = vec2{0.f, 0.f};
            // PreyTracker.AddPrey(RepresentationForCreature(source)): marked,
            // turned to Eats by the next relationship update
            if (src) {
                auto& ts = tracker::of(state, c);
                auto* r = tracker::find(ts, src);
                if (!r && room) {
                    auto const view = world::get_room_view(state, room);
                    ctx x(state, c, view, 4);
                    tracker::hooks const h = make_hooks(x);
                    tracker::see(state, c, view, src, h);
                    r = tracker::find(ts, src);
                }
                if (r)
                    r->state |= 2;
            }
            if (part.tentacle >= 0 && uint32_t(part.tentacle) < head_count) {
                auto const hp = creature::part(w, c, uint32_t(part.tentacle));
                w.creature_part_set_health(hp, w.creature_part_get_health(hp) - damage);
                damage = 0.12f;
            } else if (rng.value() < 0.001f) {
                damage = 100.f;
            } else {
                damage = 0.f;
            }
        }
        w.creature_set_behavior(c, uint8_t(behavior::idle));
        // Creature.Violence (HealthState)
        auto const hit = creature::chunk(state, c, 0);
        w.body_chunk_get_vel(hit) += rw::clamp_magnitude(momentum / w.body_chunk_get_mass(hit), 10.f);
        float const dmg = damage / base_damage_resistance;
        float st = (damage * 30.f + stun_bonus) / base_stun_resistance;
        float& health = w.creature_get_health(c);
        st *= 1.5f + mathf::inverse_lerp(0.5f, 0.f, health) * rng.value();
        creature::stun(state, c, int32_t(st));
        health -= dmg;
        if (rng.value() < -health || health < -1.f || (health < 0.f && rng.value() < 0.33f))
            creature::die(state, c);
    }

    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t) {
        auto& w = state.world;
        for (uint32_t i = 0; i < head_count; ++i) {
            auto& h = w.inspector_head_get_t(head_of(state, c, i));
            if (h.grab.kind == hold_kind::creature && h.grab.creature == t)
                h.grab = hold{};
            if (h.want.kind == hold_kind::creature && h.want.creature == t)
                h.want = hold{};
            if (h.curiosity == t)
                h.curiosity = dcon::creature_id{};
        }
        auto& ai = w.inspector_data_get_ai(data_of(w, c));
        if (ai.first_time_attention_grabber == t)
            ai.first_time_attention_grabber = dcon::creature_id{};
        if (ai.look_creature == t)
            ai.look_creature = dcon::creature_id{};
        if (ai.spotted == t)
            ai.spotted = dcon::creature_id{};
    }

    void died(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        for (uint32_t i = 0; i < head_count; ++i) {
            auto& h = w.inspector_head_get_t(head_of(state, c, i));
            h.grab = hold{};
            h.want = hold{};
        }
    }

    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const view = world::get_room_view(state, room);
        ivec2 const t = tile_of(at);
        vec2 const dir = rw::to_vec2(view.shortcut_entrance_hole_direction(t));
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        // Creature.SpitOutOfShortCut: out of the hole's middle
        auto const k = creature::chunk(state, c, 0);
        physics::hard_set_position(state, k, room_view::middle_of_tile(t) + custom::rnv(rng));
        w.body_chunk_set_last_pos(k, w.body_chunk_get_pos(k));
        w.body_chunk_set_vel(k, dir);
        w.creature_set_shortcut_delay(c, 80);
        ctx x(state, c, view, 5);
        x.p.squeeze_fac = 1.f;
        x.p.owner_iterator = owner_of(state, room);
        new_room(x);
        for (uint32_t i = 0; i < head_count; ++i) {
            x.h[i]->grab = hold{};
            x.h[i]->want = hold{};
            tent tw(x, i);
            tentacle_reset(tw, x.pos());
            tw.store();
        }
        inspector_graphics::reset_positions(state, c);
    }

    std::string describe(sys::game_state const& state, dcon::creature_id c) {
        static constexpr char const* names[] = {"idle", "hunt", "escape rain", "inspect area"};
        auto const& w = state.world;
        auto const& p = pl(state, c);
        vec2 const at = w.body_chunk_get_pos(creature::chunk(state, c, 0));
        uint32_t const b = w.creature_get_behavior(c);
        int heads = 0, holding = 0;
        for (uint32_t i = 0; i < head_count; ++i) {
            auto const part = creature::part(w, c, i);
            if (w.creature_part_get_health(part) > 0.f)
                ++heads;
            if (w.inspector_head_get_t(head_data(w, part)).grab.valid())
                ++holding;
        }
        return "inspector at " + std::to_string(int(at.x)) + "," + std::to_string(int(at.y)) + " behavior " + (b < 4 ? names[b] : "?") + " anger "
            + std::to_string(p.anger) + " heads " + std::to_string(heads) + "/3" + (holding ? " holding " + std::to_string(holding) : std::string{});
    }

    bool spawn_spot(room_view const& room, ivec2 tile) {
        if (!room.inside(tile.x, tile.y) || room.get_tile(tile).solid() || room.get_tile(tile).water != 0)
            return false;
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx)
                if (room.get_tile(tile + ivec2{dx, dy}).solid())
                    return false;
        return true;
    }

    bool trace_appendages(sys::game_state const& state, dcon::room_id room, vec2 from, vec2 to, dcon::creature_id thrown_by, dcon::creature_id& hit,
        creature::hit_part& part, float& at) {
        auto const& w = state.world;
        float best = 2.f;
        vec2 const r = to - from;
        rooms::for_each_creature(state, room, creature::type::inspector, [&](dcon::creature_id c) {
            if (c == thrown_by || w.creature_get_away(c) != 0)
                return;
            vec2 const body = w.body_chunk_get_pos(creature::chunk(state, c, 0));
            for (uint32_t app = 0; app < head_count; ++app) {
                auto const& t = w.inspector_head_get_t(head_of(state, c, app));
                // AppendagePosition: the body, then tChunks[1..7]
                vec2 pts[head_chunks];
                pts[0] = body;
                for (uint32_t s = 1; s < head_chunks; ++s)
                    pts[s] = t.c[s].pos;
                for (uint32_t s = 0; s + 1 < head_chunks; ++s) {
                    // the closest approach of the two segments (sampled along
                    // the projectile's, exact along the head's)
                    vec2 const q = pts[s + 1] - pts[s];
                    float const qq = rw::sqr_magnitude(q);
                    constexpr int samples = 8;
                    for (int k = 0; k <= samples; ++k) {
                        float const sa = float(k) / float(samples);
                        if (sa >= best)
                            break;
                        vec2 const pp = from + r * sa;
                        float const sb = qq > 0.f ? mathf::clamp01(rw::dot(pp - pts[s], q) / qq) : 0.f;
                        float const rr = mathf::lerp(3.f, 5.f, float(s) / float(head_chunks - 1)) + 2.f;
                        if (custom::dist_less(pp, pts[s] + q * sb, rr)) {
                            best = sa;
                            hit = c;
                            part = creature::hit_part{-1, int32_t(app), int32_t(s), sb};
                            break;
                        }
                    }
                }
            }
        });
        at = best;
        return best <= 1.f;
    }
}
