// KingTusks.cs: the king vulture's two harpoons. Each sits on the head
// (Attached) while its laser sight charges; homing its eyes in on its prey
// the king aims (Charging, 25 ticks, 10 in close quarters) and shoots
// (ShootingOut): the tusk flies along its aim on a wire, sticks in the
// terrain (StuckInWall), or impales the first creature it meets
// (StuckInCreature: Violence Stab 1.5, the slugcat dies), and the wire
// (500 long, reeled in after a while) pulls what it impaled to the head,
// which grabs it when they touch. Missing, it dangles, then is reeled back
// (Retracting) and reattaches. A spear hitting the head may knock an
// attached tusk loose (HitBySpear).
//
// The wire is vanilla's Rope (wrapping around the terrain's corners) as a
// straight line from the head to the tusk here; its look (`wire`, 20 points)
// moves only where a camera looks.

#include <algorithm>
#include <cmath>

#include "vulture_internal.hpp"
#include "particles.hpp"
#include "rooms.hpp"
#include "tracker.hpp"

namespace vulture::detail {
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        king_tusks& tk(ctx& x) { return x.w().vulture_data_get_tusks(x.d); }
        vec2 neck_tip(ctx& x) { return x.neck.tip().pos; }

        // the tusk's end the wire pulls at: the impaled chunk, else the tusk's back
        bool impale_chunk(ctx& x, tusk const& t, dcon::body_chunk_id& out) {
            if (!t.impale || !creature::here(x.state, t.impale, x.room.id))
                return false;
            if (t.impale_chunk >= creature::chunk_count_of(x.state, t.impale))
                return false;
            out = creature::chunk_of(x.state, t.impale, t.impale_chunk);
            return true;
        }

        void switch_mode(tusk& t, tusk_mode m) {
            if (t.mode == m)
                return;
            if (m != tusk_mode::stuck_in_creature)
                t.impale = dcon::creature_id{};
            t.mode_counter = 0;
            t.mode = m;
        }

        bool ready(tusk const& t) { return t.mode == tusk_mode::attached && t.laser_power == 1.f; }

        // KingTusks.CloseQuarters
        bool close_quarters(ctx& x) {
            auto& k = tk(x);
            ivec2 const hp = room_view::get_tile_position(x.pos(head));
            return (x.proximity(room_view::get_tile_position(k.prey_pos)) == 1 && x.proximity(hp) < 2) || (x.room.inside(hp.x, hp.y) && pathing::narrow_space(x.room, hp));
        }

        // KingTusks.Tusk.AimDir
        vec2 aim_dir(ctx& x, tusk const& t, uint32_t side) {
            auto& k = tk(x);
            vec2 v = custom::dir_vec(neck_tip(x), x.pos(head));
            float const num = mathf::inverse_lerp(0.f, 25.f, float(t.mode_counter) + 1.f);
            if (k.last_eyes_home > 0.f || k.eyes_home_in > 0.f) {
                vec2 const v2 = custom::dir_vec(x.pos(head), k.prey_pos);
                v = rw::slerp(v, v2, k.eyes_home_in * mathf::pow(mathf::inverse_lerp(0.2f, 0.85f - 0.2f * num, rw::dot(v, v2)), 2.f - 1.5f * num));
            }
            if (k.last_eyes_out > 0.f || k.eyes_out > 0.f)
                v += custom::perpendicular_vector(v) * custom::deg_to_vec(k.head_rot + (side == 0 ? -90.f : 90.f)).x * k.eyes_out * 0.5f;
            return rw::normalized(v);
        }

        // KingTusks.VisualOnAnyTargetChunk
        bool visual_on_any_target_chunk(ctx& x) {
            auto& k = tk(x);
            if (!k.target || !creature::here(x.state, k.target, x.room.id))
                return false;
            if (auto const* r = tracker::find(tracker::of(x.state, x.c), k.target); r && r->visual)
                return true;
            uint32_t const n = creature::chunk_count_of(x.state, k.target);
            for (uint32_t i = 0; i < n; ++i)
                if (x.room.visual_contact(x.pos(head), x.w().body_chunk_get_pos(creature::chunk_of(x.state, k.target, i))))
                    return true;
            return false;
        }

        // the pathfinder can reach a tile next to the target (the min
        // distance checks of WantToShoot and GoodShootAngle)
        bool target_reachable(ctx& x) {
            auto& k = tk(x);
            auto const* r = tracker::find(tracker::of(x.state, x.c), k.target);
            if (!r)
                return false;
            ivec2 const g = tracker::best_guess(*r);
            for (auto const d : custom::eight_directions_and_zero) {
                ivec2 const t = g + d;
                if (x.accessible(t) && pathing::distance(x.state, x.path(), t) != pathing::unreachable)
                    return true;
            }
            return false;
        }

