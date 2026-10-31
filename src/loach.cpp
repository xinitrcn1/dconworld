// Loach.cs, LoachAI.cs, LoachLeg.cs and LoachTentacle.cs (on Tentacle.cs),
// LoachState's interest, from the Watcher.
//
// The body: six chunks from the head (chunk 0, mainBodyChunk) back to the
// sac (4, 5). Its six legs and five mouth tentacles are Tentacles: a list
// of room tiles from the connected chunk out (`segments`, kept four way
// connected) with six TentacleChunks following them. The legs are the rain
// deer's (deer.cpp): FindGrabPos picks a grip below and ahead, the tip
// sticks there (attachedAtTip) and the legs' Support() carries the body.
// The tentacles sweep the ground under the head (SearchGround), reach for
// tracked prey (Hunt), stick to what they touch and recede to the mouth
// with it (Recede); what a tentacle holds and the body touches is swallowed
// (Eat). The limbs work on copies of their tile lists while the loach
// updates (`limb_work`), stored back after.

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

#include "loach.hpp"
#include "body_parts.hpp"
#include "daddy_internal.hpp"
#include "dens.hpp"
#include "items.hpp"
#include "loach_graphics.hpp"
#include "migrate.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "rooms.hpp"
#include "rot.hpp"
#include "tracker.hpp"

namespace loach {
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;
    using creature::relationship;
    using daddy::detail::ray_trace_tiles_for_terrain;
    using daddy::detail::ray_trace_tiles_list;
    using daddy::detail::ray_traced_tiles;

    namespace {
        constexpr float float_max = std::numeric_limits<float>::max();
        constexpr ivec2 four_directions_and_zero[5] = {{0, 0}, {-1, 0}, {0, -1}, {1, 0}, {0, 1}};
        // PhysicalObject.gravity (Loach constructor)
        constexpr float loach_gravity = 0.9f;
        // CreatureTemplate (StaticWorld.InitLoach): baseDamageResistance,
        // baseStunResistance, visualRadius
        constexpr float base_damage_resistance = 50.f;
        constexpr float base_stun_resistance = 8.f;
        constexpr float visual_radius = 1100.f;
        // Loach: maxPreferredHeight, midIdealHeight, minPreferredHeight,
        // legInFrontMargin, the LoachLeg and LoachTentacle lengths
        constexpr int32_t max_preferred_height = 12;
        constexpr int32_t mid_ideal_height = 8;
        constexpr int32_t min_preferred_height = 7;
        constexpr float leg_in_front_margin = 30.f;
        constexpr float leg_length = 200.f;
        constexpr float tentacle_length = 150.f;
        constexpr float rot_tentacle_length = 250.f;
        // Loach.rootRad, tipRad
        constexpr float root_rad = 12.f;
        constexpr float tip_rad = 2.f;
        // StuckTracker parameters (StuckTracker defaults, LoachAI constructor)
        constexpr int32_t min_stuck_counter = 100;
        constexpr int32_t max_stuck_counter = 200;
        constexpr int32_t goal_satisfaction_distance = 3;
        constexpr uint32_t check_past_positions_from = 30;
        constexpr float past_pos_stuck_distance = 5.f;
        constexpr int32_t past_stuck_positions_close = 10;
        // LoachLeg.setGrabDelay
        constexpr int32_t set_grab_delay = 5;

        // Tentacle.TentacleProps: LoachLeg's (false, false, true, 0.5f, 0f,
        // 0.75f, 0.125f, 1.04f, 1.2f, 10f, 1f, 5f, 15, 60, 12, 0) and
        // LoachTentacle's (false, true, false, 0.5f, 0f, 0.1f, 0f, 0.2f,
        // 1.2f, 10f, 0.25f, 3f, 15, 20, 20, 0); goalAttractionSpeedTip is
        // the limb's own (SwitchTask changes it)
        struct tentacle_props {
            float mass_deterioration_per_chunk;
            float goal_attraction_speed;
            float align_to_segment_speed;
            float backtrack_speed;
            float chunk_velocity_cap;
            float tile_tentacle_update_speed;
            float tile_tentacle_snap_with_path_distance;
            int32_t segment_phase_through_terrain_frames;
            int32_t max_pull_tiles_per_tick;
            int32_t terrain_hits_before_phase;
            bool shorten;
        };
        constexpr tentacle_props leg_props{0.5f, 0.125f, 1.04f, 1.2f, 10.f, 1.f, 5.f, 15, 12, 0, true};
        constexpr tentacle_props mouth_props{0.5f, 0.f, 0.2f, 1.2f, 10.f, 0.25f, 3.f, 15, 20, 0, false};

        creature::loach_payload& pl(sys::game_state& state, dcon::creature_id c) { return state.world.creature_get_payload(c).loach; }
        creature::loach_payload const& pl(sys::game_state const& state, dcon::creature_id c) { return state.world.creature_get_payload(c).loach; }
        dcon::loach_data_id data(sys::game_state const& state, dcon::creature_id c) { return creature::loach_data(state.world, c); }
        dcon::loach_limb_id limb_data(dcon::data_container const& w, dcon::creature_part_id p) {
            return dcon::loach_limb_id{dcon::loach_limb_id::value_base_t(w.creature_part_get_payload(p).loach_limb.data)};
        }

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
        // Custom.RotateAroundOrigo
        using body_parts::rotate_around_origo;

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
        // SharedPhysics.TerrainCollisionData (as deer.cpp's)
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

        // one limb while its loach updates
        struct limb_work {
            dcon::creature_part_id part;
            dcon::loach_limb_id id;
            uint32_t number = 0;
            limb t;
            std::vector<ivec2> segments;
            std::vector<ivec2> grab_path;
        };

        std::vector<limb_work>& leg_buffer() {
            static thread_local std::vector<limb_work> buf;
            return buf;
        }
        std::vector<limb_work>& mouth_buffer() {
            static thread_local std::vector<limb_work> buf;
            return buf;
        }
        std::vector<ivec2>& scratch_buffer() {
            static thread_local std::vector<ivec2> buf;
            return buf;
        }

        // a loach's update context
        struct ctx {
            sys::game_state& state;
            dcon::creature_id c;
            room_view const& room;
            creature::loach_payload& p;
            dcon::loach_data_id d;
            ai_state& ai;
            rw::rng rng;
            dcon::body_chunk_id ch[body_chunks];
            std::vector<limb_work>& legs;
            std::vector<limb_work>& tents;
            std::vector<ivec2>& scratch;
            std::optional<pathing::field_id> field;

            // (keyed by the constructing call site too: several ctxs of one
            // loach in one tick roll apart)
            ctx(sys::game_state& s, dcon::creature_id cc, room_view const& r, std::source_location const l = std::source_location::current())
                : state(s), c(cc), room(r), p(pl(s, cc)), d(data(s, cc)), ai(s.world.loach_data_get_ai(data(s, cc))), rng(rw::random(s.seed, s.tick, uint32_t(cc.value), l)),
                  legs(leg_buffer()), tents(mouth_buffer()), scratch(scratch_buffer()) {
                for (uint32_t i = 0; i < body_chunks; ++i)
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
            bool rot() const { return p.variant == variant::rot; }
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
            // AItile.floorAltitude (smoothedFloorAltitude: the same here),
            // getClampedAItile
            int32_t floor_altitude(ivec2 t) {
                return pathing::floor_altitude(room, ivec2{custom::int_clamp(t.x, 0, room.width - 1), custom::int_clamp(t.y, 0, room.height - 1)});
            }
            // PhysicalObject.WeightedPush
            void weighted_push(uint32_t a, uint32_t b, vec2 dir, float frc) {
                float const num = mass(b) / (mass(a) + mass(b));
                vel(a) += dir * frc * num;
                vel(b) -= dir * frc * (1.f - num);
            }
            // Loach.HeadDir
            vec2 head_dir() {
                vec2 const bd = p.bod_dir;
                vec2 const from = custom::perpendicular_vector(bd) * mathf::sign(bd.x);
                return rw::slerp(from, vec2{0.f, 1.f}, mathf::pow(mathf::inverse_lerp(1.f, 0.f, std::fabs(bd.x)), 0.5f));
            }
            // Loach.CloseToEdge
            float close_to_edge() {
                float const x = pos(0).x;
                float const pw = room.pixel_width();
                return mathf::max(mathf::inverse_lerp(500.f, 300.f, x), mathf::inverse_lerp(pw - 500.f, pw - 300.f, x));
            }
            // GetUnstuckForce: StuckTracker.Utility
            float unstuck_force() { return mathf::inverse_lerp(float(min_stuck_counter), float(max_stuck_counter), float(ai.stuck_counter)); }
            // Loach.isLurching
            bool is_lurching() const { return p.lurch_progress > 8; }
            // LoachState.isChasing
            bool chasing() const { return ai.interest > 0; }
        };

        void load_part(ctx& x, dcon::creature_part_id part, std::vector<limb_work>& into, uint32_t& k) {
            auto& w = x.w();
            if (k >= into.size())
                into.resize(k + 1);
            auto& lw = into[k];
            lw.part = part;
            lw.id = limb_data(w, part);
            lw.number = w.creature_part_get_index(part);
            lw.t = w.loach_limb_get_t(lw.id);
            auto const segs = w.loach_limb_get_segments(lw.id);
            lw.segments.assign(segs.begin(), segs.end());
            auto const gp = w.loach_limb_get_grab_path(lw.id);
            lw.grab_path.assign(gp.begin(), gp.end());
            ++k;
        }
        void load(ctx& x) {
            auto& w = x.w();
            uint32_t nl = 0, nt = 0;
            for (auto const o : creature::parts(w, x.c)) {
                auto const part = w.part_ownership_get_part(o);
                auto const kind = w.creature_part_get_kind(part);
                if (kind == creature::part_kind::loach_leg && nl < leg_count)
                    load_part(x, part, x.legs, nl);
                else if (kind == creature::part_kind::loach_tentacle && nt < tentacle_count)
                    load_part(x, part, x.tents, nt);
            }
            x.legs.resize(nl);
            x.tents.resize(nt);
        }
        void store(ctx& x) {
            auto& w = x.w();
            auto const put = [&](limb_work& lw) {
                w.loach_limb_set_t(lw.id, lw.t);
                w.loach_limb_get_segments(lw.id).load_range(lw.segments.data(), lw.segments.data() + lw.segments.size());
                w.loach_limb_get_grab_path(lw.id).load_range(lw.grab_path.data(), lw.grab_path.data() + lw.grab_path.size());
            };
            for (auto& lw : x.legs)
                put(lw);
            for (auto& lw : x.tents)
                put(lw);
        }

