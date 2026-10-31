#include <algorithm>
#include <cmath>
#include <string_view>

#include "scavenger_graphics.hpp"
#include "body_parts.hpp"
#include "creature.hpp"
#include "items.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "scavenger.hpp"

namespace scavenger_graphics {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using world::room_view;
    using player::limb_mode;
    using sprites::sprite;
    using scavenger::looks;
    using scavenger::graphics_state;
    using scavenger::move_mode;
    using scavenger::animation;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        // ScavengerHand: Limb(rad 3, surface friction 0.5, air friction 0.99,
        // hunt speed 24, quickness 0.95), armLength 50; ScavengerLeg: Limb(1,
        // 0.5, 0.99, 12, 0.95)
        constexpr body_parts::limb_params hand_lp{3.f, 0.5f, 0.99f, 24.f, 0.95f};
        constexpr body_parts::limb_params leg_lp{1.f, 0.5f, 0.99f, 12.f, 0.95f};
        constexpr float arm_length = 50.f;
        constexpr int head_draw_pos = 0, chest_draw_pos = 2, hips_draw_pos = 4;
        // LizardScaleA0..6's sourcePixelSize.y (the B ones are the same size)
        constexpr float scale_heights[7] = {18.f, 14.f, 10.f, 18.f, 37.f, 29.f, 17.f};
        constexpr std::string_view scale_a[7] = {"LizardScaleA0", "LizardScaleA1", "LizardScaleA2", "LizardScaleA3", "LizardScaleA4", "LizardScaleA5", "LizardScaleA6"};
        constexpr std::string_view scale_b[7] = {"LizardScaleB0", "LizardScaleB1", "LizardScaleB2", "LizardScaleB3", "LizardScaleB4", "LizardScaleB5", "LizardScaleB6"};

        dcon::scavenger_data_id data(sys::game_state const& state, dcon::creature_id c) { return creature::scavenger_data(state.world, c); }
        creature::scavenger_payload const& pl(sys::game_state const& state, dcon::creature_id c) { return state.world.creature_get_payload(c).scavenger; }
        vec2 chunk_pos(sys::game_state const& state, dcon::creature_id c, uint32_t i) { return state.world.body_chunk_get_pos(scavenger::chunk(state, c, i)); }
        vec2 chunk_last_pos(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_last_pos(scavenger::chunk(state, c, i));
        }
        vec2 chunk_vel(sys::game_state const& state, dcon::creature_id c, uint32_t i) { return state.world.body_chunk_get_vel(scavenger::chunk(state, c, i)); }
        float chunk_rad(sys::game_state const& state, dcon::creature_id c, uint32_t i) { return state.world.body_chunk_get_rad(scavenger::chunk(state, c, i)); }

        // Color.Lerp
        vec3 mix(vec3 a, vec3 b, float t) { return a + (b - a) * mathf::clamp01(t); }
        vec4 rgba(vec3 c, float a = 1.f) { return vec4{c.x, c.y, c.z, a}; }
        vec3 rgb(scavenger::hsl const& h) { return custom::hsl2rgb(h.hue, h.saturation, h.lightness); }
        float wrap01(float v) {
            if (v > 1.f)
                return v - 1.f;
            if (v < 0.f)
                return v + 1.f;
            return v;
        }
        float push_from_half(float v, float e) {
            if (v == 0.5f)
                return 0.5f;
            if (v < 0.5f)
                return custom::lerp_map(v, 0.f, 0.5f, 0.f, 0.5f, e);
            return custom::lerp_map(v, 1.f, 0.5f, 1.f, 0.5f, e);
        }
        float dist01(float a, float b) { return std::min(std::min(std::fabs(a - b), std::fabs(a + 1.f - b)), std::fabs(a - 1.f - b)); }
        // Custom.Saturate
        vec3 saturate(vec3 c, float s) {
            float const lo = std::min(std::min(c.x, c.y), c.z), hi = std::max(std::max(c.x, c.y), c.z);
            return mix(c, vec3{mathf::inverse_lerp(lo, hi, c.x), mathf::inverse_lerp(lo, hi, c.y), mathf::inverse_lerp(lo, hi, c.z)}, s);
        }
        vec2 rot(vec2 v, float deg) { return body_parts::rotate_around_origo(v, deg); }
        vec2 lerp2(vec2 a, vec2 b, float t) { return a + (b - a) * t; }

        // the ThrowChargeAnimation's UseTarget
        vec2 use_target(sys::game_state const& state, dcon::creature_id c) {
            auto const& a = state.world.scavenger_data_get_ai(data(state, c));
            if (creature::live(state, a.charge_target) && a.charge_chunk < creature::chunk_count_of(state, a.charge_target))
                return state.world.body_chunk_get_pos(creature::chunk_of(state, a.charge_target, a.charge_chunk));
            if (a.has_aim_target)
                return a.aim_target;
            return vec2{0.f, 0.f};
        }
        bool charging(sys::game_state const& state, dcon::creature_id c) {
            auto const& a = state.world.scavenger_data_get_ai(data(state, c));
            return a.anim == animation::throw_charge && a.discontinue < 20;
        }

        //
        // The spine (ScavengerGraphics.OnSpine*)
        //
        struct spine {
            graphics_state const& g;
            looks const& lk;
            float t;
            vec2 dp(int i) const { return lerp2(g.draw_positions[i][1], g.draw_positions[i][0], t); }
            float flip() const { return mathf::lerp(g.last_flip, g.flip, t); }
            vec2 seg_pos(int seg) const {
                if (seg == 0) {
                    vec2 const p = dp(0);
                    float const rhs = mathf::inverse_lerp(0.f, 10.f, custom::distance_to_line(p, seg_pos(hips_draw_pos), seg_pos(chest_draw_pos)) * -flip());
                    return p + custom::perpendicular_vector(rw::normalized(dp(1) - p)) * 5.f * rhs * flip();
                }
                return dp(seg);
            }
            float index(float f) const {
                float const num = f * mathf::lerp(g.spine_lengths[1], g.spine_lengths[0], t);
                float a = 0.f;
                float num2 = rw::distance(dp(0), dp(1));
                int n = 0;
                while (num2 < num) {
                    a = num2;
                    ++n;
                    if (n >= 4)
                        break;
                    // (vanilla lerps the last positions with themselves here)
                    num2 += rw::distance(g.draw_positions[n][1], g.draw_positions[n + 1][1]);
                }
                return float(n) + mathf::inverse_lerp(a, num2, num);
            }
            static int clampi(int i) { return custom::int_clamp(i, 0, 4); }
            vec2 pos(float f) const {
                float const n = index(f);
                int const fl = int(std::floor(n));
                return lerp2(seg_pos(clampi(fl)), seg_pos(clampi(fl + 1)), n - std::floor(n));
            }
            vec2 seg_dir(int seg) const {
                if (seg > 3)
                    return custom::dir_vec(seg_pos(3), seg_pos(4));
                return custom::dir_vec(seg_pos(seg), seg_pos(seg + 1));
            }
            vec2 dir(float f) const {
                float const n = index(f);
                int const fl = int(std::floor(n));
                return rw::slerp(seg_dir(fl), seg_dir(fl + 1), mathf::inverse_lerp(0.5f, 1.f, n - std::floor(n)));
            }
            float dt01() const { return rw::dot(rw::normalized(seg_pos(1) - seg_pos(0)), rw::normalized(seg_pos(2) - seg_pos(1))); }
            vec2 seg_perp(int seg) const {
                if (seg == 0) {
                    float const f = dt01();
                    return rw::slerp(custom::perpendicular_vector(custom::dir_vec(seg_pos(0), seg_pos(1))) * mathf::sign(f), seg_perp(1), mathf::inverse_lerp(0.5f, 1.f, 1.f - std::fabs(f)));
                }
                if (seg > 3)
                    return custom::perpendicular_vector(custom::dir_vec(seg_pos(3), seg_pos(4)));
                return custom::perpendicular_vector(custom::dir_vec(seg_pos(seg), seg_pos(seg + 1)));
            }
            vec2 perp(float f) const {
                float const n = index(f);
                int const fl = int(std::floor(n));
                return rw::slerp(seg_perp(fl), seg_perp(fl + 1), mathf::inverse_lerp(0.5f, 1.f, n - std::floor(n)));
            }
            float seg_width(int seg) const {
                switch (seg) {
                case 0: return 4.f * mathf::lerp(0.5f, 1.5f, lk.neck_thickness) * std::fabs(dt01() == 0.f ? 0.f : mathf::sign(dt01()));
                case 1: return mathf::lerp(8.f, 18.f, lk.fatness);
                case 2: return mathf::lerp(10.f, 20.f, lk.fatness);
                case 3: return mathf::lerp(4.f, 8.f, lk.waist_width());
                case 4: return 10.f;
                default: return 0.f;
                }
            }
            float width(float f) const {
                float const n = index(f);
                int const fl = int(std::floor(n));
                return mathf::lerp(seg_width(fl), seg_width(fl + 1), mathf::inverse_lerp(0.5f, 1.f, n - std::floor(n)));
            }
            vec2 back_surface(vec2 rel) const {
                float const x = mathf::clamp(rel.x - flip(), -1.f, 1.f);
                return pos(rel.y) + perp(rel.y) * width(rel.y) * 0.5f * x;
            }
            vec2 belly_surface(vec2 rel) const {
                float const x = mathf::clamp(rel.x + flip(), -1.f, 1.f);
                float const y = mathf::lerp(rel.y, 1.f, custom::lerp_map(dt01(), 1.f, -1.f, 0.f, 0.5f));
                return pos(y) + perp(y) * width(y) * 0.5f * x;
            }
            vec2 seg_outwards(int seg, float dt) const {
                if (seg == 0) {
                    vec2 const lhs = custom::perpendicular_vector(custom::dir_vec(seg_pos(0), seg_pos(1)));
                    return rw::slerp(custom::perpendicular_vector(custom::dir_vec(seg_pos(1), seg_pos(2))), lhs * (dt == 0.f ? 0.f : mathf::sign(dt)), mathf::pow(std::fabs(dt), 3.f));
                }
                return seg_perp(std::max(seg, 1));
            }
            vec2 outwards(vec2 ps) const {
                float const n = index(ps.y);
                int const fl = int(std::floor(n));
                float const dt = dt01();
                float const x = mathf::clamp(ps.x - flip(), -1.f, 1.f);
                vec2 const s = rw::slerp(seg_outwards(fl, dt), seg_outwards(fl + 1, dt), n - std::floor(n)) * (x == 0.f ? 0.f : mathf::sign(x));
                // (vanilla slerps towards the vector (ps.y, timeStacker))
                return rw::slerp(s, vec2{ps.y, t}, mathf::pow(1.f - std::fabs(x), 3.f));
            }
            vec2 seg_up(int seg, float dt, float uflp) const {
                if (seg == 0) {
                    vec2 const lhs = custom::perpendicular_vector(custom::dir_vec(seg_pos(0), seg_pos(1))) * -mathf::sign(uflp);
                    float const f = custom::distance_to_line(seg_pos(0), seg_pos(1), seg_pos(2));
                    return rw::slerp(lhs * mathf::sign(f) * mathf::sign(uflp), -seg_dir(1), std::fabs(dt));
                }
                if (seg > 3)
                    return custom::perpendicular_vector(custom::dir_vec(seg_pos(3), seg_pos(4))) * -mathf::sign(uflp);
                return custom::perpendicular_vector(custom::dir_vec(seg_pos(seg), seg_pos(seg + 1))) * -mathf::sign(uflp);
            }
            vec2 seg_dir_for_ups(int seg) const { return seg == 0 ? seg_dir(1) : seg_dir(seg); }
            vec2 up(float f) const {
                float const n = index(f);
                int const fl = int(std::floor(n));
                float const fp = flip();
                float const dt = dt01();
                vec2 const fu = rw::slerp(seg_dir_for_ups(fl), seg_dir_for_ups(fl + 1), mathf::inverse_lerp(0.5f, 1.f, n - std::floor(n)));
                return rw::slerp(rw::slerp(seg_up(fl, dt, fp), seg_up(fl + 1, dt, fp), n - std::floor(n)), -fu, 1.f - std::fabs(fp));
            }
        };

        // ScavengerGraphics.HeadDir(t), BodyAxis(t)
        vec2 head_dir(sys::game_state const& state, dcon::creature_id c, graphics_state const& g, float t) {
            vec2 const a = rw::clamp_magnitude(lerp2(g.last_look_point, g.look_point, t) - lerp2(chunk_last_pos(state, c, 0), chunk_pos(state, c, 0), t), 300.f) / 300.f * 0.2f;
            vec2 const b = rw::clamp_magnitude(lerp2(g.draw_positions[head_draw_pos][1], g.draw_positions[head_draw_pos][0], t)
                                   - lerp2(g.draw_positions[chest_draw_pos][1], g.draw_positions[chest_draw_pos][0], t), 100.f) / 100.f;
            return rw::normalized(a + b);
        }
        float body_axis(graphics_state const& g, float t) { return -custom::vec_to_deg(rw::normalized(rw::slerp(g.last_body_axis, g.body_axis, t))); }

