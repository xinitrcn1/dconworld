#include "hud.hpp"

#include <algorithm>
#include <cmath>

// Port notes
// - Kept close to HUD/KarmaMeter.cs, HUD/FoodMeter.cs and HUD/HUDCircle.cs;
//   comments name the vanilla members.
// - The HUD's owner is the player's slugcat; the map button and
//   showKarmaFoodRain are `input.reveal`.
// - Hunger (not vanilla) takes quarter pips away: a quarter count going
//   down is shown at once (QuarterPipShower only counts up), and the
//   quarters wait while a pip is still being eaten off the meter.

namespace hud {
    namespace {
        using rw::vec2;
        using rw::vec3;
        using rw::vec4;
        namespace mathf = rw::mathf;
        namespace custom = rw::custom;

        // HUDCircle.snapRad / snapThickness of FoodCircleA and FoodCircleB
        constexpr float snap_rad[2] = {11.5f, 5.5f};
        constexpr float snap_thickness[2] = {2.f, -1.f};
        constexpr std::string_view snap_graphic[2] = {"FoodCircleA", "FoodCircleB"};
        // FoodMeter.CircleDistance (no pups' meters)
        constexpr float circle_distance = 30.f;
        constexpr float pi = 3.1415927f;

        // Custom.FadableVectorCircleColors
        vec3 fadable_color(int32_t i) noexcept {
            switch (i) {
            case 1: return vec3{1.f, 0.f, 0.f};
            case 2: return vec3{0.f, 1.f, 0.f};
            case 3: return vec3{0.f, 0.f, 1.f};
            case 4: return vec3{0.529f, 0.365f, 0.184f};
            case 5: return vec3{0.f, 0.f, 0.f};
            case 6: return vec3{1.f, 0.7f, 0.35f};
            case 7: return vec3{0.373f, 0.11f, 0.831f};
            default: return vec3{1.f, 1.f, 1.f};
            }
        }

        // RainWorld.SaturatedGold (GoldRGB * 2, its alpha too)
        vec3 const saturated_gold{0.529f * 2.f, 0.365f * 2.f, 0.184f * 2.f};

        int32_t show_survival_limit(food_meter const& f) noexcept { return int32_t(std::lround(f.show_surv_lim)); }

        // MeterCircle.XAdd
        float x_add(food_meter const& f, int32_t number) noexcept {
            return circle_distance * float(number) + (number >= show_survival_limit(f) ? circle_distance / 2.f : 0.f);
        }

        // KarmaMeter.Radius (forceSleep is 0)
        float karma_radius(karma_meter const& k, input const& in) noexcept { return k.rad + (in.reinforced ? 8.f : 0.f); }
        // KarmaMeter.AnyVisibility (no reinforce animation)
        bool karma_any_visibility(karma_meter const& k, input const& in) noexcept { return in.reveal || k.fade > 0.f || k.force_visible_counter > 0; }

        // ---- FoodMeter.MeterCircle

        void init_plop(meter_circle& c) {
            c.rads[0][0] = snap_rad[0] - 1.5f;
            c.rads[0][1] += 0.5f;
            c.rads[1][1] += 0.5f;
            c.plopped = true;
        }
        void food_plop(meter_circle& c) {
            c.food_plopped = true;
            c.rads[1][1] += 2.f;
            c.food_plop_delay = 16;
        }
        void quarter_circle_plop(meter_circle& c) { c.rads[0][0] += 1.5f; }
        void eat_fade(meter_circle& c) {
            c.eat_counter = 50;
            c.eaten = true;
            c.rads[0][0] = snap_rad[0] + 1.5f;
            c.rads[0][1] += 0.6f;
        }

