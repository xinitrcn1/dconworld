// AnglerGraphics.cs (angler_graphics.hpp). Everything here is cosmetic and
// only updates where a camera looks; the lure's position is the
// simulation's (angler.cpp), the flicker, the lights' linger and the anger
// colour are here.

#include <algorithm>
#include <cmath>

#include "angler_graphics.hpp"
#include "angler.hpp"
#include "body_parts.hpp"
#include "creature.hpp"
#include "particles.hpp"
#include "physics.hpp"

namespace angler_graphics {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;
    using angler::graphics_state;
    using angler::looks;

    namespace {
        // TriangleMesh.MakeLongMesh(6): 24 vertices (the meshes move 12 pairs)
        constexpr uint32_t mesh_vertices = 24;

        dcon::angler_data_id data(sys::game_state const& state, dcon::creature_id c) { return angler::data_of(state.world, c); }
        angler::payload const& pl(sys::game_state const& state, dcon::creature_id c) { return state.world.creature_get_payload(c).angler; }
        vec2 chunk_pos(sys::game_state const& state, dcon::creature_id c, uint32_t i, float alpha) {
            auto const k = creature::chunk(state, c, i);
            return rw::lerp(state.world.body_chunk_get_last_pos(k), state.world.body_chunk_get_pos(k), alpha);
        }
        vec3 mix(vec3 a, vec3 b, float t) { return a + (b - a) * mathf::clamp01(t); }

        // AnglerGraphics.lightsOn
        bool lights_on(graphics_state const& g) { return g.light_linger > 0; }

        // AnglerGraphics.lightColor: the eye's colour gone greener, red with
        // the risk of anger; red when angry
        vec3 light_color(sys::game_state const& state, dcon::creature_id c, graphics_state const& g, looks const& lk) {
            if (pl(state, c).anger_timer <= 0)
                return mix(mix(lk.eye_color, vec3{0.f, 1.f, 0.f}, 0.25f), vec3{1.f, 0.f, 0.f}, mathf::pow(g.anger_risk, 2.f) * 0.9f);
            return vec3{1.f, 0.f, 0.f};
        }

        // FSprite placement of a TriangleMesh made in its own space: scaled,
        // turned (Futile's clockwise degrees), put at `at`
        void local_mesh(std::vector<sprites::draw_cmd>& out, std::vector<sprites::strip_vertex>& strip, vec2 const* local, size_t n, vec2 at, float rotation,
            vec2 scale, vec3 color) {
            float const s = std::sin(rotation * mathf::deg2rad);
            float const co = std::cos(rotation * mathf::deg2rad);
            vec2 v[mesh_vertices];
            n = std::min<size_t>(n, mesh_vertices);
            for (size_t i = 0; i < n; ++i) {
                float const x = local[i].x * scale.x;
                float const y = local[i].y * scale.y;
                v[i] = at + vec2{x * co + y * s, -x * s + y * co};
            }
            sprites::long_mesh(out, strip, v, n, color);
        }

        // the meshes' vertex jitter (InitiateSprites' Random.value, rolled
        // once: the same every frame)
        rw::rng jitter(sys::game_state const& state, dcon::creature_id c, uint32_t part) {
            uint32_t const seed = uint32_t(state.world.angler_data_get_seed(data(state, c)));
            return rw::rng{rw::keyed::key(seed, 0, part, 0xA9613E5Bu), 0};
        }

        // AnglerGraphics.MakeSpikeMesh
        void spike_mesh(angler::spike const& sp, rw::rng& r, vec2 (&v)[mesh_vertices]) {
            constexpr int32_t num = 6;
            vec2 const up{0.f, 1.f};
            vec2 a = up * -5.f;
            for (int32_t i = 0; i < num * 2; ++i) {
                vec2 const a2 = custom::perpendicular_vector(up);
                float const j0 = 0.8f + r.value() * 0.2f;
                float const j1 = 0.8f + r.value() * 0.2f;
                v[i * 2] = a + a2 * (sp.rad * j0);
                v[i * 2 + 1] = a - a2 * (sp.rad * j1);
                if (i == 0)
                    a = vec2{0.f, 0.f};
                a += up * (sp.length / (float(num) * 2.f - 1.f));
            }
        }

