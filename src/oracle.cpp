// Oracle.cs, Oracle.OracleArm and the parts of SSOracleBehavior,
// SLOracleBehavior, CLOracleBehavior and SSOracleRotBehavior that move an
// iterator about its chamber on its own. See oracle.hpp.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <string_view>

#include "oracle.hpp"
#include "oracle_graphics.hpp"
#include "physics.hpp"
#include "player_types.hpp"
#include "rooms.hpp"
#include "vulture_internal.hpp"

namespace oracle {
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // bodyChunkConnections[0].distance
        constexpr float connection_distance = 9.f;

        bool same_name(std::string_view a, std::string_view b) {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (std::toupper(uint8_t(a[i])) != std::toupper(uint8_t(b[i])))
                    return false;
            return true;
        }

        body_state& bs(sys::game_state& state, dcon::creature_id c) { return state.world.oracle_data_get_body(data(state, c)); }
        arm_state& as(sys::game_state& state, dcon::creature_id c) { return state.world.oracle_data_get_arm(data(state, c)); }

        // Mathf.DeltaAngle, MoveTowardsAngle
        float delta_angle(float current, float target) {
            float t = target - current;
            t = mathf::clamp(t - std::floor(t / 360.f) * 360.f, 0.f, 360.f);
            if (t > 180.f)
                t -= 360.f;
            return t;
        }
        float move_towards_angle(float current, float target, float max_delta) {
            float const num = delta_angle(current, target);
            if (-max_delta < num && num < max_delta)
                return target;
            target = current + num;
            if (std::fabs(target - current) <= max_delta)
                return target;
            return current + mathf::sign(target - current) * max_delta;
        }

        // SetUpMarbles (SS, DM: vector (200, 100)): where each marble hangs,
        // and whether it orbits the oracle (Idle skips those)
        struct marble {
            vec2 pos;
            bool orbits_oracle;
        };
        struct marble_table {
            marble m[marble_count];
        };
        marble_table const& marbles() {
            static marble_table const t = [] {
                marble_table r{};
                uint32_t n = 0;
                vec2 const v{200.f, 100.f};
                auto add = [&](vec2 p, bool orb) { r.m[n++] = marble{p, orb}; };
                for (int i = 0; i < 6; ++i)
                    add(vec2{v.x + 300.f, v.y + 200.f}, true);
                for (int j = 0; j < 2; ++j)
                    add(vec2{v.x + 300.f, v.y + 200.f}, true);
                add(vec2{v.x + 20.f, v.y + 200.f}, false);
                vec2 const a{v.x + 80.f, v.y + 30.f};
                vec2 const d = custom::deg_to_vec(-32.7346f);
                vec2 const a2 = custom::perpendicular_vector(d);
                for (int k = 0; k < 3; ++k)
                    for (int l = 0; l < 5; ++l)
                        if (k != 2 || l != 2)
                            add(a + a2 * float(k) * 17.f + d * float(l) * 17.f, false);
                add(vec2{v.x + 487.f, v.y + 218.f}, false);
                // (orbiting the one before: taken at its place)
                add(vec2{v.x + 487.f, v.y + 218.f}, false);
                add(vec2{v.x + 450.f, v.y + 467.f}, false);
                add(vec2{v.x + 450.f, v.y + 467.f}, false);
                add(vec2{v.x + 450.f, v.y + 467.f}, false);
                add(vec2{v.x + 117.f, v.y}, false);
                add(vec2{v.x + 547.f, v.y + 374.f}, false);
                add(vec2{v.x + 114.f, v.y + 500.f}, false);
                add(vec2{v.x + 108.f, v.y + 511.f}, false);
                add(vec2{v.x + 551.f, v.y + 131.f}, false);
                add(vec2{v.x + 560.f, v.y + 124.f}, false);
                add(vec2{v.x + 520.f, v.y + 134.f}, false);
                add(vec2{v.x + 109.f, v.y + 352.f}, false);
                add(vec2{v.x + 109.f, v.y + 352.f}, false);
                add(vec2{v.x + 109.f, v.y + 352.f}, false);
                return r;
            }();
            return t;
        }

        struct ctx {
            sys::game_state& state;
            dcon::creature_id c;
            room_view const& room;
            body_state& b;
            arm_state& a;
            id which;
            rw::rng& rng;

            dcon::body_chunk_id chunk(uint32_t i) const { return creature::chunk(state, c, i); }
            vec2& pos(uint32_t i) { return state.world.body_chunk_get_pos(chunk(i)); }
            vec2& vel(uint32_t i) { return state.world.body_chunk_get_vel(chunk(i)); }
            vec2 last(uint32_t i) const { return state.world.body_chunk_get_last_pos(chunk(i)); }
            ivec2 contact(uint32_t i) const { return state.world.body_chunk_get_contact_point(chunk(i)); }
            float& g() { return state.world.physical_object_get_g(creature::body(state, c)); }
            bool solid(vec2 p) const { return room.get_tile(p).solid(); }
            // PhysicalObject.WeightedPush
            void weighted_push(uint32_t i, uint32_t j, vec2 dir, float frc) {
                float const mi = state.world.body_chunk_get_mass(chunk(i)), mj = state.world.body_chunk_get_mass(chunk(j));
                float const num = mj / (mi + mj);
                vel(i) += dir * frc * num;
                vel(j) -= dir * frc * (1.f - num);
            }
        };

        // Oracle.Alive / Consious from the state alone
        bool alive(body_state const& b) { return b.health > 0.f; }
        bool awake(body_state const& b) { return alive(b) && b.stun < 1; }

