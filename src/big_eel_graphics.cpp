#include <cmath>
#include <string_view>

#include "big_eel_graphics.hpp"
#include "big_eel.hpp"
#include "body_parts.hpp"
#include "creature.hpp"
#include "particles.hpp"
#include "physics.hpp"

namespace big_eel_graphics {
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using world::room_view;
    using sprites::sprite;
    using big_eel::chunk_count;
    using big_eel::graphics_layout;
    using big_eel::graphics_state;
    using big_eel::tail_segments;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        // TailSegment(rad, 30, prev, surface friction 0.5, air friction 1,
        // affectPrevious 0.1, pullInPreviousPosition) for the tail, (rad,
        // length / segments, prev, 0.5, 1, 0.2, pull) for the fins
        constexpr float tail_connection_rad = 30.f;
        constexpr float surface_friction = 0.5f;
        constexpr float tail_affect_previous = 0.1f;
        constexpr float fin_affect_previous = 0.2f;
        constexpr int32_t total_segments = int32_t(chunk_count + tail_segments);
        // the JaggedCircle shader's ragged rim at the chunk discs' alpha 0.35
        // (the long legs' 0.25 is drawn 0.16 deep)
        constexpr float chunk_jag = 0.224f;
        // RainWorld.SaturatedGold
        constexpr vec3 saturated_gold{0.529f * 2.f, 0.365f * 2.f, 0.184f * 2.f};
        constexpr vec3 white{1.f, 1.f, 1.f};
        constexpr vec3 water_color{0.62f, 0.72f, 0.85f};

        graphics_state& gs(sys::game_state& state, dcon::creature_id c) {
            auto const d = dcon::big_eel_data_id{dcon::big_eel_data_id::value_base_t(state.world.creature_get_payload(c).big_eel.data)};
            return state.world.big_eel_data_get_graphics(d);
        }
        graphics_state const& gs(sys::game_state const& state, dcon::creature_id c) {
            auto const d = dcon::big_eel_data_id{dcon::big_eel_data_id::value_base_t(state.world.creature_get_payload(c).big_eel.data)};
            return state.world.big_eel_data_get_graphics(d);
        }
        graphics_layout const& layout(sys::game_state const& state, dcon::creature_id c) {
            auto const d = dcon::big_eel_data_id{dcon::big_eel_data_id::value_base_t(state.world.creature_get_payload(c).big_eel.data)};
            return state.world.big_eel_data_get_layout(d);
        }

        vec3 lerp3(vec3 a, vec3 b, float t) {
            t = mathf::clamp01(t);
            return a + (b - a) * t;
        }
        vec3 hsl_rgb(vec3 hsl) {
            return custom::hsl2rgb(hsl.x, hsl.y, hsl.z);
        }

        // BigEelGraphics.FinContour
        float fin_contour(float f) {
            if (f < 0.33333334f)
                return 0.3f + 0.7f * custom::s_curve(mathf::inverse_lerp(0.f, 0.33333334f, f), 0.5f);
            return mathf::inverse_lerp(1.f, 0.33333334f, f);
        }

