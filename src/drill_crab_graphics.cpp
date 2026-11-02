// Watcher/DrillCrabGraphics.cs. See drill_crab_graphics.hpp.

#include <cmath>
#include <cstdint>

#include "drill_crab_graphics.hpp"
#include "creature.hpp"
#include "drill_crab.hpp"
#include "drill_crab_types.hpp"

namespace drill_crab_graphics {
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using sprites::sprite;
    using drill_crab::graphics_state;
    using drill_crab::leg_count;
    using drill_crab::variant;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        dcon::drill_crab_data_id data(dcon::data_container const& w, dcon::creature_id c) {
            return dcon::drill_crab_data_id{dcon::drill_crab_data_id::value_base_t(w.creature_get_payload(c).drill_crab.data)};
        }
        graphics_state& gs(sys::game_state& state, dcon::creature_id c) { return state.world.drill_crab_data_get_graphics(data(state.world, c)); }

        vec3 lerp3(vec3 a, vec3 b, float t) {
            t = mathf::clamp01(t);
            return a + (b - a) * t;
        }
        float lerp_u(float a, float b, float t) { return a + (b - a) * t; }

        // Custom.BezierDerivative
        vec2 bezier_derivative(vec2 a, vec2 ca, vec2 b, vec2 cb, float f) {
            return (ca - a) * (3.f * (1.f - f) * (1.f - f)) + (cb - ca) * (6.f * (1.f - f) * f) + (b - cb) * (3.f * f * f);
        }

        // Mathf.PerlinNoise: 2D gradient noise in about 0..1 (Unity's own
        // permutation isn't reproduced: a hashed one, the same shape of noise)
        uint32_t perm(int32_t i) {
            uint32_t h = uint32_t(i & 255) * 0x9E3779B1u;
            h ^= h >> 15;
            h *= 0x85EBCA77u;
            h ^= h >> 13;
            return h & 255u;
        }
        float grad(uint32_t h, float x, float y) {
            switch (h & 7u) {
            case 0: return x + y;
            case 1: return -x + y;
            case 2: return x - y;
            case 3: return -x - y;
            case 4: return x;
            case 5: return -x;
            case 6: return y;
            default: return -y;
            }
        }
        float fade(float t) { return t * t * t * (t * (t * 6.f - 15.f) + 10.f); }
        float perlin_noise(float x, float y) {
            int32_t const xi = int32_t(std::floor(x)), yi = int32_t(std::floor(y));
            float const xf = x - std::floor(x), yf = y - std::floor(y);
            float const u = fade(xf), v = fade(yf);
            uint32_t const aa = perm(int32_t(perm(xi)) + yi), ab = perm(int32_t(perm(xi)) + yi + 1);
            uint32_t const ba = perm(int32_t(perm(xi + 1)) + yi), bb = perm(int32_t(perm(xi + 1)) + yi + 1);
            float const x1 = lerp_u(grad(aa, xf, yf), grad(ba, xf - 1.f, yf), u);
            float const x2 = lerp_u(grad(ab, xf, yf - 1.f), grad(bb, xf - 1.f, yf - 1.f), u);
            return mathf::clamp01(lerp_u(x1, x2, v) * 0.5f + 0.5f);
        }

        // DrillCrabGraphics.LegContour
        float leg_contour(drill_crab::individual_variations const& iv, uint32_t segments, uint32_t leg, uint32_t segment, float t) {
            float num = perlin_noise(float(segment) * 0.2f + float(leg), t * 40.f) * 0.5f;
            float num2;
            if (segment == 0) {
                num2 = mathf::lerp(0.3f, 1.f, t) * (mathf::pow(1.f - t, 0.4f) * 0.5f + 0.5f);
            } else if (segment == 1) {
                num2 = (std::cos(t * mathf::pi * 2.f) * 0.25f + 0.75f) * (1.f - 0.2f * t) * (0.5f + 0.5f * mathf::pow(t, 0.25f));
            } else if (segment < segments - 1) {
                num2 = (std::cos(t * mathf::pi * 2.f) * 0.25f + 0.75f) * (0.8f - 0.2f * t - 0.2f * mathf::pow(1.f - t, 10.f));
            } else {
                num2 = mathf::pow(mathf::max(1.f - t, 0.f), 0.33333334f) * (0.6f - 0.2f * t);
                num *= 1.f - t;
            }
            num2 += num * 0.5f;
            return mathf::pow(mathf::max(num2, 0.f), 1.25f - iv.leg_uniformity * 0.5f);
        }