        bool in_sit(id which, body_state const& b, vec2 p0, room_view const& room) {
            ivec2 const t = room_view::get_tile_position(p0);
            switch (which) {
            case id::sl: return t.x == 79 && !b.moon_active;
            case id::cl: return t.x == 128;
            case id::rm: return t.x > 72 && t.x < 75 && room.get_tile(vec2{p0.x, p0.y - 20.f}).solid();
            default: return false;
            }
        }

        // ClampVectorInRoom: SS's 10 px, SL's 100 px inside the arm's frame
        vec2 clamp_in_room(arm_state const& a, vec2 v, float margin) {
            v.x = mathf::clamp(v.x, a.corners[0].x + margin, a.corners[1].x - margin);
            v.y = mathf::clamp(v.y, a.corners[2].y + margin, a.corners[1].y - margin);
            return v;
        }

        vec2 get_to_pos_of(id which, body_state const& b, arm_state const& a, vec2 p0) {
            switch (which) {
            case id::ss:
            case id::dm: {
                vec2 v = b.current_get_to;
                if (b.floaty && custom::dist_less(p0, b.next_pos, 50.f))
                    v = b.next_pos;
                return clamp_in_room(a, v, 10.f);
            }
            case id::sl:
                if (b.moon_active) {
                    vec2 v = b.current_get_to;
                    if (b.floaty && custom::dist_less(p0, b.next_pos, 50.f))
                        v = b.next_pos;
                    return clamp_in_room(a, v, 100.f);
                }
                return b.moon_awake ? vec2{1585.f, 160.f} : vec2{1585.f, 200.f};
            case id::cl: return vec2{2565.f, 145.f};
            case id::rm: return vec2{1493.f, 784.f};
            }
            return p0;
        }

        vec2 get_to_dir_of(id which, body_state const& b, arm_state const& a, vec2 p0, room_view const& room) {
            switch (which) {
            case id::ss:
            case id::dm: return b.move == movement::idle ? custom::deg_to_vec(b.investigate_angle) : vec2{0.f, 1.f};
            case id::sl:
                if (!b.moon_active) {
                    if (in_sit(which, b, p0, room))
                        return vec2{0.f, 1.f};
                    return custom::dir_vec(p0, get_to_pos_of(which, b, a, p0));
                }
                return b.move == movement::idle ? custom::deg_to_vec(b.investigate_angle) : vec2{0.f, 1.f};
            case id::cl:
                if (in_sit(which, b, p0, room))
                    return vec2{0.f, 1.f};
                return custom::dir_vec(get_to_pos_of(which, b, a, p0), p0);
            case id::rm:
                if (in_sit(which, b, p0, room))
                    return vec2{0.f, 1.f};
                return custom::dir_vec(p0, get_to_pos_of(which, b, a, p0));
            }
            return vec2{0.f, 1.f};
        }

        vec2 get_to_pos_x(ctx& x) { return get_to_pos_of(x.which, x.b, x.a, x.pos(0)); }
        vec2 get_to_dir_x(ctx& x) { return get_to_dir_of(x.which, x.b, x.a, x.pos(0), x.room); }
        bool in_sit_x(ctx& x) { return in_sit(x.which, x.b, x.pos(0), x.room); }

        // SetNewDestination (SS's and SL's are the same)
        void set_new_destination(ctx& x, vec2 dst) {
            auto& b = x.b;
            b.last_pos = b.current_get_to;
            b.next_pos = dst;
            float const d = rw::distance(b.last_pos, b.next_pos);
            vec2 const r = custom::rnv(x.rng);
            b.last_pos_handle = r * mathf::lerp(0.3f, 0.65f, x.rng.value()) * d;
            b.next_pos_handle = -get_to_dir_x(x) * mathf::lerp(0.3f, 0.65f, x.rng.value()) * d;
            b.path_progression = 0.f;
        }

        // the path's progression and the point on it (SS Update, SL UpdateActive)
        void follow_path(ctx& x, float margin) {
            auto& b = x.b;
            b.path_progression = mathf::min(1.f,
                b.path_progression + 1.f / mathf::lerp(40.f + b.path_progression * 80.f, rw::distance(b.last_pos, b.next_pos) / 5.f, 0.5f));
            b.current_get_to = custom::bezier(b.last_pos, clamp_in_room(x.a, b.last_pos + b.last_pos_handle, margin), b.next_pos,
                clamp_in_room(x.a, b.next_pos + b.next_pos_handle, margin), b.path_progression);
            b.floaty = 0;
            b.investigate_angle += b.invst_ang_speed;
        }

        // the arm's base looks for a better place (end of Move)
        template<typename Score>
        void base_ideal_roll(ctx& x, Score&& score) {
            auto& b = x.b;
            ++b.consistent_base_pos_counter;
            float const vx = x.rng.value() * x.room.pixel_width();
            float const vy = x.rng.value() * x.room.pixel_height();
            vec2 const v{vx, vy};
            if (!x.solid(v) && score(v) + 40.f < score(b.base_ideal)) {
                b.base_ideal = v;
                b.consistent_base_pos_counter = 0;
            }
        }

