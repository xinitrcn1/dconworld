#include <algorithm>
#include <cmath>

#include "vulture_graphics.hpp"
#include "body_parts.hpp"
#include "creature.hpp"
#include "vulture.hpp"
#include "vulture_internal.hpp"

namespace vulture_graphics {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using sprites::sprite;
    using namespace vulture;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        // Custom.HSL2RGB exactly (a hue of 1 or more is grey, as vanilla's is)
        vec3 hsl_rgb(float h, float sl, float l) {
            float r = l, g = l, b = l;
            float const num = l <= 0.5f ? l * (1.f + sl) : l + sl - l * sl;
            if (num > 0.f) {
                float const num2 = l + l - num;
                float const num3 = (num - num2) / num;
                h *= 6.f;
                int32_t const num4 = int32_t(h);
                float const num5 = h - float(num4);
                float const num6 = num * num3 * num5;
                float const num7 = num2 + num6;
                float const num8 = num - num6;
                switch (num4) {
                case 0: r = num; g = num7; b = num2; break;
                case 1: r = num8; g = num; b = num2; break;
                case 2: r = num2; g = num; b = num7; break;
                case 3: r = num2; g = num8; b = num; break;
                case 4: r = num7; g = num2; b = num; break;
                case 5: r = num; g = num2; b = num8; break;
                default: break;
                }
            }
            return vec3{r, g, b};
        }
        vec3 rgb(vec3 hsl) { return hsl_rgb(hsl.x, hsl.y, hsl.z); }
        // Mathf.LerpAngle
        float lerp_angle(float a, float b, float t) {
            float d = (b - a) - std::floor((b - a) / 360.f) * 360.f;
            if (d > 180.f)
                d -= 360.f;
            return a + d * mathf::clamp01(t);
        }
        // HSLColor.Lerp
        vec3 hsl_lerp(vec3 a, vec3 b, float t) {
            return vec3{lerp_angle(a.x * 360.f, b.x * 360.f, t) / 360.f, mathf::lerp(a.y, b.y, t), mathf::lerp(a.z, b.z, t)};
        }
        vec3 lerp3(vec3 a, vec3 b, float t) {
            t = mathf::clamp01(t);
            return a + (b - a) * t;
        }
        vec4 rgba(vec3 c, float a) { return vec4{c.x, c.y, c.z, a}; }

        // what the module reads of its vulture
        struct bird {
            sys::game_state const& state;
            dcon::creature_id c;
            creature::vulture_payload const& p;
            neck_state const& neck;
            king_tusks const& tusks;
            wing const* wings[max_wings] = {};
            uint32_t wing_count = 0;

            bird(sys::game_state const& s, dcon::creature_id cc)
                : state(s), c(cc), p(s.world.creature_get_payload(cc).vulture), neck(s.world.vulture_data_get_neck(creature::vulture_data(s.world, cc))),
                  tusks(s.world.vulture_data_get_tusks(creature::vulture_data(s.world, cc))) {
                auto const& w = s.world;
                for (auto const o : creature::parts(w, cc)) {
                    auto const part = w.part_ownership_get_part(o);
                    if (w.creature_part_get_kind(part) != creature::part_kind::vulture_wing)
                        continue;
                    uint32_t const i = w.creature_part_get_index(part);
                    if (i >= max_wings)
                        continue;
                    auto const id = dcon::vulture_wing_id{dcon::vulture_wing_id::value_base_t(w.creature_part_get_payload(part).vulture_wing.data)};
                    wings[i] = &w.vulture_wing_get_t(id);
                    wing_count = std::max(wing_count, i + 1);
                }
                for (uint32_t i = 0; i < wing_count; ++i)
                    if (!wings[i])
                        wing_count = i;
            }
            bool king() const { return p.variant == variant::king; }
            bool miros() const { return p.variant == variant::miros; }
            vec2 pos(uint32_t i, float t) const {
                auto const k = creature::chunk(state, c, i);
                return rw::lerp(state.world.body_chunk_get_last_pos(k), state.world.body_chunk_get_pos(k), t);
            }
            vec2 vel(uint32_t i) const { return state.world.body_chunk_get_vel(creature::chunk(state, c, i)); }
            vec2 tip(float t) const { return rw::lerp(neck.c[neck_chunks - 1].last_pos, neck.c[neck_chunks - 1].pos, t); }
            bool snapping() const { return p.snap_frames > 0 && p.snap_frames <= 21; }
            // Vulture.TusksStuck
            float tusks_stuck() const { return p.snap_frames <= 0 && !creature::grasped(state.world, c, 0) ? p.tusk_charge : 1.f; }
            // VultureGraphics.AppendageConnectionPos
            vec2 appendage_connection(uint32_t app, float t) const { return rw::lerp(pos(0, t), pos(2 + app, t), 0.45f); }
        };

        // VultureFeather's chunks
        struct feather_chunks {
            tentacle_chunk const* pp;
            tentacle_chunk const* prev;
            tentacle_chunk const* next;
            float lerp;
        };
        feather_chunks chunks_of(wing const& wg, float wing_position) {
            int32_t const n = wg.chunk_count;
            int32_t const f = int32_t(std::floor(wing_position * float(n)));
            feather_chunks r;
            r.pp = &wg.c[custom::int_clamp(f - 1, 0, n - 1)];
            r.prev = &wg.c[custom::int_clamp(f, 0, n - 1)];
            r.next = &wg.c[custom::int_clamp(f + 1, 0, n - 1)];
            r.lerp = wing_position * float(n) - std::floor(wing_position * float(n));
            return r;
        }
        vec2 connected_pos(wing const& wg, float wp, float t) {
            auto const ch = chunks_of(wg, wp);
            return rw::lerp(rw::lerp(ch.prev->last_pos, ch.prev->pos, t), rw::lerp(ch.next->last_pos, ch.next->pos, t), ch.lerp);
        }

        // VultureGraphics.TuskConnectionPositions
        void tusk_connection(bird const& b, uint32_t tusk, float t, vec2 (&res)[4]) {
            vec2 const v = custom::dir_vec(b.tip(t), b.pos(head, t));
            vec2 const a = custom::perpendicular_vector(v);
            float num = custom::aim(b.tip(t), b.pos(head, t)) - custom::aim(b.pos(0, t), b.pos(1, t));
            if (num > 180.f)
                num -= 360.f;
            else if (num < -180.f)
                num += 360.f;
            vec2 const v2 = a * (tusk == 0 ? -1.f : 1.f) * mathf::pow(std::fabs(std::cos(mathf::pi * num / 180.f)), 0.5f) * (std::fabs(num) > 90.f ? -1.f : 1.f);
            bool const held = creature::grasped(b.state.world, b.c, 0) != dcon::creature_id{};
            float const stuck = b.tusks_stuck();
            vec2 const v3 = b.pos(head, t) + v * mathf::lerp(7.f, -13.f, (b.snapping() || held) ? 0.f : stuck) + v2 * mathf::lerp(4.f, 9.f, stuck);
            res[0] = v3;
            res[1] = v;
            res[2] = v2;
            res[3] = v3 + v * 20.f - v2 * (held ? 5.f : 0.f);
        }

        // VultureFeather.CurrentColor
        vec4 feather_color(bird const& b, graphics_state const& g, feather const& f, wing const& wg) {
            bool const laser = b.miros() && !b.state.world.creature_get_dead(b.c) && b.p.laser_counter > 0 && !creature::grasped(b.state.world, b.c, 0);
            float const fm = wg.flying_mode;
            if (b.miros()) {
                vec3 const c = rgb(hsl_lerp(vec3{g.color_b.x, mathf::lerp(g.color_b.y, 1.f, f.saturation_bonus), mathf::lerp(g.color_b.z, 1.f, f.lightness_bonus)}, g.color_a,
                    std::cos(mathf::pow(f.wing_position, 0.75f) * mathf::pi)));
                float a = mathf::max(mathf::max(0.4f, f.forced_alpha), mathf::lerp(0.4f, 0.8f, std::cos(mathf::pow(f.wing_position, 1.7f) * mathf::pi)))
                    * (f.extended_fac + fm) * 0.5f * (1.f - f.broken_color);
                if (laser)
                    a = 0.5f;
                return rgba(c, a);
            }
            vec3 cb = g.color_b, ca = g.color_a;
            if (g.albino) {
                cb.y = mathf::lerp(cb.y, 1.f, 0.2f);
                cb.x = 0.f;
                cb.z = mathf::lerp(cb.y, 0.2f, 0.8f);
                ca.y = 0.8f;
                ca.z = 0.6f;
            }
            vec3 const c = rgb(hsl_lerp(vec3{cb.x, mathf::lerp(cb.y, 1.f, f.saturation_bonus), mathf::lerp(cb.z, 1.f, f.lightness_bonus)}, ca,
                std::cos(mathf::pow(f.wing_position, 0.75f) * mathf::pi)));
            float const a = mathf::max(f.forced_alpha, mathf::lerp(0.2f, 0.6f, std::cos(mathf::pow(f.wing_position, 1.7f) * mathf::pi))) * (f.extended_fac + fm) * 0.5f
                * (1.f - f.broken_color);
            return rgba(c, a);
        }