        // DrillCrabGraphics: toothCount (+ extraTeeth each), toothRingRadius, toothAngle
        constexpr int32_t tooth_count[3] = {9, 8, 5};
        constexpr float tooth_ring_radius[3] = {21.f, 20.f, 9.f};
        constexpr float tooth_angle[3] = {-10.f, 30.f, 45.f};
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        auto& g = gs(state, c);
        g.last_drill_rotation = g.drill_rotation;
    }

    void update(sys::game_state& state, dcon::creature_id c, world::room_view const&) {
        auto& g = gs(state, c);
        float const spin_up = state.world.creature_get_payload(c).drill_crab.spin_up;
        g.last_drill_rotation = g.drill_rotation;
        g.drill_rotation += mathf::pow(spin_up, 1.5f) * 0.5f;
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip) {
        auto const& w = state.world;
        auto const d = data(w, c);
        auto const& p = w.creature_get_payload(c).drill_crab;
        auto const& b = w.drill_crab_data_get_body(d);
        auto const& iv = w.drill_crab_data_get_ivars(d);
        auto const& g = w.drill_crab_data_get_graphics(d);
        bool const tower = p.variant == variant::tower;
        uint32_t const nseg = drill_crab::leg_segments(p.variant);
        float const t = alpha;

        // ApplyPalette (the depth layer's fog: none, layer 0)
        vec3 const white{1.f, 1.f, 1.f};
        vec3 const black = pal.black();
        vec3 color = lerp3(black, white, 0.1f);
        vec3 color2 = lerp3(black, white, 0.2f);
        vec3 const rust{0.4f, 0.2f, 0.15f};
        if (tower) {
            color = lerp3(color, rust, 0.1f);
            color2 = lerp3(lerp3(color2, white, 0.1f), rust, 0.1f);
        }
        vec3 const eye_color = lerp3(black, white, iv.eye_lightness * 0.2f + 0.1f);
        auto const leg_gradient = [&](float x) { return lerp3(lerp3(black, white, mathf::pow(x, 2.f) * 0.2f), rust, mathf::pow(x, 3.f) * 0.2f); };

        auto const k0 = creature::chunk(state, c, 0);
        auto const k1 = creature::chunk(state, c, 1);
        vec2 const vv = rw::lerp(w.body_chunk_get_last_pos(k0), w.body_chunk_get_pos(k0), t);
        vec2 const v2 = custom::dir_vec(vv, rw::lerp(w.body_chunk_get_last_pos(k1), w.body_chunk_get_pos(k1), t));
        vec2 const a = custom::perpendicular_vector(v2);
        float const num = mathf::lerp(p.last_flip, p.flip, t);
        float const num2 = mathf::pow(p.spin_up, 2.f);

        // the eyes: the stalk and the eye at its end
        for (uint32_t j = 0; j < 2; ++j) {
            uint32_t const ei = p.flip > 0.f ? j : 1 - j;
            auto const& e = b.eyes[ei];
            float const side = ei == 0 ? 1.f : -1.f;
            float const num3 = custom::s_curve(mathf::lerp(p.last_eyes_hidden, p.eyes_hidden, t), 1.f);
            vec2 const v3 = rw::lerp(e.last_pos, e.pos, t);
            vec2 const v4 = vv - v2 * 25.f;
            vec2 const v5 = rw::slerp(rw::normalized(v3 - v4), vec2{0.f, 1.f}, 0.75f);
            vec2 const ca = v4 + a * (num * (tower ? 50.f : 20.f) + side * 15.f * std::cos(num * mathf::pi * 0.3f)) - v2 * (tower ? 45.f : 20.f);
            vec2 const cb = v3 - v5 * (tower ? -20.f : 40.f) + a * (num * -15.f);
            // MakeLongMeshAtlased(6): 24 vertices
            constexpr uint32_t nv = 24;
            vec2 verts[nv];
            for (uint32_t k = 0; k < nv; k += 2) {
                float const num4 = float(k) / float(nv - 2) * (1.f - num3);
                vec2 const a2 = custom::bezier(v4, ca, v3, cb, num4);
                vec2 const a3 = custom::perpendicular_vector(bezier_derivative(v4, ca, v3, cb, num4));
                float const dd = (std::cos(num4 * mathf::pi) * 0.4f + 0.6f) * 5.f;
                verts[k] = a2 + a3 * dd;
                verts[k + 1] = a2 - a3 * dd;
            }
            sprites::long_mesh(out, strip, verts, nv, black);
            float const sc = tower ? 1.5f : 1.f;
            sprites::add(out, sprite{"pixel", custom::bezier(v4, ca, v3, cb, 1.f - num3) + a * side, custom::vec_to_deg(v5), vec2{4.f * sc, 7.f * sc}, vec2{0.5f, 0.1f},
                                  eye_color});
        }

        // the shaking of the spinning drill (Random each frame: keyed by the tick)
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        vec2 v6{0.f, 0.f}, bshake{0.f, 0.f};
        if (num2 > 0.f) {
            float const r1 = rng.value();
            v6 = custom::rnv(rng) * (r1 * num2 * 4.f);
            float const r2 = rng.value();
            bshake = custom::rnv(rng) * (r2 * num2 * 3.f);
        }
        float const num6 = mathf::lerp(0.9f, 1.1f, iv.drill_size);
        float const num7 = lerp_u(0.9f, 1.1f, iv.body_size);
        float const num8 = custom::vec_to_deg(v2);

        // the blob: JaggedCircle over Futile_White (16 pixels) at 4.0625, alpha
        // 0.1 (its jaggedness)
        sprites::add_circle(out, vv + bshake - v2 * 5.f, 8.f * 4.0625f * num7, black, num8, 0.064f, uint32_t(w.drill_crab_data_get_seed(d)) | 1u);
        // the drill
        sprites::add(out, sprite{"CrabDrillCoverA", vv + v2 * 3.f - v6 * 0.5f, num8, vec2{num7, num7}, vec2{0.5f, 0.5f}, black});
        sprites::add(out, sprite{"CrabDrillCoverB", vv + v2 * 3.f - v6 * 0.5f, num8, vec2{num7, num7}, vec2{0.5f, 0.5f}, color2});
        sprites::add(out, sprite{"CrabDrillBodyA", vv + bshake + v2 * -10.f, num8, vec2{num7, num7}, vec2{0.5f, 0.5f}, black});
        sprites::add(out, sprite{"CrabDrillBodyB", vv + bshake + v2 * -10.f + a * (std::sin(num * 0.2f) * 35.f), num8,
                              vec2{std::cos(num * 0.2f) * 0.75f * num7, num7}, vec2{0.5f, 0.5f}, color2});
        sprites::add(out, sprite{"CrabDrillHead", vv + v2 * (5.5f * num6) + v6, num8, vec2{num6, num6}, vec2{0.5f, 0.f}, color});
        // the teeth, in three rings turning with the drill
        float const rot = mathf::lerp(g.last_drill_rotation, g.drill_rotation, t);
        for (uint32_t m = 0; m < 3; ++m) {
            vec2 const a4 = vv + v2 * (custom::lerp_map(float(m), 0.f, 2.f, 7.f, 30.f) * num6 + 5.5f);
            int32_t const count = tooth_count[m] + iv.extra_teeth;
            float const d2 = tooth_ring_radius[m] * num6;
            for (int32_t n = 0; n < count; ++n) {
                float num11 = (count > 1 ? float(n) / float(count - 1) : 0.f) * mathf::pi + rot + num * 0.5f;
                num11 = std::fmod(std::fmod(num11, mathf::pi) + mathf::pi, mathf::pi) - 1.5707964f;
                float const r = rng.value();
                vec2 const jitter = custom::rnv(rng) * (r * num2 * 1.5f);
                vec2 const pos = a4 + a * (d2 * std::sin(num11)) - v2 * (d2 * 0.2f * std::cos(num11)) + jitter + v6;
                float const sx = 0.15f + 0.2f * mathf::pow(mathf::max(std::cos(num11), 0.f), 0.7f) + num2 * 0.6f;
                float const sy = 0.35f - num2 * 0.15f;
                sprites::add(out, sprite{"SnailTail", pos, num8 + num11 * tooth_angle[m] / mathf::pi * 2.f, vec2{sx, sy}, vec2{0.5f, 0.5f}, color2});
            }
        }

        // the legs: a long mesh a segment (MakeLongMeshAtlased(ceil(prevDist /
        // 10)), the tower crab's 30), a Circle20 at each joint
        float const leg_width = lerp_u(3.f, 6.f, iv.leg_thickness);
        float const per = tower ? 30.f : 10.f;
        uint32_t li = 0;
        for (auto const o : creature::parts(w, c)) {
            auto const part = w.part_ownership_get_part(o);
            if (w.creature_part_get_kind(part) != creature::part_kind::drill_crab_leg || li >= leg_count)
                continue;
            uint32_t const leg = li++;
            auto const& lp = w.creature_part_get_payload(part).drill_crab_leg;
            vec2 v7 = rw::lerp(lp.last_anchor, lp.anchor, t);
            for (uint32_t s = 0; s < nseg; ++s) {
                auto const& sg = b.segments[leg][s];
                vec2 const v8 = rw::lerp(sg.last_pos, sg.pos, t);
                vec2 const a5 = custom::perpendicular_vector(v8, v7);
                constexpr uint32_t max_verts = 96;
                uint32_t nv = uint32_t(std::ceil(drill_crab::segment_length(state, c, leg, s) / per)) * 4u;
                nv = nv < 4u ? 4u : (nv > max_verts ? max_verts : nv);
                vec2 verts[max_verts];
                vec4 cols[max_verts];
                for (uint32_t k = 0; k < nv; k += 2) {
                    float const tt = float(k) / float(nv - 2);
                    vec2 const a6 = rw::lerp(v7, v8, tt);
                    float const d3 = leg_contour(iv, nseg, leg, s, tt) * leg_width;
                    verts[k] = a6 + a5 * d3;
                    verts[k + 1] = a6 - a5 * d3;
                    vec3 const col = tower ? leg_gradient((float(s) + float(k) / float(nv - 2)) / float(nseg)) : black;
                    cols[k] = cols[k + 1] = vec4{col.x, col.y, col.z, 1.f};
                }
                sprites::long_mesh(out, strip, verts, cols, nv);
                if (s + 1 < nseg) {
                    float const sc = leg_contour(iv, nseg, leg, s, 1.f) / 10.f * leg_width;
                    vec3 const jc = tower ? leg_gradient(float(s + 1) / float(nseg)) : black;
                    sprites::add(out, sprite{"Circle20", v8, 0.f, vec2{sc, sc}, vec2{0.5f, 0.5f}, jc});
                }
                v7 = v8;
            }
        }
    }
}