        void circle_update(meter_circle& c, food_meter const& f, int32_t number, input const& in) {
            c.last_slow_x_add = c.slow_x_add;
            c.slow_x_add = custom::lerp_and_tick(c.slow_x_add, x_add(f, number), 0.06f, 2.f);
            for (int i = 0; i < 2; ++i) {
                if (!c.plopped) {
                    c.rads[i][0] = snap_rad[i] / 2.f;
                    c.rads[i][1] = 0.f;
                } else {
                    c.rads[i][0] += c.rads[i][1];
                    c.rads[i][1] *= c.rads[i][0] < snap_rad[i] ? 0.8f : 0.95f;
                    c.rads[i][1] += (snap_rad[i] - c.rads[i][0]) * 0.2f;
                    c.rads[i][0] = custom::lerp_and_tick(c.rads[i][0], snap_rad[i], 0.0001f, 0.2f);
                }
            }
            float num = 1.f;
            c.circles[0].color = 0;
            // the pips still missing for hibernation flash in a shelter
            if (number < show_survival_limit(f) && number >= f.show_count && !c.eaten && in.in_shelter) {
                if (f.time_counter % 20 > 10) {
                    c.rads[0][0] *= 0.96f;
                    c.circles[0].color = 1;
                }
                num = 0.65f + 0.35f * std::sin(float(f.time_counter) / 20.f * pi * 2.f);
            }
            for (int j = 0; j < 2; ++j) {
                // HUDCircle.Update
                c.circles[j].last_rad = c.circles[j].rad;
                c.circles[j].last_fade = c.circles[j].fade;
                c.circles[j].rad = c.rads[j][0];
            }
            c.circles[0].fade = c.plopped ? f.fade * num : 0.f;
            if (c.eaten) {
                --c.eat_counter;
                if (c.eat_counter < 1) {
                    c.food_plopped = false;
                    c.eaten = false;
                }
                c.circles[1].fade = std::pow(mathf::inverse_lerp(0.f, 35.f, float(c.eat_counter)), 1.2f) * f.fade;
                if (c.eat_counter > 30) {
                    float const t = mathf::inverse_lerp(30.f, 50.f, float(c.eat_counter));
                    c.rads[0][0] = custom::lerp_and_tick(c.rads[0][0], 13.f + 0.5f * std::sin(t * pi), 0.02f, 1.5f + 0.5f * t);
                    c.rads[0][1] *= 0.f;
                } else if (c.eat_counter == 29) {
                    c.rads[0][1] = -1.5f;
                }
                c.rads[1][0] = custom::lerp_map(float(c.eat_counter), 40.f, 0.f, snap_rad[1], snap_rad[1] / 2.f);
            } else {
                c.circles[1].fade = c.plopped && c.food_plopped ? f.fade : 0.f;
            }
            if (c.food_plop_delay > 0) {
                --c.food_plop_delay;
                if (c.food_plop_delay == 12) {
                    c.rads[0][0] = snap_rad[0] + 2.f;
                    c.rads[0][1] += 1.f;
                }
            }
            if (f.last_fade == 0.f)
                c.plopped = false;
            c.circles[0].visible = c.plopped;
            c.circles[1].visible = c.plopped && c.food_plopped;
        }

        // ---- FoodMeter

        void reset_quarters(food_meter& f, input const& in) { f.display_quarter_food = in.quarter_food; }

        void update_show_count(food_meter& f, input const& in) {
            int32_t const count = f.max_food;
            if (f.show_count < in.food) {
                if (f.show_count_delay != 0) {
                    --f.show_count_delay;
                    return;
                }
                f.show_count_delay = 10;
                if (f.show_count >= 0 && f.show_count < count && !f.circles[f.show_count].food_plopped)
                    food_plop(f.circles[f.show_count]);
                ++f.show_count;
                reset_quarters(f, in);
            } else if (f.show_count > in.food) {
                if (f.eat_circle_delay == 0)
                    f.eat_circle_delay = 40;
                --f.eat_circle_delay;
                if (f.eat_circle_delay < 1) {
                    if (f.show_count - 1 < count)
                        eat_fade(f.circles[f.show_count - 1]);
                    --f.show_count;
                    f.eat_circle_delay = 0;
                }
            }
        }