        player::body_part as_part(vec2 pos, vec2 last, vec2 vel, float rad) {
            player::body_part bp;
            bp.pos = pos;
            bp.last_pos = last;
            bp.vel = vel;
            bp.rad = rad;
            return bp;
        }

        // KingTusks.Tusk.AimDir
        vec2 aim_dir(bird const& b, tusk const& t, uint32_t side, float ts) {
            auto const& k = b.tusks;
            vec2 v = custom::dir_vec(b.tip(ts), b.pos(head, ts));
            float const num = mathf::inverse_lerp(0.f, 25.f, float(t.mode_counter) + ts);
            float const home = mathf::lerp(k.last_eyes_home, k.eyes_home_in, ts);
            if (home > 0.f) {
                vec2 const v2 = custom::dir_vec(b.pos(head, ts), rw::lerp(k.last_prey_pos, k.prey_pos, ts));
                v = rw::slerp(v, v2, home * mathf::pow(mathf::inverse_lerp(0.2f, 0.85f - 0.2f * num, rw::dot(v, v2)), 2.f - 1.5f * num));
            }
            float const out = mathf::lerp(k.last_eyes_out, k.eyes_out, ts);
            if (out > 0.f) {
                float const off = side == 0 ? -90.f : 90.f;
                v += custom::perpendicular_vector(v) * rw::slerp(custom::deg_to_vec(k.last_head_rot + off), custom::deg_to_vec(k.head_rot + off), ts).x * out * 0.5f;
            }
            return rw::normalized(v);
        }

        float tusk_bend(float f) { return std::sin(mathf::pow(f, 0.85f) * mathf::pi * 2.f) * mathf::pow(1.f - f, 2.f); }
        float tusk_prof_bend(float f) { return -std::cos(mathf::pow(f, 0.85f) * mathf::pi * 2.5f) * mathf::pow(1.f - f, 3.f); }
        float tusk_rad(float f, float profile) {
            return 0.5f + 2.f * mathf::pow(mathf::clamp01(std::sin(mathf::pow(f, mathf::lerp(0.65f, 0.5f, profile)) * mathf::pi)), 1.2f - 0.3f * profile);
        }

        // BeakGraphic.OuterShape
        float outer_shape(float f) { return mathf::max(1.f - f, std::sin(f * mathf::pi)); }

