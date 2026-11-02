// OracleGraphics.cs. See oracle_graphics.hpp.

#include <cmath>
#include <optional>

#include "oracle_graphics.hpp"
#include "body_parts.hpp"
#include "creature.hpp"
#include "oracle.hpp"
#include "oracle_types.hpp"
#include "scavenger.hpp"
#include "vulture_internal.hpp"

namespace oracle_graphics {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using sprites::sprite;
    using world::room_view;
    using namespace oracle;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        constexpr float pi = 3.1415927f;
        constexpr uint32_t bit_ring_size[3] = {10, 30, 60};
        constexpr uint32_t bit_ring_first[3] = {0, 10, 40};

        graphics_state& gs(sys::game_state& state, dcon::creature_id c) { return state.world.oracle_data_get_graphics(oracle::data(state, c)); }
        graphics_state const& gs(sys::game_state const& state, dcon::creature_id c) { return state.world.oracle_data_get_graphics(oracle::data(state, c)); }

        vec3 lerp3(vec3 a, vec3 b, float t) {
            t = mathf::clamp01(t);
            return a + (b - a) * t;
        }

        // which parts it has (OracleGraphics' constructor)
        bool is_moon(id w) { return w == id::sl; }
        bool has_halo(id w) { return w == id::ss || w == id::dm; }
        bool has_gown(id w, body_state const& b) { return w != id::sl || b.robe; }
        bool has_umbilical(id w) { return w != id::sl; }

        //
        // GenericBodyPart helpers
        //
        void body_part_update(player::body_part& bp, room_view const& room, float air_friction, float surface_friction) {
            body_parts::generic_update(bp, room, air_friction, surface_friction);
        }

        //
        // Gown
        //
        vec2 ideal_pos_for_point(int x, int y, vec2 body_pos, vec2 dir, vec2 perp) {
            float const num = mathf::inverse_lerp(0.f, float(gown_divs - 1), float(x));
            float const t = mathf::inverse_lerp(0.f, float(gown_divs - 1), float(y));
            float const a = 5.f;
            float const num2 = 11.f;
            return body_pos + perp * (mathf::lerp(-1.f, 1.f, num) * mathf::lerp(a, num2, t))
                + dir * mathf::lerp(a, -(num2 + 7.f), t) * (1.f + std::sin(pi * num) * 0.35f * mathf::lerp(-1.f, 1.f, t));
        }

        void gown_update(graphics_state& g, vec2 c0, vec2 c1, float gravity) {
            vec2 const pos = c0;
            vec2 const vector = custom::dir_vec(c1, c0);
            vec2 const perp = custom::perpendicular_vector(vector);
            int const n = int(gown_divs);
            for (int i = 0; i < n; ++i) {
                for (int j = 0; j < n; ++j) {
                    float const t = mathf::inverse_lerp(0.f, float(n - 1), float(j));
                    auto& p = g.cloth[i][j];
                    p[1] = p[0];
                    p[0] += p[2];
                    p[2] *= 0.999f;
                    p[2].y -= 0.9f * gravity;
                    vec2 const v2 = ideal_pos_for_point(i, j, pos, vector, perp);
                    p[2] += rw::slerp(-vector, custom::dir_vec(c1, v2), t) * 0.02f;
                    float num = rw::distance(p[0], v2);
                    float const num2 = mathf::lerp(0.f, 9.f, t);
                    vec2 a = custom::dir_vec(p[0], v2);
                    if (num > num2) {
                        p[0] -= (num2 - num) * a;
                        p[2] -= (num2 - num) * a;
                    }
                    for (int k = 0; k < 4; ++k) {
                        ivec2 const q = ivec2{i, j} + custom::four_directions[k];
                        if (q.x >= 0 && q.y >= 0 && q.x < n && q.y < n) {
                            auto& o = g.cloth[q.x][q.y];
                            num = rw::distance(p[0], o[0]);
                            a = custom::dir_vec(p[0], o[0]);
                            float const num3 = rw::distance(v2, ideal_pos_for_point(q.x, q.y, pos, vector, perp));
                            p[2] -= (num3 - num) * a * 0.05f;
                            o[2] += (num3 - num) * a * 0.05f;
                        }
                    }
                }
            }
        }

        vec3 gown_color(id w, float f) {
            if (is_pebbles(w) || w == id::cl) {
                vec3 c = custom::hsl2rgb(mathf::lerp(0.08f, 0.02f, mathf::pow(f, 2.f)), mathf::lerp(1.f, 0.8f, f), 0.5f);
                if (w == id::cl)
                    c = lerp3(c, vec3{1.f, 1.f, 1.f}, 0.3f);
                return c;
            }
            if (is_moon(w))
                return custom::hsl2rgb(mathf::lerp(0.38f, 0.32f, mathf::pow(f, 2.f)), mathf::lerp(0.f, 0.1f, mathf::pow(f, 1.1f)),
                    mathf::lerp(0.7f, 0.3f, mathf::pow(f, 6.f)));
            return custom::hsl2rgb(mathf::lerp(0.08f, 0.02f, mathf::pow(f, 2.f)), mathf::lerp(0.6f, 0.4f, f), 0.4f);
        }

        //
        // Halo
        //
        void bit_set_to_max(halo_bit& b) {
            b.get_to_filled = 1.f;
            b.fill_speed = mathf::lerp(b.fill_speed, 0.25f, 0.25f);
            b.blink_counter = 20;
        }
        void bit_update(halo_bit& b, rw::rng& rng) {
            b.last_filled = b.filled;
            if (b.filled != b.get_to_filled) {
                b.filled = custom::lerp_and_tick(b.filled, b.get_to_filled, 0.03f, b.fill_speed);
                return;
            }
            if (b.blink_counter > 0) {
                --b.blink_counter;
                return;
            }
            if (rng.value() < 0.016666668f) {
                b.get_to_filled = rng.value();
                b.fill_speed = 1.f / mathf::lerp(2.f, 80.f, rng.value());
            }
        }
        float bit_fill(halo_bit const& b, float t) {
            if (b.blink_counter % 4 > 1 && b.filled == b.get_to_filled)
                return 0.f;
            return mathf::lerp(b.last_filled, b.filled, t);
        }

        void halo_update(graphics_state& g, rw::rng& rng, bool conscious) {
            for (auto& cn : g.connections) {
                cn.last_light_up = cn.light_up;
                cn.light_up *= 0.9f;
                // (SS_AI_Halo_Connection_Light_Up isn't played)
                if (rng.value() < g.connections_fire_chance / 40.f && conscious)
                    cn.light_up = 1.f;
            }
            if (rng.value() < 0.016666668f)
                g.connections_fire_chance = mathf::pow(rng.value(), 3.f);
            for (auto& r : g.ring_rotations) {
                r[1] = r[0];
                if (r[0] != r[3]) {
                    r[4] += 1.f / mathf::lerp(20.f, std::fabs(r[2] - r[3]), 0.5f);
                    r[0] = mathf::lerp(r[2], r[3], custom::s_curve(r[4], 0.5f));
                    if (r[4] > 1.f) {
                        r[4] = 0.f;
                        r[2] = r[3];
                        r[0] = r[3];
                    }
                } else if (rng.value() < 0.033333335f) {
                    float const s = rng.value() < 0.5f ? -1.f : 1.f;
                    r[3] = r[0] + s * mathf::lerp(15.f, 150.f, rng.value());
                }
            }
            for (auto& b : g.bits)
                bit_update(b, rng);
            if (rng.value() < 0.016666668f) {
                int32_t const ring = rng.range(0, 3);
                for (uint32_t m = 0; m < bit_ring_size[ring]; ++m)
                    bit_set_to_max(g.bits[bit_ring_first[ring] + m]);
            }
            g.last_expand = g.expand;
            g.last_push = g.push;
            g.last_white = g.white;
            g.expand = custom::lerp_and_tick(g.expand, g.get_to_expand, 0.05f, 0.0125f);
            g.push = custom::lerp_and_tick(g.push, g.get_to_push, 0.02f, 0.025f);
            g.white = custom::lerp_and_tick(g.white, g.get_to_white, 0.07f, 0.022727273f);
            bool flag = false;
            if (rng.value() < 0.00625f) {
                if (rng.value() < 0.125f) {
                    flag = g.get_to_white < 1.f;
                    g.get_to_white = 1.f;
                } else {
                    g.get_to_white = 0.f;
                }
            }
            if (rng.value() < 0.00625f || flag) {
                bool const one = rng.value() < 0.5f && !flag;
                g.get_to_expand = one ? 1.f : mathf::lerp(0.8f, 2.f, mathf::pow(rng.value(), 1.5f));
            }
            if (rng.value() < 0.00625f || flag) {
                bool const zero = rng.value() < 0.5f && !flag;
                if (zero) {
                    g.get_to_push = 0.f;
                } else {
                    int32_t const hi = rng.range(1, 6);
                    g.get_to_push = float(-1 + rng.range(0, hi));
                }
            }
        }