        // KingTusks.GoodShootAngle
        float good_shoot_angle(ctx& x, int32_t tusk_i, bool check_min_distance) {
            auto& k = tk(x);
            auto const* r = k.target ? tracker::find(tracker::of(x.state, x.c), k.target) : nullptr;
            if (!r || r->ticks_since_seen > 20)
                return 0.f;
            vec2 const lhs = tusk_i == -1 ? rw::slerp(aim_dir(x, k.t[0], 0), aim_dir(x, k.t[1], 1), 0.5f) : aim_dir(x, k.t[tusk_i], uint32_t(tusk_i));
            float num = mathf::inverse_lerp(0.8f, 1.f, rw::dot(lhs, custom::dir_vec(x.pos(head), k.prey_pos)));
            num *= mathf::inverse_lerp(shoot_range, shoot_range * 0.8f, rw::distance(x.pos(head), k.prey_pos));
            if (num == 0.f)
                return 0.f;
            if (check_min_distance && target_reachable(x))
                num *= mathf::inverse_lerp(min_shoot_range, mathf::lerp(min_shoot_range, shoot_range, 0.3f), rw::distance(x.pos(head), k.prey_pos));
            return num;
        }

        // KingTusks.TryToShoot
        void try_to_shoot(ctx& x) {
            auto& k = tk(x);
            int32_t num = x.rng.range(0, 2);
            if (!ready(k.t[num]))
                num = 1 - num;
            if (!ready(k.t[num]))
                return;
            switch_mode(k.t[num], tusk_mode::charging);
            // (King_Vulture_Tusk_Aim has no sample here)
            if (!custom::dist_less(x.last_pos(1), x.pos(1), 5.f))
                air_brake(x, 15);
        }

        // KingTusks.Tusk.ShootUpdate
        void shoot_update(ctx& x, tusk& t, uint32_t side, float speed) {
            (void)side;
            for (auto& cp : t.chunk_points) {
                cp.last_pos = cp.pos;
                cp.vel = vec2{0.f, 0.f};
            }
            constexpr float num = 20.f;
            vec2 const v = t.chunk_points[0].pos + t.shoot_dir * num;
            vec2 v2 = t.chunk_points[0].pos + t.shoot_dir * (num + speed);
            vec2 wall_at{0.f, 0.f}, wall_in{0.f, 0.f};
            bool wall = exact_terrain_ray_trace(x.room, v, v2, wall_at, wall_in);
            // TraceProjectileAgainstBodyChunks (rad 5, not the vulture itself)
            dcon::creature_id hit;
            creature::hit_part hp;
            bool chunk_hit = creature::trace(x.state, x.room.id, v, v2, 5.f, x.c, hit, hp) && hp.chunk >= 0 && creature::here(x.state, hit, x.room.id);
            vec2 collision_point = v2;
            if (chunk_hit) {
                // the closest approach of the tusk's way to the chunk
                vec2 const cpos = x.w().body_chunk_get_pos(creature::chunk_of(x.state, hit, uint32_t(hp.chunk)));
                vec2 const dv = v2 - v;
                float const len2 = rw::sqr_magnitude(dv);
                float const f = len2 > 0.f ? mathf::clamp01(rw::dot(cpos - v, dv) / len2) : 0.f;
                collision_point = v + dv * f;
            }
            if (wall && chunk_hit) {
                if (rw::distance(v, wall_at) < rw::distance(v, collision_point))
                    chunk_hit = false;
                else
                    wall = false;
            }
            if (wall) {
                v2 = wall_at - t.shoot_dir * num * 0.7f;
                if (t.mode_counter > 0 && rw::dot(t.shoot_dir, wall_in) > custom::lerp_map(float(t.mode_counter), 1.f, 8.f, 0.65f, 0.95f)) {
                    t.has_wall_pos = 1;
                    t.wall_pos = v2;
                    switch_mode(t, tusk_mode::stuck_in_wall);
                    // (King_Vulture_Tusk_Stick_In_Terrain has no sample here)
                    t.stuck = 1.f;
                } else {
                    if (wall_in.x != 0.f)
                        t.chunk_points[0].vel.x = (std::fabs(t.chunk_points[0].vel.x) + 15.f) * mathf::sign(wall_in.x) * -1.5f;
                    if (wall_in.y != 0.f)
                        t.chunk_points[0].vel.y = (std::fabs(t.chunk_points[0].vel.y) + 15.f) * mathf::sign(wall_in.y) * -1.5f;
                    vec2 const a = custom::rnv(x.rng);
                    t.chunk_points[0].vel += a * 10.f;
                    t.chunk_points[1].vel -= a * 10.f;
                    switch_mode(t, tusk_mode::dangling);
                }
            } else if (chunk_hit) {
                v2 = collision_point - t.shoot_dir * num * 0.7f;
                t.chunk_points[0].pos = v2 - t.shoot_dir * num;
                t.chunk_points[1].pos = v2 - t.shoot_dir * (num + tusk_length);
                switch_mode(t, tusk_mode::stuck_in_creature);
                // (King_Vulture_Tusk_Impale_Creature has no sample here)
                t.impale = hit;
                t.impale_chunk = uint8_t(hp.chunk);
                auto const kc = creature::chunk_of(x.state, hit, uint32_t(hp.chunk));
                vec2& kv = x.w().body_chunk_get_vel(kc);
                kv += t.shoot_dir * 12.f / x.w().body_chunk_get_mass(kc);
                kv = rw::clamp_magnitude(kv, 50.f);
                creature::violence_on(x.state, hit, uint32_t(hp.chunk), vec2{0.f, 0.f}, 1.5f, 0.f, creature::damage_type::stab, creature::tmpl::king_vulture);
                t.shoot_dir = rw::slerp(t.shoot_dir, custom::dir_vec(v2, x.w().body_chunk_get_pos(kc)), 0.4f);
                return;
            }
            t.chunk_points[0].pos = v2 - t.shoot_dir * num;
            t.chunk_points[1].pos = v2 - t.shoot_dir * (num + tusk_length);
            if (x.viewed && x.room.point_submerged(t.chunk_points[0].pos))
                particles::bubbles(x.state, x.room.id, rw::lerp(v, v2, 0.5f), 8, 15.f);
            t.elasticity = 0.2f;
            if (t.mode == tusk_mode::shooting_out) {
                float const num2 = rw::distance(x.pos(head), t.chunk_points[1].pos);
                t.wire_extra_slack = mathf::inverse_lerp(shoot_range * 0.8f, shoot_range * 0.5f, num2);
                if (num2 > shoot_range) {
                    switch_mode(t, tusk_mode::dangling);
                    vec2 const d = custom::dir_vec(x.pos(head), t.chunk_points[1].pos);
                    x.pos(head) += d * 10.f;
                    x.vel(head) += d * 10.f;
                    t.chunk_points[0].vel = t.shoot_dir * speed * 0.4f;
                    t.chunk_points[1].vel = t.shoot_dir * speed * 0.6f;
                    vec2 const a2 = custom::rnv(x.rng);
                    t.chunk_points[0].pos += a2 * 4.f;
                    t.chunk_points[0].vel += a2 * 6.f;
                    t.chunk_points[1].pos -= a2 * 4.f;
                    t.chunk_points[1].vel -= a2 * 6.f;
                }
            }
        }