        //
        // SSOracleBehavior (SS, DM), General_Idle with no slugcat
        //
        void ss_move(ctx& x) {
            auto& b = x.b;
            vec2 const p0 = x.pos(0);
            if (b.move == movement::idle) {
                b.invst_ang_speed = 1.f;
                auto const& ms = marbles().m;
                if (b.investigate_marble < 0)
                    b.investigate_marble = x.rng.range(0, int32_t(marble_count));
                if (b.investigate_marble >= 0) {
                    marble const& m = ms[b.investigate_marble];
                    if (m.orbits_oracle || custom::dist_less(vec2{250.f, 150.f}, m.pos, 100.f))
                        b.investigate_marble = -1;
                }
                if (b.investigate_marble >= 0) {
                    vec2 const mp = ms[b.investigate_marble].pos;
                    b.look_point = mp;
                    if (custom::dist_less(b.next_pos, mp, 100.f)) {
                        b.floaty = 1;
                        b.next_pos = mp - custom::deg_to_vec(b.investigate_angle) * 50.f;
                    } else {
                        set_new_destination(x, mp - custom::deg_to_vec(b.investigate_angle) * 50.f);
                    }
                    if (b.path_progression == 1.f && x.rng.value() < 0.005f)
                        b.investigate_marble = -1;
                }
                if (x.which == id::dm && x.rng.value() < 0.001f)
                    b.move = movement::meditate;
            } else if (b.move == movement::meditate) {
                vec2 const spot = room_view::middle_of_tile(24, 17);
                if (b.next_pos != spot)
                    set_new_destination(x, spot);
                b.investigate_angle = 0.f;
                b.look_point = p0 + vec2{0.f, -40.f};
                // (Remix)
                if (x.rng.value() < 0.001f)
                    b.move = movement::idle;
            }
            // BasePosScore with no slugcat: close to the bottom middle
            vec2 const bottom = room_view::middle_of_tile(24, 5);
            base_ideal_roll(x, [&](vec2 v) { return rw::distance(v, bottom); });
        }

        void ss_update(ctx& x) {
            auto& b = x.b;
            follow_path(x, 10.f);
            if (b.path_progression >= 1.f && b.consistent_base_pos_counter > 100 && !x.a.base_moving)
                ++b.all_still_counter;
            else
                b.all_still_counter = 0;
            // (General_Idle keeps Idle or Meditate)
            if (b.move != movement::idle && b.move != movement::meditate)
                b.move = movement::idle;
            ss_move(x);
            if (b.working != b.get_to_working)
                b.working = custom::lerp_and_tick(b.working, b.get_to_working, 0.05f, 0.033333335f);
        }

        //
        // SLOracleBehavior
        //
        vec2 random_room_point(ctx& x) {
            float const px = 1270.f + x.rng.value() * 490.f;
            float const py = 200.f + x.rng.value() * 350.f;
            return vec2{px, py};
        }

        void sl_move(ctx& x) {
            auto& b = x.b;
            vec2 const p0 = x.pos(0);
            if (b.move == movement::idle) {
                b.invst_ang_speed = 0.2f;
                b.idle_counter -= 1.f;
                if (b.idle_counter <= 0.f) {
                    if (x.rng.value() >= 0.4f) {
                        set_new_destination(x, random_room_point(x));
                        b.investigate_angle = x.rng.value() * 360.f - 180.f;
                        b.idle_counter = x.rng.value() * 400.f + 150.f;
                    } else {
                        b.idle_counter = x.rng.value() * 900.f + 450.f;
                        b.move = x.rng.value() >= 0.3f ? movement::meditate : movement::show_media;
                    }
                }
            } else if (b.move == movement::meditate) {
                vec2 const spot = room_view::middle_of_tile(77, 18);
                if (b.next_pos != spot)
                    set_new_destination(x, spot);
                b.investigate_angle = 0.f;
                b.look_point = p0 + vec2{0.f, -40.f};
                b.idle_counter -= 1.f;
                ++b.meditate_tick;
                // (the swarmers' circling)
                if (b.idle_counter <= 0.f) {
                    set_new_destination(x, random_room_point(x));
                    b.investigate_angle = x.rng.value() * 360.f - 180.f;
                    b.idle_counter = x.rng.value() * 400.f + 150.f;
                    b.move = movement::idle;
                }
            } else if (b.move == movement::show_media) {
                ++b.show_media_phase_time;
                b.investigate_angle = 0.f;
                b.look_point = p0 + vec2{0.f, 40.f};
                if (b.show_media_phase_time == 1)
                    set_new_destination(x, random_room_point(x));
                // (no projected image: done once it would have shown)
                if (b.show_media_phase_time > 10) {
                    if (b.show_media_phase_time % 120 == 0) {
                        set_new_destination(x, random_room_point(x));
                        b.investigate_angle = x.rng.value() * 360.f - 180.f;
                    }
                    b.move = movement::idle;
                }
            }
            vec2 const spot = room_view::middle_of_tile(77, 18);
            base_ideal_roll(x, [&](vec2 v) { return rw::distance(v, spot); });
        }

        void sl_update_active(ctx& x) {
            auto& b = x.b;
            follow_path(x, 100.f);
            b.hold_knees = 0;
            if (b.path_progression < 1.f || b.consistent_base_pos_counter <= 100 || x.a.base_moving)
                b.all_still_counter = 0;
            else
                ++b.all_still_counter;
            if (b.move != movement::show_media)
                b.show_media_phase_time = 0;
            sl_move(x);
        }

        // the sitting ones' common tail: joint 2 kept over the floor, the
        // body pushed upright as it gets close to its spot
        void sit_tail(ctx& x) {
            auto& j2 = x.a.joints[2];
            if (j2.pos.y < 140.f) {
                j2.pos.y = 140.f;
                j2.vel.y = std::fabs(x.a.joints[1].vel.y) * 0.2f;
            }
            x.weighted_push(0, 1, vec2{0.f, 1.f}, 4.f * mathf::inverse_lerp(60.f, 20.f, std::fabs(get_to_pos_x(x).x - x.pos(0).x)));
        }

        void damp_joints_when_sitting(ctx& x) {
            if (!in_sit_x(x))
                return;
            for (auto& j : x.a.joints)
                if (rw::magnitude(j.vel) > 0.05f)
                    j.vel *= 0.98f;
        }