        // AnglerGraphics.MakeToothMesh
        void tooth_mesh(angler::tooth const& t, rw::rng& r, vec2 (&v)[mesh_vertices]) {
            constexpr int32_t num = 6;
            vec2 nrm = rw::normalized(vec2{1.f, t.forward_tilt});
            vec2 a{-1.5f, 0.f};
            for (int32_t i = 0; i < num * 2; ++i) {
                float const num2 = mathf::inverse_lerp(0.f, float(num) * 2.f - 1.f, float(i));
                float const d = t.rad * std::sin((num2 + 1.f) * mathf::pi * 0.5f);
                vec2 const a2 = custom::perpendicular_vector(nrm);
                float const j0 = 0.8f + r.value() * 0.2f;
                float const j1 = 0.8f + r.value() * 0.2f;
                v[i * 2] = a + a2 * (d * j0);
                v[i * 2 + 1] = a - a2 * (d * j1);
                if (i == 0)
                    a = vec2{0.f, 0.f};
                nrm = rw::normalized(nrm - a2 * t.curve);
                a += nrm * (t.length / (float(num) * 2.f - 1.f));
            }
        }

        // the rudder mesh (InitiateSprites: a curved fin, the same always)
        void rudder_mesh(float rudder_length, vec2 (&v)[mesh_vertices]) {
            constexpr int32_t num = 6;
            vec2 const b{45.f, 5.f};
            vec2 const a{-35.f, 5.f};
            vec2 const vector{-7.f, -20.f};
            vec2 const p = a + vec2{0.f, 1.f};
            for (int32_t j = 0; j < num * 2; ++j) {
                float const num2 = mathf::inverse_lerp(0.f, float(num * 2 - 1), float(j));
                vec2 const v2 = rw::lerp(rw::lerp(a, vector, num2), rw::lerp(vector, b, num2), num2);
                vec2 const a2 = custom::perpendicular_vector(custom::dir_vec(p, v2));
                float const d = (1.f - mathf::pow(num2 * 2.f - 1.f, 2.f)) * (2.5f + 4.5f * rudder_length);
                v[j * 2] = v2 - a2 * d;
                v[j * 2 + 1] = v2 + a2 * d;
            }
        }