        // KingTusks.Tusk.Shoot
        void shoot(ctx& x, tusk& t, uint32_t side, vec2 hang_pos) {
            auto& k = tk(x);
            switch_mode(t, tusk_mode::shooting_out);
            // (King_Vulture_Tusk_Shoot has no sample here)
            t.shoot_dir = aim_dir(x, t, side);
            k.no_shoot_delay = 20;
            t.stuck = 0.f;
            t.attached = 0.f;
            t.curr_wire_length = max_wire_length;
            x.vel(head) -= t.shoot_dir * 25.f;
            x.pos(head) -= t.shoot_dir * 25.f;
            x.last_pos(head) -= t.shoot_dir * 25.f;
            uint32_t const n = uint32_t(x.neck.c.size());
            for (uint32_t i = 0; i < n; ++i) {
                float const f = mathf::inverse_lerp(0.f, float(n - 1), float(i)) * 35.f;
                x.neck.c[i].pos -= t.shoot_dir * f;
                x.neck.c[i].last_pos -= t.shoot_dir * f;
                x.neck.c[i].vel -= t.shoot_dir * f;
            }
            x.vel(0) -= t.shoot_dir * 5.f;
            t.wire_extra_slack = 1.f;
            t.wire_loose = 1.f;
            t.last_wire_loose = 1.f;
            t.laser_power = 0.f;
            t.laser_alpha = 0.f;
            shoot_update(x, t, side, 60.f);
            t.has_rope = 1;
            for (uint32_t j = 0; j < wire_points; ++j) {
                float const f = mathf::inverse_lerp(0.f, float(wire_points), float(j));
                vec2 const a = custom::rnv(x.rng) * (1.f - f);
                t.wire[j].last_pos = rw::lerp(x.pos(head), t.chunk_points[1].pos, f);
                t.wire[j].pos = t.wire[j].last_pos + a * 80.f * x.rng.value();
                t.wire[j].vel = a * 160.f * x.rng.value();
            }
            // the smoke of the shot (dust puffs, where a camera looks)
            if (x.viewed && !x.room.point_submerged(x.pos(head))) {
                auto g = rw::random(x.state.seed, x.state.tick, rw::ids(uint32_t(x.room.id.value), rw::ids(uint32_t(x.c.value), side)));
                vec2 const a2 = custom::dir_vec(x.pos(head), hang_pos);
                for (int32_t i = 0; i < 8; ++i) {
                    float const f = float(i) / 8.f;
                    vec2 const vel = rw::lerp(a2, -t.shoot_dir, f) * mathf::lerp(15.f, 60.f, g.value() * (1.f - f)) * 0.1f + custom::rnv(g) * g.value() * 2.f;
                    particles::spawn(x.state, x.room.id, particles::kind::dust, (x.pos(head) + t.chunk_points[0].pos + t.chunk_points[1].pos) / 3.f, vel, 10.f, 1.f,
                        rw::vec3{0.8f, 0.8f, 0.8f}, 0.5f);
                }
            }
        }

