// Deer.cs, DeerAI.cs, DeerTentacle.cs (on Tentacle.cs) and the bits of
// DeerAbstractAI (sporePos, damageGoHome) a realised deer uses.
//
// The body: the head (chunk 0, mainBodyChunk), four body chunks and the
// antlers (chunk 5, out of the object collisions). Its four legs are
// Tentacles: a list of room tiles from the body chunk out to a grip on the
// floor (`segments`, kept four way connected) with six TentacleChunks
// following them; FindGrabPos picks a grip below and ahead, the tip sticks
// there (attachedAtTip) and the legs' Support() carries the body against
// gravity. The legs work on copies of their tile lists while the deer
// updates (`leg_work`), stored back after.

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

#include "deer.hpp"
#include "daddy_internal.hpp"
#include "deer_graphics.hpp"
#include "dens.hpp"
#include "items.hpp"
#include "migrate.hpp"
#include "outposts.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "puff_ball.hpp"
#include "rooms.hpp"

namespace deer {
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;
    using daddy::detail::ray_trace_tiles_for_terrain;
    using daddy::detail::ray_trace_tiles_list;
    using daddy::detail::ray_traced_tiles;

    namespace {
        using input_ref = player::input_package const&;
        constexpr float float_max = std::numeric_limits<float>::max();
        constexpr ivec2 four_directions_and_zero[5] = {{0, 0}, {-1, 0}, {0, -1}, {1, 0}, {0, 1}};
        // PhysicalObject.gravity (Deer constructor)
        constexpr float deer_gravity = 0.9f;
        // CreatureTemplate: baseDamageResistance, baseStunResistance
        constexpr float base_damage_resistance = 200.f;
        constexpr float base_stun_resistance = 8.f;
        // Deer constructor: DeerTentacle length (no Remix)
        constexpr float leg_length = 220.f;
        // StuckTracker parameters (DeerAI constructor)
        constexpr int32_t min_stuck_counter = 140;
        constexpr int32_t max_stuck_counter = 240;
        constexpr int32_t goal_satisfaction_distance = 7;
        constexpr int32_t past_stuck_positions_close = 35;
        constexpr float past_pos_stuck_distance = 4.f;

        // DeerTentacle's Tentacle.TentacleProps(false, false, true, 0.5f, 0f,
        // 0.2f, 0.1f, 1.04f, 1.2f, 10f, 1f, 5f, 15, 60, 12, 0)
        constexpr float mass_deterioration_per_chunk = 0.5f;
        constexpr float goal_attraction_speed_tip = 0.2f;
        constexpr float goal_attraction_speed = 0.1f;
        constexpr float align_to_segment_speed = 1.04f;
        constexpr float backtrack_speed = 1.2f;
        constexpr float chunk_velocity_cap = 10.f;
        constexpr float tile_tentacle_update_speed = 1.f;
        constexpr float tile_tentacle_snap_with_path_distance = 5.f;
        constexpr int32_t segment_phase_through_terrain_frames = 15;
        constexpr int32_t max_pull_tiles_per_tick = 12;
        constexpr int32_t terrain_hits_before_phase = 0;
        // DeerTentacle.setGrabDelay
        constexpr int32_t set_grab_delay = 15;

        creature::deer_payload& pl(sys::game_state& state, dcon::creature_id c) { return state.world.creature_get_payload(c).deer; }
        creature::deer_payload const& pl(sys::game_state const& state, dcon::creature_id c) { return state.world.creature_get_payload(c).deer; }
        dcon::deer_data_id data(sys::game_state const& state, dcon::creature_id c) { return creature::deer_data(state.world, c); }

        bool has_any_solid(room_view const& room, ivec2 t) { return room.get_tile(t).solid(); }
        float float_dist(ivec2 a, ivec2 b) { return rw::magnitude(rw::to_vec2(a - b)); }
        bool are_neighbors(ivec2 a, ivec2 b) {
            return a != b && (a.x == b.x || a.y == b.y) && std::abs(a.x - b.x) < 2 && std::abs(a.y - b.y) < 2;
        }
        bool are_diagonal_neighbors(ivec2 a, ivec2 b) {
            return a.x != b.x && a.y != b.y && (std::abs(a.x - b.x) < 2 || std::abs(a.y - b.y) < 2);
        }
        vec2 restrict_in_rect(vec2 v, float left, float bottom, float right, float top) {
            return vec2{mathf::clamp(v.x, left, right), mathf::clamp(v.y, bottom, top)};
        }

        // SharedPhysics.RayTraceTilesForTerrainReturnFirstSolid
        std::optional<ivec2> first_solid(room_view const& room, ivec2 a, ivec2 b) {
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
                    return ivec2{x, y};
                if (e > 0) {
                    x += sx;
                    e -= num2;
                } else {
                    y += sy;
                    e += num;
                }
                --i;
            }
            return std::nullopt;
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

        // one leg while its deer updates
        struct leg_work {
            dcon::creature_part_id part;
            dcon::deer_leg_id id;
            uint32_t number = 0;
            leg t;
            std::vector<ivec2> segments;
            std::vector<ivec2> grab_path;
        };

        std::vector<leg_work>& work_buffer() {
            static thread_local std::vector<leg_work> buf;
            return buf;
        }
        std::vector<ivec2>& scratch_buffer() {
            static thread_local std::vector<ivec2> buf;
            return buf;
        }

        dcon::deer_leg_id leg_data(dcon::data_container const& w, dcon::creature_part_id p) {
            return dcon::deer_leg_id{dcon::deer_leg_id::value_base_t(w.creature_part_get_payload(p).deer_leg.data)};
        }

        // a deer's update context
        struct ctx {
            sys::game_state& state;
            dcon::creature_id c;
            room_view const& room;
            creature::deer_payload& p;
            dcon::deer_data_id d;
            ai_state& ai;
            rw::rng& rng;
            dcon::body_chunk_id ch[6];
            std::vector<leg_work>& legs;
            std::vector<ivec2>& scratch;
            std::optional<pathing::field_id> field;

            ctx(sys::game_state& s, dcon::creature_id cc, room_view const& r)
                : state(s), c(cc), room(r), p(pl(s, cc)), d(data(s, cc)), ai(s.world.deer_data_get_ai(data(s, cc))), rng(s.world.creature_get_random(cc)),
                  legs(work_buffer()), scratch(scratch_buffer()) {
                for (uint32_t i = 0; i < 6; ++i)
                    ch[i] = creature::chunk(s, cc, i);
            }
            dcon::data_container& w() { return state.world; }
            vec2& pos(uint32_t i) { return state.world.body_chunk_get_pos(ch[i]); }
            vec2& vel(uint32_t i) { return state.world.body_chunk_get_vel(ch[i]); }
            float mass(uint32_t i) { return state.world.body_chunk_get_mass(ch[i]); }
            float rad(uint32_t i) { return state.world.body_chunk_get_rad(ch[i]); }
            ivec2 contact(uint32_t i) { return state.world.body_chunk_get_contact_point(ch[i]); }
            // abstractCreature.pos
            ivec2 here() { return room_view::get_tile_position(pos(0)); }
            bool consious() { return creature::consious(state, c); }
            ivec2 destination() { return state.world.creature_get_destination(c); }
            pathing::field_id path() {
                if (!field)
                    field = pathing::field_to(state, room.id, destination(), movement);
                return *field;
            }
            void set_destination(ivec2 t) {
                if (destination() != t) {
                    state.world.creature_set_destination(c, t);
                    field.reset();
                }
            }
            bool accessible(ivec2 t) { return room.inside(t.x, t.y) && pathing::accessible(room, t, movement); }
            // PathFinder.CoordinateReachableAndGetbackable (every open tile
            // connects both ways: reachable from the destination's field)
            bool reachable(ivec2 t) { return accessible(t) && pathing::distance(state, path(), t) != pathing::unreachable; }
            // AItile.floorAltitude (smoothedFloorAltitude: the same here)
            int32_t floor_altitude(ivec2 t) {
                return pathing::floor_altitude(room, ivec2{custom::int_clamp(t.x, 0, room.width - 1), custom::int_clamp(t.y, 0, room.height - 1)});
            }
            // PhysicalObject.WeightedPush
            void weighted_push(uint32_t a, uint32_t b, vec2 dir, float frc) {
                float const num = mass(b) / (mass(a) + mass(b));
                vel(a) += dir * frc * num;
                vel(b) -= dir * frc * (1.f - num);
            }
            // Deer.HeadDir
            vec2 head_dir() {
                vec2 const bd = p.bod_dir;
                vec2 const from = custom::perpendicular_vector(bd) * mathf::sign(bd.x);
                return rw::slerp(from, vec2{0.f, 1.f}, mathf::pow(mathf::inverse_lerp(1.f, 0.f, std::fabs(bd.x)), 0.5f));
            }
            // Deer.CloseToEdge
            float close_to_edge() {
                float const x = pos(0).x;
                float const pw = room.pixel_width();
                return mathf::max(mathf::inverse_lerp(500.f, 300.f, x), mathf::inverse_lerp(pw - 500.f, pw - 300.f, x));
            }
            // GetUnstuckForce: StuckTracker.Utility
            float unstuck_force() { return mathf::inverse_lerp(float(min_stuck_counter), float(max_stuck_counter), float(ai.stuck_counter)); }
            uint8_t rider_count() { return state.world.deer_data_get_riders(d).count; }
        };

        void load(ctx& x) {
            auto& w = x.w();
            x.legs.resize(leg_count);
            uint32_t k = 0;
            for (auto const o : creature::parts(w, x.c)) {
                auto const part = w.part_ownership_get_part(o);
                if (w.creature_part_get_kind(part) != creature::part_kind::deer_leg || k >= leg_count)
                    continue;
                auto& lw = x.legs[k];
                lw.part = part;
                lw.id = leg_data(w, part);
                lw.number = w.creature_part_get_index(part);
                lw.t = w.deer_leg_get_t(lw.id);
                auto const segs = w.deer_leg_get_segments(lw.id);
                lw.segments.assign(segs.begin(), segs.end());
                auto const gp = w.deer_leg_get_grab_path(lw.id);
                lw.grab_path.assign(gp.begin(), gp.end());
                ++k;
            }
            x.legs.resize(k);
        }
        void store(ctx& x) {
            auto& w = x.w();
            for (auto& lw : x.legs) {
                w.deer_leg_set_t(lw.id, lw.t);
                w.deer_leg_get_segments(lw.id).load_range(lw.segments.data(), lw.segments.data() + lw.segments.size());
                w.deer_leg_get_grab_path(lw.id).load_range(lw.grab_path.data(), lw.grab_path.data() + lw.grab_path.size());
            }
        }

        //
        // Tentacle
        //
        vec2 float_base(ctx& x, leg_work const& lw) { return x.pos(lw.t.connected_chunk); }
        leg_chunk& tip(leg_work& lw) { return lw.t.c[leg_chunks - 1]; }
        std::optional<ivec2> grab_dest(leg_work const& lw) {
            if (lw.t.float_grab_dest)
                return room_view::get_tile_position(*lw.t.float_grab_dest);
            return std::nullopt;
        }
        // Tentacle.BasePos (with CheckIfAnySegmentsLeft)
        ivec2 base_pos(ctx& x, leg_work& lw) {
            if (lw.segments.empty())
                lw.segments.push_back(room_view::get_tile_position(float_base(x, lw)));
            return lw.segments[0];
        }
        int32_t current_segment(leg_work const& lw, leg_chunk const& c) {
            int32_t const n = int32_t(lw.segments.size());
            return custom::int_clamp(int32_t(c.t_pos * float(n - 1)), 0, n - 1);
        }
        ivec2 stuck_pos(leg_work const& lw, leg_chunk const& c) { return lw.segments[size_t(current_segment(lw, c))]; }
        bool visual(ctx& x, ivec2 a, ivec2 b) { return ray_trace_tiles_for_terrain(x.room, a, b); }

