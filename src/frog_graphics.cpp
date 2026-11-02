// Watcher/FrogGraphics.cs: the frog's graphics module and its Umbilical.
// See frog_graphics.hpp.

#include <algorithm>
#include <cmath>
#include <string_view>

#include "frog_graphics.hpp"
#include "body_parts.hpp"
#include "creature.hpp"
#include "frog.hpp"
#include "frog_types.hpp"
#include "particles.hpp"
#include "physics.hpp"
#include "tracker.hpp"

namespace frog_graphics {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using world::room_view;
    using player::limb_mode;
    using sprites::sprite;
    using frog::graphics_state;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        // Limb(rad 0.5, surface friction 0.5, air friction 0.98, hunt speed 4, quickness 0.5)
        constexpr body_parts::limb_params limb{0.5f, 0.5f, 0.98f, 4.f, 0.5f};
        // relEyePos, relHipPos, relShoulderPos, legLength, armLength,
        // minStepDuration
        constexpr vec2 rel_eye_pos[2] = {{2.f, 4.f}, {4.5f, 0.f}};
        constexpr vec2 rel_hip_pos{1.5f, -1.f};
        constexpr vec2 rel_shoulder_pos{1.5f, 1.5f};
        constexpr float leg_length = 12.f;
        constexpr float arm_length = 8.f;
        constexpr int32_t min_step_duration = 5;
        // bloodColor, legBloodColor
        constexpr vec3 blood_color{0.09f, 0.36f, 0.03f};
        constexpr vec3 leg_blood_color{0.023529412f, 0.078431375f, 0.011764706f};
        // ConnectLimbSprite's elements: FrogLeg{side}{stretch}
        constexpr std::string_view leg_elements[2][5] = {
            {"FrogLeg00", "FrogLeg01", "FrogLeg02", "FrogLeg03", "FrogLeg04"},
            {"FrogLeg10", "FrogLeg11", "FrogLeg12", "FrogLeg13", "FrogLeg14"},
        };

        graphics_state& gs(sys::game_state& state, dcon::creature_id c) { return state.world.frog_data_get_graphics(creature::frog_data(state.world, c)); }
        graphics_state const& gs(sys::game_state const& state, dcon::creature_id c) {
            return state.world.frog_data_get_graphics(creature::frog_data(state.world, c));
        }
        creature::frog_payload& pl(sys::game_state& state, dcon::creature_id c) { return state.world.creature_get_payload(c).frog; }
        creature::frog_payload const& pl(sys::game_state const& state, dcon::creature_id c) { return state.world.creature_get_payload(c).frog; }
        vec2 cpos(sys::game_state const& state, dcon::creature_id c, uint32_t i) { return state.world.body_chunk_get_pos(frog::chunk(state, c, i)); }
        vec2 clast(sys::game_state const& state, dcon::creature_id c, uint32_t i) { return state.world.body_chunk_get_last_pos(frog::chunk(state, c, i)); }
        vec2 cvel(sys::game_state const& state, dcon::creature_id c, uint32_t i) { return state.world.body_chunk_get_vel(frog::chunk(state, c, i)); }
        vec2 lerp_chunk(sys::game_state const& state, dcon::creature_id c, uint32_t i, float t) { return rw::lerp(clast(state, c, i), cpos(state, c, i), t); }

        // Color.Lerp
        vec3 mix(vec3 a, vec3 b, float t) { return a + (b - a) * mathf::clamp01(t); }
        vec4 rgba(vec3 c, float a = 1.f) { return vec4{c.x, c.y, c.z, a}; }
        // Mathf.MoveTowards
        float move_towards(float a, float b, float d) { return std::fabs(b - a) <= d ? b : a + mathf::sign(b - a) * d; }
        // ScalableFeatures
        vec2 scalable(vec2 v, float s) { return rw::normalized(v) * s; }

