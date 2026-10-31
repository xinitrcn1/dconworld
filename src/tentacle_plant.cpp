// TentaclePlant.cs and TentaclePlantAI.cs: the monster kelp. Two body chunks
// (0 the tip, 1 the root pinned at rootPos, in front of the den it grows
// out of) and one Tentacle (Tentacle.cs, mirrored from daddy_tentacle.cpp's
// port with the plant's TentacleProps: a rope tentacle that shortens to its
// grab path, its chunks drawn to the grab destination). It sways about an
// idle point, sees what moves near it, shakes and lashes out at it, grabs
// what its tip touches during the lash and pulls it into its den to eat.
// A hit makes it let go; hurt or threatened it draws back into its den.

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <vector>

#include "tentacle_plant.hpp"
#include "daddy_internal.hpp"
#include "dens.hpp"
#include "items.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "rooms.hpp"
#include "rot.hpp"
#include "tracker.hpp"

namespace tentacle_plant {
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

        // TentaclePlant's Tentacle.TentacleProps(false, true, true, 0.5f, 0f,
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

        // the template (StaticWorld): baseDamageResistance, baseStunResistance
        constexpr float base_damage_resistance = 2.5f;
        constexpr float base_stun_resistance = 0.8f;
        // visualRadius (movementBasedVision 1: it sees only what moves)
        constexpr float visual_radius = 400.f;