        // QuarterPipShower.Update
        void quarter_update(food_meter& f, input const& in) {
            f.last_quarter_pip_sin = f.quarter_pip_sin;
            f.last_light_up = f.light_up;
            if (f.display_quarter_food > 0)
                f.quarter_pip_sin += 1.f;
            f.light_up *= 0.95f;
            // (not vanilla: hunger takes quarters away)
            if (in.quarter_food < f.display_quarter_food) {
                f.display_quarter_food = in.quarter_food;
                f.visible_counter = std::max(f.visible_counter, 80);
                return;
            }
            if (in.quarter_food > f.display_quarter_food) {
                f.visible_counter = 80;
                if (f.fade < 0.5f || f.show_count != in.food) {
                    f.quarter_pip_delay = 20;
                    return;
                }
                if (f.quarter_pip_delay > 0) {
                    --f.quarter_pip_delay;
                    return;
                }
                f.quarter_pip_delay = 20;
                ++f.display_quarter_food;
                f.light_up = 1.f;
                if (f.show_count < f.max_food)
                    quarter_circle_plop(f.circles[f.show_count]);
            }
        }

        void food_update(meter_state& m, input const& in) {
            auto& f = m.food;
            auto const& k = m.karma;
            for (int32_t i = f.max_food - 1; i >= 0; --i)
                circle_update(f.circles[i], f, i, in);
            if (f.last_count != in.food) {
                if (f.fade == 0.f && k.fade == 0.f)
                    f.down_in_corner = 1.f;
                f.last_count = in.food;
                f.visible_counter = 200;
            }
            f.last_fade = f.fade;
            f.last_pos = f.pos;
            f.last_show_surv_lim = f.show_surv_lim;
            // GameUpdate
            if (in.reveal) {
                if (f.fade < 1.f)
                    f.fade = mathf::min(1.f, f.fade + 0.1f);
                else
                    f.fade = mathf::max(1.f, f.fade - 0.1f);
                update_show_count(f, in);
            } else {
                if (f.visible_counter > 0) {
                    --f.visible_counter;
                    f.fade = mathf::min(1.f, f.fade + 0.1f);
                } else {
                    f.fade = mathf::max(0.f, f.fade - 0.0125f);
                }
                if (f.fade == 1.f)
                    update_show_count(f, in);
                else if (f.fade == 0.f)
                    f.show_count_delay = 15;
            }
            quarter_update(f, in);
            bool const karma_visible = karma_any_visibility(k, in);
            if (f.down_in_corner > 0.f && karma_visible)
                f.down_in_corner = mathf::max(0.f, f.down_in_corner - 0.0625f);
            else if (f.fade > 0.f && k.fade == 0.f && !karma_visible)
                f.down_in_corner = mathf::min(1.f, f.down_in_corner + 0.0625f);
            vec2 const corner{50.f, 25.f};
            vec2 const beside = k.pos + custom::deg_to_vec(mathf::lerp(90.f, 135.f, f.down_in_corner))
                * (karma_radius(k, in) + 22.f + custom::s_curve(std::pow(m.rain_fade, 0.4f), 0.5f) * 8.f);
            f.pos = rw::lerp(corner, beside, custom::s_curve(1.f - f.down_in_corner, 0.5f));
            // Update: the circles plop in one by one as the meter fades in
            if (f.fade > 0.f) {
                if (f.last_fade == 0.f) {
                    f.init_plop_circle = -1;
                    f.init_plop_delay = 0;
                }
                if (f.init_plop_circle < f.max_food) {
                    --f.init_plop_delay;
                    if (f.init_plop_delay < 1) {
                        if (f.init_plop_circle >= 0)
                            init_plop(f.circles[f.init_plop_circle]);
                        ++f.init_plop_circle;
                        f.init_plop_delay = std::max(2, f.init_plop_circle / 3);
                    }
                }
                ++f.time_counter;
            }
        }

        // ---- KarmaMeter

