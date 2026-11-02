// BigSpiderGraphics.cs (big_spider_graphics.hpp)

#include <algorithm>
#include <cmath>
#include <string_view>

#include "big_spider_graphics.hpp"
#include "big_spider.hpp"
#include "body_parts.hpp"
#include "creature.hpp"
#include "pathing.hpp"
#include "vulture_internal.hpp"

namespace big_spider_graphics {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using world::room_view;
    using player::limb_mode;
    using sprites::sprite;
    using big_spider::graphics_state;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        // the elements' sourcePixelSize.y: CentipedeLegA, CentipedeLegB,
        // LizardScaleA1 (as the other ports have them), SpiderLeg{0..3}A and
        // B (assumed: check them against the atlas)
        constexpr float centipede_leg_a_h = 27.f;
        constexpr float centipede_leg_b_h = 25.f;
        constexpr float scale_h = 14.f;
        constexpr float spider_leg_a_h[4] = {25.f, 25.f, 25.f, 25.f};
        constexpr float spider_leg_b_h[4] = {25.f, 25.f, 25.f, 25.f};
        constexpr std::string_view spider_leg_a[4] = {"SpiderLeg0A", "SpiderLeg1A", "SpiderLeg2A", "SpiderLeg3A"};
        constexpr std::string_view spider_leg_b[4] = {"SpiderLeg0B", "SpiderLeg1B", "SpiderLeg2B", "SpiderLeg3B"};