        //
        // Tentacle
        //
        tentacle_props const& props(limb_work const& lw) { return lw.t.mouth ? mouth_props : leg_props; }
        vec2 float_base(ctx& x, limb_work const& lw) { return x.pos(lw.t.connected_chunk); }
        limb_chunk& tip(limb_work& lw) { return lw.t.c[limb_chunks - 1]; }
        std::optional<ivec2> grab_dest(limb_work const& lw) {
            if (lw.t.float_grab_dest)
                return room_view::get_tile_position(*lw.t.float_grab_dest);
            return std::nullopt;
        }
        // Tentacle.BasePos (with CheckIfAnySegmentsLeft)
        ivec2 base_pos(ctx& x, limb_work& lw) {
            if (lw.segments.empty())
                lw.segments.push_back(room_view::get_tile_position(float_base(x, lw)));
            return lw.segments[0];
        }
        int32_t current_segment(limb_work const& lw, limb_chunk const& c) {
            int32_t const n = int32_t(lw.segments.size());
            return custom::int_clamp(int32_t(c.t_pos * float(n - 1)), 0, n - 1);
        }
        ivec2 stuck_pos(limb_work const& lw, limb_chunk const& c) { return lw.segments[size_t(current_segment(lw, c))]; }
        bool visual(ctx& x, ivec2 a, ivec2 b) { return ray_trace_tiles_for_terrain(x.room, a, b); }

        // TentacleChunk.Reset
        void chunk_reset(limb_work& lw, limb_chunk& c) {
            c.phase = -1.f;
            c.pos = room_view::middle_of_tile(stuck_pos(lw, c));
            c.last_pos = c.pos;
            c.vel = vec2{0.f, 0.f};
        }