        void karma_update(meter_state& m, input const& in) {
            auto& k = m.karma;
            auto const& f = m.food;
            k.last_pos = k.pos;
            k.last_fade = k.fade;
            k.last_rad = k.rad;
            k.last_glowy_fac = k.glowy_fac;
            if (k.fade > 0.f) {
                k.glowy_fac = custom::lerp_and_tick(k.glowy_fac, k.fade * (in.reinforced ? 1.f : 0.9f), 0.1f, 1.f / 30.f);
                ++k.timer;
            } else {
                k.glowy_fac = 0.f;
            }
            k.last_reinforcement_cycle = k.reinforcement_cycle;
            k.reinforcement_cycle += 1.f / 90.f;
            if (f.down_in_corner > 0.f) {
                k.fade = mathf::max(0.f, k.fade - 0.05f);
            } else if (in.reveal) {
                // (the map fully shown, or showKarmaFoodRain)
                float const num = 1.f;
                if (k.fade < num)
                    k.fade = mathf::min(num, k.fade + 0.1f);
                else
                    k.fade = mathf::max(num, k.fade - 0.1f);
            } else {
                if (k.force_visible_counter > 0) {
                    --k.force_visible_counter;
                    k.fade = mathf::min(1.f, k.fade + 0.1f);
                } else {
                    k.fade = mathf::max(0.f, k.fade - 0.0125f);
                }
                if (f.fade > 0.f && k.fade > 0.f)
                    k.fade = mathf::min(k.fade + 0.1f, f.fade);
            }
            k.rad = custom::lerp_and_tick(k.rad, custom::lerp_map(k.fade, 0.f, 0.15f, 17.f, 22.5f, 1.3f), 0.12f, 0.1f);
        }

        void start(meter_state& m, input const& in) {
            m = meter_state{};
            m.started = true;
            auto& f = m.food;
            f.max_food = std::clamp(in.max_food, 1, max_food_circles);
            f.survival_limit = std::clamp(in.food_to_hibernate, 0, f.max_food);
            f.show_surv_lim = f.last_show_surv_lim = float(f.survival_limit);
            for (int32_t i = 0; i < f.max_food; ++i) {
                auto& c = f.circles[i];
                c.slow_x_add = c.last_slow_x_add = x_add(f, i);
                for (int j = 0; j < 2; ++j) {
                    c.circles[j].rad = c.circles[j].last_rad = 0.f;
                    c.rads[j][0] = snap_rad[j];
                }
            }
            // NewShowCount
            f.last_count = in.food;
            f.show_count = in.food;
            for (int32_t i = 0; i < f.max_food; ++i)
                f.circles[i].food_plopped = i < in.food;
            f.display_quarter_food = in.quarter_food;
        }

        // ---- drawing

        void add_sprite(std::vector<sprites::draw_cmd>& out, std::string_view element, vec2 pos, float scale, vec3 col, float alpha) {
            if (alpha <= 0.f)
                return;
            sprites::sprite s;
            s.element = element;
            s.pos = pos;
            s.scale = vec2{scale, scale};
            vec4 const c{col.x, col.y, col.z, mathf::clamp01(alpha)};
            sprites::add(out, s, {c, c, c, c});
        }

        // a Futile_White sprite with the FlatLight shader, `rx` by `ry`
        // (half its size): alpha (1 - d^2)^3.5 from the centre out
        void flat_light(std::vector<sprites::draw_cmd>& out, vec2 c, float rx, float ry, vec3 col, float alpha) {
            if (alpha <= 0.f || rx <= 0.f || ry <= 0.f)
                return;
            constexpr int rings = 6;
            constexpr int segs = 24;
            auto const falloff = [](float d) { return std::pow(1.f - mathf::min(d * d, 1.f), 3.5f); };
            for (int r = 0; r < rings; ++r) {
                float const d0 = float(r) / float(rings), d1 = float(r + 1) / float(rings);
                vec4 const ci{col.x, col.y, col.z, falloff(d0) * alpha};
                vec4 const co{col.x, col.y, col.z, falloff(d1) * alpha};
                for (int s = 0; s < segs; ++s) {
                    float const a0 = float(s) / float(segs) * 2.f * pi, a1 = float(s + 1) / float(segs) * 2.f * pi;
                    vec2 const u0{std::cos(a0) * rx, std::sin(a0) * ry}, u1{std::cos(a1) * rx, std::sin(a1) * ry};
                    sprites::add_flat_quad(out, {c + u0 * d0, c + u0 * d1, c + u1 * d1, c + u1 * d0}, {ci, co, co, ci});
                }
            }
        }