        // GetLegsUp, GetArmsDir, GetHipPos, GetShoulderPos
        vec2 legs_up(sys::game_state const& state, dcon::creature_id c, graphics_state const& g, float t) {
            vec2 const a = lerp_chunk(state, c, 0, t), b = lerp_chunk(state, c, 1, t);
            return rw::slerp(rw::normalized(a - b), vec2{0.f, 1.f}, mathf::lerp(g.last_smoothed_sitting, g.smoothed_sitting, t));
        }
        vec2 arms_dir(sys::game_state const& state, dcon::creature_id c, graphics_state const& g, float t) {
            vec2 const a = lerp_chunk(state, c, 0, t), b = lerp_chunk(state, c, 1, t);
            return rw::slerp(rw::normalized(a - b), vec2{0.f, -1.f}, mathf::lerp(g.last_smoothed_sitting, g.smoothed_sitting, t) * 0.5f + 0.5f);
        }
        vec2 hip_pos(sys::game_state const& state, dcon::creature_id c, graphics_state const& g, int leg, float t) {
            vec2 const a = lerp_chunk(state, c, 1, t);
            vec2 const up = legs_up(state, c, g, t);
            vec2 const a2 = -custom::perpendicular_vector(up);
            float const d = float(leg) * 2.f - 1.f;
            return a + a2 * rel_hip_pos.x * d + up * rel_hip_pos.y;
        }
        vec2 shoulder_pos(sys::game_state const& state, dcon::creature_id c, graphics_state const& g, int leg, float t) {
            vec2 const a = lerp_chunk(state, c, 0, t);
            vec2 const up = legs_up(state, c, g, t);
            vec2 const a2 = -custom::perpendicular_vector(up);
            float const d = float(leg) * 2.f - 1.f;
            return a + a2 * rel_shoulder_pos.x * d + up * rel_shoulder_pos.y;
        }

        // Limb.Update (with its `retract`) on the chunk it is connected to:
        // the legs (0, 1) on the head, the arms (2, 3) on the rump, like
        // vanilla's constructor has them
        void limb_tick(sys::game_state const& state, dcon::creature_id c, player::hand_state& h, int index, bool retract, room_view const& room) {
            uint32_t const k = index < 2 ? 0u : 1u;
            vec2 const conn = cpos(state, c, k);
            if (retract && h.mode != limb_mode::retracted) {
                h.mode = limb_mode::hunt_absolute_position;
                h.absolute_hunt_pos = conn;
                if (custom::dist_less(h.absolute_hunt_pos, h.pos, h.hunt_speed))
                    h.mode = limb_mode::retracted;
            }
            body_parts::limb_update(h, limb, conn, cvel(state, c, k), cpos(state, c, 1 - k), room);
        }
        // BodyPart.ConnectToPoint(pnt, rad, false, 0.1, hostVel, 0.2, 0)
        void connect(player::hand_state& h, vec2 pnt, float rad, vec2 host_vel) {
            body_parts::as_body_part(h, limb.rad, [&](player::body_part& bp) { body_parts::connect_to_point(bp, pnt, rad, false, 0.1f, host_vel, 0.2f, 0.f); });
        }

        // FrogGraphics.StartStep
        void start_step(sys::game_state const& state, dcon::creature_id c, graphics_state& g, int leg, room_view const& room) {
            auto const& x = pl(state, c);
            g.walk_counter = 0;
            g.moving_leg = leg;
            float const d = float(leg) * 2.f - 1.f;
            vec2 const p0 = cpos(state, c, 0), p1 = cpos(state, c, 1);
            vec2 const n = rw::normalized(p0 - p1);
            vec2 const a = -custom::perpendicular_vector(n);
            vec2 const mv = cvel(state, c, 0);
            int const fx = -int(mathf::sign(mv.x));
            int const fy = -int(mathf::sign(mv.y));
            vec2 attached = hip_pos(state, c, g, leg, 1.f);
            vec2 goal = p1 + a * 5.f * d + x.walk_dir * 10.f;
            body_parts::find_grip(g.limbs[leg], room, attached, p1, leg_length * 3.f, goal, fx, fy, true);
            attached = shoulder_pos(state, c, g, leg, 1.f);
            goal = p0 + n * 10.f + a * 5.f * d + x.walk_dir * 10.f;
            // (vanilla's feedSize / maxFeeds is an integer division)
            float const reach = (int32_t(x.feed_size) / frog::max_feeds) >= 1 ? 2.f : 0.f;
            body_parts::find_grip(g.limbs[3 - leg], room, attached, p0, arm_length * (3.f + reach), goal, fx, fy, true);
        }