        void sl_update(ctx& x) {
            auto& b = x.b;
            if (b.dont_hold_knees > 0)
                --b.dont_hold_knees;
            damp_joints_when_sitting(x);
            b.moon_active = b.moon_awake;
            // (no slugcat: never in the sit zone)
            ++b.time_out_of_sit_zone;
            if (b.move == movement::show_media && b.moon_awake) {
                b.moon_active = 1;
                x.g() = mathf::lerp(x.g(), 1.f, 0.2f);
            }
            // not noticing anyone
            b.look_point = in_sit_x(x) ? x.pos(0) + vec2{-145.f, -45.f} : get_to_pos_x(x);
            if (b.moon_active) {
                sl_update_active(x);
                x.a.is_active = 1;
                return;
            }
            if (in_sit_x(x)) {
                if (b.dont_hold_knees < 1 && x.rng.value() < 0.025f && b.health >= 1.f)
                    b.hold_knees = 1;
            } else {
                x.vel(0).x += (x.pos(0).x < get_to_pos_x(x).x ? 1.f : -1.f) * 0.6f * crawl_speed(b);
                b.hold_knees = 0;
            }
            sit_tail(x);
            x.a.is_active = 0;
        }

        //
        // CLOracleBehavior (UpdateNormal)
        //
        void cl_update(ctx& x) {
            auto& b = x.b;
            if (b.dont_hold_knees > 0)
                --b.dont_hold_knees;
            damp_joints_when_sitting(x);
            ++b.time_out_of_sit_zone;
            b.look_point = in_sit_x(x) ? x.pos(0) + vec2{-145.f, -45.f} : get_to_pos_x(x);
            if (in_sit_x(x)) {
                b.hold_knees = 1;
            } else {
                b.hold_knees = 0;
                x.vel(0).x += (x.pos(0).x >= get_to_pos_x(x).x ? -1.f : 1.f) * 0.6f * crawl_speed(b);
            }
            sit_tail(x);
            x.a.is_active = 0;
        }

        //
        // SSOracleRotBehavior
        //
        void rm_update(ctx& x) {
            auto& b = x.b;
            b.health = 5.f;
            if (b.dont_hold_knees > 0)
                --b.dont_hold_knees;
            b.look_point = in_sit_x(x) ? x.pos(0) + vec2{145.f, -45.f} : get_to_pos_x(x);
            if (in_sit_x(x)) {
                if (b.dont_hold_knees < 1 && x.rng.value() < 0.025f && b.health >= 1.f)
                    b.hold_knees = 1;
            } else {
                x.vel(0).x += (x.pos(0).x >= get_to_pos_x(x).x ? -1.f : 1.f) * 0.6f * crawl_speed(b);
                if (x.contact(0).x != 0)
                    x.vel(0).y = mathf::lerp(x.vel(0).y, 1.2f, 0.5f) + 1.2f;
                if (x.contact(1).x != 0)
                    x.vel(0).y = mathf::lerp(x.vel(0).y, 1.2f, 0.5f) + 1.2f;
                b.hold_knees = 0;
            }
            sit_tail(x);
        }

        void unconscious_update(ctx& x) {
            switch (x.which) {
            case id::ss:
            case id::dm:
                // SSOracleBehavior.UnconciousUpdate: falls (the room's
                // gravity back to 1: wanted_room_gravity)
                x.g() = 0.9f;
                break;
            case id::sl:
                if (x.b.moon_awake) {
                    x.g() = 1.f;
                    x.a.is_active = 0;
                    x.b.moon_active = 0;
                }
                break;
            default: break;
            }
        }

        //
        // Oracle.OracleArm
        //
        void joint_update(ctx& x, uint32_t i) {
            auto& a = x.a;
            arm_joint& j = a.joints[i];
            j.last_pos = j.pos;
            j.pos += j.vel;
            j.vel *= 0.7f;
            if (i == 0) {
                j.pos = on_frame_pos(a, x.which, 1.f);
            } else if (i < joint_count - 1) {
                arm_joint const& prev = a.joints[i - 1];
                arm_joint const& next = a.joints[i + 1];
                if (i == 1 && (a.base_moving || x.solid(elbow_pos(a, i - 1, 1.f, j.pos)))) {
                    vec2 va = custom::inverse_kinematic(prev.pos, next.pos, prev.total_length, j.total_length, j.inv_kin_flip);
                    vec2 const va2 = custom::inverse_kinematic(prev.pos, next.pos, prev.total_length, j.total_length, -j.inv_kin_flip);
                    float num = x.solid(va) ? 10.f : 0.f;
                    num += x.solid(elbow_pos(a, i - 1, 1.f, rw::lerp(va, prev.pos, 0.2f))) ? 1.f : 0.f;
                    float num2 = x.solid(va2) ? 10.f : 0.f;
                    num2 += x.solid(elbow_pos(a, i - 1, 1.f, rw::lerp(va2, prev.pos, 0.2f))) ? 1.f : 0.f;
                    if (num > num2) {
                        j.inv_kin_flip *= -1.f;
                    } else if (num == 0.f) {
                        j.vel += rw::clamp_magnitude(va - j.pos, 100.f) / 100.f * 1.8f;
                    }
                }
                vulture::detail::terrain_collision cd{j.pos, j.last_pos, j.vel, 1.f};
                vulture::detail::vertical_collision(x.room, cd);
                vulture::detail::horizontal_collision(x.room, cd);
                vulture::detail::slopes_vertically(x.room, cd);
                j.pos = cd.pos;
                j.vel = cd.vel;
            }
            if (i < joint_count - 1) {
                arm_joint& next = a.joints[i + 1];
                vec2 const vector = custom::dir_vec(j.pos, next.pos);
                float const num3 = rw::distance(j.pos, next.pos);
                float num4 = 0.5f;
                if (i == 0)
                    num4 = 0.f;
                else if (i == joint_count - 2)
                    num4 = 1.f;
                float num5 = -1.f;
                float num6 = 0.5f;
                if (i > 0)
                    num6 = custom::lerp_map(rw::dot(custom::dir_vec(a.joints[i - 1].pos, j.pos), vector), -1.f, 1.f, 1.f, 0.2f);
                if (num3 > j.total_length)
                    num5 = j.total_length;
                else if (num3 < j.total_length * num6)
                    num5 = j.total_length * num6;
                if (num5 > 0.f) {
                    j.pos += vector * (num3 - num5) * num4;
                    j.vel += vector * (num3 - num5) * num4;
                    next.vel -= vector * (num3 - num5) * (1.f - num4);
                }
            } else {
                vec2 a3 = x.pos(1);
                vec2 const dir = get_to_dir_x(x);
                if (is_pebbles(x.which) || x.which == id::dm)
                    a3 -= dir * j.total_length / 2.f;
                else
                    a3 -= custom::perpendicular_vector(dir) * j.total_length / 2.f;
                a3 += custom::dir_vec(x.pos(1), j.pos) * j.total_length / 2.f;
                vec2 const pull = rw::clamp_magnitude(a3 - j.pos, 50.f) / 50.f * 1.2f;
                j.vel += pull;
                j.pos += pull;
                vec2 const a4 = custom::dir_vec(j.pos, x.pos(0));
                float const num7 = rw::distance(j.pos, x.pos(0));
                j.pos += a4 * (num7 - j.total_length);
                j.vel += a4 * (num7 - j.total_length);
            }
        }