        // VectorCircleFadable: a ring from `rad - thickness` out to `rad`
        // (thickness -1: a disc)
        void vector_circle(std::vector<sprites::draw_cmd>& out, vec2 c, float rad, float thickness, vec3 col, float alpha) {
            if (alpha <= 0.f || rad <= 0.f)
                return;
            constexpr int segs = 32;
            float const inner = thickness < 0.f ? 0.f : mathf::max(0.f, rad - thickness);
            vec4 const k{col.x, col.y, col.z, mathf::clamp01(alpha)};
            for (int s = 0; s < segs; ++s) {
                float const a0 = float(s) / float(segs) * 2.f * pi, a1 = float(s + 1) / float(segs) * 2.f * pi;
                vec2 const u0{std::cos(a0), std::sin(a0)}, u1{std::cos(a1), std::sin(a1)};
                sprites::add_flat_quad(out, {c + u0 * inner, c + u0 * rad, c + u1 * rad, c + u1 * inner}, {k, k, k, k});
            }
        }

        // a pie of `quarters` quarters, clockwise from the top (stand in for QuarterPips)
        void quarter_pie(std::vector<sprites::draw_cmd>& out, vec2 c, float rad, int32_t quarters, vec3 col, float alpha) {
            if (alpha <= 0.f || rad <= 0.f || quarters < 1)
                return;
            int const segs = 8 * quarters;
            vec4 const k{col.x, col.y, col.z, mathf::clamp01(alpha)};
            for (int s = 0; s < segs; ++s) {
                float const a0 = float(s) / 32.f * 2.f * pi, a1 = float(s + 1) / 32.f * 2.f * pi;
                vec2 const u0{std::sin(a0), std::cos(a0)}, u1{std::sin(a1), std::cos(a1)};
                sprites::add_flat_quad(out, {c, c + u0 * rad, c + u1 * rad, c + u1 * rad}, {k, k, k, k});
            }
        }

        vec2 circle_draw_pos(food_meter const& f, meter_circle const& c, float ts) noexcept {
            return rw::lerp(f.last_pos, f.pos, ts) + vec2{mathf::lerp(c.last_slow_x_add, c.slow_x_add, ts), 0.f};
        }

        // HUDCircle.Draw at the MeterCircle's position
        void draw_circle(std::vector<sprites::draw_cmd>& out, circle const& h, int j, vec2 pos, float ts, bool atlas) {
            float const rad = mathf::lerp(h.last_rad, h.rad, ts);
            float thickness = snap_thickness[j];
            if (rad <= 0.f || !h.visible || (h.last_fade == 0.f && h.fade == 0.f))
                return;
            if (thickness > rad)
                thickness = rad;
            float const fade = mathf::lerp(h.last_fade, h.fade, ts);
            if (rad == snap_rad[j] && thickness == snap_thickness[j] && atlas)
                add_sprite(out, snap_graphic[j], pos, 1.f, fadable_color(h.color), fade);
            else
                vector_circle(out, pos, rad, thickness, fadable_color(h.color), fade);
        }