        // FrogGraphics.StartKick
        void start_kick(sys::game_state const& state, dcon::creature_id c, graphics_state& g, int leg, rw::rng& rng) {
            vec2 const n = rw::normalized(cpos(state, c, 0) - cpos(state, c, 1));
            g.kick[leg] = body_parts::rotate_around_origo(n, mathf::lerp(10.f, 30.f, rng.value()) * (float(leg) * 2.f - 1.f));
        }

        // FrogGraphics.DoneWithJump: the limbs set down where it landed
        void done_with_jump(sys::game_state const& state, dcon::creature_id c, graphics_state& g, room_view const& room) {
            vec2 const up = legs_up(state, c, g, 1.f);
            vec2 const ad = arms_dir(state, c, g, 1.f);
            for (int i = 0; i < 2; ++i) {
                vec2 const shoulder = shoulder_pos(state, c, g, i, 1.f);
                vec2 const hip = hip_pos(state, c, g, i, 1.f);
                auto& lg = g.limbs[i];
                lg.mode = limb_mode::hunt_absolute_position;
                lg.absolute_hunt_pos = hip - up * 8.f;
                limb_tick(state, c, lg, i, false, room);
                connect(lg, hip, leg_length, cvel(state, c, 1));
                start_step(state, c, g, i, room);
                auto& arm = g.limbs[i + 2];
                arm.mode = limb_mode::hunt_absolute_position;
                arm.absolute_hunt_pos = shoulder + ad * 8.f;
                limb_tick(state, c, arm, i + 2, false, room);
                connect(arm, shoulder, arm_length, cvel(state, c, 0));
            }
        }

        // FrogAI.CreatureUnease (no water terms)
        float creature_unease(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) {
            if (creature::dead(state, t))
                return 0.f;
            auto const tt = creature::template_of(state, t);
            auto const rel = creature::static_relationship(creature::template_of(state, c), tt);
            if (rel.type == creature::rel::ignores)
                return 0.f;
            float num = creature::body_size(tt) * mathf::lerp(rel.intensity, 1.f, 0.5f);
            if (!creature::consious(state, t))
                num /= 20.f;
            return num;
        }

        // CreatureLooker.Update (chanceToLookAtNothing 0, 30 frames between
        // focus changes) and ReevaluateLookObject with FrogGraphics'
        // CreatureInterestBonus
        void looker_update(sys::game_state& state, dcon::creature_id c, graphics_state& g, rw::rng& rng) {
            auto const& w = state.world;
            auto const& ts = tracker::of(state, c);
            if (g.look_creature && !tracker::find(ts, g.look_creature))
                g.look_creature = dcon::creature_id{};
            if (g.look_focus_delay > 0)
                --g.look_focus_delay;
            if (g.look_focus_delay >= 1 || rng.value() >= 1.f / 30.f)
                return;
            g.look_focus_delay = 30;
            g.look_creature = dcon::creature_id{};
            float best = 0.f;
            bool const flag = rng.value() < 0.5f;
            ivec2 const at = room_view::get_tile_position(cpos(state, c, 0));
            auto const me = creature::template_of(state, c);
            auto const held = creature::grasped(w, c, 0);
            for (uint32_t i = 0; i < ts.count; ++i) {
                auto const& r = ts.reps[i];
                if (!creature::live(state, r.who))
                    continue;
                auto const tt = creature::template_of(state, r.who);
                auto const rel = creature::static_relationship(me, tt);
                float num2 = 1.f + mathf::lerp(creature::body_size(tt), 1.f, 0.5f) * rel.intensity * tracker::chance_of_finding(r, 10);
                if (flag)
                    num2 += rng.value();
                if (r.visual && creature::here(state, r.who, creature::room_of(state, c)))
                    num2 *= 1.f + rw::magnitude(w.body_chunk_get_vel(creature::chunk_of(state, r.who, creature::main_chunk_of(state, r.who))));
                num2 /= mathf::lerp(rw::magnitude(rw::to_vec2(tracker::best_guess(r) - at)), 1.f, 0.6f);
                if (rel.type == creature::rel::ignores)
                    num2 /= 2.f;
                num2 *= 1.f + creature_unease(state, c, r.who);
                if (held == r.who)
                    num2 = -3.4e38f;
                if (num2 > best) {
                    best = num2;
                    g.look_creature = r.who;
                }
            }
        }