        // TentacleChunk.Reset
        void chunk_reset(leg_work& lw, leg_chunk& c) {
            c.phase = -1.f;
            c.pos = room_view::middle_of_tile(stuck_pos(lw, c));
            c.last_pos = c.pos;
            c.vel = vec2{0.f, 0.f};
        }

        // TentacleChunk.Update (no rope: RopeActive is false)
        void chunk_update(ctx& x, leg_work& lw, uint32_t i) {
            auto& c = lw.t.c[i];
            auto& t = lw.t;
            float const n = float(leg_chunks);
            c.last_pos = c.pos;
            if (c.phase == -1.f) {
                if (c.phases_to_same_location > 0 && stuck_pos(lw, c) == c.after_phase_stuck_pos) {
                    vec2 const m = room_view::middle_of_tile(stuck_pos(lw, c));
                    c.pos = restrict_in_rect(c.pos, m.x - 9.f, m.y - 9.f, m.x + 9.f, m.y + 9.f);
                }
                float const ideal = t.ideal_length / n * mathf::lerp(1.f, 0.1f, t.retract_fac);
                if (i == 0) {
                    vec2 const a = custom::dir_vec(c.pos, float_base(x, lw));
                    float const num = rw::distance(c.pos, float_base(x, lw));
                    if (num > ideal) {
                        // (pullAtConnectionChunk is 0)
                        c.pos -= a * (ideal - num);
                        c.vel -= a * (ideal - num);
                    }
                    c.stretched_fac = t.ideal_length / n / mathf::max(1.f, num);
                } else {
                    auto& prev = t.c[i - 1];
                    vec2 const a = custom::dir_vec(c.pos, prev.pos);
                    float const num = rw::distance(c.pos, prev.pos);
                    if (num > ideal) {
                        c.pos -= a * (ideal - num) * (1.f - mass_deterioration_per_chunk);
                        c.vel -= a * (ideal - num) * (1.f - mass_deterioration_per_chunk);
                        prev.pos += a * (ideal - num) * mass_deterioration_per_chunk;
                        prev.vel += a * (ideal - num) * mass_deterioration_per_chunk;
                    }
                    c.stretched_fac = t.ideal_length / n / mathf::max(1.f, num);
                }
                ivec2 const here = room_view::get_tile_position(c.pos);
                int32_t const cs = current_segment(lw, c);
                if (t.backtrack_from == -1 && !visual(x, here, lw.segments[size_t(cs)]))
                    t.backtrack_from = int32_t(i);
                if (t.backtrack_from != -1 && int32_t(i) >= t.backtrack_from) {
                    bool flag = false;
                    for (int32_t k = 1; k <= cs && !flag; ++k) {
                        if (!visual(x, here, lw.segments[size_t(k)]) && visual(x, here, lw.segments[size_t(k - 1)])) {
                            c.vel += rw::clamp_magnitude(room_view::middle_of_tile(lw.segments[size_t(k - 1)]) - c.pos, 20.f) * backtrack_speed / 20.f;
                            flag = true;
                        }
                    }
                    if (!flag) {
                        vec2 const fb = i > 0 ? t.c[i - 1].pos : float_base(x, lw);
                        c.vel += rw::clamp_magnitude(fb - c.pos, 20.f) * backtrack_speed / 20.f;
                        if (i > 0)
                            t.c[i - 1].vel += rw::clamp_magnitude(fb - c.pos, 20.f) * backtrack_speed / 20.f;
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
                        for (int32_t k = cs; k >= 0; --k) {
                            ivec2 const s = lw.segments[size_t(k)];
                            if (visual(x, here, s)) {
                                c.vel += rw::clamp_magnitude(room_view::middle_of_tile(s) - c.pos, 20.f) * 0.005f;
                                break;
                            }
                        }
                    } else {
                        if (t.float_grab_dest) {
                            float const k = i == leg_chunks - 1 ? goal_attraction_speed_tip : goal_attraction_speed;
                            c.vel += rw::clamp_magnitude(*t.float_grab_dest - c.pos, 20.f) * k / 20.f;
                        }
                        c.vel += rw::clamp_magnitude(room_view::middle_of_tile(lw.segments[size_t(cs)]) - c.pos, 20.f) * align_to_segment_speed / 20.f;
                    }
                }
                c.vel = rw::clamp_magnitude(c.vel, chunk_velocity_cap);
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
                c.phase += 1.f / float(segment_phase_through_terrain_frames);
                if (c.phase <= 1.f)
                    c.pos = rw::lerp(c.phase_from, room_view::middle_of_tile(stuck_pos(lw, c)), mathf::min(1.f, c.phase));
                c.vel = vec2{0.f, 0.f};
                if (c.phase >= 1.f) {
                    c.phase = -1.f;
                    if (c.after_phase_stuck_pos == stuck_pos(lw, c))
                        ++c.phases_to_same_location;
                    else
                        c.phases_to_same_location = 0;
                    c.after_phase_stuck_pos = stuck_pos(lw, c);
                }
                if (i > 0 && t.c[i - 1].phase == -1.f) {
                    auto& prev = t.c[i - 1];
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
        bool move_segment(ctx& x, leg_work& lw, int32_t s, ivec2 dest, bool allow_occupied, bool allow_solid) {
            auto& seg = lw.segments;
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

        // Tentacle.PullAtTentacle (+ Shorten)
        void pull_at_tentacle(ctx& x, leg_work& lw, int32_t start, bool only_simple_pull, std::vector<ivec2>& path) {
            auto& t = lw.t;
            auto& seg = lw.segments;
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
                                    seg.erase(seg.begin() + num + 1, seg.begin() + i);
                                    for (int32_t q = num3 - 2; q > 0; --q)
                                        seg.insert(seg.begin() + num + 1, path[size_t(q)]);
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

        // Tentacle.FindTentaclePath (as daddy_tentacle.cpp's: a greedy best
        // first search from the goal, the open list a heap on (heuristic,
        // insertion order))
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
                for (auto const dd : custom::four_directions) {
                    ivec2 const q = p + dd;
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
        void move_aligned_segments_with_path(ctx& x, leg_work& lw, std::vector<ivec2>& path) {
            auto& seg = lw.segments;
            auto& gp = lw.grab_path;
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
            lw.t.pull_counter += num3 - 1;
            pull_at_tentacle(x, lw, num, true, path);
        }

        // Tentacle.FindGrabPath
        void find_grab_path(ctx& x, leg_work& lw, std::vector<ivec2>& path) {
            ivec2 const g = *grab_dest(lw);
            ivec2 const b = base_pos(x, lw);
            int32_t num;
            if (!x.room.inside(g.x, g.y) || !x.room.inside(b.x, b.y) || visual(x, b, g))
                num = ray_trace_tiles_list(b.x, b.y, g.x, g.y, lw.grab_path);
            else
                num = find_tentacle_path(x, b, g, lw.grab_path);
            if (int32_t(lw.grab_path.size()) > num)
                lw.grab_path.resize(size_t(num));
            move_aligned_segments_with_path(x, lw, path);
        }

        // Tentacle.MoveGrabDest
        void move_grab_dest(ctx& x, leg_work& lw, vec2 to, std::vector<ivec2>& path) {
            if (auto const g = grab_dest(lw); g && *g == room_view::get_tile_position(to))
                return;
            lw.t.float_grab_dest = to;
            find_grab_path(x, lw, path);
        }

        // Tentacle.Grow
        void grow(ctx& x, leg_work& lw, int32_t add) {
            auto& seg = lw.segments;
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
        void move_base(ctx& x, leg_work& lw, ivec2 to, std::vector<ivec2>& path) {
            auto& seg = lw.segments;
            if (has_any_solid(x.room, to) || to == base_pos(x, lw))
                return;
            if (are_neighbors(seg[0], to)) {
                int32_t num = -1;
                for (int32_t i = 1; i < int32_t(seg.size()) - 1; ++i)
                    if (seg[size_t(i)] == to)
                        num = i;
                if (num > 0) {
                    seg.erase(seg.begin(), seg.begin() + num);
                    grow(x, lw, num);
                } else {
                    seg.insert(seg.begin(), to);
                    ++lw.t.pull_counter;
                    if (lw.t.pull_counter > 4)
                        pull_at_tentacle(x, lw, 0, false, path);
                }
            } else {
                int32_t num2;
                if (!x.room.inside(seg[0].x, seg[0].y) || !x.room.inside(to.x, to.y) || visual(x, seg[0], to))
                    num2 = ray_trace_tiles_list(seg[0].x, seg[0].y, to.x, to.y, path);
                else
                    num2 = find_tentacle_path(x, seg[0], to, path);
                ivec2 b = seg[0];
                for (int32_t j = 1; j < num2; ++j) {
                    seg.insert(seg.begin(), seg[0] + (path[size_t(j)] - b));
                    b = path[size_t(j)];
                }
                lw.t.pull_counter += num2 - 1;
                if (lw.t.pull_counter > 4)
                    pull_at_tentacle(x, lw, 0, false, path);
            }
            if (grab_dest(lw))
                find_grab_path(x, lw, path);
        }

        // Tentacle.AlignWithGrabPath
        void align_with_grab_path(ctx& x, leg_work& lw, std::vector<ivec2>& path) {
            auto& seg = lw.segments;
            auto& gp = lw.grab_path;
            bool flag = false;
            for (int32_t i = std::min(int32_t(seg.size()) - 2, int32_t(gp.size()) - 1); i > 0; --i) {
                if (i >= int32_t(seg.size()) || i >= int32_t(gp.size()))
                    continue;
                if (seg[size_t(i)] != gp[size_t(i)] && visual(x, seg[size_t(i)], gp[size_t(i)])) {
                    ray_trace_tiles_list(seg[size_t(i)].x, seg[size_t(i)].y, gp[size_t(i)].x, gp[size_t(i)].y, path);
                    if (path.size() > 1 && move_segment(x, lw, i + 1, path[1], false, false))
                        flag = true;
                }
            }
            if (!flag)
                for (int32_t k = 1; k < int32_t(seg.size()) && k < int32_t(gp.size()); ++k)
                    if (seg[size_t(k)] != gp[size_t(k)]) {
                        move_segment(x, lw, k, gp[size_t(k)], true, false);
                        return;
                    }
        }

        // Tentacle.CurrentLength
        float current_length(leg_work const& lw) {
            float num = float(lw.segments.size()) * 20.f;
            for (size_t i = 0; i + 2 < lw.segments.size(); ++i)
                if (are_diagonal_neighbors(lw.segments[i], lw.segments[i + 2]))
                    num -= 5.9000006f;
            return num;
        }

        // Tentacle.AdjustLength (`shorten` is on for the deer's legs)
        void adjust_length(ctx& x, leg_work& lw, std::vector<ivec2>& path) {
            float const num = current_length(lw);
            float num2 = lw.t.ideal_length;
            if (!lw.grab_path.empty())
                num2 = mathf::min(lw.t.ideal_length, float(lw.grab_path.size()) * 20.f);
            int32_t const num3 = int32_t(num2 / 20.f - num / 20.f);
            if (num3 > 0 && (lw.grab_path.empty() || lw.segments.size() < lw.grab_path.size())) {
                grow(x, lw, 1);
                return;
            }
            if (num3 < 0) {
                ++lw.t.pull_counter;
                pull_at_tentacle(x, lw, 0, false, path);
            }
        }

        // DeerTentacle.GravityDirection, Tentacle.Gravity
        void gravity(ctx& x, leg_work& lw) {
            ivec2 const b = x.rng.value() >= 0.5f ? ivec2{0, -1} : ivec2{int32_t(mathf::sign(lw.t.tentacle_dir.x)), -1};
            bool flag = false;
            for (int32_t i = 0; i < int32_t(lw.segments.size()) && !flag; ++i)
                if (move_segment(x, lw, i, lw.segments[size_t(i)] + b, false, false))
                    flag = true;
            if (!flag)
                for (int32_t i = 0; i < int32_t(lw.segments.size()); ++i)
                    if (move_segment(x, lw, i, lw.segments[size_t(i)] + b, true, false))
                        break;
        }

        // Tentacle.Update
        void base_update(ctx& x, leg_work& lw) {
            auto& path = x.scratch;
            base_pos(x, lw);
            move_base(x, lw, room_view::get_tile_position(float_base(x, lw)), path);
            lw.t.update_counter += tile_tentacle_update_speed;
            if (lw.t.update_counter >= 1.f) {
                lw.t.pulls_this_tick = max_pull_tiles_per_tick;
                if (lw.t.limp)
                    gravity(x, lw);
                else if (!lw.grab_path.empty())
                    align_with_grab_path(x, lw, path);
                adjust_length(x, lw, path);
                lw.t.update_counter -= 1.f;
            }
            lw.t.backtrack_from = -1;
            for (uint32_t i = 0; i < leg_chunks; ++i)
                chunk_update(x, lw, i);
        }

        //
        // DeerTentacle
        //
        // OtherTentacleInPair (sic: the third leg is its own pair)
        leg_work& other_in_pair(ctx& x, leg_work const& lw) {
            int32_t const n = int32_t(lw.number);
            int32_t const o = n < 2 ? 1 - n : 2 + (1 - (n - 1));
            for (auto& l : x.legs)
                if (int32_t(l.number) == o)
                    return l;
            return x.legs[0];
        }

        // DeerTentacle.ReleaseGrip
        void release_grip(ctx& x, leg_work& lw) {
            if (other_in_pair(x, lw).t.grab_delay < 1 && lw.t.grab_delay < 1)
                lw.t.grab_delay = set_grab_delay;
            lw.t.float_grab_dest.reset();
            lw.t.sure_of_footing_counter = 0;
        }

        // DeerTentacle.UpdateDesiredGrabPos
        void update_desired_grab_pos(ctx& x, leg_work& lw) {
            float const reach = mathf::min(x.p.next_floor_height, lw.t.max_length * 0.9f);
            vec2 const conn = float_base(x, lw);
            if (x.p.stay_still) {
                lw.t.desired_grab_pos = conn + rw::normalized(vec2{0.f, -1.f} + lw.t.tentacle_dir * 0.5f) * reach;
                return;
            }
            lw.t.desired_grab_pos = conn + rw::normalized(vec2{0.f, -1.f} + x.p.move_direction * (lw.number < 2 ? 0.8f : 0.6f) + lw.t.tentacle_dir * 0.25f) * reach;
        }

        // DeerTentacle.GripPointAttractiveness
        float grip_point_attractiveness(ctx& x, leg_work const& lw, ivec2 pos) {
            float const d = float_dist(room_view::get_tile_position(lw.t.desired_grab_pos), pos);
            if (!has_any_solid(x.room, pos + ivec2{0, 1}))
                return 100.f / d;
            return 65.f / d;
        }

        // DeerTentacle.FindGrabPos
        void find_grab_pos(ctx& x, leg_work& lw) {
            if (lw.t.grab_delay > 0) {
                --lw.t.grab_delay;
                return;
            }
            std::optional<ivec2> iv;
            ivec2 const dt = room_view::get_tile_position(lw.t.desired_grab_pos);
            for (int32_t num = 0; num < 9 && !iv; ++num) {
                iv = first_solid(x.room, base_pos(x, lw), dt + custom::eight_directions_and_zero[num] * 5);
                if (iv && has_any_solid(x.room, *iv + ivec2{0, 1}))
                    iv.reset();
            }
            if (!iv)
                return;
            auto const g = grab_dest(lw);
            if (!g || grip_point_attractiveness(x, lw, *iv) > grip_point_attractiveness(x, lw, *g) * (x.p.stay_still ? 1.f : 3.f)) {
                vec2 const m = room_view::middle_of_tile(*iv);
                vec2 const fb = float_base(x, lw);
                move_grab_dest(x, lw, restrict_in_rect(fb + lw.t.tentacle_dir * rw::distance(fb, m) * 0.5f, m.x - 11.f, m.y - 11.f, m.x + 11.f, m.y + 11.f), x.scratch);
                lw.t.sure_of_footing_counter = 0;
                return;
            }
            ++lw.t.sure_of_footing_counter;
        }

        // DeerTentacle.Support
        float support(ctx& x, leg_work& lw) {
            if (!lw.t.attached_at_tip)
                return 0.f;
            float num = 0.f;
            float const bx = x.pos(2).x;
            for (auto& o : x.legs)
                if (&o != &lw && o.t.attached_at_tip && (tip(o).pos.x < bx) != (tip(lw).pos.x < bx))
                    num = mathf::max(num, mathf::pow(std::sin(mathf::inverse_lerp(400.f, 0.f, std::fabs(bx - tip(o).pos.x)) * mathf::pi), 0.3f));
            return mathf::inverse_lerp(mathf::lerp(0.5f, -1.f, num), 1.f, rw::dot(vec2{0.f, -1.f}, custom::dir_vec(float_base(x, lw), tip(lw).pos)));
        }

        // DeerTentacle.ReleaseScore
        float release_score(ctx& x, leg_work& lw) {
            float num;
            if (!grab_dest(lw))
                num = rw::distance(tip(lw).pos, lw.t.desired_grab_pos);
            else
                num = rw::distance(*lw.t.float_grab_dest, lw.t.desired_grab_pos) * 2.f;
            if (lw.t.attached_at_tip)
                num *= 2.f;
            if (!other_in_pair(x, lw).t.attached_at_tip)
                num /= 100.f;
            num *= 1.f + (float_base(x, lw).x - tip(lw).pos.x) * mathf::sign(x.p.move_direction.x) * 1.4f;
            return num / (1.f + support(x, lw) * 10.f);
        }

        // Tentacle.NewRoom + DeerTentacle.NewRoom
        void leg_new_room(ctx& x, leg_work& lw) {
            lw.segments.assign(size_t(std::max(1, int32_t(lw.t.ideal_length / 20.f))), room_view::get_tile_position(x.pos(0)));
            for (auto& c : lw.t.c)
                chunk_reset(lw, c);
            lw.grab_path.clear();
            lw.t.float_grab_dest.reset();
            lw.t.attached_at_tip = 0;
            vec2 const fb = float_base(x, lw);
            ivec2 const target = room_view::get_tile_position(fb + vec2{0.f, -400.f} + lw.t.tentacle_dir * 100.f);
            auto const iv = first_solid(x.room, base_pos(x, lw), target);
            if (!iv)
                return;
            // Custom.RestrictInRect(FloatBase, TileRect.Grow(2))
            vec2 const ng = restrict_in_rect(fb, float(iv->x) * 20.f - 2.f, float(iv->y) * 20.f - 2.f, float(iv->x) * 20.f + 22.f, float(iv->y) * 20.f + 22.f);
            ray_traced_tiles(fb, room_view::middle_of_tile(*iv), lw.segments);
            if (lw.segments.size() > 2)
                lw.segments.pop_back();
            if (lw.segments.empty())
                lw.segments.push_back(room_view::get_tile_position(fb));
            move_grab_dest(x, lw, ng, x.scratch);
            for (auto& c : lw.t.c)
                chunk_reset(lw, c);
            lw.t.attached_at_tip = 1;
            lw.t.smoothed_footing_security = 1.f;
        }

        // DeerTentacle.Update
        void leg_update(ctx& x, leg_work& lw) {
            auto& t = lw.t;
            base_update(x, lw);
            t.smoothed_footing_security = mathf::lerp(t.smoothed_footing_security, t.attached_at_tip ? 0.f : mathf::inverse_lerp(3.f, 30.f, float(t.sure_of_footing_counter)), 0.2f);
            t.attached_at_tip = 0;
            t.max_length = 20.f * x.p.preferred_height * 1.3333334f;
            vec2 const fb = float_base(x, lw);
            float const goal_len = grab_dest(lw) ? rw::distance(fb, *t.float_grab_dest) * 1.5f * mathf::inverse_lerp(float(set_grab_delay), 0.f, float(t.grab_delay)) : t.max_length;
            t.ideal_length = mathf::min(mathf::lerp(t.ideal_length, goal_len, 0.03f), t.max_length);
            t.retract_fac = x.p.resting == 1.f ? 1.f : 0.f;
            t.limp = x.consious() ? 0 : 1;
            if (t.limp) {
                t.float_grab_dest.reset();
                for (auto& c : t.c) {
                    c.vel *= 0.9f;
                    c.vel.y -= 0.5f;
                }
            }
            vec2 const hd = x.head_dir();
            for (uint32_t j = 0; j < leg_chunks; ++j) {
                auto& c = t.c[j];
                c.rad = 5.f;
                float const num = float(j) / float(leg_chunks - 1);
                c.vel *= 0.5f;
                if (t.backtrack_from == -1 || int32_t(j) < t.backtrack_from) {
                    if (j < leg_chunks - 1)
                        c.vel += hd * mathf::lerp(-1.f, 1.f, num) * mathf::pow(1.f - num, 1.8f);
                    c.vel += x.p.move_direction * std::sin(t.smoothed_footing_security * mathf::pi) * 1.5f * mathf::lerp(1.f, 2.5f, std::sin(num * mathf::pi));
                }
                c.pos = rw::lerp(c.pos, room_view::middle_of_tile(stuck_pos(lw, c)), 0.03f);
                if (x.room.point_submerged(c.pos))
                    c.vel *= 0.5f;
            }
            if (t.backtrack_from == -1)
                t.c[1].vel += x.p.move_direction;
            if (t.backtrack_from == -1 && grab_dest(lw))
                tip(lw).vel += custom::dir_vec(tip(lw).pos, *t.float_grab_dest) * mathf::lerp(0.2f, 38.f, t.smoothed_footing_security);
            if (t.float_grab_dest && custom::dist_less(tip(lw).pos, *t.float_grab_dest, 40.f) && t.backtrack_from == -1 && x.p.resting < 1.f) {
                tip(lw).pos = *t.float_grab_dest;
                tip(lw).vel = vec2{0.f, 0.f};
                t.attached_at_tip = 1;
            }
            tip(lw).collide_with_terrain = t.attached_at_tip ? 0 : 1;
            update_desired_grab_pos(x, lw);
            vec2 const conn_vel = x.vel(t.connected_chunk);
            for (auto& c : t.c) {
                c.vel.y -= 0.1f;
                c.vel += conn_vel * 0.1f;
                if (!t.attached_at_tip) {
                    if (t.float_grab_dest)
                        c.vel += custom::dir_vec(c.pos, *t.float_grab_dest) * 0.3f;
                    else
                        c.vel += custom::dir_vec(c.pos, t.desired_grab_pos + custom::dir_vec(fb, t.desired_grab_pos) * 70.f) * 0.6f;
                }
            }
            if (t.attached_at_tip) {
                t.frames_without_reaching = 0;
                if (ray_trace_tiles_for_terrain(x.room, base_pos(x, lw), *grab_dest(lw))) {
                    if (!custom::dist_less(tip(lw).pos, fb, t.max_length))
                        release_grip(x, lw);
                    if (!custom::dist_less(tip(lw).pos, fb, t.max_length * 0.9f))
                        x.p.held_back_by_leg = 1;
                } else {
                    release_grip(x, lw);
                }
                // (Vulture_Tentacle_Grab_Terrain has no sample here)
                t.play_step_sound = 0;
                t.step_sound_vol = 0.f;
            } else {
                t.step_sound_vol = mathf::min(1.f, t.step_sound_vol + 0.025f);
                t.play_step_sound = 1;
                find_grab_pos(x, lw);
                ++t.frames_without_reaching;
                if (float(t.frames_without_reaching) > 60.f && !t.float_grab_dest)
                    t.frames_without_reaching = 0;
            }
        }

        //
        // DeerAI
        //
        // StuckTracker.Update (past positions only)
        void stuck_update(ctx& x) {
            auto& a = x.ai;
            int32_t num = 0;
            ivec2 const here = x.here();
            if (!a.satisfied && custom::manhattan_distance(here, x.destination()) > goal_satisfaction_distance) {
                a.past_head = uint8_t((a.past_head + stuck_positions - 1) % stuck_positions);
                a.past[a.past_head] = here;
                if (a.past_count < stuck_positions)
                    ++a.past_count;
                for (uint32_t j = 0; j < a.past_count; ++j)
                    if (float_dist(here, a.past[(a.past_head + j) % stuck_positions]) < past_pos_stuck_distance)
                        ++num;
            }
            if (num > past_stuck_positions_close)
                ++a.stuck_counter;
            else
                a.stuck_counter -= 2;
            a.stuck_counter = custom::int_clamp(a.stuck_counter, 0, max_stuck_counter);
        }

        // a free puff ball of the room (PuffBallLegal: not held)
        bool puff_ball_legal(ctx& x, dcon::item_id it) {
            auto const& w = x.w();
            return it && w.item_is_valid(it) && w.item_get_type(it) == world::item_type::puff_ball && !w.item_get_grabbed_by(it)
                && w.physical_object_get_room_from_object_location(items::body(x.state, it)) == x.room.id;
        }

        // ItemRepresentation.BestGuessForPosition
        ivec2 puff_best_guess(ctx& x) {
            if (x.ai.puff_visible && puff_ball_legal(x, x.ai.puff))
                return room_view::get_tile_position(x.w().body_chunk_get_pos(items::chunk(x.state, x.ai.puff)));
            return x.ai.puff_seen;
        }

        // DeerAI.FindGotoPuffBall over the puff balls it sees (the
        // ItemTracker: the free puff balls of the room in view; the one it
        // goes for is remembered where it was last seen)
        void find_goto_puff_ball(ctx& x) {
            auto& a = x.ai;
            auto& w = x.w();
            if (a.puff && !puff_ball_legal(x, a.puff))
                a.puff = dcon::item_id{};
            vec2 const head = x.pos(0);
            a.puff_visible = 0;
            if (a.puff) {
                vec2 const ip = w.body_chunk_get_pos(items::chunk(x.state, a.puff));
                if (custom::dist_less(head, ip, 1450.f) && x.room.visual_contact(head, ip)) {
                    a.puff_visible = 1;
                    a.puff_seen = room_view::get_tile_position(ip);
                }
            }
            float best = float_max;
            dcon::item_id pick;
            bool pick_visible = false;
            rooms::for_each_item(x.state, x.room.id, [&](dcon::item_id it) {
                if (w.item_get_type(it) != world::item_type::puff_ball || !puff_ball_legal(x, it))
                    return;
                auto const ic = items::chunk(x.state, it);
                vec2 const ip = w.body_chunk_get_pos(ic);
                bool const vis = it == a.puff ? a.puff_visible != 0 : (custom::dist_less(head, ip, 1450.f) && x.room.visual_contact(head, ip));
                if (!vis && it != a.puff)
                    return;
                ivec2 const guess = it == a.puff ? puff_best_guess(x) : room_view::get_tile_position(ip);
                float num2 = rw::distance(head, room_view::middle_of_tile(guess));
                if (vis)
                    num2 -= rw::magnitude(w.body_chunk_get_vel(ic)) * 5.f;
                if (it == a.puff)
                    num2 -= 300.f;
                if (!x.reachable(guess + ivec2{0, 2}))
                    num2 += 3000.f;
                if (guess == a.cant_eat_from)
                    num2 = float_max;
                if (num2 < best) {
                    best = num2;
                    pick = it;
                    pick_visible = vis;
                }
            });
            if (pick && pick != a.puff) {
                a.puff = pick;
                a.puff_visible = pick_visible ? 1 : 0;
                a.puff_seen = room_view::get_tile_position(w.body_chunk_get_pos(items::chunk(x.state, pick)));
            }
        }

        // DeerAI.WormGrassUnderPos, LegalInRoomDest, GoodInRoomDest
        bool worm_grass_under(ctx& x, ivec2 t) {
            for (int32_t i = t.y; i >= 0; --i) {
                auto const tile = x.room.get_tile(t.x, i);
                if (tile.solid())
                    return false;
                if (tile.flags & world::tile_worm_grass)
                    return true;
            }
            return false;
        }
        bool legal_in_room_dest(ctx& x, ivec2 t) {
            int32_t const margin = x.rider_count() > 0 ? 15 : 5;
            return x.reachable(t) && !custom::dist_less(rw::to_vec2(x.here()), rw::to_vec2(t), 10.f) && t.x >= margin && t.x <= x.room.width - margin;
        }
        bool good_in_room_dest(ctx& x, ivec2 t) {
            if (!legal_in_room_dest(x, t))
                return false;
            if (x.rider_count() > 0 && x.room.width > 180) {
                if (std::abs(x.here().x - t.x) < x.room.width / 3)
                    return false;
                if (t.x > 70 && t.x < x.room.width - 70)
                    return false;
            }
            bool const a = worm_grass_under(x, x.ai.in_room_destination);
            bool const b = worm_grass_under(x, t);
            return a == b || !b;
        }

        // DeerAI.CheckRestSpot (the dominance check against other deer left out)
        void check_rest_spot(ctx& x, ivec2 t) {
            if (x.rng.value() > 0.16666667f || !x.room.inside(t.x, t.y) || x.floor_altitude(t) > x.room.height)
                return;
            t.y -= x.floor_altitude(t) - 1;
            auto const floor = [&](int32_t tx, int32_t ty) { return x.room.inside(tx, ty) && pathing::tile_access(x.room, ivec2{tx, ty}) == pathing::access::floor; };
            if (!floor(t.x, t.y))
                return;
            int32_t num = t.x, num2 = t.x;
            for (int32_t k = 0; k < 20 && floor(num - 1, t.y); ++k)
                --num;
            for (int32_t k = 0; k < 20 && floor(num2 + 1, t.y); ++k)
                ++num2;
            if (num2 - num <= 7)
                return;
            // the WormGrass patches' tiles near the spot
            for (int32_t i = num; i <= num2; ++i)
                for (int32_t dy = -4; dy <= 4; ++dy)
                    if (x.room.get_tile(i, t.y + dy).flags & world::tile_worm_grass)
                        return;
            x.ai.rest_pos = ivec2{(num + num2) / 2, t.y + 2};
        }

        // Deer.EatObject
        void eat_object(ctx& x, dcon::item_id it) {
            if (x.ai.eat_object)
                return;
            x.ai.eat_object = it;
            x.p.eat_counter = 160;
        }

        // DeerAI.Update
        void ai_update(ctx& x) {
            auto& a = x.ai;
            auto& w = x.w();
            stuck_update(x);
            ivec2 const here = x.here();
            if (here.x > 10 && here.x < x.room.width - 11)
                ++a.time_in_this_room;
            // UtilityComparer: the RainTracker (1) and the SporeTracker (0.9)
            float const rain = dens::rain_utility(x.state, x.c, x.room.id);
            float const spores = (a.puff || a.spore_room) ? 0.9f : 0.f;
            behavior b = behavior::idle;
            float utility = 0.f;
            if (rain > 0.f) {
                utility = rain;
                b = behavior::escape_rain;
            }
            if (spores > utility) {
                utility = spores;
                b = behavior::track_spores;
            }
            if (a.damage_go_home) {
                utility = mathf::max(utility, 0.8f);
                b = behavior::escape_rain;
            }
            if (utility < 0.1f)
                b = behavior::idle;
            if (x.unstuck_force() > 0.9f && x.p.resting < 0.5f) {
                if (++a.seriously_stuck > 100)
                    b = behavior::get_unstuck;
            } else {
                a.seriously_stuck = 0;
            }
            w.creature_set_behavior(x.c, uint8_t(b));
            w.creature_set_current_utility(x.c, utility);
            find_goto_puff_ball(x);
            uint8_t const riders = x.rider_count();
            if (riders > 0) {
                a.lay_down_and_rest_counter = 0;
                a.rest_pos.reset();
                if (!a.last_player_in_antlers && x.room.width > 180 && std::abs(a.in_room_destination.x - x.room.width / 2) < x.room.width / 3)
                    a.in_room_destination = here;
            }
            a.last_player_in_antlers = riders > 0 ? 1 : 0;
            --a.kneel_counter;
            --a.close_eyes_counter;
            --a.lay_down_and_rest_counter;
            switch (b) {
            case behavior::idle: {
                bool flag = false;
                if (a.lay_down_and_rest_counter < 0 && riders == 0) {
                    if (!a.rest_pos) {
                        check_rest_spot(x, ivec2{(here.x + x.destination().x) / 2, (here.y + x.destination().y) / 2});
                    } else {
                        x.set_destination(*a.rest_pos);
                        if (x.p.resting > 0.5f && --a.resting_counter < 1) {
                            a.resting_counter = x.rng.range(90, 1200);
                            a.lay_down_and_rest_counter = x.rng.range(190, 1100);
                            a.rest_pos.reset();
                        }
                        flag = true;
                    }
                }
                if (!flag) {
                    if (!legal_in_room_dest(x, a.in_room_destination)) {
                        ++a.minor_wander_timer;
                        // (no Remix: a try every tick)
                        ivec2 const rndm{x.rng.range(0, x.room.width), x.rng.range(std::max(0, x.room.default_water_level), x.room.height)};
                        if (good_in_room_dest(x, rndm)) {
                            a.in_room_destination = rndm;
                            a.minor_wander_timer = x.rng.range(-20, 60);
                        }
                    }
                    x.set_destination(a.in_room_destination);
                }
                break;
            }
            case behavior::escape_rain:
                // DenFinder (dens.hpp): its den, through the room exits
                if (auto const e = dens::home_tile(x.state, x.c, x.room))
                    x.set_destination(*e);
                break;
            case behavior::track_spores: {
                bool flag = false;
                if (a.spore_room) {
                    if (a.spore_room != x.room.id) {
                        // summoned from another room: the trip there (migrate.hpp)
                        if (auto const e = migrate::travel_tile(x.state, x.c, x.room)) {
                            x.set_destination(*e);
                        } else {
                            a.spore_room = dcon::room_id{};
                            migrate::cancel(x.state, x.c);
                        }
                    } else {
                        x.set_destination(a.spore_tile);
                        a.in_room_destination = a.spore_tile;
                        if (custom::dist_less(rw::to_vec2(a.spore_tile), rw::to_vec2(here), 17.f) && x.room.visual_contact(x.pos(0), room_view::middle_of_tile(a.spore_tile)))
                            a.spore_room = dcon::room_id{};
                    }
                    flag = true;
                }
                if (!flag && a.puff) {
                    ivec2 g = puff_best_guess(x);
                    if (custom::manhattan_distance(here, g) >= 6)
                        for (int32_t num = 0; num < 2 && !x.reachable(g); ++num)
                            ++g.y;
                    x.set_destination(g);
                    if (a.puff_visible && puff_ball_legal(x, a.puff)) {
                        vec2 const ip = w.body_chunk_get_pos(items::chunk(x.state, a.puff));
                        if (custom::dist_less(x.pos(0), ip, 100.f) && room_view::get_tile_position(ip) != a.cant_eat_from)
                            eat_object(x, a.puff);
                    }
                }
                break;
            }
            case behavior::kneeling:
                break;
            case behavior::get_unstuck:
                // (GetUnstuckPosCalculator: a random spot near by)
                if (x.rng.value() < 0.02f) {
                    ivec2 const t = room_view::get_tile_position(x.pos(0) + custom::rnv(x.rng) * 200.f);
                    if (x.accessible(t))
                        x.set_destination(t);
                }
                break;
            }
            if (x.unstuck_force() > 0.5f && x.close_to_edge() < 0.5f) {
                ivec2 const t = x.destination() + custom::four_directions[x.rng.range(0, 4)];
                if (x.accessible(t))
                    x.set_destination(t);
            }
        }

        // DeerPather.FollowPath from `from`: the tile to step to
        std::optional<ivec2> follow_path(ctx& x, ivec2 from) {
            if (!x.accessible(from))
                return std::nullopt;
            return pathing::next_step(x.state, x.path(), from);
        }

        // Deer.Act
        void act(ctx& x, float support_v, float forward_power) {
            auto& p = x.p;
            auto& a = x.ai;
            auto& w = x.w();
            ai_update(x);
            a.satisfied = (p.violence_reaction < 1 && p.resting > 0.5f) ? 1 : 0;
            if (p.violence_reaction > 0)
                a.stuck_counter = max_stuck_counter;
            // eating
            if (p.eat_counter > 0) {
                --p.eat_counter;
                auto const it = a.eat_object;
                if (p.eat_counter < 1 || !it || !w.item_is_valid(it) || w.physical_object_get_room_from_object_location(items::body(x.state, it)) != x.room.id
                || w.item_get_grabbed_by(it) || !custom::dist_less(x.pos(0), w.body_chunk_get_pos(items::chunk(x.state, it)), 100.f))
                    a.eat_object = dcon::item_id{};
                if (a.eat_object) {
                    auto const ic = items::chunk(x.state, it);
                    vec2 const ip = w.body_chunk_get_pos(ic);
                    float const k = custom::lerp_map(float(p.eat_counter), 80.f, 10.f, 0.1f, 1.2f);
                    x.weighted_push(0, 1, custom::dir_vec(x.pos(0), ip), k);
                    x.weighted_push(0, 2, custom::dir_vec(x.pos(0), ip), k);
                    x.vel(0) += custom::dir_vec(x.pos(0), ip) * custom::lerp_map(float(p.eat_counter), 80.f, 10.f, 0.1f, 2.75f);
                    a.cant_eat_from = room_view::get_tile_position(ip);
                    if (custom::dist_less(x.pos(0), ip, x.rad(0) + w.body_chunk_get_rad(ic) + 20.f)) {
                        a.close_eyes_counter = std::max(5, a.close_eyes_counter);
                        vec2& iv = w.body_chunk_get_vel(ic);
                        iv = rw::lerp(iv, rw::clamp_magnitude(x.pos(0) + vec2{0.f, -14.f} - ip, 30.f) / 10.f, 0.8f);
                        x.vel(0) += custom::rnv(x.rng) * 2.6f;
                        // (Puffball_Eaten_By_Deer has no sample here)
                        if (p.eat_counter == 50)
                            puff_ball::start_being_eaten(x.state, it);
                    }
                } else {
                    p.eat_counter = 0;
                }
            }
            // AllowMovementBetweenRooms; near a side of the room otherwise,
            // pushed in (enterRoomForcePush)
            bool const allow_between = a.time_in_this_room > 100 && x.rider_count() == 0;
            ivec2 const here = x.here();
            if (allow_between) {
                p.enter_room_force_push = mathf::max(0.f, p.enter_room_force_push - 0.001f);
            } else if (std::abs(here.x) < 10) {
                if (x.room.visual_contact(x.pos(0), x.pos(0) + vec2{400.f, 0.f})) {
                    x.vel(1).x += mathf::pow(p.enter_room_force_push, 2.f) * 2.f;
                    p.enter_room_force_push = mathf::min(4.5f, p.enter_room_force_push + 0.0055555557f);
                }
                for (uint32_t i = 0; i < 6; ++i)
                    if (x.contact(i).x > 0)
                        x.vel(i) += custom::deg_to_vec(-90.f * x.rng.value()) * 15.f;
            } else if (std::abs(here.x - (x.room.width - 1)) < 10) {
                if (x.room.visual_contact(x.pos(0), x.pos(0) + vec2{-400.f, 0.f})) {
                    x.vel(1).x -= mathf::pow(p.enter_room_force_push, 2.f) * 2.f;
                    p.enter_room_force_push = mathf::min(4.5f, p.enter_room_force_push + 0.0055555557f);
                }
                for (uint32_t i = 0; i < 6; ++i)
                    if (x.contact(i).x < 0)
                        x.vel(i) += custom::deg_to_vec(90.f * x.rng.value()) * 15.f;
            }
            ivec2 const dest = x.destination();
            p.stay_still = (x.room.inside(here.x, here.y) && custom::manhattan_distance(here, dest) < 4) ? 1 : 0;
            bool flag2 = false;
            if (p.stay_still) {
                flag2 = true;
                for (int32_t num = -1; num < 2 && flag2; ++num)
                    if (x.floor_altitude(dest + ivec2{num * 2, 0}) > 5)
                        flag2 = false;
            }
            if (flag2)
                p.resting = mathf::min(1.f, p.resting + 0.016666668f);
            else
                p.resting = mathf::max(0.f, p.resting - 0.00625f);
            if (p.resting >= 0.1f)
                p.flip_dir = mathf::sign(x.pos(0).x - x.pos(4).x);
            p.preferred_height = 5.f + 10.f * mathf::pow(1.f - p.resting, 5.f);
            ivec2 const tp = room_view::get_tile_position(x.pos(1));
            p.worm_grass_below = 0;
            for (int32_t k = tp.y; k >= 0; --k) {
                auto const tile = x.room.get_tile(tp.x, k);
                if (tile.flags & world::tile_worm_grass) {
                    p.preferred_height = 15.f;
                    p.worm_grass_below = 1;
                    break;
                }
                if (tile.solid())
                    break;
            }
            if (p.stay_still) {
                p.move_direction = rw::clamp_magnitude(room_view::middle_of_tile(dest) - x.pos(0), 20.f) / mathf::lerp(30.f, 5.f, p.resting);
                p.next_floor_height = mathf::lerp(p.next_floor_height, float(custom::int_clamp(x.floor_altitude(here) + 2, 0, 17)) * 20.f, 0.2f);
            } else {
                ivec2 start = here;
                std::optional<ivec2> step = follow_path(x, start);
                for (int32_t num3 = 1; num3 < 3 && !step; ++num3)
                    for (uint32_t num4 = 0; num4 < 5 && !step; ++num4)
                        for (uint32_t num5 = 0; num5 < 5 && !step; ++num5) {
                            start = room_view::get_tile_position(x.pos(num4) + rw::to_vec2(four_directions_and_zero[num5]) * 20.f * float(num3));
                            step = follow_path(x, start);
                        }
                if (step) {
                    // as far along the path as it sees in a straight line
                    ivec2 dc = *step;
                    if (x.room.inside(here.x, here.y))
                        for (int32_t l = 0; l < 15; ++l) {
                            auto const n2 = follow_path(x, dc);
                            if (!n2 || *n2 == start || *n2 == dc || !ray_trace_tiles_for_terrain(x.room, start, *n2))
                                break;
                            dc = *n2;
                        }
                    if (p.resting < 0.1f && std::fabs(room_view::middle_of_tile(dc).x - x.pos(0).x) > 100.f)
                        p.flip_dir = mathf::sign(room_view::middle_of_tile(dc).x - x.pos(0).x);
                    if (here.x < 0 || here.x >= x.room.width || dc.x < 0 || dc.x >= x.room.width)
                        p.next_floor_height = 300.f;
                    else
                        p.next_floor_height = mathf::lerp(p.next_floor_height, float(custom::int_clamp(x.floor_altitude(dc) + 2, 0, 17)) * 20.f, 0.2f);
                    p.move_direction = custom::dir_vec(x.pos(0), room_view::middle_of_tile(dc));
                }
            }
            float const unstuck = x.unstuck_force();
            float const edge = x.close_to_edge();
            for (uint32_t m = 0; m < 5; ++m) {
                float const num7 = float(m) / 5.f;
                x.vel(m) *= mathf::lerp(1.f, p.stay_still ? 0.7f : 0.92f, support_v);
                x.vel(m) *= 1.f - p.resting * 0.5f;
                if (m < 4) {
                    float const fa = float(x.floor_altitude(room_view::get_tile_position(x.pos(m))));
                    x.vel(m).y += deer_gravity * mathf::lerp(1.3f, 2.5f, std::sin(mathf::pow(num7, 1.7f) * mathf::pi))
                        * mathf::lerp(support_v * custom::lerp_map(fa, 14.f, 18.f, 1.f, 0.5f) * custom::lerp_map(p.move_direction.y, -1.f, 1.f, 0.5f, 1.f), 0.65f, edge);
                }
                x.vel(m) += p.move_direction * mathf::lerp(0.35f, 0.f, num7) * forward_power * mathf::lerp(1.f, 3.f, unstuck);
            }
            x.vel(0).y += deer_gravity * 1.2f * p.resting;
            if (unstuck > 0.f)
                x.vel(1) += custom::rnv(x.rng) * x.rng.value() * 4.f * unstuck;
        }

        //
        // Deer.PlayerInAntlers
        //
        struct antlers_view {
            ctx& x;
            antler_index const& idx;
            float face;
            uint32_t count(int32_t part) const { return part >= 0 && part < int32_t(idx.count) ? idx.length[part] : 0; }
            // PlaySpaceCoordinate
            vec2 ps(int32_t part, int32_t segment, float side) const {
                if (part < 0 || part >= int32_t(idx.count))
                    return x.pos(5);
                auto const points = x.state.world.deer_data_get_antlers(x.d);
                uint32_t const k = uint32_t(idx.start[part]) + uint32_t(segment);
                if (k >= points.size())
                    return x.pos(5);
                return outposts::transform_to_head_rotat(outposts::transformed_pos(points.at(k), side), x.pos(5), custom::aim(x.pos(0), x.pos(5)), side, face);
            }
            vec2 ps(antler_point_ref const& r) const { return ps(r.part, r.segment, r.side); }
        };

        vec2 push_out_of_head(ctx& x, vec2 ps, float margin) {
            if (custom::dist_less(ps, x.pos(0), x.rad(0) + margin))
                return x.pos(0) + custom::dir_vec(x.pos(0), ps) * (x.rad(0) + margin);
            return ps;
        }

        // GrabStance.LegalLowerPos
        bool legal_lower_pos(ctx& x, antlers_view const& av, rider const& r, vec2 test) {
            vec2 const pp = av.ps(r.stance.upper);
            return custom::dist_less(pp, test, 30.f) && !custom::dist_less(pp, test, 5.f) && !custom::dist_less(test, x.pos(0), x.rad(0) + 9.f);
        }

        // GrabStance.UpdateLower
        void update_lower(ctx& x, antlers_view const& av, rider& r) {
            vec2 const a = av.ps(r.stance.upper);
            if (!r.stance.lower.valid()) {
                if (r.dangle)
                    return;
                vec2 const v = a + vec2{0.f, -15.f};
                float dst = float_max;
                antler_point_ref best;
                for (int32_t i = 0; i < int32_t(av.idx.count); ++i)
                    for (uint32_t j = 0; j < av.count(i); ++j) {
                        vec2 const v2 = av.ps(i, int32_t(j), r.stance.upper.side);
                        if (legal_lower_pos(x, av, r, v2) && custom::dist_less(v, v2, dst)) {
                            dst = rw::distance(v, v2);
                            best = antler_point_ref{int16_t(i), int16_t(j), r.stance.upper.side};
                        }
                    }
                if (best.valid())
                    r.stance.lower = best;
            } else if (!legal_lower_pos(x, av, r, av.ps(r.stance.lower))) {
                r.stance.lower = antler_point_ref{};
            }
        }

        // PlayerInAntlers.FindNextStance
        void find_next_stance(antlers_view const& av, rider& r, input_ref in0, input_ref in1) {
            vec2 const v = av.ps(r.stance.upper);
            vec2 const v2 = v + r.climb_goal;
            antler_point_ref best;
            float dst = rw::distance(v, v2) + 1.f;
            for (int32_t i = 0; i < int32_t(av.idx.count); ++i)
                for (uint32_t j = 0; j < av.count(i); ++j) {
                    vec2 const v3 = av.ps(i, int32_t(j), r.stance.upper.side);
                    if (custom::dist_less(v, v3, 30.f) && custom::dist_less(v2, v3, dst)) {
                        dst = rw::distance(v2, v3);
                        best = antler_point_ref{int16_t(i), int16_t(j), r.stance.upper.side};
                    }
                }
            if (best.valid() && (best.part != r.stance.upper.part || best.segment != r.stance.upper.segment)) {
                r.climb_goal = vec2{0.f, 0.f};
                r.next_stance.upper = best;
                r.next_stance.lower = r.stance.lower;
                r.next_stance.has = 1;
                r.dangle = 0;
                return;
            }
            if (in0.y > 0 && in1.y <= 0 && in0.x == 0)
                r.dangle = 0;
        }

        // PlayerInAntlers.FindHandPositions
        void find_hand_positions(antlers_view const& av, rider& r, vec2 p0, vec2 p1) {
            vec2 const v = av.ps(r.stance.upper);
            r.hand_grab_points[1 - r.odd_hand] = v;
            if (r.dangle) {
                r.hand_grab_points[r.odd_hand] = v;
                return;
            }
            if (r.next_stance.has) {
                r.hand_grab_points[r.odd_hand] = av.ps(r.next_stance.upper);
                return;
            }
            if (r.odd_hand_grip.valid() && (r.odd_hand_grip.side != r.stance.upper.side || !custom::dist_less(v, av.ps(r.odd_hand_grip), 25.f)))
                r.odd_hand_grip = antler_point_ref{};
            if (!r.odd_hand_grip.valid()) {
                float num = float_max;
                antler_point_ref best;
                vec2 const perp = custom::perpendicular_vector(rw::normalized(p0 - p1));
                for (int32_t i = 0; i < int32_t(av.idx.count); ++i)
                    for (uint32_t j = 0; j < av.count(i); ++j) {
                        vec2 const v2 = av.ps(i, int32_t(j), r.stance.upper.side);
                        float const num2 = mathf::min(rw::distance(v2, v + perp * 35.f), rw::distance(v2, v + perp * -35.f));
                        if (custom::dist_less(v, v2, 25.f) && num2 < num) {
                            num = num2;
                            best = antler_point_ref{int16_t(i), int16_t(j), r.stance.upper.side};
                            r.odd_hand = custom::distance_to_line(v2, v, p1) < 0.f ? 0 : 1;
                        }
                    }
                if (best.valid())
                    r.odd_hand_grip = best;
            }
            r.hand_grab_points[r.odd_hand] = r.odd_hand_grip.valid() ? av.ps(r.odd_hand_grip) : v;
        }

        // the slugcat lets go of the antlers (playerDisconnected, its
        // animation back to None)
        void disconnect(sys::game_state& state, rider& r) {
            r.disconnected = 1;
            if (!state.world.creature_is_valid(r.player))
                return;
            if (auto const pid = creature::player_of(state, r.player))
                if (state.world.player_get_animation(pid) == player::animation_index::antler_climb)
                    state.world.player_set_animation(pid, player::animation_index::none);
        }

        // the PlayerInAntlers constructor
        rider make_rider(ctx& x, antlers_view const& av, dcon::creature_id pc) {
            rider r;
            r.player = pc;
            auto const pid = creature::player_of(x.state, pc);
            vec2 const p0 = x.w().body_chunk_get_pos(player::body_chunk(x.state, pid, 0));
            r.hand_grab_points[0] = r.hand_grab_points[1] = p0;
            float const num = mathf::sign(av.face);
            float best = float_max;
            antler_point_ref pick{0, 0, num};
            for (int32_t j = 0; j < int32_t(av.idx.count); ++j)
                for (uint32_t k = 0; k < av.count(j); ++k)
                    for (float side = -1.f; side <= 1.f; side += 2.f) {
                        float const d = rw::distance(av.ps(j, int32_t(k), side), p0) + (side != num ? 100.f : 0.f) * std::fabs(av.face);
                        if (d < best) {
                            best = d;
                            pick = antler_point_ref{int16_t(j), int16_t(k), side};
                        }
                    }
            r.dangle = 1;
            r.stance.upper = pick;
            r.stance.lower = antler_point_ref{};
            r.stance.has = 1;
            return r;
        }

        // PlayerInAntlers.Update (without Remix's wiggling and the vultures)
        void rider_update(ctx& x, antlers_view const& av, rider& r) {
            auto& w = x.w();
            auto const pid = w.creature_is_valid(r.player) ? creature::player_of(x.state, r.player) : dcon::player_id{};
            if (!pid || !creature::here(x.state, r.player, x.room.id)) {
                r.disconnected = 1;
                return;
            }
            auto const c0 = player::body_chunk(x.state, pid, 0);
            auto const c1 = player::body_chunk(x.state, pid, 1);
            x.weighted_push(5, 0, custom::dir_vec(x.pos(0), w.body_chunk_get_pos(c0)), 0.7f);
            if (!custom::dist_less(w.body_chunk_get_pos(c0), x.pos(5), x.rad(5) + 100.f)) {
                disconnect(x.state, r);
                return;
            }
            auto const& in = w.player_get_input(pid);
            if (std::fabs(av.face) > 0.5f && r.stance.upper.side != -mathf::sign(av.face)) {
                r.force_side_change = 1;
                // FindCorrectSideStance
                r.next_stance.upper = antler_point_ref{r.stance.upper.part, r.stance.upper.segment, -mathf::sign(av.face)};
                r.next_stance.lower = antler_point_ref{};
                r.next_stance.has = 1;
            } else {
                r.force_side_change = 0;
            }
            if (r.force_side_change) {
                r.mov_prog = mathf::min(1.f, r.mov_prog + 0.1f);
                if (r.mov_prog == 1.f) {
                    r.stance = r.next_stance;
                    r.next_stance.has = 0;
                    r.mov_prog = 0.f;
                    r.force_side_change = 0;
                }
            } else {
                vec2 n{0.f, 0.f};
                if (rw::magnitude(in[0].analogue_dir) > 0.f)
                    n = rw::normalized(in[0].analogue_dir);
                else if (in[0].x != 0 || in[0].y != 0)
                    n = rw::normalized(vec2{float(in[0].x), float(in[0].y)});
                if (rw::magnitude(n) > 0.f) {
                    if (r.next_stance.has) {
                        r.mov_prog = mathf::min(1.f, r.mov_prog + 0.1f);
                        if (r.mov_prog == 1.f) {
                            r.stance = r.next_stance;
                            r.next_stance.has = 0;
                            r.mov_prog = 0.f;
                        }
                    }
                    r.climb_goal += n * custom::lerp_map(rw::dot(n, rw::normalized(r.climb_goal)), -1.f, 1.f, 1.6f, 0.8f);
                    if (rw::magnitude(r.climb_goal) > 30.f)
                        r.climb_goal = rw::clamp_magnitude(r.climb_goal, 30.f);
                } else {
                    if (r.next_stance.has)
                        r.mov_prog = mathf::max(0.f, r.mov_prog - 0.1f);
                    if (r.mov_prog == 0.f)
                        r.next_stance.has = 0;
                    if (rw::magnitude(r.climb_goal) < 1.f)
                        r.climb_goal = vec2{0.f, 0.f};
                    else
                        r.climb_goal -= rw::normalized(r.climb_goal);
                }
                if (r.mov_prog == 0.f) {
                    if (in[0].y < 0 && in[1].y >= 0 && in[0].x == 0) {
                        r.dangle = 1;
                        r.stance.lower = antler_point_ref{};
                    } else if (rw::magnitude(n) > 0.f) {
                        find_next_stance(av, r, in[0], in[1]);
                    }
                }
                update_lower(x, av, r);
            }
            vec2 v = r.next_stance.has ? rw::lerp(av.ps(r.stance.upper), av.ps(r.next_stance.upper), r.mov_prog) : av.ps(r.stance.upper);
            v = push_out_of_head(x, v, 10.f);
            find_hand_positions(av, r, w.body_chunk_get_pos(c0), w.body_chunk_get_pos(c1));
            vec2& pos0 = w.body_chunk_get_pos(c0);
            vec2& vel0 = w.body_chunk_get_vel(c0);
            if (!r.dangle) {
                // BodyChunk.MoveFromOutsideMyUpdate
                w.body_chunk_set_last_pos(c0, pos0);
                pos0 = v;
                vel0 = x.vel(5);
            } else {
                vec2 const a = custom::dir_vec(pos0, v);
                float const num = rw::distance(pos0, v);
                if (num > 10.f) {
                    pos0 += a * (num - 10.f);
                    vel0 += a * (num - 10.f);
                }
            }
            if (r.stance.lower.valid()) {
                float const num2 = 1.f - r.mov_prog;
                vec2& vel1 = w.body_chunk_get_vel(c1);
                vel1 = rw::lerp(vel1, x.vel(5), num2 * 0.9f);
                vel1 = -rw::clamp_magnitude(w.body_chunk_get_pos(c1) - push_out_of_head(x, av.ps(r.stance.lower), 10.f), 10.f * num2) / 3.f;
            }
            if (!creature::consious(x.state, r.player) || w.creature_get_entering_shortcut(r.player).x >= 0)
                disconnect(x.state, r);
        }

        // the riders' part of Deer.Update
        void riders_update(ctx& x, bool viewed) {
            auto& w = x.w();
            auto& rs = w.deer_data_get_riders(x.d);
            if (!viewed) {
                // (no graphics module: playersInAntlers.Clear())
                for (uint32_t i = 0; i < rs.count; ++i)
                    disconnect(x.state, rs.r[i]);
                rs.count = 0;
                return;
            }
            auto const& idx = w.deer_data_get_antler_index(x.d);
            if (idx.count == 0)
                return;
            antlers_view const av{x, idx, face_dir(x.state, x.c, 1.f)};
            rooms::for_each_creature(x.state, x.room.id, creature::type::slugcat, [&](dcon::creature_id pc) {
                auto const pid = creature::player_of(x.state, pc);
                if (!pid || !creature::here(x.state, pc, x.room.id) || w.player_get_want_to_grab(pid) <= 0)
                    return;
                if (!custom::dist_less(w.body_chunk_get_pos(player::body_chunk(x.state, pid, 0)), x.pos(5), x.rad(5)))
                    return;
                w.player_set_want_to_grab(pid, 0);
                for (uint32_t i = 0; i < rs.count; ++i)
                    if (rs.r[i].player == pc)
                        return;
                // in another deer's antlers: it lets go of those
                rooms::for_each_creature(x.state, x.room.id, creature::type::deer, [&](dcon::creature_id o) {
                    if (o == x.c)
                        return;
                    auto& ors = w.deer_data_get_riders(creature::deer_data(w, o));
                    for (uint32_t i = 0; i < ors.count; ++i)
                        if (ors.r[i].player == pc)
                            ors.r[i].disconnected = 1;
                });
                if (rs.count < max_riders)
                    rs.r[rs.count++] = make_rider(x, av, pc);
            });
            for (int32_t i = int32_t(rs.count) - 1; i >= 0; --i) {
                if (rs.r[i].disconnected) {
                    rs.r[i] = rs.r[--rs.count];
                    continue;
                }
                rider_update(x, av, rs.r[i]);
            }
        }

        // the object collisions with the other creatures of the room,
        // without the antlers (collideWithObjects off): Room.Update's
        // pushing apart and Deer.Collide
        void collide_others(ctx& x) {
            auto& w = x.w();
            vec2 const o = x.pos(2);
            rooms::for_each_creature(x.state, x.room.id, [&](dcon::creature_id t) {
                if (t == x.c || w.creature_get_type(t) == creature::type::deer || w.creature_get_type(t) == creature::type::leech || !creature::here(x.state, t, x.room.id)
                || w.creature_get_away(t) != 0)
                    return;
                uint32_t const m = creature::chunk_count(x.state, t);
                if (m == 0 || !custom::dist_less(o, w.body_chunk_get_pos(creature::chunk(x.state, t, 0)), 300.f))
                    return;
                for (uint32_t i = 0; i < 5; ++i)
                    for (uint32_t j = 0; j < m; ++j) {
                        auto const b = creature::chunk(x.state, t, j);
                        vec2& pa = x.pos(i);
                        vec2& pb = w.body_chunk_get_pos(b);
                        float const r = x.rad(i) + w.body_chunk_get_rad(b);
                        if (!custom::dist_less(pa, pb, r))
                            continue;
                        float const dist = rw::distance(pa, pb);
                        vec2 const dir = custom::dir_vec(pa, pb);
                        float const ratio = w.body_chunk_get_mass(b) / (x.mass(i) + w.body_chunk_get_mass(b));
                        pa -= dir * ((r - dist) * ratio);
                        x.vel(i) -= dir * ((r - dist) * ratio);
                        pb += dir * ((r - dist) * (1.f - ratio));
                        w.body_chunk_get_vel(b) += dir * ((r - dist) * (1.f - ratio));
                        // Deer.Collide
                        if (x.p.resting > 0.5f)
                            x.ai.resting_counter -= 10;
                    }
            });
        }

        // DeerAI.NewRoom (no Remix)
        void ai_new_room(ctx& x) {
            auto& a = x.ai;
            a.time_in_this_room = 0;
            ivec2 const dest{x.rng.range(0, x.room.width), x.rng.range(std::max(0, x.room.default_water_level), x.room.height)};
            x.set_destination(x.accessible(dest) ? dest : x.here());
            a.resting_counter = x.rng.range(90, 600);
            a.lay_down_and_rest_counter = x.rng.range(190, 1100);
            a.rest_pos.reset();
            a.in_room_destination = x.here();
            a.puff = dcon::item_id{};
            a.eat_object = dcon::item_id{};
            a.past_count = 0;
            a.stuck_counter = 0;
        }

        // Personality(ID).dominance
        float personality_dominance(int32_t seed) {
            rw::unity_random r{seed};
            auto const push = [](float v, float e) {
                if (v == 0.5f)
                    return 0.5f;
                return v < 0.5f ? custom::lerp_map(v, 0.f, 0.5f, 0.f, 0.5f, e) : custom::lerp_map(v, 1.f, 0.5f, 1.f, 0.5f, e);
            };
            float const sympathy = push(r.value(), 1.5f);
            float const energy = push(r.value(), 1.5f);
            float const bravery = push(r.value(), 1.5f);
            // nervous (unused here, its rolls kept)
            r.value();
            r.value();
            float const ar = r.value();
            float const ap = mathf::pow(r.value(), 0.25f);
            float const aggression = mathf::lerp(ar, (energy + bravery) / 2.f * (1.f - sympathy), ap);
            float const dr = r.value();
            float const dp = mathf::pow(r.value(), 0.25f);
            return mathf::lerp(dr, (energy + bravery + aggression) / 3.f, dp);
        }
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 pos) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::deer);
        auto const d = data(state, c);
        rw::rng rng{state.random.next() | 1u};
        int32_t seed = int32_t(rng.next() & 0x7FFFFFFFu);
        if (state.spawn_seed >= 0)
            seed = state.spawn_seed;
        float const dominance = personality_dominance(seed);
        w.deer_data_set_seed(d, seed);
        w.deer_data_set_dominance(d, dominance);
        w.deer_data_set_ai(d, ai_state{});
        w.deer_data_set_riders(d, riders{});

        physics::object_desc desc;
        desc.air_friction = 0.999f;
        desc.gravity = deer_gravity;
        desc.bounce = 0.1f;
        desc.surface_friction = 0.4f;
        desc.water_friction = 0.95f;
        desc.buoyancy = 0.93f;
        auto const o = physics::create_object(state, room, desc);
        dcon::body_chunk_id cs[6];
        cs[0] = physics::add_chunk(state, o, pos, 22.5f, 3.f);
        for (int32_t i = 1; i < 5; ++i) {
            float num = float(i) / 4.f;
            num = (1.f - num) * 0.5f + std::sin(mathf::pow(num, 0.5f) * mathf::pi) * 0.5f;
            num = mathf::pow(mathf::max(0.f, mathf::lerp(num, 1.f, 0.2f)), 0.7f);
            cs[i] = physics::add_chunk(state, o, pos, mathf::lerp(10.f, 35.f, num), mathf::lerp(1.f, 8.f, num));
            w.body_chunk_set_restrict_in_room_range(cs[i], 2000.f);
        }
        cs[5] = physics::add_chunk(state, o, pos, mathf::lerp(30.f, 70.f, dominance), 0.5f);
        physics::add_connection(state, o, cs[0], cs[1], 38.f, physics::connection_type::normal, 1.f, -1.f);
        for (int32_t j = 1; j < 4; ++j)
            physics::add_connection(state, o, cs[j], cs[j + 1], mathf::max(w.body_chunk_get_rad(cs[j]), w.body_chunk_get_rad(cs[j + 1])) * 0.8f,
                physics::connection_type::normal, 1.f, -1.f);
        physics::add_connection(state, o, cs[0], cs[5], w.body_chunk_get_rad(cs[0]) + w.body_chunk_get_rad(cs[5]) - 10.f, physics::connection_type::normal, 1.f, 0.f);
        w.try_create_creature_body(c, o);
        physics::set_go_through_floors(state, o, true);

        for (uint32_t k = 0; k < leg_count; ++k) {
            auto const part = creature::add_part(state, c, creature::part_kind::deer_leg, uint8_t(k));
            auto const l = w.create_deer_leg();
            w.creature_part_get_payload(part).deer_leg = creature::deer_leg_payload{uint16_t(l.index())};
            leg t;
            t.ideal_length = leg_length;
            t.connected_chunk = k < 2 ? 1 : 2;
            t.tentacle_dir = custom::deg_to_vec(45.f + 90.f * float(k));
            for (uint32_t i = 0; i < leg_chunks; ++i) {
                t.c[i] = leg_chunk{};
                t.c[i].t_pos = float(i + 1) / float(leg_chunks);
                t.c[i].rad = 4.f;
            }
            w.deer_leg_set_t(l, t);
            w.deer_leg_get_segments(l).resize(0);
            w.deer_leg_get_grab_path(l).resize(0);
        }

        auto& p = pl(state, c);
        p = creature::deer_payload{};
        p.data = uint16_t(d.index());
        p.flip_dir = rng.value() < 0.5f ? -1.f : 1.f;
        p.bod_dir = vec2{p.flip_dir, 0.f};
        p.move_direction = vec2{0.f, 0.f};
        p.preferred_height = 55.f;
        w.creature_set_random(c, rng);
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_set_grasps(c, creature::grasp_pair{});
        w.creature_set_behavior(c, uint8_t(behavior::idle));
        w.creature_set_destination(c, room_view::get_tile_position(pos));
        // DeerGraphics' constructor rolls (the antlers a rider climbs too)
        deer_graphics::create(state, c);
        place_in_room(state, c, room, pos);
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        auto& rs = state.world.deer_data_get_riders(data(state, c));
        for (uint32_t i = 0; i < rs.count; ++i)
            disconnect(state, rs.r[i]);
        rs.count = 0;
        creature::release_target(state, c);
        creature::erase(state, c);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        ctx x(state, c, room);
        auto& p = x.p;
        load(x);
        if (x.legs.size() < leg_count) {
            store(x);
            return;
        }
        if (p.violence_reaction > 0)
            --p.violence_reaction;
        bool const cons = x.consious();
        vec2 const flipv{p.flip_dir, 0.f};
        x.weighted_push(1, 3, rw::slerp(p.bod_dir, flipv, cons ? 0.3f : 0.f), 0.35f);
        x.weighted_push(1, 4, rw::slerp(p.bod_dir, flipv, cons ? 0.3f : 0.f), 0.35f);
        // (Kneeling is never on without the dominance games)
        if (p.eat_counter < 1) {
            x.weighted_push(0, 1, x.head_dir(), 0.85f);
            x.weighted_push(0, 1, p.bod_dir, 0.6f);
            for (uint32_t i = 0; i < 4; ++i)
                x.weighted_push(5, i, cons ? (p.bod_dir * 0.4f + x.head_dir()) : custom::dir_vec(x.pos(i), x.pos(5)), 1.1f - float(i) * 0.3f);
            x.weighted_push(4, 5, flipv, 0.6f);
        } else {
            x.weighted_push(0, 1, p.bod_dir, 0.6f);
            for (uint32_t j = 0; j < 4; ++j)
                x.weighted_push(5, j, p.bod_dir * 0.4f, (1.6f - float(j) * 0.3f) * 0.3f);
        }
        x.vel(5) *= 0.92f;
        // Creature.Update + PhysicalObject.Update (Leviathan_*_Terrain_Impact
        // have no samples here)
        creature::stun_countdown(state, c);
        physics::update_object(state, room, creature::body(state, c), nullptr, nullptr);
        p.held_back_by_leg = 0;
        p.legs_grabbing = 0;
        float num = 0.f;
        float best = -float_max;
        leg_work* release = nullptr;
        for (auto& lw : x.legs) {
            leg_update(x, lw);
            if (lw.t.attached_at_tip)
                ++p.legs_grabbing;
            float const rs = release_score(x, lw);
            if (rs > best) {
                best = rs;
                release = &lw;
            }
            num += support(x, lw);
        }
        if (!p.stay_still && p.legs_grabbing > 3 && release)
            release_grip(x, *release);
        num = mathf::pow(mathf::min(num / 3.f, 1.f), 0.8f) * mathf::pow(1.f - p.resting, 0.3f);
        for (uint32_t l = 0; l < 5; ++l)
            if (x.contact(l).x != 0 || x.contact(l).y < 0)
                num = mathf::lerp(num, 1.f, 0.6f);
        float const edge = x.close_to_edge();
        num = mathf::lerp(num, 1.f, edge);
        num = mathf::pow(num, 0.1f);
        float num3 = mathf::lerp(mathf::pow(float(p.legs_grabbing) / 4.f, 0.8f), num, 0.5f);
        bool flag = false;
        for (auto& lw : x.legs)
            if (lw.t.attached_at_tip && (tip(lw).pos.x > x.pos(2).x) == (p.move_direction.x > 0.f)) {
                flag = true;
                break;
            }
        if (!flag) {
            if (!p.stay_still)
                num3 = mathf::lerp(custom::lerp_map(float(p.hesist_counter), 20.f, 150.f, -0.5f, 1.f), num3, edge);
            ++p.hesist_counter;
        } else {
            p.hesist_counter = 0;
        }
        num3 = mathf::lerp(num3, 1.f, x.unstuck_force());
        p.bod_dir = vec2{0.f, 0.f};
        if (cons)
            act(x, num, num3);
        for (uint32_t m = 0; m < 3; ++m) {
            x.weighted_push(m, m + 2, -custom::dir_vec(x.pos(m), x.pos(m + 2)), 0.45f);
            p.bod_dir += custom::dir_vec(x.pos(m + 2), x.pos(m + 1));
        }
        if (cons)
            p.bod_dir += vec2{0.8f * p.flip_dir, 0.f};
        p.bod_dir = rw::normalized(p.bod_dir);
        collide_others(x);
        riders_update(x, state.viewed(room.id));
        store(x);
    }

    bool collides(sys::game_state const&, dcon::creature_id, dcon::creature_id) {
        // (its own collision pass, collide_others, leaves the antlers out)
        return false;
    }

    void collide(sys::game_state& state, dcon::creature_id c, room_view const&, uint32_t, dcon::creature_id, uint32_t) {
        // Deer.Collide (the deer piles aren't ported)
        if (pl(state, c).resting > 0.5f)
            state.world.deer_data_get_ai(data(state, c)).resting_counter -= 10;
    }

    void violence(sys::game_state& state, dcon::creature_id c, uint32_t k, vec2 momentum, float damage, float stun_bonus, creature::damage_type) {
        auto& w = state.world;
        auto& rng = w.creature_get_random(c);
        auto& p = pl(state, c);
        // Deer.Violence: hurt, it goes home
        if (damage > 0.9f) {
            p.violence_reaction = std::max(p.violence_reaction, rng.range(10, int32_t(custom::lerp_map(damage, 0.9f, 3.f, 20.f, 60.f))));
            auto& a = w.deer_data_get_ai(data(state, c));
            a.damage_go_home = 1;
            a.time_in_this_room += 100;
        }
        // Creature.Violence (HealthState)
        if (k >= creature::chunk_count(state, c))
            k = 0;
        auto const hit = creature::chunk(state, c, k);
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
        auto& rs = state.world.deer_data_get_riders(data(state, c));
        for (uint32_t i = 0; i < rs.count; ++i)
            if (rs.r[i].player == t)
                rs.r[i].disconnected = 1;
    }

    void died(sys::game_state&, dcon::creature_id) {
        // (a rider may hang on to a dead deer's antlers, like vanilla; the
        // legs go limp in their update)
    }

    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const view = world::get_room_view(state, room);
        vec2 const mid = room_view::middle_of_tile(at);
        uint32_t const n = creature::chunk_count(state, c);
        for (uint32_t i = 0; i < n; ++i) {
            auto const ch = creature::chunk(state, c, i);
            physics::hard_set_position(state, ch, mid + custom::rnv(w.creature_get_random(c)));
            w.body_chunk_set_vel(ch, vec2{0.f, 0.f});
        }
        auto& p = pl(state, c);
        p.preferred_height = 55.f;
        p.enter_room_force_push = 0.f;
        p.eat_counter = 0;
        auto& rs = w.deer_data_get_riders(data(state, c));
        for (uint32_t i = 0; i < rs.count; ++i)
            disconnect(state, rs.r[i]);
        rs.count = 0;
        ctx x(state, c, view);
        load(x);
        for (auto& lw : x.legs)
            leg_new_room(x, lw);
        // out of a pipe: home is no longer what it wants
        x.ai.damage_go_home = 0;
        if (x.ai.spore_room && x.ai.spore_room != room && !migrate::want_to(state, c))
            x.ai.spore_room = dcon::room_id{};
        ai_new_room(x);
        store(x);
        deer_graphics::reset_positions(state, c);
    }

    std::string describe(sys::game_state const& state, dcon::creature_id c) {
        static char const* const names[] = {"idle", "escape rain", "track spores", "kneeling", "get unstuck"};
        auto const& w = state.world;
        auto const b = uint32_t(w.creature_get_behavior(c));
        std::string s = "rain deer, ";
        s += b < 5 ? names[b] : "?";
        if (pl(state, c).resting > 0.5f)
            s += ", resting";
        if (w.deer_data_get_riders(data(state, c)).count > 0)
            s += ", ridden";
        if (w.deer_data_get_ai(data(state, c)).puff)
            s += ", after a puff ball";
        return s;
    }

    bool spawn_spot(room_view const& room, ivec2 tile) {
        // on a floor, open space above for the body, away from the sides
        if (room.get_tile(tile).solid() || !room.get_tile(tile + ivec2{0, -1}).solid())
            return false;
        for (int32_t dy = 0; dy < 12; ++dy)
            for (int32_t dx = -3; dx <= 3; ++dx)
                if (room.get_tile(tile + ivec2{dx, dy}).solid())
                    return false;
        return tile.x > 10 && tile.x < room.width - 11;
    }

    dcon::creature_id ridden(sys::game_state const& state, dcon::player_id p) {
        auto const pc = player::creature_of(state, p);
        auto const room = creature::room_of(state, pc);
        if (!room)
            return dcon::creature_id{};
        dcon::creature_id out;
        rooms::for_each_creature(state, room, creature::type::deer, [&](dcon::creature_id d) {
            auto const& rs = state.world.deer_data_get_riders(data(state, d));
            for (uint32_t i = 0; i < rs.count; ++i)
                if (rs.r[i].player == pc && !rs.r[i].disconnected)
                    out = d;
        });
        return out;
    }

    void jump_off(sys::game_state& state, dcon::player_id p, float num) {
        auto& w = state.world;
        auto const d = ridden(state, p);
        if (!d)
            return;
        auto& rs = w.deer_data_get_riders(data(state, d));
        auto const pc = player::creature_of(state, p);
        rider* r = nullptr;
        for (uint32_t i = 0; i < rs.count; ++i)
            if (rs.r[i].player == pc)
                r = &rs.r[i];
        if (!r)
            return;
        auto const room = creature::room_of(state, pc);
        auto const c0 = player::body_chunk(state, p, 0);
        auto const c1 = player::body_chunk(state, p, 1);
        vec2& v0 = w.body_chunk_get_vel(c0);
        vec2& v1 = w.body_chunk_get_vel(c1);
        auto const antler = creature::chunk(state, d, 5);
        vec2 const av = w.body_chunk_get_vel(antler);
        auto const& in = w.player_get_input(p)[0];
        w.player_set_animation(p, player::animation_index::none);
        w.player_set_jump_boost(p, 0.f);
        v0 = av;
        if (!r->dangle)
            v1 = av;
        if (r->dangle) {
            if (in.x == 0) {
                v0.y += 3.f;
                v1.y -= 3.f;
                w.player_set_standing(p, 1);
                // (Slugcat_Climb_Along_Horizontal_Beam has no sample here)
            } else {
                v1.y += 4.f;
                v1.x += 2.f * float(in.x);
                v0.y += 6.f;
                v0.x += 3.f * float(in.x);
                state.play_sound(sound::sound_id::slugcat_from_vertical_pole_jump, room, w.body_chunk_get_pos(c0), 0.15f);
            }
        } else if (in.x == 0) {
            if (in.y > 0) {
                v0.y += 4.f * num;
                v1.y += 3.f * num;
                w.player_set_jump_boost(p, w.player_get_stats(p).name == player::slugcat_name::slugpup ? 7.f : 8.f);
                state.play_sound(sound::sound_id::slugcat_from_horizontal_pole_jump, room, w.body_chunk_get_pos(c0));
                w.player_set_standing(p, 1);
            } else {
                v0.y = 3.f;
                v1.y = -3.f;
                w.player_set_standing(p, 1);
            }
        } else {
            v0.y += 8.f * num;
            v1.y += 7.f * num;
            v0.x += 6.f * float(in.x) * num;
            v1.x += 5.f * float(in.x) * num;
            state.play_sound(sound::sound_id::slugcat_from_vertical_pole_jump, room, w.body_chunk_get_pos(c0));
        }
        vec2 vec = v0 - av + (v1 - av) * (r->dangle ? 0.f : 1.f);
        vec -= custom::dir_vec(w.body_chunk_get_pos(c0), w.body_chunk_get_pos(creature::chunk(state, d, 0))) * rw::magnitude(vec);
        vec.x *= 0.1f;
        vec = rw::clamp_magnitude(vec, 10.f);
        w.body_chunk_get_vel(antler) -= vec * 1.2f;
        w.body_chunk_get_vel(creature::chunk(state, d, 0)) -= vec * 0.25f;
        r->disconnected = 1;
    }

    bool rider_hand(sys::game_state const& state, dcon::player_id p, uint32_t limb, vec2& pos, vec2& antler_vel, bool& dangle) {
        auto const d = ridden(state, p);
        if (!d || limb > 1)
            return false;
        auto const pc = player::creature_of(state, p);
        auto const& rs = state.world.deer_data_get_riders(data(state, d));
        for (uint32_t i = 0; i < rs.count; ++i)
            if (rs.r[i].player == pc) {
                pos = rs.r[i].hand_grab_points[limb];
                dangle = rs.r[i].dangle != 0;
                antler_vel = state.world.body_chunk_get_vel(creature::chunk(state, d, 5));
                return true;
            }
        return false;
    }

    void attract_to_spores(sys::game_state& state, dcon::room_id room, vec2 pos) {
        state.defer(room, [room, pos](sys::game_state& s) {
            auto& w = s.world;
            auto const view = world::get_room_view(s, room);
            // the closest tile open to the deer
            ivec2 t = room_view::get_tile_position(pos);
            if (!(view.inside(t.x, t.y) && pathing::accessible(view, t, movement))) {
                bool found = false;
                for (int32_t i = 0; i < 7 && !found; ++i)
                    for (auto const dd : custom::eight_directions) {
                        ivec2 const q = t + dd * i;
                        if (view.inside(q.x, q.y) && pathing::accessible(view, q, movement)) {
                            t = q;
                            found = true;
                            break;
                        }
                    }
            }
            auto& rng = s.room_random(room);
            // a conscious deer of the room not after a puff ball
            std::vector<dcon::creature_id> list;
            rooms::for_each_creature(s, room, creature::type::deer, [&](dcon::creature_id d) {
                if (creature::here(s, d, room) && creature::consious(s, d) && !w.deer_data_get_ai(data(s, d)).puff)
                    list.push_back(d);
            });
            if (!list.empty()) {
                auto& a = w.deer_data_get_ai(data(s, list[size_t(rng.range(0, int32_t(list.size())))]));
                a.spore_room = room;
                a.spore_tile = t;
                // (In_Room_Deer_Summoned has no sample here)
                if (rng.value() < 0.5f)
                    return;
            }
            // a deer of the region's other rooms comes over (vanilla: from
            // off screen, in the rooms deer may visit, the rain not near)
            auto const region = w.room_get_region_from_region_membership(room);
            list.clear();
            w.for_each_creature([&](dcon::creature_id d) {
                if (w.creature_get_type(d) != creature::type::deer || w.creature_get_away(d) != 0 || w.creature_get_dead(d))
                    return;
                auto const r = creature::room_of(s, d);
                if (r && r != room && w.room_get_region_from_region_membership(r) == region && dens::rain_utility(s, d, r) <= 0.f
                && !w.deer_data_get_ai(data(s, d)).spore_room)
                    list.push_back(d);
            });
            while (!list.empty()) {
                size_t const k = size_t(rng.range(0, int32_t(list.size())));
                auto const d = list[k];
                if (migrate::plan_trip(s, d, creature::room_of(s, d), room)) {
                    auto& a = w.deer_data_get_ai(data(s, d));
                    a.spore_room = room;
                    a.spore_tile = t;
                    return;
                }
                list.erase(list.begin() + std::ptrdiff_t(k));
            }
        });
    }

    dcon::item_id going_to_puff_ball(sys::game_state const& state, dcon::creature_id c) {
        auto const& a = state.world.deer_data_get_ai(data(state, c));
        return a.puff_visible ? a.puff : dcon::item_id{};
    }

    vec2 antler_point_pos(sys::game_state const& state, dcon::creature_id c, uint32_t k, float side, vec2 head, vec2 antler_pos, float fd) {
        auto const points = state.world.deer_data_get_antlers(data(state, c));
        if (k >= points.size())
            return antler_pos;
        return outposts::transform_to_head_rotat(outposts::transformed_pos(points.at(k), side), antler_pos, custom::aim(head, antler_pos), side, fd);
    }

    float face_dir(sys::game_state const& state, dcon::creature_id c, float alpha) {
        auto const& g = state.world.deer_data_get_graphics(data(state, c));
        float const f = mathf::lerp(g.last_flip, g.flip, alpha);
        return f * 0.85f + mathf::lerp(g.last_antler_random_movement, g.antler_random_movement, alpha) * 0.25f * (1.f - std::fabs(f) * 0.7f);
    }
}