        creature::tentacle_plant_payload& pl(sys::game_state& state, dcon::creature_id c) { return state.world.creature_get_payload(c).tentacle_plant; }
        creature::tentacle_plant_payload const& pl(sys::game_state const& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).tentacle_plant;
        }
        dcon::tentacle_plant_data_id data(sys::game_state const& state, dcon::creature_id c) { return creature::tentacle_plant_data(state.world, c); }

        // the tile lists while the plant updates (they grow and shrink every
        // tick; back into the vector pools after), per thread
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

        // the plant while it updates
        struct ctx {
            sys::game_state& state;
            dcon::creature_id c;
            room_view const& room;
            rw::rng& rng;
            dcon::tentacle_plant_data_id d;
            creature::tentacle_plant_payload& p;
            tentacle& t;
            tentacle_chunk* ch;
            std::vector<ivec2>& segments;
            std::vector<ivec2>& grab_path;
            std::vector<ivec2>& scratch;
            dcon::body_chunk_id b[2];
            std::vector<uint16_t> const* prox;

            ctx(sys::game_state& s, dcon::creature_id id, room_view const& r)
                : state(s), c(id), room(r), rng(s.world.creature_get_random(id)), d(data(s, id)), p(pl(s, id)), t(s.world.tentacle_plant_data_get_tentacle(d)),
                  ch(s.world.tentacle_plant_data_get_chunks(d).c), segments(segments_buffer()), grab_path(grab_path_buffer()), scratch(scratch_buffer()),
                  prox(&pathing::ai_terrain_proximity_map(s, r.id)) {
                b[0] = creature::chunk(s, id, 0);
                b[1] = creature::chunk(s, id, 1);
                auto const segs = s.world.tentacle_plant_data_get_segments(d);
                segments.assign(segs.begin(), segs.end());
                auto const gp = s.world.tentacle_plant_data_get_grab_path(d);
                grab_path.assign(gp.begin(), gp.end());
            }
            void store() {
                auto& w = state.world;
                w.tentacle_plant_data_get_segments(d).load_range(segments.data(), segments.data() + segments.size());
                w.tentacle_plant_data_get_grab_path(d).load_range(grab_path.data(), grab_path.data() + grab_path.size());
            }

            vec2& pos(uint32_t i) { return state.world.body_chunk_get_pos(b[i]); }
            vec2& vel(uint32_t i) { return state.world.body_chunk_get_vel(b[i]); }
            tentacle_chunk& tip() { return ch[chunk_count - 1]; }
            bool dead() const { return state.world.creature_get_dead(c) != 0; }
            bool consious() const { return creature::consious(state, c); }
            // AImap.getTerrainProximity, -1 outside the room
            int32_t proximity(ivec2 q) const {
                if (!room.inside(q.x, q.y))
                    return -1;
                return (*prox)[size_t(q.y) * size_t(room.width) + size_t(q.x)];
            }
            int32_t proximity(vec2 q) const { return proximity(room_view::get_tile_position(q)); }
        };

        bool has_any_solid(room_view const& room, ivec2 q) { return room.get_tile(q).solid(); }
        float float_dist(ivec2 a, ivec2 b) { return rw::magnitude(rw::to_vec2(a - b)); }
        bool inside(room_view const& room, ivec2 q) { return room.inside(q.x, q.y); }

        // Custom.AreIntVectorsNeighbors / AreIntVectorsDiagonalNeighbors (sic)
        bool are_neighbors(ivec2 a, ivec2 b) {
            return a != b && (a.x == b.x || a.y == b.y) && std::abs(a.x - b.x) < 2 && std::abs(a.y - b.y) < 2;
        }
        bool are_diagonal_neighbors(ivec2 a, ivec2 b) {
            return a.x != b.x && a.y != b.y && (std::abs(a.x - b.x) < 2 || std::abs(a.y - b.y) < 2);
        }

        //
        // SharedPhysics.TerrainCollisionData: VerticalCollision,
        // SlopesVertically, HorizontalCollision (as daddy_tentacle.cpp's)
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
        vec2 float_base(ctx& x) { return x.pos(1); }

        // Tentacle.grabDest
        std::optional<ivec2> grab_dest(ctx const& x) {
            if (x.t.float_grab_dest)
                return room_view::get_tile_position(*x.t.float_grab_dest);
            return std::nullopt;
        }

        // Tentacle.CheckIfAnySegmentsLeft, BasePos
        ivec2 base_pos(ctx& x) {
            if (x.segments.empty())
                x.segments.push_back(room_view::get_tile_position(float_base(x)));
            return x.segments[0];
        }

        // TentacleChunk.currentSegment / StuckPos
        int32_t current_segment(ctx const& x, tentacle_chunk const& c) {
            int32_t const n = int32_t(x.segments.size());
            return custom::int_clamp(int32_t(c.t_pos * float(n - 1)), 0, n - 1);
        }
        ivec2 stuck_pos(ctx const& x, tentacle_chunk const& c) { return x.segments[size_t(current_segment(x, c))]; }

        // Tentacle.Visual (with daddy_tentacle.cpp's shortcut: nothing can be
        // in the way within the terrain proximity of `a`)
        bool visual(ctx& x, ivec2 a, ivec2 b) {
            int32_t const p = x.proximity(a);
            if (p > 0 && x.room.inside(b.x, b.y) && std::abs(b.x - a.x) + std::abs(b.y - a.y) < p)
                return true;
            return ray_trace_tiles_for_terrain(x.room, a, b);
        }

        // Rope.Update (a rope without bends, far from the terrain, only
        // takes its new ends)
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
        bool rope_active(ctx const& x, uint32_t i) {
            auto const& c = x.ch[i];
            return c.phase == -1.f && (i == 0 || x.ch[i - 1].phase == -1.f) && (x.t.backtrack_from < int32_t(i) || x.t.backtrack_from == -1);
        }

        // TentacleChunk.Reset
        void chunk_reset(ctx& x, tentacle_chunk& c) {
            c.trail = stuck_pos(x, c);
            c.phase = -1.f;
            c.pos = room_view::middle_of_tile(stuck_pos(x, c));
            c.last_pos = c.pos;
            c.vel = vec2{0.f, 0.f};
            // new Rope(room, FloatBase, FloatBase + (1, 1), 1f)
            vec2 const b = float_base(x);
            world::rope_state r;
            r.a = r.last_a = b;
            r.b = r.last_b = b + vec2{1.f, 1.f};
            r.total_length = rw::distance(r.a, r.b);
            r.thickness = 1.f;
            c.rope = r;
        }

        // TentacleChunk.Update
        void chunk_update(ctx& x, uint32_t i) {
            auto& c = x.ch[i];
            auto& t = x.t;
            float const n = float(chunk_count);
            c.last_pos = c.pos;
            update_rope(x, c.rope, i == 0 ? float_base(x) : x.ch[i - 1].pos, c.pos);
            if (!rope_active(x, i) || c.phase > -1.f || (i > 0 && x.ch[i - 1].phase > -1.f) || c.rope.total_length > ideal_length / n * 5.f)
                c.rope.bend_count = 0;
            if (c.phase == -1.f) {
                if (c.phases_to_same_location > 0 && x.segments[size_t(current_segment(x, c))] == c.after_phase_stuck_pos) {
                    vec2 const m = room_view::middle_of_tile(stuck_pos(x, c));
                    c.pos = vec2{mathf::clamp(c.pos.x, m.x - 9.f, m.x + 9.f), mathf::clamp(c.pos.y, m.y - 9.f, m.y + 9.f)};
                }
                float const ideal = ideal_length / n * mathf::lerp(1.f, 0.1f, t.retract_fac);
                if (i == 0) {
                    vec2 a = custom::dir_vec(c.pos, float_base(x));
                    float num = rw::distance(c.pos, float_base(x));
                    if (rope_active(x, i)) {
                        a = custom::dir_vec(c.pos, rot::rope_b_connect(c.rope));
                        num = c.rope.total_length;
                    }
                    if (num > ideal) {
                        // (pullAtConnectionChunk is 0: the root isn't pulled)
                        c.pos -= a * (ideal - num);
                        c.vel -= a * (ideal - num);
                    }
                    c.stretched_fac = ideal_length / n / mathf::max(1.f, num);
                } else {
                    auto& prev = x.ch[i - 1];
                    vec2 a = custom::dir_vec(c.pos, prev.pos);
                    float num = rw::distance(c.pos, prev.pos);
                    if (rope_active(x, i)) {
                        a = custom::dir_vec(c.pos, rot::rope_b_connect(c.rope));
                        num = c.rope.total_length;
                    }
                    if (num > ideal) {
                        c.pos -= a * (ideal - num) * (1.f - mass_deterioration_per_chunk);
                        c.vel -= a * (ideal - num) * (1.f - mass_deterioration_per_chunk);
                        if (rope_active(x, i))
                            a = custom::dir_vec(rot::rope_a_connect(c.rope), prev.pos);
                        prev.pos += a * (ideal - num) * mass_deterioration_per_chunk;
                        prev.vel += a * (ideal - num) * mass_deterioration_per_chunk;
                    }
                    c.stretched_fac = ideal_length / n / mathf::max(1.f, num);
                }
                if (stuck_pos(x, c) != c.trail)
                    c.trail = stuck_pos(x, c);
                ivec2 const here = room_view::get_tile_position(c.pos);
                if (t.backtrack_from == -1 && !visual(x, here, x.segments[size_t(current_segment(x, c))]))
                    t.backtrack_from = int32_t(i);
                if (t.backtrack_from != -1 && int32_t(i) >= t.backtrack_from) {
                    bool flag = false;
                    int32_t const cs = current_segment(x, c);
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
                        vec2 const fb = i > 0 ? x.ch[i - 1].pos : float_base(x);
                        c.vel += rw::clamp_magnitude(fb - c.pos, 20.f) * backtrack_speed / 20.f;
                        if (i > 0)
                            x.ch[i - 1].vel += rw::clamp_magnitude(fb - c.pos, 20.f) * backtrack_speed / 20.f;
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
                    if (t.limp) {
                        for (int32_t k = current_segment(x, c); k >= 0; --k) {
                            ivec2 const s = x.segments[size_t(k)];
                            if (visual(x, here, s)) {
                                c.vel += rw::clamp_magnitude(room_view::middle_of_tile(s) - c.pos, 20.f) * 0.005f;
                                break;
                            }
                        }
                    } else {
                        if (t.float_grab_dest) {
                            float const s = i == chunk_count - 1 ? goal_attraction_speed_tip : goal_attraction_speed;
                            c.vel += rw::clamp_magnitude(*t.float_grab_dest - c.pos, 20.f) * s / 20.f;
                        }
                        c.vel += rw::clamp_magnitude(room_view::middle_of_tile(x.segments[size_t(current_segment(x, c))]) - c.pos, 20.f) * align_to_segment_speed / 20.f;
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
                    float const ideal = ideal_length / n * mathf::lerp(1.f, 0.1f, t.retract_fac);
                    if (num > ideal) {
                        prev.pos += a * (ideal - num) * mass_deterioration_per_chunk;
                        prev.vel += a * (ideal - num) * mass_deterioration_per_chunk;
                    }
                    c.stretched_fac = ideal / mathf::max(num, 0.0001f);
                }
            }
        }

        // Tentacle.MoveSegment
        bool move_segment(ctx& x, int32_t s, ivec2 dest, bool allow_occupied, bool allow_solid) {
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
        void shorten(ctx& x, int32_t from, int32_t to, std::vector<ivec2> const& straight, int32_t count) {
            x.segments.erase(x.segments.begin() + from + 1, x.segments.begin() + to);
            for (int32_t i = count - 2; i > 0; --i)
                x.segments.insert(x.segments.begin() + from + 1, straight[size_t(i)]);
        }

        // Tentacle.PullAtTentacle
        void pull_at_tentacle(ctx& x, int32_t start, bool only_simple_pull, std::vector<ivec2>& path) {
            auto& t = x.t;
            auto& seg = x.segments;
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

        // Tentacle.FindTentaclePath (daddy_tentacle.cpp's: a heap on
        // heuristic and insertion order)
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
                for (auto const d : custom::four_directions) {
                    ivec2 const q = p + d;
                    if (q.x < 0 || q.x >= w || q.y < 0 || q.y >= h)
                        continue;
                    uint32_t& v = visited[size_t(q.x) * size_t(h) + size_t(q.y)];
                    if (v == stamp)
                        continue;
                    v = stamp;
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

        // Tentacle.MoveAlignedSegmentsWithPath
        void move_aligned_segments_with_path(ctx& x, std::vector<ivec2>& path) {
            auto& seg = x.segments;
            auto& gp = x.grab_path;
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
            x.t.pull_counter += num3 - 1;
            pull_at_tentacle(x, num, true, path);
        }

        // Tentacle.FindGrabPath
        void find_grab_path(ctx& x, std::vector<ivec2>& path) {
            ivec2 const g = *grab_dest(x);
            ivec2 const b = base_pos(x);
            int32_t num;
            if (!inside(x.room, g) || !inside(x.room, b) || visual(x, b, g))
                num = ray_trace_tiles_list(b.x, b.y, g.x, g.y, x.grab_path);
            else
                num = find_tentacle_path(x, b, g, x.grab_path);
            if (int32_t(x.grab_path.size()) > num)
                x.grab_path.resize(size_t(num));
            move_aligned_segments_with_path(x, path);
        }

        // Tentacle.MoveGrabDest
        void move_grab_dest(ctx& x, vec2 to) {
            if (auto const g = grab_dest(x); g && *g == room_view::get_tile_position(to))
                return;
            x.t.float_grab_dest = to;
            find_grab_path(x, x.scratch);
        }

        // Tentacle.Grow
        void grow(ctx& x, int32_t add) {
            auto& seg = x.segments;
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
        void move_base(ctx& x, ivec2 to, std::vector<ivec2>& path) {
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
                    ++x.t.pull_counter;
                    if (x.t.pull_counter > 4)
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
                x.t.pull_counter += num2 - 1;
                if (x.t.pull_counter > 4)
                    pull_at_tentacle(x, 0, false, path);
            }
            if (grab_dest(x))
                find_grab_path(x, path);
        }

        // Tentacle.AlignWithGrabPath
        void align_with_grab_path(ctx& x, std::vector<ivec2>& path) {
            auto& seg = x.segments;
            auto& gp = x.grab_path;
            bool flag = false;
            for (int32_t i = std::min(int32_t(seg.size()) - 2, int32_t(gp.size()) - 1); i > 0; --i) {
                if (i >= int32_t(seg.size()) || i >= int32_t(gp.size()))
                    continue;
                if (seg[size_t(i)] != gp[size_t(i)] && visual(x, seg[size_t(i)], gp[size_t(i)])) {
                    ray_trace_tiles_list(seg[size_t(i)].x, seg[size_t(i)].y, gp[size_t(i)].x, gp[size_t(i)].y, path);
                    if (path.size() > 1 && i + 1 < int32_t(seg.size()) && move_segment(x, i + 1, path[1], false, false))
                        flag = true;
                }
            }
            if (!flag) {
                for (int32_t k = 1; k < int32_t(seg.size()) && k < int32_t(gp.size()); ++k) {
                    if (seg[size_t(k)] != gp[size_t(k)]) {
                        move_segment(x, k, gp[size_t(k)], true, false);
                        return;
                    }
                }
            }
        }

        // Tentacle.CurrentLength
        float current_length(ctx const& x) {
            float num = float(x.segments.size()) * 20.f;
            for (size_t i = 0; i + 2 < x.segments.size(); ++i)
                if (are_diagonal_neighbors(x.segments[i], x.segments[i + 2]))
                    num -= 5.9000006f;
            return num;
        }

        // Tentacle.AdjustLength (`shorten` on: no longer than the grab path)
        void adjust_length(ctx& x, std::vector<ivec2>& path) {
            float const num = current_length(x);
            float num2 = ideal_length;
            if (!x.grab_path.empty())
                num2 = mathf::min(ideal_length, float(x.grab_path.size()) * 20.f);
            int32_t const num3 = int32_t(num2 / 20.f - num / 20.f);
            if (num3 > 0 && (x.grab_path.empty() || x.segments.size() < x.grab_path.size())) {
                grow(x, 1);
                return;
            }
            if (num3 < 0) {
                ++x.t.pull_counter;
                pull_at_tentacle(x, 0, false, path);
            }
        }

        // Tentacle.GravityDirection, Gravity
        void gravity(ctx& x) {
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

        // Tentacle.Update
        void tentacle_update(ctx& x) {
            auto& path = x.scratch;
            move_base(x, room_view::get_tile_position(float_base(x)), path);
            x.t.update_counter += tile_tentacle_update_speed;
            if (x.t.update_counter >= 1.f) {
                x.t.pulls_this_tick = max_pull_tiles_per_tick;
                if (x.t.limp)
                    gravity(x);
                else if (!x.grab_path.empty())
                    align_with_grab_path(x, path);
                adjust_length(x, path);
                x.t.update_counter -= 1.f;
            }
            x.t.backtrack_from = -1;
            for (uint32_t i = 0; i < chunk_count; ++i)
                chunk_update(x, i);
        }

        // Tentacle.NewRoom: every segment on the tip's tile, the chunks on them
        void tentacle_new_room(ctx& x) {
            x.segments.assign(size_t(ideal_length / 20.f), room_view::get_tile_position(x.pos(0)));
            for (uint32_t i = 0; i < chunk_count; ++i)
                chunk_reset(x, x.ch[i]);
            x.grab_path.clear();
            x.t.float_grab_dest = std::nullopt;
        }

        // Tentacle.TotalRope
        float total_rope(ctx const& x) {
            float n = 0.f;
            for (uint32_t i = 0; i < chunk_count; ++i)
                n += x.ch[i].rope.total_length;
            return n;
        }

        //
        // TentaclePlant
        //
        // the room's creature hole at shortCutPos: its den (else it is rooted
        // in the terrain and has nowhere to draw back into)
        bool has_den(ctx const& x) {
            ivec2 const s = x.state.world.tentacle_plant_data_get_shortcut_pos(x.d);
            if (!x.room.inside(s.x, s.y) || x.room.get_tile(s).t != world::terrain::shortcut_entrance)
                return false;
            auto const sd = x.room.get_shortcut_data(s);
            return sd.type == world::shortcut_type::creature_hole && bool(sd.dest_exit);
        }

        // enteringShortCut = shortCutPos: dens.cpp's after_update pulls it in
        // (eating what it holds) when it is going home to that entrance
        void go_home(ctx& x) {
            auto& w = x.state.world;
            w.creature_set_going_home(x.c, 1);
            w.creature_set_travel_tile(x.c, w.tentacle_plant_data_get_shortcut_pos(x.d));
        }

        // the creature it holds and the chunk (none if gone out of reach)
        dcon::body_chunk_id grabbed_chunk(sys::game_state const& state, dcon::creature_id c) {
            auto const t = creature::grasped(state.world, c, 0);
            if (!t || !creature::here(state, t, creature::room_of(state, c)))
                return dcon::body_chunk_id{};
            uint32_t const k = creature::grasp_chunk(state.world, c, 0);
            if (k >= creature::chunk_count_of(state, t))
                return dcon::body_chunk_id{};
            return creature::chunk_of(state, t, k);
        }

        // what it pulled in, without a den to take it into (rooted in the
        // terrain): eaten where it is, like dens.cpp's take_prey
        void eat_in_place(ctx& x) {
            auto& w = x.state.world;
            auto const t = creature::grasped(w, x.c, 0);
            creature::release_grasp(x.state, x.c, 0);
            if (!creature::live(x.state, t))
                return;
            creature::release_target(x.state, t, x.room.id);
            ++dens::stats().prey_taken;
            if (auto const pp = creature::player_of(x.state, t)) {
                items::release_all(x.state, pp);
                creature::kill(x.state, t, creature::tmpl::tentacle_plant);
                w.player_set_eaten(pp, 1);
                return;
            }
            creature::kill(x.state, t, creature::tmpl::tentacle_plant);
            ++dens::stats().creatures_eaten;
            x.state.defer(x.room.id, [t](sys::game_state& st) {
                if (st.world.creature_is_valid(t))
                    creature::destroy(st, t);
            });
        }

        // gone (Destroy: drawn back in, dead or hurt, with no den to keep it)
        void vanish(ctx& x) {
            dcon::creature_id const c = x.c;
            x.state.defer(x.room.id, [c](sys::game_state& st) {
                if (st.world.creature_is_valid(c))
                    destroy(st, c);
            });
        }

        // where it roots for a spawn at `at`: the den entrance there, or the
        // terrain beside it (shortCutPos and stickOutDir)
        void root_at(room_view const& room, vec2 at, ivec2& hole, vec2& dir) {
            ivec2 const tile = room_view::get_tile_position(at);
            if (room.inside(tile.x, tile.y) && room.get_tile(tile).t == world::terrain::shortcut_entrance) {
                hole = tile;
                dir = rw::to_vec2(room.shortcut_entrance_hole_direction(tile));
                if (dir.x != 0.f || dir.y != 0.f)
                    return;
            }
            // (not vanilla: rooted at a spot of its own, the solid tile beside
            // it plays the den entrance, the floor first)
            constexpr ivec2 sides[4] = {{0, -1}, {-1, 0}, {1, 0}, {0, 1}};
            if (!room.get_tile(tile).solid()) {
                for (auto const s : sides)
                    if (room.get_tile(tile + s).solid() && !room.get_tile(tile - s).solid()) {
                        hole = tile + s;
                        dir = -rw::to_vec2(s);
                        return;
                    }
                for (int32_t k = 1; k < 30 && tile.y - k >= 0; ++k)
                    if (room.get_tile(tile.x, tile.y - k).solid()) {
                        hole = ivec2{tile.x, tile.y - k};
                        dir = vec2{0.f, 1.f};
                        return;
                    }
            }
            hole = tile;
            dir = vec2{0.f, 1.f};
        }

        // TentaclePlant.NewRoom
        void new_room(ctx& x, vec2 at) {
            ivec2 hole;
            vec2 dir;
            root_at(x.room, at, hole, dir);
            x.state.world.tentacle_plant_data_set_shortcut_pos(x.d, hole);
            tentacle_new_room(x);
            x.p.stick_out_dir = dir;
            x.p.root_pos = room_view::middle_of_tile(hole) + dir * 30.f;
            x.p.idle_pos = x.p.root_pos + dir * mathf::lerp(200.f, 300.f, x.rng.value());
        }

        //
        // TentaclePlantAI: Tracker(4, 1, 100, .., 3), PreyTracker(1, 1.2, 0,
        // 400), ThreatTracker(1)
        //
        tracker::params const& tracker_params() {
            static tracker::params const p = [] {
                tracker::params q;
                q.see_around_corners = 4;
                q.frames_to_remember = 100;
                q.ghost_dismissal_range = 3;
                q.prey_max = 1;
                q.persistance_bias = 1.2f;
                q.sure_to_get = 0.f;
                q.sure_to_lose = 400.f;
                q.give_up_unreachable = -1;
                q.threat_max = 1;
                return q;
            }();
            return p;
        }

        struct hook_ctx {
            sys::game_state& state;
            dcon::creature_id c;
            room_view const& room;
        };
        // ArtificialIntelligence.VisualContact from TentaclePlant.VisionPoint
        // (a random tentacle chunk), with BodyChunk.VisibilityBonus by
        // movementBasedVision 1: what stands still isn't seen
        bool visual_contact(hook_ctx& x, vec2 look, float bonus) {
            auto& w = x.state.world;
            auto const& cs = w.tentacle_plant_data_get_chunks(data(x.state, x.c)).c;
            vec2 const vp = cs[uint32_t(w.creature_get_random(x.c).range(0, int32_t(chunk_count)))].pos;
            float const r = visual_radius * (1.f + bonus);
            if (!custom::dist_less(vp, look, r))
                return false;
            float score = mathf::inverse_lerp(r, 0.f, rw::distance(vp, look));
            // (waterVision 1, throughSurfaceVision 0)
            if (x.room.point_submerged(vp) != x.room.point_submerged(look))
                score -= 1.f;
            if (pathing::narrow_space(x.room, room_view::get_tile_position(look)))
                score -= 0.5f;
            return score > 0.f && x.room.visual_contact(vp, look);
        }
        bool hook_sees(void* u, dcon::body_chunk_id k) {
            auto& x = *static_cast<hook_ctx*>(u);
            auto const& w = x.state.world;
            float const moved = rw::distance(w.body_chunk_get_last_pos(k), w.body_chunk_get_pos(k));
            float const bonus = mathf::clamp(mathf::lerp(-1.f, 1.f, mathf::inverse_lerp(1.f, 10.f, moved)), -1.f, 1.f);
            return visual_contact(x, w.body_chunk_get_pos(k), bonus);
        }
        bool hook_sees_point(void* u, vec2 p) { return visual_contact(*static_cast<hook_ctx*>(u), p, 0.f); }
        // TentaclePlantAI.UpdateDynamicRelationship: its fears as they are,
        // everything it eats by its mass
        creature::relationship hook_relate(void* u, creature::tracked const& r) {
            auto& x = *static_cast<hook_ctx*>(u);
            auto const st = creature::static_relationship(x.state, x.c, r.who);
            if (st.type == creature::rel::afraid)
                return st;
            if (st.type != creature::rel::eats)
                return creature::relationship{};
            float const m = creature::live(x.state, r.who) ? creature::total_mass(x.state, r.who) : 0.f;
            return creature::relationship{creature::rel::eats, mathf::inverse_lerp(0.f, 10000.f, m)};
        }

        // TentaclePlantAI.Update (the item interest left out)
        void ai_update(ctx& x) {
            hook_ctx hx{x.state, x.c, x.room};
            tracker::hooks h;
            h.user = &hx;
            h.sees = hook_sees;
            h.sees_point = hook_sees_point;
            h.relate = hook_relate;
            h.module = tracker::module_without_aggression;
            tracker::update(x.state, x.c, x.room, room_view::get_tile_position(x.p.root_pos), tracker_params(), h);
            // what it hasn't seen for 30 ticks is forgotten (its one
            // representation; here every one)
            auto const& s = tracker::of(x.state, x.c);
            dcon::creature_id gone[creature::max_tracked];
            uint32_t n = 0;
            for (uint32_t i = 0; i < s.count; ++i)
                if (s.reps[i].ticks_since_seen > 30)
                    gone[n++] = s.reps[i].who;
            for (uint32_t i = 0; i < n; ++i)
                tracker::forget(x.state, x.c, gone[i]);
        }

        // TentaclePlant.Collide on its tip: what it touches while lashing out
        // (or barely moving against it) it grabs
        void touch(sys::game_state& state, dcon::creature_id c, uint32_t i, dcon::creature_id t, uint32_t j) {
            auto& w = state.world;
            auto const tt = w.creature_get_type(t);
            if (tt == creature::type::tentacle_plant || tt == creature::type::pole_mimic)
                return;
            if (i > 0 || creature::grasped_object(w, c, 0) || !creature::consious(state, c))
                return;
            if (j >= creature::chunk_count_of(state, t))
                return;
            auto const k = creature::chunk_of(state, t, j);
            vec2 const mv = w.body_chunk_get_vel(creature::chunk(state, c, 0));
            vec2& v = w.body_chunk_get_vel(k);
            v = rw::lerp(v, mv, 0.2f / w.body_chunk_get_mass(k));
            if (pl(state, c).can_grab > 0.f || custom::dist_less(v, mv, 1.f))
                creature::grab(state, c, 0, t, j);
        }
    }

    // TentaclePlant.Rad
    float rad(float f) noexcept {
        f = mathf::max(1.f - f, std::sin(mathf::pi * mathf::inverse_lerp(0.7f, 1.f, f)));
        return mathf::lerp(tip_rad, root_rad, f);
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 pos) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::tentacle_plant);
        auto const d = data(state, c);
        rw::rng rng{state.random.next() | 1u};
        int32_t seed = int32_t(rng.next() & 0x7FFFFFFFu);
        if (state.spawn_seed >= 0)
            seed = state.spawn_seed;
        physics::object_desc desc;
        desc.gravity = 0.9f;
        desc.air_friction = 0.99f;
        desc.bounce = 0.1f;
        desc.surface_friction = 0.47f;
        desc.water_friction = 0.92f;
        desc.buoyancy = 0.95f;
        auto const o = physics::create_object(state, room, desc);
        // the tip (tipRad) and the root (rootRad), 0.2 each, no connection
        physics::add_chunk(state, o, pos, tip_rad, 0.2f);
        auto const root = physics::add_chunk(state, o, pos, root_rad, 0.2f);
        w.body_chunk_set_collide_with_terrain(root, 0);
        physics::set_go_through_floors(state, o, true);
        w.try_create_creature_body(c, o);
        w.creature_set_random(c, rng);
        w.creature_set_graphics_random(c, rw::rng{rng.next() | 1u});
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_set_grasps(c, creature::grasp_pair{});
        w.creature_set_destination(c, room_view::get_tile_position(pos));
        w.tentacle_plant_data_set_seed(d, seed);
        w.tentacle_plant_data_set_tentacle(d, tentacle{});
        w.tentacle_plant_data_set_graphics(d, graphics_state{});
        // the TentacleChunks: tPos (i + 1) / 8, from rootRad down to tipRad
        auto& cs = w.tentacle_plant_data_get_chunks(d).c;
        for (uint32_t i = 0; i < chunk_count; ++i) {
            cs[i] = tentacle_chunk{};
            cs[i].t_pos = float(i + 1) / float(chunk_count);
            cs[i].rad = mathf::lerp(root_rad, tip_rad, float(i) / float(chunk_count - 1));
        }
        // AbstractCreature.Personality(ID).aggression
        {
            rw::unity_random r{seed};
            auto const push = [](float v, float e) {
                if (v == 0.5f)
                    return 0.5f;
                return v < 0.5f ? custom::lerp_map(v, 0.f, 0.5f, 0.f, 0.5f, e) : custom::lerp_map(v, 1.f, 0.5f, 1.f, 0.5f, e);
            };
            float const sympathy = push(r.value(), 1.5f);
            float const energy = push(r.value(), 1.5f);
            float const bravery = push(r.value(), 1.5f);
            // (nervous: two rolls)
            r.value();
            r.value();
            float const a2 = r.value(), b2 = r.value();
            float const aggression = mathf::lerp(a2, (energy + bravery) / 2.f * (1.f - sympathy), mathf::pow(b2, 0.25f));
            auto& p = pl(state, c);
            uint16_t const keep = p.data;
            p = creature::tentacle_plant_payload{};
            p.data = keep;
            p.aggression = push(aggression, 2.5f);
        }
        place_in_room(state, c, room, pos);
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        creature::release_target(state, c);
        creature::erase(state, c);
    }

    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, vec2 at) {
        auto const view = world::get_room_view(state, room);
        ctx x(state, c, view);
        new_room(x, at);
        x.p.extended = 1.f;
        x.p.attack = 0.f;
        x.p.can_grab = 0.f;
        x.p.force_into_shortcut = 0.f;
        x.p.hide = 0;
        // the root at once where it grows from (the first Update's)
        physics::hard_set_position(state, x.b[1], x.p.root_pos);
        x.store();
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        ctx x(state, c, room);
        auto& p = x.p;
        auto& t = x.t;
        // (the sound loops have no sound ids here)
        // Creature.Update, PhysicalObject.Update
        creature::stun_countdown(state, c);
        physics::update_object(state, room, creature::body(state, c), nullptr, nullptr);
        tentacle_update(x);
        t.limp = x.consious() ? 0 : 1;
        t.retract_fac = mathf::clamp01(1.f - p.extended);
        w.body_chunk_set_rad(x.b[0], p.attack > 1.f ? 9.f : tip_rad);
        w.body_chunk_set_collide_with_terrain(x.b[0], p.extended == 1.f && t.backtrack_from == -1);
        if (float const lim = ideal_length * 2.f * p.extended; !custom::dist_less(x.pos(1), x.pos(0), lim))
            x.pos(0) = x.pos(1) + custom::dir_vec(x.pos(1), x.pos(0)) * lim;
        if (!t.float_grab_dest)
            move_grab_dest(x, p.idle_pos);
        // drawn back in for a while (rooted out of a den): no hunting
        bool const hidden = p.hide > 0;
        auto const* prey = hidden ? nullptr : tracker::prey(tracker::of(state, c));
        vec2 const out = p.root_pos + p.stick_out_dir * 200.f;
        if (!prey) {
            // the idle point wanders about, away from the terrain, back
            // towards 200 out of the root
            if (x.proximity(p.idle_pos) < 8) {
                ivec2 a = custom::eight_directions[x.rng.range(0, 8)];
                if (x.rng.value() < 0.025f)
                    a *= 2;
                vec2 const q = p.idle_pos + rw::to_vec2(a) * 20.f;
                if (!x.room.get_tile(q).solid() && (x.proximity(q) > x.proximity(p.idle_pos) || x.proximity(p.idle_pos) < 1))
                    p.idle_pos += rw::to_vec2(a) * 5.f + custom::rnv(x.rng) * 3.f;
            }
            if (!custom::dist_less(p.idle_pos, out, 150.f)) {
                vec2 a2 = custom::rnv(x.rng);
                if (x.rng.value() < 0.025f)
                    a2 *= 2.f;
                vec2 const q = p.idle_pos + a2 * 20.f;
                if (!x.room.get_tile(q).solid() && rw::distance(q, out) < rw::distance(p.idle_pos, out))
                    p.idle_pos += a2 * 5.f + custom::rnv(x.rng) * 3.f;
            }
            if (x.rng.value() < 0.005882353f
            && (!custom::dist_less(x.pos(0), p.idle_pos, 50.f) || x.proximity(p.idle_pos) < 1 || pathing::narrow_space(room, room_view::get_tile_position(p.idle_pos))))
                p.idle_pos = x.pos(0);
            move_grab_dest(x, p.idle_pos);
        } else if (t.float_grab_dest) {
            p.idle_pos = *t.float_grab_dest;
        }
        if (x.consious() && !hidden) {
            ai_update(x);
            prey = tracker::prey(tracker::of(state, c));
            if (!creature::grasped_object(w, c, 0)) {
                if (p.attack >= 1.f) {
                    // the lash
                    p.attack += 0.1f;
                    x.tip().vel += p.attack_dir * 20.f;
                    x.vel(0) += p.attack_dir * 20.f;
                    if (p.attack > 2.f)
                        p.attack = 0.f;
                } else {
                    // shaking towards it while it is within reach and seen
                    bool flag2 = false;
                    if (prey) {
                        if (prey->visual && creature::here(state, prey->who, room.id)) {
                            vec2 const pos2 = creature::main_pos(state, prey->who);
                            if (custom::dist_less(pos2, p.root_pos, ideal_length)) {
                                flag2 = true;
                                vec2 const pv = w.body_chunk_get_vel(creature::chunk_of(state, prey->who, creature::main_chunk_of(state, prey->who)));
                                p.attack_dir = custom::dir_vec(x.pos(0), pos2 + pv * rw::distance(x.pos(0), pos2) * 0.03f);
                            }
                            move_grab_dest(x, pos2);
                        } else {
                            move_grab_dest(x, room_view::middle_of_tile(tracker::best_guess(*prey)));
                        }
                    }
                    if (flag2)
                        p.attack += 0.011111111f;
                    else
                        p.attack = mathf::max(0.f, p.attack - 0.0055555557f);
                }
            } else {
                p.attack = 0.f;
            }
            if (t.float_grab_dest)
                x.vel(0) += rw::clamp_magnitude(*t.float_grab_dest - x.pos(0), 20.f) / 20.f;
        } else {
            p.attack = 0.f;
        }
        // (Tentacle_Plant_Init_Thrust would play as the lash starts: no sound id)
        if (p.attack > 1.f) {
            // lashing out: whatever the tip passes through it may grab
            p.can_grab = 1.f;
            vec2 const tp = x.pos(0);
            float const tr = w.body_chunk_get_rad(x.b[0]);
            rooms::for_each_creature(state, room.id, [&](dcon::creature_id o) {
                if (o == c || creature::grasped_object(w, c, 0) || !creature::here(state, o, room.id))
                    return;
                uint32_t const m = creature::chunk_count_of(state, o);
                for (uint32_t j = 0; j < m; ++j) {
                    auto const k = creature::chunk_of(state, o, j);
                    if (custom::dist_less(tp, w.body_chunk_get_pos(k), tr + w.body_chunk_get_rad(k))) {
                        touch(state, c, 0, o, j);
                        if (creature::grasped_object(w, c, 0))
                            return;
                    }
                }
            });
        } else {
            p.can_grab = mathf::max(0.f, p.can_grab - 0.025f);
        }
        // the tentacle stiffened out of the root, shaking before the lash
        for (uint32_t k = 0; k < chunk_count; ++k) {
            float const f = float(k) / float(chunk_count - 1);
            auto& ck = x.ch[k];
            ck.vel *= 0.96f;
            if (p.extended > 0.5f && creature::grasped_object(w, c, 0) && ck.phase > -1.f)
                creature::release_grasp(state, c, 0);
            if (t.backtrack_from == -1 || t.backtrack_from > int32_t(k) - 2) {
                if (p.attack > 0.5f && p.attack < 1.f && t.float_grab_dest)
                    ck.vel += custom::dir_vec(*t.float_grab_dest, rw::lerp(x.pos(0), out, 0.5f)) * p.attack * 0.8f;
                vec2 q = p.root_pos - p.stick_out_dir * 30.f;
                if (k == 1) {
                    q = p.root_pos;
                } else if (k > 1) {
                    q = x.ch[k - 2].pos;
                    x.ch[k - 2].vel += custom::dir_vec(ck.pos, x.ch[k - 2].pos);
                }
                ck.vel += custom::dir_vec(q, ck.pos);
                if (x.consious())
                    ck.vel += p.stick_out_dir * mathf::lerp(0.3f, 0.f, f);
            }
        }
        t.retract_fac = mathf::clamp01(1.f - p.extended);
        if (p.extended == 0.f) {
            // drawn in: the body out of the way under the root
            for (uint32_t l = 0; l < 2; ++l) {
                w.body_chunk_set_collide_with_terrain(x.b[l], 0);
                physics::hard_set_position(state, x.b[l], p.root_pos + vec2{0.f, -50.f});
                x.vel(l) = vec2{0.f, 0.f};
            }
        } else {
            x.pos(1) = p.root_pos;
            x.vel(1) = vec2{0.f, 0.f};
            for (uint32_t m = 0; m < 2; ++m)
                w.body_chunk_set_collide_with_terrain(x.b[m], t.backtrack_from == -1);
            float num3 = 0.f;
            if (t.backtrack_from == -1)
                num3 = x.consious() ? 0.5f : 0.7f;
            // the tip chunk and the tentacle's tip held together
            vec2 const a3 = custom::dir_vec(x.pos(0), x.tip().pos);
            float const num4 = rw::distance(x.pos(0), x.tip().pos);
            x.pos(0) += a3 * num4 * (1.f - num3);
            x.vel(0) += a3 * num4 * (1.f - num3);
            x.tip().pos -= a3 * num4 * num3;
            x.tip().vel -= a3 * num4 * num3;
            if (x.consious())
                x.vel(0).y += physics::gravity(state, creature::body(state, c), room);
        }
        auto const& ts = tracker::of(state, c);
        auto const* threat = tracker::threat(ts);
        if (x.dead() || mathf::inverse_lerp(0.3f, 0.65f, w.creature_get_health(c)) < 1.f - p.aggression) {
            // dead or hurt: it draws back into its den (Remix: what it holds
            // may get away; a slugcat's grasp wiggle is off)
            if (creature::grasped_object(w, c, 0)) {
                bool flag3 = true;
                if (!x.dead())
                    flag3 = !creature::is_slugcat(w, creature::grasped(w, c, 0)) && x.rng.value() > 0.98f;
                if (flag3) {
                    creature::stun(state, c, 70);
                    creature::release_grasp(state, c, 0);
                }
            }
            p.extended = mathf::max(0.f, p.extended - 0.016666668f);
            if (p.extended <= 0.f) {
                if (!x.dead() && has_den(x))
                    go_home(x);
                else
                    vanish(x);
            }
        } else if (threat && float_dist(tracker::best_guess(*threat), room_view::get_tile_position(p.root_pos)) < 20.f) {
            // something it fears close by: in it goes
            p.extended = mathf::max(0.f, p.extended - 0.016666668f);
            if (p.extended <= 0.f) {
                tracker::forget(state, c, threat->who);
                threat = nullptr;
                w.creature_set_remain_in_den(c, 400);
                if (has_den(x))
                    go_home(x);
                else
                    p.hide = 400;
            }
        }
        if (auto const gc = grabbed_chunk(state, c)) {
            // TentaclePlant.Carry: the held chunk and the tip together, then
            // pulled in towards the root
            auto const held = creature::grasped(w, c, 0);
            vec2& gp = w.body_chunk_get_pos(gc);
            vec2& gv = w.body_chunk_get_vel(gc);
            float const gm = w.body_chunk_get_mass(gc);
            {
                vec2 const a = custom::dir_vec(gp, x.tip().pos);
                float const num = rw::distance(gp, x.tip().pos);
                float const num2 = gm / (gm + 0.1f);
                gp += a * num * (1.f - num2);
                gv += a * num * (1.f - num2);
                x.tip().pos -= a * num * num2;
                x.tip().vel -= a * num * num2;
            }
            p.extended -= 0.0125f;
            gv += rw::clamp_magnitude(p.root_pos - gp, mathf::lerp(50.f, 500.f, p.force_into_shortcut)) * (1.f - p.extended) / 20.f
                / mathf::lerp(gm, 0.1f, p.force_into_shortcut);
            if (w.creature_get_type(held) == creature::type::tentacle_plant) {
                creature::release_grasp(state, c, 0);
                x.store();
                return;
            }
            if (p.extended < 0.f) {
                p.extended = 0.f;
                p.force_into_shortcut += 0.033333335f;
                if (custom::dist_less(gp, p.root_pos, 20.f) || p.force_into_shortcut > 1.f) {
                    // into the den with it (dens.cpp eats it on the way in)
                    w.creature_set_remain_in_den(c, 1200);
                    if (has_den(x))
                        go_home(x);
                    else
                        eat_in_place(x);
                    p.force_into_shortcut = 0.f;
                }
            } else {
                p.force_into_shortcut = 0.f;
            }
            // (Player.GraspWiggle, a Remix option, is off: no wiggling free)
            if (creature::grasped_object(w, c, 0)
            && (total_rope(x) > 600.f || w.creature_get_entering_shortcut(held).x >= 0)) {
                creature::release_grasp(state, c, 0);
                x.store();
                return;
            }
        } else {
            if (creature::grasped_object(w, c, 0))
                creature::release_grasp(state, c, 0);
            if (!x.dead() && !threat) {
                if (p.hide > 0) {
                    --p.hide;
                    p.extended = 0.f;
                } else {
                    p.extended = 1.f;
                }
                p.force_into_shortcut = 0.f;
            }
        }
        x.store();
    }

    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) {
        // (what it holds is kept to its tip by Carry)
        return !creature::holds(state.world, c, t);
    }

    void collide(sys::game_state& state, dcon::creature_id c, room_view const&, uint32_t i, dcon::creature_id t, uint32_t j) { touch(state, c, i, t, j); }

    void violence(sys::game_state& state, dcon::creature_id c, uint32_t k, vec2 momentum, float damage, float stun_bonus, creature::damage_type type) {
        auto& w = state.world;
        auto& rng = w.creature_get_random(c);
        // TentaclePlant.Violence: a spear stuns it longer
        if (type == creature::damage_type::stab)
            stun_bonus += 50.f;
        // Creature.Violence
        if (k < 2) {
            auto const hit = creature::chunk(state, c, k);
            w.body_chunk_get_vel(hit) += rw::clamp_magnitude(momentum / w.body_chunk_get_mass(hit), 10.f);
        }
        float const dmg = damage / base_damage_resistance;
        float st = (damage * 30.f + stun_bonus) / base_stun_resistance;
        float& health = w.creature_get_health(c);
        st *= 1.5f + mathf::inverse_lerp(0.5f, 0.f, health) * rng.value();
        creature::stun(state, c, int32_t(st));
        health -= dmg;
        if (rng.value() < -health || health < -1.f || (health < 0.f && rng.value() < 0.33f))
            creature::die(state, c);
        // (Remix) a hit makes it let go
        creature::release_grasp(state, c, 0);
    }

    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t) {
        if (creature::grasped(state.world, c, 0) == t)
            creature::release_grasp(state, c, 0);
    }

    void died(sys::game_state&, dcon::creature_id) {
        // (Creature.Die's letting go is creature::die's; it draws back in
        // and is gone in its update)
    }

    bool trace_tentacle(sys::game_state const& state, dcon::room_id room, vec2 from, vec2 to, dcon::creature_id& hit, float& f) {
        auto const& w = state.world;
        float best = 2.f;
        vec2 const dv = to - from;
        rooms::for_each_creature(state, room, creature::type::tentacle_plant, [&](dcon::creature_id c) {
            if (w.creature_get_away(c) != 0)
                return;
            auto const& cs = w.tentacle_plant_data_get_chunks(data(state, c)).c;
            vec2 prev = w.body_chunk_get_pos(creature::chunk(state, c, 1));
            for (uint32_t s = 0; s < chunk_count; ++s) {
                vec2 const cur = cs[s].pos;
                // Custom.LineIntersection of the two segments
                vec2 const r = dv, q = cur - prev;
                float const den = r.x * q.y - r.y * q.x;
                if (den != 0.f) {
                    float const a = ((prev.x - from.x) * q.y - (prev.y - from.y) * q.x) / den;
                    float const b = ((prev.x - from.x) * r.y - (prev.y - from.y) * r.x) / den;
                    if (a >= 0.f && a <= 1.f && b >= 0.f && b <= 1.f && a < best) {
                        best = a;
                        hit = c;
                    }
                }
                prev = cur;
            }
        });
        f = best;
        return best <= 1.f;
    }

    std::string describe(sys::game_state const& state, dcon::creature_id c) {
        auto const& w = state.world;
        auto const& p = pl(state, c);
        std::string s = "monster kelp";
        if (creature::grasped(w, c, 0))
            s += " holding " + std::string{creature::to_string(w.creature_get_type(creature::grasped(w, c, 0)))};
        else if (p.attack > 1.f)
            s += " lashing out";
        else if (p.attack > 0.5f)
            s += " shaking";
        if (p.extended < 1.f)
            s += " drawn in " + std::to_string(int32_t((1.f - p.extended) * 100.f)) + "%";
        return s;
    }

    bool spawn_spot(room_view const& room, ivec2 tile) {
        // a den entrance, or an open tile on the terrain (a floor, a wall,
        // a ceiling) with room to grow out of it
        if (!room.inside(tile.x, tile.y))
            return false;
        if (room.get_tile(tile).t == world::terrain::shortcut_entrance)
            return room.get_shortcut_data(tile).type == world::shortcut_type::creature_hole;
        if (room.get_tile(tile).solid())
            return false;
        constexpr ivec2 sides[4] = {{0, -1}, {-1, 0}, {1, 0}, {0, 1}};
        for (auto const s : sides)
            if (room.get_tile(tile + s).solid() && !room.get_tile(tile - s).solid() && !room.get_tile(tile - s * 2).solid())
                return true;
        return false;
    }
}
