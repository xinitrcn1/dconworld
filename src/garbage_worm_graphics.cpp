#include <cmath>

#include "garbage_worm_graphics.hpp"
#include "creature.hpp"
#include "garbage_worm.hpp"
#include "garbage_worm_types.hpp"

namespace garbage_worm_graphics {
    using rw::vec2;
    using rw::vec3;
    using sprites::sprite;
    using garbage_worm::graphics_state;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        graphics_state& gs(sys::game_state& state, dcon::creature_id c) {
            return state.world.garbage_worm_data_get_graphics(creature::garbage_worm_data(state.world, c));
        }
        graphics_state const& gs(sys::game_state const& state, dcon::creature_id c) {
            return state.world.garbage_worm_data_get_graphics(creature::garbage_worm_data(state.world, c));
        }
        garbage_worm::tentacle_state const& tentacle(sys::game_state const& state, dcon::creature_id c) {
            return state.world.garbage_worm_data_get_tentacle(creature::garbage_worm_data(state.world, c));
        }
        // TentacleChunk.stretchedRad (the tentacle's stretchAndSqueeze 0.1)
        float stretched_rad(garbage_worm::tentacle_chunk const& ch) {
            return ch.rad * mathf::clamp(mathf::pow(ch.stretched_fac, 0.1f), 0.5f, 1.5f);
        }
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        auto& g = gs(state, c);
        float const e = state.world.creature_get_payload(c).garbage_worm.extended;
        g.last_extended = e;
        g.extended = e;
        for (auto& s : g.swallow)
            s = 0.f;
    }

    void update(sys::game_state& state, dcon::creature_id c, world::room_view const&) {
        auto& w = state.world;
        auto& g = gs(state, c);
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        uint32_t const n = tentacle(state, c).count;
        if (creature::consious(state, c) && n > 0) {
            int32_t const attack = garbage_worm::attack_counter(state, c);
            float const stress = garbage_worm::stress(state, c);
            if (attack < 20) {
                g.number_of_waves_on_body = mathf::lerp(g.number_of_waves_on_body, mathf::lerp(1.8f, 3.4f, stress), 0.1f);
                g.sin_speed = mathf::lerp(g.sin_speed, mathf::lerp(0.016666668f, 0.05f, stress), 0.05f);
            } else {
                g.number_of_waves_on_body = mathf::lerp(g.number_of_waves_on_body, 5.f, 0.01f);
                g.sin_speed = mathf::lerp(g.sin_speed, 0.05f, 0.1f);
            }
            g.sin_wave += g.sin_speed;
            if (g.sin_wave > 1.f)
                g.sin_wave -= 1.f;
            if (attack > 40 && attack < 190 && rng.value() < 0.033333335f)
                g.swallow[n - 1] = mathf::pow(rng.value(), 0.5f);
            if (rng.value() < 0.33333334f)
                for (uint32_t i = 0; i + 1 < n; ++i)
                    g.swallow[i] = mathf::lerp(g.swallow[i], g.swallow[i + 1], 0.7f);
            g.swallow[n - 1] = mathf::lerp(g.swallow[n - 1], 0.f, 0.7f);
        }
        g.last_extended = g.extended;
        g.extended = w.creature_get_payload(c).garbage_worm.extended;
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip) {
        auto const& w = state.world;
        auto const& g = gs(state, c);
        auto const& t = tentacle(state, c);
        auto const& p = w.creature_get_payload(c).garbage_worm;
        uint32_t const n = t.count;
        if (n < 2)
            return;
        float const body_size = p.body_size;
        // ApplyPalette: the body and the head in the palette's black; the
        // eyes white, red when angry
        vec3 const black = pal.black();
        vec3 const eye_color = garbage_worm::show_as_angry(state, c) ? vec3{1.f, 0.f, 0.f} : vec3{1.f, 1.f, 1.f};
        float const head_scale = mathf::lerp(body_size, 1.f, 0.5f);

        auto const root = creature::chunk(state, c, 1);
        vec2 const root_pos = w.body_chunk_get_pos(root);
        float const num = mathf::lerp(g.last_extended, g.extended, alpha);
        vec2 vector = root_pos + vec2{0.f, -30.f - 100.f * (1.f - num)};
        float num2 = 4.f;
        constexpr uint32_t max_v = garbage_worm::max_tentacle_chunks * 4;
        vec2 vs[max_v];
        vec2 head_pos{0.f, 0.f};
        float head_rot = 0.f;
        vec2 eye_pos[2]{{0.f, 0.f}, {0.f, 0.f}};
        for (uint32_t i = 0; i < n; ++i) {
            vec2 v2 = rw::lerp(t.c[i].last_pos, t.c[i].pos, alpha);
            float const num3 = float(i) / float(n - 1);
            float num4 = mathf::pow(mathf::max(1.f - num3 - num, 0.f), 1.5f);
            if (num < 0.2f)
                num4 = mathf::min(1.f, num4 + mathf::inverse_lerp(0.2f, 0.f, num));
            v2 = rw::lerp(v2, root_pos, num4) + vec2{0.f, -100.f * mathf::pow(num4, 0.5f)};
            float const d = std::sin((mathf::lerp(g.sin_wave - g.sin_speed, g.sin_wave, alpha) + num3 * g.number_of_waves_on_body) * mathf::pi * 2.f);
            v2 += custom::perpendicular_vector(rw::normalized(v2 - vector)) * d * 11.f * mathf::pow(mathf::max(0.f, std::sin(num3 * mathf::pi)), 0.75f) * num;
            vec2 const normalized = rw::normalized(v2 - vector);
            vec2 const v3 = custom::perpendicular_vector(normalized);
            if (i == n - 1) {
                head_pos = v2;
                head_rot = custom::aim(-normalized, normalized);
                float num5 = std::cos(head_rot / 360.f * 2.f * mathf::pi);
                num5 = mathf::pow(std::fabs(num5), 0.25f) * mathf::sign(num5);
                // num6: which eye sprite (0 or 3) gets which side
                uint32_t const num6 = (num5 * mathf::sign(normalized.x) > 0.f) ? 3 : 0;
                vec2 const fwd = normalized * 5.f * body_size;
                vec2 const side = v3 * 3.f * mathf::lerp(body_size, 1.f, 0.75f) * num5;
                // sprites[3 - num6] on the + side, sprites[num6] on the - side
                eye_pos[num6 == 3 ? 0 : 1] = v2 + fwd + side;
                eye_pos[num6 == 3 ? 1 : 0] = v2 + fwd - side;
            }
            float const d2 = rw::distance(v2, vector) / 7.f;
            float const num7 = stretched_rad(t.c[i]) + g.swallow[i] * 5.f;
            vs[i * 4] = vector - v3 * (num7 + num2) * 0.5f + normalized * d2;
            vs[i * 4 + 1] = vector + v3 * (num7 + num2) * 0.5f + normalized * d2;
            vs[i * 4 + 2] = v2 - v3 * num7 - normalized * d2;
            vs[i * 4 + 3] = v2 + v3 * num7 - normalized * d2;
            num2 = num7;
            vector = v2;
        }
        // sprites[0]: an eye, behind the body
        sprites::add(out, sprite{"WormEye", eye_pos[0], 0.f, vec2{1.f, 1.f}, vec2{0.5f, 0.5f}, eye_color});
        // sprites[1]: the body
        sprites::long_mesh(out, strip, vs, size_t(n) * 4, black);
        // sprites[2]: the head
        sprites::add(out, sprite{"WormHead", head_pos, head_rot, vec2{head_scale, head_scale}, vec2{0.5f, 0.5f}, black});
        // sprites[3]: the other eye
        sprites::add(out, sprite{"WormEye", eye_pos[1], 0.f, vec2{1.f, 1.f}, vec2{0.5f, 0.5f}, eye_color});
    }
}