        // FrogGraphics.Umbilical's constructor (MakeUmbilical)
        void make_umbilical(sys::game_state const& state, dcon::creature_id c, graphics_state& g, dcon::creature_id host, rw::rng& rng) {
            auto const& w = state.world;
            uint32_t const n = creature::chunk_count_of(state, host);
            g.has_umbilical = 1;
            g.umbilical_host = host;
            g.umbilical_count = uint8_t(rng.range(5, 8));
            vec2 const shoot = cvel(state, c, 0);
            for (uint32_t i = 0; i < g.umbilical_count; ++i) {
                // (Random.Range(0, Length - 1), rolled for every point, the
                // last one kept)
                g.umbilical_chunk = uint8_t(rng.range(0, int32_t(n) - 1));
                vec2 const hp = w.body_chunk_get_pos(creature::chunk_of(state, host, g.umbilical_chunk));
                auto& s = g.umbilical[i];
                s.pos = hp + custom::rnv(rng);
                s.last_pos = hp + custom::rnv(rng);
                s.vel = shoot * 0.3f * rng.value() + custom::rnv(rng) * rng.value() * 1.5f;
                s.life = 2.f;
                s.life_time = mathf::lerp(150.f, 200.f, mathf::pow(rng.value(), 0.3f));
            }
        }

        float life_of(graphics_state const& g, uint32_t i) {
            if (i > 0)
                return mathf::min(g.umbilical[i].life, g.umbilical[i - 1].life);
            return g.umbilical[i].life;
        }

        // FrogGraphics.Umbilical.Update
        void umbilical_update(sys::game_state& state, dcon::creature_id c, graphics_state& g, room_view const& room) {
            auto const& w = state.world;
            uint32_t const n = g.umbilical_count;
            if (n < 4) {
                g.has_umbilical = 0;
                return;
            }
            auto const host = g.umbilical_host;
            bool const host_here = creature::live(state, host) && creature::here(state, host, room.id);
            bool const kill = !creature::grasped_object(w, c, 0) || w.creature_get_dead(c) != 0;
            bool flag = true;
            for (uint32_t i = 0; i < n; ++i) {
                auto& s = g.umbilical[i];
                s.last_pos = s.pos;
                s.pos += s.vel;
                s.vel *= custom::lerp_map(rw::magnitude(s.vel), 1.f, 30.f, 0.99f, 0.8f);
                s.vel.y -= mathf::lerp(0.1f, 0.6f, life_of(g, i));
                if (life_of(g, i) <= 0.f)
                    continue;
                if (room.get_tile(s.pos).solid()) {
                    player::body_part bp;
                    bp.pos = s.pos;
                    bp.last_pos = s.last_pos;
                    bp.vel = s.vel;
                    bp.rad = 1.f;
                    body_parts::push_out_of_terrain(bp, room, 1.f);
                    s.pos = bp.pos;
                    s.vel = bp.vel;
                }
                if (i > 0) {
                    auto& p = g.umbilical[i - 1];
                    if (!custom::dist_less(s.pos, p.pos, 6.f)) {
                        vec2 const a = custom::dir_vec(s.pos, p.pos) * (rw::distance(s.pos, p.pos) - 6.f);
                        s.pos += a * 0.15f;
                        s.vel += a * 0.25f;
                        p.pos -= a * 0.15f;
                        p.vel -= a * 0.25f;
                    }
                    if (!room.visual_contact(s.pos, p.pos) && kill)
                        s.life -= 0.2f;
                }
                if (i > 1 && life_of(g, i - 1) > 0.f) {
                    vec2 const d = custom::dir_vec(g.umbilical[i - 2].pos, s.pos) * 0.6f;
                    s.vel += d;
                    g.umbilical[i - 2].vel -= d;
                }
                if (kill)
                    s.life -= 1.f / s.life_time;
                if (s.life > 0.f)
                    flag = false;
            }
            if (life_of(g, 0) > 0.f) {
                if (host_here) {
                    uint32_t const k = g.umbilical_chunk < creature::chunk_count_of(state, host) ? g.umbilical_chunk : 0u;
                    vec2 const hp = w.body_chunk_get_pos(creature::chunk_of(state, host, k));
                    g.umbilical[0].pos = hp;
                    g.umbilical[0].vel = vec2{0.f, 0.f};
                    // (vanilla's DirVec(pos, pos * 3) and DirVec(pos, pos * 1.5))
                    g.umbilical[1].vel += custom::dir_vec(hp, hp * 3.f);
                    g.umbilical[3].vel += custom::dir_vec(hp, hp * 1.5f);
                }
                if (life_of(g, 1) <= 0.f || !host_here || w.creature_get_entering_shortcut(host).x >= 0)
                    g.umbilical[0].life -= 1.f;
            }
            if (life_of(g, n - 1) > 0.f) {
                vec2 const rot = rw::normalized(cpos(state, c, 0) - cpos(state, c, 1));
                g.umbilical[n - 1].pos = cpos(state, c, 0);
                g.umbilical[n - 1].vel = cvel(state, c, 0);
                g.umbilical[n - 2].vel += rot * -2.f + vec2{0.f, -1.5f};
                g.umbilical[n - 3].vel += rot * -1.f + vec2{0.f, -1.f};
                if (creature::room_of(state, c) != room.id)
                    g.umbilical[n - 1].life -= 1.f;
            }
            if (flag)
                g.has_umbilical = 0;
        }
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        auto& g = gs(state, c);
        vec2 const main = cpos(state, c, 0);
        for (auto& lg : g.limbs) {
            lg = player::hand_state{};
            lg.pos = main;
            lg.last_pos = main;
            lg.vel = vec2{0.f, 0.f};
            lg.mode = limb_mode::hunt_relative_position;
            lg.hunt_speed = limb.hunt_speed;
            lg.quickness = limb.quickness;
            lg.absolute_hunt_pos = main;
        }
        g.kick[0] = g.kick[1] = vec2{0.f, 0.f};
        g.last_roll = g.roll;
        g.last_smoothed_sitting = g.smoothed_sitting;
        g.look_creature = dcon::creature_id{};
        g.look_focus_delay = 0;
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto& g = gs(state, c);
        auto& x = pl(state, c);
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        bool const awake = creature::consious(state, c);
        auto const host = frog::attached_to(state, c);