        // the body as BodyMeshPos and the rest see it at a time stacker
        struct body_view {
            sys::game_state const& state;
            dcon::creature_id c;
            graphics_state const& g;
            float ts;
        };
        vec2 chunk_at(body_view const& b, int32_t i) {
            auto const k = big_eel::chunk(b.state, b.c, uint32_t(i));
            return rw::lerp(b.state.world.body_chunk_get_last_pos(k), b.state.world.body_chunk_get_pos(k), b.ts);
        }
        float chunk_rad(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_rad(big_eel::chunk(state, c, i));
        }
        // BodyMeshPos: the chunks, then the tail with its wave
        vec2 body_mesh_pos(body_view const& b, int32_t index) {
            index = custom::int_clamp(index, 0, total_segments - 1);
            if (index < int32_t(chunk_count))
                return chunk_at(b, index);
            int32_t const i = index - int32_t(chunk_count);
            auto const& s = b.g.tail[i];
            vec2 v = rw::lerp(s.last_pos, s.pos, b.ts);
            if (i > 0) {
                auto const& p = b.g.tail[i - 1];
                float const swim = mathf::lerp(b.g.last_tail_swim, b.g.tail_swim, b.ts);
                v += custom::perpendicular_vector(rw::normalized(v - rw::lerp(p.last_pos, p.pos, b.ts)))
                    * std::sin((swim + float(i) / 6.f) * mathf::pi * 2.f) * 20.f * std::sin(float(i) / float(tail_segments) * mathf::pi);
            }
            return v;
        }
        // BodyMeshRad
        float body_mesh_rad(body_view const& b, int32_t index) {
            index = custom::int_clamp(index, 0, total_segments - 1);
            if (index < int32_t(chunk_count))
                return chunk_rad(b.state, b.c, uint32_t(index));
            return b.g.tail[index - int32_t(chunk_count)].rad;
        }
        // SmoothedBodyMeshRad
        float smoothed_rad(body_view const& b, int32_t index) {
            return (body_mesh_rad(b, index - 2) + body_mesh_rad(b, index) + body_mesh_rad(b, index + 2)) / 3.f;
        }
        // SmoothedBodyMeshPos: averaged around the joint of the body and the
        // tail; the vibrating segment shakes
        vec2 smoothed_pos(body_view const& b, int32_t index, rw::rng& rng) {
            if (index == b.g.vibrate_segment)
                return body_mesh_pos(b, index) + custom::rnv(rng) * mathf::lerp(body_mesh_rad(b, index) / 3.f, 10.f, 0.5f);
            float const num = mathf::inverse_lerp(index < int32_t(chunk_count) ? 4.f : 20.f, 1.f, float(std::abs(index - int32_t(chunk_count))));
            if (num == 0.f)
                return body_mesh_pos(b, index);
            vec2 v = body_mesh_pos(b, index);
            for (int32_t i = 1; i < 5; ++i)
                v += body_mesh_pos(b, index - i) + body_mesh_pos(b, index + i);
            v /= 9.f;
            return rw::lerp(body_mesh_pos(b, index), v, num);
        }
        // BodyMeshDir
        vec2 mesh_dir(body_view const& b, int32_t index, rw::rng& rng) {
            vec2 const p = smoothed_pos(b, index, rng);
            return rw::normalized(p - smoothed_pos(b, index + 1, rng));
        }
        // SidePos
        vec2 side_pos(body_view const& b, int32_t segment, int32_t side, float rad_subtract, rw::rng& rng) {
            vec2 const dir = mesh_dir(b, segment, rng);
            return smoothed_pos(b, segment, rng) + custom::perpendicular_vector(dir) * (side == 0 ? -1.f : 1.f) * (body_mesh_rad(b, segment) - rad_subtract);
        }
        // FinConnectionIndex, FinConnectionPos
        int32_t fin_connection_index(int32_t fin) {
            return fin * 2 + (fin == 0 ? 1 : 0);
        }
        vec2 fin_connection_pos(body_view const& b, int32_t fin, int32_t side, rw::rng& rng) {
            int32_t const i = fin_connection_index(fin);
            return rw::lerp(side_pos(b, i, side, 10.f, rng), side_pos(b, i + 1, side, 10.f, rng), 0.3f);
        }

