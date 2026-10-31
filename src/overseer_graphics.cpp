#include <algorithm>
#include <cmath>

#include "overseer_graphics.hpp"
#include "creature.hpp"
#include "overseer.hpp"
#include "overseer_types.hpp"
#include "particles.hpp"

namespace overseer_graphics {
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using sprites::sprite;
    using overseer::graphics_state;
    using overseer::mesh_segments;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        constexpr float two_pi = 6.2831855f;

        creature::overseer_payload const& pl(sys::game_state const& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).overseer;
        }
        dcon::overseer_data_id data(sys::game_state const& state, dcon::creature_id c) {
            return creature::overseer_data(state.world, c);
        }
        graphics_state& gs(sys::game_state& state, dcon::creature_id c) {
            return state.world.overseer_data_get_graphics(data(state, c));
        }
        graphics_state const& gs(sys::game_state const& state, dcon::creature_id c) {
            return state.world.overseer_data_get_graphics(data(state, c));
        }
        overseer::body_state const& bs(sys::game_state const& state, dcon::creature_id c) {
            return state.world.overseer_data_get_body(data(state, c));
        }
        dcon::body_chunk_id ch(sys::game_state const& state, dcon::creature_id c) {
            return creature::chunk(state, c, 0);
        }

        vec2 xy(vec3 v) { return vec2{v.x, v.y}; }
        float len3(vec3 v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
        vec3 lerp3(vec3 a, vec3 b, float t) {
            t = mathf::clamp01(t);
            return a + (b - a) * t;
        }
        vec4 lerp4(vec4 a, vec4 b, float t) {
            t = mathf::clamp01(t);
            return a + (b - a) * t;
        }
        // Vector3.Slerp: the direction turned, the length lerped
        vec3 slerp3(vec3 a, vec3 b, float t) {
            t = mathf::clamp01(t);
            float const la = len3(a), lb = len3(b);
            if (la < 1e-6f || lb < 1e-6f)
                return lerp3(a, b, t);
            vec3 const na = a / la, nb = b / lb;
            float const d = mathf::clamp(na.x * nb.x + na.y * nb.y + na.z * nb.z, -1.f, 1.f);
            float const ang = std::acos(d);
            if (ang < 1e-5f)
                return lerp3(a, b, t);
            vec3 dir;
            if (ang > mathf::pi - 1e-4f) {
                // opposite: around any axis at right angles
                vec3 const ref = std::fabs(na.x) < 0.9f ? vec3{1.f, 0.f, 0.f} : vec3{0.f, 1.f, 0.f};
                vec3 o{na.y * ref.z - na.z * ref.y, na.z * ref.x - na.x * ref.z, na.x * ref.y - na.y * ref.x};
                o /= len3(o);
                dir = na * std::cos(t * ang) + o * std::sin(t * ang);
            } else {
                float const s = std::sin(ang);
                dir = na * (std::sin((1.f - t) * ang) / s) + nb * (std::sin(t * ang) / s);
            }
            return dir * mathf::lerp(la, lb, t);
        }
        vec4 rgba(vec3 c, float a) { return vec4{c.x, c.y, c.z, a}; }
        void add_sprite(std::vector<sprites::draw_cmd>& out, sprite const& s, vec4 col) {
            vec4 const corners[4] = {col, col, col, col};
            sprites::add(out, s, corners);
        }

        // what UpdateDrawPositions works out for a timeStacker
        struct draw_pos {
            vec2 use_pos{0.f, 0.f};
            vec2 use_look_at{0.f, 0.f};
            vec3 use_dir{0.f, 0.f, 0.f};
            vec2 use_root_pos{0.f, 0.f};
            vec2 use_root_dir{0.f, 0.f};
            float z_start = 0.f;
            float z_end = 0.f;
            // Lerp(lastExtended, extended, timeStacker)
            float ext = 1.f;
        };
        draw_pos update_draw_positions(sys::game_state const& state, dcon::creature_id c, float ts) {
            auto const& w = state.world;
            auto const& p = pl(state, c);
            auto const& b = bs(state, c);
            auto const& g = gs(state, c);
            draw_pos d;
            d.ext = mathf::lerp(p.last_extended, p.extended, ts);
            d.use_pos = rw::lerp(w.body_chunk_get_last_pos(ch(state, c)), w.body_chunk_get_pos(ch(state, c)), ts);
            d.use_look_at = rw::lerp(g.last_cosmetic_look_at, g.cosmetic_look_at, ts);
            vec2 const dv = custom::dir_vec(d.use_pos, d.use_look_at);
            d.use_dir = slerp3(vec3{dv.x, dv.y, 0.f}, vec3{0.f, 0.f, 1.f},
                            mathf::inverse_lerp(600.f, 0.f, rw::distance(d.use_pos, d.use_look_at)) * (1.f - mathf::lerp(g.last_convo_mode, g.convo_mode, ts)))
                * d.ext;
            d.use_root_pos = rw::lerp(b.last_root_pos, b.root_pos, ts);
            d.use_root_dir = rw::slerp(b.last_root_dir, b.root_dir, ts);
            float const num = 0.5f / float(std::max<uint16_t>(1, b.path_count));
            constexpr uint32_t n = overseer::zip_prog_count;
            d.z_start = mathf::min(1.f - num * 3.f, custom::s_curve(mathf::lerp(b.zip_progs[n - 1], b.zip_progs[n - 2], ts), 0.5f));
            d.z_end = mathf::max(num, custom::s_curve(mathf::lerp(b.zip_progs[1], b.zip_progs[0], ts), 0.5f));
            return d;
        }

        // ExtensionOfSegment
        float extension_of_segment(float f, draw_pos const& d) {
            return mathf::pow(mathf::max(0.f, d.ext), mathf::lerp(0.1f, 4.f, f));
        }
        // PositionOfZipProg
        vec2 position_of_zip_prog(overseer::body_state const& b, float f) {
            f = 1.f - f;
            int32_t const count = int32_t(std::max<uint16_t>(1, b.path_count));
            int32_t const num = custom::int_clamp(int32_t(f * float(count) - 1.f), 0, count - 1);
            int32_t const num2 = custom::int_clamp(int32_t(f * float(count) - 1.f) + 1, 0, count - 1);
            return rw::lerp(world::room_view::middle_of_tile(b.path[num]), world::room_view::middle_of_tile(b.path[num2]),
                mathf::inverse_lerp(float(num), float(num2), f * float(count) - 1.f));
        }
        // DrawPosOfSegment: 0 the eye, 1 the root
        vec2 draw_pos_of_segment(sys::game_state const& state, dcon::creature_id c, float f, draw_pos const& d) {
            auto const& p = pl(state, c);
            vec2 const bz = custom::bezier(d.use_pos, d.use_pos - xy(d.use_dir) * 15.f * p.size * d.ext, d.use_root_pos,
                d.use_root_pos + d.use_root_dir * mathf::max(40.f * p.size, rw::distance(d.use_pos, d.use_root_pos) * 0.85f) * d.ext, f);
            vec2 const zip = position_of_zip_prog(bs(state, c), mathf::lerp(d.z_end, d.z_start, p.zip_mesh_direction ? f : 1.f - f));
            return rw::lerp(zip, bz, extension_of_segment(f, d));
        }
        // RadOfSegment
        float rad_of_segment(sys::game_state const& state, dcon::creature_id c, float f, float ts, draw_pos const& d) {
            auto const& p = pl(state, c);
            float const total = gs(state, c).total_length;
            float const r = mathf::lerp(1.f + std::sin(f * mathf::pi),
                mathf::lerp(5.f, 0.f, mathf::pow(f, custom::lerp_map(total, 20.f * p.size, 90.f * p.size, 1.75f, 0.35f))), extension_of_segment(f, d));
            return r * mathf::pow(1.f - mathf::lerp(p.last_dying, p.dying, ts), 0.2f);
        }
        // ColorOfSegment (with its alpha)
        vec4 color_of_segment(sys::game_state const& state, dcon::creature_id c, float f, float ts, draw_pos const& d, vec3 main, vec3 earth) {
            auto const& p = pl(state, c);
            vec3 const neutral = (vec3{1.f, 1.f, 1.f} + earth) / 2.f;
            vec4 const a = rgba((main + vec3{0.f, 0.f, 1.f} + earth * 8.f) / 10.f, 0.5f);
            vec4 const b = rgba(lerp3(main, lerp3(neutral, earth, mathf::pow(f, 2.f)), 0.5f), 1.f);
            return lerp4(lerp4(a, b, extension_of_segment(f, d)), rgba(main, 0.f), mathf::lerp(p.last_dying, p.dying, ts));
        }

        // Mycelium.Reset (ResetDir: Custom.RNV)
        template<typename R>
        void reset_mycelium(overseer::mycelium& m, vec2 at, R& r) {
            vec2 const v = at + custom::rnv(r) * (m.length * 0.6f);
            float const dist = rw::distance(at, v);
            vec2 const ca = at + rw::normalized(custom::dir_vec(at, v) + custom::rnv(r)) * dist * 0.5f;
            vec2 const cb = v + rw::normalized(custom::dir_vec(v, at) + custom::rnv(r)) * dist * 0.5f;
            for (uint32_t i = 0; i < m.count; ++i) {
                m.pos[i] = custom::bezier(at, ca, v, cb, float(i) / float(m.count - 1)) + custom::rnv(r);
                m.last_pos[i] = m.pos[i];
                m.vel[i] = custom::rnv(r);
            }
        }

        // MyceliaPosTo2D
        vec2 mycelia_pos_to_2d(vec3 position, float ext, float ur_x, float ur_y, float ur_z) {
            position *= ext;
            float const x = position.x * std::cos(ur_z * two_pi) - position.y * std::sin(ur_z * two_pi);
            position.y = position.x * std::sin(ur_z * two_pi) + position.y * std::cos(ur_z * two_pi);
            position.x = x;
            float const y = position.y * std::cos(ur_x * two_pi) - position.z * std::sin(ur_x * two_pi);
            position.z = position.y * std::sin(ur_x * two_pi) + position.z * std::cos(ur_x * two_pi);
            position.y = y;
            float const z = position.z * std::cos(ur_y * two_pi) - position.x * std::sin(ur_y * two_pi);
            position.x = position.z * std::sin(ur_y * two_pi) + position.x * std::cos(ur_y * two_pi);
            position.z = z;
            return vec2{position.x, position.y};
        }
        // HologramMatrix.OnPlanePos
        vec2 on_plane_pos(vec3 position, float ur_x, float ur_y, float ur_z, float fold) {
            position.x *= 1.f - fold;
            position.y *= 1.f - fold;
            position.z *= 1.f + fold * 5.f;
            return mycelia_pos_to_2d(position, 1.f, ur_x, ur_y, ur_z);
        }

        // HologramMatrix.GenerateShape
        void generate_shape(graphics_state& g, rw::unity_random& r, int32_t mycelia, float size) {
            g.line_count = 0;
            auto const add = [&](vec3 a, vec3 alt_a, vec3 b, vec3 alt_b, uint8_t rotation, uint8_t movement) {
                if (g.line_count >= overseer::max_holo_lines)
                    return;
                auto& l = g.lines[g.line_count++];
                l.a = a;
                l.alt_a = alt_a;
                l.b = b;
                l.alt_b = alt_b;
                l.rotation = rotation;
                l.movement = movement;
            };
            auto const dv = [](float deg, float rad, float z) {
                vec2 const v = custom::deg_to_vec(deg) * rad;
                return vec3{v.x, v.y, z};
            };
            int32_t num = r.range(3, 7);
            if (r.value() < 0.5f && mycelia != 4 && mycelia > 2)
                num = mycelia;
            float const num2 = mathf::lerp(4.f, 8.f, mathf::pow(r.value(), 0.5f));
            float num3 = mathf::lerp(12.f, 20.f, r.value());
            float const num4 = mathf::min(num2, mathf::lerp(3.f, 8.f, r.value()));
            float const z = num3 + mathf::lerp(5.f, 12.f, r.value());
            float const num5 = mathf::lerp(num2 * 0.5f, 4.f, 0.5f + r.value() * 0.5f);
            float num6 = mathf::lerp(12.f, 20.f, r.value());
            float const num7 = mathf::lerp(num4 * 0.5f, 3.f, 0.5f + r.value() * 0.5f);
            float const z2 = num6 + mathf::lerp(10.f, 20.f, mathf::pow(r.value(), 0.5f));
            for (int32_t i = 1; i < num + 1; ++i) {
                float const a6 = float(i) * 360.f / float(num), a7 = float(i + 1) * 360.f / float(num);
                add(dv(a6, num2, num3), dv(a6, num5, num6), dv(a7, num2, num3), dv(a7, num5, num6), 0, 0);
                add(dv(a6, num4, z), dv(a6, num7, z2), dv(a7, num4, z), dv(a7, num7, z2), 0, 0);
                add(dv(a6, num2, num3), dv(a6, num5, num6), dv(a6, num4, z), dv(a6, num7, z2), 0, 0);
            }
            int32_t num8 = r.range(3, 9);
            if (num == 3)
                num8 = 6;
            if (r.value() < 0.5f) {
                int32_t const hi = r.range(3, 9);
                num8 = r.range(3, hi);
            }
            if (r.value() < 0.5f)
                num8 = num;
            num8 = std::clamp(num8, 1, int32_t(overseer::max_holo_movements) - 3);
            num3 = mathf::lerp(7.f, 16.f, r.value());
            float const num9 = mathf::lerp(7.f, 25.f, r.value());
            float num10 = mathf::lerp(0.2f, 0.6f, mathf::pow(r.value(), 1.5f));
            float const num11 = num10 * mathf::lerp(1.f, 0.25f, mathf::pow(r.value(), 1.5f));
            float const num12 = mathf::lerp(num2 + 2.f, 11.f, r.value());
            float num13 = mathf::lerp(num12 + 5.f, 18.f, mathf::pow(r.value(), custom::lerp_map(float(num8), 3.f, 8.f, 1.f, 5.f)));
            int32_t num14 = r.range(0, 5);
            if (r.value() < 0.33333334f)
                num14 = 2;
            if (num14 == 0) {
                num13 = mathf::lerp(num13, 18.f, r.value());
                num10 = mathf::lerp(num10, 0.6f, r.value());
            }
            float const t = mathf::lerp(0.2f, 0.8f, mathf::pow(r.value(), num14 == 3 ? 0.5f : 1.f));
            float const d = mathf::lerp(num12, num13, t);
            float z3 = mathf::lerp(num3, num9, t);
            {
                float const a = mathf::pow(r.value(), 3.f);
                float const b = r.value() < 0.85f ? 5.f : 10.f;
                float const s = r.value() < 0.85f ? -1.f : 1.f;
                z3 += a * b * s;
            }
            {
                float const to = mathf::lerp(7.f, 16.f, r.value());
                num6 = mathf::lerp(num3, to, r.value());
            }
            float num15;
            {
                float const a = mathf::lerp(11.f, 20.f, r.value());
                float const s = r.value() < 0.15f ? -1.f : 1.f;
                float const b = mathf::lerp(11.f, 35.f, r.value());
                num15 = mathf::lerp(num9 + a * s, b, r.value());
            }
            float const num16 = mathf::lerp(0.2f, 0.6f, mathf::pow(r.value(), 1.5f));
            float const num17 = num16 * mathf::lerp(1.f, 0.25f, mathf::pow(r.value(), 1.5f));
            float const num18 = mathf::lerp(num2, 16.f, r.value());
            float const num19 = mathf::lerp(num18 + 5.f, 18.f, mathf::pow(r.value(), custom::lerp_map(float(num8), 3.f, 8.f, 1.f, 5.f)))
                * custom::lerp_map(std::fabs(num15), 0.f, 25.f, 1.5f, 0.5f);
            float const d2 = mathf::lerp(num18, num19, t);
            float z4 = mathf::lerp(num6, num15, t);
            {
                float const a = mathf::pow(r.value(), 3.f);
                float const b = r.value() < 0.85f ? 5.f : 10.f;
                float const s = r.value() < 0.85f ? -1.f : 1.f;
                z4 += a * b * s;
            }
            float const k = 180.f / float(num8);
            for (int32_t j = 0; j < num8; ++j) {
                float const a = float(j) / float(num8) * 360.f;
                uint8_t const mv = uint8_t(2 + j);
                switch (num14) {
                case 0: {
                    vec3 const v6 = dv(a - k * num10, num12, num3), v7 = dv(a + k * num10, num12, num3), v8 = dv(a, num13, num9);
                    vec3 const p1 = dv(a - k * num16, num18, num6), p2 = dv(a + k * num16, num18, num6), p3 = dv(a, num19, num15);
                    add(v6, p1, v7, p2, 1, mv);
                    add(v7, p2, v8, p3, 1, mv);
                    add(v8, p3, v6, p1, 1, mv);
                    break;
                }
                case 1: {
                    vec3 const v6 = dv(a, num12, num3), v7 = dv(a - k * num10, num13, num9), v8 = dv(a + k * num10, num13, num9);
                    vec3 const p1 = dv(a, num18, num6), p2 = dv(a - k * num16, num19, num15), p3 = dv(a + k * num16, num19, num15);
                    add(v6, p1, v7, p2, 1, mv);
                    add(v7, p2, v8, p3, 1, mv);
                    add(v8, p3, v6, p1, 1, mv);
                    break;
                }
                case 2: {
                    vec3 const v6 = dv(a - k * num10, num12, num3), v7 = dv(a - k * num11, num13, num9), v8 = dv(a + k * num11, num13, num9),
                               v9 = dv(a + k * num10, num12, num3);
                    vec3 const p1 = dv(a - k * num16, num18, num6), p2 = dv(a - k * num17, num19, num15), p3 = dv(a + k * num17, num19, num15),
                               p4 = dv(a + k * num16, num18, num6);
                    add(v6, p1, v7, p2, 1, mv);
                    add(v7, p2, v8, p3, 1, mv);
                    add(v8, p3, v9, p4, 1, mv);
                    add(v9, p4, v6, p1, 1, mv);
                    break;
                }
                case 3: {
                    vec3 const v6 = dv(a - k * num10, num12, num3), v7 = dv(a - k * num11, d, z3), v8 = dv(a, num13, num9), v9 = dv(a + k * num11, d, z3),
                               v10 = dv(a + k * num10, num12, num3);
                    vec3 const p1 = dv(a - k * num16, num18, num6), p2 = dv(a - k * num17, d2, z4), p3 = dv(a, num19, num15), p4 = dv(a + k * num17, d2, z4),
                               p5 = dv(a + k * num16, num18, num6);
                    add(v6, p1, v7, p2, 1, mv);
                    add(v7, p2, v8, p3, 1, mv);
                    add(v8, p3, v9, p4, 1, mv);
                    add(v9, p4, v10, p5, 1, mv);
                    add(v10, p5, v6, p1, 1, mv);
                    break;
                }
                default: {
                    vec3 const v6 = dv(a, num12, num3), v7 = dv(a - k * num10, d, z3), v8 = dv(a - k * num11, num13, num9), v9 = dv(a + k * num11, num13, num9),
                               v10 = dv(a + k * num10, d, z3);
                    vec3 const p1 = dv(a, num18, num6), p2 = dv(a - k * num16, d2, z4), p3 = dv(a - k * num17, num19, num15), p4 = dv(a + k * num17, num19, num15),
                               p5 = dv(a + k * num16, d2, z4);
                    add(v6, p1, v7, p2, 1, mv);
                    add(v7, p2, v8, p3, 1, mv);
                    add(v8, p3, v9, p4, 1, mv);
                    add(v9, p4, v10, p5, 1, mv);
                    add(v10, p5, v6, p1, 1, mv);
                    break;
                }
                }
            }
            float const d3 = mathf::lerp(1.f, size, r.value() * 0.5f);
            for (uint32_t i = 0; i < g.line_count; ++i) {
                auto& l = g.lines[i];
                l.a *= d3;
                l.alt_a *= d3;
                l.b *= d3;
                l.alt_b *= d3;
            }
            g.movement_count = uint8_t(3 + num8);
            for (uint32_t l = 0; l < g.movement_count; ++l) {
                for (auto& v : g.movements[l])
                    v = 0.f;
                g.movements[l][0] = r.value();
                g.movements[l][1] = g.movements[l][0];
                g.movements[l][4] = 1.f;
            }
        }

        // HologramMatrix.Update
        void holo_update(graphics_state& g, rw::rng& rng) {
            if (g.holo_lens_up == 0.f && g.last_holo_lens_up == 0.f)
                return;
            for (uint32_t i = 0; i < 2; ++i) {
                auto& m = g.rotations[i];
                m[1] = m[0];
                m[0] = mathf::lerp(m[2], m[3], custom::s_curve(m[4], 0.5f));
                m[4] = mathf::min(1.f, m[4] + m[5]);
                if (rng.value() < 1.f / (i == 0 ? 40.f : 70.f) && m[4] == 1.f) {
                    m[4] = 0.f;
                    m[2] = m[0];
                    float const a = mathf::pow(rng.value(), 3.f) * 0.8f;
                    m[3] = m[0] + a * (rng.value() < 0.5f ? -1.f : 1.f);
                    m[5] = 0.02f / std::fabs(m[2] - m[3]);
                }
            }
            for (uint32_t j = 0; j < g.movement_count; ++j) {
                auto& m = g.movements[j];
                m[1] = m[0];
                m[0] = mathf::lerp(m[2], m[3], custom::s_curve(m[4], 0.5f));
                m[4] = mathf::min(1.f, m[4] + m[5]);
                if (rng.value() < 0.05f && m[4] == 1.f) {
                    m[4] = 0.f;
                    m[2] = m[0];
                    if (j == 0)
                        m[3] = rng.value();
                    else if (j == 2)
                        m[3] = rng.value() < 0.5f ? 0.f : 1.f;
                    else
                        m[3] = rng.value() < 0.5f ? 0.f : rng.value();
                    m[5] = 0.02f / std::fabs(m[2] - m[3]);
                }
            }
            if (rng.value() < 0.025f)
                for (uint32_t k = 1; k < g.movement_count; ++k) {
                    auto& m = g.movements[k];
                    m[4] = 0.f;
                    m[2] = m[0];
                    m[3] = 0.f;
                    m[5] = 0.02f / std::fabs(m[2] - m[3]);
                }
        }

        // Mycelium.Update (no CoralNeuronSystem, no connections, no culling)
        void mycelium_update(overseer::mycelium& m, vec2 connection) {
            uint32_t const n = m.count;
            vec2 p0[overseer::max_mycelium_points], p2[overseer::max_mycelium_points];
            for (uint32_t i = 0; i < n; ++i) {
                p0[i] = m.pos[i] + m.vel[i];
                m.last_pos[i] = m.pos[i];
                p2[i] = m.vel[i] * 0.999f;
            }
            for (uint32_t i = 1; i < n; ++i) {
                vec2 const f = p0[i] - p0[i - 1];
                float const num = rw::magnitude(f);
                vec2 const d = rw::normalized(f) * (m.con_rad - num) * 0.5f;
                p0[i] += d;
                p2[i] += d;
                p0[i - 1] -= d;
                p2[i - 1] -= d;
                if (i > 1) {
                    vec2 const s = rw::normalized(p0[i] - p0[i - 2]);
                    p2[i] += s * 0.2f;
                    p2[i - 2] -= s * 0.2f;
                }
            }
            for (uint32_t i = 0; i < n; ++i) {
                m.pos[i] = p0[i];
                m.vel[i] = p2[i];
            }
            m.pos[0] = connection;
            m.vel[0] = vec2{0.f, 0.f};
            if (m.rest > 0)
                --m.rest;
        }
    }

    vec3 main_color(sys::game_state const& state, dcon::creature_id c) noexcept {
        switch (pl(state, c).owner_iterator) {
        case 1: return vec3{1.f, 0.8f, 0.3f};
        case 2: return vec3{0.f, 1.f, 0.f};
        case 3: return vec3{1.f, 0.2f, 0.f};
        case 4: return vec3{0.9f, 0.95f, 1.f};
        case 5: return vec3{0.56f, 0.27f, 0.68f};
        default: return vec3{0.44705883f, 0.9019608f, 0.76862746f};
        }
    }

    void setup(sys::game_state& state, dcon::creature_id c, int32_t seed) {
        auto& g = gs(state, c);
        g = graphics_state{};
        float const size = pl(state, c).size;
        rw::unity_random r{seed};
        int32_t n;
        {
            float const r1 = r.value();
            float const r2 = mathf::pow(r.value(), 0.5f);
            float const r3 = r.value();
            n = int32_t(mathf::lerp(mathf::lerp(2.f, 6.f, mathf::lerp(r1, mathf::inverse_lerp(0.5f, 1.f, size), r2)), 4.f, r3));
        }
        n = std::clamp(n, 1, int32_t(overseer::max_mycelia));
        g.mycelia_count = uint8_t(n);
        float const length = custom::lerp_map(size, 0.5f, 1.f, 55.f, 80.f);
        float const num = mathf::max(length, mathf::lerp(length, 40.f, 0.5f));
        int32_t const points = std::min(custom::int_clamp(int32_t(num / 15.f), 2, 20), int32_t(overseer::max_mycelium_points));
        vec2 const at = state.world.body_chunk_get_pos(ch(state, c));
        for (int32_t i = 0; i < n; ++i) {
            auto& m = g.mycelia[i];
            m.count = uint8_t(points);
            m.length = length;
            m.con_rad = length / float(points);
            m.rest = 0;
            reset_mycelium(m, at, r);
        }
        g.mycelia_con_rad = g.mycelia[0].con_rad / 2.f;
        // HologramMatrix
        generate_shape(g, r, n, size);
        for (uint32_t i = 0; i < 2; ++i) {
            g.rotations[i][0] = r.value();
            g.rotations[i][1] = g.rotations[i][0];
            g.rotations[i][4] = 1.f;
        }
        g.mycelia_stuck_at = custom::lerp_map(size, 0.5f, 1.f, 0.5f, 0.2f);
        g.color_mycelia_from = custom::lerp_map(size, 0.5f, 1.f, 0.5f, 0.3f);
        for (uint32_t j = 0; j < 2; ++j) {
            g.mycelia_movements[j][0] = r.value();
            g.mycelia_movements[j][3] = 1.f;
        }
        float const lens = state.world.overseer_data_get_ai(data(state, c)).bring_up_lens;
        g.holo_lens_up = lens < 0.5f ? 0.f : 1.f;
        g.last_holo_lens_up = g.holo_lens_up;
        g.cosmetic_look_at = g.last_cosmetic_look_at = overseer::cosmetic_look_at(state, c);
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        auto& g = gs(state, c);
        g.cosmetic_look_at = g.last_cosmetic_look_at = overseer::cosmetic_look_at(state, c);
        emerge(state, c);
    }

    void emerge(sys::game_state& state, dcon::creature_id c) {
        auto& g = gs(state, c);
        auto& rng = state.world.creature_get_graphics_random(c);
        draw_pos const d = update_draw_positions(state, c, 1.f);
        vec2 const at = draw_pos_of_segment(state, c, 0.f, d);
        for (uint32_t i = 0; i < g.mycelia_count; ++i)
            reset_mycelium(g.mycelia[i], at, rng);
    }

    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room) {
        auto& w = state.world;
        auto& g = gs(state, c);
        auto const& p = pl(state, c);
        auto const& b = bs(state, c);
        auto const& ai = w.overseer_data_get_ai(data(state, c));
        auto& rng = w.creature_get_graphics_random(c);
        if (p.offscreen)
            return;
        draw_pos const d = update_draw_positions(state, c, 1.f);
        vec2 const pos = w.body_chunk_get_pos(ch(state, c));
        vec2 const vel = w.body_chunk_get_vel(ch(state, c));
        g.last_cosmetic_look_at = g.cosmetic_look_at;
        g.cosmetic_look_at = overseer::cosmetic_look_at(state, c);
        g.last_convo_mode = g.convo_mode;
        g.convo_mode = p.mode == overseer::mode::conversing ? mathf::min(1.f, g.convo_mode + 0.033333335f) : mathf::max(0.f, g.convo_mode - 0.033333335f);
        g.last_holo_lens_up = g.holo_lens_up;
        if (ai.bring_up_lens > 0.7f || (ai.bring_up_lens >= 0.3f && g.holo_lens_up > 0.5f))
            g.holo_lens_up = mathf::min(1.f, g.holo_lens_up + 0.033333335f);
        else
            g.holo_lens_up = mathf::max(0.f, g.holo_lens_up - 0.033333335f);
        g.holo_lens_up = mathf::min(g.holo_lens_up, mathf::min(1.f - g.convo_mode, 1.f - p.dying));
        if (p.mode == overseer::mode::zipping)
            g.holo_lens_up = ai.bring_up_lens < 0.5f ? 0.f : 1.f;
        vec3 const main = main_color(state, c);
        if (p.dying > 0.f) {
            // (the OverseerEffect glows aren't ported) the sparks
            for (int j = 0; j < 3; ++j) {
                vec2 const at = draw_pos_of_segment(state, c, rng.value(), d);
                vec2 const r = custom::rnv(rng);
                vec2 const v = vel * 0.5f + r * 14.f * rng.value();
                particles::spawn(state, room.id, particles::kind::spark, at, v, 1.f, mathf::lerp(14.f, 21.f, rng.value()) / 40.f, main);
            }
            g.cosmetic_look_at = pos + custom::dir_vec(b.root_pos, pos) * 1000.f;
        }
        holo_update(g, rng);
        // (no Overseer_Zip_LOOP)
        for (uint32_t k = 0; k < 2; ++k) {
            auto& m = g.mycelia_movements[k];
            m[0] = mathf::lerp(m[1], m[2], custom::s_curve(m[3], 0.5f));
            m[3] = mathf::min(1.f, m[3] + m[4]);
            if (rng.value() < 1.f / (k == 0 ? 80.f : 20.f) && m[3] == 1.f) {
                m[1] = m[0];
                m[3] = 0.f;
                if (k == 0) {
                    float const a = mathf::pow(rng.value(), 3.f) * 0.8f;
                    m[2] = m[0] + a * (rng.value() < 0.5f ? -1.f : 1.f);
                    m[4] = 0.01f / std::fabs(m[1] - m[2]);
                } else {
                    m[2] = rng.value();
                    m[4] = 0.02f / std::fabs(m[1] - m[2]);
                }
            }
        }
        float const ur_x = -d.use_dir.y / two_pi;
        float const ur_y = d.use_dir.x / two_pi;
        vec2 const connection = draw_pos_of_segment(state, c, g.mycelia_stuck_at, d);
        auto const& mm = g.mycelia_movements;
        for (uint32_t l = 0; l < g.mycelia_count; ++l) {
            auto& m = g.mycelia[l];
            mycelium_update(m, connection);
            m.con_rad = g.mycelia_con_rad * p.extended * mathf::lerp(1.f, 2.f - p.size, g.convo_mode);
            vec2 v = custom::deg_to_vec(float(l) / float(g.mycelia_count) * 360.f);
            v *= mathf::lerp(25.f, 15.f, mm[1][0]) * p.size;
            vec2 const v2 = pos
                + mycelia_pos_to_2d(vec3{v.x, v.y, mm[1][0] * -25.f * p.size - custom::lerp_map(p.size, 0.5f, 1.f, 5.f, 0.f)}, p.extended, ur_x, ur_y, mm[0][0]);
            vec2 const dir = custom::dir_vec(pos, v2);
            vec2 const toward = xy(slerp3(-d.use_dir, vec3{dir.x, dir.y, 0.f}, 0.5f));
            for (uint32_t i = 0; i < m.count; ++i) {
                float const num = float(i) / float(m.count - 1);
                m.vel[i] -= vel;
                m.vel[i] *= 0.9f * p.extended;
                m.vel[i] += vel;
                m.vel[i] += toward * (1.f - num);
                m.vel[i] += (v2 - m.pos[i]) * 0.05f * std::sin(num * mathf::pi);
                m.vel[i] += xy(d.use_dir) * mathf::pow(num, 2.f) * 0.5f;
                m.pos[i] = rw::lerp(m.pos[i], connection, 1.f - p.extended);
            }
        }
        // totalLength, as DrawSprites lays the body mesh out
        {
            vec2 v2 = draw_pos_of_segment(state, c, 1.f, d);
            v2 += custom::dir_vec(draw_pos_of_segment(state, c, 0.999f, d), v2);
            float total = 0.f;
            for (uint32_t i = 0; i < mesh_segments; ++i) {
                vec2 const v3 = draw_pos_of_segment(state, c, 1.f - float(i) / float(mesh_segments - 1), d);
                total += rw::distance(v2, v3);
                v2 = v3;
            }
            g.total_length = total;
        }
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip) {
        auto const& p = pl(state, c);
        if (p.offscreen)
            return;
        auto const& g = gs(state, c);
        // UnityEngine.Random in DrawSprites: the lens's flicker
        rw::rng rng{(state.tick * 2654435761u) ^ (uint32_t(c.index()) * 40503u) ^ 0x0E5EE7u};
        draw_pos const d = update_draw_positions(state, c, alpha);
        vec3 const main = main_color(state, c);
        // ApplyPalette: earthColor
        vec3 const earth = pal.texel(0, 2);
        auto const color = [&](float f) { return color_of_segment(state, c, f, alpha, d, main, earth); };
        auto const dp = [&](float f) { return draw_pos_of_segment(state, c, f, d); };
        float const num = d.ext;
        vec2 vector = dp(0.f);
        if (num < 1.f)
            vector += custom::dir_vec(vector, dp(0.001f)) * 5.f * p.size * (1.f - num);
        vec3 const ud = d.use_dir;
        float const deg = custom::vec_to_deg(xy(ud));
        float const rad0 = rad_of_segment(state, c, 0.f, alpha, d);
        vec4 const col0 = color(0.f);

        // the body mesh (MeshSprite, its copy BkgMeshSprite behind)
        vec2 verts[mesh_segments * 4];
        vec4 cols[mesh_segments * 4];
        {
            float num2 = rad_of_segment(state, c, 1.f, alpha, d);
            vec2 v2 = dp(1.f);
            v2 += custom::dir_vec(dp(0.999f), v2);
            for (uint32_t i = 0; i < mesh_segments; ++i) {
                float const f = 1.f - float(i) / float(mesh_segments - 1);
                vec2 const v3 = dp(f);
                float const num4 = rad_of_segment(state, c, f, alpha, d);
                vec2 const n = rw::normalized(v2 - v3);
                vec2 const a = custom::perpendicular_vector(n);
                float const num5 = rw::distance(v2, v3) / 5.f;
                verts[i * 4] = v2 - n * num5 - a * (num4 + num2) * 0.5f;
                verts[i * 4 + 1] = v2 - n * num5 + a * (num4 + num2) * 0.5f;
                verts[i * 4 + 2] = v3 + n * num5 - a * num4;
                verts[i * 4 + 3] = v3 + n * num5 + a * num4;
                v2 = v3;
                num2 = num4;
            }
            constexpr uint32_t nv = mesh_segments * 4;
            for (uint32_t k = 0; k < nv; ++k)
                cols[k] = color(1.f - float(k) / float(nv - 1));
        }
        vec2 const bulb_scale{rad0 / 10.f, rad0 * mathf::lerp(1.2f, 1.f, ud.z) / 10.f};

        // BkgMeshSprite, BkgBulbSprite (the Items container)
        sprites::long_mesh(out, strip, verts, cols, mesh_segments * 4);
        add_sprite(out, sprite{"Circle20", vector, deg, bulb_scale, vec2{0.5f, 0.5f}, main}, col0);
        // GlowSprite (Futile_White with the FlatLight shader, 50 px across):
        // a soft disc
        {
            constexpr int segs = 16;
            constexpr float r = 25.f;
            vec4 const inner = rgba(main, 0.25f * num), outer = rgba(main, 0.f);
            for (int k = 0; k < segs; ++k) {
                float const a0 = float(k) / segs * two_pi, a1 = float(k + 1) / segs * two_pi;
                sprites::draw_cmd cmd;
                cmd.is_triangle = true;
                cmd.vertex_colors = true;
                cmd.tri[0] = vector;
                cmd.tri[1] = vector + vec2{std::cos(a0), std::sin(a0)} * r;
                cmd.tri[2] = vector + vec2{std::cos(a1), std::sin(a1)} * r;
                cmd.vcol[0] = inner;
                cmd.vcol[1] = outer;
                cmd.vcol[2] = outer;
                out.push_back(cmd);
            }
        }
        // MeshSprite, BulbSprite
        sprites::long_mesh(out, strip, verts, cols, mesh_segments * 4);
        add_sprite(out, sprite{"Circle20", vector, deg, bulb_scale, vec2{0.5f, 0.5f}, main}, col0);
        // the mycelia, each drawn twice (doubleRenderMycelia)
        if (p.extended > 0.f) {
            vec4 const mcol = color(g.mycelia_stuck_at);
            vec4 const tip = rgba(custom::hsl2rgb(0.4888889f, 0.5f, 0.2f), 1.f);
            vec2 const connection = draw_pos_of_segment(state, c, g.mycelia_stuck_at, d);
            for (uint32_t l = 0; l < g.mycelia_count; ++l) {
                auto const& m = g.mycelia[l];
                vec2 mv[overseer::max_mycelium_points * 4];
                vec4 mc[overseer::max_mycelium_points * 4];
                vec2 a = connection;
                for (uint32_t i = 0; i < m.count; ++i) {
                    vec2 const v = rw::lerp(m.last_pos[i], m.pos[i], alpha);
                    vec2 const n = rw::normalized(a - v);
                    vec2 const a2 = custom::perpendicular_vector(n);
                    float const d2 = rw::distance(a, v) / 5.f;
                    mv[i * 4] = a - n * d2 - a2 * 0.5f;
                    mv[i * 4 + 1] = a - n * d2 + a2 * 0.5f;
                    mv[i * 4 + 2] = v + n * d2 - a2 * 0.5f;
                    mv[i * 4 + 3] = v + n * d2 + a2 * 0.5f;
                    a = v;
                }
                uint32_t const nv = uint32_t(m.count) * 4;
                // Mycelium.UpdateColor
                for (uint32_t i = 0; i < nv; ++i)
                    mc[i] = lerp4(mcol, tip, mathf::inverse_lerp(g.color_mycelia_from, 1.f, float(i) / float(nv - 1)));
                for (uint32_t j = 1; j < 3; ++j)
                    mc[nv - j] = vec4{0.f, 0.f, 1.f, 1.f};
                sprites::long_mesh(out, strip, mv, mc, nv);
                sprites::long_mesh(out, strip, mv, mc, nv);
            }
        }
        // WhiteSprite, InnerGlowSprite, PupilSprite
        {
            vec4 const white = lerp4(color(0.75f), vec4{0.f, 0.f, 1.f, 1.f}, 0.5f);
            add_sprite(out,
                sprite{"Circle20", vector + xy(ud) * 4.f * num, deg, vec2{rad0 * 0.7f * num / 10.f, rad0 * mathf::lerp(0.35f, 0.7f, ud.z) * num / 10.f},
                    vec2{0.5f, 0.5f}, vec3{white.x, white.y, white.z}},
                white);
            sprites::add(out, sprite{"Circle20", vector + xy(ud) * 2.5f * num, deg,
                                  vec2{rad0 * 0.7f * num * 0.75f / 10.f, rad0 * mathf::lerp(0.35f, 0.7f, ud.z) * num * 0.75f / 10.f}, vec2{0.5f, 0.5f},
                                  vec3{1.f, 1.f, 1.f}});
            float const l = len3(ud);
            vec2 const nd = l > 1e-5f ? xy(ud / l) : vec2{0.f, 0.f};
            add_sprite(out, sprite{"pixel", vector + xy(ud) * 2.f * num + nd * std::sin(l * mathf::pi) * 2.f, 0.f, vec2{1.f, 1.f}, vec2{0.5f, 0.5f}, vec3{0.f, 0.f, 0.f}},
                vec4{0.f, 0.f, 0.f, 0.5f});
        }
        // HologramMatrix: the lens (pixel lines, the Hologram shader as a
        // plain colour)
        float const up = mathf::lerp(g.last_holo_lens_up, g.holo_lens_up, alpha);
        if (g.holo_lens_up == 0.f && g.last_holo_lens_up == 0.f)
            return;
        vec2 v = d.use_pos;
        float const fold = 1.f - num;
        v += xy(ud) * fold * 30.f;
        float const num2 = mathf::max(fold, 1.f - up);
        float const ur_x = -ud.y / two_pi;
        float const ur_y = ud.x / two_pi;
        vec3 const lc = lerp3(main, vec3{col0.x, col0.y, col0.z}, rng.value());
        for (uint32_t j = 0; j < g.line_count; ++j) {
            if (rng.value() < num2)
                continue;
            auto const& line = g.lines[j];
            auto const& mv = g.movements[std::min<uint32_t>(line.movement, overseer::max_holo_movements - 1)];
            float num3 = mathf::lerp(mv[1], mv[0], alpha);
            if (line.movement > 0)
                num3 = mathf::lerp(mathf::lerp(g.movements[1][1], g.movements[1][0], alpha), num3, mathf::lerp(g.movements[2][1], g.movements[2][0], alpha));
            auto const& rot = g.rotations[line.rotation & 1];
            float const rz = mathf::lerp(rot[1], rot[0], alpha);
            vec2 v2 = v + on_plane_pos(slerp3(line.a, line.alt_a, num3), ur_x, ur_y, rz, fold);
            vec2 v3 = v + on_plane_pos(slerp3(line.b, line.alt_b, num3), ur_x, ur_y, rz, fold);
            if (rng.value() < 0.5f) {
                if (rng.value() < num2)
                    v2 = v;
                if (rng.value() < num2)
                    v3 = v;
            }
            add_sprite(out, sprite{"pixel", v2, custom::aim(v2, v3), vec2{1.f, rw::distance(v2, v3)}, vec2{0.5f, 0.f}, lc}, rgba(lc, 1.f - num2));
        }
    }
}