        // DrawSprites' points: the head's frame (vector7, the head and jaw
        // directions and sizes), the tail cone, the lure arm
        struct frame {
            float flip = 0.f;
            float num = 1.f;
            vec2 v{0.f, 0.f}, v2{0.f, 0.f}, v3{0.f, 0.f};
            vec2 v4{0.f, 0.f}, v5{0.f, 0.f}, v6{0.f, 0.f}, a{0.f, 0.f};
            vec2 a2{0.f, 0.f};
            vec2 v7{0.f, 0.f};
            vec2 head_dir{0.f, 0.f}, a3{0.f, 0.f};
            vec2 v8{0.f, 0.f}, a4{0.f, 0.f};
            vec2 v9{0.f, 0.f}, v10{0.f, 0.f};
            // the arm: root, elbow, tip, and the bulb's light point
            vec2 v19{0.f, 0.f}, v20{0.f, 0.f}, v21{0.f, 0.f}, v22{0.f, 0.f}, v23{0.f, 0.f}, v26{0.f, 0.f};
        };
        constexpr float num2_cone = 42.f;
        frame frame_of(sys::game_state const& state, dcon::creature_id c, looks const& lk, float alpha) {
            auto const& p = pl(state, c);
            frame f;
            f.flip = p.flip;
            float const af = std::fabs(f.flip);
            f.num = f.flip > 0.f ? 1.f : -1.f;
            f.v = chunk_pos(state, c, 0, alpha);
            f.v2 = chunk_pos(state, c, 1, alpha);
            f.v3 = chunk_pos(state, c, 2, alpha);
            f.v4 = custom::dir_vec(f.v2, f.v);
            f.v5 = custom::dir_vec(f.v3, f.v2);
            f.v6 = custom::perpendicular_vector(f.v4);
            f.a = custom::perpendicular_vector(f.v5);
            f.a2 = f.v3 + f.v5 * 19.f;
            f.v7 = f.v - f.v4 * 2.f - f.v6 * 25.f * f.flip;
            f.head_dir = rw::normalized(f.v6 * (f.flip * (p.mouth_openness * 0.3f + 0.7f)) + f.v4 * 0.7f);
            f.a3 = custom::perpendicular_vector(f.head_dir);
            vec2 v8 = rw::normalized(-f.v6 * 0.4f * f.flip + f.v4);
            f.v8 = rw::normalized(rw::slerp(f.head_dir, v8, 0.2f + p.mouth_openness * 0.8f));
            f.a4 = custom::perpendicular_vector(f.v8);
            f.v9 = vec2{0.5f + (1.f - af) * 1.05f, 2.7f - (1.f - af) * 0.5f} * 20.f;
            f.v10 = vec2{0.5f + (1.f - af) * 2.f, 2.2f + af * 0.8f} * 20.f;
            // the lure's arm (Custom.InverseKinematic from its root to the lure)
            f.v19 = rw::lerp(f.v7 + f.head_dir * f.v10.y, f.a2 + f.a * lk.cone_rad * f.flip, 0.3f);
            vec2 v20 = angler::glow_pos(state, c, alpha);
            f.v21 = custom::inverse_kinematic(f.v19, v20, lk.arm_low_length, lk.arm_high_length, -f.flip);
            f.v22 = custom::dir_vec(f.v19, f.v21);
            f.v23 = custom::dir_vec(f.v21, v20);
            f.v20 = f.v21 + f.v23 * lk.arm_high_length;
            f.v26 = f.v20 + f.v23 * 7.f;
            return f;
        }
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        // (AnglerGraphics.Reset's lure is the simulation's: angler.cpp)
        auto& g = state.world.angler_data_get_graphics(data(state, c));
        float const on = lights_on(g) ? 1.f : 0.f;
        for (auto& f : g.flicker) {
            f[0] = on;
            f[1] = on;
            f[2] = on;
        }
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto const d = data(state, c);
        auto& g = w.angler_data_get_graphics(d);
        auto const& p = pl(state, c);
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        g.anger_risk = custom::lerp_and_tick(g.anger_risk, p.anger_risk_target, 0.05f, 0.01f);
        if (!w.creature_get_dead(c) && (p.locked || p.anger_timer > 0))
            g.light_linger = 70;
        else if (g.light_linger > 0)
            --g.light_linger;
        bool const on = lights_on(g);
        for (auto& f : g.flicker) {
            f[1] = f[0];
            f[0] += mathf::pow(rng.value(), 3.f) * 0.1f * (rng.value() < 0.5f ? -1.f : 1.f);
            f[0] = custom::lerp_and_tick(f[0], f[2], 0.05f, 0.033333335f);
            if (rng.value() < 0.2f && on)
                f[2] = 1.f + mathf::pow(rng.value(), 3.f) * 0.2f * (rng.value() < 0.5f ? -1.f : 1.f);
            f[2] = mathf::lerp(f[2], on ? 1.f : 0.f, 0.03f);
            if (!on && f[0] < 0.1f && f[2] < 0.1f) {
                f[0] = 0.f;
                f[1] = 0.f;
                f[2] = 0.f;
            }
        }
        // the propellers, and the bubbles they churn up (no WaterDrips)
        float const num = p.swim_speed * 0.15f;
        g.prop_spin += num;
        vec2 const c1 = w.body_chunk_get_pos(creature::chunk(state, c, 1));
        vec2 const vector = custom::dir_vec(w.body_chunk_get_pos(creature::chunk(state, c, 2)), c1);
        vec2 const a = custom::perpendicular_vector(vector);
        float const r0 = rng.value() * 2.f - 1.f;
        float const ang = rng.value() * 2.f * mathf::pi;
        float const rr = std::sqrt(rng.value()) * 7.f;
        vec2 const at = c1 + a * r0 * 60.f + vec2{std::cos(ang), std::sin(ang)} * rr;
        for (int32_t j = 0; j < 3; ++j)
            if (rng.value() < mathf::pow(p.swim_speed, 2.f) * 0.9f && room.point_submerged(at))
                particles::bubbles(state, room.id, at, 1, 3.f);
        // (no Angler_Pulse sonar, Angler_Swim or Angler_Shriek samples)
    }