        float halo_radius(graphics_state const& g, float ring, float t) {
            // (no voice: averageVoice 0)
            return (3.f + ring + mathf::lerp(g.last_push, g.push, t)) * mathf::lerp(g.last_expand, g.expand, t) * 10.f;
        }

        //
        // UbilicalCord / DisconnectedUbilicalCord
        //
        void terrain_collide(room_view const& room, vec2& pos, vec2 last, vec2& vel, float rad) {
            vulture::detail::terrain_collision cd{pos, last, vel, rad};
            vulture::detail::vertical_collision(room, cd);
            vulture::detail::horizontal_collision(room, cd);
            vulture::detail::slopes_vertically(room, cd);
            pos = cd.pos;
            vel = cd.vel;
        }

        void umb_stuck(graphics_state& g, id w, arm_state const& arm) {
            constexpr uint32_t n = umb_coords;
            g.coord[0][0] = w == id::rm ? room_view::middle_of_tile(75, 38) : w == id::cl ? room_view::middle_of_tile(118, 6) : room_view::middle_of_tile(24, 2);
            g.coord[0][2] *= 0.f;
            vec2 const pos = arm.joints[1].pos;
            vec2 const vector = oracle::elbow_pos(arm, 1, 1.f, arm.joints[2].pos);
            for (int i = -1; i < 2; ++i) {
                float const num = i == 0 ? 1.f : 0.5f;
                auto& p = g.coord[n - 20 + i];
                p[0] = rw::lerp(p[0], rw::lerp(pos, vector, 0.4f + 0.07f * float(i)) + custom::perpendicular_vector(pos, vector) * 8.f, num);
                p[2] *= 1.f - num;
            }
        }

        template<size_t N>
        void cord_constraints(vec2 (&cord)[N][3], float rest) {
            for (size_t k = 1; k < N; ++k) {
                vec2 const a = custom::dir_vec(cord[k][0], cord[k - 1][0]);
                float const d = rw::distance(cord[k][0], cord[k - 1][0]);
                cord[k][0] -= (rest - d) * a * 0.5f;
                cord[k][2] -= (rest - d) * a * 0.5f;
                cord[k - 1][0] += (rest - d) * a * 0.5f;
                cord[k - 1][2] += (rest - d) * a * 0.5f;
            }
            for (size_t k = 0; k + 1 < N; ++k) {
                vec2 const a = custom::dir_vec(cord[k][0], cord[k + 1][0]);
                float const d = rw::distance(cord[k][0], cord[k + 1][0]);
                cord[k][0] -= (rest - d) * a * 0.5f;
                cord[k][2] -= (rest - d) * a * 0.5f;
                cord[k + 1][0] += (rest - d) * a * 0.5f;
                cord[k + 1][2] += (rest - d) * a * 0.5f;
            }
        }

        void umb_update(graphics_state& g, id w, arm_state const& arm, room_view const& room, vec2 c0) {
            constexpr uint32_t n = umb_coords;
            for (uint32_t i = 0; i < n; ++i) {
                float const value = float(i) / float(n - 1);
                auto& p = g.coord[i];
                p[1] = p[0];
                p[0] += p[2];
                p[2] *= 0.995f;
                p[2].y += mathf::inverse_lerp(0.2f, 0.f, value);
                p[2].y -= room.gravity * 0.9f;
                terrain_collide(room, p[0], p[1], p[2], 5.f);
            }
            umb_stuck(g, w, arm);
            for (uint32_t j = 1; j < n; ++j) {
                vec2 const a = custom::dir_vec(g.coord[j][0], g.coord[j - 1][0]);
                float const num = rw::distance(g.coord[j][0], g.coord[j - 1][0]);
                g.coord[j][0] -= (10.f - num) * a * 0.5f;
                g.coord[j][2] -= (10.f - num) * a * 0.5f;
                g.coord[j - 1][0] += (10.f - num) * a * 0.5f;
                g.coord[j - 1][2] += (10.f - num) * a * 0.5f;
            }
            umb_stuck(g, w, arm);
            for (uint32_t k = 0; k + 1 < n; ++k) {
                vec2 const a2 = custom::dir_vec(g.coord[k][0], g.coord[k + 1][0]);
                float const num2 = rw::distance(g.coord[k][0], g.coord[k + 1][0]);
                g.coord[k][0] -= (10.f - num2) * a2 * 0.5f;
                g.coord[k][2] -= (10.f - num2) * a2 * 0.5f;
                g.coord[k + 1][0] += (10.f - num2) * a2 * 0.5f;
                g.coord[k + 1][2] += (10.f - num2) * a2 * 0.5f;
            }
            umb_stuck(g, w, arm);
            float num3 = 0.5f;
            for (uint32_t l = 2; l < 4; ++l) {
                for (uint32_t m = l; m < n - l; ++m) {
                    vec2 const d1 = custom::dir_vec(g.coord[m - l][0], g.coord[m][0]);
                    g.coord[m][2] += d1 * num3;
                    g.coord[m - l][2] -= d1 * num3;
                    vec2 const d2 = custom::dir_vec(g.coord[m + l][0], g.coord[m][0]);
                    g.coord[m][2] += d2 * num3;
                    g.coord[m + l][2] -= d2 * num3;
                }
                num3 *= 0.75f;
            }
            auto& last = g.coord[n - 1];
            if (!custom::dist_less(last[0], c0, 80.f)) {
                vec2 const a3 = custom::dir_vec(last[0], c0);
                float const num4 = rw::distance(last[0], c0);
                last[0] -= (80.f - num4) * a3 * 0.25f;
                last[2] -= (80.f - num4) * a3 * 0.5f;
            }
            vec2 const tail_dir = custom::dir_vec(g.coord[n - 2][0], g.coord[n - 1][0]);
            for (uint32_t c = 0; c < umb_small; ++c) {
                auto& sc = g.small_cords[c];
                for (uint32_t k = 0; k < umb_small_points; ++k) {
                    sc[k][1] = sc[k][0];
                    sc[k][0] += sc[k][2];
                    sc[k][2] *= custom::lerp_map(rw::magnitude(sc[k][2]), 2.f, 6.f, 0.999f, 0.9f);
                    sc[k][2].y -= room.gravity * 0.9f;
                }
                cord_constraints(sc, g.small_lengths[c] / float(umb_small_points));
                sc[0][0] = g.coord[n - 1][0];
                sc[0][2] *= 0.f;
                sc[1][2] += tail_dir * 5.f;
                sc[2][2] += tail_dir * 3.f;
                sc[3][2] += tail_dir * 1.5f;
                sc[umb_small_points - 1][0] = g.head.pos;
                sc[umb_small_points - 1][2] *= 0.f;
                sc[umb_small_points - 2][2] -= (g.look_dir + g.small_head_dirs[c]) * 2.f;
                sc[umb_small_points - 3][2] -= g.look_dir + g.small_head_dirs[c];
            }
        }