        // TentacleChunk.Update (no rope: RopeActive is false)
        void chunk_update(ctx& x, limb_work& lw, uint32_t i) {
            auto& c = lw.t.c[i];
            auto& t = lw.t;
            auto const& tp = props(lw);
            float const n = float(limb_chunks);
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
                        c.pos -= a * (ideal - num) * (1.f - tp.mass_deterioration_per_chunk);
                        c.vel -= a * (ideal - num) * (1.f - tp.mass_deterioration_per_chunk);
                        prev.pos += a * (ideal - num) * tp.mass_deterioration_per_chunk;
                        prev.vel += a * (ideal - num) * tp.mass_deterioration_per_chunk;
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
                            c.vel += rw::clamp_magnitude(room_view::middle_of_tile(lw.segments[size_t(k - 1)]) - c.pos, 20.f) * tp.backtrack_speed / 20.f;
                            flag = true;
                        }
                    }
                    if (!flag) {
                        vec2 const fb = i > 0 ? t.c[i - 1].pos : float_base(x, lw);
                        c.vel += rw::clamp_magnitude(fb - c.pos, 20.f) * tp.backtrack_speed / 20.f;
                        if (i > 0)
                            t.c[i - 1].vel += rw::clamp_magnitude(fb - c.pos, 20.f) * tp.backtrack_speed / 20.f;
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
                            ivec2 const s = lw.segments[size_t(k)];
                            if (visual(x, here, s)) {
                                c.vel += rw::clamp_magnitude(room_view::middle_of_tile(s) - c.pos, 20.f) * 0.005f;
                                break;
                            }
                        }
                    } else {
                        if (t.float_grab_dest) {
                            float const k = i == limb_chunks - 1 ? t.goal_attraction_speed_tip : tp.goal_attraction_speed;
                            c.vel += rw::clamp_magnitude(*t.float_grab_dest - c.pos, 20.f) * k / 20.f;
                        }
                        c.vel += rw::clamp_magnitude(room_view::middle_of_tile(lw.segments[size_t(cs)]) - c.pos, 20.f) * tp.align_to_segment_speed / 20.f;
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
                        prev.pos += a * (ideal - num) * tp.mass_deterioration_per_chunk;
                        prev.vel += a * (ideal - num) * tp.mass_deterioration_per_chunk;
                    }
                    c.stretched_fac = num > 0.f ? ideal / num : 1.f;
                }
            }
        }

        // Tentacle.MoveSegment
        bool move_segment(ctx& x, limb_work& lw, int32_t s, ivec2 dest, bool allow_occupied, bool allow_solid) {
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

        // Tentacle.PullAtTentacle
        void pull_at_tentacle(ctx& x, limb_work& lw, int32_t start, bool only_simple_pull, std::vector<ivec2>& path) {
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

        // Tentacle.FindTentaclePath (as deer.cpp's: a greedy best first
        // search from the goal)
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
        void move_aligned_segments_with_path(ctx& x, limb_work& lw, std::vector<ivec2>& path) {
            auto& seg = lw.segments;
            auto& gp = lw.grab_path;
            int32_t const lim = std::min(int32_t(seg.size()) - 1, int32_t(gp.size()) - 1);
            int32_t num = lim;
            int32_t k = 0;
            while (k < lim && float_dist(seg[size_t(k + 1)], gp[size_t(k + 1)]) <= props(lw).tile_tentacle_snap_with_path_distance
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
            lw.t.pull_counter += num3 - 1;
            pull_at_tentacle(x, lw, num, true, path);
        }

        // Tentacle.FindGrabPath
        void find_grab_path(ctx& x, limb_work& lw, std::vector<ivec2>& path) {
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
        void move_grab_dest(ctx& x, limb_work& lw, vec2 to, std::vector<ivec2>& path) {
            if (auto const g = grab_dest(lw); g && *g == room_view::get_tile_position(to))
                return;
            lw.t.float_grab_dest = to;
            find_grab_path(x, lw, path);
        }

        // Tentacle.Grow
        void grow(ctx& x, limb_work& lw, int32_t add) {
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
        void move_base(ctx& x, limb_work& lw, ivec2 to, std::vector<ivec2>& path) {
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
        void align_with_grab_path(ctx& x, limb_work& lw, std::vector<ivec2>& path) {
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
        float current_length(limb_work const& lw) {
            float num = float(lw.segments.size()) * 20.f;
            for (size_t i = 0; i + 2 < lw.segments.size(); ++i)
                if (are_diagonal_neighbors(lw.segments[i], lw.segments[i + 2]))
                    num -= 5.9000006f;
            return num;
        }

        // Tentacle.AdjustLength
        void adjust_length(ctx& x, limb_work& lw, std::vector<ivec2>& path) {
            float const num = current_length(lw);
            float num2 = lw.t.ideal_length;
            if (props(lw).shorten && !lw.grab_path.empty())
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

        // LoachLeg.GravityDirection (the tentacles': Tentacle's down),
        // Tentacle.Gravity
        void gravity(ctx& x, limb_work& lw) {
            ivec2 b{0, -1};
            if (!lw.t.mouth && x.rng.value() < 0.5f)
                b = ivec2{int32_t(mathf::sign(lw.t.tentacle_dir.x)), -1};
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
        void base_update(ctx& x, limb_work& lw) {
            auto& path = x.scratch;
            base_pos(x, lw);
            move_base(x, lw, room_view::get_tile_position(float_base(x, lw)), path);
            lw.t.update_counter += props(lw).tile_tentacle_update_speed;
            if (lw.t.update_counter >= 1.f) {
                lw.t.pulls_this_tick = props(lw).max_pull_tiles_per_tick;
                if (lw.t.limp)
                    gravity(x, lw);
                else if (!lw.grab_path.empty())
                    align_with_grab_path(x, lw, path);
                adjust_length(x, lw, path);
                lw.t.update_counter -= 1.f;
            }
            lw.t.backtrack_from = -1;
            for (uint32_t i = 0; i < limb_chunks; ++i)
                chunk_update(x, lw, i);
        }

        // Tentacle.NewRoom
        void limb_new_room(ctx& x, limb_work& lw) {
            lw.segments.assign(size_t(std::max(1, int32_t(lw.t.ideal_length / 20.f))), room_view::get_tile_position(x.pos(0)));
            for (auto& c : lw.t.c)
                chunk_reset(lw, c);
            lw.grab_path.clear();
            lw.t.float_grab_dest.reset();
        }

        //
        // LoachLeg
        //
        // OtherTentacleInPair
        limb_work& other_in_pair(ctx& x, limb_work const& lw) {
            int32_t const n = int32_t(lw.number);
            int32_t const half = int32_t(leg_count / 2);
            int32_t const o = n < half ? n + half : n - half;
            for (auto& l : x.legs)
                if (int32_t(l.number) == o)
                    return l;
            return x.legs[0];
        }

        // LoachLeg.ReleaseGrip
        void release_grip(ctx& x, limb_work& lw) {
            if (other_in_pair(x, lw).t.grab_delay < 1 && lw.t.grab_delay < 1)
                lw.t.grab_delay = set_grab_delay;
            lw.t.float_grab_dest.reset();
            lw.t.sure_of_footing_counter = 0;
            lw.t.fatigue = mathf::min(lw.t.fatigue + 0.1f, 1.f);
        }

        // LoachLeg.UpdateDesiredGrabPos
        void update_desired_grab_pos(ctx& x, limb_work& lw) {
            float const reach = mathf::min(x.p.next_floor_height, lw.t.max_length * 0.9f);
            vec2 const conn = float_base(x, lw);
            if (x.p.stay_still) {
                lw.t.desired_grab_pos = conn + rw::normalized(vec2{0.f, -1.f} + lw.t.tentacle_dir * 0.5f) * reach;
                return;
            }
            float const pk = lw.number < 2 ? 0.8f : 0.6f;
            lw.t.desired_grab_pos = conn + rw::normalized(vec2{0.f, -1.f} + x.p.move_direction * pk + lw.t.tentacle_dir * 0.5f) * reach;
        }

        // LoachLeg.GripPointAttractiveness
        float grip_point_attractiveness(ctx& x, limb_work const& lw, ivec2 pos) {
            float const d = float_dist(room_view::get_tile_position(lw.t.desired_grab_pos), pos);
            if (!has_any_solid(x.room, pos + ivec2{0, 1}))
                return 100.f / d;
            return 65.f / d;
        }

        // LoachLeg.FindGrabPos
        void find_grab_pos(ctx& x, limb_work& lw) {
            if (lw.t.grab_delay > 0) {
                --lw.t.grab_delay;
                return;
            }
            std::optional<ivec2> iv;
            int32_t const b = x.rng.range(2, 5) * (x.rng.value() < 0.5f ? -1 : 1);
            ivec2 const dt = room_view::get_tile_position(lw.t.desired_grab_pos);
            for (int32_t num = 0; num < 9 && !iv; ++num) {
                iv = first_solid(x.room, base_pos(x, lw), dt + custom::eight_directions_and_zero[num] * b);
                if (iv && has_any_solid(x.room, *iv + ivec2{0, 1}))
                    iv.reset();
            }
            if (!iv)
                return;
            auto const g = grab_dest(lw);
            if (!g || grip_point_attractiveness(x, lw, *iv) > grip_point_attractiveness(x, lw, *g) * (x.p.stay_still ? 1.f : 3.f)) {
                vec2 const m = room_view::middle_of_tile(*iv);
                vec2 const fb = float_base(x, lw);
                move_grab_dest(x, lw, restrict_in_rect(fb + lw.t.tentacle_dir * rw::distance(fb, m) * 0.5f, m.x - 18.f, m.y - 11.f, m.x + 18.f, m.y + 11.f), x.scratch);
                lw.t.sure_of_footing_counter = 0;
                return;
            }
            ++lw.t.sure_of_footing_counter;
        }

        // LoachLeg.Support (no Remix: 0.5)
        float support(ctx& x, limb_work& lw) {
            if (!lw.t.attached_at_tip)
                return 0.f;
            float num = 0.f;
            float const bx = x.pos(2).x;
            for (auto& o : x.legs)
                if (&o != &lw && o.t.attached_at_tip && (tip(o).pos.x < bx) != (tip(lw).pos.x < bx))
                    num = mathf::max(num, mathf::pow(std::sin(mathf::inverse_lerp(400.f, 0.f, std::fabs(bx - tip(o).pos.x)) * mathf::pi), 0.3f));
            return mathf::inverse_lerp(mathf::lerp(0.5f, -1.f, num), 1.f, rw::dot(vec2{0.f, -1.f}, custom::dir_vec(float_base(x, lw), tip(lw).pos)));
        }

        // LoachLeg.ReleaseScore
        float release_score(ctx& x, limb_work& lw) {
            float num;
            if (!grab_dest(lw))
                num = rw::distance(tip(lw).pos, lw.t.desired_grab_pos);
            else
                num = rw::distance(*lw.t.float_grab_dest, lw.t.desired_grab_pos);
            if (lw.t.attached_at_tip)
                num *= 2.f;
            if (!other_in_pair(x, lw).t.attached_at_tip)
                num /= 100.f;
            num *= 1.f + (float_base(x, lw).x - tip(lw).pos.x) * mathf::sign(x.p.move_direction.x) * 1.4f;
            num /= 1.f + support(x, lw) * 10.f;
            return num * (1.f - lw.t.fatigue);
        }

        // Tentacle.NewRoom + LoachLeg.NewRoom
        void leg_new_room(ctx& x, limb_work& lw) {
            limb_new_room(x, lw);
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

        // LoachLeg.Update
        void leg_update(ctx& x, limb_work& lw) {
            auto& t = lw.t;
            base_update(x, lw);
            t.fatigue = mathf::lerp(t.fatigue, 0.f, 0.01f);
            t.smoothed_footing_security = mathf::lerp(t.smoothed_footing_security, t.attached_at_tip ? 0.f : mathf::inverse_lerp(3.f, 30.f, float(t.sure_of_footing_counter)), 0.2f);
            t.attached_at_tip = 0;
            t.max_length = 20.f * float(max_preferred_height) * 1.3333334f;
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
            for (uint32_t j = 0; j < limb_chunks; ++j) {
                auto& c = t.c[j];
                c.rad = 5.f;
                float const num = float(j) / float(limb_chunks - 1);
                c.vel *= 0.5f;
                if (t.backtrack_from == -1 || int32_t(j) < t.backtrack_from) {
                    if (j < limb_chunks - 1)
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
                    // (headChunk: the head's reach, not the shoulder's)
                    if (!custom::dist_less(tip(lw).pos, x.pos(0), t.max_length))
                        release_grip(x, lw);
                    if (!custom::dist_less(tip(lw).pos, x.pos(0), t.max_length * 0.9f))
                        x.p.held_back_by_leg = 1;
                } else {
                    release_grip(x, lw);
                }
                // (Loach_Step has no sample here)
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
        // LoachAI (tracker hooks) and LoachState
        //
        // the tracker's settings: Tracker(10, 5, -1, ...), PreyTracker(5, 2,
        // 3, 70, 0.5), ThreatTracker(3)
        tracker::params const& tracker_params() {
            static tracker::params const p = [] {
                tracker::params q;
                q.see_around_corners = 10;
                q.frames_to_remember = -1;
                q.ghost_dismissal_range = 10;
                q.prey_max = 5;
                q.persistance_bias = 2.f;
                q.sure_to_get = 3.f;
                q.sure_to_lose = 70.f;
                q.threat_max = 3;
                return q;
            }();
            return p;
        }

        // ArtificialIntelligence.VisualScore > 0 (visualRadius 1100, no
        // movement based vision) and the room's visual contact, from the
        // head (Loach.VisionPoint)
        bool visual_contact(ctx& x, vec2 look) {
            vec2 const eye = x.pos(0);
            return custom::dist_less(eye, look, visual_radius) && x.room.visual_contact(eye, look);
        }

        // LoachAI.ModuleToTrackRelationship
        creature::track_module module_of(relationship r) noexcept {
            if (r.type == creature::rel::afraid)
                return creature::track_module::threat;
            if (r.type == creature::rel::eats || r.type == creature::rel::attacks)
                return creature::track_module::prey;
            return creature::track_module::none;
        }

        // LoachAI.UpdateDynamicRelationship: the static one; the creature
        // it was given as prey (PreyTracker.AddPrey after
        // LoachState.SetNewTarget) is tracked as prey while the interest lasts
        relationship relate_to(ctx& x, dcon::creature_id t) {
            if (t && t == x.ai.target && x.ai.interest > 0)
                return relationship{creature::rel::eats, 1.f};
            return creature::static_relationship(x.state, x.c, t);
        }

        bool hook_sees(void* u, dcon::body_chunk_id k) {
            auto& x = *static_cast<ctx*>(u);
            return visual_contact(x, x.state.world.body_chunk_get_pos(k));
        }
        bool hook_sees_point(void* u, vec2 p) { return visual_contact(*static_cast<ctx*>(u), p); }
        relationship hook_relate(void* u, creature::tracked const& rep) { return relate_to(*static_cast<ctx*>(u), rep.who); }
        bool hook_reach(void* u, ivec2 t) {
            auto& x = *static_cast<ctx*>(u);
            return x.reachable(t);
        }
        tracker::hooks make_hooks(ctx& x) {
            tracker::hooks h;
            h.user = &x;
            h.sees = hook_sees;
            h.sees_point = hook_sees_point;
            h.relate = hook_relate;
            h.module = module_of;
            h.reach = hook_reach;
            return h;
        }

        // LoachState.SetNewTarget
        bool set_new_target(ctx& x, float bonus) {
            bonus = mathf::clamp(bonus, -1.f, 1.f);
            float const num = mathf::inverse_lerp(0.f, 350.f, float(x.ai.interest));
            if (x.rng.value() + bonus > num) {
                x.ai.interest = 250;
                return true;
            }
            return false;
        }
        // LoachState.UpdateInterest: true once it is gone
        bool update_interest(ctx& x, int32_t change) {
            x.ai.interest = custom::int_clamp(x.ai.interest + change, 0, 400);
            return x.ai.interest <= 0;
        }
        // PreyTracker.ForgetAllPrey + AddPrey(rep of `t`)
        void give_prey(ctx& x, dcon::creature_id t) {
            x.ai.target = t;
            tracker::see(x.state, x.c, x.room, t, make_hooks(x));
        }

        // the prey tracker's representations (TotalTrackedPrey, GetTrackedPrey)
        uint32_t tracked_prey(ctx& x, creature::tracked const* out[creature::max_tracked]) {
            auto const& ts = tracker::of(x.state, x.c);
            uint32_t n = 0;
            for (uint32_t i = 0; i < ts.count; ++i)
                if (ts.reps[i].module == creature::track_module::prey && creature::live(x.state, ts.reps[i].who))
                    out[n++] = &ts.reps[i];
            return n;
        }
        creature::tracked const* rep_of(ctx& x, dcon::creature_id t) { return t ? tracker::find(tracker::of(x.state, x.c), t) : nullptr; }

        // Loach.StartLurch
        void start_lurch(ctx& x) {
            x.p.lurching = 1;
            x.ai.lurch_sound = 1;
            x.ai.floor_detected = 0;
            x.ai.interest -= 300;
        }

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
                for (uint32_t j = check_past_positions_from; j < a.past_count; ++j)
                    if (float_dist(here, a.past[(a.past_head + j) % stuck_positions]) < past_pos_stuck_distance)
                        ++num;
            }
            if (num > past_stuck_positions_close)
                ++a.stuck_counter;
            else
                a.stuck_counter -= 2;
            a.stuck_counter = custom::int_clamp(a.stuck_counter, 0, max_stuck_counter);
        }

        // Loach.Climbable
        bool climbable(ctx& x, ivec2 t) {
            return x.room.inside(t.x, t.y)
                && (pathing::ai_terrain_proximity(x.state, x.room.id, t) == 1 || pathing::tile_access(x.room, t) == pathing::access::climb);
        }

        // LoachAI.LegalInRoomDest, GoodInRoomDest
        bool legal_in_room_dest(ctx& x, ivec2 t) {
            return x.room.inside(t.x, t.y) && x.reachable(t) && !custom::dist_less(rw::to_vec2(x.here()), rw::to_vec2(t), 10.f) && t.x >= 5
                && t.x <= x.room.width - 5;
        }
        bool good_in_room_dest(ctx& x, ivec2 t) {
            if (!legal_in_room_dest(x, t))
                return false;
            if (x.room.width > 180) {
                if (std::abs(x.here().x - t.x) < x.room.width / 3)
                    return false;
                if (t.x > 70 && t.x < x.room.width - 70)
                    return false;
            }
            return true;
        }

        // LoachAI.GetWanderCoordFromXPosition
        ivec2 wander_coord_from_x(ctx& x, int32_t tile_x) {
            int32_t num = x.here().y;
            for (int32_t i = 0; i < 2000; i += 20) {
                if (legal_in_room_dest(x, room_view::get_tile_position(vec2{float(tile_x) * 20.f, float(i)}))) {
                    num = custom::int_clamp(i / 20 + int32_t(x.p.preferred_height) + x.rng.range(-15, 15) - 2, 0, x.room.height - 2);
                    if (good_in_room_dest(x, room_view::get_tile_position(vec2{float(tile_x) * 20.f, float(num) * 20.f})))
                        break;
                }
            }
            return ivec2{tile_x, num};
        }

        // LoachAI.IdleRoomWanderGoal
        ivec2 idle_room_wander_goal(ctx& x) {
            float num = x.p.flip_dir * -1.f;
            if (x.rng.value() < 0.4f)
                num = x.p.flip_dir;
            int32_t num2 = x.rng.range(16, 25);
            ivec2 const here = x.here();
            ivec2 g;
            auto const pick = [&]() {
                float const r = mathf::lerp(0.85f, 1.f, x.rng.value());
                int32_t const lo = num2, hi = std::max(num2, x.room.width - num2);
                return custom::int_clamp(int32_t(float(here.x) + num * (float(x.room.width) * r)), lo, hi);
            };
            do {
                int32_t num3 = pick();
                if (std::abs(here.x - num3) < 22) {
                    num *= -1.f;
                    num3 = pick();
                }
                g = wander_coord_from_x(x, num3);
                num2 += 7;
            } while (num2 <= 80 && x.room.get_tile(g).solid());
            return g;
        }

        // LoachAI.IdleSpotScore (lower is better; AItile.visibility is
        // taken as 1)
        float idle_spot_score(ctx& x, ivec2 coord) {
            if (!x.room.inside(coord.x, coord.y))
                return 50.f;
            if (!x.reachable(coord))
                return float_max;
            float num = 10.f;
            num -= mathf::max(0.f, float_dist(coord, x.here()) - 30.f) * 1.5f;
            num += float(x.floor_altitude(coord)) * 10.f;
            return num;
        }

        // LoachAI.TravelUtility (migrate.hpp's trips)
        float travel_utility(ctx& x) {
            if (!migrate::want_to(x.state, x.c))
                return 0.f;
            auto const dest = migrate::destination(x.state, x.c);
            if (!dest)
                return 0.f;
            float const num = migrate::attraction(x.state, x.c, x.room.id);
            float const num2 = migrate::attraction(x.state, x.c, dest);
            float const num3 = mathf::lerp(0.99f, 0.1f, mathf::pow(num, 0.25f));
            if (num > num2)
                return num3 * 0.25f;
            if (num == num2)
                return num3 * 0.5f;
            return num3;
        }

        // LoachAI.Update: the modules, DetermineBehavior and Act
        void ai_update(ctx& x) {
            auto& a = x.ai;
            auto& w = x.w();
            ivec2 const here = x.here();
            if (here.x > 10 && here.x < x.room.width - 11)
                ++a.time_in_this_room;
            a.focus = dcon::creature_id{};
            // the forced prey gone: forgotten
            if (a.target && (!creature::live(x.state, a.target) || creature::dead(x.state, a.target)))
                a.target = dcon::creature_id{};
            tracker::hooks const h = make_hooks(x);
            tracker::update(x.state, x.c, x.room, here, tracker_params(), h);
            stuck_update(x);
            auto const& ts = tracker::of(x.state, x.c);
            // DetermineBehavior: UtilityComparer over the rain (1), the
            // threats (0.4), the prey (0.4) and being stuck (1)
            behavior b = behavior::idle;
            float utility = 0.f;
            if (x.chasing()) {
                b = behavior::hunt;
                utility = 1.f;
            } else {
                if (float const rain = dens::rain_utility(x.state, x.c, x.room.id); rain > utility) {
                    utility = rain;
                    b = behavior::escape_rain;
                }
                if (float const threat = tracker::threat_utility(ts) * 0.4f; threat > utility && tracker::threat(ts)) {
                    utility = threat;
                    b = behavior::flee;
                }
                if (float const prey = tracker::prey(ts) ? tracker::prey_utility(x.state, x.c, x.room, here, tracker_params()) * 0.4f : 0.f; prey > utility) {
                    utility = prey;
                    b = behavior::hunt;
                }
                if (float const stuck = x.unstuck_force(); stuck > utility) {
                    utility = stuck;
                    b = behavior::get_unstuck;
                }
                if (utility < 0.05f) {
                    utility = 0.05f;
                    b = behavior::idle;
                }
                if (b != behavior::escape_rain && b != behavior::flee) {
                    if (float const tu = travel_utility(x); utility < tu) {
                        utility = tu;
                        b = behavior::travelling;
                    }
                }
            }
            w.creature_set_behavior(x.c, uint8_t(b));
            w.creature_set_current_utility(x.c, utility);
            // stuckTracker.satisfiedWithThisPosition = !AtSitDestination
            ivec2 const dest = x.destination();
            bool const at_sit = b == behavior::idle && climbable(x, dest) && custom::manhattan_distance(here, dest) < 2;
            a.satisfied = at_sit ? 0 : 1;
            // LoachAI.Act
            switch (b) {
            case behavior::idle: {
                // the abstract AI's trips to other rooms (migrate.hpp)
                migrate::idle_roll(x.state, x.c, x.room);
                // (slime molds aren't sought out)
                int32_t const was = a.idle_counter;
                --a.idle_counter;
                if (idle_spot_score(x, dest) == float_max)
                    a.idle_counter = 0;
                if (custom::dist_less(x.pos(0), room_view::middle_of_tile(dest), 250.f))
                    a.idle_counter -= 40;
                if (a.idle_counter < 1) {
                    if (was > 0)
                        a.forbidden_idle_spot = dest;
                    ivec2 const wc = idle_room_wander_goal(x);
                    if (idle_spot_score(x, wc) < idle_spot_score(x, dest)) {
                        x.set_destination(wc);
                        a.idle_counter = x.rng.range(2000, 6000);
                    } else if (++a.idle_spot_win_streak > 50) {
                        a.idle_counter = x.rng.range(1000, 4000);
                        a.idle_spot_win_streak = 0;
                    }
                }
                break;
            }
            case behavior::flee:
                if (auto const* t = tracker::threat(ts))
                    a.focus = t->who;
                x.set_destination(tracker::flee_to(x.state, x.c, x.room, here, 1, 30, tracker_params(), h));
                break;
            case behavior::travelling:
                if (auto const t = migrate::travel_tile(x.state, x.c, x.room))
                    x.set_destination(*t);
                break;
            case behavior::hunt:
                if (auto const* r = tracker::prey(ts)) {
                    x.set_destination(tracker::best_guess(*r));
                    a.focus = r->who;
                }
                break;
            case behavior::escape_rain:
                // DenFinder (dens.hpp): its den, through the room exits
                if (auto const e = dens::home_tile(x.state, x.c, x.room))
                    x.set_destination(*e);
                break;
            case behavior::get_unstuck:
                // (GetUnstuckPosCalculator: a random spot near by)
                if (x.rng.value() < 0.02f) {
                    ivec2 const t = room_view::get_tile_position(x.pos(0) + custom::rnv(x.rng) * 200.f);
                    if (x.accessible(t))
                        x.set_destination(t);
                }
                break;
            case behavior::return_prey:
            case behavior::investigate_sound:
                break;
            }
            a.last_behavior = b;
        }

        // StandardPather.FollowPath from `from`: the tile to step to
        std::optional<ivec2> follow_path(ctx& x, ivec2 from) {
            if (!x.accessible(from))
                return std::nullopt;
            return pathing::next_step(x.state, x.path(), from);
        }

        //
        // LoachTentacle
        //
        uint32_t grab_total(ctx& x) {
            uint32_t n = 0;
            for (auto const& tw : x.tents)
                if (tw.t.grab)
                    ++n;
            return n;
        }

        // the creature chunk a tentacle holds (none: gone, or out of the room)
        dcon::body_chunk_id grab_chunk(ctx& x, limb_work const& tw) {
            if (!tw.t.grab || !creature::here(x.state, tw.t.grab, x.room.id))
                return dcon::body_chunk_id{};
            if (tw.t.grab_chunk >= creature::chunk_count_of(x.state, tw.t.grab))
                return dcon::body_chunk_id{};
            return creature::chunk_of(x.state, tw.t.grab, tw.t.grab_chunk);
        }

        // LoachTentacle.SwitchTask
        void switch_task(limb_work& tw, task nt, rw::rng& rng) {
            auto& t = tw.t;
            t.goal_attraction_speed_tip = nt == task::lurch_search ? 250.f : 0.1f;
            if (nt != task::hunt)
                t.hunted = dcon::creature_id{};
            if (nt != task::recede) {
                t.grasp_age = 0;
                t.grab = dcon::creature_id{};
            } else {
                t.grasp_age = rng.range(0, 35);
            }
            if (nt == task::hunt_backup) {
                t.hunt_backup_check1 = 1;
                t.hunt_backup_check2 = 0;
            }
            t.current = nt;
        }

        // LoachTentacle.LookForHuntedCreature
        void look_for_hunted_creature(ctx& x, limb_work& tw) {
            if (tw.t.current == task::recede)
                return;
            creature::tracked const* prey[creature::max_tracked];
            uint32_t const n = tracked_prey(x, prey);
            if (n == 0)
                return;
            auto const* r = prey[size_t(x.rng.range(0, int32_t(n)))];
            if (rw::distance(room_view::middle_of_tile(tracker::best_guess(*r)), float_base(x, tw)) > tw.t.ideal_length)
                return;
            // (IsCreatureCaughtEnough needs 7 of its 5 tentacles: never)
            dcon::creature_id const who = r->who;
            switch_task(tw, task::hunt, x.rng);
            tw.t.hunted = who;
        }

        // LoachTentacle.CollideWithCreature
        void collide_with_creature(ctx& x, limb_work& tw, uint32_t k, dcon::body_chunk_id cc) {
            auto& t = tw.t;
            if (t.backtrack_from > -1 && t.backtrack_from <= int32_t(k))
                return;
            auto& w = x.w();
            auto& c = t.c[k];
            vec2& cp = w.body_chunk_get_pos(cc);
            float const crad = w.body_chunk_get_rad(cc);
            float const num = rw::distance(c.pos, cp);
            float const num2 = (c.rad + crad) / 4.f;
            vec2 const a = custom::dir_vec(c.pos, cp);
            float const m = w.body_chunk_get_mass(cc);
            float const num3 = m / (m + 0.01f);
            float const d = 0.8f;
            c.pos += a * (num - num2) * num3 * d;
            c.vel += a * (num - num2) * num3 * d;
            cp -= a * (num - num2) * (1.f - num3) * d;
            w.body_chunk_get_vel(cc) -= a * (num - num2) * (1.f - num3) * d;
        }

        // the creature `t` is immune to tentacles (AbstractCreature.tentacleImmune:
        // the spawn flag, another loach)
        bool tentacle_immune(sys::game_state const& state, dcon::creature_id t) {
            return state.world.creature_get_type(t) == creature::type::loach || (state.world.creature_get_spawn_flags(t) & dens::flag_tentacle_immune) != 0;
        }

        // LoachTentacle.Touch
        void touch(ctx& x, limb_work& tw) {
            auto& w = x.w();
            auto& t = tw.t;
            bool flag = false;
            bool done = false;
            rooms::for_each_creature(x.state, x.room.id, [&](dcon::creature_id o) {
                if (done || o == x.c || !creature::here(x.state, o, x.room.id) || tentacle_immune(x.state, o))
                    return;
                uint32_t const m = creature::chunk_count(x.state, o);
                if (m == 0 || !custom::dist_less(t.c[0].pos, w.body_chunk_get_pos(creature::chunk(x.state, o, 0)), t.ideal_length + 200.f))
                    return;
                for (uint32_t j = 0; j < limb_chunks && !done; ++j) {
                    for (uint32_t k = 0; k < m; ++k) {
                        auto const cc = creature::chunk(x.state, o, k);
                        if (!custom::dist_less(t.c[j].pos, w.body_chunk_get_pos(cc), t.c[j].rad + w.body_chunk_get_rad(cc)))
                            continue;
                        // (the Void Weaver's gift sparing the Watcher from the
                        // rot's tentacles isn't ported: the only flag for it is
                        // graphics state, which the simulation doesn't read)
                        tracker::see(x.state, x.c, x.room, o, make_hooks(x));
                        collide_with_creature(x, tw, j, cc);
                        if (t.grab || j != limb_chunks - 1)
                            break;
                        flag = true;
                        if (rw::distance(t.c[j].vel, w.body_chunk_get_vel(cc)) >= mathf::lerp(1.f, 8.f, t.sticky))
                            break;
                        bool flag2 = rep_of(x, o) != nullptr;
                        for (uint32_t q = 0; q < limb_chunks && flag2; ++q)
                            if (t.c[q].phase > -1.f || x.room.get_tile(t.c[q].pos).solid())
                                flag2 = false;
                        if (flag2) {
                            if (set_new_target(x, 0.25f))
                                give_prey(x, o);
                            // (Loach_Tentacle_Grab has no sample here)
                            switch_task(tw, task::recede, x.rng);
                            t.grab = o;
                            t.grab_chunk = uint8_t(k);
                            done = true;
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

        // LoachTentacle.Hunt
        void hunt(ctx& x, limb_work& tw) {
            auto& t = tw.t;
            auto const* r = rep_of(x, t.hunted);
            if (!r || !creature::here(x.state, t.hunted, x.room.id)) {
                switch_task(tw, task::search_ground, x.rng);
                return;
            }
            float const num = float(tw.number) / float(tentacle_count - 1) * 2.f - 1.f;
            // connectedChunk.Rotation: the head from chunk 1
            vec2 const rot = rw::normalized(x.pos(0) - x.pos(1));
            vec2 v = rw::slerp(vec2{0.f, -1.f}, rot, 0.75f);
            v = rotate_around_origo(v, num * 20.f);
            for (uint32_t i = 0; i < limb_chunks; ++i) {
                float const num2 = 1.f - float(i) / float(limb_chunks - 1) * 2.f;
                if (num2 < 0.f)
                    break;
                t.c[i].vel += v * num2;
            }
            if (r->visual) {
                move_grab_dest(x, tw, creature::main_pos(x.state, t.hunted), x.scratch);
                return;
            }
            move_grab_dest(x, tw, room_view::middle_of_tile(tracker::best_guess(*r)), x.scratch);
        }

        // LoachTentacle.Update (UpdateTentacle's tip pull down first)
        void tentacle_update(ctx& x, limb_work& tw) {
            auto& w = x.w();
            auto& t = tw.t;
            tip(tw).vel += vec2{0.f, -0.5f};
            base_update(x, tw);
            if (t.current != task::hunt_backup)
                look_for_hunted_creature(x, tw);
            // (no Player.GraspWiggle without Remix: a held slugcat can't wiggle free)
            if (auto const gc = grab_chunk(x, tw)) {
                vec2& gp = w.body_chunk_get_pos(gc);
                float const num = rw::distance(tip(tw).pos, gp);
                float const num2 = (tip(tw).rad + w.body_chunk_get_rad(gc)) / 4.f;
                vec2 const a = custom::dir_vec(tip(tw).pos, gp);
                float const m = w.body_chunk_get_mass(gc);
                float const num3 = m / (m + 0.01f);
                tip(tw).pos += a * (num - num2) * num3;
                tip(tw).vel += a * (num - num2) * num3;
                gp -= a * (num - num2) * (1.f - num3);
                w.body_chunk_get_vel(gc) -= a * (num - num2) * (1.f - num3);
            } else if (t.grab) {
                t.grab = dcon::creature_id{};
            }
            if (x.p.digesting_counter > 0 && t.current != task::digest)
                switch_task(tw, task::digest, x.rng);
            vec2 const head = x.pos(0);
            if (t.current == task::hunt) {
                hunt(x, tw);
            } else if (t.current == task::recede) {
                if (grab_total(x) < 4)
                    ++t.grasp_age;
                auto const gc = grab_chunk(x, tw);
                if (!gc) {
                    switch_task(tw, task::search_ground, x.rng);
                } else {
                    vec2 const gpos = w.body_chunk_get_pos(gc);
                    move_grab_dest(x, tw, head + custom::dir_vec(head, gpos) * 20.f, x.scratch);
                    vec2 p = head;
                    bool flag = x.room.visual_contact(gpos, head);
                    bool released = false;
                    for (int32_t i = int32_t(limb_chunks) - 1; i >= 0; --i) {
                        vec2 p2 = float_base(x, tw);
                        if (i > 0) {
                            p2 = t.c[i - 1].pos;
                            if (!flag && !x.room.visual_contact(gpos, t.c[i - 1].pos)) {
                                p = t.c[i].pos;
                                flag = true;
                            }
                        }
                        t.c[i].vel += custom::dir_vec(t.c[i].pos, p2) * 1.2f;
                        if (t.c[i].phase > -1.f || x.room.get_tile(t.c[i].pos).solid()) {
                            // (Loach_Tentacle_Release has no sample here)
                            switch_task(tw, task::search_ground, x.rng);
                            released = true;
                            break;
                        }
                    }
                    if (!released && t.grasp_age > 120 && x.rng.value() + float(t.grasp_age) / 600.f > 1.2f)
                        switch_task(tw, task::hunt_backup, x.rng);
                    if (t.current == task::recede) {
                        uint32_t const total = std::max(1u, grab_total(x));
                        vec2 const dir = rw::slerp(custom::dir_vec(gpos, p), custom::dir_vec(tip(tw).pos, t.c[limb_chunks - 2].pos), 0.5f);
                        float const m = w.body_chunk_get_mass(gc);
                        w.body_chunk_get_vel(gc) += dir * custom::lerp_map(float(tw.grab_path.size()), 3.f, 18.f, 0.65f, 0.25f) * 0.3f / (m * 1.4f / float(total));
                    }
                }
            }
            if (t.current == task::digest) {
                t.float_grab_dest = head;
                if (x.p.digesting_counter <= 0)
                    switch_task(tw, task::search_ground, x.rng);
            }
            if (t.current == task::hunt_backup) {
                if (t.hunt_backup_check1) {
                    t.float_grab_dest = head;
                    if (custom::dist_less(tip(tw).pos, head, tip(tw).rad + x.rad(0))) {
                        for (auto const& o : x.tents) {
                            auto const* r = rep_of(x, o.t.hunted);
                            if (!o.t.hunted || !r)
                                continue;
                            vec2 const bg = room_view::middle_of_tile(tracker::best_guess(*r));
                            vec2 v = custom::dir_vec(head, bg);
                            v = custom::perpendicular_vector(v) + vec2{float(x.rng.range(-3, 3)), float(x.rng.range(-3, 3))};
                            t.prey_pos = bg;
                            tip(tw).vel += v * 2.f;
                            t.hunt_backup_check1 = 0;
                            t.hunt_backup_check2 = 1;
                            t.bezier_t = 0.f;
                            // (Random.Range(0, 1) == 0: always)
                            t.up = 1;
                            break;
                        }
                        if (!t.hunt_backup_check2)
                            switch_task(tw, task::search_ground, x.rng);
                    }
                }
                if (t.hunt_backup_check2) {
                    vec2 const n = rw::normalized(rotate_around_origo(custom::dir_vec(head, t.prey_pos), mathf::lerp(45.f, 90.f, x.rng.value())));
                    vec2 const s = t.up ? n : -n;
                    t.float_grab_dest = custom::bezier(head, head + s * 60.f, t.prey_pos, t.prey_pos + s * 60.f, t.bezier_t);
                    t.bezier_t += 0.008333334f;
                    if (t.bezier_t > 0.6f) {
                        t.hunt_backup_check1 = 1;
                        t.hunt_backup_check2 = 0;
                        switch_task(tw, task::search_ground, x.rng);
                        look_for_hunted_creature(x, tw);
                    }
                }
            }
            if (t.current == task::search_ground) {
                if (x.is_lurching())
                    switch_task(tw, task::lurch_search, x.rng);
                // (no slime molds: the ground under the head)
                t.float_grab_dest = head + x.vel(0) * mathf::lerp(8.f, 16.f, x.rng.value()) - vec2{0.f, float(x.rng.range(140, 200))};
            }
            if (t.current == task::lurch_search) {
                if (t.lurch_search_update == 0) {
                    t.lurch_search_update = 3;
                    if (!x.is_lurching())
                        switch_task(tw, task::search_ground, x.rng);
                    vec2 b = custom::dir_vec(x.pos(1), x.pos(0)) * mathf::lerp(50.f, 100.f, x.rng.value());
                    b.y = mathf::lerp(-60.f, 120.f, x.rng.value());
                    t.float_grab_dest = head + b;
                } else {
                    --t.lurch_search_update;
                }
            }
            touch(x, tw);
        }

        // Loach.UpdateTentacle
        void update_tentacles(ctx& x) {
            for (auto& tw : x.tents) {
                tentacle_update(x, tw);
                tw.t.limp = x.consious() ? 0 : 1;
            }
        }

        //
        // Loach
        //
        // `t` is gone into the loach (Player.PermaDie, Destroy): everything
        // lets go of it; a creature is gone at the end of the tick
        void finish(ctx& x, dcon::creature_id t) {
            auto& w = x.w();
            for (auto& tw : x.tents)
                if (tw.t.grab == t || tw.t.hunted == t)
                    switch_task(tw, task::search_ground, x.rng);
            if (x.ai.target == t)
                x.ai.target = dcon::creature_id{};
            tracker::forget(x.state, x.c, t);
            creature::release_target(x.state, t, x.room.id);
            rot::release(x.state, t);
            ++dens::stats().prey_taken;
            if (auto const pp = creature::player_of(x.state, t)) {
                items::release_all(x.state, pp);
                creature::kill(x.state, t, creature::tmpl::loach);
                w.player_set_eaten(pp, 1);
                return;
            }
            creature::kill(x.state, t, creature::tmpl::loach);
            ++dens::stats().creatures_eaten;
            x.state.defer(x.room.id, [t](sys::game_state& st) {
                if (st.world.creature_is_valid(t))
                    creature::destroy(st, t);
            });
        }

        // Loach.Eat
        void eat(ctx& x) {
            auto& w = x.w();
            auto& a = x.ai;
            vec2 const pos = x.pos(0);
            // (Loach_Eat has no sample here)
            for (int32_t i = int32_t(a.eat_count) - 1; i >= 0; --i) {
                auto& e = a.eats[i];
                auto const t = e.who;
                if (!creature::here(x.state, t, x.room.id) || e.chunk >= creature::chunk_count_of(x.state, t)) {
                    a.eats[i] = a.eats[--a.eat_count];
                    continue;
                }
                if (e.progression > 1.f) {
                    x.p.digesting_counter = int16_t(custom::lerp_map(creature::body_size(creature::template_of(x.state, t)), 0.2f, 5.f, 50.f, 250.f));
                    a.eats[i] = a.eats[--a.eat_count];
                    for (auto& tw : x.tents)
                        switch_task(tw, task::search_ground, x.rng);
                    finish(x, t);
                    continue;
                }
                float const prog = e.progression;
                e.progression += 0.0125f;
                auto const o = creature::body_of(x.state, t);
                if (prog <= 0.5f && e.progression > 0.5f) {
                    creature::kill(x.state, t, creature::tmpl::loach);
                    w.physical_object_for_each_chunk_connection_as_owner(o, [&](dcon::chunk_connection_id k) {
                        w.chunk_connection_set_type(k, physics::connection_type::pull);
                    });
                }
                float const dd = e.distance * (1.f - e.progression);
                auto const ec = creature::chunk_of(x.state, t, e.chunk);
                w.body_chunk_set_vel(ec, vec2{0.f, 0.f});
                // BodyChunk.MoveFromOutsideMyUpdate
                w.body_chunk_set_pos(ec, pos + custom::dir_vec(pos, w.body_chunk_get_pos(ec)) * dd);
                uint32_t const n = creature::chunk_count_of(x.state, t);
                for (uint32_t k = 0; k < n; ++k) {
                    auto const c = creature::chunk_of(x.state, t, k);
                    w.body_chunk_get_vel(c) *= 1.f - e.progression;
                    vec2 const cp = w.body_chunk_get_pos(c);
                    w.body_chunk_set_pos(c, rw::lerp(cp, pos + custom::dir_vec(pos, cp) * dd, e.progression));
                }
                // (the slugcat's graphics body parts aren't pulled in with it)
            }
        }

        // Loach.Act
        void act(ctx& x, float support_v, float forward_power) {
            auto& p = x.p;
            auto& a = x.ai;
            ai_update(x);
            --a.vocalize_sound_cooldown;
            if (a.vocalize_sound_cooldown < 0 && x.rng.value() < 0.00055555557f)
                // (Loach_Passive_Groan / Aggressive_Groan have no sample here)
                a.vocalize_sound_cooldown = x.rng.range(500, 900);
            update_tentacles(x);
            if (a.random_stumble_counter <= 0)
                a.random_stumble_counter = x.rng.range(200, 550);
            else
                --a.random_stumble_counter;
            // AllowMovementBetweenRooms; near a side of the room otherwise,
            // pushed in (enterRoomForcePush, no Remix)
            ivec2 const here = x.here();
            if (a.time_in_this_room > 100) {
                p.enter_room_force_push = mathf::max(0.f, p.enter_room_force_push - 0.001f);
            } else if (std::abs(here.x) < 10) {
                if (x.room.visual_contact(x.pos(0), x.pos(0) + vec2{400.f, 0.f})) {
                    x.vel(1).x += mathf::pow(p.enter_room_force_push, 2.f) * 2.f;
                    p.enter_room_force_push = mathf::min(2.f, p.enter_room_force_push + 0.0055555557f);
                }
                for (uint32_t i = 0; i < body_chunks; ++i)
                    if (x.contact(i).x > 0)
                        x.vel(i) += custom::deg_to_vec(-90.f * x.rng.value()) * 15.f;
            } else if (std::abs(here.x - (x.room.width - 1)) < 10) {
                if (x.room.visual_contact(x.pos(0), x.pos(0) + vec2{-400.f, 0.f})) {
                    x.vel(1).x -= mathf::pow(p.enter_room_force_push, 2.f) * 2.f;
                    p.enter_room_force_push = mathf::min(2.f, p.enter_room_force_push + 0.0055555557f);
                }
                for (uint32_t i = 0; i < body_chunks; ++i)
                    if (x.contact(i).x < 0)
                        x.vel(i) += custom::deg_to_vec(90.f * x.rng.value()) * 15.f;
            }
            ivec2 const dest = x.destination();
            p.stay_still = (x.room.inside(here.x, here.y) && custom::manhattan_distance(here, dest) < 4) ? 1 : 0;
            // (the resting check never passes in vanilla: it never rests)
            p.resting = mathf::max(0.f, p.resting - 0.00625f);
            p.flip_dir = mathf::sign(x.pos(0).x - x.pos(4).x);
            p.preferred_height = mathf::lerp(float(min_preferred_height), float(mid_ideal_height), p.height_lerp);
            p.height_lerp += 0.0125f;
            p.height_lerp -= 0.008333334f * float(grab_total(x));
            p.height_lerp = mathf::clamp01(p.height_lerp);
            ivec2 const tp = room_view::get_tile_position(x.pos(1));
            p.worm_grass_below = 0;
            for (int32_t k = tp.y; k >= 0; --k) {
                auto const tile = x.room.get_tile(tp.x, k);
                if (tile.flags & world::tile_worm_grass) {
                    p.preferred_height = float(mid_ideal_height);
                    p.worm_grass_below = 1;
                    break;
                }
                if (tile.solid())
                    break;
            }
            // StandardPather.FollowPath, from the head or near the chunks
            ivec2 start = here;
            std::optional<ivec2> step = follow_path(x, start);
            for (int32_t num2 = 1; num2 < 3 && !step; ++num2)
                for (uint32_t num3 = 0; num3 < 5 && !step; ++num3)
                    for (uint32_t num4 = 0; num4 < 5 && !step; ++num4) {
                        start = room_view::get_tile_position(x.pos(num3) + rw::to_vec2(four_directions_and_zero[num4]) * 2.f * float(num2));
                        step = follow_path(x, start);
                    }
            if (step) {
                // as far along the path as it goes in a straight line
                ivec2 dc = *step;
                if (x.room.inside(here.x, here.y))
                    for (int32_t l = 0; l < 8; ++l) {
                        auto const n2 = follow_path(x, dc);
                        if (!n2 || *n2 == start || *n2 == dc || !ray_trace_tiles_for_terrain(x.room, start, *n2))
                            break;
                        dc = *n2;
                    }
                if (std::fabs(room_view::middle_of_tile(dc).x - x.pos(0).x) > 100.f)
                    p.flip_dir = mathf::sign(room_view::middle_of_tile(dc).x - x.pos(0).x);
                if (here.x < 0 || here.x >= x.room.width || dc.x < 0 || dc.x >= x.room.width)
                    p.next_floor_height = float(mid_ideal_height);
                else
                    p.next_floor_height = mathf::lerp(p.next_floor_height, float(custom::int_clamp(x.floor_altitude(dc) + 2, 0, mid_ideal_height + 2)) * 20.f, 0.2f);
                p.move_direction = custom::dir_vec(x.pos(0), room_view::middle_of_tile(dc));
            }
            float const unstuck = x.unstuck_force();
            float const edge = x.close_to_edge();
            auto const support_lift = [&](uint32_t m) {
                return custom::lerp_map(float(x.floor_altitude(room_view::get_tile_position(x.pos(m)))), float(mid_ideal_height), float(mid_ideal_height + 4), 1.f, 0.5f);
            };
            if (!p.lurching) {
                p.lurch_progress = int16_t(std::max(p.lurch_progress - 1, 0));
                float const num5 = mathf::inverse_lerp(75.f, 0.f, float(p.lurch_progress));
                vec2 const md = rw::normalized(p.move_direction);
                for (uint32_t m = 0; m < body_chunks; ++m) {
                    float const num6 = float(m) / 5.f;
                    x.vel(m) *= mathf::lerp(1.f, p.stay_still ? 0.75f : 0.9f, support_v);
                    x.vel(m) *= 1.f - p.resting * 0.5f;
                    if (m < body_chunks - 1)
                        x.vel(m).y += loach_gravity * mathf::lerp(1.3f, 2.5f, std::sin(mathf::pow(num6, 1.7f) * mathf::pi)) * num5 * support_v * 0.825f * support_lift(m);
                    x.vel(m) += md * mathf::lerp(0.65f, 0.15f, num6) * forward_power * mathf::lerp(1.f, 3.f, unstuck) * (m == 4 ? -0.2f : 1.f) * num5;
                }
                x.vel(0).y += loach_gravity * 1.2f * p.resting * num5;
                if (unstuck > 0.f)
                    x.vel(1) += custom::rnv(x.rng) * x.rng.value() * 4.f * unstuck * num5;
            } else {
                vec2 av = custom::dir_vec(x.pos(1), x.pos(0));
                av.y = 0.f;
                auto const lifted = [&](uint32_t m, float num7) {
                    if (m < body_chunks - 1)
                        x.vel(m).y += loach_gravity * mathf::lerp(1.3f, 2.5f, std::sin(mathf::pow(num7, 1.7f) * mathf::pi))
                            * mathf::lerp(support_v * 0.825f * support_lift(m) * custom::lerp_map(p.move_direction.y, -1.f, 1.f, 0.5f, 1.f), 0.65f, edge);
                };
                if (p.lurch_progress < 30) {
                    // drawing back
                    ++p.lurch_progress;
                    for (uint32_t n = 0; n < body_chunks; ++n) {
                        float const num7 = float(n) / 5.f;
                        x.vel(n) *= mathf::lerp(1.f, p.stay_still ? 0.75f : 0.9f, support_v);
                        x.vel(n) *= 1.f - p.resting * 0.5f;
                        lifted(n, num7);
                        x.vel(n) += -av * mathf::lerp(0.65f, 0.15f, num7) * mathf::lerp(1.f, 3.f, unstuck) * (n == 4 ? -0.2f : 1.f);
                    }
                    x.vel(0).y += loach_gravity * 1.2f * p.resting;
                    if (unstuck > 0.f)
                        x.vel(1) += custom::rnv(x.rng) * x.rng.value() * 4.f * unstuck;
                } else if (p.lurch_progress < 60) {
                    // the lunge (Loach_Lunge has no sample here)
                    a.lurch_sound = 0;
                    ++p.lurch_progress;
                    for (uint32_t n = 0; n < body_chunks; ++n) {
                        float const num9 = float(n) / 5.f;
                        x.vel(n) *= mathf::lerp(1.f, p.stay_still ? 0.75f : 0.9f, support_v);
                        x.vel(n) *= 1.f - p.resting * 0.5f;
                        lifted(n, num9);
                        float const num10 = std::fabs(float(p.lurch_progress) - 45.f);
                        float const d = mathf::pow(mathf::max(0.f, (100.f - float(p.lurch_progress) - num10) / 60.f), 2.5f);
                        x.vel(n) += av * mathf::lerp(0.65f, 0.15f, num9) * 3.5f * d * mathf::lerp(1.f, 3.f, unstuck) * (n == 4 ? -0.2f : 1.f) * 2.f;
                    }
                    x.vel(0).y += loach_gravity * 1.2f * p.resting;
                    if (unstuck > 0.f)
                        x.vel(1) += custom::rnv(x.rng) * x.rng.value() * 4.f * unstuck;
                } else if (float(p.lurch_progress) < 75.f) {
                    for (uint32_t n = 0; n < body_chunks; ++n) {
                        float const num12 = float(n) / 3.5f;
                        x.vel(n) *= mathf::lerp(1.f, p.stay_still ? 0.75f : 0.9f, support_v);
                        x.vel(n) *= 1.f - p.resting * 0.5f;
                        // (an integer division in vanilla)
                        float const d2 = mathf::pow(float((105 - int32_t(p.lurch_progress)) / 75), 2.f);
                        x.vel(n) += av * mathf::lerp(0.65f, 0.15f, num12) * num12 * d2 * mathf::lerp(1.f, 3.f, unstuck) * (n == 4 ? -0.2f : 1.f) * 2.f;
                    }
                    ++p.lurch_progress;
                } else {
                    p.lurching = 0;
                }
            }
            if (a.spasm_force != vec2{0.f, 0.f}) {
                x.weighted_push(uint32_t(a.spasm_chunk), 2, a.spasm_force, 1.f);
                a.spasm_force = rw::lerp(a.spasm_force, vec2{0.f, 0.f}, 0.2f);
            }
            if (x.rng.value() < 0.04f) {
                a.spasm_chunk = x.rng.range(0, int32_t(body_chunks));
                // Random.insideUnitCircle
                a.spasm_force = custom::rnv(x.rng) * std::sqrt(x.rng.value()) * 5.f;
            }
        }

        // LoachAI.NewRoom and the parts of Loach.NewRoom that aren't the limbs'
        void ai_new_room(ctx& x) {
            auto& a = x.ai;
            a.time_in_this_room = 0;
            a.idle_counter = 0;
            a.idle_spot_win_streak = 0;
            a.past_count = 0;
            a.stuck_counter = 0;
            a.eat_count = 0;
            a.focus = dcon::creature_id{};
            // (the idle sit spot: a random tile of the room, if reachable)
            ivec2 const t{x.rng.range(0, x.room.width), x.rng.range(0, x.room.height)};
            x.set_destination(x.accessible(t) ? t : x.here());
        }
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 pos, variant v) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::loach);
        auto const d = data(state, c);
        auto rng = rw::random(state.seed, 0, uint32_t(c.value));
        int32_t seed = int32_t(rng.next() & 0x7FFFFFFFu);
        if (state.spawn_seed >= 0)
            seed = state.spawn_seed;
        w.loach_data_set_seed(d, seed);
        // CreatureParams: everything at 1 (sizeMultiplier) and the dominance
        // roll from the ID's seed (GenerateIVars)
        rw::unity_random iv{seed};
        w.loach_data_set_dominance(d, iv.value());
        w.loach_data_set_ai(d, ai_state{});

        physics::object_desc desc;
        desc.air_friction = 0.999f;
        desc.gravity = loach_gravity;
        desc.bounce = 0.1f;
        desc.surface_friction = 0.4f;
        desc.water_friction = 0.96f;
        desc.buoyancy = 0.95f;
        auto const o = physics::create_object(state, room, desc);
        constexpr float rads[body_chunks] = {20.f, 30.f, 40.f, 45.f, 55.f, 50.f};
        constexpr float masses[body_chunks] = {2.f, 3.f, 4.f, 4.5f, 5.5f, 5.f};
        dcon::body_chunk_id cs[body_chunks];
        for (uint32_t i = 0; i < body_chunks; ++i) {
            cs[i] = physics::add_chunk(state, o, pos, rads[i], masses[i]);
            // (CollideWithSlopes off)
            w.body_chunk_set_collide_with_slopes(cs[i], 0);
        }
        physics::add_connection(state, o, cs[0], cs[1], 50.f, physics::connection_type::pull, 0.7f, -1.f);
        physics::add_connection(state, o, cs[1], cs[2], 48.f, physics::connection_type::pull, 0.85f, -1.f);
        physics::add_connection(state, o, cs[2], cs[3], 48.f, physics::connection_type::pull, 1.f, -1.f);
        physics::add_connection(state, o, cs[3], cs[4], 30.f, physics::connection_type::normal, 0.85f, -1.f);
        physics::add_connection(state, o, cs[4], cs[5], 20.f, physics::connection_type::normal, 0.7f, -1.f);
        w.try_create_creature_body(c, o);

        auto const add_limb = [&](creature::part_kind kind, uint32_t k, limb const& t) {
            auto const part = creature::add_part(state, c, kind, uint8_t(k));
            auto const l = w.create_loach_limb();
            w.creature_part_get_payload(part).loach_limb = creature::loach_limb_payload{uint16_t(l.index())};
            w.loach_limb_set_t(l, t);
            w.loach_limb_get_segments(l).resize(0);
            w.loach_limb_get_grab_path(l).resize(0);
        };
        // LoachTentacle(this, bodyChunks[0], Rotted ? 250 : 150, i)
        for (uint32_t i = 0; i < tentacle_count; ++i) {
            limb t;
            t.mouth = 1;
            t.connected_chunk = 0;
            t.ideal_length = v == variant::rot ? rot_tentacle_length : tentacle_length;
            t.goal_attraction_speed_tip = 0.1f;
            t.current = task::search_ground;
            for (uint32_t j = 0; j < limb_chunks; ++j) {
                t.c[j] = limb_chunk{};
                t.c[j].t_pos = float(j + 1) / float(limb_chunks);
                // (Lerp(rootRad, tipRad, j / 6 + 1): the tip's radius for all)
                t.c[j].rad = mathf::lerp(root_rad, tip_rad, float(j) / float(limb_chunks) + 1.f);
            }
            add_limb(creature::part_kind::loach_tentacle, i, t);
        }
        // LoachLeg(this, bodyChunks[j % 2 + 1], firstChunk, 200, j)
        for (uint32_t k = 0; k < leg_count; ++k) {
            limb t;
            t.mouth = 0;
            t.ideal_length = leg_length;
            t.connected_chunk = uint8_t(k % 2 + 1);
            t.tentacle_dir = custom::deg_to_vec(float(k) * 60.f + 30.f);
            t.goal_attraction_speed_tip = 0.75f;
            for (uint32_t j = 0; j < limb_chunks; ++j) {
                t.c[j] = limb_chunk{};
                t.c[j].t_pos = float(j + 1) / float(limb_chunks);
                t.c[j].rad = 4.f;
            }
            add_limb(creature::part_kind::loach_leg, k, t);
        }

        auto& p = pl(state, c);
        p = creature::loach_payload{};
        p.data = uint16_t(d.index());
        p.variant = v;
        p.flip_dir = rng.value() < 0.5f ? -1.f : 1.f;
        p.bod_dir = vec2{p.flip_dir, 0.f};
        p.move_direction = vec2{0.f, 0.f};
        p.preferred_height = float(max_preferred_height);
        p.height_lerp = 1.f;
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_set_grasps(c, creature::grasp_pair{});
        w.creature_set_behavior(c, uint8_t(behavior::idle));
        w.creature_set_destination(c, room_view::get_tile_position(pos));
        // abstractCreature.tentacleImmune
        w.creature_set_spawn_flags(c, uint8_t(w.creature_get_spawn_flags(c) | dens::flag_tentacle_immune));
        loach_graphics::create(state, c);
        place_in_room(state, c, room, pos);
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        creature::release_target(state, c);
        creature::erase(state, c);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        ctx x(state, c, room);
        auto& p = x.p;
        auto& a = x.ai;
        load(x);
        if (x.legs.size() < leg_count || x.tents.size() < tentacle_count) {
            store(x);
            return;
        }
        // Creature.Update + PhysicalObject.Update
        creature::stun_countdown(state, c);
        physics::update_object(state, room, creature::body(state, c), nullptr, nullptr);
        bool const cons = x.consious();
        // LoachState's interest in the prey it was given
        if (x.chasing() && !x.is_lurching()) {
            creature::tracked const* prey[creature::max_tracked];
            if (tracked_prey(x, prey) > 0) {
                auto const* most = tracker::prey(tracker::of(state, c));
                if (most && most->visual) {
                    update_interest(x, 1);
                    if (a.interest >= 350 && x.rng.value() > 0.999f)
                        start_lurch(x);
                } else if (update_interest(x, -1)) {
                    // ForgetAllPrey
                    a.target = dcon::creature_id{};
                }
            }
        }
        vec2 const flipv{p.flip_dir, 0.f};
        float const sl = cons ? 0.5f : 0.f;
        x.weighted_push(0, 1, rw::slerp(p.bod_dir, flipv, sl), 1.f);
        x.weighted_push(1, 2, rw::slerp(p.bod_dir, flipv, sl), 0.65f);
        x.weighted_push(2, 3, rw::slerp(p.bod_dir, vec2{p.flip_dir, -0.2f}, sl), 0.5f);
        x.weighted_push(3, 4, rw::slerp(p.bod_dir, flipv, sl), 0.5f);
        x.weighted_push(0, 1, flipv, 0.85f);
        x.weighted_push(0, 1, p.bod_dir, 0.6f);
        for (uint32_t i = 0; i < 4; ++i)
            x.weighted_push(i, 5, cons ? p.bod_dir * 0.4f : vec2{0.f, 0.f}, 1.1f - float(i) * 0.3f);
        x.weighted_push(4, 2, vec2{0.f, 1.f}, p.sac_lift_factor * 2.f);
        x.weighted_push(5, 2, vec2{0.f, 1.f}, p.sac_lift_factor * 2.f);
        if (mathf::sign(p.bod_dir.x) == mathf::sign(p.flip_dir)) {
            x.weighted_push(0, 2, vec2{0.f, -1.f}, (1.f - p.head_lift_factor) * 1.5f);
            x.weighted_push(1, 2, vec2{0.f, -1.f}, (1.f - p.head_lift_factor) * 0.5f);
        }
        ivec2 const c4 = x.contact(4), c5 = x.contact(5);
        if ((c4 != ivec2{0, 0} && c4.y < 1) || (c5 != ivec2{0, 0} && c5.y < 1))
            p.sac_lift_factor = mathf::min(p.sac_lift_factor + 0.1f, 1.f);
        else
            p.sac_lift_factor = custom::lerp_and_tick(p.sac_lift_factor, 0.f, 0.02f, 0.01f);
        bool lifted = false;
        if (behavior(state.world.creature_get_behavior(c)) == behavior::hunt && a.focus) {
            if (auto const* r = rep_of(x, a.focus)) {
                vec2 const b = r->visual && creature::live(state, a.focus) ? creature::main_pos(state, a.focus) : room_view::middle_of_tile(tracker::best_guess(*r));
                vec2 const v = x.pos(0) - b;
                float const b2 = mathf::inverse_lerp(-20.f, 100.f, v.y / mathf::max(std::fabs(v.x) / 100.f, 1.f));
                p.head_lift_factor = mathf::lerp(p.head_lift_factor, b2, 0.05f);
                lifted = true;
            }
        }
        if (!lifted)
            p.head_lift_factor = mathf::lerp(p.head_lift_factor, 0.2f, 0.05f);
        // the legs
        p.held_back_by_leg = 0;
        p.legs_grabbing = 0;
        float num = 0.f;
        float best = -float_max;
        limb_work* release = nullptr;
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
        if (!p.stay_still && p.legs_grabbing > 5 && release)
            release_grip(x, *release);
        num = mathf::pow(mathf::min(num / 3.f, 1.f), 0.8f) * mathf::pow(1.f - p.resting, 0.3f);
        for (uint32_t k = 0; k < 5; ++k)
            if (x.contact(k).x != 0 || x.contact(k).y < 0)
                num = mathf::lerp(num, 1.f, 0.6f);
        float const edge = x.close_to_edge();
        num = mathf::lerp(num, mathf::lerp(0.05f, 0.085f, x.rng.value()), edge);
        num = mathf::pow(num, 0.1f);
        num *= mathf::lerp(0.85f, 1.f, x.rng.value());
        float num3 = mathf::lerp(mathf::pow(float(std::max(int32_t(p.legs_grabbing) - 2, 0)) / 4.f, 0.8f), num, 0.5f);
        bool flag2 = false;
        for (auto& lw : x.legs) {
            float const dx = tip(lw).pos.x - float_base(x, lw).x;
            if (lw.t.attached_at_tip && ((dx > 0.f) == (p.move_direction.x > 0.f) || std::fabs(dx) < leg_in_front_margin)) {
                flag2 = true;
                break;
            }
        }
        if (!flag2) {
            if (!p.stay_still)
                num3 = mathf::lerp(custom::lerp_map(float(p.hesist_counter), 20.f, 150.f, -0.5f, 1.f), num3, edge);
            if (p.hesist_counter < 30000)
                ++p.hesist_counter;
        } else {
            p.hesist_counter = 0;
        }
        num3 = mathf::lerp(num3, 1.f, x.unstuck_force());
        p.bod_dir = vec2{0.f, 0.f};
        if (cons)
            act(x, num, num3);
        for (uint32_t l = 0; l < 3; ++l) {
            x.weighted_push(l, l + 2, -custom::dir_vec(x.pos(l), x.pos(l + 2)), 0.45f);
            p.bod_dir += custom::dir_vec(x.pos(l + 2), x.pos(l + 1));
        }
        if (cons)
            p.bod_dir += vec2{0.8f * p.flip_dir, 0.f};
        p.bod_dir = rw::normalized(p.bod_dir);
        if (p.digesting_counter > 0)
            --p.digesting_counter;
        eat(x);
        store(x);
    }

    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) {
        // what it swallows is in collision layer 0 (ChangeCollisionLayer)
        auto const& a = state.world.loach_data_get_ai(data(state, c));
        for (uint32_t i = 0; i < a.eat_count; ++i)
            if (a.eats[i].who == t)
                return false;
        return true;
    }

    void collide(sys::game_state& state, dcon::creature_id c, room_view const& room, uint32_t, dcon::creature_id t, uint32_t j) {
        // Loach.Collide (stunOnCollideVelocity is 0: no ramming)
        if (!creature::consious(state, c) || !t || !creature::here(state, t, room.id) || state.world.creature_get_type(t) == creature::type::loach)
            return;
        if (!tracker::find(state, c, t))
            return;
        auto& w = state.world;
        auto& a = w.loach_data_get_ai(data(state, c));
        // one of its tentacles holds it
        bool flag = false;
        for (auto const o : creature::parts(w, c)) {
            auto const part = w.part_ownership_get_part(o);
            if (w.creature_part_get_kind(part) == creature::part_kind::loach_tentacle && w.loach_limb_get_t(limb_data(w, part)).grab == t)
                flag = true;
        }
        for (uint32_t i = 0; i < a.eat_count && flag; ++i)
            if (a.eats[i].who == t)
                flag = false;
        if (!flag || a.eat_count >= max_eat_objects || j >= creature::chunk_count_of(state, t))
            return;
        vec2 const head = w.body_chunk_get_pos(creature::chunk(state, c, 0));
        auto& e = a.eats[a.eat_count++];
        e = eat_object{};
        e.who = t;
        e.chunk = uint8_t(j);
        e.distance = rw::distance(head, w.body_chunk_get_pos(creature::chunk_of(state, t, j)));
        e.progression = 0.f;
    }

    void violence(sys::game_state& state, dcon::creature_id c, uint32_t k, vec2 momentum, float damage, float stun_bonus, creature::damage_type) {
        auto& w = state.world;
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        auto& a = w.loach_data_get_ai(data(state, c));
        // Loach.Violence: a groan (Loach_*_Groan have no sample here)
        if (!w.creature_get_dead(c) && a.vocalize_sound_cooldown <= 0)
            a.vocalize_sound_cooldown = rng.range(500, 900);
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

    void weapon_hit(sys::game_state& state, dcon::creature_id c, dcon::creature_id thrower) {
        auto& w = state.world;
        if (!thrower || !w.creature_is_valid(thrower) || thrower == c || w.creature_get_dead(c))
            return;
        auto const room = creature::room_of(state, c);
        if (!room || !creature::here(state, thrower, room))
            return;
        // the weapon's thrower, if it isn't its prey already, may become it
        // (preyTracker.MostAttractivePrey != thrower && SetNewTarget(0))
        auto const view = world::get_room_view(state, room);
        ctx x(state, c, view);
        auto const* most = tracker::prey(tracker::of(state, c));
        if (most && most->who == thrower)
            return;
        if (set_new_target(x, 0.f))
            give_prey(x, thrower);
    }

    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t) {
        auto& w = state.world;
        for (auto const o : creature::parts(w, c)) {
            auto const part = w.part_ownership_get_part(o);
            if (w.creature_part_get_kind(part) != creature::part_kind::loach_tentacle)
                continue;
            auto& lt = w.loach_limb_get_t(limb_data(w, part));
            if (lt.grab == t) {
                lt.grab = dcon::creature_id{};
                lt.grasp_age = 0;
                lt.current = task::search_ground;
                lt.goal_attraction_speed_tip = 0.1f;
            }
            if (lt.hunted == t) {
                lt.hunted = dcon::creature_id{};
                lt.current = task::search_ground;
                lt.goal_attraction_speed_tip = 0.1f;
            }
        }
        auto& a = w.loach_data_get_ai(data(state, c));
        for (uint32_t i = 0; i < a.eat_count;)
            if (a.eats[i].who == t)
                a.eats[i] = a.eats[--a.eat_count];
            else
                ++i;
        if (a.target == t)
            a.target = dcon::creature_id{};
        if (a.focus == t)
            a.focus = dcon::creature_id{};
    }

    void died(sys::game_state&, dcon::creature_id) {
        // (Loach.Die is Creature.Die's; the legs go limp in their update,
        // the tentacles stop with the Act)
    }

    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const view = world::get_room_view(state, room);
        vec2 const mid = room_view::middle_of_tile(at);
        uint32_t const n = creature::chunk_count(state, c);
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        for (uint32_t i = 0; i < n; ++i) {
            auto const ch = creature::chunk(state, c, i);
            physics::hard_set_position(state, ch, mid + custom::rnv(rng));
            w.body_chunk_set_vel(ch, vec2{0.f, 0.f});
        }
        auto& p = pl(state, c);
        // Loach.NewRoom
        p.preferred_height = float(max_preferred_height);
        p.enter_room_force_push = 0.f;
        p.lurching = 0;
        p.lurch_progress = 0;
        ctx x(state, c, view);
        load(x);
        for (auto& tw : x.tents) {
            limb_new_room(x, tw);
            switch_task(tw, task::search_ground, x.rng);
        }
        for (auto& lw : x.legs)
            leg_new_room(x, lw);
        ai_new_room(x);
        store(x);
        loach_graphics::reset_positions(state, c);
    }

    std::string describe(sys::game_state const& state, dcon::creature_id c) {
        static char const* const names[] = {"idle", "travelling", "flee", "hunt", "return prey", "investigate sound", "escape rain", "get unstuck"};
        auto const& w = state.world;
        auto const b = uint32_t(w.creature_get_behavior(c));
        auto const& p = pl(state, c);
        auto const& a = w.loach_data_get_ai(data(state, c));
        std::string s = p.variant == variant::rot ? "rot loach, " : "loach, ";
        s += b < 8 ? names[b] : "?";
        s += " legs " + std::to_string(int(p.legs_grabbing)) + "/6";
        uint32_t holding = 0;
        for (auto const o : creature::parts(w, c)) {
            auto const part = w.part_ownership_get_part(o);
            if (w.creature_part_get_kind(part) == creature::part_kind::loach_tentacle && w.loach_limb_get_t(limb_data(w, part)).grab)
                ++holding;
        }
        if (holding)
            s += ", " + std::to_string(holding) + " tentacles holding";
        if (a.eat_count)
            s += ", swallowing";
        if (p.digesting_counter > 0)
            s += ", digesting";
        if (a.interest > 0)
            s += ", interest " + std::to_string(a.interest);
        if (p.lurching)
            s += ", lunging";
        return s;
    }

    bool spawn_spot(room_view const& room, ivec2 tile) {
        // on a floor, open space above for the body, away from the sides
        if (room.get_tile(tile).solid() || !room.get_tile(tile + ivec2{0, -1}).solid())
            return false;
        for (int32_t dy = 0; dy < 10; ++dy)
            for (int32_t dx = -4; dx <= 4; ++dx)
                if (room.get_tile(tile + ivec2{dx, dy}).solid())
                    return false;
        return tile.x > 10 && tile.x < room.width - 11;
    }

    bool rotted(sys::game_state const& state, dcon::creature_id c) noexcept { return pl(state, c).variant == variant::rot; }

    float most_digested(sys::game_state const& state, dcon::creature_id c) noexcept {
        auto const& a = state.world.loach_data_get_ai(data(state, c));
        float m = 0.f;
        for (uint32_t i = 0; i < a.eat_count; ++i)
            m = mathf::max(m, a.eats[i].progression);
        return m;
    }

    dcon::creature_id focus(sys::game_state const& state, dcon::creature_id c) noexcept { return state.world.loach_data_get_ai(data(state, c)).focus; }

    bool holds(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) noexcept {
        auto const& w = state.world;
        for (auto const o : creature::parts(w, c)) {
            auto const part = w.part_ownership_get_part(o);
            if (w.creature_part_get_kind(part) == creature::part_kind::loach_tentacle && w.loach_limb_get_t(limb_data(w, part)).grab == t)
                return true;
        }
        return false;
    }

    bool eating(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) noexcept {
        auto const& a = state.world.loach_data_get_ai(data(state, c));
        for (uint32_t i = 0; i < a.eat_count; ++i)
            if (a.eats[i].who == t)
                return true;
        return false;
    }
}