        // TailSegment.Update (pullInPreviousPosition): after the segment
        // before it, or the point
        void segment_update(player::body_part& bp, player::body_part* prev, vec2 point, float connection_rad, float affect_previous, room_view const& room) {
            bp.last_pos = bp.pos;
            bp.pos += bp.vel;
            // (airFriction 1)
            bp.stretched = 1.f;
            if (prev) {
                if (!custom::dist_less(bp.pos, prev->pos, connection_rad)) {
                    float const num = rw::distance(bp.pos, prev->pos);
                    vec2 const a = custom::dir_vec(bp.pos, prev->pos);
                    bp.pos -= a * (connection_rad - num) * (1.f - affect_previous);
                    bp.vel -= a * (connection_rad - num) * (1.f - affect_previous);
                    prev->pos += a * (connection_rad - num) * affect_previous;
                    prev->vel += a * (connection_rad - num) * affect_previous;
                    bp.stretched = mathf::clamp((connection_rad / (num * 0.5f) + 2.f) / 3.f, 0.2f, 1.f);
                }
            } else if (!custom::dist_less(bp.pos, point, connection_rad)) {
                float const num = rw::distance(bp.pos, point);
                vec2 const a = custom::dir_vec(bp.pos, point);
                bp.pos -= a * (connection_rad - num);
                bp.vel -= a * (connection_rad - num);
                bp.stretched = mathf::clamp((connection_rad / (num * 0.5f) + 2.f) / 3.f, 0.2f, 1.f);
            }
            body_parts::push_out_of_terrain(bp, room, surface_friction);
        }
    }

    graphics_layout make_layout(int32_t fins_seed) {
        graphics_layout L;
        rw::unity_random r{fins_seed};
        // the tail's TailSegments' Reset rolls
        for (uint32_t i = 0; i < tail_segments; ++i)
            r.value();
        L.scale_start = r.range(1, 6);
        int32_t const fins = r.range(6, 8);
        L.fins = uint8_t(fins);
        float num = mathf::lerp(6.f, 8.f, r.value());
        if (fins > 6)
            num *= 0.8f;
        for (int32_t j = 0; j < fins; ++j) {
            L.fin_length[j] = 100.f + 200.f * std::sin(mathf::pow(float(j) / float(fins - 1), 0.5f) * mathf::pi);
            int32_t const num2 = std::min(int32_t(std::floor(L.fin_length[j] / 20.f)) + 1, int32_t(big_eel::max_fin_segments));
            float const num3 = num + num * std::sin(mathf::pow(float(j) / 5.f, 0.8f) * mathf::pi);
            L.fin_segments[j] = uint8_t(num2);
            L.fin_connection_rad[j] = L.fin_length[j] / float(num2);
            for (int32_t k = 0; k < 2; ++k) {
                L.fin_roll[j] = r.value();
                for (int32_t l = 0; l < num2; ++l) {
                    L.fin_rad[j][l] = 1.f + fin_contour(float(l) / float(num2 - 1)) * num3;
                    // (its Reset roll)
                    r.value();
                }
            }
        }
        for (uint32_t m = 0; m < chunk_count; ++m) {
            L.chunk_rotation[m] = r.value() * 360.f;
            L.chunk_flip[m] = r.value() < 0.5f ? -1.f : 1.f;
        }
        L.scales = uint8_t(r.range(20, 40));
        L.scale_size = mathf::lerp(0.5f, 1.2f, mathf::pow(r.value(), 0.5f));
        for (uint32_t n = 0; n < big_eel::eye_count; ++n) {
            vec2 const v = custom::rnv(r);
            vec2 e = v * mathf::pow(r.value(), 0.6f);
            if (e.y > 0.7f)
                e.y = mathf::lerp(e.y, 0.7f, 0.3f);
            L.eyes[n] = e;
            L.eye_scales[n][0] = mathf::lerp(0.5f, 1.5f, mathf::pow(r.value(), custom::lerp_map(e.y, -1.f, 0.7f, 1.5f, 0.2f)));
            L.eye_scales[n][1] = mathf::lerp(0.1f, L.eye_scales[n][0] * 0.9f, mathf::pow(r.value(), custom::lerp_map(e.y, -1.f, 0.7f, 2.f, 0.1f)));
            L.eye_scales[n][2] = mathf::pow(r.value(), custom::lerp_map(e.y, -1.f, 0.7f, 2.f, 0.1f));
        }
        return L;
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        auto& g = gs(state, c);
        auto const& L = layout(state, c);
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        // BodyPart.Reset: every part at the first chunk, a pixel off
        vec2 const at = state.world.body_chunk_get_pos(big_eel::chunk(state, c, 0));
        auto const reset = [&](player::body_part& bp, float rad) {
            bp.pos = at + custom::deg_to_vec(rng.value() * 360.f);
            bp.last_pos = bp.pos;
            bp.vel = vec2{0.f, 0.f};
            bp.rad = rad;
            bp.stretched = 1.f;
        };
        float const last_rad = chunk_rad(state, c, chunk_count - 1);
        for (uint32_t i = 0; i < tail_segments; ++i)
            reset(g.tail[i], mathf::lerp(last_rad, 1.f, float(i) / float(tail_segments - 1)));
        for (uint32_t j = 0; j < L.fins && j < big_eel::max_fins; ++j)
            for (uint32_t k = 0; k < 2; ++k)
                for (uint32_t l = 0; l < L.fin_segments[j]; ++l)
                    reset(g.fins[j][k][l], L.fin_rad[j][l]);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        if (big_eel::hidden(state, c))
            return;
        auto const& w = state.world;
        auto& g = gs(state, c);
        auto const& L = layout(state, c);
        auto const& x = big_eel::state_of(state, c);
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        body_view const b{state, c, g, 1.f};
        // (no Leviathan_Hydraulics, Charged_Jaws or Fin loops: no samples)
        vec2 const root = w.body_chunk_get_pos(big_eel::chunk(state, c, chunk_count - 1));
        g.last_jaw_charge = g.jaw_charge;
        g.jaw_charge = x.jaw_charge;
        if (g.jaw_charge == 0.f)
            g.last_jaw_charge = 0.f;
        if (x.snap_frame) {
            g.vibrate_segment = 0;
            for (int32_t i = 0; i < int32_t(L.fins); ++i) {
                vec2 const a = mesh_dir(b, fin_connection_index(i), rng);
                int32_t const n = L.fin_segments[i];
                for (int32_t j = 0; j < 2; ++j)
                    for (int32_t k = 0; k < n; ++k)
                        g.fins[i][j][k].vel += a * std::sin(float(k) / (float(n) - 1.f) * mathf::pi) * 30.f;
            }
            vec2 const s0 = smoothed_pos(b, 0, rng);
            vec2 const d0 = mesh_dir(b, 0, rng);
            vec2 const a2 = s0 + d0 * 40.f;
            vec2 const b2 = s0 + d0 * 200.f;
            for (int32_t l = 0; l < 20; ++l) {
                vec2 const p = rw::lerp(a2, b2, mathf::pow(rng.value(), 1.6f));
                if (room.point_submerged(p)) {
                    vec2 const v = custom::rnv(rng) * mathf::pow(rng.value(), 2.1f) * 30.f;
                    particles::spawn(state, room.id, particles::kind::bubble, p, v, 1.2f + 2.f * rng.value(), 1.5f + 1.5f * rng.value(), water_color, 0.8f);
                } else {
                    vec2 const v = custom::rnv(rng) * rng.value() * 60.f;
                    particles::spawn(state, room.id, particles::kind::drop, p, v, 0.8f + 0.8f * rng.value(), 2.f, water_color, 0.85f);
                }
            }
        } else if (g.vibrate_segment < 1000000) {
            ++g.vibrate_segment;
        }
        g.last_tail_swim = g.tail_swim;
        // (no Remix: it swims on stunned or dead)
        g.tail_swim -= 1.f / mathf::lerp(20.f, 10.f, x.swim_speed);
        // the tail, after the last chunk
        for (uint32_t m = 0; m < tail_segments; ++m) {
            auto& s = g.tail[m];
            segment_update(s, m > 0 ? &g.tail[m - 1] : nullptr, root, tail_connection_rad, tail_affect_previous, room);
            float const t = float(m) / float(tail_segments - 1);
            if (room.point_submerged(s.pos)) {
                s.vel *= mathf::lerp(0.98f, 0.1f, mathf::pow(mathf::inverse_lerp(2.f, 20.f, rw::magnitude(s.vel)), mathf::lerp(5.f, 0.5f, t)));
                for (uint32_t n = 2; n < 8; ++n) {
                    vec2 const p2 = m < n ? w.body_chunk_get_pos(big_eel::chunk(state, c, chunk_count - n)) : g.tail[m - n].pos;
                    s.vel += custom::dir_vec(p2, s.pos) * mathf::lerp(1.4f, 0.1f, t);
                }
            } else {
                s.vel *= mathf::lerp(0.98f, 0.5f, t);
                s.vel.y -= 0.9f;
            }
            if (!custom::dist_less(s.pos, root, 30.f * float(m + 1)))
                s.pos = root + custom::dir_vec(root, s.pos) * 30.f * float(m + 1);
        }
        // the fins, waving with the swim
        for (int32_t num = 0; num < int32_t(L.fins); ++num) {
            int32_t const n = L.fin_segments[num];
            int32_t const fi = fin_connection_index(num);
            for (int32_t side = 0; side < 2; ++side) {
                vec2 const vector = fin_connection_pos(b, num, side, rng);
                vec2 const dir = mesh_dir(b, fi, rng);
                if (fi == g.vibrate_segment && n > 1)
                    g.fins[num][side][1].vel += dir * 20.f;
                for (int32_t s = 0; s < n; ++s) {
                    auto& seg = g.fins[num][side][s];
                    float const num4 = float(s) / float(n - 1);
                    segment_update(seg, s > 0 ? &g.fins[num][side][s - 1] : nullptr, vector, L.fin_connection_rad[num], fin_affect_previous, room);
                    vec2 a3 = vector + custom::perpendicular_vector(dir) * (side == 0 ? -1.f : 1.f) * 20.f * float(s + 1);
                    a3 += dir * std::sin(g.tail_swim * 0.25f * mathf::pi) * (side == num % 2 ? -1.f : 1.f) * mathf::lerp(10.f, 17.f, x.swim_speed) * float(s + 1);
                    float const max_length = mathf::inverse_lerp(3.f, 0.f, float(s)) * 1.1f + mathf::pow(1.f - num4, 2.5f) * 1.8f;
                    seg.vel += rw::clamp_magnitude(a3 - seg.pos, max_length);
                    if (room.point_submerged(seg.pos)) {
                        seg.vel *= mathf::lerp(0.98f, 0.1f, mathf::pow(mathf::inverse_lerp(2.f, 20.f, rw::magnitude(seg.vel)), mathf::lerp(5.f, 0.5f, num4)));
                        vec2 const p = s == 0 ? vector : g.fins[num][side][s - 1].pos;
                        seg.vel += custom::dir_vec(p, seg.pos) * mathf::lerp(1.8f, 0.3f, num4);
                    } else {
                        seg.vel *= 0.9f;
                        seg.vel.y -= 0.9f;
                    }
                    if (!custom::dist_less(seg.pos, vector, 20.f * float(s + 1)))
                        seg.pos = vector + custom::dir_vec(vector, seg.pos) * 20.f * float(s + 1);
                }
            }
        }
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>&, std::vector<sprites::rope_point>& ropes) {
        if (big_eel::hidden(state, c))
            return;
        auto const& g = gs(state, c);
        auto const& L = layout(state, c);
        auto const& x = big_eel::state_of(state, c);
        auto const& iv = big_eel::ivars(state, c);
        bool const voided = big_eel::voided(state, c);
        bool const albino = x.albino != 0;
        float const t = alpha;
        // the draw's rolls (the vibrating segment, the jaws' shake)
        auto rng = rw::random(state.seed, state.tick, rw::ids(uint32_t(c.value), 0xEE1u));
        body_view const b{state, c, g, t};

        // ApplyPalette: the head, the body (LeviathanColorA) and the
        // palette's (3, 7) (LeviathanColorB)
        vec3 const black = pal.black();
        vec3 color = black, color2 = black, color3 = pal.texel(3, 7);
        if (voided) {
            color = lerp3(black, saturated_gold, 0.6f);
            color2 = lerp3(color2, saturated_gold, 0.3f);
            color3 = lerp3(color3, lerp3(color2, saturated_gold, 0.5f), 0.7f);
        } else if (albino) {
            color = lerp3(black, vec3{0.9f, 0.4f, 0.4f}, 0.6f);
            color2 = lerp3(lerp3(color2, white, 0.9f), color, 0.15f);
            color3 = lerp3(color3, lerp3(color2, vec3{1.f, 0.f, 0.f}, 0.5f), 0.7f);
        }
        vec3 const beak_b = lerp3(black, white, 0.07f);

        // the jaws: sprites 0..19 (BeakSprite(side, module) = module * 2 +
        // side, BeakArmSprite(module, arm, side) = 4 + 8 * arm + side * 4 + module)
        sprite head_sprites[20];
        vec2 const vector3 = chunk_at(b, 0);
        vec2 const vector4 = custom::dir_vec(chunk_at(b, 1), vector3);
        vec2 const a2 = custom::perpendicular_vector(vector4);
        float const num3 = mathf::lerp(g.last_jaw_charge, g.jaw_charge, t);
        float const num4 = mathf::min(mathf::inverse_lerp(0.f, 0.3f, num3), mathf::inverse_lerp(1.f, 0.7f, num3));
        float const tt = num3 > 0.35f ? mathf::inverse_lerp(1.f, 0.65f, num3) : 0.f;
        float const num5 = num4 * mathf::inverse_lerp(0.7f, 0.4f, num3);
        float const num6 = std::sin(x.jaw_charge_fatigue * mathf::pi) * num5;
        static constexpr std::string_view jaw_elements[2][2] = {{"EelJaw2A", "EelJaw2B"}, {"EelJaw1A", "EelJaw1B"}};
        for (int32_t k = 0; k < 2; ++k) {
            float const num7 = k == 0 ? -1.f : 1.f;
            vec2 vector8 = vector3 + vector4 * 60.f * num5 + custom::rnv(rng) * num6 * 2.f;
            vector8 += a2 * num7 * (mathf::lerp(30.f, 12.5f + x.beak_gap / 20.f, tt) + 10.f * std::sin(mathf::pow(num4, 2.f) * mathf::pi));
            float const num9 = custom::vec_to_deg(vector4) + std::sin(num4 * mathf::pi) * (num3 < 0.35f ? -20.f : -10.f) * num7
                + mathf::lerp(-2.f, 2.f, rng.value()) * num6;
            for (int32_t arm = 0; arm < 2; ++arm) {
                vec3 const beak_color = arm == 1 ? beak_b : black;
                head_sprites[arm * 2 + k] = sprite{jaw_elements[k][arm], vector8, num9, vec2{1.f, 1.f}, vec2{k == 1 ? 0.25f : 0.75f, 0.5f}, beak_color};
                float d3;
                if (num3 < 0.35f)
                    d3 = arm == 0 ? mathf::lerp(15.f, 43.f, mathf::inverse_lerp(0.35f, 0.15f, num3)) : 30.f * mathf::pow(mathf::inverse_lerp(0.f, 0.5f, num4), 0.2f);
                else
                    d3 = arm == 0 ? 43.f * std::fabs(std::cos(mathf::inverse_lerp(1.f, 0.4f, num3) * mathf::pi)) : 30.f * mathf::inverse_lerp(1.f, 0.4f, num3);
                vec2 const vector9 = vector3 + a2 * num7 * d3 - vector4 * (arm == 0 ? 22.f : 30.f);
                vec2 const vector10 = vector8 + custom::deg_to_vec(num9) * (arm == 0 ? -12.f : 10.f) + custom::perpendicular_vector(custom::deg_to_vec(num9)) * 15.f * num7;
                vec2 const vector11 = custom::inverse_kinematic(vector9, vector10, 55.f, 45.f, -num7);
                int32_t const base = 4 + 8 * arm + k * 4;
                // module 0: the rod from the elbow to the base, 1 the elbow,
                // 2 the rod from the jaw to the elbow, 3 the joint at the jaw
                head_sprites[base + 0] = sprite{"pixel", vector11, custom::aim(vector11, vector9), vec2{5.f, 55.f}, vec2{0.5f, 0.f}, black};
                head_sprites[base + 1] = sprite{"Circle20", vector11, 0.f, vec2{0.5f, 0.5f}, vec2{0.5f, 0.5f}, black};
                head_sprites[base + 2] = sprite{"pixel", vector10, custom::aim(vector10, vector11), vec2{5.f, 55.f}, vec2{0.5f, 0.f}, black};
                head_sprites[base + 3] = sprite{"Circle20", vector10, 0.f, vec2{0.5f, 0.5f}, vec2{0.5f, 0.5f}, black};
            }
        }
        for (auto const& s : head_sprites)
            sprites::add(out, s);

        // BodyChunksSprite: the JaggedCircle discs (scaleX's flip folded
        // into the rim's rolls)
        for (int32_t i = 0; i < int32_t(chunk_count); ++i) {
            vec2 const p = smoothed_pos(b, i, rng);
            float const rot = custom::vec_to_deg(mesh_dir(b, i, rng)) + L.chunk_rotation[i];
            uint32_t const seed = rw::keyed::mix(uint32_t(iv.fins_seed) ^ uint32_t(i) * 2654435761u ^ (L.chunk_flip[i] < 0.f ? 0x9E3779B9u : 0u));
            sprites::add_circle(out, p, chunk_rad(state, c, uint32_t(i)) * 1.06f, lerp3(color, color2, float(i) / 2.f), rot, chunk_jag, seed);
        }

        // MeshSprite: the long mesh along the body and tail, in the
        // palette's black (the albino's / voided colours as ApplyPalette).
        // The EelBody shader isn't ported, so its pattern (the
        // verticeColors from patternColorA to B) isn't drawn
        {
            size_t const first = ropes.size();
            for (int32_t j = 0; j < total_segments; ++j) {
                vec2 const p = smoothed_pos(b, j, rng);
                float num2 = smoothed_rad(b, j) + custom::lerp_map(float(std::abs(j - 16)), 1.f, 6.f, 20.f, 5.f) * mathf::inverse_lerp(20.f, 2.f, float(j));
                if (j == total_segments - 1)
                    num2 = 0.5f;
                vec3 const col = lerp3(color, color2, mathf::inverse_lerp(0.f, 4.f, float(j)));
                ropes.push_back(sprites::rope_point{p, num2, vec4{col.x, col.y, col.z, 1.f}});
            }
            sprites::add_rope(out, ropes, first);
        }

        // FinSprite(fin, side): the fins' long meshes (the EelFin shader
        // isn't ported: in the body's colour)
        for (int32_t l = 0; l < int32_t(L.fins); ++l)
            for (int32_t k = 0; k < 2; ++k) {
                size_t const first = ropes.size();
                vec2 const conn = fin_connection_pos(b, l, k, rng);
                int32_t const n = L.fin_segments[l];
                float const r0 = n > 0 ? g.fins[l][k][0].rad : 1.f;
                ropes.push_back(sprites::rope_point{conn, (20.f + r0) * 0.5f, vec4{color2.x, color2.y, color2.z, 1.f}});
                for (int32_t m = 0; m < n; ++m) {
                    auto const& s = g.fins[l][k][m];
                    vec2 const p = rw::lerp(s.last_pos, s.pos, t);
                    float const r = m == n - 1 ? 0.5f : s.rad;
                    ropes.push_back(sprites::rope_point{p, r, vec4{color2.x, color2.y, color2.z, 1.f}});
                }
                sprites::add_rope(out, ropes, first);
            }

        // ScaleSprite(scale, side): LizardScaleA4 along the sides
        float const swim = mathf::lerp(g.last_tail_swim, g.tail_swim, t);
        uint32_t const scales = L.scales < big_eel::max_scales ? L.scales : big_eel::max_scales;
        for (uint32_t n = 0; n < scales; ++n)
            for (int32_t k = 0; k < 2; ++k) {
                float const num7 = k == 0 ? -1.f : 1.f;
                float const num8 = float(n) / float(scales - 1);
                int32_t const s = L.scale_start + int32_t(n);
                vec2 const vector6 = side_pos(b, s, k, 3.f, rng);
                vec2 const normalized3 = rw::normalized(mesh_dir(b, s - 2, rng) + mesh_dir(b, s, rng) + mesh_dir(b, s + 2, rng));
                vec2 const out_dir = rw::normalized(custom::perpendicular_vector(normalized3) * num7
                    + normalized3 * (mathf::lerp(0.5f, -1.3f, num8) + 2.f * std::sin((num8 * 2.f + swim * 0.13f) * mathf::pi * 2.f))
                        * mathf::pow(std::sin(num8 * mathf::pi), 1.f - 0.7f * num8));
                vec2 const vector7 = vector6 + out_dir * mathf::lerp(5.f, 42.f, std::sin(num8 * mathf::pi)) * L.scale_size;
                sprites::add(out, sprite{"LizardScaleA4", vector6, custom::aim(vector6, vector7), vec2{num7 < 0.f ? 1.f : -1.f, rw::distance(vector6, vector7) / 37.f},
                                      vec2{0.5f, 0.f}, color2});
            }

        // EyeSprite(eye, part): the eyes all over the head, the sockets first
        vec2 eye_pos[big_eel::eye_count];
        float eye_rot[big_eel::eye_count];
        for (uint32_t n = 0; n < big_eel::eye_count; ++n) {
            vec2 const e = L.eyes[n];
            float const ey = e.y * (e.y > 0.f ? custom::lerp_map(std::fabs(e.x) * num4, 0.f, 1.f, 1.f, 1.5f) : 1.f);
            eye_pos[n] = vector3 + body_parts::rotate_around_origo(vec2{e.x, ey}, custom::vec_to_deg(vector4)) * 45.f;
            eye_rot[n] = custom::aim(vector3, eye_pos[n] + vector4 * mathf::lerp(40.f, 80.f, num4));
        }
        for (uint32_t n = 0; n < big_eel::eye_count; ++n) {
            float const sc = L.eye_scales[n][0];
            sprites::add(out, sprite{"Cicada8head", eye_pos[n], eye_rot[n], vec2{sc, sc}, vec2{0.5f, 0.7f}, color});
        }
        for (uint32_t n = 0; n < big_eel::eye_count; ++n) {
            float const sc = L.eye_scales[n][1];
            vec3 col;
            if (voided)
                col = white;
            else if (albino)
                col = lerp3(hsl_rgb(iv.pattern_b), black, L.eye_scales[n][2]);
            else
                col = hsl_rgb(big_eel::hsl_lerp(iv.pattern_a, iv.pattern_b, L.eye_scales[n][2]));
            sprites::add(out, sprite{"Cicada0shield", eye_pos[n], eye_rot[n], vec2{sc, sc}, vec2{0.5f, 0.4f}, col});
        }
    }
}
