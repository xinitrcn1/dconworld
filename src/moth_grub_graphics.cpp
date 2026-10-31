#include <cmath>

#include "moth_grub_graphics.hpp"
#include "creature.hpp"
#include "moth_grub.hpp"
#include "moth_grub_types.hpp"

namespace moth_grub_graphics {
    using rw::vec2;
    using rw::vec3;
    using sprites::sprite;
    using moth_grub::graphics_state;
    using moth_grub::limb_segments;
    using moth_grub::wrapping_points;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        graphics_state& gs(sys::game_state& state, dcon::creature_id c) {
            return state.world.moth_grub_data_get_graphics(creature::moth_grub_data(state.world, c));
        }
        graphics_state const& gs(sys::game_state const& state, dcon::creature_id c) {
            return state.world.moth_grub_data_get_graphics(creature::moth_grub_data(state.world, c));
        }
        vec2 cp(sys::game_state const& state, dcon::creature_id c, uint32_t i) { return state.world.body_chunk_get_pos(creature::chunk(state, c, i)); }

        vec3 lerp3(vec3 a, vec3 b, float t) {
            t = mathf::clamp01(t);
            return a + (b - a) * t;
        }

        // Custom.BezierThroughHandles: the handles moved so the curve goes
        // through them (left as they are when the points coincide)
        void bezier_through_handles(vec2 a, vec2& ca, vec2 b, vec2& cb) {
            float const num = rw::distance(a, ca);
            float const num2 = rw::distance(ca, cb);
            float const num3 = rw::distance(b, cb);
            float const total = num + num2 + num3;
            if (total < 1e-5f)
                return;
            float const num4 = num / total;
            float const num5 = (num + num2) / total;
            float const num6 = num4 * (1.f - num4) * (1.f - num4) * 3.f;
            float const num7 = (1.f - num4) * num4 * num4 * 3.f;
            float const num8 = num5 * (1.f - num5) * (1.f - num5) * 3.f;
            float const num9 = (1.f - num5) * num5 * num5 * 3.f;
            if (num6 == 0.f || num8 == 0.f || (num7 - num6 * num9 / num8) == 0.f)
                return;
            float const num10 = ca.x - a.x * mathf::pow(1.f - num4, 3.f) - b.x * mathf::pow(num4, 3.f);
            float const num11 = cb.x - a.x * mathf::pow(1.f - num5, 3.f) - b.x * mathf::pow(num5, 3.f);
            float const num12 = ca.y - a.y * mathf::pow(1.f - num4, 3.f) - b.y * mathf::pow(num4, 3.f);
            float const num13 = cb.y - a.y * mathf::pow(1.f - num5, 3.f) - b.y * mathf::pow(num5, 3.f);
            cb.x = (num10 - num6 / num8 * num11) / (num7 - num6 * num9 / num8);
            ca.x = (num10 - num7 * cb.x) / num6;
            cb.y = (num12 - num6 / num8 * num13) / (num7 - num6 * num9 / num8);
            ca.y = (num12 - num7 * cb.y) / num6;
        }
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        auto& g = gs(state, c);
        vec2 const c0 = cp(state, c, 0), c1 = cp(state, c, 1), c2 = cp(state, c, 2);
        for (uint32_t i = 0; i < wrapping_points; ++i) {
            float const t = mathf::inverse_lerp(0.f, float(wrapping_points - 1), float(i));
            g.wrapping[0][i][0] = rw::lerp(c2, c0, t) - vec2{20.f, 0.f};
            g.wrapping[1][i][0] = rw::lerp(c2, c0, t) + vec2{20.f, 0.f};
            g.wrapping[0][i][1] = g.wrapping[0][i][0];
            g.wrapping[1][i][1] = g.wrapping[1][i][0];
        }
        for (uint32_t j = 0; j < 2; ++j) {
            float const num = j == 0 ? 1.f : -1.f;
            for (uint32_t k = 0; k < 2; ++k) {
                float const num2 = k == 0 ? 1.f : -1.f;
                for (uint32_t l = 0; l < limb_segments; ++l) {
                    g.limbs[j][k][l][0] = c1 + vec2{num * 3.f + num * float(l), num2 * 3.f};
                    g.limbs[j][k][l][1] = g.limbs[j][k][l][0];
                    g.limbs[j][k][l][2] = vec2{0.f, 0.f};
                }
            }
        }
    }

    void update(sys::game_state& state, dcon::creature_id c, world::room_view const&) {
        auto& w = state.world;
        auto& g = gs(state, c);
        float const energy = w.creature_get_payload(c).moth_grub.energy;
        float to = 0.5f + std::cos(float(g.flap_anim) * (0.05f + energy * 0.05f)) * 0.3f;
        float num = 0.03f;
        if (creature::consious(state, c)) {
            ++g.flap_anim;
            // (it warms up its wings for a big moth wanting to fly: none
            // are ported)
            if (g.flap_warm_up > 0)
                g.flap_warm_up = mathf::max(g.flap_warm_up - 2, 0);
            if (g.flap_warm_up > 0) {
                float const num2 = mathf::min(float(g.flap_warm_up), 230.f) / 230.f;
                to = std::cos(float(g.flap_anim) * (0.15f + energy * 0.2f));
                num = 0.05f + num2 * 0.95f;
            }
        }
        g.flapness = custom::lerp_and_tick(g.flapness, to, num, num * 0.1f);
        vec2 const c0 = cp(state, c, 0), c1 = cp(state, c, 1), c2 = cp(state, c, 2), c3 = cp(state, c, 3), c4 = cp(state, c, 4);
        vec2 vector = c2;
        vec2 const vector2 = custom::closest_point_on_line(c3, c4, c2);
        if (rw::dot(custom::perpendicular_vector(custom::dir_vec(c3, c4)), custom::dir_vec(c2, vector2)) < 0.f)
            vector = vector2;
        vec2 const vector3 = custom::dir_vec(c1, c0);
        vec2 const vector4 = custom::dir_vec(vector, c1);
        vec2 const a = custom::perpendicular_vector(vector3);
        vec2 const vector5 = custom::perpendicular_vector(vector4);
        vec2 const b = c0 + (vector3 - vector4 * 0.4f) * 10.f;
        float const num3 = std::fabs(rw::dot(vector3, vector5));
        float const r = 8.f + num3 * 2.f;
        vec2 const mid = rw::lerp(c1, c0 + vector3 * 4.f, 0.5f);
        vec2 cb = mid + rw::normalized(a * 0.5f + custom::dir_vec(c4, c0)) * r;
        vec2 cb2 = mid + rw::normalized(-a * 0.5f + custom::dir_vec(c3, c0)) * r;
        vec2 ca = c3 + rw::normalized(vector5 * 2.f + custom::dir_vec(c1, c3)) * r;
        vec2 ca2 = c4 + rw::normalized(-vector5 * 2.f + custom::dir_vec(c1, c4)) * r;
        vec2 const a2 = vector - vector4 * (12.f + num3 * 2.f) - vector5 * rw::dot(vector3, vector5) * 5.f;
        bezier_through_handles(a2, ca, b, cb);
        bezier_through_handles(a2, ca2, b, cb2);
        for (uint32_t i = 0; i < wrapping_points; ++i) {
            g.wrapping[0][i][1] = g.wrapping[0][i][0];
            g.wrapping[1][i][1] = g.wrapping[1][i][0];
            float const f = mathf::inverse_lerp(0.f, float(wrapping_points - 1), float(i));
            g.wrapping[0][i][0] = custom::bezier(a2, ca, b, cb, f);
            g.wrapping[1][i][0] = custom::bezier(a2, ca2, b, cb2, f);
        }
        constexpr float num4 = 17.f;
        constexpr float num5 = num4 / float(limb_segments);
        for (uint32_t j = 0; j < 2; ++j)
            for (uint32_t k = 0; k < 2; ++k) {
                vec2 const vector6 = custom::dir_vec(g.wrapping[j][wrapping_points - 5][0], g.wrapping[j][wrapping_points - 3][0]);
                vec2 const a3 = custom::perpendicular_vector(vector6) * (j == 0 ? 1.f : -1.f);
                vec2 const normalized = rw::normalized(a3 + vector6 * (k == 0 ? 1.f : -1.f) * 0.7f * g.flapness);
                vec2 const vector7 = g.wrapping[j][wrapping_points - 4][0] - a3 * 2.f + vector6 * (k == 0 ? 0.f : -2.f);
                auto& limb = g.limbs[j][k];
                for (uint32_t l = 0; l < limb_segments; ++l) {
                    limb[l][1] = limb[l][0];
                    limb[l][0] += limb[l][2];
                    limb[l][2] *= 0.9f;
                    float const num6 = mathf::inverse_lerp(0.f, float(limb_segments), float(l + 1));
                    vec2 const b2 = (vector7 + normalized * num4 * num6 - limb[l][0]) * 0.25f * (1.f - num6 * 0.8f);
                    limb[l][0] += b2;
                    limb[l][2] += b2;
                }
                for (uint32_t m = 1; m < limb_segments; ++m) {
                    vec2 const a4 = custom::dir_vec(limb[m - 1][0], limb[m][0]);
                    if (rw::distance(limb[m - 1][0], limb[m][0]) > num5)
                        limb[m][0] = limb[m - 1][0] + a4 * num5;
                }
                limb[0][0] = vector7;
                limb[1][0] = rw::lerp(limb[1][0], vector7, 0.5f);
            }
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip) {
        auto const& g = gs(state, c);
        float const t = alpha;
        // ApplyPalette
        vec3 const black = pal.black();
        vec3 const body_color = lerp3(vec3{1.f, 0.72f, 0.5f}, black, 0.2f);
        vec3 const mouth_color = lerp3(body_color, black, 0.9f);
        // BodyMesh: MakeLongMesh(5) over the two sides
        vec2 vs[wrapping_points * 2];
        for (uint32_t k = 0; k < wrapping_points; ++k) {
            vs[k * 2] = rw::lerp(g.wrapping[0][k][1], g.wrapping[0][k][0], t);
            vs[k * 2 + 1] = rw::lerp(g.wrapping[1][k][1], g.wrapping[1][k][0], t);
        }
        sprites::long_mesh(out, strip, vs, wrapping_points * 2, body_color);
        // (BodyStripeMesh: the same mesh through the MothGrubStripes shader,
        // which isn't ported: left out)
        // the mouth: two crossed "pixel"s
        vec2 v = rw::lerp(g.wrapping[0][wrapping_points - 1][1], g.wrapping[0][wrapping_points - 1][0], t);
        vec2 const n = rw::normalized(custom::dir_vec(rw::lerp(g.wrapping[0][wrapping_points - 2][1], g.wrapping[0][wrapping_points - 2][0], t), v)
            + custom::dir_vec(rw::lerp(g.wrapping[1][wrapping_points - 2][1], g.wrapping[1][wrapping_points - 2][0], t), v));
        v -= n * 2.5f;
        float const rot = custom::vec_to_deg(n);
        sprites::add(out, sprite{"pixel", v, rot, vec2{1.7f, 7.f}, vec2{0.5f, 0.5f}, mouth_color});
        sprites::add(out, sprite{"pixel", v, rot, vec2{7.f, 1.7f}, vec2{0.5f, 0.5f}, mouth_color});
        // LimbMesh(side, i): MakeLongMesh(3)
        for (uint32_t l = 0; l < 2; ++l)
            for (uint32_t m = 0; m < 2; ++m) {
                auto const& limb = g.limbs[l][m];
                vec2 lv[limb_segments * 2];
                vec2 const v2 = rw::lerp(limb[0][1], limb[0][0], t);
                vec2 const p = rw::lerp(limb[1][1], limb[1][0], t);
                vec2 v3 = v2 + custom::dir_vec(p, v2) * 4.f;
                for (uint32_t k = 0; k < limb_segments; ++k) {
                    float const num2 = mathf::inverse_lerp(0.f, float(limb_segments - 1), float(k));
                    vec2 const vector3 = rw::lerp(limb[k][1], limb[k][0], t);
                    vec2 const a = custom::perpendicular_vector(vector3, v3);
                    float const d = (2.5f - 0.4f * num2) * (num2 > 0.7f ? 1.f - mathf::pow(num2 - 0.7f, 2.f) * 11.f : 1.f);
                    lv[k * 2] = vector3 - a * d;
                    lv[k * 2 + 1] = vector3 + a * d;
                    v3 = vector3;
                }
                sprites::long_mesh(out, strip, lv, limb_segments * 2, body_color);
            }
    }
}