        graphics_state& gs(sys::game_state& state, dcon::creature_id c) {
            return state.world.big_spider_data_get_graphics(creature::big_spider_data(state.world, c));
        }
        graphics_state const& gs(sys::game_state const& state, dcon::creature_id c) {
            return state.world.big_spider_data_get_graphics(creature::big_spider_data(state.world, c));
        }
        creature::big_spider_payload const& pl(sys::game_state const& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).big_spider;
        }
        vec2 cpos(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_pos(creature::chunk(state, c, i));
        }
        vec2 clast(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_last_pos(creature::chunk(state, c, i));
        }
        vec2 cvel(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_vel(creature::chunk(state, c, i));
        }
        vec3 mix(vec3 a, vec3 b, float t) {
            return a + (b - a) * mathf::clamp01(t);
        }
        vec4 rgba(vec3 c) {
            return vec4{c.x, c.y, c.z, 1.f};
        }
        // Limb(rad 0.1, surface friction 0.7, air friction 0.99, hunt speed
        // 18 (the spitter's 12), quickness 0.95)
        body_parts::limb_params leg_params(bool spitter) {
            return body_parts::limb_params{0.1f, 0.7f, 0.99f, spitter ? 12.f : 18.f, 0.95f};
        }

        // ScaleAttachPos (positions now: timeStacker 1)
        vec2 scale_attach_pos(graphics_state const& g, bool spitter, vec2 c0, vec2 c1, float mandibles, uint32_t scl, rw::rng& rng) {
            auto const& sc = g.scales[scl];
            float const num = g.flip * (spitter ? 0.25f : 1.f);
            float const num2 = mathf::inverse_lerp(spitter ? 0.2f : 0.5f, 0.f, sc.stuck_pos.y);
            vec2 v = c1 + custom::rnv(rng) * rng.value() * 3.5f * mandibles;
            if (spitter)
                v = rw::lerp(v, c0, mathf::inverse_lerp(0.5f, 1.f, sc.stuck_pos.y) * 0.7f);
            vec2 const v2 = custom::dir_vec(v, c0);
            float const s = mathf::clamp01(std::sin(sc.stuck_pos.y * mathf::pi));
            return rw::lerp(v, g.tail_end.pos, num2 * 0.6f) + custom::perpendicular_vector(v2) * (sc.stuck_pos.x - 0.5f * num) * s * s * (spitter ? 14.f : 7.f)
                + v2 * mathf::lerp(-1.f, 1.f, sc.stuck_pos.y) * (spitter ? 14.f : 7.f) * mathf::pow(1.f - num2, 2.f);
        }
        // ScaleDir
        vec2 scale_dir(graphics_state const& g, bool spitter, vec2 c0, vec2 c1, uint32_t scl, rw::rng& rng) {
            auto const& sc = g.scales[scl];
            vec2 v = c1 + custom::rnv(rng) * rng.value() * 3.5f * g.mandibles_charge;
            if (spitter)
                v = rw::lerp(v, c0, mathf::inverse_lerp(0.5f, 1.f, sc.stuck_pos.y) * 0.7f);
            vec2 v2 = custom::dir_vec(v, c0);
            float const t = mathf::inverse_lerp(spitter ? 0.2f : 0.5f, 0.f, sc.stuck_pos.y);
            v2 = rw::slerp(v2, custom::dir_vec(g.tail_end.pos, v), t);
            vec2 const p = scale_attach_pos(g, spitter, c0, c1, g.mandibles_charge, scl, rng);
            vec2 v3 = rw::normalized((custom::dir_vec(v, p) - v2 * (1.f - g.mandibles_charge)) / 2.f - g.spikes_dir * sc.stuck_pos.y);
            if (spitter)
                return v3;
            float const deg = custom::vec_to_deg(v2);
            v3 = body_parts::rotate_around_origo(v3, deg);
            v3.x *= 1.f + 3.f * sc.stuck_pos.y * (0.5f + 0.5f * std::sin(g.breath_counter / 10.f)) + 3.f * g.mandibles_charge;
            v3 = body_parts::rotate_around_origo(v3, -deg);
            return rw::normalized(v3);
        }
    }

    void init(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        auto& g = gs(state, c);
        g = graphics_state{};
        auto const v = big_spider::variant_of(state, c);
        bool const spitter = v == big_spider::variant::spitter;
        bool const mother = v == big_spider::variant::mother;
        g.leg_length = mother ? 90.f : spitter ? 65.f : 55.f;
        rw::unity_random r{w.big_spider_data_get_seed(creature::big_spider_data(w, c))};
        int32_t const inner = r.range(20, 28);
        int32_t n = (spitter ? 10 : 0) + r.range(spitter ? 16 : 10, inner);
        n = std::min<int32_t>(n, int32_t(big_spider::max_scales));
        g.scale_count = uint8_t(n);
        g.legs_thickness = mathf::lerp(0.7f, 1.1f, r.value());
        g.body_thickness = mathf::lerp(0.9f, 1.1f, r.value()) + (spitter ? 1.7f : 0.9f);
        if (mother)
            g.body_thickness = mathf::lerp(0.9f, 1.1f, r.value()) + 5.f;
        if (r.value() < 0.5f) {
            int32_t const x = r.range(0, 2);
            int32_t const y = r.range(0, 4);
            g.dead_leg = ivec2{x, y};
        }
        uint32_t first = 0;
        for (int32_t i = 0; i < n; ++i) {
            auto& sc = g.scales[i];
            int32_t count;
            if (spitter) {
                float const a = 0.5f + 0.5f * r.value();
                float const b = mathf::lerp(3.f, 11.f, r.value());
                sc.spec0 = vec2{a, b};
                sc.spec1 = custom::rnv(r);
                int32_t const in = r.range(3, 5);
                count = r.range(2, in);
                float const sx = mathf::lerp(-0.5f, 0.5f, r.value());
                float const sv = r.value();
                sc.stuck_pos = vec2{sx, mathf::lerp(sv, mathf::inverse_lerp(0.f, float(n - 1), float(i)), 0.5f)};
            } else {
                sc.spec0 = vec2{r.value(), 5.f};
                int32_t const in = i % 3 == 0 ? r.range(7, 13) : r.range(4, 5);
                count = r.range(2, in);
                float const sx = mathf::lerp(-0.5f, 0.5f, r.value());
                float const sv = r.value();
                sc.stuck_pos = vec2{sx, mathf::pow(sv, custom::lerp_map(float(count), 2.f, 9.f, 0.5f, 2.f))};
                if (i % 3 == 0 && sc.stuck_pos.y > 0.5f)
                    sc.stuck_pos.y *= 0.5f;
            }
            if (first + uint32_t(count) > big_spider::max_scale_segments)
                count = int32_t(big_spider::max_scale_segments - first);
            sc.first = uint8_t(first);
            sc.count = uint8_t(count);
            first += uint32_t(count);
        }
        g.total_segments = uint8_t(first);
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        auto& g = gs(state, c);
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        bool const spitter = big_spider::spitter(state, c);
        vec2 const c0 = cpos(state, c, 0), c1 = cpos(state, c, 1);
        auto const reset = [&](player::body_part& bp, vec2 at, float rad) {
            bp.pos = at + custom::rnv(rng);
            bp.last_pos = bp.pos;
            bp.vel = vec2{0.f, 0.f};
            bp.rad = rad;
        };
        reset(g.tail_end, c1, 3.f);
        for (auto& m : g.mandibles)
            reset(m, c0, 1.f);
        auto const lp = leg_params(spitter);
        for (auto& side : g.legs)
            for (auto& lg : side) {
                lg = player::hand_state{};
                lg.pos = c0 + custom::rnv(rng);
                lg.last_pos = lg.pos;
                lg.mode = limb_mode::hunt_relative_position;
                lg.hunt_speed = lp.hunt_speed;
                lg.quickness = lp.quickness;
                lg.absolute_hunt_pos = lg.pos;
            }
        // (no Remix: the scales' velocities doubled)
        for (uint32_t i = 0; i < g.total_segments; ++i) {
            g.seg_pos[i] = c0;
            g.seg_last[i] = c0;
            g.seg_vel[i] *= 2.f;
        }
        g.last_flip = g.flip;
        g.last_mandibles_charge = g.mandibles_charge;
        g.last_breath_counter = g.breath_counter;
        g.legs_dangle_counter = 30;
        g.seen_jumps = state.world.big_spider_data_get_move(creature::big_spider_data(state.world, c)).jump_count;
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto& g = gs(state, c);
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        auto const& x = pl(state, c);
        auto const& mv = w.big_spider_data_get_move(creature::big_spider_data(w, c));
        bool const spitter = big_spider::spitter(state, c);
        bool const awake = big_spider::consious(state, c);
        bool const footing = big_spider::footing(state, c);
        bool const dead = w.creature_get_dead(c) != 0;
        vec2 const c0 = cpos(state, c, 0), c1 = cpos(state, c, 1);
        vec2 const v0 = cvel(state, c, 0), v1 = cvel(state, c, 1);
        auto const lp = leg_params(spitter);

        g.last_mandibles_charge = g.mandibles_charge;
        g.mandibles_charge = mathf::inverse_lerp(0.1f, 0.7f, x.mandibles_charged);
        // (the rustle sound loop isn't ported)
        g.last_breath_counter = g.breath_counter;
        if (!dead)
            g.breath_counter += rng.value();
        if (!awake || !footing) {
            g.legs_dangle_counter = 30;
        } else if (g.legs_dangle_counter > 0) {
            --g.legs_dangle_counter;
            for (uint32_t i = 0; i < 2; ++i)
                if (pathing::accessible(room, room_view::get_tile_position(cpos(state, c, i)), big_spider::spider_movement))
                    g.legs_dangle_counter = 0;
        }
        g.last_flip = g.flip;

        // BigSpider.Jump's and Act's doings to the legs
        if (g.seen_jumps != mv.jump_count) {
            g.seen_jumps = mv.jump_count;
            for (uint32_t i = 0; i < 2; ++i)
                for (uint32_t j = 0; j < 4; ++j) {
                    g.legs[i][j].mode = limb_mode::dangle;
                    g.legs[i][j].vel += mv.jump_dir * 30.f * (j < 2 ? 1.f : -1.f);
                }
        }
        if (x.jumping && mv.jump_target_valid) {
            vec2 const pp = mv.jump_target;
            for (uint32_t j = 0; j < 2; ++j)
                for (uint32_t k = 0; k < 2; ++k) {
                    g.legs[j][k].mode = limb_mode::dangle;
                    g.leg_flips[j][k][0] = j == 0 ? -1.f : 1.f;
                    g.legs[j][k].vel += rw::slerp(custom::dir_vec(c0, pp), custom::perpendicular_vector(c0, pp) * (j == 0 ? -1.f : 1.f), k == 0 ? 0.1f : 0.5f) * 3.f;
                }
        }

        // the tail end
        body_parts::generic_update(g.tail_end, room, 0.99f, 0.5f);
        body_parts::connect_to_point(g.tail_end, c1 + custom::dir_vec(c0, c1) * 7.f + custom::perpendicular_vector(c0, c1) * g.flip * 10.f, spitter ? 12.f : 6.f, false,
            0.2f, v1, 0.5f, 0.1f);
        g.tail_end.vel.y -= 0.4f;
        g.tail_end.vel += custom::dir_vec(c0, c1) * 0.8f;
        if (!dead) {
            g.tail_end.vel += g.breath_dir * 0.7f;
            if (rng.value() < 0.1f)
                g.breath_dir = custom::rnv(rng) * rng.value();
        }

        // the mandibles
        float const num = custom::aim(c1, c0);
        vec2 const vector = custom::dir_vec(c1, c0);
        dcon::body_chunk_id held;
        if (auto const t = big_spider::grasped(state, c)) {
            uint32_t const k = creature::grasp_chunk(w, c, 0);
            if (k < creature::chunk_count_of(state, t))
                held = creature::chunk_of(state, t, k);
        }
        for (uint32_t j = 0; j < 2; ++j) {
            auto& m = g.mandibles[j];
            body_parts::generic_update(m, room, 0.9f, 0.5f);
            body_parts::connect_to_point(m, c0 + vector * 10.f * (1.f - g.mandibles_charge), 10.f + 10.f * g.mandibles_charge, false, 0.f, v0, 0.3f, 0.05f);
            float const fl = g.flip * (1.f - 2.f * g.mandibles_charge);
            float const d = mathf::lerp(j == 0 ? -1.f : 1.f, fl, std::fabs(fl) * 0.85f);
            vec2 const perp = custom::perpendicular_vector(vector);
            m.vel += rw::lerp(rw::normalized(vector + perp * d * mathf::lerp(2.2f, 1.1f, std::fabs(g.flip))) * (1.f - g.mandibles_charge),
                (c0 + rw::normalized(perp * d + vector * mathf::lerp(2.f, 0.5f, x.mandibles_charged)) * 20.f - m.pos) / 7.f, g.mandibles_charge);
            if (held) {
                m.pos = rw::lerp(m.pos, w.body_chunk_get_pos(held), 0.5f);
                m.vel *= 0.7f;
            }
            if (awake)
                m.vel += custom::rnv(rng) * rng.value();
        }

        // the flip: by which side the legs are on, and the walls beside
        float num2 = 0.f;
        for (auto const& side : g.legs)
            for (auto const& lg : side)
                num2 += custom::distance_to_line(lg.pos, c1, c0);
        if (!std::isnan(num2))
            g.flip = custom::lerp_and_tick(g.flip, mathf::clamp(num2 / 40.f, -1.f, 1.f), 0.07f, 0.1f);
        float num5 = 0.f;
        if (awake) {
            if (room.get_tile(c0 + custom::perpendicular_vector(c0, c1) * 20.f).solid())
                num5 += 1.f;
            if (room.get_tile(c0 - custom::perpendicular_vector(c0, c1) * 20.f).solid())
                num5 -= 1.f;
        }
        if (num5 != 0.f)
            g.flip = custom::lerp_and_tick(g.flip, num5, 0.07f, 0.05f);

        // the legs
        int32_t num6 = 0;
        g.spikes_dir = vec2{0.f, 0.f};
        for (int m = 0; m < 4; ++m)
            for (int n = 0; n < 2; ++n) {
                auto& lg = g.legs[n][m];
                float const num7 = mathf::inverse_lerp(0.f, 3.f, float(m));
                float const num8 = 0.5f + 0.5f * std::sin((x.run_cycle + float(num6) * 0.25f) * mathf::pi);
                g.spikes_dir += custom::dir_vec(c0, lg.pos) * (1.f - num7);
                g.leg_flips[n][m][1] = g.leg_flips[n][m][0];
                if (rng.value() < num8 * 0.5f && !custom::dist_less(lg.last_pos, lg.pos, 2.f)) {
                    if (rng.value() < num8)
                        g.leg_flips[n][m][0] = custom::lerp_and_tick(g.leg_flips[n][m][0], mathf::lerp(n == 0 ? -1.f : 1.f, g.flip, std::fabs(g.flip)), 0.01f, rng.value() / 6.f);
                    if (rng.value() < num8)
                        g.legs_travel_dirs[n][m] = rw::lerp(g.legs_travel_dirs[n][m], x.travel_dir, mathf::pow(rng.value(), 1.f - 0.9f * num8));
                }
                if (!awake && rng.value() < x.death_convulsions)
                    g.legs_travel_dirs[n][m] = custom::rnv(rng) * rng.value();
                else if (x.charging > 0.f)
                    g.legs_travel_dirs[n][m] = vec2{0.f, 0.f};
                body_parts::limb_update(lg, lp, c0, v0, c1, room);
                auto const& gc = mv.grab[n][m];
                bool const is_dead_leg = g.dead_leg.x == n && g.dead_leg.y == m;
                if (gc.who && creature::live(state, gc.who) && gc.chunk < creature::chunk_count_of(state, gc.who)) {
                    auto const k = creature::chunk_of(state, gc.who, gc.chunk);
                    vec2 const kp = w.body_chunk_get_pos(k);
                    vec2 const a = custom::deg_to_vec(custom::aim(c0, kp) + mathf::lerp(10.f, 170.f, num7) * (n == 0 ? 1.f : -1.f));
                    lg.pos = kp + a * mathf::max(0.f, w.body_chunk_get_rad(k) - 2.f);
                    lg.vel *= 0.7f;
                    lg.mode = limb_mode::dangle;
                } else {
                    if (lg.mode == limb_mode::hunt_relative_position || g.legs_dangle_counter > 0 || is_dead_leg)
                        lg.mode = limb_mode::dangle;
                    vec2 const v2 = custom::deg_to_vec(num + mathf::lerp(40.f, 160.f, num7) * (num5 != 0.f ? -num5 : (n == 0 ? 1.f : -1.f)));
                    vec2 const v3 = c0 + rw::slerp(g.legs_travel_dirs[n][m], v2, 0.1f) * g.leg_length * 0.85f * mathf::pow(num8, 0.5f);
                    body_parts::as_body_part(lg, lp.rad, [&](player::body_part& bp) {
                        body_parts::connect_to_point(bp, v3, g.leg_length, false, 0.f, v0, 0.1f, 0.f);
                        body_parts::connect_to_point(bp, c0, g.leg_length, false, 0.f, v0, 0.1f, 0.f);
                    });
                    if (g.legs_dangle_counter > 0 || num8 < 0.1f || is_dead_leg) {
                        vec2 a2 = v3 + v2 * g.leg_length * 0.5f;
                        if (!awake)
                            a2 = v3 + g.legs_travel_dirs[n][m] * g.leg_length * 0.5f;
                        lg.vel = rw::lerp(lg.vel, a2 - lg.pos, 0.05f);
                        lg.vel.y -= 0.4f;
                        if (awake && !is_dead_leg)
                            lg.vel += custom::rnv(rng) * 3.f;
                    } else {
                        vec2 v4 = v3 + v2 * g.leg_length;
                        for (int n2 = 0; n2 < 2; ++n2)
                            for (int m2 = 0; m2 < 4; ++m2) {
                                vec2 const other = g.legs[n2][m2].absolute_hunt_pos;
                                if (n2 != n && m2 != m && custom::dist_less(v4, other, g.leg_length * 0.1f))
                                    v4 = other + custom::dir_vec(other, v4) * g.leg_length * 0.1f;
                            }
                        constexpr float reach = 1.2f;
                        if (!lg.reached_snap_position)
                            body_parts::find_grip(lg, room, v3, v3, g.leg_length * reach, v4, -2, -2, true);
                        else if (!custom::dist_less(v3, lg.absolute_hunt_pos, g.leg_length * reach * mathf::pow(1.f - num8, 0.2f)))
                            lg.mode = limb_mode::dangle;
                    }
                }
                ++num6;
            }
        g.spikes_dir /= 4.f;

        // the hair
        for (uint32_t i = 0; i < g.scale_count; ++i) {
            auto const& sc = g.scales[i];
            vec2 const a3 = scale_dir(g, spitter, c0, c1, i, rng) * (spitter ? 1.5f : 1.f);
            vec2 const a4 = rw::lerp(body_parts::rotate_around_origo(sc.spec1, num), -vector, 0.5f);
            uint32_t const cnt = sc.count;
            for (uint32_t j = 0; j < cnt; ++j) {
                uint32_t const s = sc.first + j;
                g.seg_last[s] = g.seg_pos[s];
                g.seg_pos[s] += g.seg_vel[s];
                g.seg_vel[s] *= 0.9f;
                g.seg_vel[s].y -= 0.9f;
                if (cnt > 4) {
                    vulture::detail::terrain_collision cd{g.seg_pos[s], g.seg_last[s], g.seg_vel[s], 1.f};
                    vulture::detail::horizontal_collision(room, cd);
                    vulture::detail::vertical_collision(room, cd);
                    vulture::detail::slopes_vertically(room, cd);
                    g.seg_pos[s] = cd.pos;
                    g.seg_vel[s] = cd.vel;
                }
                g.seg_vel[s] += a3 * custom::lerp_map(float(j), 0.f, 8.f, 4.f, 0.f, 0.5f);
                if (spitter)
                    g.seg_vel[s] += a4 * mathf::inverse_lerp(float(cnt / 2), float(cnt - 1), float(j)) * 3.f;
                if (j == 0) {
                    vec2 const at = scale_attach_pos(g, spitter, c0, c1, g.mandibles_charge, i, rng);
                    vec2 const d = custom::dir_vec(g.seg_pos[s], at) * (rw::distance(g.seg_pos[s], at) - sc.spec0.y * 2.4f);
                    g.seg_vel[s] += d;
                    g.seg_pos[s] += d;
                } else {
                    vec2 const b = custom::dir_vec(g.seg_pos[s], g.seg_pos[s - 1]) * (rw::distance(g.seg_pos[s], g.seg_pos[s - 1]) - sc.spec0.y) * 0.5f;
                    g.seg_vel[s] += b;
                    g.seg_pos[s] += b;
                    g.seg_vel[s - 1] -= b;
                    g.seg_pos[s - 1] -= b;
                }
                if (j > 1) {
                    vec2 const d = custom::dir_vec(g.seg_pos[s], g.seg_pos[s - 2]) * 2.f;
                    g.seg_vel[s] -= d;
                    g.seg_vel[s - 2] += d;
                }
                if (j > 2) {
                    vec2 const d = custom::dir_vec(g.seg_pos[s], g.seg_pos[s - 3]);
                    g.seg_vel[s] -= d;
                    g.seg_vel[s - 3] += d;
                }
            }
        }
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip) {
        auto const& w = state.world;
        auto const& g = gs(state, c);
        bool const spitter = big_spider::spitter(state, c);
        float const t = alpha;
        // DrawSprites' UnityEngine.Random: a per creature, per tick roll
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));

        // ApplyPalette (Room.Darkness without the light sources)
        float const darkness = pal.darkness();
        vec3 const black = pal.black();
        vec3 const yellow = mix(big_spider::yellow_col(state, c), pal.fog(), 0.2f);

        float const num = mathf::lerp(g.last_flip, g.flip, t);
        vec2 const vector = rw::lerp(clast(state, c, 0), cpos(state, c, 0), t);
        float const num2 = mathf::lerp(g.last_mandibles_charge, g.mandibles_charge, t);
        vec2 const vector2 = rw::lerp(clast(state, c, 1), cpos(state, c, 1), t) + custom::rnv(rng) * rng.value() * 3.5f * num2;
        vec2 const vector3 = custom::dir_vec(vector2, vector);
        vec2 const a = custom::perpendicular_vector(vector3);
        vec2 const b = rw::lerp(g.tail_end.last_pos, g.tail_end.pos, t);

        // MeshSprite: MakeLongMesh(7) along the bezier (JaggedSquare: drawn solid)
        {
            vec2 vs[28];
            vec2 v4 = vector + vector3;
            float num3 = 0.f;
            float const breath = mathf::lerp(g.last_breath_counter, g.breath_counter, t);
            for (int i = 0; i < 7; ++i) {
                float const f = mathf::inverse_lerp(0.f, 6.f, float(i));
                vec2 const v5 = custom::bezier(vector + vector3 * 3.f, vector2, b, vector2, f);
                float const num4 = mathf::lerp(2.5f, 10.f + std::sin(breath / 10.f), std::sin(mathf::pow(f, 0.75f) * mathf::pi)) * g.body_thickness;
                vec2 const a2 = custom::perpendicular_vector(v5, v4);
                vs[i * 4] = (v4 + v5) / 2.f - a2 * (num4 + num3) * 0.5f;
                vs[i * 4 + 1] = (v4 + v5) / 2.f + a2 * (num4 + num3) * 0.5f;
                vs[i * 4 + 2] = v5 - a2 * num4;
                vs[i * 4 + 3] = v5 + a2 * num4;
                v4 = v5;
                num3 = num4;
            }
            sprites::long_mesh(out, strip, vs, 28, black);
        }
        // HeadSprite
        sprites::add(out, sprite{"Circle20", vector, custom::vec_to_deg(vector3), vec2{(spitter ? 14.f : 11.f) / 20.f, (spitter ? 16.f : 14.f) / 20.f}, vec2{0.5f, 0.5f}, black});
        // LegSprites (side, leg, part)
        for (int j = 0; j < 2; ++j)
            for (int k = 0; k < 4; ++k) {
                float const tt = mathf::inverse_lerp(0.f, 3.f, float(k));
                vec2 v6 = rw::lerp(vector, vector2, 0.3f);
                v6 += a * (j == 0 ? -1.f : 1.f) * 3.f * (1.f - std::fabs(num));
                v6 += vector3 * mathf::lerp(10.f, -6.f, tt);
                auto const& lg = g.legs[j][k];
                vec2 const v7 = rw::lerp(lg.last_pos, lg.pos, t);
                float const f2 = mathf::lerp(g.leg_flips[j][k][1], g.leg_flips[j][k][0], t);
                vec2 v8 = custom::inverse_kinematic(v6, v7, g.leg_length * 0.7f, g.leg_length * 0.7f, f2);
                v8 = rw::lerp(v8,
                    (v6 + v7) * 0.5f - a * num * custom::lerp_map(rw::distance(v6, v7), 0.f, g.leg_length * 1.3f, g.leg_length * 0.7f, 0.f, 3.f), std::fabs(num));
                vec2 v9 = rw::lerp(v6, v8, 0.5f);
                vec2 v10 = rw::lerp(v8, v7, 0.5f);
                float const d = g.leg_length / 4.f;
                vec2 const v11 = rw::lerp(v9, v10, 0.5f);
                v9 = v11 + custom::dir_vec(v11, v9) * d / 2.f;
                v10 = v11 + custom::dir_vec(v11, v10) * d / 2.f;
                float const sx = -mathf::sign(f2);
                sprites::add(out, sprite{"CentipedeLegA", v6, custom::aim(v6, v9), vec2{sx * 1.5f * g.legs_thickness, rw::distance(v6, v9) / centipede_leg_a_h},
                                      vec2{0.5f, 0.f}, black});
                sprites::add(out, sprite{spider_leg_a[k], v9, custom::aim(v9, v10), vec2{sx * 1.2f * g.legs_thickness, (rw::distance(v9, v10) + 2.f) / spider_leg_a_h[k]},
                                      vec2{0.5f, 0.1f}, black});
                float const bh = k == 0 ? centipede_leg_b_h : spider_leg_b_h[k];
                sprites::add(out, sprite{k == 0 ? std::string_view{"CentipedeLegB"} : spider_leg_b[k], v10, custom::aim(v10, v7),
                                      vec2{sx * 1.2f * g.legs_thickness, (rw::distance(v10, v7) + 1.f) / bh}, vec2{0.5f, 0.1f}, black});
            }
        // MandibleSprites
        for (int l = 0; l < 2; ++l) {
            float const num5 = mathf::lerp(l == 0 ? 1.f : -1.f, num, mathf::pow(std::fabs(num), 2.f));
            vec2 const v12 = vector + vector3 * 4.f + a * num5 * -3.f;
            vec2 const v13 = rw::lerp(g.mandibles[l].last_pos, g.mandibles[l].pos, t) + custom::rnv(rng) * rng.value() * 4.f * num2;
            vec2 const v14 = custom::inverse_kinematic(v12, v13, 13.f, 12.f, num5);
            sprites::add(out, sprite{"SpiderLeg0A", v12, custom::aim(v12, v14), vec2{-mathf::sign(num5), rw::distance(v12, v14) / spider_leg_a_h[0]}, vec2{0.5f, 0.f}, black});
            vec2 const a3 = custom::perpendicular_vector(v14, v13);
            float num6 = 4.6f * mathf::sign(num5) * mathf::lerp(0.5f, 1.f, std::fabs(num5));
            vec2 corners[4];
            corners[0] = v13 + a3 * num6;
            corners[1] = v13 - a3 * num6;
            num6 = mathf::lerp(num6, 2.5f * mathf::sign(num5), 0.5f);
            corners[2] = v14 - a3 * num6;
            corners[3] = v14 + a3 * num6;
            vec4 cols[4] = {rgba(black), rgba(black), rgba(black), rgba(black)};
            if (!spitter) {
                vec3 const tip = mix(yellow, black, 0.6f * (1.f - num2) + 0.4f * darkness);
                cols[0] = rgba(tip);
                cols[1] = rgba(tip);
            }
            sprites::add_quad(out, "SpiderLeg0B", corners, cols);
        }
        // the hair (FirstScaleSprite on, LizardScaleA1 anchored at its root)
        for (uint32_t m = 0; m < g.scale_count; ++m) {
            auto const& sc = g.scales[m];
            vec2 v15 = rw::lerp(b, vector2, sc.stuck_pos.y);
            uint32_t const cnt = sc.count;
            for (uint32_t n = 0; n < cnt; ++n) {
                uint32_t const s = sc.first + n;
                vec2 const v16 = rw::lerp(g.seg_last[s], g.seg_pos[s], t);
                float const num7 = mathf::inverse_lerp(0.f, float(cnt - 1), float(n));
                float const cf = (mathf::inverse_lerp(0.f, float(cnt - 1), float(n)) + mathf::inverse_lerp(0.f, 5.f, float(n))) / 2.f;
                vec3 const col = mix(black, yellow, cf * mathf::lerp(0.3f, 0.9f, sc.spec0.x) * (1.f - darkness));
                float const sx = spitter ? mathf::lerp(1.2f, 0.9f, num7) : mathf::lerp(0.9f, 0.4f, mathf::pow(num7, 0.4f));
                sprites::add(out, sprite{"LizardScaleA1", v15, custom::aim(v15, v16), vec2{sx, rw::distance(v16, v15) / scale_h}, vec2{0.5f, 0.f}, col});
                v15 = v16;
            }
        }
        (void)w;
    }
}
