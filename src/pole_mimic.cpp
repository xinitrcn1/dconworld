// PoleMimic.cs: the pole plant. It roots in a den's pipe (or, spawned by
// hand, the terrain it grows out of) and stands its stalk up along the
// tiles in front of it: a Tentacle whose TentacleChunks are pulled onto
// every second tile of that line, still and straight, looking like a pole
// (the slugcat climbs it: it is an IClimbableVine, rot.hpp's vines). What
// touches it wakes it: an edible creature's chunk brushing a tentacle chunk
// sticks to it, the stalk lets go of its tip's spot, hunts the tastiest
// stuck chunk with its tip and grabs what the tip touches; it pulls that to
// its root, shakes it (the hurt plant's jitter, the tentacle's tugs) and
// drags edible prey into its den. Hurt enough (or dead) it withdraws into
// the den. It has no AI (CreatureTemplate.AI false).

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <queue>
#include <span>
#include <string>
#include <vector>

#include "pole_mimic.hpp"
#include "daddy_internal.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "rooms.hpp"
#include "rot.hpp"

namespace pole_mimic {
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // CreatureTemplate: baseDamageResistance, baseStunResistance, instantDeathDamageLimit
        constexpr float base_damage_resistance = 0.9f;
        constexpr float base_stun_resistance = 0.5f;
        constexpr float instant_death_damage_limit = 1.f;
        // PoleMimic.rad, its chunks' mass
        constexpr float rad = 2.f;
        constexpr float chunk_mass = 0.2f;
        // Initiate's walk along the line (a story session's 100; the spawn
        // data's own number isn't read)
        constexpr int32_t max_walk = 100;

        // Tentacle.TentacleProps(false, true, true, 0.5f, 0f, 0.2f, 0.5f,
        // 0.05f, 2.2f, 12f, 0.33333334f, 5f, 15, 60, 12, 20): a rope
        // tentacle, shortening to its grab path (goalAttractionSpeed and
        // alignToSegmentSpeed are set every tick)
        constexpr float mass_deterioration_per_chunk = 0.5f;
        constexpr float goal_attraction_speed_tip = 0.2f;
        constexpr float backtrack_speed = 2.2f;
        constexpr float chunk_velocity_cap = 12.f;
        constexpr float tile_tentacle_update_speed = 0.33333334f;
        constexpr float tile_tentacle_snap_with_path_distance = 5.f;
        constexpr int32_t segment_phase_through_terrain_frames = 15;
        constexpr int32_t max_pull_tiles_per_tick = 12;
        constexpr int32_t terrain_hits_before_phase = 20;

        constexpr float float_max = std::numeric_limits<float>::max();
        constexpr ivec2 four_directions_and_zero[5] = {{0, 0}, {-1, 0}, {0, -1}, {1, 0}, {0, 1}};

        creature::pole_mimic_payload& pl(sys::game_state& state, dcon::creature_id c) { return state.world.creature_get_payload(c).pole_mimic; }
        creature::pole_mimic_payload const& pl(sys::game_state const& state, dcon::creature_id c) { return state.world.creature_get_payload(c).pole_mimic; }
        dcon::pole_mimic_data_id data(sys::game_state const& state, dcon::creature_id c) { return creature::pole_mimic_data(state.world, c); }

        // PoleMimic.WakeUp, AnyWakeUp
        float wake_up(creature::pole_mimic_payload const& p) { return mathf::inverse_lerp(10.f, 80.f, float(p.wake_up_counter)); }
        float any_wake_up(creature::pole_mimic_payload const& p) { return mathf::inverse_lerp(0.f, 40.f, float(p.wake_up_counter)); }

        bool has_any_solid(room_view const& room, ivec2 t) { return room.get_tile(t).solid(); }
        float float_dist(ivec2 a, ivec2 b) { return rw::magnitude(rw::to_vec2(a - b)); }
        // Custom.AreIntVectorsNeighbors / AreIntVectorsDiagonalNeighbors (sic)
        bool are_neighbors(ivec2 a, ivec2 b) { return a != b && (a.x == b.x || a.y == b.y) && std::abs(a.x - b.x) < 2 && std::abs(a.y - b.y) < 2; }
        bool are_diagonal_neighbors(ivec2 a, ivec2 b) { return a.x != b.x && a.y != b.y && (std::abs(a.x - b.x) < 2 || std::abs(a.y - b.y) < 2); }

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

        // one pole plant while it updates: working copies of its tentacle's
        // tile lists (they grow and shrink every tick: std::vectors, back
        // into the vector pools after), its chunks and stuck chunks in place
        struct ctx {
            sys::game_state& state;
            dcon::creature_id c;
            room_view const& room;
            rw::rng rng;
            creature::pole_mimic_payload& p;
            dcon::pole_mimic_data_id d;
            body_state& b;
            std::vector<ivec2> segments;
            std::vector<ivec2> grab_path;
            // the `ref List<IntVector2> path` scratch list
            std::vector<ivec2> path;
            std::span<tentacle_chunk> ch;
            std::span<stick> st;
            std::span<ivec2 const> tiles;
            dcon::body_chunk_id k0, k1;
            // AImap.getTerrainProximity of the room (null: none asked for)
            std::vector<uint16_t> const* prox = nullptr;

            // (keyed by the constructing call site too: several ctxs of one
            // creature in one tick roll apart)
            ctx(sys::game_state& s, dcon::creature_id id, room_view const& r, std::source_location const l = std::source_location::current())
                : state(s), c(id), room(r), rng(rw::random(s.seed, s.tick, uint32_t(id.value), l)), p(pl(s, id)), d(data(s, id)), b(s.world.pole_mimic_data_get_body(d)) {
                auto& w = state.world;
                auto const segs = w.pole_mimic_data_get_segments(d);
                segments.assign(segs.begin(), segs.end());
                auto const gp = w.pole_mimic_data_get_grab_path(d);
                grab_path.assign(gp.begin(), gp.end());
                refresh_spans();
                k0 = creature::chunk(state, c, 0);
                k1 = creature::chunk(state, c, 1);
            }
            void refresh_spans() {
                auto& w = state.world;
                auto const cs = w.pole_mimic_data_get_chunks(d);
                ch = std::span<tentacle_chunk>{cs.begin(), cs.size()};
                auto const ss = w.pole_mimic_data_get_sticks(d);
                st = std::span<stick>{ss.begin(), ss.size()};
                auto const ts = w.pole_mimic_data_get_tiles(d);
                tiles = std::span<ivec2 const>{ts.begin(), ts.size()};
            }
            void store() {
                auto& w = state.world;
                w.pole_mimic_data_get_segments(d).load_range(segments.data(), segments.data() + segments.size());
                w.pole_mimic_data_get_grab_path(d).load_range(grab_path.data(), grab_path.data() + grab_path.size());
            }
            vec2& pos(dcon::body_chunk_id k) { return state.world.body_chunk_get_pos(k); }
            vec2& vel(dcon::body_chunk_id k) { return state.world.body_chunk_get_vel(k); }
            int32_t proximity(ivec2 t) const {
                if (!prox || !room.inside(t.x, t.y))
                    return -1;
                return (*prox)[size_t(t.y) * size_t(room.width) + size_t(t.x)];
            }
            bool consious() const { return creature::consious(state, c); }
        };