        void arm_update(ctx& x) {
            auto& a = x.a;
            auto& b = x.b;
            bool const conscious = awake(b);
            if (x.which == id::rm) {
                a.joints[1].vel += rw::clamp_magnitude(vec2{1289.f, 854.f} - a.joints[1].pos, 50.f) / 50.f * 5.f;
                x.vel(1) *= 0.9f;
                x.vel(0) *= 0.9f;
                vec2 const to = get_to_pos_x(x);
                if (!in_sit_x(x))
                    x.vel(0) += rw::clamp_magnitude(to - x.pos(0), 100.f) / 100.f * 0.5f;
                if (rw::distance(x.pos(1), to) > 48.f)
                    x.vel(1) += rw::clamp_magnitude(to - get_to_dir_x(x) * connection_distance - x.pos(0), 100.f) / 100.f * 0.2f;
            } else if (x.which == id::ss || x.which == id::dm || a.is_active) {
                if (conscious) {
                    float const d = b.dazed > 240.f ? 0.f : 1.f - b.dazed / 240.f;
                    x.vel(0) *= 0.4f;
                    x.vel(1) *= 0.4f;
                    vec2 const to = get_to_pos_x(x);
                    x.vel(0) += rw::clamp_magnitude(to - x.pos(0), 100.f) / 100.f * 6.2f * d;
                    x.vel(1) += rw::clamp_magnitude(to - get_to_dir_x(x) * connection_distance - x.pos(0), 100.f) / 100.f * 3.2f * d;
                }
                // BaseGetToPos: SS's baseIdeal (SLOracleBehavior has none: zero)
                vec2 const bgp = x.which == id::sl ? vec2{0.f, 0.f} : b.base_ideal;
                vec2 const* cp = a.corners;
                vec2 vector{mathf::clamp(bgp.x, cp[0].x, cp[1].x), cp[0].y};
                float num = rw::distance(vector, bgp);
                float num2 = mathf::inverse_lerp(cp[0].x, cp[1].x, bgp.x);
                for (int k = 1; k < 4; ++k) {
                    vec2 v2;
                    if (k % 2 == 0)
                        v2 = vec2{mathf::clamp(bgp.x, cp[0].x, cp[1].x), cp[k].y};
                    else
                        v2 = vec2{cp[k].x, mathf::clamp(bgp.y, cp[2].y, cp[0].y)};
                    float const num3 = rw::distance(v2, bgp);
                    if (num3 < num) {
                        vector = v2;
                        num = num3;
                        if (k == 1)
                            num2 = float(k) + mathf::inverse_lerp(cp[0].y, cp[2].y, bgp.y);
                        else if (k == 2)
                            num2 = float(k) + mathf::inverse_lerp(cp[1].x, cp[0].x, bgp.x);
                        else
                            num2 = float(k) + mathf::inverse_lerp(cp[2].y, cp[0].y, bgp.y);
                    }
                }
                a.base_moving = rw::distance(on_frame_pos(a, x.which, 1.f), vector) > (a.base_moving ? 50.f : 350.f) && b.consistent_base_pos_counter > 30;
                a.last_frame_pos = a.frame_pos;
                if (a.base_moving)
                    a.frame_pos = move_towards_angle(a.frame_pos * 90.f, num2 * 90.f, 1.f) / 90.f;
            } else {
                a.joints[1].vel += rw::clamp_magnitude(vec2{1774.f, 66.f} - a.joints[1].pos, 50.f) / 50.f * 5.f;
                x.vel(1) *= 0.9f;
                x.vel(0) *= 0.9f;
                if (!conscious) {
                    vec2 const to = get_to_pos_x(x);
                    x.vel(0) += rw::clamp_magnitude(to - x.pos(0), 100.f) / 100.f * 0.5f;
                    x.vel(1) += rw::clamp_magnitude(to - get_to_dir_x(x) * connection_distance - x.pos(0), 100.f) / 100.f * 0.2f;
                } else if (x.pos(1).y > 140.f && !b.moon_awake) {
                    x.vel(1).x -= 1.f;
                }
            }
            for (uint32_t i = 0; i < joint_count; ++i)
                joint_update(x, i);
        }