        void disc_update(graphics_state& g, room_view const& room, vec2 c0) {
            for (uint32_t i = 0; i < disc_small; ++i) {
                auto& sc = g.disc_cords[i];
                for (uint32_t j = 0; j < disc_small_points; ++j) {
                    sc[j][1] = sc[j][0];
                    sc[j][0] += sc[j][2];
                    sc[j][2] *= 0.94f;
                    sc[j][2].y -= room.gravity * 0.9f;
                    terrain_collide(room, sc[j][0], sc[j][1], sc[j][2], 1.f);
                }
                cord_constraints(sc, g.disc_lengths[i] / float(disc_small_points));
                sc[disc_small_points - 1][0] = g.head.pos + custom::dir_vec(c0, g.head.pos) * 3.f;
                sc[disc_small_points - 1][2] *= 0.f;
                sc[disc_small_points - 2][2] -= (g.look_dir + g.disc_head_dirs[i]) * 3.f;
                sc[disc_small_points - 3][2] -= (g.look_dir + g.disc_head_dirs[i]) * 1.5f;
            }
        }

        //
        // the hands and feet of the sitting ones (Moon, Saint's and
        // Rivulet's Pebbles: the same but for what the slugcat does)
        //
        void sitting_limbs(graphics_state& g, uint32_t j, bool flag, bool sit, body_state const& b, room_view const& room, vec2 c0, vec2 c1, vec2 get_to) {
            auto const ray = [&](vec2 a, vec2 bb) { return scavenger::exact_terrain_ray_trace_pos(room, a, bb); };
            float const side = j != 0 ? 1.f : -1.f;
            float const cs = crawl_speed(b);
            std::optional<vec2> v;
            if (flag) {
                v = ray(c0, c0 + vec2{j != 0 ? -14.f : -24.f, -40.f});
            } else {
                vec2 const from = sit ? c0 : c1;
                v = ray(from, from + vec2{j != 0 ? -24.f : -54.f, -40.f} * 2.f * cs);
            }
            auto& foot = g.feet[j];
            auto& hand = g.hands[j];
            if (v)
                foot.vel += rw::clamp_magnitude(*v - foot.pos, 10.f) / 2.f;
            vec2 v5 = (foot.pos + c1) / 2.f;
            if (flag && v) {
                vec2 v6 = foot.pos + custom::dir_vec(c1, c0) * 15.f;
                v6 += custom::dir_vec(c0, v6) * 5.f;
                v5 = rw::lerp(v6, (foot.pos + c1) / 2.f, mathf::inverse_lerp(7.f, 14.f, rw::distance(foot.pos, c1)));
            } else {
                v5 += custom::perpendicular_vector(c1, c0) * side * 5.f;
            }
            g.knees[j][0] = rw::lerp(g.knees[j][0], v5, 0.4f);
            if (!custom::dist_less(g.knees[j][0], v5, 15.f))
                g.knees[j][0] = v5 + custom::dir_vec(v5, g.knees[j][0]);
            // (nobody near, nothing held, no protest)
            if (flag) {
                hand.vel += rw::clamp_magnitude(g.knees[j][0] - hand.pos, 10.f) / 3.f;
            } else if (!sit) {
                float const cr = crawl(b);
                auto const v2 = ray(c0, c0 + vec2{side * cr * 40.f, -40.f});
                if (v2)
                    hand.vel += rw::clamp_magnitude(*v2 - hand.pos, 10.f) / 3.f;
                else
                    hand.vel.x += side * cr;
                g.knees[j][0] = foot.pos + custom::dir_vec(foot.pos, get_to + vec2{-50.f, 0.f}) * 8.f
                    + custom::perpendicular_vector(c1, c0) * side * mathf::lerp(2.f, 6.f, cs);
            }
        }