    bool light_of(sys::game_state const& state, dcon::creature_id c, float alpha, light& out) {
        auto const& w = state.world;
        auto const d = data(state, c);
        auto const& g = w.angler_data_get_graphics(d);
        auto const& lk = w.angler_data_get_looks(d);
        float const f0 = mathf::lerp(g.flicker[0][1], g.flicker[0][0], alpha);
        float const f1 = mathf::lerp(g.flicker[1][1], g.flicker[1][0], alpha);
        if (g.flicker[0][0] <= 0.f && g.flicker[1][0] <= 0.f && f0 <= 0.f && f1 <= 0.f)
            return false;
        frame const f = frame_of(state, c, lk, alpha);
        out.color = light_color(state, c, g, lk);
        out.source_pos = angler::glow_pos(state, c, alpha);
        out.source_rad = 300.f * g.flicker[0][0];
        out.source_alpha = mathf::min((g.flicker[0][0] + g.flicker[1][0]) * 0.5f, 1.f);
        out.tip = f.v26;
        // Futile_White (16 px): scale flicker * 2.5 and flicker * 250 / 8
        out.flat_half = 8.f * f0 * 2.5f;
        out.glow_half = 8.f * f1 * 250.f / 8.f;
        return true;
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip) {
        auto const& w = state.world;
        auto const d = data(state, c);
        auto const& g = w.angler_data_get_graphics(d);
        auto const& lk = w.angler_data_get_looks(d);
        auto const& p = pl(state, c);
        // ApplyPalette: everything in the palette's black (the three chunk
        // sprites are invisible)
        vec3 const black = pal.black();
        frame const f = frame_of(state, c, lk, alpha);
        float const af = std::fabs(f.flip);
        float const num = f.num;
        vec3 const lcolor = light_color(state, c, g, lk);
        bool const on = lights_on(g);

        // the body mesh (sprite 3): from the cone's rim to the head's sides
        {
            vec2 const v28 = rw::lerp(f.v2, f.v, 0.5f) + f.v6 * 30.f;
            vec2 const v29 = rw::lerp(f.v2, f.v, 0.5f) - f.v6 * 30.f;
            float const num7 = custom::vec_to_deg(f.head_dir);
            vec2 v30 = body_parts::rotate_around_origo(f.v6, -num7);
            v30 = vec2{v30.x * f.v10.x, v30.y * f.v10.y} * 0.5f;
            v30 = body_parts::rotate_around_origo(v30, num7);
            vec2 const side = rw::slerp(v30, f.head_dir * (f.flip > 0.f ? 1.f : -1.f) * f.v10.y * 0.5f, mathf::pow(f.flip, 2.f));
            vec2 const b = f.v7 + f.head_dir * f.v10.y * 0.5f - side;
            vec2 const b2 = f.v7 + f.head_dir * f.v10.y * 0.5f + side;
            vec2 v[mesh_vertices];
            for (uint32_t m = 0; m < 12; ++m) {
                float const t = 1.f - mathf::inverse_lerp(0.f, 11.f, float(m));
                v[m * 2] = rw::lerp(rw::lerp(f.a2 + f.a * lk.cone_rad, v28, t), rw::lerp(v28, b2, t), t);
                v[m * 2 + 1] = rw::lerp(rw::lerp(f.a2 - f.a * lk.cone_rad, v29, t), rw::lerp(v29, b, t), t);
            }
            sprites::long_mesh(out, strip, v, mesh_vertices, black);
        }
        // the tail cone (sprite 4)
        {
            vec2 v[mesh_vertices];
            for (uint32_t i = 0; i < 12; ++i) {
                float num3 = mathf::inverse_lerp(0.f, 11.f, float(i));
                num3 = mathf::pow(num3, 2.f);
                float const dd = 1.f - num3;
                float const d2 = (1.f - mathf::pow(num3 - 1.f, 2.f)) * (lk.cone_rad + 2.f);
                v[i * 2] = f.a2 - f.v5 * num2_cone * dd - f.a * d2;
                v[i * 2 + 1] = f.a2 - f.v5 * num2_cone * dd + f.a * d2;
            }
            sprites::long_mesh(out, strip, v, mesh_vertices, black);
        }
        // the jaw and the head (Circle20 on their bottom edge), the cheek
        sprites::add(out, sprites::sprite{"Circle20", f.v7, custom::vec_to_deg(f.v8), f.v9 / 20.f, vec2{0.5f, 0.f}, black});
        sprites::add(out, sprites::sprite{"Circle20", f.v7, custom::vec_to_deg(f.head_dir), f.v10 / 20.f, vec2{0.5f, 0.f}, black});
        {
            vec2 const v11 = f.v7 + f.head_dir * f.v10.y * 0.3f - f.v4 * 20.f;
            vec2 const v12 = f.v7 + f.v8 * f.v9.y * 0.33f;
            vec2 const v13 = rw::lerp(v11, v12, 0.5f);
            sprites::add(out, sprites::sprite{"Circle20", v13, custom::vec_to_deg(custom::dir_vec(v11, v12)),
                vec2{1.1f - 0.4f * p.mouth_openness, rw::distance(v11, v12) / 20.f}, vec2{0.5f, 0.5f}, black});
        }
        // the rudder (sprite 8)
        {
            vec2 const v14 = f.a2 - f.v5 * (num2_cone + 20.f);
            float const rot = custom::vec_to_deg(rw::normalized(
                f.v5 - f.a * 3.f * p.rudder_turn_way * mathf::pow(mathf::max(1.f - af - 0.3f, 0.f) / 0.7f, 3.f)));
            float const num4 = mathf::max(af, 0.15f * std::fabs(p.rudder_turn_way)) * (f.flip > 0.f ? 1.f : -1.f);
            float const sx = std::sin(std::fabs(num4) * (num4 > 0.f ? -1.f : 1.f) * mathf::pi * 0.5f);
            vec2 v[mesh_vertices];
            rudder_mesh(lk.rudder_length, v);
            local_mesh(out, strip, v, mesh_vertices, v14, rot, vec2{sx, 1.f}, black);
        }
        // the spikes
        for (uint32_t j = 0; j < angler::spike_count; ++j) {
            auto const& sp = angler::spikes[j];
            float const d3 = (sp.top_side ? 1.f : -1.f) * af;
            vec2 const at = f.a2 - f.v5 * num2_cone * sp.placement + f.a * mathf::pow(sp.placement - 1.f, 2.f) * 20.f * d3;
            float const rot = custom::vec_to_deg(rw::normalized(-f.v5 + f.a * (1.f - sp.placement) * d3));
            auto r = jitter(state, c, j);
            vec2 v[mesh_vertices];
            spike_mesh(sp, r, v);
            local_mesh(out, strip, v, mesh_vertices, at, rot, vec2{1.f, 1.f}, black);
        }
        // the bottom teeth, along the jaw
        float const tooth_side = 1.f - mathf::pow(1.f - af, 3.f);
        for (uint32_t k = 0; k < lk.bottom_count; ++k) {
            float num5 = lk.bottom_count > 1 ? float(k) / (float(lk.bottom_count) - 1.f) : 0.f;
            num5 = num5 * 0.85f * 0.93f + 0.15f;
            vec2 const at = rw::lerp(f.v7, f.v7 + f.v8 * f.v9.y, num5) + f.a4 * f.v9.x * std::sin(num5 * mathf::pi) * 0.5f * num;
            float const rot = custom::vec_to_deg(f.v8 - f.a4 * p.mouth_openness * 0.5f * num);
            auto r = jitter(state, c, 100u + k);
            vec2 v[mesh_vertices];
            tooth_mesh(lk.bottom_teeth[k], r, v);
            local_mesh(out, strip, v, mesh_vertices, at, rot, vec2{-tooth_side * num, 1.f}, black);
        }
        // the top teeth, along the head
        for (uint32_t l = 0; l < lk.top_count; ++l) {
            float num6 = lk.top_count > 1 ? float(l) / (float(lk.top_count) - 1.f) : 0.f;
            num6 = num6 * 0.8f * 0.935f + 0.2f;
            vec2 const at = rw::lerp(f.v7, f.v7 + f.head_dir * f.v10.y, num6)
                + f.a3 * f.v10.x * mathf::lerp(std::sin(num6 * mathf::pi) * 0.5f, 0.5f, num6 < 0.5f ? 1.f - af : 0.f) * (f.flip > 0.f ? -1.f : 1.f);
            float const rot = custom::vec_to_deg(f.head_dir + f.a3 * p.mouth_openness * 0.5f * num);
            auto r = jitter(state, c, 200u + l);
            vec2 v[mesh_vertices];
            tooth_mesh(lk.top_teeth[l], r, v);
            local_mesh(out, strip, v, mesh_vertices, at, rot, vec2{tooth_side * num, 1.f}, black);
        }
        // the eye: dim, or lit with the lights (under water vanilla gives it
        // the water shader's glow colour; lit here either way)
        {
            vec2 const v18 = f.v7 + f.head_dir * f.v10.y * 0.85f - f.v4 * 3.f
                + custom::perpendicular_vector(f.head_dir) * 20.f * (1.f - af) * (f.flip > 0.f ? -1.f : 1.f);
            vec3 const col = on ? lk.eye_color : mix(black, lk.eye_color, 0.4f);
            sprites::add(out, sprites::sprite{"Circle20", v18, custom::vec_to_deg(f.head_dir),
                vec2{(0.125f + 0.175f * af) * lk.eye_size, 0.3f * lk.eye_size}, vec2{0.5f, 0.5f}, col});
        }
        // the lure's arm: the two bones, the two tendons, the bulb
        {
            sprites::add(out, sprites::sprite{"pixel", f.v19, custom::vec_to_deg(f.v22), vec2{3.f, lk.arm_low_length + lk.arm_low_extra}, vec2{0.5f, 0.f}, black});
            sprites::add(out, sprites::sprite{"pixel", f.v21, custom::vec_to_deg(f.v23), vec2{2.f, lk.arm_high_length}, vec2{0.5f, 0.f}, black});
            vec2 const v24 = f.v19 - f.v4 * 20.f;
            vec2 const v25 = f.v21 + f.v22 * lk.arm_low_extra;
            sprites::add(out, sprites::sprite{"pixel", v24, custom::vec_to_deg(custom::dir_vec(v24, v25)), vec2{1.f, rw::distance(v24, v25)}, vec2{0.5f, 0.f}, black});
            vec2 const v27 = f.v21 + f.v23 * lk.arm_high_length;
            sprites::add(out, sprites::sprite{"pixel", v25, custom::vec_to_deg(custom::dir_vec(v25, v27)), vec2{1.f, rw::distance(v25, v27)}, vec2{0.5f, 0.f}, black});
            float const fl = mathf::lerp(g.flicker[0][1], g.flicker[0][0], alpha);
            vec3 col = mix(black, lcolor, 0.05f + fl * 0.95f);
            if (on)
                col = mix(lcolor, vec3{1.f, 1.f, 1.f}, 0.95f);
            sprites::add(out, sprites::sprite{"Circle20", f.v20, custom::vec_to_deg(f.v23), vec2{0.4f, 0.8f}, vec2{0.5f, 0.f}, col});
            // (the two light sprites are the Water container's: light_of)
        }
        // the propeller blades round the cone
        for (uint32_t n = 0; n < lk.number_of_props && n < angler::max_props; ++n) {
            float const ph = g.prop_spin + 6.2831855f * float(n) / float(lk.number_of_props);
            float const num8 = std::sin(ph);
            float const num9 = std::cos(ph);
            vec2 const at = f.a2 + f.a * num8 * lk.cone_rad;
            float const num10 = 0.7f + (1.f + num9) * 0.5f * 0.3f;
            sprites::add(out, sprites::sprite{"Circle20", at, custom::vec_to_deg(f.v5), vec2{std::fabs(num8) * 2.f * num10, num10},
                vec2{(1.f + num8) * 0.5f, 0.5f}, black});
        }
    }
}