        // the wire's look (cosmetic)
        void wire_update(ctx& x, tusk& t, uint32_t side, vec2 a) {
            if (t.last_wire_loose == 0.f && t.wire_loose == 0.f) {
                auto g = rw::random(x.state.seed, x.state.tick, rw::ids(uint32_t(x.room.id.value), rw::ids(uint32_t(x.c.value), side)));
                for (auto& p : t.wire) {
                    p.pos = x.pos(head) + custom::rnv(g);
                    p.last_pos = p.pos;
                    p.vel = vec2{0.f, 0.f};
                }
                return;
            }
            vec2 const end = t.chunk_points[0].pos;
            float const total = rw::distance(x.pos(head), end);
            float num3 = t.has_rope ? total / float(wire_points) * 0.5f : 1.f;
            num3 *= t.wire_loose;
            num3 += 10.f * t.wire_extra_slack;
            float num4 = mathf::inverse_lerp(t.curr_wire_length * 0.75f, t.curr_wire_length, total);
            num4 *= 1.f - t.wire_extra_slack;
            for (uint32_t l = 0; l < wire_points; ++l) {
                auto& p = t.wire[l];
                p.last_pos = p.pos;
                p.pos += p.vel;
                if (x.room.point_submerged(p.pos)) {
                    p.vel *= 0.7f;
                    p.vel.y += 0.2f;
                } else {
                    p.vel *= mathf::lerp(0.98f, 1.f, t.wire_extra_slack);
                    p.vel.y -= 0.9f * (1.f - t.wire_extra_slack);
                }
                if (t.has_rope) {
                    // OnRopePos along the straight line
                    vec2 const a3 = rw::lerp(x.pos(head), end, mathf::inverse_lerp(0.f, float(wire_points - 1), float(l)));
                    p.vel += (a3 - p.pos) * (1.f - t.wire_extra_slack) / mathf::lerp(60.f, 2.f, num4);
                    p.pos += (a3 - p.pos) * (1.f - t.wire_extra_slack) / mathf::lerp(60.f, 2.f, num4);
                    p.pos = rw::lerp(a3, p.pos, t.wire_loose);
                }
            }
            for (uint32_t m = 1; m < wire_points; ++m) {
                if (!custom::dist_less(t.wire[m].pos, t.wire[m - 1].pos, num3)) {
                    vec2 const v4 = custom::dir_vec(t.wire[m].pos, t.wire[m - 1].pos) * (rw::distance(t.wire[m].pos, t.wire[m - 1].pos) - num3);
                    t.wire[m].pos += v4 / 2.f;
                    t.wire[m].vel += v4 / 2.f;
                    t.wire[m - 1].pos -= v4 / 2.f;
                    t.wire[m - 1].vel -= v4 / 2.f;
                }
            }
            vec2 v5 = neck_tip(x);
            v5 += a * t.z_rot.x * 15.f;
            v5 += a * t.z_rot.y * (side == 0 ? -1.f : 1.f) * 7.f;
            if (!custom::dist_less(t.wire[0].pos, v5, num3)) {
                vec2 const v4 = custom::dir_vec(t.wire[0].pos, v5) * (rw::distance(t.wire[0].pos, v5) - num3);
                t.wire[0].pos += v4;
                t.wire[0].vel += v4;
            }
            v5 = t.chunk_points[0].pos;
            auto& last = t.wire[wire_points - 1];
            if (!custom::dist_less(last.pos, v5, num3)) {
                vec2 const v4 = custom::dir_vec(last.pos, v5) * (rw::distance(last.pos, v5) - num3);
                last.pos += v4;
                last.vel += v4;
            }
        }

        // KingTusks.DoIWantToHoldCreature
        bool want_to_hold(ctx& x, dcon::creature_id t) {
            behavior const b = current(x);
            return !x.grasped() && (b == behavior::hunt || b == behavior::idle) && dynamic_relationship(x, t).type == creature::rel::eats;
        }