        void quad(std::vector<sprites::draw_cmd>& out, vec2 a, vec2 b, vec2 c, vec2 d, vec4 ca, vec4 cb, vec4 cc, vec4 cd) {
            vec2 const corners[4] = {a, b, c, d};
            vec4 const cols[4] = {ca, cb, cc, cd};
            sprites::add_quad(out, "Futile_White", corners, cols);
        }
        void add_alpha(std::vector<sprites::draw_cmd>& out, sprite const& s, vec4 col) {
            vec4 const corners[4] = {col, col, col, col};
            sprites::add(out, s, corners);
        }
    }

    void make_color_wave(sys::game_state& state, dcon::creature_id c, int32_t delay) {
        auto& w = state.world;
        auto& g = w.vulture_data_get_graphics(creature::vulture_data(w, c));
        if (g.wave_count >= max_color_waves)
            return;
        // (keyed by the wave's slot: several can start in one tick)
        auto r = rw::random(state.seed, state.tick, rw::ids(uint32_t(c.value), uint32_t(g.wave_count)));
        color_wave& cw = g.waves[g.wave_count++];
        cw = color_wave{};
        cw.delay = delay;
        cw.speed = 1.f / mathf::lerp(10.f, 30.f, r.value());
        cw.lightness = 1.f - r.value() * r.value() * r.value();
        cw.saturation = 1.f - r.value() * r.value() * r.value();
        cw.force_alpha = 1.f - r.value() * r.value() * r.value();
    }

    void init(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        auto const d = creature::vulture_data(w, c);
        auto& g = w.vulture_data_get_graphics(d);
        g = graphics_state{};
        bird const b{state, c};
        bool const king = b.king(), miros = b.miros();
        // Random.InitState(abstractCreature.ID.RandomSeed)
        rw::unity_random r{w.vulture_data_get_seed(d)};
        if (miros) {
            g.color_a = vec3{custom::wrapped_random_variation(0.0025f, 0.02f, 0.6f, r), 1.f, custom::clamped_random_variation(0.5f, 0.15f, 0.1f, r)};
            float const h = g.color_a.x + mathf::lerp(-0.25f, 0.25f, r.value());
            float const s = mathf::lerp(0.8f, 1.f, 1.f - r.value() * r.value());
            float const v = r.value();
            g.color_b = vec3{h, s, mathf::lerp(0.45f, 1.f, v * r.value())};
        } else if (king) {
            float const h = mathf::lerp(0.93f, 1.07f, r.value());
            float const s1 = r.value();
            float const s = mathf::lerp(0.8f, 1.f, 1.f - s1 * r.value());
            float const l1 = r.value();
            g.color_b = vec3{h, s, mathf::lerp(0.45f, 1.f, l1 * r.value())};
            float const ha = g.color_b.x + mathf::lerp(-0.25f, 0.25f, r.value());
            float const sa = mathf::lerp(0.5f, 0.7f, r.value());
            g.color_a = vec3{ha, sa, mathf::lerp(0.7f, 0.8f, r.value())};
        } else {
            float const h = mathf::lerp(0.9f, 1.6f, r.value());
            float const s = mathf::lerp(0.5f, 0.7f, r.value());
            g.color_a = vec3{h, s, mathf::lerp(0.7f, 0.8f, r.value())};
            float const hb = g.color_a.x + mathf::lerp(-0.25f, 0.25f, r.value());
            float const s1 = r.value();
            float const sb = mathf::lerp(0.8f, 1.f, 1.f - s1 * r.value());
            float const l1 = r.value();
            g.color_b = vec3{hb, sb, mathf::lerp(0.45f, 1.f, l1 * r.value())};
        }
        int32_t fpw = miros ? r.range(6, 8) : r.range(king ? 15 : 13, king ? 25 : 20);
        uint32_t const wings = std::max<uint32_t>(b.wing_count, 1u);
        fpw = std::min<int32_t>(fpw, int32_t(std::min<uint32_t>(max_feathers_per_wing, max_feathers / wings)));
        g.feathers_per_wing = uint8_t(fpw);
        float const num = r.value() < 0.5f ? 40.f : mathf::lerp(8.f, 15.f, r.value());
        float const num2 = r.value() < 0.5f ? 40.f : mathf::lerp(8.f, 15.f, r.value());
        float const num3 = r.value() < 0.5f ? 20.f : mathf::lerp(3.f, 6.f, r.value());
        if (miros) {
            g.beak_fatness = r.value();
            for (int32_t i = 0; i < 2; ++i) {
                int32_t const n = std::min<int32_t>(r.range(10, 20), int32_t(max_teeth));
                g.teeth_count[i] = uint8_t(n);
                for (int32_t k = 0; k < n; ++k) {
                    g.teeth[i][k][0] = mathf::lerp(0.1f, 0.9f, mathf::pow(r.value(), 0.8f));
                    g.teeth[i][k][1] = mathf::lerp(0.5f, 1.f, r.value());
                    float const v = r.value();
                    g.teeth[i][k][2] = mathf::lerp(-1.f, 1.f, mathf::lerp(v, g.teeth[i][k][0], 1.f));
                }
            }
        }
        for (uint32_t j = 0; j < b.wing_count; ++j) {
            for (int32_t k = 0; k < fpw; ++k) {
                float const num5 = (float(k) + 0.5f) / float(fpw);
                float num6 = mathf::lerp(1.f - mathf::pow(miros ? 0.95f : 0.89f, float(k)), std::sqrt(num5), 0.5f);
                num6 = mathf::inverse_lerp(0.1f, 1.1f, num6);
                if (miros && k == fpw - 1)
                    num6 = 0.8f;
                feather& f = g.feathers[j * uint32_t(fpw) + uint32_t(k)];
                f = feather{};
                f.wing_position = num6;
                float const cl = r.value();
                f.contracted_length = detail::feather_contour(num5, 0.f) * mathf::lerp(miros ? 60.f : 50.f, miros ? 80.f : 75.f, cl);
                float const el = r.value();
                f.extended_length = detail::feather_contour(num5, 1.f) * mathf::lerp(miros ? 80.f : 65.f, miros ? 100.f : 75.f, el) * (king ? 1.3f : 1.f);
                f.width = mathf::lerp(miros ? 5.f : 3.f, miros ? 8.f : 6.f, detail::feather_width(num5));
                bool const flag = r.value() < 0.025f;
                if (r.value() < 1.f / num || (flag && r.value() < 0.5f)) {
                    f.lose = 1.f - r.value() * r.value() * r.value();
                    if (r.value() < 0.4f)
                        f.broken_color = 1.f - r.value() * r.value();
                }
                if (r.value() < 1.f / num2) {
                    f.extended_length /= 5.f;
                    f.contracted_length = f.extended_length;
                    f.broken_color = 1.f;
                    f.width /= 1.7f;
                }
                if (r.value() < 0.025f || (flag && r.value() < 0.5f))
                    f.contracted_length = f.extended_length * 0.7f;
                if (r.value() < 1.f / num3 || (flag && r.value() < 0.5f))
                    f.broken_color = r.value() < 0.5f ? 1.f : r.value();
            }
        }
        for (uint32_t l = 0; l < 2; ++l) {
            int32_t num7 = king ? (14 - r.range(0, r.range(2, 12))) : r.range(2, 12);
            num7 = custom::int_clamp(num7, 1, int32_t(max_appendages));
            g.appendage_count[l] = uint8_t(num7);
            float num8 = 3.f;
            bool flag2 = false, flag3 = false;
            for (int32_t m = 0; m < num7; ++m) {
                float const num9 = std::sqrt(1.f - mathf::pow((float(m) + 0.5f) / float(num7), 3.f));
                if (m == num7 - 1)
                    num8 = 2.f;
                auto& a = g.appendages[l][m];
                a = appendage{};
                a.rad = num8 * num9;
                a.stretched_rad = a.rad;
                a.con_rad = m == 0 ? 15.f : mathf::lerp(3.f, 5.f, r.value());
                if (!flag2 && r.value() < 0.5f) {
                    flag2 = true;
                    num8 = flag3 ? 4.f : 6.f;
                    flag3 = !flag3;
                } else {
                    flag2 = false;
                }
                num8 += mathf::lerp(-1.f, 1.f, mathf::lerp(r.value(), 0.5f, 0.5f)) * 2.f;
                num8 = mathf::clamp(num8, 3.f, 7.f);
            }
        }
        if (king)
            w.vulture_data_get_tusks(d).pattern_displace = r.value();
        // (MSC albino: one in a thousand)
        g.albino = !miros && r.value() < 0.001f ? 1 : 0;
        if (miros) {
            g.eye_size = 0.6f;
            g.eye_col = custom::hsl2rgb(mathf::lerp(0.08f, 0.17f, r.value()), 1.f, 0.5f);
        }
        for (auto& t : g.tusks) {
            t = player::body_part{};
            t.rad = 2.f;
        }
        g.color_waves_seen = w.vulture_data_get_body(d).color_waves;
        reset_positions(state, c);
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        auto& g = w.vulture_data_get_graphics(creature::vulture_data(w, c));
        bird const b{state, c};
        vec2 const m = b.pos(0, 1.f);
        for (auto& t : g.tusks) {
            t.pos = t.last_pos = b.pos(head, 1.f);
            t.vel = vec2{0.f, 0.f};
        }
        for (auto& s : g.shells) {
            s[0] = m;
            s[1] = vec2{0.f, 0.f};
            s[2] = m;
        }
        for (uint32_t k = 0; k < b.wing_count; ++k)
            for (uint32_t l = 0; l < g.feathers_per_wing; ++l) {
                auto& f = g.feathers[k * g.feathers_per_wing + l];
                f.pos = f.last_pos = b.pos(b.wings[k]->t.connected_chunk, 1.f);
                f.vel = b.vel(b.wings[k]->t.connected_chunk);
            }
        for (uint32_t s = 0; s < 2; ++s)
            for (uint32_t n = 0; n < g.appendage_count[s]; ++n) {
                auto& a = g.appendages[s][n];
                a.pos = a.last_pos = m;
                a.vel = vec2{0.f, 0.f};
            }
        for (auto& tube : g.neck_tubes)
            for (auto& p : tube) {
                p[0] = p[1] = m;
                p[2] = vec2{0.f, 0.f};
            }
        g.eye_trail[0] = b.pos(head, 1.f);
        g.eye_trail_count = 1;
    }

    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room) {
        auto& w = state.world;
        auto& g = w.vulture_data_get_graphics(creature::vulture_data(w, c));
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        bird const b{state, c};
        bool const miros = b.miros(), king = b.king();
        if (miros) {
            g.last_head_flip = g.head_flip;
            if (custom::distance_to_line(b.pos(head, 1.f), b.pos(1, 1.f), b.pos(0, 1.f)) < 0.f)
                g.head_flip = mathf::min(1.f, g.head_flip + 0.16666667f);
            else
                g.head_flip = mathf::max(-1.f, g.head_flip - 0.16666667f);
            g.last_laser_active = g.laser_active;
            bool const laser = !w.creature_get_dead(c) && b.p.laser_counter > 0 && !creature::grasped(w, c, 0);
            g.laser_active = custom::lerp_and_tick(g.laser_active, laser ? 1.f : 0.f, 0.05f, 0.05f);
            g.last_flash = g.flash;
            g.flash = custom::lerp_and_tick(g.flash, 0.f, 0.02f, 0.025f);
        }
        // VultureFeather.Update
        for (uint32_t i = 0; i < b.wing_count; ++i) {
            wing const& wg = *b.wings[i];
            for (uint32_t j = 0; j < g.feathers_per_wing; ++j) {
                auto& f = g.feathers[i * g.feathers_per_wing + j];
                if (room.point_submerged(f.pos))
                    f.vel *= 0.1f;
                f.last_pos = f.pos;
                f.pos += f.vel;
                f.vel *= 0.7f;
                auto const ch = chunks_of(wg, f.wing_position);
                vec2 const conn = connected_pos(wg, f.wing_position, 1.f);
                vec2 const nrm = rw::normalized(rw::lerp(ch.prev->pos - ch.pp->pos, ch.next->pos - ch.prev->pos, ch.pp == ch.prev ? 1.f : ch.lerp));
                float sideways;
                if (miros)
                    sideways = i == 0 ? 1.f : i == 1 ? -1.f : i == 2 ? 4.f : -4.f;
                else
                    sideways = i == 1 ? -1.f : 1.f;
                vec2 const a = custom::perpendicular_vector(nrm) * sideways;
                vec2 const base = b.pos(wg.t.connected_chunk, 1.f);
                float const reach = rw::distance(base, wg.c[wg.chunk_count - 1].pos);
                float const cur_len = mathf::lerp(f.contracted_length, f.extended_length, f.extended_fac);
                float const dd = mathf::lerp(mathf::lerp(1.f, mathf::lerp(-0.9f, 1.5f, mathf::inverse_lerp(wg.t.ideal_length * 0.5f, wg.t.ideal_length, reach)), f.wing_position),
                    mathf::lerp(-0.5f, 4.f, f.wing_position), f.extended_fac);
                vec2 const goal = conn + rw::normalized(a + nrm * dd) * cur_len;
                f.vel += (goal - f.pos) * mathf::lerp(0.3f, 0.8f, wg.flying_mode) * (1.f - f.lose);
                if (wg.flying_mode > f.extended_fac) {
                    f.extended_fac = mathf::clamp01(f.extended_fac + 1.f / mathf::lerp(10.f, 40.f, rng.value()));
                } else if (wg.flying_mode < f.extended_fac) {
                    if (f.extended_fac == 1.f) {
                        f.mode = miros ? contract_mode::jerky : contract_mode::even;
                        f.contract_speed = 1.f / mathf::lerp(20.f, 800.f, rng.value() * rng.value() * rng.value());
                        if (rng.value() < 0.3f)
                            f.mode = rng.value() < 0.7f ? contract_mode::jerky : contract_mode::jammed;
                    }
                    if (f.mode != contract_mode::even && f.extended_fac > 0.5f)
                        f.extended_fac = mathf::clamp01(f.extended_fac - 0.008333334f);
                    switch (f.mode) {
                    case contract_mode::even: f.extended_fac = mathf::clamp01(f.extended_fac - f.contract_speed); break;
                    case contract_mode::jerky:
                        if (rng.value() < 0.0016666667f)
                            f.extended_fac = mathf::clamp01(f.extended_fac - 1.f / mathf::lerp(4.f, 30.f, rng.value()));
                        break;
                    case contract_mode::jammed:
                        if (rng.value() < 0.0007142857f)
                            f.mode = contract_mode::jerky;
                        break;
                    }
                }
                f.lightness_bonus = mathf::max(f.lightness_bonus - 0.1f, 0.f);
                if (f.lightness_bonus == 0.f)
                    f.saturation_bonus = mathf::max(f.saturation_bonus - 0.02f, 0.f);
                f.forced_alpha = mathf::lerp(f.forced_alpha, 0.f, 0.05f);
                auto bp = as_part(f.pos, f.last_pos, f.vel, 1.f);
                float const len = mathf::lerp(f.contracted_length, f.extended_length, f.extended_fac);
                body_parts::connect_to_point(bp, conn, len, true, 0.f, ch.prev->vel, 0.3f, 0.f);
                bool const contact = body_parts::push_out_of_terrain(bp, room, 0.5f);
                f.terrain_contact_timer = contact ? f.terrain_contact_timer + 1 : 0;
                // (Vulture_Feather_Hit_Terrain has no sample here)
                if (contact && f.terrain_contact_timer > 4)
                    f.terrain_contact_timer = 0;
                f.pos = bp.pos;
                f.vel = bp.vel;
            }
        }
        // VultureAppendage.Update
        vec2 const mvel = b.vel(0);
        for (uint32_t s = 0; s < 2; ++s) {
            vec2 const conn = b.appendage_connection(s, 1.f);
            for (uint32_t i = 0; i < g.appendage_count[s]; ++i) {
                auto& a = g.appendages[s][i];
                a.last_pos = a.pos;
                a.pos += a.vel;
                a.vel *= 0.98f;
                a.vel.y += room.point_submerged(a.pos) ? 0.2f : -0.9f;
                a.vel -= mvel;
                a.vel *= 0.9f;
                a.vel += mvel;
                if (i < 4)
                    a.vel += rw::normalized(conn - b.pos(3 - s, 1.f)) * 3.5f / float(i + 1);
                if (i == 0) {
                    vec2 const dir = custom::dir_vec(a.pos, conn);
                    float const num = rw::distance(a.pos, conn);
                    a.pos -= dir * (a.con_rad - num);
                    a.vel -= dir * (a.con_rad - num);
                    a.stretched_rad = a.rad * mathf::clamp(mathf::pow(a.con_rad / mathf::max(num, 0.01f), 0.1f), 0.5f, 1.5f);
                } else {
                    auto& prev = g.appendages[s][i - 1];
                    vec2 const dir = custom::dir_vec(a.pos, prev.pos);
                    float const num3 = rw::distance(a.pos, prev.pos);
                    a.pos -= dir * (a.con_rad - num3) * 0.5f;
                    a.vel -= dir * (a.con_rad - num3) * 0.5f;
                    prev.pos += dir * (a.con_rad - num3) * 0.5f;
                    prev.vel += dir * (a.con_rad - num3) * 0.5f;
                    a.stretched_rad = a.rad * mathf::clamp(mathf::pow(a.con_rad / mathf::max(num3, 0.01f), 0.1f), 0.5f, 1.5f);
                }
                float const lim = 25.f + 7.f * float(i);
                if (!miros && !custom::dist_less(a.pos, conn, lim)) {
                    vec2 const dir = custom::dir_vec(a.pos, conn);
                    float const num5 = rw::distance(a.pos, conn);
                    a.pos -= dir * (lim - num5);
                    a.vel -= dir * (lim - num5);
                }
                for (uint32_t k = 0; k < 4; ++k) {
                    vec2 const cp = b.pos(k, 1.f);
                    float const cr = state.world.body_chunk_get_rad(creature::chunk(state, c, k));
                    if (custom::dist_less(a.pos, cp, cr)) {
                        float const num = rw::distance(a.pos, cp);
                        vec2 const dir = custom::dir_vec(a.pos, cp);
                        a.pos -= (cr - num) * dir;
                        a.vel -= (cr - num) * dir;
                    }
                }
                if (custom::dist_less(a.pos, conn, 100.f) || !room.get_tile(a.pos).solid()) {
                    auto bp = as_part(a.pos, a.last_pos, a.vel, a.rad);
                    body_parts::push_out_of_terrain(bp, room, 0.5f);
                    a.pos = bp.pos;
                    a.vel = bp.vel;
                }
            }
        }
        // the waves the body asked for (taking off, falling)
        {
            auto const& body = w.vulture_data_get_body(creature::vulture_data(w, c));
            if (body.color_waves != g.color_waves_seen) {
                g.color_waves_seen = body.color_waves;
                make_color_wave(state, c, body.color_wave_delay);
            }
        }
        if (rng.value() < 0.005f)
            make_color_wave(state, c, 0);
        // the colour waves along the feathers
        for (int32_t n = int32_t(g.wave_count) - 1; n >= 0; --n) {
            auto& cw = g.waves[n];
            if (cw.last_position > 1.f) {
                g.waves[n] = g.waves[g.wave_count - 1];
                --g.wave_count;
                continue;
            }
            if (cw.delay > 0) {
                --cw.delay;
                continue;
            }
            cw.last_position = cw.position;
            cw.position += cw.speed / (1.f + cw.position);
            for (uint32_t i = 0; i < b.wing_count; ++i)
                for (uint32_t j = 0; j < g.feathers_per_wing; ++j) {
                    auto& f = g.feathers[i * g.feathers_per_wing + j];
                    if (cw.last_position < f.wing_position && cw.position >= f.wing_position) {
                        f.saturation_bonus = mathf::max(f.saturation_bonus, cw.saturation);
                        f.lightness_bonus = mathf::max(f.lightness_bonus, cw.lightness);
                        f.forced_alpha = mathf::max(f.forced_alpha, cw.force_alpha);
                        break;
                    }
                }
        }
        // the head graphic
        float const num3 = custom::aim(b.tip(1.f), b.pos(head, 1.f));
        float num5 = num3 - custom::aim(b.pos(0, 1.f), b.pos(1, 1.f));
        if (num5 > 180.f)
            num5 -= 360.f;
        else if (num5 < -180.f)
            num5 += 360.f;
        int32_t const num6 = custom::int_clamp(8 - int32_t(std::fabs(num5 / 180.f) * 9.f), 0, 8);
        if (num6 != g.head_graphic) {
            if (++g.change_head_graphic_counter > 4) {
                g.change_head_graphic_counter = 0;
                g.head_graphic += num6 < g.head_graphic ? -1 : 1;
            }
        } else {
            g.change_head_graphic_counter = 0;
        }
        if (miros) {
            // EyeTrail.Update
            for (uint32_t i = eye_trail_positions - 1; i > 0; --i)
                g.eye_trail[i] = g.eye_trail[i - 1];
            g.eye_trail[0] = b.pos(head, 1.f);
            g.eye_trail_count = uint8_t(std::min<uint32_t>(g.eye_trail_count + 1u, eye_trail_positions));
        } else {
            float const stuck = b.tusks_stuck();
            if (stuck > 0.5f) {
                g.tusk_rotations[0] = mathf::lerp(g.tusk_rotations[0], 0.f, 0.05f);
                g.tusk_rotations[1] = mathf::lerp(g.tusk_rotations[1], 0.f, 0.05f);
            } else if (std::fabs(num3) < 30.f || std::fabs(num3) > 150.f) {
                g.tusk_rotations[0] = mathf::lerp(g.tusk_rotations[0], 1.f, 0.1f);
                g.tusk_rotations[1] = mathf::lerp(g.tusk_rotations[1], -1.f, 0.1f);
            } else if (std::fabs(num3) > 60.f && std::fabs(num3) < 120.f) {
                g.tusk_rotations[0] = mathf::lerp(g.tusk_rotations[0], 2.f * mathf::sign(num3), 0.1f);
                g.tusk_rotations[1] = mathf::lerp(g.tusk_rotations[1], 2.f * mathf::sign(num3), 0.1f);
            } else {
                uint32_t const a = num3 < 0.f ? 0 : 1;
                g.tusk_rotations[a] = mathf::lerp(g.tusk_rotations[a], mathf::sign(num3), 0.1f);
                g.tusk_rotations[1 - a] = mathf::lerp(g.tusk_rotations[1 - a], 2.f * mathf::sign(num3), 0.1f);
            }
            vec2 const hv = b.vel(head);
            for (uint32_t i = 0; i < 2; ++i) {
                auto& t = g.tusks[i];
                body_parts::generic_update(t, room, 0.95f, 0.5f);
                body_parts::push_out_of_terrain(t, room, 0.5f);
                t.vel.y -= 0.6f;
                vec2 con[4];
                tusk_connection(b, i, 1.f, con);
                t.vel += con[1] * 0.7f * (-1.f + 2.f * mathf::pow(mathf::inverse_lerp(0.f, 180.f, std::fabs(num5)), 0.5f)) + con[2] * 0.3f;
                body_parts::connect_to_point(t, con[0], 20.f, true, 0.f, hv, 0.f, 0.f);
                t.vel = rw::lerp(t.vel, con[3] - t.pos, stuck);
            }
        }
        // the shells
        vec2 const v = custom::dir_vec(b.pos(0, 1.f), b.pos(1, 1.f));
        vec2 const a = custom::perpendicular_vector(v);
        for (uint32_t k = 0; k < 4; ++k) {
            float const dd = k <= 1 ? 1.f : -1.f;
            uint32_t const s = k <= 1 ? 0 : 1;
            if (s >= b.wing_count)
                continue;
            wing const& wg = *b.wings[s];
            auto& sh = g.shells[k];
            float const mode = g.shell_modes[s][0];
            sh[2] = sh[0];
            sh[0] += sh[1];
            sh[1] *= 0.9f;
            sh[1].y -= 0.5f;
            vec2 v2 = b.pos(1, 1.f) + a * (king ? 18.f : 13.f) * dd;
            vec2 const c3 = wg.c[std::min<uint32_t>(3, wg.chunk_count - 1)].pos;
            if (k % 2 == 1) {
                v2 += v * mathf::lerp(14.f, 22.f, mode);
                v2 += 14.f * a * dd * mode;
                v2 += custom::dir_vec(v2, c3) * 2.5f * (1.f - mode);
            } else {
                v2 += v * mathf::lerp(24.f, 28.f, mode);
                v2 += 16.f * a * dd * mode;
                v2 += custom::dir_vec(v2, c3) * 3.5f * mode;
            }
            sh[1] += a * dd * 0.5f;
            float const lim = mathf::lerp(2.f, 4.f, mode);
            if (!custom::dist_less(sh[0], v2, lim)) {
                vec2 const a2 = custom::dir_vec(sh[0], v2);
                float const dist = rw::distance(sh[0], v2) - lim;
                sh[1] += a2 * dist;
                sh[0] += a2 * dist;
            } else {
                sh[1] += (v2 - sh[0]) / 5.f;
            }
        }
        for (uint32_t s = 0; s < 2 && s < b.wing_count; ++s) {
            float const fm = b.wings[s]->flying_mode;
            g.shell_modes[s][1] = g.shell_modes[s][0];
            if (g.shell_mode_change_counter[s] >= 30) {
                g.shell_modes[s][0] = mathf::lerp(g.shell_modes[s][0], fm, 0.2f);
                if (std::fabs(g.shell_modes[s][0] - fm) < 0.1f) {
                    g.shell_modes[s][0] = fm;
                    g.shell_mode_change_counter[s] = 0;
                }
            } else if ((fm > 0.f && g.shell_modes[s][0] == 0.f) || (fm == 0.f && g.shell_modes[s][0] != 0.f)) {
                ++g.shell_mode_change_counter[s];
            } else {
                g.shell_mode_change_counter[s] = 0;
            }
        }
        // the king's neck tubes
        if (king) {
            vec2 v3{0.f, 0.f};
            for (uint32_t i = 0; i < chunk_count; ++i) {
                auto const k = creature::chunk(state, c, i);
                v3 += w.body_chunk_get_pos(k) - w.body_chunk_get_last_pos(k);
            }
            v3 /= float(chunk_count);
            auto const connect = [&](uint32_t s, vec2 const (&tc)[4]) {
                auto& tube = g.neck_tubes[s];
                vec2 end = rw::lerp(b.pos(2 + s, 1.f), b.pos(0, 1.f), 0.5f);
                end += custom::dir_vec(b.pos(1, 1.f), end) * 11.f;
                vec2 const start = tc[0] + tc[1] * 5.f - tc[2] * 10.f;
                tube[0][0] = start;
                tube[0][2] = vec2{0.f, 0.f};
                tube[neck_tube_points - 1][0] = end;
                tube[neck_tube_points - 1][2] = vec2{0.f, 0.f};
                for (uint32_t i = 0; i + 1 < neck_tube_points; ++i) {
                    vec2 const d = custom::dir_vec(tube[i][0], tube[i + 1][0]) * (rw::distance(tube[i][0], tube[i + 1][0]) - 5.f);
                    tube[i][0] += d / 2.f;
                    tube[i][2] += d / 2.f;
                    tube[i + 1][0] -= d / 2.f;
                    tube[i + 1][2] -= d / 2.f;
                }
                tube[0][0] = start;
                tube[0][2] = vec2{0.f, 0.f};
                tube[neck_tube_points - 1][0] = end;
                tube[neck_tube_points - 1][2] = vec2{0.f, 0.f};
            };
            for (uint32_t s = 0; s < 2; ++s) {
                vec2 tc[4];
                tusk_connection(b, s, 1.f, tc);
                connect(s, tc);
                auto& tube = g.neck_tubes[s];
                vec2 const a3 = custom::dir_vec(b.pos(1, 1.f), rw::lerp(b.pos(2 + s, 1.f), b.pos(0, 1.f), 0.5f));
                for (uint32_t i = 0; i < neck_tube_points; ++i) {
                    float const value = mathf::inverse_lerp(0.f, float(neck_tube_points - 1), float(i));
                    tube[i][2] += a3 * mathf::inverse_lerp(0.6f, 1.f, value) * 2.f;
                    tube[i][2] -= tc[1] * mathf::inverse_lerp(0.25f, 0.f, value) * 2.f;
                    tube[i][1] = tube[i][0];
                    tube[i][0] += tube[i][2];
                    tube[i][2] = rw::lerp(tube[i][2], v3, 0.05f);
                    tube[i][2].y -= 0.9f;
                    if (i > 1) {
                        vec2 const bb = custom::dir_vec(tube[i][0], tube[i - 2][0]);
                        tube[i][2] -= bb;
                        tube[i - 2][2] += bb;
                    }
                    if (i > 2 && i < neck_tube_points - 3) {
                        detail::terrain_collision cd{tube[i][0], tube[i][1], tube[i][2], 1.f};
                        detail::vertical_collision(room, cd);
                        detail::horizontal_collision(room, cd);
                        tube[i][0] = cd.pos;
                        tube[i][2] = cd.vel;
                    }
                }
                connect(s, tc);
            }
        }
        // shadowMode: high over the room
        float const top = room.pixel_height();
        if (!g.shadow_mode && b.pos(0, 1.f).y > top + 400.f)
            g.shadow_mode = 1;
        else if (g.shadow_mode && b.pos(0, 1.f).y < top + 200.f)
            g.shadow_mode = 0;
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip) {
        auto const& w = state.world;
        auto const& g = w.vulture_data_get_graphics(creature::vulture_data(w, c));
        // (the shadow it casts from high over the room isn't drawn)
        if (g.shadow_mode)
            return;
        bird const b{state, c};
        float const t = alpha;
        bool const king = b.king(), miros = b.miros();
        auto const room = world::get_room_view(state, creature::room_of(state, c));
        // ApplyPalette (ExitShadowMode; no Remix: no darkness)
        vec3 black = pal.black();
        vec3 color2{1.f, 1.f, 1.f};
        if (g.albino) {
            black = lerp3(black, vec3{1.f, 1.f, 1.f}, 0.86f - pal.darkness() / 1.8f);
            color2 = lerp3(black, pal.black(), 0.74f);
            black = lerp3(black, pal.sky(), 0.21f);
        }
        vec3 const ca = rgb(g.color_a), cb = rgb(g.color_b);
        vec3 const color3 = lerp3(ca, vec3{1.f, 1.f, 1.f}, 0.35f);
        uint32_t const fpw = g.feathers_per_wing;

        vec2 const body1 = b.pos(1, t), body0 = b.pos(0, t);
        vec2 const hp = b.pos(head, t);
        vec2 const tip = b.tip(t);
        float const num2 = custom::aim(tip, hp);

        // AppendageSprite(0, 1): the danglers under the body
        for (uint32_t s = 0; s < 2; ++s) {
            uint32_t const n = g.appendage_count[s];
            std::vector<vec2> v(n * 4);
            std::vector<vec4> col(n * 4);
            vec2 v6 = b.appendage_connection(s, t);
            float num4 = 10.f;
            for (uint32_t i = 0; i < n; ++i) {
                auto const& a = g.appendages[s][i];
                vec2 const v10 = rw::lerp(a.last_pos, a.pos, t);
                vec2 const nn = rw::normalized(v10 - v6);
                vec2 const p = custom::perpendicular_vector(nn);
                float const d = rw::distance(v10, v6) / 5.f;
                v[i * 4] = v6 - p * (a.stretched_rad + num4) * 0.5f + nn * d;
                v[i * 4 + 1] = v6 + p * (a.stretched_rad + num4) * 0.5f + nn * d;
                v[i * 4 + 2] = v10 - p * a.stretched_rad - nn * d;
                v[i * 4 + 3] = v10 + p * a.stretched_rad - nn * d;
                num4 = a.stretched_rad;
                v6 = v10;
            }
            size_t const nv = size_t(n) * 4;
            for (size_t k = 0; k < nv; ++k) {
                float const value = float(k / 2) * 2.f / float(std::max<size_t>(nv - 1, 24));
                float const num = mathf::pow(mathf::clamp01(mathf::inverse_lerp(0.15f, 0.7f, value)), 0.5f);
                col[k] = rgba(g.albino ? lerp3(black, color2, num) : lerp3(pal.black(), ca, num), 1.f);
            }
            sprites::long_mesh(out, strip, v.data(), col.data(), nv);
        }
        // TentacleSprite: the wings
        for (uint32_t k = 0; k < b.wing_count; ++k) {
            wing const& wg = *b.wings[k];
            uint32_t const n = wg.chunk_count;
            vec2 v[max_wing_chunks * 4];
            vec2 v6 = b.pos(wg.t.connected_chunk, t);
            for (uint32_t m = 0; m < n; ++m) {
                vec2 const v9 = rw::lerp(wg.c[m].last_pos, wg.c[m].pos, t);
                vec2 const nn = rw::normalized(v9 - v6);
                vec2 const p = custom::perpendicular_vector(nn);
                float const d = rw::distance(v9, v6) / 5.f;
                float const sq = mathf::clamp(mathf::pow(wg.c[m].stretched_fac, 0.35f), 0.5f, 1.5f);
                float num6 = detail::tentacle_contour(b.p.variant, wg.flying_mode, (float(m) + 0.5f) / float(n)) * sq;
                v[m * 4] = v6 - p * num6 + nn * d;
                v[m * 4 + 1] = v6 + p * num6 + nn * d;
                num6 = detail::tentacle_contour(b.p.variant, wg.flying_mode, (float(m) + 1.f) / float(n)) * sq;
                v[m * 4 + 2] = v9 - p * num6 - nn * d;
                v[m * 4 + 3] = v9 + p * num6 - nn * d;
                v6 = v9;
            }
            sprites::long_mesh(out, strip, v, n * 4, black);
        }
        // FeatherSprite and FeatherColorSprite, wing after wing
        for (uint32_t k = 0; k < b.wing_count; ++k) {
            wing const& wg = *b.wings[k];
            float const side = k % 2 == 0 ? 1.f : -1.f;
            for (uint32_t n = 0; n < fpw; ++n) {
                auto const& f = g.feathers[k * fpw + n];
                vec2 const conn = connected_pos(wg, f.wing_position, t);
                vec2 const fp = rw::lerp(f.last_pos, f.pos, t);
                float const rot = custom::aim(fp, conn);
                float const sx = mathf::lerp(3.f, f.width, (f.extended_fac + wg.flying_mode) * 0.5f) / 9.f * side * (king ? 1.3f : 1.f);
                float const sy = rw::distance(conn, fp) / 107.f;
                bool const claw = miros && n == fpw - 1;
                if (claw) {
                    float const e3 = mathf::pow(f.extended_fac, 3.f);
                    sprites::add(out, sprite{"MirosClaw", conn, rot + 200.f * side, vec2{side * e3, e3}, vec2{0.f, 0.3f}, black});
                    continue;
                }
                sprites::add(out, sprite{miros ? "MirosWingSolid" : "KrakenFeather", conn, rot, vec2{sx, sy}, vec2{0.5f, miros ? 0.94f : 0.97f}, black});
                add_alpha(out, sprite{miros ? "MirosWingColor" : "KrakenFeatherColor", conn, rot, vec2{sx, sy}, vec2{0.5f, miros ? 0.94f : 0.97f}, vec3{1.f, 1.f, 1.f}},
                    feather_color(b, g, f, wg));
            }
        }
        // the shields
        auto const shell = [&](uint32_t s, uint32_t i) { return rw::lerp(g.shells[s * 2 + i][2], g.shells[s * 2 + i][0], t); };
        auto const shield = [&](uint32_t s, bool front) {
            vec2 const p = rw::lerp(shell(s, 0), shell(s, 1), 0.1f);
            float const rot = custom::aim(shell(s, 1), shell(s, 0));
            float const fm = s < b.wing_count ? b.wings[s]->flying_mode : 0.f;
            float const mode = mathf::lerp(mathf::lerp(g.shell_modes[s][1], g.shell_modes[s][0], t), fm, 0.35f);
            float const sy = (front ? mathf::lerp(1.f, 0.5f, mode) : mathf::lerp(1.f, 1.2f, mode)) * (king ? 1.15f : 1.f);
            float const sx = (s == 1 ? -1.f : 1.f) * (king ? 1.15f : 1.f);
            vec3 col = front ? lerp3(ca, pal.black(), 0.55f) : lerp3(ca, pal.black(), 0.85f);
            if (g.albino)
                col = front ? lerp3(color2, black, 0.35f) : lerp3(color2, black, 0.75f);
            sprites::add(out, sprite{"KrakenShield0", p, rot, vec2{sx, sy}, vec2{0.5f, 1.f}, col});
        };
        shield(0, false);
        shield(1, false);
        // BodySprite
        sprites::add(out, sprite{"KrakenBody", rw::lerp(body1, body0, 0.5f), custom::aim(body0, body1), vec2{king ? 1.2f : 1.f, king ? 1.2f : 1.f}, vec2{0.5f, 0.5f}, black});
        shield(0, true);
        shield(1, true);

        // the king's tusks: the wires, the tusks behind the head
        vec2 const hdir = custom::dir_vec(tip, hp);
        vec2 const hperp = custom::perpendicular_vector(hdir);
        auto const tusk_front = [&](uint32_t side) {
            vec2 const hr = custom::deg_to_vec(b.tusks.head_rot);
            return (side == 0) == (mathf::sign(hr.x) == mathf::sign(hr.y));
        };
        auto const draw_tusk = [&](uint32_t side) {
            auto const& tk = b.tusks.t[side];
            // LaserSprite
            float num = mathf::lerp(tk.last_laser_alpha, tk.laser_alpha, t);
            vec3 col = hsl_rgb(g.color_b.x, 1.f, 0.5f);
            if (tk.mode == tusk_mode::charging) {
                num = tk.mode_counter % 6 < 3 ? 1.f : 0.f;
                if (tk.mode_counter % 2 == 0)
                    col = lerp3(col, vec3{1.f, 1.f, 1.f}, 0.5f);
            }
            float const off = side == 0 ? -90.f : 90.f;
            vec2 const v3 = hp + hdir * 15.f
                + hperp * rw::slerp(custom::deg_to_vec(b.tusks.last_head_rot + off), custom::deg_to_vec(b.tusks.head_rot + off), t).x * 7.f;
            vec2 const v4 = aim_dir(b, tk, side, t);
            if (num > 0.f) {
                vec2 const corner = detail::beam_end(room, v3, v4);
                vec2 const pp = custom::perpendicular_vector(v4);
                float const end = mathf::pow(num, 2.f) * (tk.mode == tusk_mode::charging ? 1.f : 0.5f);
                quad(out, v3 + v4 * 2.f + pp * 0.5f, v3 + v4 * 2.f - pp * 0.5f, corner - pp * 0.5f, corner + pp * 0.5f, rgba(col, num), rgba(col, num), rgba(col, end),
                    rgba(col, end));
            }
            // TuskSprite, TuskDetailSprite: MakeLongMesh(15, pointyTip)
            constexpr uint32_t segs = 15;
            vec2 const c0 = rw::lerp(tk.chunk_points[0].last_pos, tk.chunk_points[0].pos, t);
            vec2 const c1 = rw::lerp(tk.chunk_points[1].last_pos, tk.chunk_points[1].pos, t);
            vec2 const v5 = (c0 + c1) / 2.f;
            vec2 const v6 = custom::dir_vec(c1, c0);
            vec2 const a3 = custom::perpendicular_vector(v6);
            vec2 const zr = rw::slerp(tk.last_z_rot, tk.z_rot, t);
            float const sd = side == 0 ? -1.f : 1.f;
            vec2 v9 = v5 + v6 * -35.f + a3 * zr.y * sd * -15.f;
            float num2r = 0.f;
            vec2 v[segs * 4];
            for (uint32_t i = 0; i < segs; ++i) {
                float const f = mathf::inverse_lerp(0.f, float(segs - 1), float(i));
                vec2 const v10 = v5 + v6 * mathf::lerp(-30.f, 60.f, f) + tusk_bend(f) * a3 * 20.f * zr.x + tusk_prof_bend(f) * a3 * zr.y * sd * 10.f;
                vec2 const nn = rw::normalized(v10 - v9);
                vec2 const a4 = custom::perpendicular_vector(nn);
                float const d = rw::distance(v10, v9) / 5.f;
                float const num4 = tusk_rad(f, std::fabs(zr.y));
                v[i * 4] = v9 - a4 * (num4 + num2r) * 0.5f + nn * d;
                v[i * 4 + 1] = v9 + a4 * (num4 + num2r) * 0.5f + nn * d;
                if (i == segs - 1) {
                    v[i * 4 + 2] = v10 + nn * d;
                } else {
                    v[i * 4 + 2] = v10 - a4 * num4 - nn * d;
                    v[i * 4 + 3] = v10 + a4 * num4 - nn * d;
                }
                num2r = num4;
                v9 = v10;
            }
            constexpr uint32_t nv = segs * 4 - 1;
            vec4 cs[nv], cd[nv];
            for (uint32_t i = 0; i < nv; ++i) {
                float const f = mathf::inverse_lerp(0.f, float(nv - 1), float(i));
                cs[i] = rgba(lerp3(color3, vec3{1.f, 1.f, 1.f}, mathf::pow(f, 2.f)), 1.f);
                cd[i] = rgba(lerp3(lerp3(rgb(hsl_lerp(g.color_a, g.color_b, f)), pal.black(), 0.65f - 0.4f * f), color3, mathf::pow(f, 2.f)), b.tusks.pattern_displace);
            }
            sprites::long_mesh(out, strip, v, cs, nv);
            sprites::long_mesh(out, strip, v, cd, nv);
        };
        if (king) {
            // TuskWireSprite(0, 1)
            for (uint32_t side = 0; side < 2; ++side) {
                auto const& tk = b.tusks.t[side];
                float const loose = mathf::lerp(tk.last_wire_loose, tk.wire_loose, t);
                if (loose <= 0.f)
                    continue;
                vec2 const c0 = rw::lerp(tk.chunk_points[0].last_pos, tk.chunk_points[0].pos, t);
                vec2 const c1 = rw::lerp(tk.chunk_points[1].last_pos, tk.chunk_points[1].pos, t);
                vec2 const v5 = (c0 + c1) / 2.f;
                vec2 const v6 = custom::dir_vec(c1, c0);
                vec2 v9 = hp - hdir * 14.f;
                vec2 v[wire_points * 4];
                vec4 col[wire_points * 4];
                for (uint32_t k = 0; k < wire_points; ++k) {
                    vec2 v11 = rw::lerp(tk.wire[k].last_pos, tk.wire[k].pos, t);
                    if (loose < 1.f)
                        v11 = rw::lerp(rw::lerp(hp - hdir * 14.f, v5 + v6 * 6.f, mathf::inverse_lerp(0.f, float(wire_points - 1), float(k))), v11, loose);
                    if (k == wire_points - 1)
                        v11 = c0;
                    vec2 const nn = rw::normalized(v11 - v9);
                    vec2 const pp = custom::perpendicular_vector(nn);
                    float const d = k == wire_points - 1 ? 0.f : rw::distance(v11, v9) / 5.f;
                    v[k * 4] = v9 - pp + nn * d;
                    v[k * 4 + 1] = v9 + pp + nn * d;
                    v[k * 4 + 2] = v11 - pp - nn * d;
                    v[k * 4 + 3] = v11 + pp - nn * d;
                    v9 = v11;
                }
                for (uint32_t j = 0; j < wire_points * 4; ++j)
                    col[j] = rgba(lerp3(pal.black(), pal.fog(), 0.33f * std::sin(mathf::inverse_lerp(0.f, float(wire_points * 4 - 1), float(j)) * mathf::pi)), 1.f);
                sprites::long_mesh(out, strip, v, col, wire_points * 4);
            }
            for (uint32_t side = 0; side < 2; ++side)
                if (!tusk_front(side))
                    draw_tusk(side);
        }

        // NeckSprite
        vec2 v5 = hp;
        {
            vec2 v[neck_chunks * 4];
            vec2 v6 = b.pos(0, t);
            float num4 = king ? 11.f : 8.f;
            for (uint32_t j = 0; j < neck_chunks; ++j) {
                vec2 v7 = rw::lerp(b.neck.c[j].last_pos, b.neck.c[j].pos, t);
                if (j == neck_chunks - 1)
                    v7 = rw::lerp(v7, v5, 0.5f) - custom::deg_to_vec(num2) * (king ? 1.f : 5.f);
                vec2 const nn = rw::normalized(v7 - v6);
                vec2 const a = custom::perpendicular_vector(nn);
                float d = rw::distance(v7, v6) / 5.f;
                float const r = b.neck.c[j].rad;
                v[j * 4] = v6 - a * (r + num4) * 0.5f + nn * d;
                v[j * 4 + 1] = v6 + a * (r + num4) * 0.5f + nn * d;
                if (j == neck_chunks - 1 && !miros)
                    d = 0.f;
                v[j * 4 + 2] = v7 - a * r - nn * d;
                v[j * 4 + 3] = v7 + a * r - nn * d;
                num4 = r;
                v6 = v7;
            }
            sprites::long_mesh(out, strip, v, neck_chunks * 4, black);
        }
        if (king) {
            // TubeSprite(0, 1)
            for (uint32_t s = 0; s < 2; ++s) {
                vec2 v[neck_tube_points * 4];
                vec2 v6 = hp;
                float num4 = 0.f;
                for (uint32_t l = 0; l < neck_tube_points; ++l) {
                    vec2 v8 = rw::lerp(g.neck_tubes[s][l][1], g.neck_tubes[s][l][0], t);
                    if (l == neck_tube_points - 1)
                        v8 = b.pos(0, t);
                    vec2 const nn = rw::normalized(v8 - v6);
                    vec2 const a2 = custom::perpendicular_vector(nn);
                    float const d2 = rw::distance(v8, v6) / 5.f;
                    float const num5 = l % 3 == 0 ? 2.5f : 1.5f;
                    v[l * 4] = v6 - a2 * (num5 + num4) * 0.5f + nn * d2;
                    v[l * 4 + 1] = v6 + a2 * (num5 + num4) * 0.5f + nn * d2;
                    v[l * 4 + 2] = v8 - a2 * num5 - nn * d2;
                    v[l * 4 + 3] = v8 + a2 * num5 - nn * d2;
                    num4 = num5;
                    v6 = v8;
                }
                sprites::long_mesh(out, strip, v, neck_tube_points * 4, black);
            }
            // NeckLumpSprite(0, 1)
            for (uint32_t s = 0; s < 2; ++s) {
                auto const& tk = b.tusks.t[s];
                vec2 const c0 = rw::lerp(tk.chunk_points[0].last_pos, tk.chunk_points[0].pos, t);
                vec2 const c1 = rw::lerp(tk.chunk_points[1].last_pos, tk.chunk_points[1].pos, t);
                vec2 const tv5 = (c0 + c1) / 2.f;
                vec2 const tv6 = custom::dir_vec(c1, c0);
                vec2 const v7 = hp - tv6 * 10.f;
                vec2 const v8 = rw::lerp(hp, tv5, mathf::inverse_lerp(0.f, 0.25f, tk.attached));
                sprites::add(out, sprite{"Circle20", v7, custom::aim(v7, v8), vec2{0.6f, (rw::distance(v7, v8) + 4.f) / 20.f}, vec2{0.5f, 0.f}, black});
            }
        }
        // HeadSprite
        float const num3 = float(8 - g.head_graphic) * mathf::sign(num2) * 22.5f;
        int32_t const hg = custom::int_clamp(g.head_graphic, 0, 8);
        static char const* const heads[9] = {"KrakenHead0", "KrakenHead1", "KrakenHead2", "KrakenHead3", "KrakenHead4", "KrakenHead5", "KrakenHead6", "KrakenHead7", "KrakenHead8"};
        static char const* const eyes[9] = {"KrakenEyes0", "KrakenEyes1", "KrakenEyes2", "KrakenEyes3", "KrakenEyes4", "KrakenEyes5", "KrakenEyes6", "KrakenEyes7", "KrakenEyes8"};
        static char const* const masks[9] = {"KrakenMask0", "KrakenMask1", "KrakenMask2", "KrakenMask3", "KrakenMask4", "KrakenMask5", "KrakenMask6", "KrakenMask7", "KrakenMask8"};
        static char const* const arrows[9] = {"KrakenArrow0", "KrakenArrow1", "KrakenArrow2", "KrakenArrow3", "KrakenArrow4", "KrakenArrow5", "KrakenArrow6", "KrakenArrow7", "KrakenArrow8"};
        float const hsx = (num2 > 0.f ? -1.f : 1.f) * (king ? 1.15f : 1.f);
        float const hsy = king ? 1.15f : 1.f;
        sprites::add(out, sprite{heads[hg], v5, num2 - num3, vec2{hsx, hsy}, vec2{0.5f, 0.5f}, black});
        if (!miros) {
            // TuskSprite(0, 1)
            static char const* const tusks[3] = {"KrakenTusk0", "KrakenTusk1", "KrakenTusk2"};
            for (uint32_t i = 0; i < 2; ++i) {
                int32_t const n = custom::int_clamp(int32_t(std::lround(std::fabs(g.tusk_rotations[i]))), 0, 2);
                vec2 con[4];
                tusk_connection(b, i, t, con);
                vec2 const p = rw::lerp(g.tusks[i].last_pos, g.tusks[i].pos, t);
                sprites::add(out, sprite{tusks[n], con[0], custom::aim(p, con[0]), vec2{g.tusk_rotations[i] < 0.f ? 1.f : -1.f, 1.f}, vec2{0.5f, 1.f}, black});
            }
            // EyesSprite, MaskSprite (MaskArrowSprite)
            vec3 ec = lerp3(cb, vec3{0.f, 0.f, 0.f}, king ? 0.4f : 0.6f);
            if (g.albino)
                ec = lerp3(vec3{1.f, 0.f, 0.f}, vec3{0.f, 0.f, 0.f}, king ? 0.2f : 0.3f);
            sprites::add(out, sprite{eyes[hg], v5, num2 - num3, vec2{hsx, hsy}, vec2{0.5f, 0.5f}, ec});
            if (b.p.mask) {
                sprites::add(out, sprite{masks[hg], v5, num2 - num3, vec2{hsx, hsy}, vec2{0.5f, 0.5f}, color3});
                if (king)
                    sprites::add(out, sprite{arrows[hg], v5, num2 - num3, vec2{hsx, hsy}, vec2{0.5f, 0.5f},
                        lerp3(lerp3(rgb(hsl_lerp(g.color_a, g.color_b, 0.5f)), pal.black(), 0.53f), color3, 0.1f)});
            }
            if (king)
                for (uint32_t side = 0; side < 2; ++side)
                    if (tusk_front(side))
                        draw_tusk(side);
            return;
        }
        // the miros vulture's BeakGraphic halves: the mesh, then its teeth
        float const flip = mathf::lerp(g.last_head_flip, g.head_flip, t);
        float const hang = custom::vec_to_deg(hdir);
        float const head_rad = w.body_chunk_get_rad(creature::chunk(state, c, head));
        float const jaw = mathf::lerp(b.p.last_jaw_open, b.p.jaw_open, t);
        for (int32_t index = 0; index < 2; ++index) {
            float const sd = -1.f + 2.f * float(index);
            vec2 const v = custom::deg_to_vec(hang + 60.f * jaw * sd * flip * mathf::pow(std::fabs(flip), 0.5f));
            vec2 const a = custom::perpendicular_vector(v) * sd * mathf::sign(flip);
            vec2 m[13];
            m[0] = hp + hdir * head_rad;
            m[1] = hp + hperp * (1.f - 2.f * float(index)) * head_rad * flip;
            float const dd = 65.f * mathf::lerp(mathf::lerp(1.f, 0.6f, jaw), 1.f, std::fabs(flip));
            float const d2 = mathf::lerp(5.f, 7.f, jaw) * mathf::lerp(0.5f, 1.5f, g.beak_fatness);
            for (int32_t i = 1; i < 6; ++i) {
                float const f = float(i) / 6.f;
                vec2 const a2 = hp + hdir * head_rad * (1.f - f) * jaw + v * dd * f;
                m[i * 2] = a2 + a * d2 * mathf::lerp(0.6f, 1.f, jaw) * outer_shape(f) * mathf::pow(1.f - std::fabs(flip), 0.4f);
                m[i * 2 + 1] = a2 - a * d2 * outer_shape(f) * std::fabs(flip);
            }
            m[12] = hp + v * dd + a * d2 * 0.75f * jaw;
            sprites::long_mesh(out, strip, m, 13, black);
            for (uint32_t j = 0; j < g.teeth_count[index]; ++j) {
                float const* tt = g.teeth[index][j];
                vec2 const v2 = hp + hdir * head_rad * (1.f - tt[0]) * jaw + v * dd * tt[0];
                float const open = mathf::inverse_lerp(0.f, 0.1f, jaw);
                sprites::add(out, sprite{"LizardScaleA6", v2, custom::vec_to_deg(rw::normalized(-a - v * tt[2])),
                    vec2{0.4f * open * tt[1], 0.5f * outer_shape(tt[0]) * std::fabs(flip) * open * tt[1]}, vec2{0.5f, 0.8f}, black});
            }
        }
        // EyesSprite
        sprites::add(out, sprite{"Circle20", hp, 0.f, vec2{g.eye_size, g.eye_size}, vec2{0.5f, 0.5f}, g.eye_col});
        // EyeTrail: MakeLongMesh(14)
        {
            constexpr uint32_t segs = eye_trail_positions - 1;
            uint32_t const count = std::max<uint32_t>(g.eye_trail_count, 1u);
            auto const get = [&](int32_t i) { return g.eye_trail[custom::int_clamp(i, 0, int32_t(count) - 1)]; };
            auto const smooth = [&](int32_t i) { return rw::lerp(get(i + 1), get(i), t); };
            vec2 v[segs * 4];
            vec4 col[segs * 4];
            vec2 a = hp;
            float const dd = 8.f * g.eye_size;
            for (uint32_t i = 0; i < segs; ++i) {
                vec2 const sp = smooth(int32_t(i));
                vec2 const sp2 = smooth(int32_t(i) + 1);
                vec2 vec = rw::normalized(a - sp);
                vec2 const a2 = custom::perpendicular_vector(vec);
                vec *= rw::distance(a, sp2) / 5.f;
                v[i * 4] = a - a2 * dd - vec;
                v[i * 4 + 1] = a + a2 * dd - vec;
                v[i * 4 + 2] = sp - a2 * dd + vec;
                v[i * 4 + 3] = sp + a2 * dd + vec;
                a = sp;
            }
            for (uint32_t j = 0; j < segs * 4; ++j) {
                float const f = float(j) / float(segs * 4 - 1);
                col[j] = rgba(g.eye_col, mathf::min(1.f, mathf::pow(1.f - f, 1.5f)));
            }
            sprites::long_mesh(out, strip, v, col, segs * 4);
        }
        // LaserSprite
        float const la = mathf::lerp(g.last_laser_active, g.laser_active, t);
        if (la > 0.f) {
            vec3 const lc{1.f, 0.9f, 0.f};
            float const fl = mathf::lerp(g.last_flash, g.flash, t);
            vec2 const corner = detail::beam_end(room, hp, hdir);
            vec2 const v4 = -hdir;
            vec2 const pp = custom::perpendicular_vector(v4);
            float const end = mathf::pow(la, 2.f) * mathf::lerp(0.5f, 1.f, fl);
            quad(out, hp - v4 * 4.f + pp * 0.5f, hp - v4 * 4.f - pp * 0.5f, corner - pp * 0.5f, corner + pp * 0.5f, rgba(lc, la), rgba(lc, la), rgba(lc, end), rgba(lc, end));
        }
    }
}