        // ScavengerHand.MyFlip
        float my_flip(graphics_state const& g, int limb, float t) {
            vec2 const hp = lerp2(g.hands[limb].limb.last_pos, g.hands[limb].limb.pos, t);
            vec2 const hips = lerp2(g.draw_positions[hips_draw_pos][1], g.draw_positions[hips_draw_pos][0], t);
            vec2 const chest = lerp2(g.draw_positions[chest_draw_pos][1], g.draw_positions[chest_draw_pos][0], t);
            return mathf::lerp(custom::lerp_map(mathf::lerp(g.last_flip, g.flip, t), -1.f + float(limb), float(limb), -1.f, 1.f),
                custom::lerp_map(custom::distance_to_line(hp, hips, chest), 10.f, -10.f, -1.f, 1.f), mathf::lerp(g.last_swim_arms, g.swim_arms, t));
        }

        // ScavengerHand.WeaponDir
        vec2 weapon_dir(sys::game_state const& state, dcon::creature_id c, graphics_state const& g, int limb) {
            auto const& b = state.world.scavenger_data_get_body(data(state, c));
            vec2 a = custom::perpendicular_vector(chunk_pos(state, c, 0), g.hands[limb].limb.pos) * -my_flip(g, limb, 1.f);
            a += custom::dir_vec(chunk_pos(state, c, 1), chunk_pos(state, c, 0)) * (b.has_swing_pos ? 0.1f : 1.f);
            if (state.world.scavenger_data_get_ai(data(state, c)).anim == animation::throw_charge)
                a += custom::dir_vec(g.hands[limb].limb.pos, use_target(state, c)) * 2.f;
            return rw::normalized(a);
        }

        // ScavengerGraphics.ItemPosition, ItemDirection
        vec2 item_position(sys::game_state const& state, dcon::creature_id c, graphics_state const& g, looks const& lk, uint32_t grasp) {
            if (grasp == 0)
                return g.hands[0].limb.pos + g.hands[0].spear_pos_add;
            auto const it = scavenger::held_item(state, c, grasp);
            auto const t = state.world.item_get_type(it);
            if (t == world::item_type::spear || t == world::item_type::explosive_spear || t == world::item_type::electric_spear)
                return spine{g, lk, 1.f}.back_surface(vec2{-g.flip * 0.9f, 0.4f + float(grasp - 1) * 0.1f});
            return lerp2(chunk_pos(state, c, 1), chunk_pos(state, c, 0), 0.4f - 0.1f * float(grasp - 1));
        }
        vec2 item_direction(sys::game_state const& state, dcon::creature_id c, graphics_state const& g, looks const& lk, uint32_t grasp) {
            if (grasp == 0)
                return weapon_dir(state, c, g, 0);
            auto const it = scavenger::held_item(state, c, grasp);
            auto const t = state.world.item_get_type(it);
            if (t == world::item_type::spear || t == world::item_type::explosive_spear || t == world::item_type::electric_spear) {
                vec2 const axis = rw::slerp(g.body_axis, custom::dir_vec(g.draw_positions[hips_draw_pos][1], g.draw_positions[chest_draw_pos][1]), 0.75f);
                return rot(custom::deg_to_vec(45.f - g.flip * float(grasp - 1) * 10.f + custom::lerp_map(g.flip, -1.f, 1.f, -30.f, 0.f)), custom::vec_to_deg(axis));
            }
            return custom::dir_vec(state.world.body_chunk_get_pos(items::chunk(state, it)), item_position(state, c, g, lk, grasp));
        }

        //
        // The rolls
        //
        // BackDecals.GeneratePattern
        void generate_pattern(looks& lk, rw::unity_random& r, bool wobbly) {
            int32_t n = 0;
            vec2 tmp[looks::max_decals];
            switch (lk.pattern) {
            case scavenger::back_pattern::spine_ridge: {
                lk.top = mathf::lerp(0.07f, 0.3f, r.value());
                lk.bottom = mathf::lerp(0.6f, 1.f, r.value());
                float const num = mathf::lerp(2.5f, 12.f, r.value());
                n = std::min(int32_t((lk.bottom - lk.top) * 100.f / num), looks::max_decals);
                for (int32_t i = 0; i < n; ++i)
                    tmp[i] = vec2{0.f, mathf::lerp(lk.top, lk.bottom, n > 1 ? float(i) / float(n - 1) : 0.f)};
                break;
            }
            case scavenger::back_pattern::double_spine_ridge: {
                lk.top = mathf::lerp(0.07f, 0.3f, r.value());
                lk.bottom = mathf::lerp(0.6f, 1.f, r.value());
                if (wobbly)
                    lk.bottom = mathf::lerp(lk.bottom, 0.5f, r.value());
                float const num = mathf::lerp(4.5f, 12.f, r.value());
                int32_t const m = std::min(int32_t((lk.bottom - lk.top) * 100.f / num), looks::max_decals / 2);
                for (int32_t j = 0; j < m; ++j) {
                    float const y = mathf::lerp(lk.top, lk.bottom, m > 1 ? float(j) / float(m - 1) : 0.f);
                    tmp[j * 2] = vec2{-0.9f, y};
                    tmp[j * 2 + 1] = vec2{0.9f, y};
                }
                n = m * 2;
                break;
            }
            case scavenger::back_pattern::random_back_blotch: {
                float const value = r.value();
                float const v2 = r.value();
                float const v3 = r.value();
                n = std::min(int32_t(mathf::lerp(mathf::lerp(20.f, 4.f, lk.scruffy), 40.f, mathf::lerp(value, v2, 0.5f * v3))), looks::max_decals);
                for (int32_t k = 0; k < n; ++k)
                    tmp[k] = custom::rnv(r);
                lk.top = mathf::lerp(0.02f, 0.2f, r.value());
                lk.bottom = mathf::lerp(0.4f, 0.9f, mathf::pow(r.value(), 1.5f));
                for (int32_t l = 0; l < n; ++l)
                    tmp[l].y = custom::lerp_map(tmp[l].y, -1.f, 1.f, lk.top, lk.bottom);
                break;
            }
            }
            // orderby y descending (stable)
            std::stable_sort(tmp, tmp + n, [](vec2 a, vec2 b) { return a.y > b.y; });
            for (int32_t i = 0; i < n; ++i)
                lk.positions[i] = tmp[i];
            lk.decal_count = uint8_t(n);
        }

        // HardBackSpikes / WobblyBackTufts' colorAlphas
        void color_alphas(looks& lk, rw::unity_random& r) {
            if (lk.colored <= 0.f)
                return;
            if (r.value() < 0.25f + 0.5f * lk.colored) {
                float lo = 3.4e38f, hi = -3.4e38f;
                for (int32_t j = 0; j < lk.decal_count; ++j) {
                    lo = mathf::min(lo, lk.positions[j].y);
                    hi = mathf::max(hi, lk.positions[j].y);
                }
                float const p2 = mathf::lerp(0.2f, 1.2f, r.value());
                for (int32_t k = 0; k < lk.decal_count; ++k)
                    lk.color_alphas[k] = mathf::lerp(lk.colored, 0.f, mathf::pow(mathf::inverse_lerp(lo, hi, lk.positions[k].y), p2));
                return;
            }
            for (int32_t l = 0; l < lk.decal_count; ++l)
                lk.color_alphas[l] = lk.colored;
        }

        // Eartlers.DefineBranch: the branch and its mirror
        struct vtx {
            vec2 pos;
            float rad;
        };
        void define_branch(looks& lk, vtx const* v, int32_t n) {
            for (int32_t m = 0; m < 2 && lk.branch_count < looks::max_branches; ++m) {
                auto const b = lk.branch_count++;
                lk.branch_length[b] = uint8_t(n);
                for (int32_t i = 0; i < n; ++i) {
                    lk.branch_pos[b][i] = m == 0 ? v[i].pos : vec2{-v[i].pos.x, v[i].pos.y};
                    lk.branch_rad[b][i] = v[i].rad;
                }
            }
        }
        // Eartlers.GenerateSegments (a plain scavenger's)
        void generate_eartlers(looks& lk, rw::unity_random& r) {
            lk.branch_count = 0;
            vtx l[4];
            l[0] = {vec2{0.f, 0.f}, 1.f};
            l[1] = {custom::deg_to_vec(mathf::lerp(40.f, 90.f, r.value())) * 0.4f, 1.f};
            vec2 const f2 = custom::deg_to_vec(mathf::lerp(15.f, 45.f, r.value()));
            vec2 f3 = f2 - custom::deg_to_vec(mathf::lerp(40.f, 90.f, r.value())) * 0.4f;
            if (f3.x < 0.2f)
                f3.x = mathf::lerp(f3.x, f2.x, 0.4f);
            l[2] = {f3, 1.5f};
            l[3] = {f2, 2.f};
            define_branch(lk, l, 4);
            vec2 const p1 = lk.branch_pos[0][1], p2 = lk.branch_pos[0][2], p3 = lk.branch_pos[0][3];
            int32_t const n7 = (rw::distance(p1, p2) > 0.6f && r.value() < 0.5f) ? 2 : 1;
            {
                vec2 const f4 = lerp2(p1, p2, mathf::lerp(0.f, n7 == 1 ? 0.7f : 0.25f, r.value()));
                l[0] = {p1, 1.f};
                l[1] = {f4, 1.2f};
                l[2] = {f4 + p3 - p2 + custom::deg_to_vec(r.value() * 360.f) * 0.1f, 1.75f};
                define_branch(lk, l, 3);
            }
            if (n7 == 2) {
                vec2 const f4 = lerp2(p1, p2, mathf::lerp(0.45f, 0.7f, r.value()));
                l[0] = {f4, 1.2f};
                l[1] = {f4 + p3 - p2 + custom::deg_to_vec(r.value() * 360.f) * 0.1f, 1.75f};
                define_branch(lk, l, 2);
            }
            bool const flag = r.value() < 0.5f;
            if (flag) {
                float const a = 90.f + mathf::lerp(-20.f, 20.f, r.value());
                vec2 f5 = custom::deg_to_vec(a) * mathf::lerp(0.2f, 0.5f, r.value());
                if (f5.y > p1.y - 0.1f)
                    f5.y -= 0.2f;
                float const num8 = mathf::lerp(0.8f, 2.f, r.value());
                if (r.value() < 0.5f) {
                    float const d = mathf::lerp(120.f, 170.f, r.value());
                    f5 += custom::deg_to_vec(d) * mathf::lerp(0.1f, 0.3f, r.value());
                    l[0] = {vec2{0.f, 0.f}, num8};
                    l[1] = {f5, num8};
                    define_branch(lk, l, 2);
                } else {
                    l[0] = {vec2{0.f, 0.f}, 1.f};
                    l[1] = {f5, (1.f + num8) / 2.f};
                    float const d = mathf::lerp(95.f, 170.f, r.value());
                    l[2] = {f5 + custom::deg_to_vec(d) * mathf::lerp(0.1f, 0.2f, r.value()), num8};
                    define_branch(lk, l, 3);
                }
            }
            if (r.value() > 0.25f || !flag) {
                float const num9 = 1.f + r.value() * 1.5f;
                bool const flag2 = r.value() < 0.5f;
                l[0] = {vec2{0.f, 0.f}, 1.f};
                float const num10 = mathf::lerp(95.f, 135.f, r.value());
                float const num11 = mathf::lerp(0.25f, 0.4f, r.value());
                l[1] = {custom::deg_to_vec(num10) * num11, flag2 ? 0.8f : mathf::lerp(1.f, num9, 0.3f)};
                float const d = mathf::lerp(5.f, 35.f, r.value());
                float const len = mathf::max(num11 + 0.1f, mathf::lerp(0.3f, 0.6f, r.value()));
                l[2] = {custom::deg_to_vec(num10 + d) * len, flag2 ? 0.8f : mathf::lerp(1.f, num9, 0.6f)};
                l[3] = {rw::normalized(l[2].pos) * (rw::magnitude(l[2].pos) + mathf::lerp(0.15f, 0.25f, r.value())), num9};
                define_branch(lk, l, 4);
            }
        }