        // KingTusks.Tusk.Update
        void tusk_update(ctx& x, tusk& t, uint32_t side) {
            auto& k = tk(x);
            auto& other = k.t[1 - side];
            t.last_z_rot = t.z_rot;
            t.last_wire_loose = t.wire_loose;
            t.last_laser_alpha = t.laser_alpha;
            t.z_rot = rw::slerp(t.z_rot, custom::deg_to_vec(k.head_rot + (side == 0 ? -90.f : 90.f)), 0.9f * t.attached);
            vec2 const v = custom::dir_vec(neck_tip(x), x.pos(head));
            vec2 const a = custom::perpendicular_vector(v);
            vec2 v2 = x.pos(head) + v * -5.f;
            v2 += a * t.z_rot.x * 15.f;
            v2 += a * t.z_rot.y * (side == 0 ? -1.f : 1.f) * 7.f;
            t.laser_power = custom::lerp_and_tick(t.laser_power, t.attached, 0.01f, 0.008333334f);
            if (other.mode == tusk_mode::charging || !x.awake())
                t.laser_alpha = mathf::max(t.laser_alpha - 0.1f, 0.f);
            else if (x.rng.value() < 0.25f)
                t.laser_alpha = x.rng.value() < t.laser_power ? mathf::lerp(t.laser_alpha, mathf::pow(t.laser_power, 0.25f), mathf::pow(x.rng.value(), 0.5f))
                                                              : t.laser_alpha * x.rng.value() * x.rng.value();
            ++t.mode_counter;
            if (t.mode != tusk_mode::shooting_out) {
                t.wire_extra_slack = mathf::max(0.f, t.wire_extra_slack - 0.033333335f);
                t.elasticity = mathf::min(0.9f, t.elasticity + 0.025f);
            }
            switch (t.mode) {
            case tusk_mode::attached:
                t.attached = 1.f;
                break;
            case tusk_mode::charging: {
                t.attached = custom::lerp_map(float(t.mode_counter), 0.f, 25.f, 0.2f, 1.f);
                bool const cq = close_quarters(x);
                if (t.mode_counter > (cq ? 10 : 25)) {
                    if (x.awake() && k.target && k.no_shoot_delay < 1
                        && good_shoot_angle(x, int32_t(side), false) > (cq ? 0.6f : (ready(other) ? 0.2f : 0.4f)) && visual_on_any_target_chunk(x)) {
                        shoot(x, t, side, v2);
                    } else {
                        // (King_Vulture_Tusk_Cancel_Shot has no sample here)
                        switch_mode(t, tusk_mode::attached);
                        k.no_shoot_delay = std::max(k.no_shoot_delay, 10);
                    }
                }
                break;
            }
            case tusk_mode::shooting_out:
                t.attached = 0.f;
                t.curr_wire_length = max_wire_length;
                if (t.mode_counter > (x.room.point_submerged(t.chunk_points[0].pos) ? 6 : 10))
                    switch_mode(t, tusk_mode::dangling);
                break;
            case tusk_mode::dangling:
                t.attached = 0.f;
                if (t.mode_counter > 80)
                    switch_mode(t, tusk_mode::retracting);
                break;
            case tusk_mode::retracting:
                if (t.curr_wire_length > 0.f) {
                    t.curr_wire_length = mathf::max(0.f, t.curr_wire_length - max_wire_length / 90.f);
                    t.attached = 0.f;
                } else if (t.attached < 1.f) {
                    t.attached = mathf::min(1.f, t.attached + 0.05f);
                } else {
                    switch_mode(t, tusk_mode::attached);
                }
                break;
            case tusk_mode::stuck_in_creature:
                t.attached = 0.f;
                if (t.mode_counter > 80)
                    t.curr_wire_length = mathf::max(100.f, t.curr_wire_length - max_wire_length / 180.f);
                if (!t.impale)
                    switch_mode(t, tusk_mode::dangling);
                break;
            case tusk_mode::stuck_in_wall:
                t.attached = 0.f;
                if (t.mode_counter > 240)
                    t.curr_wire_length = mathf::max(100.f, t.curr_wire_length - max_wire_length / 180.f);
                if (!t.has_wall_pos || t.stuck <= 0.f) {
                    switch_mode(t, tusk_mode::dangling);
                } else {
                    for (auto& cp : t.chunk_points) {
                        cp.last_pos = cp.pos;
                        cp.vel = vec2{0.f, 0.f};
                    }
                    t.chunk_points[0].pos = t.wall_pos;
                    t.chunk_points[1].pos = t.wall_pos - t.shoot_dir * tusk_length;
                    if (t.has_rope && rw::distance(x.pos(head), t.chunk_points[0].pos) >= t.curr_wire_length)
                        t.chunk_points[1].pos += custom::dir_vec(t.chunk_points[1].pos, x.pos(head)) * x.rng.value() * 10.f * (1.f - t.stuck);
                }
                break;
            }
            vec2 a2 = v;
            if (t.mode == tusk_mode::charging)
                a2 = rw::slerp(v, aim_dir(x, t, side), mathf::inverse_lerp(0.f, 25.f, float(t.mode_counter)));
            bool const stuck_or_shooting = t.mode == tusk_mode::stuck_in_creature || t.mode == tusk_mode::stuck_in_wall || t.mode == tusk_mode::shooting_out;
            if (!stuck_or_shooting) {
                for (uint32_t j = 0; j < 2; ++j) {
                    auto& cp = t.chunk_points[j];
                    cp.last_pos = cp.pos;
                    cp.pos += cp.vel;
                    if (x.room.point_submerged(cp.pos)) {
                        cp.vel *= 0.95f;
                        cp.vel.y += 0.1f;
                    } else {
                        cp.vel *= 0.98f;
                        cp.vel.y -= 0.9f;
                    }
                    if (t.attached != 1.f && custom::dist_less(cp.pos, cp.last_pos, 200.f)) {
                        terrain_collision cd{cp.pos, cp.last_pos, cp.vel, 2.f};
                        vertical_collision(x.room, cd);
                        horizontal_collision(x.room, cd);
                        cp.pos = cd.pos;
                        cp.vel = cd.vel;
                        if (cd.contact.y != 0)
                            cp.vel.x *= 0.5f;
                        if (cd.contact.x != 0)
                            cp.vel.y *= 0.5f;
                    }
                    if (t.attached > 0.f) {
                        vec2 const v3 = v2 + a2 * tusk_length * (j == 0 ? 0.5f : -0.5f);
                        float const num2 = mathf::lerp(6.f, 1.f, t.attached);
                        if (!custom::dist_less(cp.pos, v3, num2)) {
                            vec2 const v4 = custom::dir_vec(cp.pos, v3) * (rw::distance(cp.pos, v3) - num2);
                            cp.pos += v4;
                            cp.vel += v4;
                        }
                    }
                }
                auto& c0 = t.chunk_points[0];
                auto& c1 = t.chunk_points[1];
                vec2 const v4 = custom::dir_vec(c0.pos, c1.pos) * (rw::distance(c0.pos, c1.pos) - tusk_length);
                c0.pos += v4 / 2.f;
                c0.vel += v4 / 2.f;
                c1.pos -= v4 / 2.f;
                c1.vel -= v4 / 2.f;
            }
            t.wire_loose = custom::lerp_and_tick(t.wire_loose, t.attached > 0.f ? 0.f : 1.f, 0.07f, 0.033333335f);
            if (x.viewed)
                wire_update(x, t, side, a);
            if (t.mode == tusk_mode::shooting_out)
                shoot_update(x, t, side, custom::lerp_map(float(t.mode_counter), 0.f, 8.f, 50.f, 30.f, 3.f));
            dcon::body_chunk_id ic;
            if (t.impale) {
                if (t.mode != tusk_mode::stuck_in_creature || !impale_chunk(x, t, ic) || x.w().creature_get_entering_shortcut(t.impale).x >= 0) {
                    t.impale = dcon::creature_id{};
                } else if (x.awake() && t.mode_counter > 20 && x.rng.value() < custom::lerp_map(float(t.mode_counter), 20.f, 80.f, 0.0016666667f, 0.033333335f)
                    && !want_to_hold(x, t.impale)) {
                    if (x.grasped() == t.impale) {
                        t.curr_wire_length = 0.f;
                        switch_mode(t, tusk_mode::retracting);
                    } else {
                        switch_mode(t, tusk_mode::dangling);
                    }
                    t.impale = dcon::creature_id{};
                } else {
                    for (auto& cp : t.chunk_points) {
                        cp.last_pos = cp.pos;
                        cp.vel = vec2{0.f, 0.f};
                    }
                    vec2 const ipos = x.w().body_chunk_get_pos(ic);
                    float const irad = x.w().body_chunk_get_rad(ic);
                    // (no rotationChunk: the rope's way back to the head)
                    vec2 const v6 = custom::dir_vec(ipos, x.pos(head));
                    t.chunk_points[0].pos = ipos - v6 * irad;
                    t.chunk_points[1].pos = ipos - v6 * (irad + tusk_length);
                    auto const* f = focus(x);
                    if (current(x) == behavior::hunt && !x.grasped() && f && f->who == t.impale) {
                        uint32_t const n = creature::chunk_count_of(x.state, t.impale);
                        for (uint32_t i = 0; i < n; ++i) {
                            auto const kc = creature::chunk_of(x.state, t.impale, i);
                            if (custom::dist_less(x.w().body_chunk_get_pos(kc), x.pos(head), x.w().body_chunk_get_rad(kc) + x.rad(head))) {
                                grab_in_beak(x, t.impale, i);
                                break;
                            }
                        }
                    }
                    // (another creature's exclusive grasp on it pulls it off the tusk)
                    if (t.impale && x.rng.value() < 0.05f) {
                        dcon::creature_id other_holder;
                        creature::for_each_holder(x.state, x.room.id, t.impale, [&](dcon::creature_id h, uint32_t) {
                            if (h != x.c)
                                other_holder = h;
                        });
                        if (other_holder) {
                            switch_mode(t, tusk_mode::dangling);
                            t.impale = dcon::creature_id{};
                        }
                    }
                }
            }
            if (t.attached == 0.f) {
                vec2 const end = (t.impale && impale_chunk(x, t, ic)) ? x.w().body_chunk_get_pos(ic) : t.chunk_points[1].pos;
                if (!t.has_rope && x.room.visual_contact(x.pos(head), end))
                    t.has_rope = 1;
                if (t.has_rope) {
                    float const total = rw::distance(x.pos(head), end);
                    if (total > t.curr_wire_length) {
                        t.wire_extra_slack = mathf::max(0.f, t.wire_extra_slack - 0.1f);
                        float const num7 = t.stuck;
                        t.stuck -= mathf::inverse_lerp(30.f, 90.f, float(t.mode_counter)) * x.rng.value() / custom::lerp_map(total / t.curr_wire_length, 1.f, 1.3f, 120.f, 10.f, 0.7f);
                        if (x.grasped())
                            t.stuck -= 0.1f;
                        (void)num7;
                        // (King_Vulture_Tusk_Out_Of_Terrain has no sample here)
                        float num8 = x.mass(head) / (0.1f + x.mass(head));
                        float const d = total - t.curr_wire_length;
                        if (t.mode == tusk_mode::stuck_in_wall) {
                            vec2 const v4 = custom::dir_vec(x.pos(head), end) * d;
                            x.pos(head) += v4 * t.elasticity;
                            x.vel(head) += v4 * t.elasticity;
                        } else if (t.mode == tusk_mode::stuck_in_creature && t.impale && impale_chunk(x, t, ic)) {
                            // the wire pulls the impaled creature to the head
                            float const im = x.w().body_chunk_get_mass(ic);
                            num8 = x.mass(head) / (im + x.mass(head));
                            vec2 const v4 = custom::dir_vec(x.pos(head), end) * d;
                            x.pos(head) += v4 * (1.f - num8) * t.elasticity;
                            x.vel(head) += v4 * (1.f - num8) * t.elasticity;
                            vec2 const v5 = custom::dir_vec(end, x.pos(head)) * d;
                            x.w().body_chunk_get_pos(ic) += v5 * num8 * t.elasticity;
                            x.w().body_chunk_get_vel(ic) += v5 * num8 * t.elasticity;
                        } else {
                            vec2 const v4 = custom::dir_vec(x.pos(head), end) * d;
                            x.pos(head) += v4 * (1.f - num8) * t.elasticity;
                            x.vel(head) += v4 * (1.f - num8) * t.elasticity;
                            vec2 const v5 = custom::dir_vec(t.chunk_points[1].pos, x.pos(head)) * d;
                            t.chunk_points[1].pos += v5 * num8 * t.elasticity;
                            t.chunk_points[1].vel += v5 * num8 * t.elasticity;
                        }
                    }
                }
                bool const stuck_any = t.mode == tusk_mode::stuck_in_creature || t.mode == tusk_mode::stuck_in_wall;
                float const neck_len = x.neck.t->ideal_length * 0.75f;
                if (stuck_any && !custom::dist_less(x.pos(head), x.pos(0), neck_len)) {
                    vec2 const v4 = custom::dir_vec(x.pos(head), x.pos(0)) * (rw::distance(x.pos(head), x.pos(0)) - neck_len);
                    float const num9 = x.mass(head) / (x.mass(0) + x.mass(head));
                    x.pos(head) += v4 * (1.f - num9);
                    x.vel(head) += v4 * (1.f - num9);
                    x.pos(0) -= v4 * num9;
                    x.vel(0) -= v4 * num9;
                }
            } else {
                t.has_rope = 0;
                if (x.viewed) {
                    auto g = rw::random(x.state.seed, x.state.tick, rw::ids(uint32_t(x.room.id.value), rw::ids(uint32_t(x.c.value), side)));
                    for (auto& p : t.wire)
                        p.pos = x.pos(head) + custom::rnv(g);
                }
            }
        }