        void init_arm(arm_state& a, id which, room_view const& room, rw::rng& rng, bool sofanthiel) {
            (void)room;
            a = arm_state{};
            a.sofanthiel = sofanthiel ? 1 : 0;
            if (which == id::rm) {
                a.corners[0] = room_view::middle_of_tile(61, 68);
                a.corners[1] = room_view::middle_of_tile(89, 68);
                a.corners[2] = room_view::middle_of_tile(89, 40);
                a.corners[3] = room_view::middle_of_tile(61, 40);
            } else {
                a.corners[0] = room_view::middle_of_tile(10, 31);
                a.corners[1] = room_view::middle_of_tile(38, 31);
                a.corners[2] = room_view::middle_of_tile(38, 3);
                a.corners[3] = room_view::middle_of_tile(10, 3);
                if (which == id::sl)
                    for (auto& cp : a.corners)
                        cp += vec2{1040.f, -20.f};
                if (which == id::cl)
                    for (auto& cp : a.corners)
                        cp += vec2{2810.f, 370.f};
            }
            constexpr float lengths[joint_count] = {300.f, 150.f, 90.f, 30.f};
            // (the joints are made before framePos is set: BasePos at 0)
            for (uint32_t k = 0; k < joint_count; ++k) {
                arm_joint& j = a.joints[k];
                j.inv_kin_flip = rng.value() < 0.5f ? -1.f : 1.f;
                j.total_length = lengths[k];
                j.pos = on_frame_pos(a, which, 1.f);
                j.last_pos = j.pos;
                j.vel = vec2{0.f, 0.f};
            }
            a.frame_pos = 10002.5f;
            a.last_frame_pos = a.frame_pos;
        }