        // ScavengerGraphics.GenerateColors (the Elite's body hue and red
        // eyes in)
        void generate_colors(looks& lk, scavenger::personality const& p, rw::unity_random& r, bool elite) {
            float const mel = lk.general_melanin;
            float num = r.value() * 0.1f;
            if (r.value() < 0.025f)
                num = mathf::pow(r.value(), 0.4f);
            if (elite)
                num = mathf::pow(r.value(), 5.f);
            float num2;
            {
                float const a = mathf::lerp(-1.f, 1.f, r.value());
                float const b = mathf::pow(r.value(), 2.f);
                num2 = wrap01(num + a * 0.3f * b);
            }
            auto& body = lk.body_color;
            {
                float const s = mathf::lerp(0.05f, 1.f, mathf::pow(r.value(), 0.85f));
                float const l = mathf::lerp(0.05f, 0.8f, r.value());
                body = scavenger::hsl{num, s, l};
            }
            body.saturation *= 1.f - mel;
            body.lightness = mathf::lerp(body.lightness, 0.5f + 0.5f * mathf::pow(r.value(), 0.8f), 1.f - mel);
            auto const body_black = [&] {
                vec3 const c = rgb(body);
                float v = custom::lerp_map((c.x + c.y + c.z) / 3.f, 0.04f, 0.8f, 0.3f, 0.95f, 0.5f);
                float const a = mathf::lerp(0.5f, 1.f, r.value());
                float const t1 = r.value();
                float const t2 = r.value();
                float const t3 = r.value();
                v = mathf::lerp(v, a, t1 * t2 * t3);
                return v * mel;
            };
            lk.body_color_black = body_black();
            vec2 f{body.saturation, mathf::lerp(-1.f, 1.f, body.lightness * (1.f - lk.body_color_black))};
            if (rw::magnitude(f) < 0.5f) {
                f = rw::lerp(f, rw::normalized(f), mathf::inverse_lerp(0.5f, 0.3f, rw::magnitude(f)));
                body = scavenger::hsl{body.hue, mathf::inverse_lerp(-1.f, 1.f, f.x), mathf::inverse_lerp(-1.f, 1.f, f.y)};
                lk.body_color_black = body_black();
            }
            float num3;
            if (r.value() < custom::lerp_map(lk.body_color_black, 0.5f, 0.8f, 0.9f, 0.3f)) {
                float const a = mathf::lerp(-1.f, 1.f, r.value());
                float const b = mathf::pow(r.value(), 1.5f);
                num3 = num2 + a * 0.1f * b;
                num3 = wrap01(mathf::lerp(num3, 0.15f, r.value()));
            } else {
                float const base = r.value() < 0.5f ? custom::decimal(num + 0.5f) : custom::decimal(num2 + 0.5f);
                float const a = mathf::lerp(-1.f, 1.f, r.value());
                float const b = mathf::pow(r.value(), 2.f);
                num3 = base + a * 0.25f * b;
                if (r.value() < mathf::lerp(0.8f, 0.2f, p.energy))
                    num3 = mathf::lerp(num3, 0.15f, r.value());
                num3 = wrap01(num3);
            }
            auto& head = lk.head_color;
            {
                float const hue = r.value() < 0.75f ? num2 : num3;
                head = scavenger::hsl{hue, 1.f, 0.05f + 0.15f * r.value()};
            }
            head.saturation *= mathf::pow(1.f - mel, 2.f);
            head.lightness = mathf::lerp(head.lightness, 0.5f + 0.5f * mathf::pow(r.value(), 0.8f), 1.f - mel);
            head.saturation *= 0.1f + 0.9f * mathf::inverse_lerp(0.1f, 0.f, dist01(body.hue, head.hue) * custom::lerp_map(std::fabs(0.5f - head.lightness), 0.f, 0.5f, 1.f, 0.3f));
            if (head.lightness < 0.5f)
                head.lightness *= 0.5f + 0.5f * mathf::inverse_lerp(0.2f, 0.05f, dist01(body.hue, head.hue));
            {
                vec3 const c = rgb(head);
                float v = custom::lerp_map((c.x + c.y + c.z) / 3.f, 0.035f, 0.26f, 0.7f, 0.95f, 0.25f);
                float const a = mathf::lerp(0.8f, 1.f, r.value());
                float const t1 = r.value();
                float const t2 = r.value();
                float const t3 = r.value();
                v = mathf::lerp(v, a, t1 * t2 * t3);
                v *= 0.2f + 0.7f * mel;
                lk.head_color_black = mathf::max(v, lk.body_color_black);
            }
            head.saturation = custom::lerp_map(head.lightness * (1.f - lk.head_color_black), 0.f, 0.15f, 1.f, head.saturation);
            if (head.lightness > body.lightness)
                head = body;
            if (head.saturation < body.saturation * 0.75f) {
                if (r.value() < 0.5f)
                    head.hue = body.hue;
                else
                    head.lightness *= 0.25f;
                head.saturation = body.saturation * 0.75f;
            }
            auto& deco = lk.decoration_color;
            {
                float hue;
                if (r.value() < 0.65f)
                    hue = num;
                else
                    hue = r.value() < 0.5f ? num2 : num3;
                float const s = r.value();
                float const l = 0.5f + 0.5f * mathf::pow(r.value(), 0.5f);
                deco = scavenger::hsl{hue, s, l};
            }
            deco.lightness *= mathf::lerp(mel, r.value(), 0.5f);
            {
                float const l = r.value() < 0.2f ? 0.5f + r.value() * 0.5f : 0.5f;
                lk.eye_color = scavenger::hsl{elite ? 0.f : num3, 1.f, l};
            }
            if (lk.colored_pupils > 0)
                lk.eye_color.lightness = mathf::lerp(lk.eye_color.lightness, 1.f, 0.3f);
            if (head.lightness * (1.f - lk.head_color_black) > lk.eye_color.lightness / 2.f && (lk.pupil_size == 0.f || lk.deep_pupils))
                lk.eye_color.lightness *= 0.2f;
            float const value = r.value();
            float const value2 = r.value();
            lk.belly_color = scavenger::hsl{mathf::lerp(body.hue, deco.hue, value * 0.7f), body.saturation * mathf::lerp(1.f, 0.5f, value), body.lightness + 0.05f + 0.3f * value2};
            lk.belly_color_black = mathf::lerp(lk.body_color_black, 1.f, 0.3f * mathf::pow(value2, 1.4f));
            if (r.value() < 0.033333335f) {
                head.lightness = mathf::lerp(0.2f, 0.35f, r.value());
                lk.head_color_black *= mathf::lerp(1.f, 0.8f, r.value());
                lk.belly_color.hue = mathf::lerp(lk.belly_color.hue, head.hue, mathf::pow(r.value(), 0.5f));
            }
        }

        // ScavengerGraphics.IndividualVariations
        void individual_variations(looks& lk, scavenger::personality const& p, rw::unity_random& r, bool elite) {
            lk.general_melanin = push_from_half(r.value(), 2.f);
            lk.head_size = custom::clamped_random_variation(0.5f, 0.5f, 0.1f, r);
            lk.eartler_width = r.value();
            {
                float const a = r.value();
                float const t = r.value() * 0.4f;
                lk.eye_size = mathf::pow(mathf::max(0.f, mathf::lerp(a, mathf::pow(lk.head_size, 0.5f), t)), mathf::lerp(0.95f, 0.55f, p.sympathy));
            }
            lk.narrow_eyes = r.value() < mathf::lerp(0.3f, 0.7f, p.sympathy) ? 0.f : mathf::pow(r.value(), mathf::lerp(0.5f, 1.5f, p.sympathy));
            if (elite)
                lk.narrow_eyes = 1.f;
            lk.eyes_angle = mathf::pow(r.value(), mathf::lerp(2.5f, 0.5f, mathf::pow(p.energy, 0.03f)));
            {
                float const a = r.value();
                float const t = r.value() * 0.2f;
                lk.fatness = mathf::lerp(a, p.dominance, t);
            }
            if (p.energy < 0.5f)
                lk.fatness = mathf::lerp(lk.fatness, 1.f, r.value() * mathf::inverse_lerp(0.5f, 0.f, p.energy));
            else
                lk.fatness = mathf::lerp(lk.fatness, 0.f, r.value() * mathf::inverse_lerp(0.5f, 1.f, p.energy));
            {
                float const a = r.value();
                float const t = r.value();
                float const t2 = r.value();
                lk.narrow_waist = mathf::lerp(mathf::lerp(a, 1.f - lk.fatness, t), 1.f - p.energy, t2);
            }
            {
                float const a = mathf::pow(r.value(), 1.5f - p.aggression);
                float const t = r.value() * 0.5f;
                lk.neck_thickness = mathf::lerp(a, 1.f - lk.fatness, t);
            }
            lk.pupil_size = 0.f;
            lk.deep_pupils = 0;
            lk.colored_pupils = 0;
            if (r.value() < 0.65f && lk.eye_size > 0.4f && lk.narrow_eyes < 0.3f) {
                if (r.value() < mathf::pow(p.sympathy, 1.5f) * 0.8f) {
                    lk.pupil_size = mathf::lerp(0.4f, 0.8f, mathf::pow(r.value(), 0.5f));
                    if (r.value() < 0.6666667f)
                        lk.colored_pupils = uint8_t(r.range(1, 4));
                } else {
                    lk.pupil_size = 0.7f;
                    lk.deep_pupils = 1;
                }
            }
            if (r.value() < lk.general_melanin) {
                if (r.value() < 0.3f)
                    lk.hands_head_color = r.value();
                else
                    lk.hands_head_color = r.value() < 0.6f ? 1.f : 0.f;
            } else {
                if (r.value() < 0.2f)
                    lk.hands_head_color = r.value();
                else
                    lk.hands_head_color = r.value() < 0.8f ? 1.f : 0.f;
            }
            lk.legs_size = r.value();
            {
                float const a = r.value();
                float const t = r.value();
                lk.arm_thickness = mathf::lerp(a, mathf::lerp(p.dominance, lk.fatness, 0.5f), t);
            }
            lk.colored_eartler_tips = r.value() < 1.f / mathf::lerp(1.2f, 10.f, lk.general_melanin);
            lk.wide_teeth = r.value();
            lk.tail_segs = uint8_t(r.value() < 0.5f ? 0 : r.range(1, 5));
            lk.scruffy = 0.f;
            if (r.value() < 0.25f)
                lk.scruffy = mathf::pow(r.value(), 0.3f);
            // (vanilla sets it to 1 right after)
            lk.scruffy = 1.f;
        }

        //
        // The limbs' updates
        //
        struct ctx {
            sys::game_state& state;
            dcon::creature_id c;
            room_view const& room;
            graphics_state& g;
            looks const& lk;
            creature::scavenger_payload const& x;
            scavenger::body_state const& b;
            scavenger::ai_state const& a;
            rw::rng& rng;
            vec2 main() const { return chunk_pos(state, c, 0); }
        };

        // ScavengerHand.CheckForGrabPos / GrabPosLegal
        bool grab_pos_legal(ctx const& k, vec2 p) {
            vec2 const main = k.main();
            return custom::dist_less(main, p, arm_length * 1.5f) || custom::dist_less(main + chunk_vel(k.state, k.c, 0) * 5.f, p, arm_length * 1.5f);
        }
        std::optional<vec2> check_for_grab_pos(ctx const& k, int limb) {
            if (!k.b.has_knuckle_pos)
                return std::nullopt;
            vec2 const main = k.main();
            float const num = custom::aim(main, k.b.knuckle_pos);
            for (float n2 = 0.f; n2 < 45.f; n2 += 5.f)
                for (int i = -1; i <= 1; i += 2) {
                    auto const r = scavenger::exact_terrain_ray_trace_pos(k.room, main, main + custom::deg_to_vec(num + n2 * float(i) + (limb == 0 ? -4.f : 4.f)) * arm_length * 10.5f);
                    if (r && grab_pos_legal(k, *r))
                        return r;
                }
            return std::nullopt;
        }

        // ScavengerHand.StandardLocomotionProcedure
        void standard_locomotion(ctx& k, int limb) {
            auto& h = k.g.hands[limb];
            auto& lb = h.limb;
            lb.hunt_speed = hand_lp.hunt_speed;
            h.has_grab_pos = 0;
            bool flag = true;
            vec2 f = lb.pos;
            move_mode m = k.x.climbing_up_coming ? move_mode::climb : k.x.mov_mode;
            vec2 const main = k.main();
            float const body_aim = custom::aim(chunk_pos(k.state, k.c, 1), chunk_pos(k.state, k.c, 0));
            float const mf = my_flip(k.g, limb, 1.f);
            if (m == move_mode::stand_still) {
                lb.hunt_speed *= 0.2f;
                f = main + rot(vec2{mf * 30.f, 0.f}, body_aim);
                for (int i = 0; i < 3; ++i)
                    if (!k.room.get_tile(f + vec2{0.f, -20.f}).solid())
                        f.y -= 20.f;
            } else if (m == move_mode::crawl) {
                lb.hunt_speed *= 0.6f;
                vec2 const off{mathf::sign(mf) * 5.f + (limb == 0 ? -5.f : 5.f), 0.f};
                if (k.b.has_knuckle_pos)
                    f = k.b.knuckle_pos + rot(off, custom::aim(chunk_pos(k.state, k.c, 0), k.b.knuckle_pos));
                else
                    f = main + rot(off, body_aim);
                vec2 const other = k.g.hands[1 - limb].limb.absolute_hunt_pos;
                if (custom::dist_less(f, other, 30.f))
                    f = other + custom::dir_vec(other, f) * 30.f;
            } else if (m == move_mode::climb) {
                flag = false;
                if (k.b.has_swing_pos) {
                    if (limb == k.b.swing_arm) {
                        lb.mode = limb_mode::hunt_absolute_position;
                        lb.absolute_hunt_pos = k.b.swing_pos;
                        lb.pos = k.b.swing_pos;
                        f = k.b.swing_pos;
                    } else if (k.b.has_next_swing_pos) {
                        lb.mode = limb_mode::hunt_absolute_position;
                        lb.absolute_hunt_pos = lerp2((main + lb.pos) / 2.f, k.b.next_swing_pos, 0.3f + 0.7f * mathf::inverse_lerp(100.f, 50.f, rw::distance(main, k.b.next_swing_pos)));
                    } else {
                        lb.mode = limb_mode::dangle;
                        vec2 const v = main + rot(vec2{mathf::sign(mf) * 8.f + (limb == 0 ? -11.f : 11.f), -21.f}, body_aim);
                        lb.vel *= 0.4f;
                        lb.vel += (v - lb.pos) / 5.f;
                    }
                } else {
                    if (lb.mode == limb_mode::hunt_absolute_position) {
                        if (!custom::dist_less(main, lb.absolute_hunt_pos, arm_length))
                            lb.mode = limb_mode::dangle;
                    } else {
                        for (int n = int(k.b.connection_count) - 1; n >= 0 && lb.mode != limb_mode::hunt_absolute_position; --n) {
                            vec2 const v2 = room_view::middle_of_tile(k.b.connections[n].dest);
                            body_parts::find_grip(lb, k.room, main, v2, arm_length, v2, -2, -2, false);
                        }
                    }
                    if (lb.mode == limb_mode::dangle) {
                        flag = true;
                        if (k.b.connection_count > 0) {
                            f = room_view::middle_of_tile(k.b.connections[k.b.connection_count - 1].dest);
                            f += rot(vec2{limb == 0 ? -10.f : 10.f, 10.f}, body_aim);
                        }
                    }
                }
            } else {
                f = main + rot(vec2{mathf::sign(mf) * 30.f, 0.f}, body_aim);
            }
            if (flag) {
                if (!custom::dist_less(f, main, arm_length))
                    f = main + custom::dir_vec(main, f) * arm_length;
                if (k.room.get_tile(f).solid())
                    if (auto const hit = scavenger::exact_terrain_ray_trace_pos(k.room, main, f))
                        f = *hit;
                if (lb.mode == limb_mode::hunt_absolute_position) {
                    if (!custom::dist_less(main, lb.absolute_hunt_pos, arm_length))
                        lb.mode = limb_mode::dangle;
                    if (k.x.mov_mode == move_mode::stand_still && !custom::dist_less(f, lb.absolute_hunt_pos, 40.f))
                        body_parts::find_grip(lb, k.room, main, f, arm_length, f, -2, -2, false);
                } else {
                    body_parts::find_grip(lb, k.room, main, f, arm_length, f, -2, -2, false);
                }
            }
            if (lb.mode != limb_mode::hunt_absolute_position && k.x.climbing_up_coming && !k.b.has_swing_pos) {
                vec2 const r1 = custom::rnv(k.rng);
                float const v1 = k.rng.value();
                vec2 const r2 = custom::rnv(k.rng);
                float const v2 = k.rng.value();
                body_parts::find_grip(lb, k.room, main, main + r1 * arm_length * v1, arm_length, main + r2 * arm_length * v2, -2, -2, false);
            }
        }