        void graphics_tick(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& g = gs(state, c);
            auto const& b = oracle::body_of(state, c);
            auto const& arm = oracle::arm_of(state, c);
            id const w = oracle::id_of(state, c);
            auto& wd = state.world;
            auto const k0 = creature::chunk(state, c, 0);
            auto const k1 = creature::chunk(state, c, 1);
            vec2 const c0 = wd.body_chunk_get_pos(k0), c1 = wd.body_chunk_get_pos(k1);
            vec2 const v0 = wd.body_chunk_get_vel(k0), v1 = wd.body_chunk_get_vel(k1);
            bool const conscious = oracle::consious(state, c);
            bool const closed = oracle::eyes_closed(state, c);
            auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));

            g.breathe += 1.f / mathf::lerp(10.f, 60.f, b.health);
            g.last_breath_fac = g.breath_fac;
            g.breath_fac = mathf::lerp(0.5f + 0.5f * std::sin(g.breathe * pi * 2.f), 1.f, mathf::pow(mathf::max(b.health, 0.f), 2.f));
            if (has_gown(w, b))
                gown_update(g, c0, c1, room.gravity);
            if (has_halo(w))
                halo_update(g, rng, conscious);
            bool const flag = sits(w) && b.hold_knees && wd.body_chunk_get_contact_point(k1).y < 0;
            g.last_look_dir = g.look_dir;
            if (conscious) {
                g.look_dir = rw::clamp_magnitude(b.look_point - c0, 100.f) / 100.f;
                g.look_dir = rw::clamp_magnitude(g.look_dir, 1.f);
            }
            body_part_update(g.head, room, 0.995f, 0.5f);
            body_parts::connect_to_point(g.head, c0 + custom::dir_vec(c1, c0) * 6.f, 8.f, true, 0.f, v0, 0.5f, 0.01f);
            if (conscious) {
                if (flag && closed) {
                    g.head.vel += custom::deg_to_vec(-90.f);
                } else {
                    g.head.vel += custom::dir_vec(c1, c0) * g.breath_fac;
                    g.head.vel += g.look_dir * 0.5f * g.breath_fac;
                }
            } else {
                g.head.vel += custom::dir_vec(c1, c0) * 0.75f;
                g.head.vel.y -= 0.7f;
            }
            bool const sit = oracle::in_sit_position(state, c);
            vec2 const get_to = oracle::get_to_pos(state, c);
            for (uint32_t j = 0; j < 2; ++j) {
                float const sgn = j == 0 ? -1.f : 1.f;
                auto& foot = g.feet[j];
                auto& hand = g.hands[j];
                body_part_update(foot, room, 0.98f, 0.5f);
                body_parts::connect_to_point(foot, c1, is_moon(w) ? 20.f : 10.f, false, 0.f, v1, 0.3f, 0.01f);
                if (is_moon(w))
                    foot.vel.y -= 0.5f;
                foot.vel += custom::dir_vec(c0, c1) * 0.3f;
                foot.vel += custom::perpendicular_vector(custom::dir_vec(c0, c1)) * 0.15f * sgn;
                body_part_update(hand, room, 0.98f, 0.5f);
                body_parts::connect_to_point(hand, c0, 15.f, false, 0.f, v0, 0.3f, 0.01f);
                hand.vel.y -= 0.5f;
                hand.vel += custom::dir_vec(c0, c1) * 0.3f;
                hand.vel += custom::perpendicular_vector(custom::dir_vec(c0, c1)) * 0.3f * sgn;
                g.knees[j][1] = g.knees[j][0];
                if (w == id::rm || w == id::cl) {
                    sitting_limbs(g, j, flag, sit, b, room, c0, c1, get_to);
                } else if (!is_moon(w)) {
                    // (no voice, no hand towards the slugcat)
                    g.knees[j][0] = (foot.pos + c1) / 2.f + custom::perpendicular_vector(custom::dir_vec(c0, c1)) * 4.f * sgn;
                } else {
                    sitting_limbs(g, j, flag, sit, b, room, c0, c1, get_to);
                }
            }
            for (uint32_t k = 0; k < joint_count; ++k) {
                auto const& jt = arm.joints[k];
                vec2 const p = k + 1 < joint_count ? arm.joints[k + 1].pos : c1;
                float const num = rw::distance(jt.pos, p);
                float const num2 = (g.last_ac[k] - num) / jt.total_length;
                g.last_cog_rotat[k] = g.cog_rotat[k];
                g.cog_rotat[k] += num2 * (k % 2 == 0 ? -1.f : 1.f) * 2.f;
                g.last_ac[k] = num;
                // (the joints' sound loops aren't played)
            }
            if (has_umbilical(w))
                umb_update(g, w, arm, room, c0);
            else
                disc_update(g, room, c0);
            g.last_eyes_open = g.eyes_open;
            g.eyes_open = closed ? 0.f : 1.f;
            if (w == id::dm) {
                // MoonSigilSprite's alpha (DrawSprites: no conversations)
                if (!conscious)
                    g.sigil_alpha = 0.f;
                else if (b.working > 0.9f)
                    g.sigil_alpha = mathf::lerp(g.sigil_alpha, 1.f, 0.02f);
                else
                    g.sigil_alpha = mathf::lerp(g.sigil_alpha, 0.f, 0.05f);
            }
        }

        //
        // DrawSprites helpers
        //
        void spr(std::vector<sprites::draw_cmd>& out, std::string_view el, vec2 p, float rot, vec2 scale, vec2 anchor, vec3 col) {
            sprites::add(out, sprite{el, p, rot, scale, anchor, col});
        }
        void spr_alpha(std::vector<sprites::draw_cmd>& out, std::string_view el, vec2 p, float rot, vec2 scale, vec2 anchor, vec3 col, float alpha) {
            vec4 const c4{col.x, col.y, col.z, mathf::clamp01(alpha)};
            sprites::add(out, sprite{el, p, rot, scale, anchor, col}, {c4, c4, c4, c4});
        }
        void tri(std::vector<sprites::draw_cmd>& out, vec2 a, vec2 b, vec2 c, vec3 col) {
            sprites::draw_cmd d;
            d.is_triangle = true;
            d.tri[0] = a;
            d.tri[1] = b;
            d.tri[2] = c;
            d.color = col;
            out.push_back(d);
        }
        vec2 lerp_chunk(sys::game_state const& state, dcon::creature_id c, uint32_t i, float t) {
            return rw::lerp(oracle::chunk_last_pos(state, c, i), oracle::chunk_pos(state, c, i), t);
        }

        // ApplyPalette's colours
        struct colors {
            vec3 skin;
            vec3 metal;
            vec3 sl_base_a{0.52156866f, 0.52156866f, 0.5137255f};
            vec3 sl_hl_a{0.5686275f, 0.5686275f, 0.54901963f};
            vec3 sl_base_b;
            vec3 sl_hl_b;
            float darkness = 0.f;
            bool sl_like = false;
            vec3 base(vec2 ps) const {
                if (sl_like)
                    return lerp3(sl_base_a, sl_base_b, mathf::inverse_lerp(50.f, 400.f, rw::distance(ps, vec2{1387.f, 188.f})));
                return custom::hsl2rgb(0.025f, 0.1f, 0.7f - 0.5f * darkness);
            }
            vec3 highlight(vec2 ps) const {
                if (sl_like)
                    return lerp3(sl_hl_a, sl_hl_b, mathf::inverse_lerp(50.f, 400.f, rw::distance(ps, vec2{1387.f, 188.f})));
                return custom::hsl2rgb(0.025f, 0.1f, 0.85f - 0.65f * darkness);
            }
        };

        vec3 skin_color(id w) {
            if (is_pebbles(w) || w == id::cl)
                return vec3{1.f, 0.4f, 0.79607844f};
            if (w == id::sl)
                return vec3{0.105882354f, 0.27058825f, 0.34117648f};
            if (w == id::dm)
                return vec3{0.13f, 0.53f, 0.69f};
            return vec3{0.89f, 0.89f, 0.79f};
        }

        // ArmJointGraphics.DrawSprites (its 15 sprites in order; the first
        // cog of the first joint left out when `skip_cog0`)
        void draw_joint(std::vector<sprites::draw_cmd>& out, std::vector<sprites::strip_vertex>& strip, sys::game_state const& state, dcon::creature_id c,
            arm_state const& arm, graphics_state const& g, uint32_t i, float t, colors const& col, bool moon, bool skip_cog0) {
            auto const& jt = arm.joints[i];
            float const total = jt.total_length;
            int const mesh_segs = int(total / 10.f);
            float const small_arm = total / 4.f;
            vec2 vector = rw::lerp(jt.last_pos, jt.pos, t);
            vec2 vector2 = i + 1 < joint_count ? rw::lerp(arm.joints[i + 1].last_pos, arm.joints[i + 1].pos, t) : lerp_chunk(state, c, 1, t);
            vec2 const vector3 = oracle::elbow_pos(arm, i, t, vector2);
            vec2 const jd = custom::dir_vec(vector, vector3);
            float const off = i == 0 ? 12.f : 2.f;
            vec2 const joint0 = vector + jd * off;
            float const cog = mathf::lerp(g.last_cog_rotat[i], g.cog_rotat[i], t);
            vec3 const hl0 = col.highlight(vec2{0.f, 0.f});
            vec3 const joint_hl = lerp3(col.metal, hl0, 0.5f);
            // cogs
            for (int j = 0; j < 3; ++j) {
                if (j == 0 && skip_cog0)
                    continue;
                spr(out, "pixel", joint0, custom::aim(vector, vector3) + 60.f * float(j) + cog * 360.f, vec2{5.f - float(i) * 0.5f, 18.f - float(i) * 2.f},
                    vec2{0.5f, 0.5f}, col.metal);
            }
            // metal parts
            float const mx = (i == 0 ? 1.5f : (i == 1 ? 1.f : 0.8f)) * (i % 2 == 0 ? -1.f : 1.f);
            float const my = i == 0 ? 2.f : (i == 1 ? 1.f : 0.5f);
            spr(out, "MirosLegSmallPart", vector3, custom::aim(rw::lerp(vector, vector2, 0.2f), vector3), vec2{mx, my}, vec2{0.5f, 1.f}, col.metal);
            float const thick = i == 0 ? 2.f : 1.f;
            spr(out, "pixel", vector3, custom::aim(vector3, rw::lerp(vector, vector2, 0.8f)), vec2{thick, small_arm}, vec2{0.5f, 0.f}, col.metal);
            vec2 const vector4 = vector3 + custom::dir_vec(vector3, rw::lerp(vector, vector2, 0.8f)) * small_arm;
            spr(out, "pixel", vector2, custom::aim(vector2, vector4), vec2{thick, rw::distance(vector4, vector2) + 10.f}, vec2{0.5f, 0.f}, col.metal);
            spr(out, "deerEyeB", vector4, 0.f, vec2{1.f, 1.f}, vec2{0.5f, 0.5f}, col.metal);
            // JointSprite(1, *)
            spr(out, "Circle20", vector3, 0.f, vec2{1.f, 1.f} * ((6.f - float(i) * 1.5f) / 10.f), vec2{0.5f, 0.5f}, col.metal);
            spr(out, "deerEyeB", vector3, 0.f, vec2{1.f, 1.f}, vec2{0.5f, 0.5f}, joint_hl);
            // the segments
            vec2 const vector5 = custom::dir_vec(vector, vector3);
            vec2 const vector6 = custom::dir_vec(vector3, vector2);
            vec2 const vector7 = rw::slerp(vector5, vector6, 0.5f);
            vector += vector5 * mathf::lerp(total * 0.05f, 4.f, 0.5f);
            vector2 -= vector6 * mathf::lerp(total * 0.025f, 4.f, 0.5f);
            float const d1 = rw::distance(vector, vector3), d2 = rw::distance(vector3, vector2);
            vec2 const ca = vector + vector5 * d1 * 0.2f;
            vec2 const cb = vector3 - vector7 * d1 * 0.2f;
            vec2 const ca2 = vector3 + vector7 * d2 * 0.2f;
            vec2 const cb2 = vector2 - vector6 * d2 * 0.2f;
            float const num3 = i == 0 ? 7.f : (i == 2 ? 4.f : (i == 3 ? 3.f : 5.f));
            float const flip = i % 2 == 0 ? 1.f : -1.f;
            static thread_local std::vector<vec2> v_base, v_hl;
            static thread_local std::vector<vec4> c_base, c_hl;
            for (int k = 0; k < 2; ++k) {
                v_base.clear();
                v_hl.clear();
                vec2 a = k == 0 ? vector : vector3;
                vec2 v = k == 0 ? vector5 : vector7;
                float num2 = 0.5f;
                auto curve = [&](float f) { return k == 0 ? custom::bezier(vector, ca, vector3, cb, f) : custom::bezier(vector3, ca2, vector2, cb2, f); };
                for (int l = 1; l <= mesh_segs; ++l) {
                    float const num4 = float(l) / float(mesh_segs - 1);
                    float num5 = 0.6f + 0.5f * (1.f - std::sin(num4 * pi))
                        + 0.3f * mathf::max(std::sin(mathf::inverse_lerp(0.f, 0.3f, num4) * pi), std::sin(mathf::inverse_lerp(0.7f, 1.f, num4) * pi));
                    if (num4 == 1.f)
                        num5 = 0.5f;
                    vec2 const vector8 = curve(num4);
                    vec2 const vector9 = num4 < 1.f ? custom::dir_vec(vector8, curve(float(l + 1) / float(mesh_segs - 1))) : (k == 0 ? vector7 : vector6);
                    float num6 = 0.f;
                    if ((k == 0 && num4 > 0.75f) || (k == 1 && num4 < 0.25f)) {
                        num5 *= 0.5f;
                        num6 = num5 * num3 * flip;
                    }
                    vec2 const mid = (a + vector8) / 2.f;
                    vec2 const pv = custom::perpendicular_vector(v);
                    vec2 const p9 = custom::perpendicular_vector(vector9);
                    v_base.push_back(mid + pv * (num6 + num3 * (num5 + num2) * 0.5f));
                    v_base.push_back(mid + pv * (num6 - num3 * (num5 + num2) * 0.5f));
                    v_base.push_back(vector8 + p9 * (num6 + num3 * num5));
                    v_base.push_back(vector8 + p9 * (num6 - num3 * num5));
                    v_hl.push_back(mid + pv * (num6 + num3 * (num5 + num2) * 0.5f * 0.5f));
                    v_hl.push_back(mid + pv * (num6 - num3 * (num5 + num2) * 0.5f * 0.5f));
                    v_hl.push_back(vector8 + p9 * (num6 + num3 * num5 * 0.5f));
                    v_hl.push_back(vector8 + p9 * (num6 - num3 * num5 * 0.5f));
                    a = vector8;
                    v = vector9;
                    num2 = num5;
                }
                if (moon) {
                    // (customColor: every vertex by where it is)
                    c_base.clear();
                    c_hl.clear();
                    for (auto const p : v_base) {
                        vec3 const cc = col.base(p);
                        c_base.push_back(vec4{cc.x, cc.y, cc.z, 1.f});
                    }
                    for (auto const p : v_hl) {
                        vec3 const cc = col.highlight(p);
                        c_hl.push_back(vec4{cc.x, cc.y, cc.z, 1.f});
                    }
                    sprites::long_mesh(out, strip, v_base.data(), c_base.data(), v_base.size());
                    sprites::long_mesh(out, strip, v_hl.data(), c_hl.data(), v_hl.size());
                } else {
                    sprites::long_mesh(out, strip, v_base.data(), v_base.size(), col.base(vec2{0.f, 0.f}));
                    sprites::long_mesh(out, strip, v_hl.data(), v_hl.size(), hl0);
                }
            }
            // JointSprite(0, *)
            spr(out, "Circle20", joint0, 0.f, vec2{1.f, 1.f} * ((7.f - float(i) * 0.5f) / 10.f), vec2{0.5f, 0.5f}, col.metal);
            spr(out, "deerEyeB", joint0, 0.f, vec2{1.f, 1.f}, vec2{0.5f, 0.5f}, joint_hl);
        }

        // ArmBase.UpdateMesh
        void arm_base_mesh(std::vector<sprites::draw_cmd>& out, vec2 pos, vec2 dir, vec2 perp, float width, float height, float inner, vec3 col) {
            vec2 const v[12] = {
                pos - inner * 0.5f * perp - height * dir,
                pos - width * perp - height * 0.75f * dir,
                pos - width * perp + height * 0.75f * dir,
                pos - width * 0.8f * perp + height * dir,
                pos - width * 0.5f * perp + height * dir,
                pos - width * 0.3f * perp - height * 0.1f * dir,
                pos + width * 0.3f * perp - height * 0.1f * dir,
                pos + width * 0.5f * perp + height * dir,
                pos + width * 0.8f * perp + height * dir,
                pos + width * perp + height * 0.75f * dir,
                pos + width * perp - height * 0.75f * dir,
                pos + inner * 0.5f * perp - height * dir,
            };
            constexpr int tris[10][3] = {{0, 1, 2}, {0, 2, 3}, {0, 3, 4}, {0, 4, 5}, {4, 5, 7}, {6, 5, 7}, {11, 6, 7}, {11, 7, 8}, {11, 8, 9}, {11, 9, 10}};
            for (auto const& tr : tris)
                tri(out, v[tr[0]], v[tr[1]], v[tr[2]], col);
        }

        // a ring of radius `r` and line width `w` (Futile_White with the
        // VectorCircle shader: alpha the line's share of the radius)
        void ring(std::vector<sprites::draw_cmd>& out, std::vector<sprites::rope_point>& ropes, vec2 center, float r, float alpha, vec3 col) {
            if (alpha >= 1.f) {
                sprites::add_circle(out, center, r, col);
                return;
            }
            float const w = r * mathf::max(alpha, 0.f);
            float const rr = r - w * 0.5f;
            size_t const first = ropes.size();
            constexpr int n = 48;
            for (int k = 0; k <= n; ++k)
                ropes.push_back(sprites::rope_point{center + custom::deg_to_vec(float(k) * 360.f / float(n)) * rr, w * 0.5f, vec4{col.x, col.y, col.z, 1.f}});
            sprites::add_rope(out, ropes, first);
        }
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        auto& g = gs(state, c);
        id const w = oracle::id_of(state, c);
        auto const room_id = creature::room_of(state, c);
        auto const room = world::get_room_view(state, room_id);
        auto const& arm = oracle::arm_of(state, c);
        vec2 const c0 = oracle::chunk_pos(state, c, 0);
        g = graphics_state{};
        // UnityEngine.Random.InitState(56) for Pebbles (Saint's too), 10544
        rw::unity_random r{is_pebbles(w) || w == id::cl ? 56 : 10544};
        auto rnv = [&]() { return custom::deg_to_vec(r.value() * 360.f); };
        for (uint32_t i = 0; i < joint_count; ++i) {
            g.cog_rotat[i] = r.value();
            g.last_cog_rotat[i] = g.cog_rotat[i];
        }
        if (is_moon(w)) {
            for (uint32_t i = 0; i < disc_small; ++i) {
                g.disc_lengths[i] = mathf::lerp(1.f, 20.f, r.value());
                g.disc_colors[i] = r.value() < 0.5f ? 0 : uint8_t(r.range(1, 3));
                vec2 const d = rnv();
                g.disc_head_dirs[i] = d * r.value();
            }
            // Reset(firstChunk.pos)
            for (auto& sc : g.disc_cords)
                for (auto& p : sc) {
                    vec2 const d = rnv();
                    p[0] = c0 + d * r.value();
                    p[1] = p[0];
                    p[2] = vec2{0.f, 0.f};
                }
        } else {
            for (auto& p : g.coord) {
                p[0] = c0;
                p[1] = c0;
            }
            for (uint32_t j = 0; j < umb_small; ++j) {
                if (r.value() < 0.5f)
                    g.small_lengths[j] = 50.f + r.value() * 15.f;
                else
                    g.small_lengths[j] = mathf::lerp(50.f, 200.f, mathf::pow(r.value(), 1.5f));
                g.small_colors[j] = uint8_t(r.range(0, 3));
                vec2 const d = rnv();
                g.small_head_dirs[j] = d * r.value();
            }
        }
        if (has_halo(w)) {
            vec2 const mid{room.pixel_width() / 2.f, room.pixel_height() / 2.f};
            for (auto& cn : g.connections) {
                vec2 const d = rnv();
                vec2 const stuck = mid + d * mathf::lerp(300.f, 500.f, r.value());
                vec2 v = stuck;
                v.x = mathf::clamp(v.x, arm.corners[0].x, arm.corners[1].x);
                v.y = mathf::clamp(v.y, arm.corners[2].y, arm.corners[1].y);
                cn.stuck_at = rw::lerp(stuck, v, 0.5f);
                vec2 const d2 = rnv();
                cn.handle = stuck + d2 * mathf::lerp(400.f, 700.f, r.value());
            }
            g.connections_fire_chance = mathf::pow(r.value(), 3.f);
            for (auto& b : g.bits) {
                b.filled = r.value();
                b.last_filled = b.filled;
                b.get_to_filled = b.filled;
                b.fill_speed = 0.f;
            }
        }
        // the body parts start at the head end
        g.head.pos = g.head.last_pos = c0;
        g.head.rad = 5.f;
        for (uint32_t j = 0; j < 2; ++j) {
            g.hands[j].pos = g.hands[j].last_pos = c0;
            g.hands[j].rad = 2.f;
            g.feet[j].pos = g.feet[j].last_pos = c0;
            g.feet[j].rad = 2.f;
            g.knees[j][0] = g.knees[j][1] = c0;
        }
        g.expand = g.last_expand = g.get_to_expand = 1.f;
        // Oracle.Update: a hundred updates once the module exists
        g.initiated = 1;
        if (room_id)
            for (int i = 0; i < 100; ++i)
                graphics_tick(state, c, room);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        if (!gs(state, c).initiated) {
            reset_positions(state, c);
            return;
        }
        graphics_tick(state, c, room);
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip, std::vector<sprites::rope_point>& ropes) {
        auto const& g = gs(state, c);
        auto const& b = oracle::body_of(state, c);
        auto const& arm = oracle::arm_of(state, c);
        id const w = oracle::id_of(state, c);
        float const t = alpha;
        bool const conscious = oracle::consious(state, c);
        bool const moon = is_moon(w);
        bool const pebbles = is_pebbles(w);
        bool const past_moon = w == id::dm;
        bool const gown = has_gown(w, b);

        // ApplyPalette
        colors col;
        col.skin = skin_color(w);
        col.sl_like = w == id::sl || w == id::cl;
        col.sl_base_b = pal.texel(5, 1);
        col.sl_hl_b = pal.texel(5, 2);
        col.darkness = pal.darkness();
        vec3 const black = pal.black();
        col.metal = moon ? black : lerp3(black, pal.texel(5, 5), 0.12f);
        vec3 const base0 = col.base(vec2{0.f, 0.f});
        vec3 const hl0 = col.highlight(vec2{0.f, 0.f});

        vec2 const vector = lerp_chunk(state, c, 0, t);
        vec2 const c1 = lerp_chunk(state, c, 1, t);
        vec2 const vector2 = custom::dir_vec(c1, vector);
        vec2 const a = custom::perpendicular_vector(vector2);
        vec2 const a2 = rw::lerp(g.last_look_dir, g.look_dir, t);
        vec2 const vector3 = rw::lerp(g.head.last_pos, g.head.pos, t);

        // the halo (BackgroundShortcuts: behind the rest)
        if (has_halo(w) && conscious) {
            vec2 const center = vector3 + custom::dir_vec(vector, vector3) * 20.f;
            for (int k = 0; k < 2; ++k) {
                float const rad = halo_radius(g, float(k), t);
                float const al = k == 0 ? mathf::lerp(3.f / rad, 1.f, mathf::lerp(g.last_white, g.white, t)) : 3.f / rad;
                ring(out, ropes, center, rad, al, vec3{0.f, 0.f, 0.f});
            }
            for (auto const& cn : g.connections) {
                if (!(cn.last_light_up > 0.05f || cn.light_up > 0.05f))
                    continue;
                float const d = 2.f * mathf::lerp(cn.last_light_up, cn.light_up, t);
                vec2 const ad = custom::dir_vec(center, cn.stuck_at);
                size_t const first = ropes.size();
                for (int m = 0; m < 20; ++m) {
                    float const f = float(m) / 19.f;
                    vec2 const v3 = custom::bezier(cn.stuck_at, cn.handle, center + ad * halo_radius(g, 2.f, t), center + ad * 400.f, f);
                    ropes.push_back(sprites::rope_point{v3, d, vec4{0.f, 0.f, 0.f, 1.f}});
                }
                sprites::add_rope(out, ropes, first, false);
            }
            for (uint32_t n = 0; n < 3; ++n) {
                float const rot = mathf::lerp(g.ring_rotations[n][1], g.ring_rotations[n][0], t);
                float const rad = halo_radius(g, float(n) + 0.5f, t);
                for (uint32_t k = 0; k < bit_ring_size[n]; ++k) {
                    float const num3 = float(k) / float(bit_ring_size[n]) * 360.f + rot;
                    vec2 const v5 = center + custom::deg_to_vec(num3) * rad;
                    float const fill = bit_fill(g.bits[bit_ring_first[n] + k], t);
                    if (fill <= 0.f)
                        continue;
                    spr(out, "pixel", v5, num3, vec2{4.f, 8.f * fill}, vec2{0.5f, 0.5f}, vec3{0.f, 0.f, 0.f});
                }
            }
        }

        // ArmBase (its Midground sprites go in before the body's)
        vec3 const ab_col = lerp3(col.metal, base0, 0.8f);
        vec3 const ab_col2 = lerp3(col.metal, hl0, 0.8f);
        vec2 const j0 = rw::lerp(arm.joints[0].last_pos, arm.joints[0].pos, t);
        vec2 const bdir = oracle::base_dir(arm, w, t);
        vec2 const bpos = j0 - bdir * 10.f;
        vec2 const bperp = custom::perpendicular_vector(bdir);
        vec2 const elbow0 = oracle::elbow_pos(arm, 0, t, rw::lerp(arm.joints[1].last_pos, arm.joints[1].pos, t));
        vec2 circles[2];
        for (int k = 0; k < 2; ++k) {
            float const s = k == 0 ? -1.f : 1.f;
            vec2 const v4 = bpos + bdir * 11.f + bperp * 17.f * s;
            vec2 const v5 = rw::lerp(j0, elbow0, 0.25f);
            vec2 const v6 = custom::inverse_kinematic(v4, v5, 25.f, 45.f, k == 0 ? 1.f : -1.f);
            spr(out, "pixel", v4, custom::aim(v4, v6), vec2{2.f, rw::distance(v4, v6) + 10.f}, vec2{0.5f, 0.f}, col.metal);
            spr(out, "pixel", v6, custom::aim(v6, v5), vec2{2.f, rw::distance(v6, v5)}, vec2{0.5f, 0.f}, col.metal);
            spr(out, "deerEyeB", v6, 0.f, vec2{1.f, 1.f}, vec2{0.5f, 0.5f}, col.metal);
            circles[k] = bpos + 17.f * bperp * s;
        }
        arm_base_mesh(out, bpos, bdir, bperp, 30.f, 17.f, 20.f, ab_col);

        // the arm's joints (DM's and Saint's Pebbles' first cog is their
        // killSprite: replaced or hidden; Moon's goes to the front)
        bool const cog0_elsewhere = past_moon || w == id::cl || moon;
        for (uint32_t i = 0; i < joint_count; ++i)
            draw_joint(out, strip, state, c, arm, g, i, t, col, moon, i == 0 && cog0_elsewhere);

        // the umbilical cord(s)
        if (has_umbilical(w)) {
            size_t const first = ropes.size();
            for (uint32_t i = 0; i < umb_coords; ++i)
                ropes.push_back(sprites::rope_point{rw::lerp(g.coord[i][1], g.coord[i][0], t), 1.2f, vec4{black.x, black.y, black.z, 1.f}});
            sprites::add_rope(out, ropes, first, false);
            vec3 const seg0 = lerp3(base0, col.metal, 0.5f);
            vec3 const seg1 = lerp3(hl0, col.metal, 0.35f);
            vec2 prev = g.coord[0][0];
            for (uint32_t i = 0; i < umb_coords; ++i) {
                vec2 const v2 = rw::lerp(g.coord[i][1], g.coord[i][0], t);
                vec2 const v3 = custom::dir_vec(prev, v2);
                vec2 bb = v3;
                if (i + 1 < umb_coords)
                    bb = custom::dir_vec(v2, rw::lerp(g.coord[i + 1][1], g.coord[i + 1][0], t));
                float const rot = custom::vec_to_deg(rw::normalized(v3 + bb)) + 90.f;
                spr(out, "CentipedeSegment", v2, rot, vec2{0.5f, 0.3f}, vec2{0.5f, 0.5f}, seg0);
                spr(out, "CentipedeSegment", v2, rot, vec2{0.4f, 0.15f}, vec2{0.5f, 0.5f}, seg1);
                prev = v2;
            }
            for (uint32_t j = 0; j < umb_small; ++j) {
                vec3 cc = col.metal;
                if (g.small_colors[j] == 1)
                    cc = lerp3(vec3{1.f, 0.f, 0.f}, col.metal, 0.5f);
                else if (g.small_colors[j] == 2)
                    cc = lerp3(vec3{0.f, 0.f, 1.f}, col.metal, 0.5f);
                size_t const f = ropes.size();
                for (uint32_t k = 0; k < umb_small_points; ++k)
                    ropes.push_back(sprites::rope_point{rw::lerp(g.small_cords[j][k][1], g.small_cords[j][k][0], t), 0.5f, vec4{cc.x, cc.y, cc.z, 1.f}});
                sprites::add_rope(out, ropes, f, false);
            }
        } else {
            vec3 const bb = lerp3(col.metal, vec3{0.7f, 0.7f, 0.7f}, 0.15f);
            for (uint32_t j = 0; j < disc_small; ++j) {
                vec3 cc = lerp3(col.metal, bb, 0.5f);
                if (g.disc_colors[j] == 1)
                    cc = lerp3(vec3{1.f, 0.f, 0.f}, bb, 0.5f);
                else if (g.disc_colors[j] == 2)
                    cc = lerp3(vec3{0.f, 0.f, 1.f}, bb, 0.5f);
                size_t const f = ropes.size();
                for (uint32_t k = 0; k < disc_small_points; ++k)
                    ropes.push_back(sprites::rope_point{rw::lerp(g.disc_cords[j][k][1], g.disc_cords[j][k][0], t), 0.5f, vec4{cc.x, cc.y, cc.z, 1.f}});
                sprites::add_rope(out, ropes, f, false);
            }
        }

        // the body chunks and the neck
        float const breath = mathf::lerp(g.last_breath_fac, g.breath_fac, t);
        spr(out, "Circle20", vector, custom::aim(vector, vector3) - mathf::lerp(14.f, 0.f, breath), vec2{0.6f, moon || past_moon ? 0.8f : 0.6f},
            vec2{0.5f, 0.5f}, col.skin);
        spr(out, "Circle20", c1, custom::vec_to_deg(custom::dir_vec(c1, vector)), vec2{0.6f, 0.6f}, vec2{0.5f, 0.5f}, col.skin);
        spr(out, "pixel", vector, custom::aim(vector, vector3), vec2{pebbles || w == id::cl ? 3.f : 4.f, rw::distance(vector, vector3)}, vec2{0.5f, 0.f},
            col.skin);

        // hands and feet (computed here, drawn in their places)
        vec2 hand_v[2][28], foot_v[2][28];
        vec2 hand_at[2], foot_at[2];
        for (int m = 0; m < 2; ++m) {
            float const s = m == 1 ? -1.f : 1.f;
            float const s0 = m == 0 ? -1.f : 1.f;
            vec2 vector9 = rw::lerp(g.hands[m].last_pos, g.hands[m].pos, t);
            vec2 vector10 = vector + a * 4.f * s;
            if (moon)
                vector10 += vector2 * 3.f;
            vec2 cb = vector9 + custom::dir_vec(vector9, vector10) * 3.f + vector2;
            vec2 ca = vector10 + a * 5.f * s;
            hand_at[m] = vector9;
            vec2 vector11 = vector10 - a * 2.f * s;
            float num4 = pebbles ? 4.f : 2.f;
            for (int n = 0; n < 7; ++n) {
                float const f = float(n) / 6.f;
                vec2 const v12 = custom::bezier(vector10, ca, vector9, cb, f);
                vec2 const v13 = custom::dir_vec(vector11, v12);
                vec2 const a4 = custom::perpendicular_vector(v13) * s0;
                float const d = rw::distance(vector11, v12);
                hand_v[m][n * 4] = v12 - v13 * d * 0.3f - a4 * num4;
                hand_v[m][n * 4 + 1] = v12 - v13 * d * 0.3f + a4 * num4;
                hand_v[m][n * 4 + 2] = v12 - a4 * num4;
                hand_v[m][n * 4 + 3] = v12 + a4 * num4;
                vector11 = v12;
            }
            vector9 = rw::lerp(g.feet[m].last_pos, g.feet[m].pos, t);
            vector10 = c1;
            vec2 const kb = rw::lerp(g.knees[m][1], g.knees[m][0], t);
            cb = rw::lerp(vector9, kb, 0.9f);
            ca = rw::lerp(vector10, kb, 0.9f);
            foot_at[m] = vector9;
            vector11 = vector10 - a * 2.f * s;
            float num6 = 4.f;
            for (int n = 0; n < 7; ++n) {
                float const f = float(n) / 6.f;
                num4 = pebbles ? 2.f : mathf::lerp(4.f, 2.f, mathf::pow(f, 0.5f));
                vec2 const v14 = custom::bezier(vector10, ca, vector9, cb, f);
                vec2 const v15 = custom::dir_vec(vector11, v14);
                vec2 const a5 = custom::perpendicular_vector(v15) * s0;
                float const d2 = rw::distance(vector11, v14);
                foot_v[m][n * 4] = v14 - v15 * d2 * 0.3f - a5 * (num6 + num4) * 0.5f;
                foot_v[m][n * 4 + 1] = v14 - v15 * d2 * 0.3f + a5 * (num6 + num4) * 0.5f;
                foot_v[m][n * 4 + 2] = v14 - a5 * num4;
                foot_v[m][n * 4 + 3] = v14 + a5 * num4;
                vector11 = v14;
                num6 = num4;
            }
        }
        for (int m = 0; m < 2; ++m) {
            spr(out, "haloGlyph-1", foot_at[m], 0.f, vec2{1.f, 1.f}, vec2{0.5f, 0.5f}, col.skin);
            sprites::long_mesh(out, strip, foot_v[m], 28, col.skin);
        }

        // the gown
        if (gown) {
            int const n = int(gown_divs);
            auto vp = [&](int v) { return rw::lerp(g.cloth[v / n][v % n][1], g.cloth[v / n][v % n][0], t); };
            auto vc = [&](int v) {
                vec3 const cc = gown_color(w, float(v % n) / float(n - 1));
                return vec4{cc.x, cc.y, cc.z, 1.f};
            };
            // (MakeGridMesh: per cell the triangles a, b, c and b, c, d)
            for (int i = 0; i < n - 1; ++i)
                for (int j = 0; j < n - 1; ++j) {
                    int const va = j * n + i, vb = j * n + i + 1, vcc = (j + 1) * n + i, vd = (j + 1) * n + i + 1;
                    vec2 const corners[4] = {vp(vb), vp(va), vp(vcc), vp(vd)};
                    vec4 const cols[4] = {vc(vb), vc(va), vc(vcc), vc(vd)};
                    sprites::add_flat_quad(out, corners, cols);
                }
        }

        for (int m = 0; m < 2; ++m) {
            spr(out, "haloGlyph-1", hand_at[m], 0.f, vec2{1.f, 1.f}, vec2{0.5f, 0.5f}, col.skin);
            if (gown) {
                vec3 const g4 = gown_color(w, 0.4f), g0 = gown_color(w, 0.f);
                vec4 cols[28];
                for (int l = 0; l < 7; ++l) {
                    cols[l * 4] = vec4{g4.x, g4.y, g4.z, 1.f};
                    cols[l * 4 + 1] = vec4{g0.x, g0.y, g0.z, 1.f};
                    cols[l * 4 + 2] = vec4{g4.x, g4.y, g4.z, 1.f};
                    cols[l * 4 + 3] = vec4{g0.x, g0.y, g0.z, 1.f};
                }
                sprites::long_mesh(out, strip, hand_v[m], cols, 28);
            } else {
                sprites::long_mesh(out, strip, hand_v[m], 28, col.skin);
            }
        }

        // the head: phones (side 0), head, chin, eyes, phones (side 1)
        vec2 const vector4 = custom::dir_vec(vector3, vector);
        vec2 const a3 = custom::perpendicular_vector(vector4);
        vec2 const vector5 = body_parts::rotate_around_origo(a2, -custom::aim(c1, vector));
        vec2 const vector6 = rw::lerp(vector3, vector, 0.15f) + a3 * vector5.x * 2.f;
        float const num2 = mathf::lerp(g.last_eyes_open, g.eyes_open, t);
        sprite phones[2][3];
        sprite eyes[2];
        float eye_alpha = 0.5f + 0.5f * num2;
        vec3 const eye_col = moon || past_moon ? vec3{0.02f, 0.f, 0.f} : vec3{1.f, 1.f, 1.f};
        for (int m = 0; m < 2; ++m) {
            float const num3 = m == 0 ? -1.f : 1.f;
            vec2 const v7 = vector3 + a3 * mathf::clamp(vector5.x * 3.f + 2.5f * num3, -5.f, 5.f) + vector4 * (1.f - vector5.y * 3.f);
            float const sx = 1.f + (m == 0 ? mathf::inverse_lerp(-1.f, -0.5f, vector5.x) : mathf::inverse_lerp(1.f, 0.5f, vector5.x)) + (1.f - num2);
            eyes[m] = sprite{"pixel", v7, custom::vec_to_deg(vector4), vec2{sx, mathf::lerp(1.f, pebbles ? 2.f : 3.f, num2)}, vec2{0.5f, 0.5f}, eye_col};
            int const side = (m < 1) == (vector5.x < 0.f) ? 0 : 1;
            vec2 const v8 = vector3 + a3 * mathf::clamp(mathf::lerp(7.f, 5.f, std::fabs(vector5.x)) * num3, -11.f, 11.f);
            for (int n = 0; n < 2; ++n) {
                float const f = n == 0 ? 1.f : 0.8f;
                phones[side][n] = sprite{"Circle20", v8, custom::vec_to_deg(vector4),
                    vec2{mathf::lerp(3.5f, 5.f, std::fabs(vector5.x)) * f / 20.f, 5.5f * f / 20.f}, vec2{0.5f, 0.5f}, n == 0 ? base0 : hl0};
            }
            phones[side][2] = sprite{"LizardScaleA1", v8, custom::aim(vector, v8 - vector4 * 40.f - a2 * 10.f), vec2{(side == 0 ? -1.f : 1.f) * 0.75f, 0.8f},
                vec2{0.5f, 0.f}, hl0};
        }
        for (int n = 0; n < 3; ++n)
            sprites::add(out, phones[0][n]);
        spr(out, "Circle20", vector3, custom::vec_to_deg(vector4), vec2{5.f / 9.f, 5.f / 11.f}, vec2{0.5f, 0.5f}, col.skin);
        spr(out, "Circle20", vector6, 0.f, vec2{5.f / 15.f, 5.f / 15.f}, vec2{0.5f, 0.5f}, col.skin);
        for (int m = 0; m < 2; ++m)
            spr_alpha(out, eyes[m].element, eyes[m].pos, eyes[m].rotation, eyes[m].scale, eyes[m].anchor, eyes[m].color, eye_alpha);
        for (int n = 0; n < 3; ++n)
            sprites::add(out, phones[1][n]);

        // Moon's third eye, the past Moon's sigil
        if (moon || past_moon) {
            vec2 const v16 = vector3 + a3 * vector5.x * 2.5f + vector4 * (-2.f - vector5.y * 1.5f);
            vec3 const ec = lerp3(vec3{1.f, 0.f, 1.f}, col.skin, moon ? 0.5f : 0.3f);
            spr(out, "Circle20", v16, custom::aim(v16, vector3 - vector4 * 10.f),
                vec2{mathf::lerp(0.2f, 0.15f, std::fabs(vector5.x)), custom::lerp_map(vector5.y, 0.f, 1.f, 0.2f, 0.05f)}, vec2{0.5f, 0.5f}, ec);
            if (past_moon && g.sigil_alpha > 0.f)
                spr_alpha(out, "MoonSigil", v16 + vec2{0.f, 10.f}, 0.f, vec2{1.f, 1.f}, vec2{0.5f, 0.5f}, vec3{0.12156863f, 0.28627452f, 0.48235294f},
                    g.sigil_alpha);
        }

        // the Shortcuts container: the ArmBase's plates 6 and 7 and circles,
        // and Moon's first cog (her killSprite)
        arm_base_mesh(out, bpos, bdir, bperp, 30.f, 17.f, 20.f, ab_col);
        arm_base_mesh(out, bpos, bdir, bperp, 24.f, 15.f, 30.f, ab_col2);
        for (int k = 0; k < 2; ++k) {
            spr(out, "Circle20", circles[k], 0.f, vec2{0.5f, 0.5f}, vec2{0.5f, 0.5f}, ab_col);
            spr(out, "Circle20", circles[k] + vec2{-1.f, 1.f}, 0.f, vec2{0.45f, 0.45f}, vec2{0.5f, 0.5f}, ab_col2);
        }
        if (moon) {
            auto const& jt = arm.joints[0];
            vec2 const v = rw::lerp(jt.last_pos, jt.pos, t);
            vec2 const v2 = rw::lerp(arm.joints[1].last_pos, arm.joints[1].pos, t);
            vec2 const v3 = oracle::elbow_pos(arm, 0, t, v2);
            float const cog = mathf::lerp(g.last_cog_rotat[0], g.cog_rotat[0], t);
            spr(out, "pixel", v + custom::dir_vec(v, v3) * 12.f, custom::aim(v, v3) + cog * 360.f, vec2{5.f, 18.f}, vec2{0.5f, 0.5f}, col.metal);
        }
    }
}