        //
        // Tentacle
        //
        // FloatBase: the connected chunk (bodyChunks[1], at the root)
        vec2 float_base(ctx& x) { return x.pos(x.k1); }
        tentacle_chunk& tip(ctx& x) { return x.ch.back(); }

        std::optional<ivec2> grab_dest(ctx const& x) {
            if (x.b.float_grab_dest)
                return room_view::get_tile_position(*x.b.float_grab_dest);
            return std::nullopt;
        }
        // CheckIfAnySegmentsLeft, BasePos
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

        // Tentacle.Visual (the terrain proximity map rules out the ray
        // traces that can't meet a solid tile, as daddy_tentacle.cpp's)
        bool visual(ctx& x, ivec2 a, ivec2 b) {
            int32_t const p = x.proximity(a);
            if (p > 0 && x.room.inside(b.x, b.y) && std::abs(b.x - a.x) + std::abs(b.y - a.y) < p)
                return true;
            return daddy::detail::ray_trace_tiles_for_terrain(x.room, a, b);
        }
        int32_t ray_trace_tiles_list(ivec2 a, ivec2 b, std::vector<ivec2>& path) { return daddy::detail::ray_trace_tiles_list(a.x, a.y, b.x, b.y, path); }

        // Rope.Update (a rope in the clear only takes its new ends)
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
            return c.phase == -1.f && (i == 0 || x.ch[i - 1].phase == -1.f) && (x.b.backtrack_from < int32_t(i) || x.b.backtrack_from == -1);
        }

        // TentacleChunk.Reset
        void chunk_reset(ctx& x, tentacle_chunk& c) {
            c.trail = stuck_pos(x, c);
            c.phase = -1.f;
            c.pos = room_view::middle_of_tile(stuck_pos(x, c));
            c.last_pos = c.pos;
            c.vel = vec2{0.f, 0.f};
            // new Rope(room, FloatBase, FloatBase + (1, 1), 1f)
            vec2 const fb = float_base(x);
            world::rope_state r;
            r.a = r.last_a = fb;
            r.b = r.last_b = fb + vec2{1.f, 1.f};
            r.total_length = rw::distance(r.a, r.b);
            r.thickness = 1.f;
            c.rope = r;
        }

        void phase_to_segment(tentacle_chunk& c) {
            c.phase_from = c.pos;
            c.phase = 0.f;
        }

        // TentacleChunk.Update
        void chunk_update(ctx& x, uint32_t i) {
            auto& c = x.ch[i];
            auto& t = x.b;
            float const n = float(x.ch.size());
            c.last_pos = c.pos;
            update_rope(x, c.rope, i == 0 ? float_base(x) : x.ch[i - 1].pos, c.pos);
            if (!rope_active(x, i) || c.phase > -1.f || (i > 0 && x.ch[i - 1].phase > -1.f) || c.rope.total_length > t.ideal_length / n * 5.f)
                c.rope.bend_count = 0;
            if (c.phase == -1.f) {
                if (c.phases_to_same_location > 0 && stuck_pos(x, c) == c.after_phase_stuck_pos) {
                    vec2 const m = room_view::middle_of_tile(stuck_pos(x, c));
                    c.pos = vec2{mathf::clamp(c.pos.x, m.x - 9.f, m.x + 9.f), mathf::clamp(c.pos.y, m.y - 9.f, m.y + 9.f)};
                }
                float const ideal = t.ideal_length / n * mathf::lerp(1.f, 0.1f, t.retract_fac);
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
                    c.stretched_fac = t.ideal_length / n / mathf::max(1.f, num);
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
                        vec2 const fb = i > 0 ? x.ch[i - 1].pos : float_base(x);
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
                    if (t.limp) {
                        for (int32_t k = cs; k >= 0; --k) {
                            ivec2 const s = x.segments[size_t(k)];
                            if (visual(x, here, s)) {
                                c.vel += rw::clamp_magnitude(room_view::middle_of_tile(s) - c.pos, 20.f) * 0.005f;
                                break;
                            }
                        }
                    } else {
                        if (t.float_grab_dest) {
                            float const speed = i == x.ch.size() - 1 ? goal_attraction_speed_tip : t.goal_attraction_speed;
                            c.vel += rw::clamp_magnitude(*t.float_grab_dest - c.pos, 20.f) * speed / 20.f;
                        }
                        c.vel += rw::clamp_magnitude(room_view::middle_of_tile(x.segments[size_t(cs)]) - c.pos, 20.f) * t.align_to_segment_speed / 20.f;
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
                    float const ideal = t.ideal_length / n * mathf::lerp(1.f, 0.1f, t.retract_fac);
                    if (num > ideal) {
                        prev.pos += a * (ideal - num) * mass_deterioration_per_chunk;
                        prev.vel += a * (ideal - num) * mass_deterioration_per_chunk;
                    }
                    c.stretched_fac = num > 0.f ? ideal / num : 1.f;
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
            auto& t = x.b;
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
        int32_t find_tentacle_path(ctx& x, ivec2 start, ivec2 goal, std::vector<ivec2>& path) {
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
            int32_t const num3 = ray_trace_tiles_list(gp[size_t(num)], seg[size_t(num)], path);
            for (int32_t i = num3 - 2; i >= 0; --i)
                seg.insert(seg.begin() + num, path[size_t(i)]);
            x.b.pull_counter += num3 - 1;
            pull_at_tentacle(x, num, true, path);
        }

        // Tentacle.FindGrabPath
        void find_grab_path(ctx& x, std::vector<ivec2>& path) {
            ivec2 const g = *grab_dest(x);
            ivec2 const b = base_pos(x);
            int32_t num;
            if (!x.room.inside(g.x, g.y) || !x.room.inside(b.x, b.y) || visual(x, b, g))
                num = ray_trace_tiles_list(b, g, x.grab_path);
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
            x.b.float_grab_dest = to;
            find_grab_path(x, x.path);
        }

        // Tentacle.Grow
        void grow(ctx& x, int32_t add) {
            auto& seg = x.segments;
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
                    ++x.b.pull_counter;
                    if (x.b.pull_counter > 4)
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
                x.b.pull_counter += num2 - 1;
                if (x.b.pull_counter > 4)
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
                    ray_trace_tiles_list(seg[size_t(i)], gp[size_t(i)], path);
                    if (path.size() > 1 && move_segment(x, i + 1, path[1], false, false))
                        flag = true;
                }
            }
            if (!flag) {
                for (int32_t k = 1; k < int32_t(seg.size()) && k < int32_t(gp.size()); ++k)
                    if (seg[size_t(k)] != gp[size_t(k)]) {
                        move_segment(x, k, gp[size_t(k)], true, false);
                        return;
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

        // Tentacle.AdjustLength (tProps.shorten: no longer than the grab path)
        void adjust_length(ctx& x, std::vector<ivec2>& path) {
            float const num = current_length(x);
            float num2 = x.b.ideal_length;
            if (!x.grab_path.empty())
                num2 = mathf::min(x.b.ideal_length, float(x.grab_path.size()) * 20.f);
            int32_t const num3 = int32_t(num2 / 20.f - num / 20.f);
            if (num3 > 0 && (x.grab_path.empty() || x.segments.size() < x.grab_path.size())) {
                grow(x, 1);
                return;
            }
            if (num3 < 0) {
                ++x.b.pull_counter;
                pull_at_tentacle(x, 0, false, path);
            }
        }

        // Tentacle.Gravity with Tentacle.GravityDirection
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
            auto& path = x.path;
            move_base(x, room_view::get_tile_position(float_base(x)), path);
            x.b.update_counter += tile_tentacle_update_speed;
            if (x.b.update_counter >= 1.f) {
                x.b.pulls_this_tick = max_pull_tiles_per_tick;
                if (x.b.limp)
                    gravity(x);
                else if (!x.grab_path.empty())
                    align_with_grab_path(x, path);
                adjust_length(x, path);
                x.b.update_counter -= 1.f;
            }
            x.b.backtrack_from = -1;
            for (uint32_t i = 0; i < x.ch.size(); ++i)
                chunk_update(x, i);
        }

        // Tentacle.NewRoom
        void tentacle_new_room(ctx& x) {
            x.segments.assign(size_t(std::max(1, int32_t(x.b.ideal_length / 20.f))), room_view::get_tile_position(x.pos(x.k0)));
            for (auto& c : x.ch)
                chunk_reset(x, c);
            x.grab_path.clear();
        }

        // Tentacle.Reset
        void tentacle_reset(ctx& x, vec2 at) {
            x.segments.assign(1, room_view::get_tile_position(at));
            for (auto& c : x.ch) {
                c.pos = at;
                c.vel = vec2{0.f, 0.f};
                chunk_reset(x, c);
            }
        }

        // Tentacle.TotalRope
        float total_rope(ctx const& x) {
            float n = 0.f;
            for (auto const& c : x.ch)
                n += c.rope.total_length;
            return n;
        }

        //
        // PoleMimic
        //
        // a creature's chunk (none for one gone or out of reach)
        dcon::body_chunk_id target_chunk(ctx const& x, dcon::creature_id t, uint8_t k) {
            if (!t || !creature::here(x.state, t, x.room.id) || k >= creature::chunk_count_of(x.state, t))
                return dcon::body_chunk_id{};
            return creature::chunk_of(x.state, t, k);
        }

        // PoleMimic.ChunkTastyness
        float tastyness(ctx const& x, dcon::creature_id t, uint8_t k) {
            auto const ck = target_chunk(x, t, k);
            if (!ck)
                return 0.f;
            auto const r = creature::static_relationship(x.state, x.c, t);
            if (r.type != creature::rel::eats)
                return 0.f;
            return x.state.world.body_chunk_get_mass(ck) * r.intensity;
        }
        float tastyness(ctx const& x, int32_t i) {
            if (i < 0 || i >= int32_t(x.st.size()) || !x.st[size_t(i)].who)
                return 0.f;
            return tastyness(x, x.st[size_t(i)].who, x.st[size_t(i)].chunk);
        }

        // PoleMimic.ChunkInPosition
        bool chunk_in_position(ctx const& x, uint32_t i) {
            auto const& c = x.ch[i];
            return x.p.tip_attached && custom::dist_less(c.pos, room_view::middle_of_tile(x.tiles[i]), 5.f) && rw::magnitude(c.vel) < mathf::lerp(2.f, 8.f, x.p.mimic);
        }

        // ReleaseGrasp(0)
        void release_grasp(ctx& x) { creature::release_grasp(x.state, x.c, 0); }

        // Custom.LerpAndTick on the leaves when a chunk sticks (the graphics
        // module, where a camera made it)
        void ruffle_leaves(ctx& x, float num2) {
            auto& w = x.state.world;
            auto const& g = w.pole_mimic_data_get_graphics(x.d);
            auto leaves = w.pole_mimic_data_get_leaves(x.d);
            if (!g.initiated || g.leaf_pairs <= 0 || leaves.size() < uint32_t(g.leaf_pairs) * 2u)
                return;
            // (the caller's stream: called in a loop over the chunks)
            auto& gr = x.rng;
            for (int32_t j = gr.range(2, 6); j >= 0; --j) {
                int32_t const num6 = custom::int_clamp(int32_t(num2 * float(g.leaf_pairs)) - 2 + gr.range(0, 5), 0, g.leaf_pairs - 1);
                int32_t const num7 = gr.value() < 0.5f ? 0 : 1;
                auto& lf = leaves.at(uint32_t(num6 * 2 + num7));
                lf.m[0] = gr.value();
                lf.m[2] = gr.value();
                lf.p[0] += custom::rnv(gr) * 2.f * gr.value();
            }
        }

        // PoleMimic.Act
        void act(ctx& x) {
            auto& p = x.p;
            float const wake = wake_up(p);
            if (p.hunt_counter > 100) {
                p.want_to_wake_up = 1;
                p.get_to_goal_force = mathf::min(1.f, p.get_to_goal_force + wake / 140.f);
            }
            if (p.want_to_wake_up && p.wake_up_counter < 250)
                ++p.wake_up_counter;
            else if (!p.want_to_wake_up && p.wake_up_counter > 0)
                --p.wake_up_counter;
            else if (p.angered_and_aggressive > 0)
                p.want_to_wake_up = 1;
            else
                p.want_to_wake_up = 0;
            float const num2 = custom::lerp_map(p.get_to_goal_force, 0.7f, 1.f, p.free_standing ? 5.f : 20.f, 30.f);
            auto const hunt_k = target_chunk(x, p.hunt, p.hunt_chunk);
            if (p.tip_attached) {
                if (!p.free_standing || (custom::dist_less(x.pos(x.k0), x.b.tip_pos, num2) && rw::magnitude(x.vel(x.k0)) < num2)) {
                    x.pos(x.k0) = x.b.tip_pos;
                    x.vel(x.k0) = vec2{0.f, 0.f};
                    tip(x).pos = x.b.tip_pos;
                    tip(x).vel = vec2{0.f, 0.f};
                    p.get_to_goal_force = mathf::max(0.f, p.get_to_goal_force - 0.033333335f);
                }
                if (hunt_k && (x.rng.value() < 0.033333335f || p.free_standing))
                    p.tip_attached = 0;
            } else {
                if (p.wake_up_counter == 0)
                    p.get_to_goal_force = mathf::min(1.f, p.get_to_goal_force + 0.007142857f);
                if (custom::dist_less(x.pos(x.k0), x.b.tip_pos, num2))
                    p.tip_attached = 1;
            }
            if (x.rng.value() < mathf::pow(wake_up(p), 5.f) || (p.free_standing && wake_up(p) > 0.f))
                p.tip_attached = 0;
            if (p.tip_attached && p.wake_up_counter == 0)
                ++p.mimic_delay_counter;
            else
                p.mimic_delay_counter = 0;
            p.mimic = custom::lerp_and_tick(p.mimic, p.mimic_delay_counter > 40 ? 1.f : 0.f, 0.01f, 0.0125f);
            x.b.ideal_length = float(x.tiles.size()) * 40.f * mathf::lerp(1.f, 0.75f, (1.f - p.get_to_goal_force) * (1.f - p.mimic));
            if (hunt_k && !p.tip_attached) {
                vec2 const hp = x.state.world.body_chunk_get_pos(hunt_k);
                if (x.room.visual_contact(x.pos(x.k0), hp))
                    move_grab_dest(x, hp);
                // (the other collision layer's prey, a leech: touched by hand)
                if (creature::type_of(x.state.world, p.hunt) == creature::type::leech
                    && custom::dist_less(hp, x.pos(x.k0), x.state.world.body_chunk_get_rad(hunt_k) + rad))
                    collide(x.state, x.c, x.room, 0, p.hunt, p.hunt_chunk);
            } else {
                move_grab_dest(x, x.b.tip_pos);
            }
            if (p.hunt_counter > 0)
                --p.hunt_counter;
            if (p.hunt && (p.hunt_counter < 1 || !target_chunk(x, p.hunt, p.hunt_chunk)))
                p.hunt = dcon::creature_id{};
        }

        // PoleMimic.Carry
        void carry(ctx& x, dcon::body_chunk_id g) {
            auto& w = x.state.world;
            vec2& gp = w.body_chunk_get_pos(g);
            vec2& gv = w.body_chunk_get_vel(g);
            vec2 const a = custom::dir_vec(gp, tip(x).pos);
            float const num = rw::distance(gp, tip(x).pos);
            float const m = w.body_chunk_get_mass(g);
            float const num2 = m / (m + 0.07f);
            gp += a * num * (1.f - num2);
            gv += a * num * (1.f - num2);
            tip(x).pos -= a * num * num2;
            tip(x).vel -= a * num * num2;
        }

        // the creatures out in the room (AbstractRoom.creatures, realized
        // and not in a den), itself included
        void room_creatures(ctx const& x, std::vector<dcon::creature_id>& out) {
            out.clear();
            rooms::for_each_creature(x.state, x.room.id, [&](dcon::creature_id t) {
                if (creature::here(x.state, t, x.room.id))
                    out.push_back(t);
            });
        }

        // the rest of PoleMimic.Update after Act
        void update_body(ctx& x) {
            auto& w = x.state.world;
            auto& p = x.p;
            uint32_t const n = uint32_t(x.ch.size());
            bool const consious = x.consious();
            x.b.goal_attraction_speed = p.get_to_goal_force * 0.4f;
            x.b.align_to_segment_speed = p.get_to_goal_force * 0.5f;
            std::vector<dcon::creature_id> crits;
            room_creatures(x, crits);
            dcon::creature_id best_who;
            uint8_t best_chunk = 0;
            float num = 0.f;
            float const health = w.creature_get_health(x.c);
            for (uint32_t i = 0; i < n; ++i) {
                float const num2 = float(i) / float(std::max(1u, n - 1));
                auto& tc = x.ch[i];
                auto& sc = x.st[i];
                tc.vel *= 0.96f;
                if (auto const g = creature::grasped(w, x.c, 0); g && w.creature_get_entering_shortcut(g).x >= 0)
                    release_grasp(x);
                if (p.extended > 0.5f && creature::grasped_object(w, x.c, 0) && tc.phase > -1.f)
                    release_grasp(x);
                if (health < 0.5f)
                    tc.vel += custom::rnv(x.rng) * mathf::inverse_lerp(0.5f, 0.f, health) * 2.f;
                if (x.b.backtrack_from == -1 || (x.b.backtrack_from > int32_t(i) - 2 && consious)) {
                    if (sc.who) {
                        p.want_to_wake_up = 1;
                        auto const sk = target_chunk(x, sc.who, sc.chunk);
                        float const srad = sk ? w.body_chunk_get_rad(sk) : 0.f;
                        float const num3 = sk ? rw::distance(tc.pos, w.body_chunk_get_pos(sk)) : 0.f;
                        if (!sk || num3 > srad + mathf::lerp(14.f, 20.f + 5.f * wake_up(p), any_wake_up(p))) {
                            sc = stick{};
                        } else {
                            if (tastyness(x, int32_t(i)) > num) {
                                best_who = sc.who;
                                best_chunk = sc.chunk;
                                num = tastyness(x, int32_t(i));
                            }
                            vec2& sp = w.body_chunk_get_pos(sk);
                            vec2& sv = w.body_chunk_get_vel(sk);
                            vec2 const a = custom::dir_vec(tc.pos, sp);
                            float const m = w.body_chunk_get_mass(sk);
                            float const num4 = m / (m + 0.18f);
                            float const dd = mathf::lerp(0.35f, 0.6f, wake_up(p));
                            tc.pos += a * (num3 - srad) * num4 * dd;
                            tc.vel += a * (num3 - srad) * num4 * dd;
                            sp -= a * (num3 - srad) * (1.f - num4) * dd;
                            sv -= a * (num3 - srad) * (1.f - num4) * dd;
                            // (Player.GraspWiggle is a Remix option: a stuck
                            // slugcat doesn't wiggle free)
                        }
                    } else {
                        // the neighbours' stuck chunks pull this one along
                        dcon::body_chunk_id nb;
                        float num5 = 0.f;
                        if (tastyness(x, int32_t(i) - 1) > num5) {
                            nb = target_chunk(x, x.st[i - 1].who, x.st[i - 1].chunk);
                            num5 = tastyness(x, int32_t(i) - 1);
                        }
                        if (tastyness(x, int32_t(i) + 1) > num5) {
                            nb = target_chunk(x, x.st[i + 1].who, x.st[i + 1].chunk);
                            num5 = tastyness(x, int32_t(i) + 1);
                        }
                        if (nb) {
                            tc.vel += rw::clamp_magnitude(w.body_chunk_get_pos(nb) - tc.pos, 10.f) / 5.f;
                        } else {
                            // a random creature of the room it eats, a random
                            // chunk of it touching this tentacle chunk
                            dcon::creature_id const t = crits.empty() ? dcon::creature_id{} : crits[size_t(x.rng.range(0, int32_t(crits.size())))];
                            if (t && creature::static_relationship(x.state, x.c, t).type == creature::rel::eats) {
                                uint32_t const m = creature::chunk_count_of(x.state, t);
                                uint8_t const k = uint8_t(x.rng.range(0, int32_t(m)));
                                if (auto const ck = target_chunk(x, t, k);
                                    ck && custom::dist_less(w.body_chunk_get_pos(ck), tc.pos, w.body_chunk_get_rad(ck) + 2.f)) {
                                    if ((p.angered_and_aggressive > 0 && !creature::is_slugcat(w, t)) || x.rng.value() < mathf::pow(any_wake_up(p), 0.25f)
                                        || x.rng.value() < 0.14285715f) {
                                        // (Pole_Mimic_Stick: no sample id)
                                        sc = stick{t, k};
                                    }
                                    ruffle_leaves(x, num2);
                                }
                            }
                            // standing as a pole: on its tile, or pulled there
                            if (p.mimic == 1.f && chunk_in_position(x, i)) {
                                tc.pos = room_view::middle_of_tile(x.tiles[i]);
                                tc.vel = vec2{0.f, 0.f};
                            } else {
                                if ((i > 0 && chunk_in_position(x, i - 1)) || (i < n - 1 && chunk_in_position(x, i + 1)))
                                    tc.vel *= mathf::lerp(1.f, 0.6f, mathf::inverse_lerp(0.5f, 1.f, p.mimic));
                                if (p.tip_attached || p.free_standing)
                                    tc.vel += rw::clamp_magnitude(room_view::middle_of_tile(x.tiles[i]) - tc.pos, 10.f) / 5.f
                                        * mathf::inverse_lerp(p.free_standing ? 0.75f : 0.5f, 1.f, p.mimic);
                                vec2 pp = x.b.root_pos - x.b.stick_out_dir * 30.f;
                                if (i == 1) {
                                    pp = x.b.root_pos;
                                } else if (i > 1) {
                                    pp = x.ch[i - 2].pos;
                                    x.ch[i - 2].vel += custom::dir_vec(tc.pos, x.ch[i - 2].pos);
                                }
                                tc.vel += custom::dir_vec(pp, tc.pos);
                                if (consious)
                                    tc.vel += x.b.stick_out_dir * mathf::lerp(0.3f, 0.f, num2);
                            }
                        }
                    }
                } else {
                    sc = stick{};
                }
            }
            // the tastiest stuck chunk is hunted
            if (best_who && mathf::pow(x.rng.value(), 3.f) < wake_up(p) && tastyness(x, best_who, best_chunk) > tastyness(x, p.hunt, p.hunt_chunk)) {
                p.hunt = best_who;
                p.hunt_chunk = best_chunk;
                p.hunt_counter = x.rng.range(110, 190);
            }
            x.b.retract_fac = mathf::clamp01(1.f - p.extended);
            if (p.extended == 0.f) {
                for (auto const k : {x.k0, x.k1}) {
                    w.body_chunk_set_collide_with_terrain(k, false);
                    physics::hard_set_position(x.state, k, x.b.root_pos + vec2{0.f, -50.f});
                    w.body_chunk_set_vel(k, vec2{0.f, 0.f});
                }
            } else {
                x.pos(x.k1) = x.b.root_pos;
                x.vel(x.k1) = vec2{0.f, 0.f};
                for (auto const k : {x.k0, x.k1})
                    w.body_chunk_set_collide_with_terrain(k, x.b.backtrack_from == -1);
                float num8 = 0.f;
                if (x.b.backtrack_from == -1)
                    num8 = consious ? 0.5f : 0.7f;
                vec2 const a2 = custom::dir_vec(x.pos(x.k0), tip(x).pos);
                float const num9 = rw::distance(x.pos(x.k0), tip(x).pos);
                x.pos(x.k0) += a2 * num9 * (1.f - num8);
                x.vel(x.k0) += a2 * num9 * (1.f - num8);
                tip(x).pos -= a2 * num9 * num8;
                tip(x).vel -= a2 * num9 * num8;
                if (consious)
                    x.vel(x.k0).y += w.physical_object_get_g(creature::body(x.state, x.c));
            }
        }

        // into its den (dens.hpp: after_update squeezes it in with what it
        // holds, which is eaten), or gone without one
        void withdraw(ctx& x, bool dead) {
            auto& w = x.state.world;
            if (dead || !x.p.has_den) {
                x.p.fresh = 2;
                x.state.defer(x.room.id, [c = x.c](sys::game_state& st) {
                    if (st.world.creature_is_valid(c))
                        creature::destroy(st, c);
                });
                return;
            }
            // (the hole pulls its chunks from the root, in sight of the pipe)
            for (auto const k : {x.k0, x.k1}) {
                physics::hard_set_position(x.state, k, x.b.root_pos);
                w.body_chunk_set_vel(k, vec2{0.f, 0.f});
            }
            w.creature_set_going_home(x.c, 1);
            w.creature_set_travel_tile(x.c, x.b.short_cut_pos);
        }

        // the end of PoleMimic.Update: hurt, it withdraws; holding prey, it
        // pulls it to its root and into the den
        void update_grasp(ctx& x) {
            auto& w = x.state.world;
            auto& p = x.p;
            bool const dead = w.creature_get_dead(x.c) != 0;
            if (dead || mathf::inverse_lerp(0.3f, 0.65f, w.creature_get_health(x.c)) < 1.f - p.aggression) {
                w.creature_set_stun(x.c, 0);
                p.extended = mathf::max(0.f, p.extended - 0.016666668f);
                release_grasp(x);
                if (p.extended <= 0.f) {
                    withdraw(x, dead);
                    return;
                }
            }
            if (creature::grasped_object(w, x.c, 0)) {
                auto const t = creature::grasped(w, x.c, 0);
                auto const g = t ? target_chunk(x, t, creature::grasp_chunk(w, x.c, 0)) : dcon::body_chunk_id{};
                if (!g || total_rope(x) > 600.f || w.creature_get_entering_shortcut(t).x >= 0) {
                    release_grasp(x);
                    return;
                }
                carry(x, g);
                vec2& gp = w.body_chunk_get_pos(g);
                float const dist = rw::distance(gp, x.b.root_pos);
                p.extended -= 1.f / custom::lerp_map(dist, 150.f, 50.f, 280.f, 30.f);
                w.body_chunk_get_vel(g) += rw::clamp_magnitude(x.b.root_pos - gp, mathf::lerp(25.f, 400.f, p.force_into_shortcut)) * (1.f - p.extended) / 20.f
                    / mathf::lerp(w.body_chunk_get_mass(g), 0.1f, p.force_into_shortcut) * mathf::inverse_lerp(200.f, 50.f, dist);
                if (p.extended < 0.f) {
                    if (creature::static_relationship(x.state, x.c, t).type != creature::rel::eats) {
                        release_grasp(x);
                        p.extended = 0.f;
                        p.force_into_shortcut = 0.f;
                        return;
                    }
                    p.extended = 0.f;
                    p.force_into_shortcut += mathf::inverse_lerp(200.f, 50.f, dist) / 30.f;
                    if (custom::dist_less(gp, x.b.root_pos, 20.f) || p.force_into_shortcut > 1.f) {
                        // (without a den it keeps it pinned at its root)
                        if (p.has_den) {
                            w.creature_set_going_home(x.c, 1);
                            w.creature_set_travel_tile(x.c, x.b.short_cut_pos);
                        }
                        p.force_into_shortcut = 0.f;
                    }
                } else {
                    p.force_into_shortcut = 0.f;
                }
            } else if (!dead) {
                p.extended = 1.f;
                p.force_into_shortcut = 0.f;
            }
        }

        // where the stalk roots: a pipe entrance (out of it, the vanilla
        // den spawn), else the terrain closest to `at` in the four
        // directions (below first), the stalk standing away from it
        struct root_spot {
            ivec2 base{0, 0};
            ivec2 dir{0, 1};
            bool entrance = false;
        };
        root_spot find_root(room_view const& room, vec2 at) {
            ivec2 const t = room_view::get_tile_position(at);
            if (room.inside(t.x, t.y) && room.get_tile(t).t == world::terrain::shortcut_entrance) {
                ivec2 const d = room.shortcut_entrance_hole_direction(t);
                if (d.x != 0 || d.y != 0)
                    return root_spot{t, d, true};
            }
            constexpr ivec2 dirs[4] = {{0, -1}, {-1, 0}, {1, 0}, {0, 1}};
            root_spot best{t + ivec2{0, -1}, ivec2{0, 1}, false};
            int32_t best_k = std::numeric_limits<int32_t>::max();
            if (has_any_solid(room, t))
                return best;
            for (auto const s : dirs)
                for (int32_t k = 1; k <= 10; ++k) {
                    ivec2 const q = t + s * k;
                    if (!room.inside(q.x, q.y))
                        break;
                    if (has_any_solid(room, q)) {
                        if (k < best_k) {
                            best_k = k;
                            best = root_spot{q, ivec2{-s.x, -s.y}, false};
                        }
                        break;
                    }
                }
            return best;
        }

        // PoleMimic.Initiate (the tentacle, laid along the line as a pole)
        void initiate(sys::game_state& state, dcon::creature_id c, room_view const& room, vec2 at) {
            auto& w = state.world;
            auto const d = data(state, c);
            auto& p = pl(state, c);
            root_spot const rs = find_root(room, at);
            auto& b = w.pole_mimic_data_get_body(d);
            b = body_state{};
            b.short_cut_pos = rs.base;
            ivec2 const dir = rs.dir;
            b.stick_out_dir = rw::to_vec2(dir);
            b.root_pos = room_view::middle_of_tile(rs.base) + b.stick_out_dir * 30.f;
            p.has_den = rs.entrance && w.creature_get_home_room(c) == room.id ? 1 : 0;
            // every second tile of the line until the terrain
            std::vector<ivec2> list;
            ivec2 iv = rs.base;
            bool flag = false;
            int32_t num3 = 0;
            p.free_standing = 0;
            while (!has_any_solid(room, iv + dir) && room.inside(iv.x + dir.x, iv.y + dir.y)) {
                iv += dir;
                if (flag)
                    list.push_back(iv);
                flag = !flag;
                if (++num3 >= max_walk) {
                    p.free_standing = 1;
                    break;
                }
            }
            if (list.size() < 2)
                list.insert(list.begin(), rs.base);
            // (a single tile: the base twice, a stalk needs two chunks)
            if (list.size() < 2)
                list.push_back(rs.base + dir);
            w.pole_mimic_data_get_tiles(d).load_range(list.data(), list.data() + list.size());
            b.tip_pos = room_view::middle_of_tile(iv) + b.stick_out_dir * 10.f;
            b.length = rw::distance(b.root_pos, b.tip_pos);
            b.ideal_length = float(list.size()) * 40.f;
            uint32_t const n = uint32_t(list.size());
            auto chunks = w.pole_mimic_data_get_chunks(d);
            chunks.resize(n);
            for (uint32_t i = 0; i < n; ++i) {
                tentacle_chunk tc;
                tc.t_pos = float(i + 1) / float(n);
                tc.rad = rad;
                chunks.at(i) = tc;
            }
            auto sticks = w.pole_mimic_data_get_sticks(d);
            sticks.resize(n);
            for (uint32_t i = 0; i < n; ++i)
                sticks.at(i) = stick{};
            physics::hard_set_position(state, creature::chunk(state, c, 1), b.root_pos);
            {
                ctx x(state, c, room);
                x.prox = &pathing::ai_terrain_proximity_map(state, room.id);
                tentacle_new_room(x);
                for (uint32_t j = 0; j < n; ++j) {
                    x.ch[j].pos = room_view::middle_of_tile(list[j]);
                    x.ch[j].last_pos = x.ch[j].pos;
                }
                x.segments.clear();
                for (uint32_t k = 0; k < n; ++k) {
                    x.segments.push_back(list[k]);
                    if (k < n - 1)
                        x.segments.push_back(list[k] - custom::clamp_at_one(list[k] - list[k + 1]));
                }
                move_grab_dest(x, b.tip_pos);
                x.store();
            }
            physics::hard_set_position(state, creature::chunk(state, c, 0), b.tip_pos);
            p.tip_attached = 1;
            p.mimic = 1.f;
            p.mimic_delay_counter = 40;
            p.extended = 1.f;
            p.hunt = dcon::creature_id{};
        }

        // PoleMimic.NewRoom with the tentacle made
        void new_room(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& p = pl(state, c);
            p.hunt = dcon::creature_id{};
            p.tip_attached = 0;
            ctx x(state, c, room);
            x.prox = &pathing::ai_terrain_proximity_map(state, room.id);
            physics::hard_set_position(state, x.k1, x.b.root_pos);
            tentacle_new_room(x);
            tentacle_reset(x, x.b.root_pos);
            move_grab_dest(x, x.b.tip_pos);
            x.b.ideal_length = x.b.length;
            for (uint32_t i = 0; i < x.ch.size(); ++i)
                x.ch[i].vel = x.b.stick_out_dir * mathf::clamp(float(i), 1.f, 15.f);
            for (auto& s : x.st)
                s = stick{};
            x.store();
        }

        // Custom.PushFromHalf
        float push_from_half(float v, float e) {
            if (v == 0.5f)
                return 0.5f;
            if (v < 0.5f)
                return custom::lerp_map(v, 0.f, 0.5f, 0.f, 0.5f, e);
            return custom::lerp_map(v, 1.f, 0.5f, 1.f, 0.5f, e);
        }

        // where two segments cross
        std::optional<vec2> segment_intersection(vec2 a1, vec2 a2, vec2 b1, vec2 b2) {
            vec2 const r = a2 - a1, s = b2 - b1;
            float const den = r.x * s.y - r.y * s.x;
            if (std::fabs(den) < 1e-6f)
                return std::nullopt;
            vec2 const q = b1 - a1;
            float const t = (q.x * s.y - q.y * s.x) / den;
            float const u = (q.x * r.y - q.y * r.x) / den;
            if (t < 0.f || t > 1.f || u < 0.f || u > 1.f)
                return std::nullopt;
            return a1 + r * t;
        }
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::pole_mimic);
        auto const d = data(state, c);
        auto rng = rw::random(state.seed, 0, uint32_t(c.value));
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
        // two chunks of rad 2, 0.2 each: the tip (main) and the root
        physics::add_chunk(state, o, at, rad, chunk_mass);
        auto const c1 = physics::add_chunk(state, o, at, rad, chunk_mass);
        w.body_chunk_set_collide_with_terrain(c1, false);
        physics::set_go_through_floors(state, o, true);
        w.try_create_creature_body(c, o);
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_set_grasps(c, creature::grasp_pair{});
        w.creature_set_destination(c, room_view::get_tile_position(at));
        w.pole_mimic_data_set_seed(d, seed);
        w.pole_mimic_data_set_graphics(d, graphics_state{});
        auto& p = pl(state, c);
        {
            uint16_t const keep = p.data;
            p = creature::pole_mimic_payload{};
            p.data = keep;
        }
        // AbstractCreature.Personality(ID): aggression
        {
            rw::unity_random r{seed};
            float const sympathy = push_from_half(r.value(), 1.5f);
            float const energy = push_from_half(r.value(), 1.5f);
            float const bravery = push_from_half(r.value(), 1.5f);
            (void)mathf::lerp(r.value(), mathf::lerp(energy, 1.f - bravery, 0.5f), mathf::pow(r.value(), 0.25f));
            float const a1 = r.value();
            float const a2 = r.value();
            p.aggression = push_from_half(mathf::lerp(a1, (energy + bravery) / 2.f * (1.f - sympathy), mathf::pow(a2, 0.25f)), 2.5f);
        }
        p.extended = 1.f;
        initiate(state, c, world::get_room_view(state, room), at);
        // out of a den later: placed as a pole then (dens.cpp)
        p.fresh = 1;
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        creature::release_target(state, c);
        creature::erase(state, c);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto& p = pl(state, c);
        if (p.fresh == 2)
            return;
        p.fresh = 0;
        // Creature.Update: the stun, the body's physics
        creature::stun_countdown(state, c);
        physics::update_object(state, room, creature::body(state, c), nullptr, nullptr);
        ctx x(state, c, room);
        if (x.ch.size() < 2 || x.tiles.size() != x.ch.size() || x.st.size() != x.ch.size())
            return;
        x.prox = &pathing::ai_terrain_proximity_map(state, room.id);
        p.just_out_of_shortcut = 0;
        if (p.angered_and_aggressive > 0) {
            if (w.creature_get_stun(c) < 1)
                --p.angered_and_aggressive;
            p.wake_up_counter = std::max(p.wake_up_counter, p.angered_and_aggressive);
        }
        tentacle_update(x);
        bool const consious = x.consious();
        x.b.limp = consious ? 0 : 1;
        x.b.retract_fac = mathf::clamp01(1.f - p.extended);
        w.body_chunk_set_collide_with_terrain(x.k0, p.extended == 1.f && x.b.backtrack_from == -1);
        float const reach = x.b.ideal_length * 2.f * p.extended;
        if (!custom::dist_less(x.pos(x.k1), x.pos(x.k0), reach))
            x.pos(x.k0) = x.pos(x.k1) + custom::dir_vec(x.pos(x.k1), x.pos(x.k0)) * reach;
        if (consious)
            act(x);
        else
            p.tip_attached = 0;
        update_body(x);
        update_grasp(x);
        x.store();
    }

    bool collides(sys::game_state const&, dcon::creature_id, dcon::creature_id) { return true; }

    void collide(sys::game_state& state, dcon::creature_id c, room_view const&, uint32_t i, dcon::creature_id t, uint32_t j) {
        auto& w = state.world;
        auto const tt = w.creature_get_type(t);
        if (tt == creature::type::tentacle_plant || tt == creature::type::pole_mimic)
            return;
        if (i > 0 || creature::grasped_object(w, c, 0))
            return;
        if (!creature::live(state, t) || j >= creature::chunk_count_of(state, t))
            return;
        auto const k = creature::chunk_of(state, t, j);
        vec2& v = w.body_chunk_get_vel(k);
        v = rw::lerp(v, w.body_chunk_get_vel(creature::chunk(state, c, 0)), 0.2f / w.body_chunk_get_mass(k));
        if (creature::consious(state, c) && wake_up(pl(state, c)) > 0.3f) {
            // (Pole_Mimic_Grab_Player / _Creature: no sample ids)
            creature::grab(state, c, 0, t, j);
        }
    }

    void violence(sys::game_state& state, dcon::creature_id c, uint32_t k, vec2 momentum, float damage, float stun_bonus, creature::damage_type type) {
        creature::hit_part part;
        part.chunk = int32_t(k);
        // (qualified: creature::violence is found by ADL too)
        pole_mimic::violence(state, c, part, momentum, damage, stun_bonus, type);
    }

    void violence(sys::game_state& state, dcon::creature_id c, creature::hit_part const& part, vec2 momentum, float damage, float stun_bonus,
        creature::damage_type type) {
        auto& w = state.world;
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        if (part.chunk >= 0 && uint32_t(part.chunk) < creature::chunk_count(state, c)) {
            auto const hit = creature::chunk(state, c, uint32_t(part.chunk));
            w.body_chunk_get_vel(hit) += rw::clamp_magnitude(momentum / w.body_chunk_get_mass(hit), 10.f);
        } else if (part.tentacle >= 0) {
            // PoleMimic.ApplyForceOnAppendage
            auto chunks = w.pole_mimic_data_get_chunks(data(state, c));
            int32_t const ps = part.prev_segment;
            float const dn = part.distance_to_next;
            if (ps > 0 && uint32_t(ps - 1) < chunks.size()) {
                auto& tc = chunks.at(uint32_t(ps - 1));
                tc.pos += momentum / 0.1f * (1.f - dn);
                tc.vel += momentum / 0.05f * (1.f - dn);
            }
            if (ps >= 0 && uint32_t(ps) < chunks.size()) {
                auto& tc = chunks.at(uint32_t(ps));
                tc.pos += momentum / 0.1f * dn;
                tc.vel += momentum / 0.05f * dn;
            }
        }
        // Creature.Violence (HealthState)
        float const dmg = damage / base_damage_resistance;
        float st = (damage * 30.f + stun_bonus) / base_stun_resistance;
        float& health = w.creature_get_health(c);
        st *= 1.5f + mathf::inverse_lerp(0.5f, 0.f, health) * rng.value();
        // PoleMimic.Stun: it lets go
        creature::stun(state, c, int32_t(st));
        creature::release_grasp(state, c, 0);
        health -= dmg;
        if (rng.value() < -health || health < -1.f || (health < 0.f && rng.value() < 0.33f) || dmg >= instant_death_damage_limit)
            creature::die(state, c);
        // PoleMimic.HitByWeapon (what is thrown at it: stabs and blows)
        if (type == creature::damage_type::stab || type == creature::damage_type::blunt) {
            auto& p = pl(state, c);
            p.angered_and_aggressive = std::max(p.angered_and_aggressive, rng.range(80, 120));
        }
    }

    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t) {
        auto& p = pl(state, c);
        if (p.hunt == t)
            p.hunt = dcon::creature_id{};
        for (auto& s : state.world.pole_mimic_data_get_sticks(data(state, c)))
            if (s.who == t)
                s = stick{};
    }

    void died(sys::game_state& state, dcon::creature_id c) {
        // (Creature.Die let go of everything; the stuck chunks come loose)
        for (auto& s : state.world.pole_mimic_data_get_sticks(data(state, c)))
            s = stick{};
    }

    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, vec2 at) {
        auto& p = pl(state, c);
        auto const view = world::get_room_view(state, room);
        if (p.fresh == 1) {
            // the world's spawn out of its den: placed as a pole
            // (AbstractCreature.RealizeInRoom's PlaceInRoom: Initiate)
            initiate(state, c, view, at);
            p.fresh = 0;
            return;
        }
        // SpitOutOfShortCut: NewRoom, awake, not mimicking
        new_room(state, c, view);
        p.just_out_of_shortcut = 1;
        p.wake_up_counter = 250;
        p.mimic = 0.f;
        p.mimic_delay_counter = 0;
        state.world.pole_mimic_data_get_graphics(data(state, c)).look_like_a_pole = 0.f;
    }

    std::string describe(sys::game_state const& state, dcon::creature_id c) {
        auto const& p = pl(state, c);
        std::string s = "pole plant";
        s += " tiles " + std::to_string(state.world.pole_mimic_data_get_tiles(data(state, c)).size());
        s += p.mimic > 0.5f ? " mimicking" : " revealed";
        s += " wake " + std::to_string(p.wake_up_counter);
        if (creature::grasped_object(state.world, c, 0))
            s += " holding";
        if (p.has_den)
            s += " den";
        return s;
    }

    bool spawn_spot(room_view const& room, ivec2 tile) {
        // on a floor, with room above to stand up in
        if (has_any_solid(room, tile) || !has_any_solid(room, tile + ivec2{0, -1}))
            return false;
        for (int32_t k = 1; k <= 4; ++k)
            if (!room.inside(tile.x, tile.y + k) || has_any_solid(room, tile + ivec2{0, k}))
                return false;
        return true;
    }

    //
    // IClimbableVine
    //
    uint32_t vine_positions(sys::game_state const& state, dcon::creature_id c) {
        return state.world.pole_mimic_data_get_chunks(data(state, c)).size();
    }

    vec2 vine_pos(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
        return state.world.pole_mimic_data_get_chunks(data(state, c)).at(i).pos;
    }

    void vine_push(sys::game_state& state, dcon::creature_id c, uint32_t i, vec2 movement) {
        auto chunks = state.world.pole_mimic_data_get_chunks(data(state, c));
        if (i < chunks.size())
            chunks.at(i).vel += movement / 0.3f;
    }

    bool currently_climbable(sys::game_state const& state, dcon::creature_id c) {
        auto const& w = state.world;
        auto const& p = pl(state, c);
        return w.creature_get_away(c) == 0 && p.fresh == 0 && vine_positions(state, c) >= 2 && wake_up(p) < 1.f;
    }

    void being_climbed_on(sys::game_state& state, dcon::creature_id c, dcon::creature_id) {
        auto& p = pl(state, c);
        p.mimic_delay_counter = 0;
        p.mimic = 0.f;
        // (Player_Grab_Pole_Mimic: no sample id)
        p.want_to_wake_up = 1;
        if (p.free_standing)
            p.tip_attached = 0;
    }

    bool trace_stalk(sys::game_state const& state, dcon::room_id room, vec2 from, vec2 to, dcon::creature_id& hit, creature::hit_part& part, float& at) {
        auto const& w = state.world;
        float best = 2.f;
        float const len = rw::distance(from, to);
        rooms::for_each_creature(state, room, creature::type::pole_mimic, [&](dcon::creature_id c) {
            if (w.creature_get_away(c) != 0 || pl(state, c).fresh != 0)
                return;
            auto const cs = w.pole_mimic_data_get_chunks(data(state, c));
            // AppendagePosition: the root, then the tentacle chunks
            vec2 prev = w.pole_mimic_data_get_body(data(state, c)).root_pos;
            for (uint32_t s = 0; s < cs.size(); ++s) {
                vec2 const cur = cs.at(s).pos;
                if (auto const hp = segment_intersection(from, to, prev, cur)) {
                    float const f = len > 0.f ? rw::distance(from, *hp) / len : 0.f;
                    if (f < best) {
                        best = f;
                        hit = c;
                        float const seg = rw::distance(prev, cur);
                        part = creature::hit_part{-1, 0, int32_t(s), seg > 0.f ? rw::distance(prev, *hp) / seg : 0.f};
                    }
                }
                prev = cur;
            }
        });
        at = best;
        return best <= 1.f;
    }
}