        // ScavengerHand.Update (no pointing, gestures or rummaging)
        void hand_update(ctx& k, int limb) {
            auto& h = k.g.hands[limb];
            auto& lb = h.limb;
            auto const& w = k.state.world;
            vec2 const main = k.main();
            body_parts::limb_update(lb, hand_lp, main, chunk_vel(k.state, k.c, 0), chunk_pos(k.state, k.c, 1), k.room);
            h.last_spear_pos_add = h.spear_pos_add;
            auto const held = scavenger::held_item(k.state, k.c, 0);
            bool const spear = held && (w.item_get_type(held) == world::item_type::spear || w.item_get_type(held) == world::item_type::explosive_spear
                                           || w.item_get_type(held) == world::item_type::electric_spear);
            if (limb == 0 && (k.x.mov_mode == move_mode::run || k.x.mov_mode == move_mode::stand_still) && spear)
                h.spear_pos_add = weapon_dir(k.state, k.c, k.g, 0) * 20.f;
            else
                h.spear_pos_add *= 0.f;
            if (!scavenger::consious(k.state, k.c)) {
                lb.mode = limb_mode::dangle;
            } else if (limb == 0 && k.a.anim == animation::throw_ && k.a.thrown_item && w.item_is_valid(k.a.thrown_item)) {
                lb.mode = limb_mode::hunt_absolute_position;
                lb.absolute_hunt_pos = w.body_chunk_get_pos(items::chunk(k.state, k.a.thrown_item));
                if (k.a.anim_age < 5)
                    lb.pos = lb.absolute_hunt_pos;
            } else if (limb == 0 && charging(k.state, k.c)) {
                lb.mode = limb_mode::hunt_absolute_position;
                vec2 f = rw::slerp(-custom::dir_vec(main, use_target(k.state, k.c)),
                    custom::dir_vec(k.g.draw_positions[hips_draw_pos][1], k.g.draw_positions[chest_draw_pos][1]), 0.6f);
                float const rhs = std::sin(0.15707964f * k.a.charge_cycle);
                f += custom::dir_vec(k.g.draw_positions[head_draw_pos + 1][1], k.g.draw_positions[head_draw_pos][1]) * rhs * (0.1f + 0.9f * k.a.charge_shake) * 0.2f;
                lb.absolute_hunt_pos = main + f * 35.f;
                vec2 const hd = k.g.draw_positions[head_draw_pos][1];
                if (custom::dist_less(lb.absolute_hunt_pos, hd, 10.f))
                    lb.absolute_hunt_pos += custom::dir_vec(hd, lb.absolute_hunt_pos) * (10.f - rw::distance(hd, lb.absolute_hunt_pos));
            } else if (k.x.mov_mode == move_mode::swim) {
                lb.mode = limb_mode::dangle;
                float const flip = k.x.flip;
                if (k.x.moving) {
                    float const ph = (k.g.cycle * 0.3f + float(limb) * 0.5f) * mathf::pi * 2.f;
                    float const t = std::sin(ph);
                    float const xx = main.x + (limb == 0 ? -1.f : 1.f) * 5.f + flip * mathf::lerp(10.f, 50.f, t);
                    lb.absolute_hunt_pos = vec2{xx, k.room.float_water_level - 12.f + 20.f * std::cos(ph) * flip};
                } else {
                    float const xx = main.x + (limb == 0 ? -1.f : 1.f) * 35.f + flip * 15.f;
                    lb.absolute_hunt_pos = vec2{xx, k.room.float_water_level - 4.f};
                }
                lb.vel += rw::clamp_magnitude(lb.absolute_hunt_pos - lb.pos, 4.f) / 5.f;
                lb.pos = lb.absolute_hunt_pos;
            } else if (k.x.mov_mode == move_mode::run && !k.x.climbing_up_coming) {
                lb.hunt_speed = hand_lp.hunt_speed;
                if (h.has_grab_pos) {
                    lb.mode = limb_mode::hunt_absolute_position;
                    lb.absolute_hunt_pos = h.grab_pos;
                    if (k.b.has_knuckle_pos && !custom::dist_less(h.grab_pos, k.b.knuckle_pos, 40.f)) {
                        auto const f2 = check_for_grab_pos(k, limb);
                        if (f2 && rw::distance(k.b.knuckle_pos, *f2) < rw::distance(k.b.knuckle_pos, h.grab_pos))
                            h.grab_pos = *f2;
                    }
                    if (!grab_pos_legal(k, h.grab_pos))
                        h.has_grab_pos = 0;
                } else {
                    lb.mode = limb_mode::dangle;
                    if (auto const f2 = check_for_grab_pos(k, limb)) {
                        h.grab_pos = *f2;
                        h.has_grab_pos = 1;
                    }
                }
            } else {
                standard_locomotion(k, limb);
            }
            // out of the chest
            vec2 f3 = k.g.draw_positions[chest_draw_pos][0];
            f3 += custom::perpendicular_vector(rw::normalized(f3 - k.g.draw_positions[hips_draw_pos][0])) * (1.f - std::fabs(k.g.flip)) * 10.f * (limb == 0 ? -1.f : 1.f);
            f3 += custom::dir_vec(k.g.draw_positions[hips_draw_pos][0], k.g.draw_positions[chest_draw_pos][0]) * 5.f;
            if (custom::dist_less(lb.pos, f3, 10.f)) {
                vec2 const bb = custom::dir_vec(f3, lb.pos) * (10.f - rw::distance(f3, lb.pos));
                lb.vel += bb;
                lb.pos += bb;
            }
            body_parts::as_body_part(lb, hand_lp.rad, [&](player::body_part& bp) {
                if (lb.mode != limb_mode::dangle)
                    body_parts::connect_to_point(bp, main, arm_length, false, 0.f, chunk_vel(k.state, k.c, 0), 0.4f, 0.1f);
                else
                    body_parts::connect_to_point(bp, main, arm_length, false, 0.f, vec2{0.f, 0.f}, 0.f, 0.f);
            });
            if (lb.mode == limb_mode::dangle) {
                if (k.room.point_submerged(lb.pos))
                    lb.vel *= 0.8f;
                else
                    lb.vel.y -= 0.9f;
            }
        }

        // ScavengerLeg.Update
        void leg_update(ctx& k, int limb) {
            auto& lb = k.g.legs[limb];
            vec2 const hips = chunk_pos(k.state, k.c, 1), hips_vel = chunk_vel(k.state, k.c, 1);
            float const leg_length = mathf::lerp(20.f, 40.f, k.lk.legs_size);
            body_parts::limb_update(lb, leg_lp, hips, hips_vel, chunk_pos(k.state, k.c, 0), k.room);
            body_parts::as_body_part(lb, leg_lp.rad, [&](player::body_part& bp) { body_parts::connect_to_point(bp, hips, leg_length, false, 0.f, hips_vel, 0.8f, 0.5f); });
            vec2 const ideal = k.x.mov_mode == move_mode::run ? hips + rw::clamp_magnitude(hips_vel * 10.f, leg_length) : hips;
            float const flip = k.x.flip;
            float const body_aim = custom::aim(hips, chunk_pos(k.state, k.c, 0));
            if (scavenger::consious(k.state, k.c)) {
                if (k.x.mov_mode == move_mode::swim) {
                    lb.mode = limb_mode::dangle;
                    float const ph = (k.g.cycle + float(limb) * 0.5f) * mathf::pi * 2.f;
                    vec2 f{(limb == 0 ? -10.f : 10.f) - flip * 7.f + std::sin(ph) * 12.5f * flip, -leg_length * 0.6f + std::cos(ph) * 3.5f * flip};
                    f = rot(f, body_aim);
                    lb.vel += rw::clamp_magnitude(k.g.draw_positions[hips_draw_pos][0] + f - lb.pos, 8.f) / 2.f;
                } else if (k.x.mov_mode != move_mode::climb || !k.b.has_swing_pos || k.b.swing_climb_counter < 30) {
                    lb.vel += custom::dir_vec(k.main(), hips);
                    if (lb.mode == limb_mode::hunt_absolute_position) {
                        if (!custom::dist_less(ideal, lb.absolute_hunt_pos, leg_length))
                            lb.mode = limb_mode::hunt_relative_position;
                    } else {
                        lb.relative_hunt_pos = vec2{-flip * 10.f + (limb == 0 ? -4.f : 4.f), 10.f};
                        body_parts::find_grip(lb, k.room, ideal, ideal, leg_length, ideal + rot(vec2{limb == 0 ? -1.f : 1.f, 0.f} * 5.f, body_aim), -2, -2, false);
                    }
                } else {
                    lb.mode = limb_mode::dangle;
                }
            } else {
                lb.mode = limb_mode::dangle;
            }
            if (lb.mode == limb_mode::dangle) {
                lb.vel.y -= 0.9f;
                lb.vel += rot(vec2{0.1f * flip + (limb == 0 ? -0.15f : 0.15f), -0.1f}, custom::aim(hips, chunk_pos(k.state, k.c, 0)));
                float const third = leg_length / 3.f;
                if (!custom::dist_less(lb.pos, hips, third)) {
                    vec2 const d = custom::dir_vec(hips, lb.pos) * (third - rw::distance(hips, lb.pos)) / 15.f;
                    lb.vel += d;
                    lb.pos += d;
                }
            }
        }

        // BodyPart.Reset
        template<typename P>
        void reset_part(P& bp, vec2 at, rw::rng& rng) {
            bp.pos = at + custom::rnv(rng);
            bp.last_pos = bp.pos;
            bp.vel = vec2{0.f, 0.f};
        }