        // what the simulation asked of the module: DoneWithJump, MakeUmbilical
        if (x.done_with_jump) {
            x.done_with_jump = 0;
            done_with_jump(state, c, g, room);
        }
        if (host && (x.make_new_umbilical || !g.has_umbilical || g.umbilical_host != host) && !w.creature_get_dead(c)) {
            x.make_new_umbilical = 0;
            make_umbilical(state, c, g, host, rng);
        }
        if (g.has_umbilical)
            umbilical_update(state, c, g, room);

        looker_update(state, c, g, rng);
        g.last_smoothed_sitting = g.smoothed_sitting;
        g.smoothed_sitting = move_towards(g.smoothed_sitting, x.sitting, 0.4f);
        vec2 const up = legs_up(state, c, g, 1.f);
        vec2 const a = -custom::perpendicular_vector(up);
        vec2 const p0 = cpos(state, c, 0), p1 = cpos(state, c, 1);
        vec2 const n = rw::normalized(p0 - p1);
        g.last_roll = g.roll;
        float const to = (host || !awake) ? 0.f : mathf::clamp(n.x * 2.f, -1.f, 1.f);
        g.roll = custom::lerp_and_tick(g.roll, to, 0.1f, 0.05f);
        if (x.walking)
            ++g.walk_counter;
        float const num = mathf::min(float(x.time_attached) / 120.f, 1.f);
        for (int i = 0; i < 2; ++i) {
            if (num == 1.f) {
                if (x.kick_until_detached) {
                    ++g.kick_counter;
                    if (rng.value() < 0.075f) {
                        start_kick(state, c, g, i, rng);
                        // WaterDrip
                        vec2 const at = rw::lerp(p1, p0, rng.value());
                        particles::spawn(state, room.id, particles::kind::drop, at, vec2{mathf::lerp(-3.f, 3.f, rng.value()), 5.f}, 1.5f, 0.75f,
                            vec3{0.6f, 0.65f, 0.7f}, 0.6f);
                    }
                } else if (rng.value() < 0.002f) {
                    start_kick(state, c, g, i, rng);
                }
            } else if (num > 0.f && rw::magnitude(g.kick[i]) < 0.5f && rng.value() < (1.f - num) * 0.1f) {
                start_kick(state, c, g, i, rng);
            }
            g.kick[i] = custom::move_towards(g.kick[i], vec2{0.f, 0.f}, 0.2f);
        }
        // the legs, on the hips
        for (int j = 0; j < 2; ++j) {
            float const d = float(j) * 2.f - 1.f;
            auto& lg = g.limbs[j];
            vec2 const hip = hip_pos(state, c, g, j, 1.f);
            if (host) {
                lg.mode = limb_mode::dangle;
                lg.vel += up * -1.f + a * d * 0.3f + g.kick[j] * 3.f;
            } else if (x.walking) {
                if (g.moving_leg != j && !custom::dist_less(lg.absolute_hunt_pos, hip, leg_length * 0.9f) && g.walk_counter > min_step_duration)
                    start_step(state, c, g, j, room);
            } else if (!awake) {
                lg.mode = limb_mode::dangle;
            } else if (g.smoothed_sitting == 0.f) {
                lg.mode = limb_mode::dangle;
                lg.vel += a * 0.6f + up * -3.f;
            } else {
                lg.mode = limb_mode::hunt_absolute_position;
                lg.absolute_hunt_pos = hip - up * 8.f;
            }
            limb_tick(state, c, lg, j, false, room);
            connect(lg, hip, leg_length, cvel(state, c, 1));
        }
        // the arms, on the shoulders
        vec2 const ad = arms_dir(state, c, g, 1.f);
        for (int k = 0; k < 2; ++k) {
            float const d2 = float(k) * 2.f - 1.f;
            auto& arm = g.limbs[k + 2];
            vec2 const shoulder = shoulder_pos(state, c, g, k, 1.f);
            bool retract = false;
            if (!x.walking) {
                if (host) {
                    retract = true;
                } else if (!awake) {
                    arm.mode = limb_mode::dangle;
                } else if (g.smoothed_sitting == 0.f) {
                    arm.mode = limb_mode::dangle;
                    arm.vel += a * d2 * 0.6f + ad * 3.f;
                } else {
                    arm.mode = limb_mode::hunt_absolute_position;
                    arm.absolute_hunt_pos = shoulder + ad * 8.f;
                }
            }
            limb_tick(state, c, arm, k + 2, retract, room);
            connect(arm, shoulder, arm_length, cvel(state, c, 0));
        }
        float const sub = physics::object_submersion(state, room, frog::body(state, c));
        for (auto& lg : g.limbs)
            if (lg.mode == limb_mode::dangle)
                lg.vel.y -= 0.5f * (1.f - sub);
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>&, std::vector<sprites::rope_point>& ropes) {
        auto const& w = state.world;
        auto const& g = gs(state, c);
        auto const& x = pl(state, c);
        auto const d = creature::frog_data(w, c);
        auto const& lk = w.frog_data_get_looks(d);
        auto const& m = w.frog_data_get_move(d);
        float const t = alpha;
        bool const awake = creature::consious(state, c);
        auto const host = frog::attached_to(state, c);

        vec2 const vector = lerp_chunk(state, c, 0, t);
        vec2 vector2 = lerp_chunk(state, c, 1, t);
        vector2 += custom::perpendicular_vector(vector, vector2) * 2.f;
        vec2 const normalized = rw::normalized(vector - vector2);
        vec2 const vector3 = -custom::perpendicular_vector(normalized);
        float const rotation = custom::vec_to_deg(normalized);
        float const num = mathf::lerp(g.last_roll, g.roll, t);
        float const bloat = frog::blood_bank_bloat(state, c);

        // ApplyPalette (the darkness isn't lowered by light sources)
        float const darkness = pal.darkness();
        vec3 const black = pal.black();
        float const feed = float(x.feed_size) / float(frog::max_feeds);
        float const poison = mathf::clamp01(m.injected_poison) * 0.4f;
        vec3 const color = mix(mix(lk.body, black, darkness), m.poison_color, poison);
        vec3 const color2 = mix(mix(mix(lk.belly, blood_color, feed), black, darkness), m.poison_color, poison);
        vec3 const limb_color = mix(color, leg_blood_color, float(x.feed_size) / (float(frog::max_feeds) * 1.5f));

        // the sprites by their vanilla index, drawn in that order: horns,
        // legs, arms, body, eyes
        int const horns = std::max(0, std::min(lk.horns, 3));
        int const first_horn = 0;
        int const first_leg = horns * 2;
        int const first_arm = first_leg + 2;
        int const first_body = first_arm + 2;
        int const first_eye = first_body + 4;
        int const total = first_eye + 8;
        constexpr int max_sprites = 6 + 2 + 2 + 4 + 8;
        sprites::draw_cmd slot[max_sprites];
        bool visible[max_sprites] = {};
        auto const put = [&](int i, sprite const& s) {
            slot[i] = sprites::draw_cmd{};
            slot[i].spr = s;
            visible[i] = true;
        };
        auto const eye_sprite = [&](int side, int index, int part) { return first_eye + part * 4 + index * 2 + side; };

        bool const whole = x.bites > 2;
        // the body
        if (whole) {
            put(first_body, sprite{"Circle20", vector + scalable(normalized, bloat) + normalized * 1.f, rotation, vec2{0.4f, 0.55f}, vec2{0.5f, 0.5f}, color});
            put(first_body + 1, sprite{"Circle20", vector2 + normalized * 2.f, rotation, vec2{0.45f, 0.55f}, vec2{0.5f, 0.5f}, color});
            float const croaking = frog::croaking(state, c);
            float const num2 = croaking * (1.f - mathf::pow(croaking, 4.f));
            vec2 const mid = (vector + vector2) / 2.f;
            float const s2 = (1.f + bloat + num2 * 0.5f) * 4.f / 20.f;
            put(first_body + 2, sprite{"Circle20", mid + vector3 * 1.5f * num - scalable(normalized, bloat), 0.f, vec2{s2, s2}, vec2{0.5f, 0.5f}, color2});
            float const s3 = (1.f + bloat + num2 * 2.f) * 5.f / 20.f;
            put(first_body + 3,
                sprite{"Circle20", mid + vector3 * (2.5f + num2 * 5.f) * num - scalable(normalized, bloat) + normalized * 3.f, 0.f, vec2{s3, s3}, vec2{0.5f, 0.5f}, color2});
        }
        // the eyes, looking at what its CreatureLooker looks at
        vec2 look{0.f, 0.f};
        if (awake && g.look_creature) {
            if (auto const* r = tracker::find(tracker::of(state, c), g.look_creature)) {
                if (r->visual && creature::here(state, r->who, creature::room_of(state, c)))
                    look = rw::normalized(creature::main_pos(state, r->who) - vector);
                else
                    look = rw::normalized(room_view::middle_of_tile(tracker::best_guess(*r)) - vector);
            }
        }
        if (x.bites >= 3) {
            vec3 const dim = mix(lk.body, black, darkness * 0.6f + 0.4f);
            bool const flag = w.creature_get_stun(c) > 0 || !awake || host;
            for (int i = 0; i < 2; ++i) {
                float const num3 = float(i) * 2.f - 1.f + num * 0.5f;
                float const num4 = mathf::inverse_lerp(1.2f, 1.f, std::fabs(num3));
                float const rotation2 = num4 < 1.f ? custom::vec_to_deg(normalized) : 0.f;
                for (int j = 0; j < 2; ++j) {
                    vec2 const rel = rel_eye_pos[j];
                    vec2 const a = vector + scalable(vector3 * rel.x * num3, bloat / 3.f) + vector3 * rel.x * num3 + scalable(normalized * rel.y, bloat) + normalized * rel.y;
                    put(eye_sprite(i, j, 0), sprite{j == 0 ? "tinyStar" : "Circle4", a, rotation2, vec2{num4, 1.f}, vec2{0.5f, 0.5f}, flag ? dim : black});
                    put(eye_sprite(i, j, 1), sprite{"pixel", a + (j == 1 ? look : vec2{0.f, 0.f}), rotation2, vec2{j == 1 ? 2.f * num4 : num4, j == 1 ? 2.f : 1.f},
                                                     vec2{0.5f, 0.5f}, flag ? dim : lk.eye});
                }
            }
        }
        // ConnectLimbSprite
        auto const limb_sprite = [&](vec2 from, vec2 to, float scale, float side, vec3 col) {
            int const n1 = custom::int_clamp(std::abs(int32_t(std::nearbyint(side * 2.f))), 0, 1);
            int const n2 = custom::int_clamp(int32_t(std::nearbyint(rw::distance(from, to) / 2.f / scale - 2.f)), 0, 4);
            return sprite{leg_elements[n1][n2], from, custom::vec_to_deg(to - from) - 90.f, vec2{scale, side < 0.f ? scale : -scale}, vec2{0.5f, 0.5f}, col};
        };
        // the legs (sprite.scale then sets both scales, the flip with them)
        for (int k = 0; k < 2; ++k) {
            float const side = float(k) * 2.f - 1.f;
            vec2 const hip = hip_pos(state, c, g, k, t);
            vec2 const to = rw::lerp(g.limbs[k].last_pos, g.limbs[k].pos, t);
            sprite s = limb_sprite(hip, to, 0.9f, side, limb_color);
            float const sc = mathf::lerp(1.f, 1.15f, mathf::inverse_lerp(0.f, 4.f, bloat));
            s.scale = vec2{sc, sc};
            put(first_leg + k, s);
        }
        // the arms
        for (int l = 0; l < 2; ++l) {
            float const num5 = float(l) * 2.f - 1.f;
            vec2 const shoulder = shoulder_pos(state, c, g, l, t);
            vec2 const to2 = rw::lerp(g.limbs[l + 2].last_pos, g.limbs[l + 2].pos, t);
            put(first_arm + l, limb_sprite(shoulder, to2, 0.6f, -num5, limb_color));
        }
        // the horns
        if (x.bites >= 3) {
            for (int mm = 0; mm < horns; ++mm) {
                vec2 const a2 = vector + scalable(normalized * (4.f - float(mm) * 2.f), bloat) + normalized * (4.f - float(mm) * 2.f) - vector3 * num * 3.f;
                float const rot = custom::vec_to_deg(vector3);
                vec2 const sc{lk.horn_scale * 0.4f, -num * lk.horn_scale * 0.35f};
                put(first_horn + mm * 2, sprite{"LizardScaleA0", a2, rot, sc, vec2{0.5f, 0.f}, color});
                put(first_horn + mm * 2 + 1, sprite{"LizardScaleB0", a2, rot, sc, vec2{0.5f, 0.f}, color2});
            }
        }
        for (int i = 0; i < total; ++i)
            if (visible[i])
                out.push_back(slot[i]);

        // the umbilical (Umbilical.DrawSprites: a rope through its points,
        // each as wide as the life left in it)
        uint32_t const n = g.umbilical_count;
        if (!g.has_umbilical || n < 4)
            return;
        vec3 const fog = pal.fog();
        vec3 const thread = mix(vec3{0.95f, 0.8f, 0.55f}, fog, 0.2f);
        bool const dark = darkness > 0.2f;
        bool const host_ok = g.umbilical_host && creature::live(state, g.umbilical_host) && creature::here(state, g.umbilical_host, creature::room_of(state, c));
        vec2 const rot = rw::normalized(cpos(state, c, 0) - cpos(state, c, 1));
        size_t const first = ropes.size();
        float b = 1.f;
        for (uint32_t i = 0; i < n; ++i) {
            float const f = mathf::inverse_lerp(0.f, float(n - 1), float(i));
            float num2 = life_of(g, i);
            if (i < n - 1)
                num2 = mathf::min(num2, life_of(g, i + 1));
            float const num3 = 0.5f * mathf::inverse_lerp(0.f, 0.3f, num2);
            vec2 v2 = rw::lerp(g.umbilical[i].last_pos, g.umbilical[i].pos, t);
            if (i == 0 && life_of(g, 0) > 0.f && host_ok) {
                uint32_t const k = g.umbilical_chunk < creature::chunk_count_of(state, g.umbilical_host) ? g.umbilical_chunk : 0u;
                auto const hk = creature::chunk_of(state, g.umbilical_host, k);
                v2 = rw::lerp(w.body_chunk_get_last_pos(hk), w.body_chunk_get_pos(hk), t);
            } else if (i == n - 1 && life_of(g, i) > 0.f) {
                v2 = lerp_chunk(state, c, 0, t) - rot * 5.f;
            }
            vec3 col = mix(fog, mix(vec3{1.f, 0.f, 0.f}, thread, 0.1f + 0.9f * mathf::pow(f, 0.25f + num2)), mathf::min(num2, b));
            if (dark && num2 > 0.f)
                col = mix(col, black, darkness);
            if (i == 0)
                col = mix(fog, black, life_of(g, 0));
            ropes.push_back(sprites::rope_point{v2, num3, rgba(col)});
            b = num2;
        }
        sprites::add_rope(out, ropes, first);
    }
}