        // KingTusks.Tusk.Reset
        void tusk_reset(ctx& x, tusk& t) {
            t.attached = 1.f;
            for (auto& cp : t.chunk_points) {
                cp.pos = x.pos(head) + custom::rnv(x.rng);
                cp.last_pos = cp.pos;
                cp.vel = vec2{0.f, 0.f};
            }
            t.has_rope = 0;
            for (auto& p : t.wire) {
                p.pos = x.pos(head) + custom::rnv(x.rng) * x.rng.value();
                p.last_pos = p.pos;
                p.vel = vec2{0.f, 0.f};
            }
            t.mode = tusk_mode::attached;
            t.impale = dcon::creature_id{};
            t.mode_counter = 0;
            t.wire_loose = 0.f;
            t.last_wire_loose = 0.f;
            t.wire_extra_slack = 0.f;
            t.elasticity = 0.9f;
        }
    }

    void tusks_new_room(ctx& x) {
        auto& k = tk(x);
        for (auto& t : k.t)
            tusk_reset(x, t);
        k.last_prey_pos = x.pos(0);
        k.prey_pos = x.pos(0);
        for (auto& p : k.target_trail)
            p = x.pos(0);
        k.prey_vel_estimate = vec2{0.f, 0.f};
        k.no_shoot_delay = 220;
        k.target = dcon::creature_id{};
    }

    void tusks_update(ctx& x) {
        auto& k = tk(x);
        k.last_eyes_out = k.eyes_out;
        k.eyes_out_cycle += 0.06666667f;
        k.eyes_out = (0.5f + 0.5f * std::sin(k.eyes_out_cycle)) * (1.f - k.eyes_home_in);
        k.last_eyes_home = k.eyes_home_in;
        k.last_prey_pos = k.prey_pos;
        if (k.no_shoot_delay > 0)
            --k.no_shoot_delay;
        k.last_head_rot = k.head_rot;
        k.head_rot = custom::aim(neck_tip(x), x.pos(head)) - custom::aim(x.pos(0), x.pos(1));
        for (uint32_t i = 0; i < 2; ++i)
            tusk_update(x, k.t[i], i);
        auto const* prey = most_attractive_prey(x);
        if (current(x) == behavior::hunt && x.awake()) {
            if (prey && (prey->visual || x.room.visual_contact(k.prey_pos, x.pos(head)))) {
                if (k.target == prey->who) {
                    k.eyes_home_in = mathf::min(1.f, k.eyes_home_in
                            + mathf::inverse_lerp(0.25f, 0.9f, rw::dot(rw::normalized(neck_tip(x) - x.pos(head)), rw::normalized(x.pos(head) - k.prey_pos))) / 40.f);
                } else {
                    k.eyes_home_in = mathf::max(0.f, k.eyes_home_in - 0.2f);
                    if (k.eyes_home_in == 0.f && k.last_eyes_home == 0.f)
                        k.target = prey->who;
                }
            } else {
                k.eyes_home_in = mathf::max(0.f, k.eyes_home_in - custom::lerp_map(float(prey ? prey->ticks_since_seen : 120), 60.f, 120.f, 0.f, 0.01f));
            }
        } else {
            k.eyes_home_in = custom::lerp_and_tick(k.eyes_home_in, 0.f, 0.07f, 0.025f);
        }
        auto const* tr = k.target ? tracker::find(tracker::of(x.state, x.c), k.target) : nullptr;
        if (tr && creature::here(x.state, k.target, x.room.id)) {
            vec2 v;
            if (tr->visual) {
                auto const mc = creature::chunk_of(x.state, k.target, creature::main_chunk_of(x.state, k.target));
                v = x.w().body_chunk_get_pos(mc);
                k.prey_vel_estimate = custom::move_towards(k.prey_vel_estimate, v - x.w().body_chunk_get_last_pos(mc), 0.1f);
            } else {
                v = room_view::middle_of_tile(tracker::best_guess(*tr));
                k.prey_vel_estimate *= 0.99f;
            }
            for (uint32_t j = target_trail_length - 1; j > 0; --j)
                k.target_trail[j] = k.target_trail[j - 1];
            k.target_trail[0] = v;
            vec2 const last = k.target_trail[target_trail_length - 1];
            k.prey_pos = last + k.prey_vel_estimate * float(target_trail_length) * custom::lerp_map(rw::distance(last, x.pos(head)), 100.f, 500.f, 1.f, 1.5f);
            bool const cq = close_quarters(x);
            if (k.eyes_home_in == 1.f && k.no_shoot_delay < 1 && x.awake() && custom::dist_less(k.prey_pos, x.pos(head), shoot_range) && !charging_snap(x)
                && k.t[0].mode != tusk_mode::charging && k.t[1].mode != tusk_mode::charging && tusks_want_to_shoot(x, true, !cq)
                && good_shoot_angle(x, -1, !cq) > (cq ? 0.3f : 0.7f))
                try_to_shoot(x);
        } else {
            k.target = dcon::creature_id{};
        }
    }

    bool tusks_hit_by_spear(ctx& x, vec2 momentum) {
        auto& k = tk(x);
        int32_t const num = x.rng.range(0, 2);
        auto& t = k.t[num];
        if (t.mode != tusk_mode::attached && t.mode != tusk_mode::charging)
            return false;
        switch_mode(t, tusk_mode::dangling);
        t.chunk_points[x.rng.range(0, 2)].vel += momentum * 3.3f;
        return true;
    }

    bool tusks_ready_to_shoot(ctx& x) {
        auto& k = tk(x);
        return ready(k.t[0]) || ready(k.t[1]);
    }

    bool tusks_want_to_shoot(ctx& x, bool check_visual, bool check_min_distance) {
        auto& k = tk(x);
        if (x.grasped())
            return false;
        behavior const b = current(x);
        if (b != behavior::hunt && b != behavior::idle)
            return false;
        if (!k.target || !creature::here(x.state, k.target, x.room.id))
            return false;
        for (auto const& t : k.t)
            if (t.impale == k.target)
                return false;
        if (check_min_distance && (custom::dist_less(k.prey_pos, x.pos(head), min_shoot_range) || custom::dist_less(k.prey_pos, x.pos(0), min_shoot_range))
            && target_reachable(x))
            return false;
        return (!check_visual || visual_on_any_target_chunk(x)) && dynamic_relationship(x, k.target).type == creature::rel::eats;
    }

    bool any_impaled(ctx& x) {
        auto& k = tk(x);
        return k.t[0].impale || k.t[1].impale;
    }

    bool impaled(ctx& x, dcon::creature_id t) {
        auto& k = tk(x);
        return t && (k.t[0].impale == t || k.t[1].impale == t);
    }

    void tusks_release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t) {
        auto& w = state.world;
        if (creature::vulture_data(w, c) == dcon::vulture_data_id{})
            return;
        auto& k = w.vulture_data_get_tusks(creature::vulture_data(w, c));
        for (auto& tu : k.t)
            if (tu.impale == t) {
                tu.impale = dcon::creature_id{};
                switch_mode(tu, tusk_mode::dangling);
            }
    }
}