        // Scavenger.GraphicsModuleUpdated: the held items to the hands
        void carry_items(sys::game_state& state, dcon::creature_id c, graphics_state const& g, looks const& lk) {
            auto& w = state.world;
            for (uint32_t i = 0; i < 2; ++i) {
                auto const it = scavenger::held_item(state, c, i);
                if (!it)
                    continue;
                auto const k = items::chunk(state, it);
                vec2 const ip = item_position(state, c, g, lk, i);
                auto const t = w.item_get_type(it);
                bool const spear = t == world::item_type::spear || t == world::item_type::explosive_spear || t == world::item_type::electric_spear;
                if (spear || i == 0) {
                    w.body_chunk_set_last_pos(k, w.body_chunk_get_pos(k));
                    w.body_chunk_set_pos(k, ip);
                    w.body_chunk_set_vel(k, vec2{0.f, 0.f});
                } else {
                    vec2& p = w.body_chunk_get_pos(k);
                    vec2 const d = custom::dir_vec(ip, p) * (5.f - rw::distance(ip, p));
                    w.body_chunk_get_vel(k) += d;
                    p += d;
                }
                // Weapon.setRotation
                vec2 const dir = item_direction(state, c, g, lk, i);
                w.item_set_last_rotation(it, w.item_get_rotation(it));
                w.item_set_rotation(it, dir);
                w.item_set_rotation_speed(it, 0.f);
            }
        }
    }

    void roll(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        auto const d = data(state, c);
        auto const& p = w.scavenger_data_get_personality(d);
        looks lk;
        rw::unity_random r{w.scavenger_data_get_seed(d)};
        auto const var = scavenger::variant_of(state, c);
        bool const elite = var == scavenger::variant::elite;
        individual_variations(lk, p, r, elite);
        generate_colors(lk, p, r, elite);
        // (the Templar's TemplarCloak rolls from its own seed)
        // the back cosmetic (the Elite's always HardBackSpikes)
        lk.hard_spikes = r.value() < 0.1f || elite;
        // BackTuftsAndRidges
        lk.scale_graf = uint8_t(r.range(0, 7));
        lk.x_flip = lk.scale_graf == 3 ? 1.f : -1.f;
        if (r.value() < 0.025f)
            lk.x_flip = -lk.x_flip;
        if (r.value() < 0.5f)
            lk.x_flip *= 0.5f + 0.5f * r.value();
        lk.colored = 0.f;
        if (r.value() > lk.general_melanin)
            lk.colored = mathf::pow(r.value(), 0.5f);
        lk.use_detail_color = r.value() < 0.5f;
        if (lk.hard_spikes) {
            // HardBackSpikes
            lk.pattern = r.value() < 0.6f ? scavenger::back_pattern::spine_ridge : scavenger::back_pattern::double_spine_ridge;
            if (r.value() < 0.1f)
                lk.pattern = scavenger::back_pattern::random_back_blotch;
            generate_pattern(lk, r, false);
            if (r.value() < 0.5f)
                lk.scale_graf = uint8_t(r.value() < 0.85f ? r.range(0, 4) : 6);
            // (the sizes' rolls; DrawSprites never reads the sizes)
            (void)r.value();
            (void)r.value();
            lk.general_size = custom::lerp_map(float(lk.decal_count), 5.f, 35.f, 1.f, 0.2f);
            lk.general_size = mathf::lerp(lk.general_size, p.dominance, r.value());
            {
                float const a = mathf::pow(r.value(), 0.75f);
                lk.general_size = mathf::lerp(lk.general_size, a, r.value());
            }
            color_alphas(lk, r);
        } else {
            // WobblyBackTufts
            lk.pattern = scavenger::back_pattern::random_back_blotch;
            if (r.value() < 0.25f && (lk.scruffy == 0.f || r.value() < 0.05f))
                lk.pattern = r.value() < 0.5f ? scavenger::back_pattern::double_spine_ridge : scavenger::back_pattern::spine_ridge;
            if (r.value() < 0.2f) {
                lk.scale_graf = 0;
            } else if (r.value() < 0.5f) {
                lk.scale_graf = uint8_t(r.value() < 1.1764705f ? r.range(3, 6) : 0);
            } else {
                lk.x_flip *= 0.5f + r.value() * 0.5f;
            }
            generate_pattern(lk, r, true);
            lk.out_to_sides = lk.pattern == scavenger::back_pattern::random_back_blotch ? r.value() : 0.f;
            lk.down_along_spine = r.value();
            {
                float const a = r.value();
                lk.general_size = mathf::lerp(a, p.dominance, r.value());
                float const b = r.value();
                lk.general_size = mathf::lerp(lk.general_size, b, r.value());
                lk.general_size = mathf::pow(lk.general_size, mathf::lerp(2.f, 0.65f, p.dominance));
            }
            lk.scale_grav = mathf::lerp(0.f, 0.9f, r.value());
            lk.scale_air_fric = mathf::lerp(0.2f, 0.95f, r.value());
            lk.scale_rigid = mathf::lerp(0.1f, 9.f, mathf::pow(r.value(), 0.2f));
            lk.scale_rigid_grad_rad = mathf::lerp(mathf::max(4.f, lk.scale_rigid * 1.5f), 37.f, mathf::pow(r.value(), 2.f));
            lk.scale_rigid_exp = mathf::lerp(1.f, 6.f, mathf::pow(r.value(), 5.f));
            lk.scale_elastic = 0.5f + r.value() * 0.5f;
            float const num2 = mathf::lerp(0.1f, 0.6f, r.value());
            float const pw = mathf::lerp(1.2f, 0.3f, r.value());
            float const a = num2 * r.value();
            bool const ridge = lk.pattern != scavenger::back_pattern::random_back_blotch;
            for (int32_t i = 0; i < lk.decal_count; ++i) {
                {
                    vec2 const v = custom::rnv(r);
                    lk.random_dirs[i] = v * r.value() * lk.scruffy;
                }
                float const il = mathf::inverse_lerp(lk.top, lk.bottom, lk.positions[i].y);
                float num3;
                if (ridge) {
                    num3 = mathf::lerp(num2, 1.f, std::sin(mathf::pow(il, pw) * mathf::pi));
                } else {
                    num3 = mathf::lerp(num2, 1.f, r.value());
                    num3 = mathf::min(num3, mathf::lerp(a, 1.f, std::sin(mathf::pow(il, pw) * mathf::pi)));
                    lk.random_dirs[i] = custom::rnv(r) * (1.f - 2.f * std::fabs(0.5f - il));
                }
                if (r.value() < lk.scruffy) {
                    float const v = r.value();
                    float const t = mathf::pow(r.value(), mathf::lerp(4.f, 0.5f, lk.scruffy));
                    num3 = mathf::lerp(num3, v, t);
                }
                lk.scale_lengths[i] = 40.f * num3 * mathf::lerp(0.1f, 1.f, lk.general_size);
            }
            color_alphas(lk, r);
        }
        generate_eartlers(lk, r);
        // the teeth
        lk.teeth_count = uint8_t(r.range(2, 5) * 2);
        float const tc = float(lk.teeth_count);
        float num2 = mathf::lerp(0.5f, 1.5f, mathf::pow(r.value(), 1.5f - p.aggression));
        num2 = mathf::lerp(num2, num2 * custom::lerp_map(tc, 4.f, 8.f, 1.f, 0.5f), 0.3f);
        float num3;
        {
            float const inner = mathf::lerp(0.7f, 1.2f, r.value());
            num3 = mathf::lerp(num2 + 0.2f, inner, r.value());
        }
        num3 = mathf::lerp(num3, custom::lerp_map(tc, 4.f, 8.f, 1.5f, 0.2f), 0.4f);
        float const a = 0.3f + 0.7f * r.value();
        for (int32_t l = 0; l < lk.teeth_count; ++l) {
            float const f = float(l) / float(lk.teeth_count - 1);
            lk.teeth[l][0] = mathf::lerp(a, 1.f, std::sin(f * mathf::pi)) * num2;
            if (r.value() < lk.scruffy && r.value() < 0.2f)
                lk.teeth[l][0] = 0.f;
            lk.teeth[l][1] = mathf::lerp(0.5f, 1.f, std::sin(f * mathf::pi)) * num3;
        }
        // the mask (VultureMaskGraphics isn't drawn): the Elite's overrideSprite
        // roll, the Templar's own
        lk.mask = 0;
        if (elite)
            lk.mask = uint8_t(1 + r.range(0, 4));
        if (var == scavenger::variant::disciple)
            lk.mask = 5;
        w.scavenger_data_set_looks(d, lk);
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        auto const d = data(state, c);
        auto& g = w.scavenger_data_get_graphics(d);
        auto const& lk = w.scavenger_data_get_looks(d);
        auto& rng = w.creature_get_graphics_random(c);
        if (rng.state == 0x9E3779B9u || rng.state == 0u)
            rng = rw::rng{uint32_t(w.scavenger_data_get_seed(d)) * 2654435761u | 1u};
        vec2 const head = chunk_pos(state, c, 2), chest = chunk_pos(state, c, 0), hips = chunk_pos(state, c, 1);
        g.draw_positions[0][0] = head;
        g.draw_positions[2][0] = chest;
        g.draw_positions[4][0] = hips;
        g.draw_positions[1][0] = lerp2(head, chest, 0.5f);
        g.draw_positions[3][0] = lerp2(chest, hips, 0.5f);
        for (auto& dp : g.draw_positions)
            dp[1] = dp[0];
        g.spine_lengths[0] = g.spine_lengths[1] = 0.f;
        for (int i = 0; i < 4; ++i)
            g.spine_lengths[0] += rw::distance(g.draw_positions[i][0], g.draw_positions[i + 1][0]);
        g.spine_lengths[1] = g.spine_lengths[0];
        g.flip = g.last_flip = w.creature_get_payload(c).scavenger.flip;
        for (int j = 0; j < 2; ++j) {
            auto& h = g.hands[j];
            h = scavenger::hand{};
            reset_part(h.limb, chest, rng);
            h.limb.mode = limb_mode::dangle;
            h.limb.hunt_speed = hand_lp.hunt_speed;
            h.limb.quickness = hand_lp.quickness;
            h.limb.absolute_hunt_pos = h.limb.pos;
            auto& l = g.legs[j];
            l = player::hand_state{};
            reset_part(l, hips, rng);
            l.mode = limb_mode::hunt_relative_position;
            l.hunt_speed = leg_lp.hunt_speed;
            l.quickness = leg_lp.quickness;
            l.absolute_hunt_pos = l.pos;
        }
        for (int i = 0; i < 4; ++i) {
            g.tail[i].rad = mathf::lerp(2.f, 1.f, lk.tail_segs > 1 ? float(i) / float(lk.tail_segs - 1) : 0.f);
            reset_part(g.tail[i], hips, rng);
            g.tail[i].stretched = 1.f;
        }
        g.look_point = g.last_look_point = scavenger::look_point(state, c);
        g.body_axis = g.last_body_axis = vec2{0.f, 1.f};
        // WobblyBackTufts.Reset
        spine const sp{g, lk, 1.f};
        for (int32_t i = 0; i < lk.decal_count; ++i) {
            g.scales[i].pos = sp.back_surface(lk.positions[i]);
            g.scales[i].last_pos = g.scales[i].pos;
            g.scales[i].vel = vec2{0.f, 0.f};
        }
        g.ready = 1;
    }

    void shock_reaction(sys::game_state& state, dcon::creature_id c, float intensity) {
        auto& w = state.world;
        if (w.creature_get_dead(c) || w.creature_get_stun(c) > 20)
            return;
        auto const d = data(state, c);
        auto& g = w.scavenger_data_get_graphics(d);
        auto const& p = w.scavenger_data_get_personality(d);
        intensity = mathf::clamp01(intensity);
        g.eyes_pop = mathf::max(g.eyes_pop, mathf::pow(intensity, 0.5f));
        g.eyes_pop_go_back = mathf::max(g.eyes_pop_go_back, mathf::lerp(2.f, 20.f, mathf::pow(intensity, mathf::lerp(0.8f, 2.f, p.bravery))));
        g.shake = mathf::max(g.shake, mathf::max(0.f, intensity - custom::lerp_map(p.bravery, 0.75f, 1.f, 0.f, 0.5f)));
        g.blink = w.creature_get_graphics_random(c).range(10, 40);
        g.bristle = std::max({g.bristle, w.scavenger_data_get_ai(d).agitation, intensity});
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto const d = data(state, c);
        auto& g = w.scavenger_data_get_graphics(d);
        if (!g.ready)
            reset_positions(state, c);
        auto const& lk = w.scavenger_data_get_looks(d);
        auto const& x = pl(state, c);
        auto const& b = w.scavenger_data_get_body(d);
        auto const& a = w.scavenger_data_get_ai(d);
        auto const& p = w.scavenger_data_get_personality(d);
        auto& rng = w.creature_get_graphics_random(c);
        ctx k{state, c, room, g, lk, x, b, a, rng};
        bool const awake = scavenger::consious(state, c);
        bool const dead = w.creature_get_dead(c) != 0;
        vec2 const main = chunk_pos(state, c, 0), hips = chunk_pos(state, c, 1);
        // the sub modules first (ComplexGraphicsModule.Update): the back
        // scales (WobblyBackTufts.Update)
        if (!lk.hard_spikes) {
            spine const sp{g, lk, 1.f};
            float const deg = custom::aim(g.draw_positions[hips_draw_pos][1], g.draw_positions[chest_draw_pos][1]);
            for (int32_t i = 0; i < lk.decal_count; ++i) {
                auto& s = g.scales[i];
                vec2 const ps = lk.positions[i];
                vec2 const at = sp.back_surface(ps);
                {
                    vec2 const r = custom::rnv(rng);
                    s.vel += r * (rng.value() * (g.shake + g.bristle * 1.5f));
                }
                s.vel.y -= (lk.scale_grav * 0.5f + 0.2f * rng.value()) * g.bristle;
                vec2 const ideal = at
                    + rw::normalized(sp.up(ps.y) + sp.dir(ps.y) * (0.5f * ps.y * lk.down_along_spine * (1.f - g.bristle))
                                     + sp.outwards(ps) * (mathf::pow(std::fabs(ps.x), 0.4f) * lk.out_to_sides) + rot(lk.random_dirs[i], deg))
                        * (lk.scale_lengths[i] * (1.f + 0.5f * g.bristle) + 5.f * g.bristle);
                // Scale.Update
                s.last_pos = s.pos;
                s.pos += s.vel;
                s.vel *= lk.scale_air_fric;
                s.vel.y -= lk.scale_grav;
                s.vel += custom::dir_vec(s.pos, ideal) * mathf::pow(mathf::inverse_lerp(0.f, lk.scale_rigid_grad_rad, rw::distance(s.pos, ideal)), lk.scale_rigid_exp) * lk.scale_rigid;
                s.pos += custom::dir_vec(at, s.pos) * (lk.scale_lengths[i] - rw::distance(at, s.pos)) * (1.f - lk.scale_elastic);
                if (!custom::dist_less(s.pos, ideal, lk.scale_lengths[i])) {
                    vec2 const dd = custom::dir_vec(s.pos, ideal) * (rw::distance(s.pos, ideal) - lk.scale_lengths[i]);
                    s.vel += dd;
                    s.pos += dd;
                }
            }
        }
        // riseBody
        if (b.has_knuckle_pos && x.mov_mode == move_mode::run) {
            float const num = custom::lerp_map(std::fabs(main.x - b.knuckle_pos.x), 20.f, 0.f, 0.f, 10.f);
            if (g.rise_body < num)
                g.rise_body = mathf::min(num, g.rise_body + 2.f);
            else
                g.rise_body = mathf::lerp(g.rise_body, num, 0.2f);
        } else {
            g.rise_body = mathf::max(0.f, g.rise_body - 0.5f);
        }
        // the tail
        if (lk.tail_segs > 0) {
            float const sub = physics::submersion(state, room, scavenger::chunk(state, c, 1));
            vec2 prev = g.draw_positions[hips_draw_pos][0];
            vec2 prev2 = g.draw_positions[hips_draw_pos - 1][0];
            float num2 = 28.f;
            for (int i = 0; i < lk.tail_segs; ++i) {
                auto& t = g.tail[i];
                // TailSegment.Update (10 apart; the first hangs from the hips)
                t.last_pos = t.pos;
                t.pos += t.vel;
                t.vel *= 0.9f;
                t.stretched = 1.f;
                vec2 const to = i == 0 ? g.draw_positions[hips_draw_pos][0] : g.tail[i - 1].pos;
                if (!custom::dist_less(t.pos, to, 10.f)) {
                    float const num = rw::distance(t.pos, to);
                    vec2 const dv = custom::dir_vec(t.pos, to);
                    float const affect = i == 0 ? 0.f : 0.5f;
                    t.pos -= dv * (10.f - num) * (1.f - affect);
                    t.vel -= dv * (10.f - num) * (1.f - affect);
                    if (i > 0) {
                        g.tail[i - 1].pos += dv * (10.f - num) * affect;
                        g.tail[i - 1].vel += dv * (10.f - num) * affect;
                    }
                    t.stretched = mathf::clamp((10.f / (num * 0.5f) + 2.f) / 3.f, 0.2f, 1.f);
                }
                body_parts::push_out_of_terrain(t, room, 0.5f);
                t.vel *= mathf::lerp(1.f, 0.5f, sub);
                t.vel.y -= 0.9f * (1.f - sub) * room.gravity;
                if (!custom::dist_less(t.pos, hips, 9.f * float(i + 1)))
                    t.pos = hips + custom::dir_vec(hips, t.pos) * 9.f * float(i + 1);
                t.vel += custom::dir_vec(prev2, t.pos) * num2 / mathf::max(0.01f, rw::distance(prev2, t.pos));
                num2 *= 0.25f;
                prev2 = prev;
                prev = t.pos;
            }
        }
        for (int j = 0; j < 2; ++j)
            hand_update(k, j);
        for (int j = 0; j < 2; ++j)
            leg_update(k, j);
        g.last_flip = g.flip;
        g.flip = x.flip;
        g.last_cycle = g.cycle;
        if (x.moving)
            g.cycle += mathf::sign(g.flip) * 0.1f;
        g.last_swim_arms = g.swim_arms;
        if (x.mov_mode == move_mode::swim)
            g.swim_arms = mathf::min(g.swim_arms + 0.1f, 1.f);
        else
            g.swim_arms = mathf::max(g.swim_arms - 0.1f, 0.f);
        g.last_body_axis = g.body_axis;
        {
            vec2 const ch = g.draw_positions[chest_draw_pos][0], hp = g.draw_positions[hips_draw_pos][0];
            g.body_axis = rw::slerp(g.body_axis, rw::normalized(ch - hp + custom::perpendicular_vector(ch, hp) * 10.f * g.flip), 0.1f);
            if (x.mov_mode != move_mode::crawl && x.mov_mode != move_mode::climb)
                g.body_axis = rw::slerp(g.body_axis, vec2{0.f, 1.f}, 0.5f);
        }
        g.last_look_up = g.look_up;
        if (rot(head_dir(state, c, g, 1.f), body_axis(g, 1.f)).y > 0.8f)
            g.look_up = mathf::min(1.f, g.look_up + 1.f / mathf::lerp(5.f, 20.f, g.shifting_neutral_face));
        else
            g.look_up = mathf::max(0.f, g.look_up - 1.f / mathf::lerp(20.f, 5.f, g.shifting_neutral_face));
        g.last_look_point = g.look_point;
        g.look_point = scavenger::look_point(state, c);
        g.last_eyes_open = g.eyes_open;
        if (awake) {
            if (--g.blink < 0) {
                g.eyes_open = mathf::max(0.f, g.eyes_open - 0.5f);
                if (g.blink < -rng.range(2, 6))
                    g.blink = rng.range(4, rng.value() < 0.5f ? 12 : int32_t(mathf::lerp(800.f, 200.f, p.nervous)));
            } else {
                g.eyes_open = mathf::min(1.f, g.eyes_open + 0.05f + 0.95f * p.energy);
            }
            g.last_neutral_face = g.neutral_face;
            float const num3 = mathf::lerp(200.f, 600.f, mathf::pow(g.shifting_neutral_face, 1.7f));
            float const num4 = mathf::inverse_lerp(num3, 0.f, rw::distance(main, g.look_point));
            g.neutral_face = mathf::lerp(g.shifting_neutral_face, num4, custom::lerp_map(std::fabs(num4 - num3 * 0.5f), 0.f, num3 * 0.5f, 0.05f, 0.55f));
            if (g.shifting_neutral_face < g.shifting_neutral_face_goal)
                g.shifting_neutral_face = mathf::min(g.shifting_neutral_face + 0.025f, g.shifting_neutral_face_goal);
            else
                g.shifting_neutral_face = mathf::max(g.shifting_neutral_face - 0.025f, g.shifting_neutral_face_goal);
            g.shifting_neutral_face = mathf::lerp(g.shifting_neutral_face, g.shifting_neutral_face_goal, 0.15f);
            if (rng.value() < 0.0125f)
                g.shifting_neutral_face_goal = rng.value();
        } else {
            g.eyes_open = mathf::lerp(g.eyes_open, 0.5f, 0.1f);
        }
        g.last_eyes_pop = g.eyes_pop;
        if (g.eyes_pop > 0.f) {
            g.eyes_pop = mathf::max(0.f, g.eyes_pop - 1.f / mathf::max(1.f, g.eyes_pop_go_back));
            g.eyes_open = 1.f;
        }
        if (a.scared > 0.8f && p.bravery < 0.3f)
            g.eyes_pop = mathf::lerp(g.eyes_pop, rng.value() * mathf::inverse_lerp(0.8f, 1.f, a.scared), 0.1f);
        g.last_shake = g.shake;
        if (g.shake > 0.f)
            g.shake = mathf::max(mathf::inverse_lerp(0.6f, 1.f, a.scared * (1.f - p.bravery)) * 0.3f, g.shake - 0.1f);
        g.last_bristle = g.bristle;
        if (g.bristle > 0.f)
            g.bristle = mathf::max(0.f, g.bristle - (1.f - a.agitation) / 30.f);
        if (dead) {
            g.eyes_pop = 0.f;
            g.bristle = 0.f;
            g.shake = 0.f;
        }
        // the draw positions
        for (auto& dp : g.draw_positions)
            dp[1] = dp[0];
        g.draw_positions[0][0] = chunk_pos(state, c, 2);
        g.draw_positions[2][0] = main;
        g.draw_positions[4][0] = hips;
        if (awake) {
            vec2 const f3 = custom::dir_vec(g.look_point, g.draw_positions[2][0]) * custom::lerp_map(rw::distance(g.look_point, g.draw_positions[2][0]), 50.f, 400.f, 11.f, 0.f, 0.5f);
            g.draw_positions[2][0] += vec2{f3.x, f3.y + g.rise_body};
        }
        if (x.mov_mode == move_mode::climb && !b.has_swing_pos) {
            float const rhs = rw::distance(g.draw_positions[4][0], g.draw_positions[2][0]);
            vec2 const lhs = g.draw_positions[2][0];
            for (int m = 0; m < 2; ++m)
                g.draw_positions[2][0] += custom::dir_vec(main, g.hands[m].limb.pos) * custom::lerp_map(rw::distance(main, g.hands[m].limb.pos), 5.f, 40.f, 5.f, -7.5f);
            g.draw_positions[2][0] = lerp2(g.draw_positions[2][0], g.draw_positions[4][0] + custom::dir_vec(g.draw_positions[4][0], g.draw_positions[2][0]) * rhs, 0.5f);
            g.draw_positions[1][0] += lhs - g.draw_positions[2][0];
        }
        if (awake && p.nervous > 0.8f && rng.value() < 0.5f + 0.5f * p.nervous) {
            vec2 const r = custom::rnv(rng);
            g.draw_positions[2][0] += r * mathf::inverse_lerp(0.8f, 1.f, p.nervous) * 1.5f * rng.value();
        }
        g.draw_positions[3][0] = (g.draw_positions[2][0] + g.draw_positions[4][0]) / 2.f;
        g.draw_positions[3][0] -= custom::perpendicular_vector(g.draw_positions[4][0], g.draw_positions[2][0]) * mathf::lerp(5.f, 15.f, lk.narrow_waist) * x.flip;
        g.draw_positions[3][0] = lerp2(g.draw_positions[3][0], g.draw_positions[4][0], 0.25f);
        g.draw_positions[1][0] = main + custom::dir_vec(g.draw_positions[3][0], g.draw_positions[chest_draw_pos][0]) * 8.f;
        for (int n = 0; n < 2; ++n)
            g.draw_positions[4][0] += custom::dir_vec(g.draw_positions[4][0], g.legs[n].pos);
        if (g.shake > 0.f) {
            float const sp = custom::lerp_map(rw::distance(chunk_last_pos(state, c, 0), main), 2.f, 8.f, 1.f, 4.f);
            for (auto& dp : g.draw_positions) {
                vec2 const r = custom::rnv(rng);
                dp[0] += r * rng.value() * g.shake * 7.f * sp;
            }
        }
        g.spine_lengths[1] = g.spine_lengths[0];
        g.spine_lengths[0] = 0.f;
        for (int i = 0; i < 4; ++i)
            g.spine_lengths[0] = mathf::clamp(g.spine_lengths[0] + rw::distance(g.draw_positions[i][0], g.draw_positions[i + 1][0]), 0.f, 200.f);
        // Scavenger.GraphicsModuleUpdated
        carry_items(state, c, g, lk);
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip) {
        auto const& w = state.world;
        auto const d = data(state, c);
        auto const& g = w.scavenger_data_get_graphics(d);
        if (!g.ready)
            return;
        auto const& lk = w.scavenger_data_get_looks(d);
        auto const& x = pl(state, c);
        auto const& b = w.scavenger_data_get_body(d);
        float const t = alpha;
        spine const sp{g, lk, t};

        // ApplyPalette (the darkness isn't lowered by light sources)
        float const darkness = pal.darkness();
        vec3 const black = pal.black();
        vec3 const body = mix(rgb(lk.body_color), black, mathf::lerp(lk.body_color_black, 1.f, darkness));
        vec3 const headc = mix(rgb(lk.head_color), black, mathf::lerp(lk.head_color_black, 1.f, darkness));
        vec3 const deco = mix(rgb(lk.decoration_color), black, darkness * 0.9f);
        // the eartlers' tips: the Templar's RainWorld.GoldRGB
        vec3 const eartler_tips = scavenger::variant_of(state, c) == scavenger::variant::disciple ? vec3{0.529f, 0.365f, 0.184f} : deco;
        vec3 const eye = darkness < 0.5f ? rgb(lk.eye_color) : saturate(rgb(lk.eye_color), mathf::inverse_lerp(0.5f, 1.f, darkness));
        vec3 const belly = mix(rgb(lk.belly_color), black, mathf::lerp(lk.belly_color_black, 1.f, darkness));
        vec3 eye_white = eye, pupil = headc;
        if (lk.pupil_size > 0.f) {
            if (lk.deep_pupils) {
                if (lk.head_color.lightness * (1.f - lk.head_color_black) > 0.1f) {
                    pupil = mix(eye, mix(body, headc, 0.5f), 0.5f);
                    eye_white = mix(eye, vec3{1.f, 1.f, 1.f}, 0.5f);
                } else {
                    eye_white = mix(eye, mix(body, headc, 0.5f), 0.5f);
                    pupil = mix(eye, vec3{1.f, 1.f, 1.f}, 0.5f);
                }
            } else {
                switch (lk.general_melanin < 0.5f ? 0 : lk.colored_pupils) {
                case 1: pupil = custom::hsl2rgb(lk.body_color.hue, 1.f, 0.35f); break;
                case 2: pupil = custom::hsl2rgb(lk.head_color.hue, 1.f, 0.35f); break;
                case 3: pupil = custom::hsl2rgb(lk.decoration_color.hue, 1.f, 0.35f); break;
                default:
                    if (lk.head_color.lightness * (1.f - lk.head_color_black) > 0.1f)
                        pupil = mix(custom::hsl2rgb(lk.head_color.hue, lk.head_color.saturation, 0.15f), black, lk.head_color_black);
                    else
                        pupil = headc;
                    break;
                }
            }
        }

        float const flip = mathf::lerp(g.last_flip, g.flip, t);
        vec2 const head = sp.dp(head_draw_pos), chest = sp.dp(chest_draw_pos), hips = sp.dp(hips_draw_pos);
        vec2 f5 = (chest + hips) / 2.f;
        f5 -= custom::perpendicular_vector(rw::normalized(hips - chest)) * mathf::lerp(5.f, 15.f, lk.narrow_waist) * flip;
        float const main_rad = chunk_rad(state, c, 0), hip_rad = chunk_rad(state, c, 1);

        auto const tri = [&](vec2 a, vec2 bb, vec2 cc, vec4 ca, vec4 cb, vec4 cc2) {
            sprites::draw_cmd cmd;
            cmd.is_triangle = true;
            cmd.vertex_colors = true;
            cmd.tri[0] = a;
            cmd.tri[1] = bb;
            cmd.tri[2] = cc;
            cmd.vcol[0] = ca;
            cmd.vcol[1] = cb;
            cmd.vcol[2] = cc2;
            out.push_back(cmd);
        };

        // ScavengerLeg.DrawSprites (the leg's TriangleMesh)
        auto const draw_leg = [&](int limb) {
            float const num = custom::lerp_map(flip, 0.f - float(limb), 1.f - float(limb), -1.f, 1.f);
            float const n2 = mathf::lerp(1.f, 1.4f, lk.legs_size);
            vec2 const f = hips;
            vec2 f2 = lerp2(g.legs[limb].last_pos, g.legs[limb].pos, t);
            f2 -= custom::perpendicular_vector(rw::normalized(f2 - f)) * n2 * custom::lerp_map(rw::distance(f, f2) / n2, 2.f, 20.f, 6.f, -1.f) * num;
            if (custom::dist_less(f2, f, 3.f))
                f2 = f + custom::dir_vec(f, f2) * 3.f;
            vec2 const f3 = f2 + custom::dir_vec(f2, f) * 1.5f * n2;
            vec2 f4 = custom::inverse_kinematic(f, f3, 10.f * n2, 10.f * n2, -num);
            if (custom::dist_less(f4, (f + f3) / 2.f, 3.f * n2))
                f4 = (f + f3) / 2.f + custom::dir_vec((f + f3) / 2.f, f4) * 3.f * n2;
            f2 += custom::dir_vec(f, f4) * 3.f * n2;
            f2 += custom::dir_vec(f3, f4) * 2.f * n2;
            vec2 const lhs = lerp2((f + f4 + f3) / 3.f, (f + f3) / 2.f, 0.35f);
            f4 += custom::perpendicular_vector(rw::normalized(f3 - f)) * n2 * custom::lerp_map(std::fabs(custom::distance_to_line(f4, f, f3)), 0.f, 20.f, 2.f, 0.f) * mathf::sign(num);
            float const s = -mathf::sign(num);
            vec2 v[12];
            v[0] = f - custom::perpendicular_vector(rw::normalized(f - f4)) * 2.f * n2 * s;
            v[1] = f + custom::perpendicular_vector(rw::normalized(f - f4)) * 2.f * n2 * s;
            v[2] = lhs;
            v[3] = f4 - rw::normalized(f3 - f) * 3.f * n2;
            v[4] = f4 + rw::normalized(f3 - f) * 3.f * n2;
            v[5] = f3 - rw::normalized(rw::normalized(f3 - f4) + rw::normalized(f - f4)) * custom::lerp_map(rw::distance(f, f3), 10.f * n2, 30.f * n2, 3.f, 1.f) * n2;
            v[6] = f3 + rw::normalized(f - f2) * 0.8f * n2;
            v[7] = f3 - rw::normalized(f - f2) * 0.8f * n2;
            v[8] = f2 + custom::perpendicular_vector(rw::normalized(f3 - f2)) * 0.8f * n2 * s;
            v[9] = f2 - custom::perpendicular_vector(rw::normalized(f3 - f2)) * 0.8f * n2 * s;
            v[10] = f2 + custom::perpendicular_vector(rw::normalized(f3 - f2)) * 0.7f * n2 * s + custom::dir_vec(f3, f2) * n2 * 2.f;
            v[11] = f2 - custom::perpendicular_vector(rw::normalized(f3 - f2)) * 1.2f * n2 * s + custom::dir_vec(f3, f2) * n2 * 2.f;
            static constexpr int tris[10][3] = {{0, 1, 2}, {1, 2, 3}, {3, 4, 2}, {4, 2, 5}, {2, 5, 6}, {5, 6, 7}, {5, 7, 8}, {8, 7, 9}, {8, 9, 10}, {8, 9, 11}};
            vec4 const cb = rgba(body);
            for (auto const& q : tris)
                tri(v[q[0]], v[q[1]], v[q[2]], cb, cb, cb);
        };

        // ScavengerHand.DrawSprites (the arm's TriangleMesh and the hand)
        auto const draw_hand = [&](int limb) {
            auto const& h = g.hands[limb];
            vec2 f = chest;
            f += custom::perpendicular_vector(rw::normalized(f - hips)) * (1.f - std::fabs(flip)) * 10.f * (limb == 0 ? -1.f : 1.f);
            f += custom::dir_vec(hips, chest) * 5.f;
            bool closed = h.limb.reached_snap_position != 0;
            if (limb == 0 && creature::grasped_object(w, c, 0))
                closed = true;
            vec2 f2 = lerp2(h.limb.last_pos, h.limb.pos, t) + lerp2(h.last_spear_pos_add, h.spear_pos_add, t);
            if (b.has_swing_pos) {
                if (limb == b.swing_arm) {
                    f2 = b.swing_pos;
                    closed = true;
                } else if (b.has_next_swing_pos) {
                    closed = custom::dist_less(f2, b.next_swing_pos, 5.f);
                }
            }
            vec2 f3 = f2 + custom::dir_vec(f2, f) * 6.f;
            if (custom::dist_less(f3, f, 5.f)) {
                f2 += custom::dir_vec(f, f3) * (5.f - rw::distance(f, f3));
                f3 += custom::dir_vec(f, f3) * (5.f - rw::distance(f, f3));
            }
            float const mf = my_flip(g, limb, t);
            vec2 const f4 = custom::inverse_kinematic(f, f3, 18.f, 18.f, mf);
            vec2 f5 = chest;
            vec2 f6 = custom::perpendicular_vector(rw::normalized(f - f5));
            float const num = -(1.75f + mathf::lerp(-1.f, 1.f, lk.arm_thickness)) * mathf::sign(mf);
            vec2 v[19];
            for (int i = 0; i < 4; ++i) {
                vec2 const f7 = i == 0 ? f : i == 1 ? f4 : i == 2 ? f3 : f2;
                vec2 const f8 = -rw::normalized(f5 - f7);
                vec2 const f9 = custom::perpendicular_vector(f8);
                float const rhs = rw::distance(f7, f5);
                switch (i) {
                case 0:
                    v[0] = f5;
                    v[18] = f7 - f6 * num * 2.f - f8 * rhs * 0.2f;
                    v[1] = lerp2(f7 + rw::normalized(f9 + f6) * num * 2.f - f8, f5, 0.5f);
                    break;
                case 1:
                    v[17] = f5 - rw::normalized(f9 * 2.f + f6) * num * 2.f;
                    v[16] = f7 - f9 * num - f8 * rhs * 0.8f;
                    v[15] = f7 - f9 * num * 2.5f - f8 * rhs * 0.5f;
                    v[2] = f7 + f9 * num * 2.5f - f8 * rhs * 0.5f;
                    v[14] = f7 - f9 * num - f8 * rhs * 0.2f;
                    v[3] = f7 + f9 * num * 1.6f - f8 * rhs * 0.2f;
                    break;
                case 2:
                    v[13] = f5 - rw::normalized(f9 + f6) * num * 2.f;
                    v[4] = f5 + rw::slerp(f9, f6, 0.7f) * num * 2.f;
                    v[12] = f7 - f9 * num * 1.3f - f8 * rhs * 0.8f;
                    v[5] = f5 + rw::slerp(f9, f6, 0.3f) * num * 2.f;
                    v[11] = f7 - f9 * num * 1.5f - f8 * rhs * 0.65f;
                    v[6] = f7 + f9 * num * 1.5f - f8 * rhs * 0.65f;
                    break;
                default:
                    v[10] = f5 - rw::normalized(f9 + f6) * num;
                    v[7] = f5 + rw::normalized(f9 + f6) * num;
                    v[9] = f7 - f9 * num;
                    v[8] = f7 + f9 * num;
                    break;
                }
                f5 = f7;
                f6 = f9;
            }
            vec4 col[19];
            for (auto& cc : col)
                cc = rgba(body);
            if (lk.hands_head_color > 0.f) {
                for (int n = 7; n < 11; ++n)
                    col[n] = rgba(mix(body, headc, lk.hands_head_color));
                col[6] = col[11] = rgba(mix(body, headc, 0.5f * lk.hands_head_color));
                col[5] = col[12] = rgba(mix(body, headc, 0.2f * lk.hands_head_color));
            }
            static constexpr int tris[17][3] = {{0, 18, 1}, {18, 17, 1}, {16, 17, 1}, {1, 15, 16}, {1, 2, 15}, {14, 2, 15}, {3, 2, 14}, {14, 3, 13}, {4, 3, 13},
                {12, 4, 13}, {4, 12, 5}, {11, 12, 5}, {11, 6, 5}, {11, 6, 10}, {7, 6, 10}, {7, 9, 10}, {7, 9, 8}};
            for (auto const& q : tris)
                tri(v[q[0]], v[q[1]], v[q[2]], col[q[0]], col[q[1]], col[q[2]]);
            sprite s;
            s.element = closed ? "ScavengerHandB" : "ScavengerHandA";
            s.pos = f3;
            s.rotation = custom::aim(f3, f2);
            s.scale = vec2{-(num * 2.f) * 3.f / 18.f, 1.f};
            s.color = lk.hands_head_color > 0.f ? mix(body, headc, lk.hands_head_color) : body;
            sprites::add(out, s);
        };

        // the behind limbs
        draw_leg(1);
        draw_hand(1);

        // the back cosmetics: the tail
        if (lk.tail_segs > 0) {
            vec2 v[16];
            int n = 0;
            vec2 prev = sp.dp(3);
            float dd = 3.f;
            for (int i = 0; i < lk.tail_segs; ++i) {
                vec2 const p2 = lerp2(g.tail[i].last_pos, g.tail[i].pos, t);
                vec2 const nn = rw::normalized(p2 - prev);
                vec2 const a = custom::perpendicular_vector(nn);
                float const d2 = i == 0 ? 0.f : rw::distance(p2, prev) / 5.f;
                float const sr = g.tail[i].rad * g.tail[i].stretched;
                v[n++] = prev - a * dd + nn * d2;
                v[n++] = prev + a * dd + nn * d2;
                if (i < lk.tail_segs - 1) {
                    v[n++] = p2 - a * sr - nn * d2;
                    v[n++] = p2 + a * sr - nn * d2;
                } else {
                    v[n++] = p2;
                }
                dd = sr;
                prev = p2;
            }
            sprites::long_mesh(out, strip, v, size_t(n), body);
        }
        // the back decals (BackTuftsAndRidges' LizardScales)
        {
            int const graf = custom::int_clamp(lk.scale_graf, 0, 6);
            float const height = scale_heights[graf];
            vec3 const col = lk.use_detail_color ? deco : headc;
            bool const colored = lk.colored > 0.f;
            for (int32_t i = 0; i < lk.decal_count; ++i) {
                vec2 const ps = lk.positions[i];
                vec2 const at = sp.back_surface(ps);
                float rotation, sx, sy;
                if (lk.hard_spikes) {
                    // HardBackSpikes.DrawSprites (its bristle shiver aside)
                    vec2 f2 = sp.up(ps.y) * mathf::lerp(4.f, 14.f, lk.general_size) * (1.f - 0.5f * mathf::inverse_lerp(0.5f, 1.f, ps.y))
                        + sp.dir(ps.y) * mathf::lerp(4.f, 18.f, lk.general_size) * 0.5f * mathf::inverse_lerp(0.5f, 1.f, ps.y);
                    f2 *= 1.f + mathf::lerp(g.last_bristle, g.bristle, t) * 0.5f;
                    rotation = custom::vec_to_deg(rw::normalized(f2));
                    sx = mathf::sign(flip + ps.x * 0.5f) * mathf::lerp(0.5f, 1.f, lk.general_size) * lk.x_flip;
                    sy = rw::magnitude(f2) / height;
                } else {
                    // WobblyBackTufts.DrawSprites
                    vec2 const tip = lerp2(g.scales[i].last_pos, g.scales[i].pos, t);
                    rotation = custom::aim(at, tip);
                    sx = mathf::sign(flip + ps.x * 0.5f) * mathf::lerp(0.5f, 1.f, lk.general_size) * lk.x_flip;
                    sy = rw::distance(at, tip) / height;
                }
                sprite s;
                s.element = scale_a[graf];
                s.pos = at;
                s.rotation = rotation;
                s.scale = vec2{sx, sy};
                s.anchor = vec2{0.5f, 0.1f};
                s.color = body;
                sprites::add(out, s);
                if (colored) {
                    s.element = scale_b[graf];
                    s.color = col;
                    vec4 const cc = rgba(col, lk.color_alphas[i]);
                    vec4 const corners[4] = {cc, cc, cc, cc};
                    sprites::add(out, s, corners);
                }
            }
        }
        // ChestSprite, HipSprite
        {
            sprite s;
            s.element = "Circle20";
            s.pos = chest;
            s.rotation = custom::aim(chest, f5);
            s.scale = vec2{main_rad * mathf::lerp(0.7f, 1.3f, lk.fatness) / 10.f, (main_rad + mathf::lerp(2.f, 1.5f, lk.narrow_waist)) / 10.f};
            s.anchor = vec2{0.5f, 0.4f + 0.05f * lk.narrow_waist};
            s.color = body;
            sprites::add(out, s);
            sprite h;
            h.element = "Circle20";
            h.pos = hips;
            h.scale = vec2{hip_rad / 15.f, hip_rad / 15.f};
            h.color = body;
            sprites::add(out, h);
        }
        // WaistSprite
        {
            vec2 const f6 = custom::dir_vec(hips, chest);
            float const ww = lk.waist_width();
            vec2 const mid = lerp2(f5, (chest + hips) / 2.f, 0.4f);
            vec2 const hp = custom::perpendicular_vector(rw::normalized(hips - f5));
            vec2 const np = custom::perpendicular_vector(-f6);
            vec2 const cp = custom::perpendicular_vector(rw::normalized(f5 - chest));
            vec2 v[8];
            v[0] = hips + hp * hip_rad * mathf::lerp(0.65f, 0.9f, ww);
            v[1] = hips - hp * hip_rad * mathf::lerp(0.65f, 0.9f, ww);
            v[2] = mid - f6 * 4.f + np * main_rad * mathf::lerp(0.35f, 0.9f, mathf::pow(ww, 1.3f));
            v[3] = mid - f6 * 4.f - np * main_rad * mathf::lerp(0.35f, 0.9f, mathf::pow(ww, 1.3f));
            v[4] = mid + f6 * 4.f + np * main_rad * mathf::lerp(0.25f, 0.8f, mathf::pow(ww, 1.3f));
            v[5] = mid + f6 * 4.f - np * main_rad * mathf::lerp(0.25f, 0.8f, mathf::pow(ww, 1.3f));
            v[6] = chest + cp * main_rad * mathf::lerp(0.7f, 1.3f, lk.fatness);
            v[7] = chest - cp * main_rad * mathf::lerp(0.7f, 1.3f, lk.fatness);
            sprites::long_mesh(out, strip, v, 8, body);
        }
        // ChestPatchSprite: chestPatchShape on the belly surface
        {
            constexpr float n5 = 0.4f, n6 = 0.95f, n7 = 1.f;
            vec2 const shape[8] = {{-n7 / 4.f, n5}, {n7 / 4.f, n5}, {-n7, mathf::lerp(n5, n6, 0.33333334f)}, {n7, mathf::lerp(n5, n6, 0.33333334f)},
                {-n7, mathf::lerp(n5, n6, 0.6666667f)}, {n7, mathf::lerp(n5, n6, 0.6666667f)}, {-n7 / 3.f, n6}, {n7 / 3.f, n6}};
            vec2 v[8];
            for (int n = 0; n < 8; ++n)
                v[n] = sp.belly_surface(shape[n]);
            sprites::long_mesh(out, strip, v, 8, belly);
        }
        // the front limbs
        draw_leg(0);
        draw_hand(0);
        // NeckSprite
        {
            vec2 const f7 = chest + custom::dir_vec(f5, chest) * 5.f;
            vec2 f8 = f7;
            float n3 = main_rad;
            float const rhs = mathf::inverse_lerp(0.f, 10.f, custom::distance_to_line(head, hips, chest) * -flip);
            vec2 v[16];
            vec4 col[16];
            for (int i = 0; i < 4; ++i) {
                float const n4 = float(i) / 3.f;
                vec2 f9 = lerp2(f7, head, n4);
                f9 += custom::perpendicular_vector(rw::normalized(f7 - head)) * std::sin(n4 * mathf::pi) * 3.f * rhs * flip;
                vec2 const f10 = rw::normalized(f8 - f9);
                vec2 const l = custom::perpendicular_vector(f10);
                float const r2 = rw::distance(f9, f8) / 4.f;
                float const n5 = mathf::lerp(7.f, 3.f, n4) - 2.f * std::sin(n4 * mathf::pi) * mathf::lerp(0.5f, 1.5f, lk.neck_thickness);
                v[i * 4] = f8 - l * (n3 + n5) * 0.5f - f10 * r2;
                v[i * 4 + 1] = f8 + l * (n3 + n5) * 0.5f - f10 * r2;
                v[i * 4 + 2] = f9 - l * n5 + f10 * r2;
                v[i * 4 + 3] = f9 + l * n5 + f10 * r2;
                col[i * 4] = col[i * 4 + 1] = rgba(mix(body, headc, (float(i) - 0.5f) / 3.f));
                col[i * 4 + 2] = col[i * 4 + 3] = rgba(mix(body, headc, float(i) / 3.f));
                f8 = f9;
                n3 = n5;
            }
            sprites::long_mesh(out, strip, v, col, 16);
        }
        // the face: HeadDir, BodyAxis, lookUp, neutralFace
        vec2 f11 = head_dir(state, c, g, t);
        float const n6 = body_axis(g, t);
        float n7 = mathf::lerp(g.last_look_up, g.look_up, t);
        float const n8 = mathf::lerp(g.last_neutral_face, g.neutral_face, t);
        n7 *= 1.f - n8;
        vec2 f12 = rw::normalized(lerp2(rw::normalized(f11), -custom::deg_to_vec(-n6), mathf::lerp(0.5f, 1.f, mathf::max(mathf::pow(n7, 1.1f), n8))));
        vec2 const f13 = rot(rw::normalized(f12), n6);
        f11 = rw::normalized(f11);
        f12 = rw::normalized(f12);
        float const n9 = mathf::lerp(0.6f, 1.1f, lk.head_size);
        // the Eartlers
        {
            vec2 const hd = rw::slerp(f12, -f11, n7);
            float const rotat = custom::vec_to_deg(-hd);
            float const squeeze = 1.f - mathf::pow(std::fabs(rot(hd, n6).x), 1.5f) * mathf::lerp(1.f, 0.5f, n7);
            float const ysq = mathf::lerp(1.f, -0.25f, n7);
            float const size = mathf::lerp(15.f, 35.f, w.scavenger_data_get_personality(d).dominance);
            auto const vpos = [&](int br, int vt) {
                vec2 const p = lk.branch_pos[br][vt];
                return head + rot(vec2{p.x * squeeze, p.y * ysq}, rotat) * size;
            };
            for (int i = 0; i < lk.branch_count; ++i) {
                int const len = lk.branch_length[i];
                vec2 v[16];
                vec4 col[16];
                vec2 p = vpos(i, 0);
                vec2 a = custom::perpendicular_vector(lk.branch_pos[i][1], lk.branch_pos[i][0]);
                float n3 = lk.branch_rad[i][0];
                for (int j = 0; j < len; ++j) {
                    float const n4 = float(j) / float(len - 1);
                    vec2 const f2 = vpos(i, j);
                    vec2 const f3 = rw::normalized(p - f2);
                    vec2 const f4 = custom::perpendicular_vector(f3);
                    float const rhs = rw::distance(f2, p) / 10.f;
                    float n5 = mathf::lerp(1.f, 2.f, lk.eartler_width) * lk.branch_rad[i][j] * mathf::lerp(0.5f, 1.f, squeeze);
                    v[j * 4] = p - rw::slerp(a, f4, 0.5f) * (n5 + n3) * 0.5f - f3 * rhs;
                    v[j * 4 + 1] = p + rw::slerp(a, f4, 0.5f) * (n5 + n3) * 0.5f - f3 * rhs;
                    if (n4 == 1.f)
                        n5 /= 4.f;
                    v[j * 4 + 2] = f2 - f4 * n5 + f3 * rhs;
                    v[j * 4 + 3] = f2 + f4 * n5 + f3 * rhs;
                    for (int q = 0; q < 4; ++q)
                        col[j * 4 + q] = rgba(headc);
                    p = f2;
                    a = f4;
                    n3 = n5;
                }
                if (lk.colored_eartler_tips)
                    for (int k = 0; k < 2; ++k)
                        col[(len - 1) * 4 + 3 - k] = rgba(eartler_tips);
                sprites::long_mesh(out, strip, v, col, size_t(len * 4));
            }
        }
        // HeadSprite
        {
            sprite s;
            s.element = "Circle20";
            s.pos = head;
            s.rotation = custom::vec_to_deg(f12);
            s.scale = vec2{mathf::lerp(8.f, 9.f, mathf::pow(n7, 0.5f)) * n9 / 10.f, mathf::lerp(11.f, 8.f, mathf::pow(n7, 0.5f)) * n9 / 10.f};
            s.color = headc;
            sprites::add(out, s);
        }
        vec2 const f14 = rw::slerp(f12, -f11, n7);
        vec2 const lhs2 = head + f14 * (4.f - mathf::max(10.f * mathf::pow(n7, 0.5f), 3.f * n8)) * n9;
        float const eyes_open = mathf::lerp(g.last_eyes_open, g.eyes_open, t);
        vec2 const vlook = lhs2 + f14 * mathf::lerp(-5.f, 60.f, lk.eyes_angle) * eyes_open;
        float const n11 = mathf::pow(mathf::max(0.f, mathf::lerp(g.last_eyes_pop, g.eyes_pop, t)), 0.25f);
        float const n12 = mathf::clamp(lk.eye_size + mathf::lerp(0.5f, 0.25f, n7) * n11, 0.f, 1.f);
        // TeethSprite
        {
            vec4 const tc = rgba(headc);
            for (int m = 0; m < lk.teeth_count; ++m) {
                float const n17 = float(m) / float(lk.teeth_count - 1);
                vec2 const perp = custom::perpendicular_vector(f14);
                float const sx = mathf::sign(f13.x);
                float const across = 1.f - std::fabs(f13.y);
                vec2 const f17 = lhs2 + f14 * 4.f + perp * mathf::lerp(-3.f, 3.f, n17) * f13.x;
                vec2 const f18 = lhs2 + f14 * mathf::lerp(8.f, 10.f, std::sin(n17 * mathf::pi)) * lk.teeth[m][0]
                    + perp * mathf::lerp(mathf::lerp(-9.f, 9.f, n17) * mathf::lerp(0.5f, 1.2f, lk.wide_teeth), -2.f * sx, across) * lk.teeth[m][0];
                vec2 const f19 = lhs2 + f14 * mathf::lerp(12.f, 15.f, std::sin(n17 * mathf::pi)) * lk.teeth[m][0]
                    + perp * mathf::lerp(mathf::lerp(-9.f, 9.f, n17) * mathf::lerp(0.5f, 1.2f, lk.wide_teeth), -15.f * sx, across) * lk.teeth[m][0];
                vec2 v[5];
                v[0] = f17 - custom::perpendicular_vector(f17, f18) * 1.f;
                v[1] = f17 + custom::perpendicular_vector(f17, f18) * 1.f;
                v[2] = f18 - custom::perpendicular_vector(f18, f19) * lk.teeth[m][1];
                v[3] = f18 + custom::perpendicular_vector(f18, f19) * lk.teeth[m][1];
                v[4] = f19;
                for (int q = 0; q < 3; ++q)
                    tri(v[q], v[q + 1], v[q + 2], tc, tc, tc);
            }
        }
        // the eyes (EyeSprite(eye, 0) both, then the pupils)
        {
            sprite whites[2], pupils[2];
            for (int j = 0; j < 2; ++j) {
                float n13 = (j == 0 ? -1.f : 1.f) * 0.5f + f13.x * (1.f - n8);
                n13 = mathf::clamp(n13, -1.f, 1.f);
                vec2 const f15 = lhs2 + custom::perpendicular_vector(f14) * 8.f * n9 * n13;
                float const n14 = custom::aim(f15, vlook);
                float const n15 = mathf::lerp(1.5f, 2.f, mathf::pow(n7, 1.5f)) * mathf::lerp(0.3f, 1.5f, mathf::pow(n12, 0.7f)) * (1.f + 0.2f * n11 * (1.f - n7))
                    * mathf::inverse_lerp(1.f, 0.7f, std::fabs(n13)) * mathf::lerp(1.f, mathf::lerp(0.5f, 0.25f, n12), lk.narrow_eyes * mathf::lerp(1.f, 0.5f, n11)) * eyes_open;
                float const n16 = mathf::lerp(2.5f, 1.5f, mathf::pow(n7, 0.5f)) * mathf::lerp(0.3f, 1.5f, mathf::pow(n12, 0.7f)) * (1.f + 0.2f * n11)
                    * mathf::inverse_lerp(0.f, 0.75f, eyes_open);
                whites[j].element = "Circle20";
                whites[j].pos = f15;
                whites[j].rotation = n14;
                whites[j].scale = vec2{n15 * 0.1f, n16 * 0.1f};
                whites[j].color = eye_white;
                if (lk.pupil_size > 0.f) {
                    vec2 f16;
                    if (lk.deep_pupils)
                        f16 = -f11;
                    else
                        f16 = custom::dir_vec(f15, g.look_point) * mathf::inverse_lerp(0.f, 30.f, rw::distance(f15, g.look_point))
                            * mathf::inverse_lerp(0.3f, 0.7f, w.scavenger_data_get_personality(d).sympathy);
                    f16 = rot(f16, n14);
                    f16 = vec2{f16.x * n15 * (1.f - lk.pupil_size), f16.y * n16 * (1.f - lk.pupil_size)};
                    f16 = rot(f16, -n14);
                    pupils[j].element = "Circle20";
                    pupils[j].pos = f15 + f16;
                    pupils[j].scale = vec2{n15 * 0.1f * lk.pupil_size * (1.f - 0.5f * n11), n16 * 0.1f * lk.pupil_size * (1.f - 0.5f * n11)};
                    pupils[j].rotation = n14;
                    pupils[j].color = pupil;
                }
            }
            for (auto const& s : whites)
                sprites::add(out, s);
            if (lk.pupil_size > 0.f)
                for (auto const& s : pupils)
                    sprites::add(out, s);
        }
        (void)x;
    }
}