        void build_food(meter_state const& m, input const& in, float ts, atlas_has const& has, std::vector<sprites::draw_cmd>& out) {
            auto const& f = m.food;
            float const num = mathf::lerp(f.last_fade, f.fade, ts);
            if (num <= 0.f && f.last_fade <= 0.f && f.fade <= 0.f)
                return;
            vec2 const pos = rw::lerp(f.last_pos, f.pos, ts);
            // MeterCircle.gradient (added first)
            for (int32_t i = 0; i < f.max_food; ++i) {
                float const a = 0.1f * std::pow(mathf::max(0.f, num), 2.f);
                flat_light(out, circle_draw_pos(f, f.circles[i], ts), 50.f, 50.f, vec3{0.f, 0.f, 0.f}, a);
            }
            // darkFade
            flat_light(out, pos + vec2{circle_distance * float(f.max_food) * 0.35f, -70.f}, 300.f, 150.f, vec3{0.f, 0.f, 0.f}, 0.2f * num * num);
            // lineSprite: the hibernation requirement ("pixel" 2 x 34.5)
            if (num > 0.f) {
                float const x = pos.x + circle_distance * (f.show_surv_lim - 0.25f);
                vec4 const w{1.f, 1.f, 1.f, mathf::clamp01(num)};
                sprites::add_flat_quad(out, {vec2{x - 1.f, pos.y + 17.25f}, vec2{x + 1.f, pos.y + 17.25f}, vec2{x + 1.f, pos.y - 17.25f}, vec2{x - 1.f, pos.y - 17.25f}},
                    {w, w, w, w});
            }
            // the circles
            for (int32_t i = 0; i < f.max_food; ++i) {
                auto const& c = f.circles[i];
                vec2 const at = circle_draw_pos(f, c, ts);
                for (int j = 0; j < 2; ++j)
                    draw_circle(out, c.circles[j], j, at, ts, has.food_circles);
            }
            // QuarterPipShower.Draw
            int32_t const q = f.display_quarter_food;
            if (q >= 1 && q <= 3 && f.show_count >= 0 && f.show_count < f.max_food) {
                auto const& c = f.circles[f.show_count];
                auto const& c0 = c.circles[0];
                float const s = mathf::lerp(f.last_quarter_pip_sin, f.quarter_pip_sin, ts);
                float const light = mathf::max(0.f, mathf::lerp(f.last_light_up, f.light_up, ts));
                float const a = mathf::lerp(c0.last_fade, c0.fade, ts)
                    * mathf::lerp(mathf::lerp(0.2f, 0.5f, std::pow(mathf::clamp01(0.5f + 0.5f * std::sin(s / 20.f)), 0.4f)), 0.6f, light * light);
                float const scale = mathf::lerp(c0.last_rad, c0.rad, ts) / snap_rad[0];
                vec2 const at = circle_draw_pos(f, c, ts);
                if (has.quarter_pips) {
                    static constexpr std::string_view names[3] = {"QuarterPips1", "QuarterPips2", "QuarterPips3"};
                    add_sprite(out, names[q - 1], at, scale, fadable_color(c0.color), a);
                } else {
                    quarter_pie(out, at, snap_rad[1] * scale, q, fadable_color(c0.color), a);
                }
            }
            (void)in;
        }

        void build_karma(meter_state const& m, input const& in, float ts, atlas_has const& has, std::vector<sprites::draw_cmd>& out) {
            auto const& k = m.karma;
            float const num = mathf::lerp(k.last_fade, k.fade, ts);
            vec2 const pos = rw::lerp(k.last_pos, k.pos, ts);
            vec3 const base = in.ripple ? saturated_gold : vec3{1.f, 1.f, 1.f};
            float const scale = mathf::lerp(k.last_rad, k.rad, ts) / 22.5f;
            // darkFade, karmaSprite, glowSprite, then ringSprite (added later)
            flat_light(out, pos, 150.f, 150.f, vec3{0.f, 0.f, 0.f}, 0.2f * num * num);
            if (has.symbol)
                add_sprite(out, symbol_element(in), pos, scale, base, num);
            float num5 = 0.7f + 0.3f * std::sin(2.f * pi * mathf::lerp(k.last_reinforcement_cycle, k.reinforcement_cycle, ts));
            float const num6 = mathf::lerp(k.last_glowy_fac, k.glowy_fac, ts);
            num5 *= mathf::inverse_lerp(0.9f, 1.f, num6);
            float const glow = (60.f + 10.f * num5) * num6;
            flat_light(out, pos, glow, glow, base, (0.2f + 0.2f * num5) * num6 * num * num);
            if (in.reinforced && has.reinforced_ring)
                add_sprite(out, "smallKarmaRingReinforced", pos, scale, base, num);
        }
    }