        char const* name_of(id which) {
            switch (which) {
            case id::ss: return "Five Pebbles";
            case id::sl: return "Looks to the Moon";
            case id::dm: return "Looks to the Moon (past)";
            case id::cl: return "Five Pebbles (Saint)";
            case id::rm: return "Five Pebbles (rot)";
            }
            return "oracle";
        }
    }

    dcon::oracle_data_id data(sys::game_state const& state, dcon::creature_id c) noexcept {
        return dcon::oracle_data_id{dcon::oracle_data_id::value_base_t(state.world.creature_get_payload(c).oracle.data)};
    }
    id id_of(sys::game_state const& state, dcon::creature_id c) noexcept { return state.world.creature_get_payload(c).oracle.which; }
    body_state const& body_of(sys::game_state const& state, dcon::creature_id c) noexcept { return state.world.oracle_data_get_body(data(state, c)); }
    arm_state const& arm_of(sys::game_state const& state, dcon::creature_id c) noexcept { return state.world.oracle_data_get_arm(data(state, c)); }
    vec2 chunk_pos(sys::game_state const& state, dcon::creature_id c, uint32_t i) noexcept {
        return state.world.body_chunk_get_pos(creature::chunk(state, c, i));
    }
    vec2 chunk_last_pos(sys::game_state const& state, dcon::creature_id c, uint32_t i) noexcept {
        return state.world.body_chunk_get_last_pos(creature::chunk(state, c, i));
    }

    bool consious(sys::game_state const& state, dcon::creature_id c) noexcept { return awake(body_of(state, c)); }

    vec2 get_to_pos(sys::game_state const& state, dcon::creature_id c) noexcept {
        return get_to_pos_of(id_of(state, c), body_of(state, c), arm_of(state, c), chunk_pos(state, c, 0));
    }
    vec2 get_to_dir(sys::game_state const& state, dcon::creature_id c) noexcept {
        auto const room = world::get_room_view(state, creature::room_of(state, c));
        return get_to_dir_of(id_of(state, c), body_of(state, c), arm_of(state, c), chunk_pos(state, c, 0), room);
    }
    bool in_sit_position(sys::game_state const& state, dcon::creature_id c) noexcept {
        auto const room = world::get_room_view(state, creature::room_of(state, c));
        return in_sit(id_of(state, c), body_of(state, c), chunk_pos(state, c, 0), room);
    }
    bool eyes_closed(sys::game_state const& state, dcon::creature_id c) noexcept {
        auto const& b = body_of(state, c);
        switch (id_of(state, c)) {
        case id::ss:
        case id::dm: return b.move == movement::meditate;
        case id::sl: return b.health == 0.f || in_sit_position(state, c) || b.move == movement::meditate || !awake(b);
        case id::cl:
        case id::rm: return b.health == 0.f || in_sit_position(state, c);
        }
        return false;
    }

    vec2 on_frame_pos(arm_state const& a, id which, float t) noexcept {
        if (which == id::sl)
            return a.sofanthiel ? vec2{1670.f, 605.f} : vec2{1810.f, 356.f};
        if (which == id::cl)
            return vec2{2810.f, 370.f};
        if (which == id::rm)
            return vec2{1210.f, 1140.f};
        float const num = std::fmod(mathf::lerp(a.last_frame_pos, a.frame_pos, t), 4.f);
        float const num2 = 0.1f;
        vec2 const* cp = a.corners;
        float const num3 = std::fabs(cp[0].x - cp[1].x) * num2;
        vec2 av;
        float ang;
        if (num < num2) {
            av = vec2{cp[0].x + num3, cp[1].y - num3};
            ang = -45.f + mathf::inverse_lerp(0.f, num2, num) * 45.f;
        } else if (num < 1.f - num2) {
            return rw::lerp(cp[0], cp[1], mathf::inverse_lerp(0.f, 1.f, num));
        } else if (num < 1.f + num2) {
            av = vec2{cp[1].x - num3, cp[1].y - num3};
            ang = mathf::inverse_lerp(1.f - num2, 1.f + num2, num) * 90.f;
        } else if (num < 2.f - num2) {
            return rw::lerp(cp[1], cp[2], mathf::inverse_lerp(1.f, 2.f, num));
        } else if (num < 2.f + num2) {
            av = vec2{cp[2].x - num3, cp[2].y + num3};
            ang = 90.f + mathf::inverse_lerp(2.f - num2, 2.f + num2, num) * 90.f;
        } else if (num < 3.f - num2) {
            return rw::lerp(cp[2], cp[3], mathf::inverse_lerp(2.f, 3.f, num));
        } else if (num < 3.f + num2) {
            av = vec2{cp[3].x + num3, cp[3].y + num3};
            ang = 180.f + mathf::inverse_lerp(3.f - num2, 3.f + num2, num) * 90.f;
        } else if (num < 4.f - num2) {
            return rw::lerp(cp[3], cp[0], mathf::inverse_lerp(3.f, 4.f, num));
        } else {
            av = vec2{cp[0].x + num3, cp[0].y - num3};
            ang = 270.f + mathf::inverse_lerp(4.f - num2, 4.f, num) * 45.f;
        }
        return av + custom::deg_to_vec(ang) * num3;
    }

    vec2 base_dir(arm_state const& a, id which, float t) noexcept {
        if (which == id::sl)
            return a.sofanthiel ? vec2{0.f, -1.f} : vec2{-1.f, 0.f};
        if (which == id::cl)
            return vec2{-1.f, 0.4f};
        if (which == id::rm)
            return vec2{1.f, 0.f};
        float const num = std::fmod(mathf::lerp(a.last_frame_pos, a.frame_pos, t), 4.f);
        float const num2 = 0.1f;
        vec2 const r{1.f, 0.f}, d{0.f, -1.f}, l{-1.f, 0.f}, u{0.f, 1.f};
        if (num < num2)
            return rw::slerp(r, d, 0.5f + mathf::inverse_lerp(0.f, num2, num) * 0.5f);
        if (num < 1.f - num2)
            return d;
        if (num < 1.f + num2)
            return rw::slerp(d, l, mathf::inverse_lerp(1.f - num2, 1.f + num2, num));
        if (num < 2.f - num2)
            return l;
        if (num < 2.f + num2)
            return rw::slerp(l, u, mathf::inverse_lerp(2.f - num2, 2.f + num2, num));
        if (num < 3.f - num2)
            return u;
        if (num < 3.f + num2)
            return rw::slerp(u, r, mathf::inverse_lerp(3.f - num2, 3.f + num2, num));
        if (num < 4.f - num2)
            return r;
        return rw::slerp(r, d, mathf::inverse_lerp(4.f - num2, 4.f, num) * 0.5f);
    }

    vec2 elbow_pos(arm_state const& a, uint32_t joint, float t, vec2 tip) noexcept {
        arm_joint const& j = a.joints[joint];
        vec2 const vc = rw::lerp(j.last_pos, j.pos, t);
        float const flip = joint % 2 == 0 ? 1.f : -1.f;
        if (joint < joint_count - 1)
            return custom::inverse_kinematic(tip, vc, j.total_length * 0.33333334f, j.total_length * 0.6666667f, flip);
        return custom::inverse_kinematic(tip, vc, j.total_length / 3.f, j.total_length / 3.f, flip);
    }

    dcon::creature_id spawn_room(sys::game_state& state, dcon::room_id room) {
        auto const& w = state.world;
        std::string_view const name = state.to_string_view(w.room_get_name(room));
        auto const region = w.room_get_region_from_region_membership(room);
        std::string_view const region_name = region ? state.to_string_view(w.region_get_name(region)) : std::string_view{};
        // Room.Loaded: SS_AI, SL_AI, and Downpour's DM_AI, CL_AI, RM_AI
        // (HR_AI's only come after Saint's ascension)
        bool const vanilla_room = same_name(name, "SS_AI") || same_name(name, "SL_AI");
        bool const msc_room = same_name(name, "DM_AI") || same_name(name, "CL_AI") || same_name(name, "RM_AI");
        if (!vanilla_room && !msc_room)
            return dcon::creature_id{};
        // Oracle's constructor
        id which;
        if (same_name(name, "CL_AI")) {
            which = id::cl;
        } else {
            bool const rm = same_name(region_name, "RM");
            which = same_name(name, "SS_AI") || rm ? id::ss : id::sl;
            if (same_name(name, "DM_AI"))
                which = id::dm;
            if (which == id::ss && rm)
                which = id::rm;
        }
        return create(state, room, which);
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, id which) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::oracle);
        {
            auto& p = w.creature_get_payload(c).oracle;
            p.which = which;
        }
        auto const d = data(state, c);
        auto rng = rw::random(state.seed, 0, uint32_t(c.value));
        auto const view = world::get_room_view(state, room);
        physics::object_desc desc;
        desc.gravity = which == id::sl || which == id::cl ? 0.9f : 0.f;
        desc.air_friction = 0.99f;
        desc.bounce = 0.1f;
        desc.surface_friction = 0.17f;
        desc.water_friction = 0.92f;
        desc.buoyancy = 0.95f;
        auto const o = physics::create_object(state, room, desc);
        dcon::body_chunk_id k[2];
        for (uint32_t l = 0; l < 2; ++l) {
            vec2 p;
            switch (which) {
            case id::cl: p = vec2{2570.f, 160.f}; break;
            case id::rm: p = vec2{1395.f, 850.f}; break;
            case id::sl: p = vec2{1585.f + 5.f * float(l), 240.f - 5.f * float(l)}; break;
            default: p = vec2{350.f, 350.f}; break;
            }
            k[l] = physics::add_chunk(state, o, p, 6.f, 0.5f);
        }
        physics::add_connection(state, o, k[0], k[1], connection_distance, physics::connection_type::normal, 1.f, 0.5f);
        w.try_create_creature_body(c, o);
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_set_grasps(c, creature::grasp_pair{});
        w.creature_set_behavior(c, 0);
        w.creature_set_current_utility(c, 0.f);
        w.creature_set_destination(c, room_view::get_tile_position(w.body_chunk_get_pos(k[0])));

        body_state b{};
        auto const story = state.story_slugcat;
        using sn = ::player::slugcat_name;
        if (which == id::sl) {
            // SLOrcacleState's neurons: none for Hunter and Sofanthiel, 7 for
            // Rivulet and Saint, else 5
            int32_t const neurons = story == sn::red || story == sn::inv ? 0 : story == sn::rivulet || story == sn::saint ? 7 : 5;
            b.health = mathf::inverse_lerp(0.f, 5.f, float(neurons));
            b.moon_awake = story == sn::saint && neurons > 0 ? 1 : 0;
        } else if (which == id::rm) {
            b.health = 5.f;
        }
        vec2 const p0 = w.body_chunk_get_pos(k[0]);
        b.current_get_to = p0;
        b.last_pos = p0;
        b.next_pos = p0;
        b.path_progression = 1.f;
        if (which == id::ss || which == id::dm) {
            // SSOracleBehavior's constructor
            b.investigate_angle = rng.value() * 360.f;
            b.working = 1.f;
            b.get_to_working = 1.f;
            b.move = rng.value() < 0.5f ? movement::meditate : movement::idle;
        }
        b.hold_knees = 1;
        arm_state a;
        init_arm(a, which, view, rng, which == id::sl && story == sn::inv);
        w.oracle_data_set_body(d, b);
        w.oracle_data_set_arm(d, a);
        w.oracle_data_set_graphics(d, graphics_state{});
        oracle_graphics::reset_positions(state, c);
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        creature::release_target(state, c);
        creature::erase(state, c);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        ctx x{state, c, room, bs(state, c), as(state, c), id_of(state, c), rng};
        auto& b = x.b;
        // PhysicalObject.Update
        physics::update_object(state, room, creature::body(state, c), nullptr, nullptr);
        if (b.dazed > 0.f)
            b.dazed -= 1.f;
        if (awake(b)) {
            ++b.behavior_time;
            switch (x.which) {
            case id::ss:
            case id::dm: ss_update(x); break;
            case id::sl: sl_update(x); break;
            case id::cl: cl_update(x); break;
            case id::rm: rm_update(x); break;
            }
        } else {
            unconscious_update(x);
        }
        arm_update(x);
        if (b.spasms > 0) {
            --b.spasms;
            for (uint32_t i = 0; i < 2; ++i) {
                vec2 const r = custom::rnv(rng);
                x.vel(i) += r * rng.value() * 5.f;
            }
        }
        if (b.stun > 0)
            --b.stun;
        if (b.health < 1.f && rng.value() > mathf::pow(mathf::max(b.health, 0.f), 0.2f) * 2.5f)
            b.stun = std::max(b.stun, rng.range(2, 8));
    }

    bool collides(sys::game_state const&, dcon::creature_id, dcon::creature_id) { return true; }

    void collide(sys::game_state&, dcon::creature_id, room_view const&, uint32_t, dcon::creature_id, uint32_t) {}

    void violence(sys::game_state& state, dcon::creature_id c, uint32_t k, vec2 momentum, float, float, creature::damage_type) {
        auto& w = state.world;
        if (k >= 2)
            k = 0;
        auto const hit = creature::chunk(state, c, k);
        w.body_chunk_get_vel(hit) += rw::clamp_magnitude(momentum / w.body_chunk_get_mass(hit), 10.f);
    }

    void release(sys::game_state&, dcon::creature_id, dcon::creature_id) {}

    void died(sys::game_state& state, dcon::creature_id c) {
        // (an iterator isn't a creature that dies: back up)
        state.world.creature_set_dead(c, 0);
    }

    void place_in_room(sys::game_state&, dcon::creature_id, dcon::room_id, vec2) {}

    std::string describe(sys::game_state const& state, dcon::creature_id c) {
        auto const& b = body_of(state, c);
        vec2 const p = chunk_pos(state, c, 0);
        static constexpr char const* moves[] = {"idle", "meditating", "showing media"};
        std::string what = sits(id_of(state, c)) && !b.moon_active ? (in_sit_position(state, c) ? "sitting" : "crawling") : moves[uint32_t(b.move) % 3];
        if (!awake(b))
            what = "unconscious";
        return std::string{name_of(id_of(state, c))} + " at " + std::to_string(int(p.x)) + "," + std::to_string(int(p.y)) + " " + what;
    }

    float wanted_room_gravity(sys::game_state const& state, dcon::creature_id c) {
        auto const& b = body_of(state, c);
        switch (id_of(state, c)) {
        case id::ss:
        case id::dm: return awake(b) ? 1.f - b.working : 1.f;
        case id::sl: return b.moon_awake && awake(b) ? 0.2f : -1.f;
        default: return -1.f;
        }
    }
}