    std::string_view symbol_element(input const& in) noexcept {
        if (in.ripple) {
            // RippleSymbolSprite(true, level): to the half step, away from zero
            static constexpr std::string_view ripple[9] = {"smallRipple1.0", "smallRipple1.5", "smallRipple2.0", "smallRipple2.5", "smallRipple3.0",
                "smallRipple3.5", "smallRipple4.0", "smallRipple4.5", "smallRipple5.0"};
            int32_t const halves = int32_t(std::floor(std::fabs(in.ripple_level) * 2.f + 0.5f));
            return ripple[std::clamp(halves, 2, 10) - 2];
        }
        // KarmaSymbolSprite(true, (karma, cap)), Downpour's -1 allowed
        static constexpr std::string_view low[6] = {"smallKarma-1", "smallKarma0", "smallKarma1", "smallKarma2", "smallKarma3", "smallKarma4"};
        static constexpr std::string_view high[5][5] = {
            {"smallKarma5-5", "smallKarma5-6", "smallKarma5-7", "smallKarma5-8", "smallKarma5-9"},
            {"smallKarma6-6", "smallKarma6-6", "smallKarma6-7", "smallKarma6-8", "smallKarma6-9"},
            {"smallKarma7-7", "smallKarma7-7", "smallKarma7-7", "smallKarma7-8", "smallKarma7-9"},
            {"smallKarma8-8", "smallKarma8-8", "smallKarma8-8", "smallKarma8-8", "smallKarma8-9"},
            {"smallKarma9-9", "smallKarma9-9", "smallKarma9-9", "smallKarma9-9", "smallKarma9-9"},
        };
        // (the KarmaMeter clamps its displayKarma.x to [0, cap])
        int32_t const cap = std::clamp(in.karma_cap, 0, 9);
        int32_t const x = std::clamp(in.karma, 0, cap);
        if (x < 5)
            return low[std::clamp(x, -1, 4) + 1];
        int32_t const hx = std::clamp(x, 5, 9);
        int32_t const hy = std::clamp(cap, x, 9);
        return high[hx - 5][hy - 5];
    }

    void update(meter_state& m, input const& in) {
        // the RainMeter's fade (shown with the map)
        m.rain_fade = in.reveal ? mathf::min(1.f, m.rain_fade + 0.1f) : mathf::max(0.f, m.rain_fade - 0.0125f);
        // HUD.parts order: the FoodMeter, then the KarmaMeter
        food_update(m, in);
        karma_update(m, in);
    }

    void advance(meter_state& m, input const& in, uint32_t tick) {
        if (!m.started || m.food.max_food != std::clamp(in.max_food, 1, max_food_circles) || tick < m.last_tick) {
            start(m, in);
            m.last_tick = tick;
            return;
        }
        // (a long stall or fast forward: the meters only need to catch up a little)
        uint32_t const n = std::min<uint32_t>(tick - m.last_tick, 40u);
        for (uint32_t i = 0; i < n; ++i)
            update(m, in);
        m.last_tick = tick;
    }

    void build(meter_state const& m, input const& in, float time_stacker, atlas_has const& has, std::vector<sprites::draw_cmd>& out) {
        float const ts = mathf::clamp01(time_stacker);
        build_food(m, in, ts, has, out);
        build_karma(m, in, ts, has, out);
    }

    symbol_place karma_symbol_place(meter_state const& m, float time_stacker) noexcept {
        auto const& k = m.karma;
        float const ts = mathf::clamp01(time_stacker);
        return symbol_place{rw::lerp(k.last_pos, k.pos, ts), mathf::lerp(k.last_rad, k.rad, ts) / 22.5f, mathf::lerp(k.last_fade, k.fade, ts)};
    }
}
