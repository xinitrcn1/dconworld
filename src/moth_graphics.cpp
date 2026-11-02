// Watcher/BigMothGraphics.cs: the moths' graphics module. See moth_graphics.hpp.

#include <algorithm>
#include <cmath>
#include <utility>

#include "moth_graphics.hpp"
#include "creature.hpp"
#include "moth.hpp"
#include "moth_types.hpp"

namespace moth_graphics {
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using sprites::sprite;
    using namespace moth;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        graphics_state& gs(sys::game_state& state, dcon::creature_id c) { return state.world.moth_data_get_graphics(creature::moth_data(state.world, c)); }
        graphics_state const& gs(sys::game_state const& state, dcon::creature_id c) {
            return state.world.moth_data_get_graphics(creature::moth_data(state.world, c));
        }
        body_state const& bs(sys::game_state const& state, dcon::creature_id c) { return state.world.moth_data_get_body(creature::moth_data(state.world, c)); }
        ivars const& ivs(sys::game_state const& state, dcon::creature_id c) { return state.world.moth_data_get_ivars(creature::moth_data(state.world, c)); }
        vec2 cp(sys::game_state const& state, dcon::creature_id c, uint32_t i) { return state.world.body_chunk_get_pos(creature::chunk(state, c, i)); }
        vec2 cv(sys::game_state const& state, dcon::creature_id c, uint32_t i) { return state.world.body_chunk_get_vel(creature::chunk(state, c, i)); }
        vec2 cl(sys::game_state const& state, dcon::creature_id c, uint32_t i, float t) {
            auto const k = creature::chunk(state, c, i);
            return rw::lerp(state.world.body_chunk_get_last_pos(k), state.world.body_chunk_get_pos(k), t);
        }
        bool is_small(sys::game_state const& state, dcon::creature_id c) { return moth::variant_of(state, c) == variant::small; }
        uint32_t tongue_count(bool small) { return small ? 6u : 12u; }
        uint32_t wing_count(bool small) { return small ? 6u : 8u; }

        vec3 lerp3(vec3 a, vec3 b, float t) { return a + (b - a) * mathf::clamp01(t); }
        vec4 v4(vec3 c, float a = 1.f) { return vec4{c.x, c.y, c.z, a}; }
        vec2 lp(vec2 const (&s)[3], float t) { return rw::lerp(s[1], s[0], t); }
        vec2 lp2(vec2 const (&s)[2], float t) { return rw::lerp(s[1], s[0], t); }

        // LegGraphics.legSegmentLengths (the back pair's own)
        float leg_segment_length(uint32_t pair, uint32_t i, float scale) {
            static constexpr float back[3] = {24.f, 24.f, 22.f};
            static constexpr float front[3] = {30.f, 34.f, 30.f};
            return (pair == 0 ? back[i] : front[i]) * scale;
        }

        // ApplyPalette / ExitShadowMode's colours
        struct colors {
            vec3 body;
            vec3 depth;
            vec3 secondary;
            vec3 tongue_base;
            vec3 tongue_tip;
            vec3 black;
            vec3 fog;
            // BodyColorWithDepth
            vec3 with_depth(float depth, float secondary_t = 0.f) const { return lerp3(lerp3(body, secondary, secondary_t), this->depth, depth); }
        };
        colors make_colors(ivars const& iv, sprites::room_palette const& pal) {
            colors k;
            k.black = pal.black();
            k.fog = pal.fog();
            k.body = lerp3(iv.body_color, k.fog, 0.03f);
            k.body = lerp3(k.body, k.black, pal.darkness() * 0.3f);
            k.depth = lerp3(lerp3(k.body, vec3{0.f, 0.f, 0.f}, 0.2f), k.fog, 0.7f);
            k.secondary = iv.secondary_color;
            k.tongue_base = lerp3(lerp3(vec3{1.f, 0.f, 0.f}, k.body, 0.7f), k.black, 0.15f);
            k.tongue_tip = lerp3(k.tongue_base, k.black, 0.44f);
            return k;
        }

        // TongueGraphics.Update (the reach is the simulation's: moth.cpp)
        void tongue_update_impl(sys::game_state& state, dcon::creature_id c, world::room_view const& room) {
            auto& g = gs(state, c);
            auto const& b = bs(state, c);
            float const scale = moth::scale_of(state, c);
            bool const small = is_small(state, c);
            uint32_t const n = tongue_count(small);
            auto& seg = g.tongue;
            bool const drink_ok = b.drink && creature::here(state, b.drink, room.id) && b.drink_chunk < creature::chunk_count_of(state, b.drink);
            g.glug_size = custom::lerp_and_tick(g.glug_size, b.drinking ? 2.8f : 0.f, 0.03f, 0.004f);
            float const outness = b.tongue_outness;
            float const foldness = b.tongue_foldness;
            float const grav = state.world.physical_object_get_g(creature::body(state, c));
            float num = 10.f;
            seg[0][1] = seg[0][0];
            vec2 const vector = g.head_pos + g.head_dir * 23.f * scale;
            seg[0][0] = vector;
            num *= outness * scale;
            vec2 a = custom::perpendicular_vector(g.head_dir);
            vec2 const normalized = rw::normalized(g.head_dir + a * g.face_flip * foldness * 0.7f * outness);
            a = custom::perpendicular_vector(normalized);
            vec2 vector2{0.f, 0.f};
            vec2 vector3{1.f, 0.f};
            vec2 const v0 = cv(state, c, 0);
            for (uint32_t i = 1; i < n; ++i) {
                float const num2 = float(i) / float(n - 1) * outness;
                float const num3 = 1.f - num2;
                seg[i][1] = seg[i][0];
                seg[i][0] += seg[i][2];
                seg[i][2].y -= grav * (num2 * 0.5f + 0.5f);
                seg[i][2] *= 0.95f;
                float const num4 = rw::distance(seg[i - 1][0], seg[i][0]);
                vec2 const a2 = custom::dir_vec(seg[i - 1][0], seg[i][0]);
                if (num4 > num) {
                    vec2 const a3 = a2 * (num - num4);
                    seg[i][0] = seg[i - 1][0] + a2 * num;
                    seg[i][2] += a3 * 0.3f;
                }
                vec2 vector4 = vector3;
                vec2 const a4 = custom::perpendicular_vector(vector4);
                vector4 = rw::normalized(vector4 * (1.f - (num2 + 0.4f) * foldness * outness) + a4 * (num2 + 0.4f) * foldness * outness) * (num3 * 0.8f + 0.2f);
                vec2 const bb = (vector + (normalized * (vector4.x + vector2.x) - a * (vector4.y + vector2.y) * g.face_flip) * num - seg[i][0]) * 0.25f * foldness;
                seg[i][0] += bb;
                seg[i][2] += bb;
                seg[i][2] += v0 * num3 * 0.1f;
                seg[i][2] += g.head_dir * (num3 * 0.7f + 0.2f) * (1.f - foldness);
                vector2 += vector4;
                vector3 = vector4;
            }
            seg[0][0] = vector;
            if (!drink_ok)
                return;
            vec2 const dp = state.world.body_chunk_get_pos(creature::chunk_of(state, b.drink, b.drink_chunk));
            if (!b.drinking && outness > 0.5f && foldness < 0.9f) {
                float const num5 = rw::distance(seg[n - 1][0], dp);
                vec2 const a5 = custom::dir_vec(seg[n - 1][0], dp);
                for (uint32_t j = 1; j < n; ++j) {
                    float const num6 = mathf::inverse_lerp(0.f, float(n - 1), float(j));
                    seg[j][2] += a5 * num5 * 0.15f * mathf::pow(num6 * (1.f - foldness), 2.f);
                }
            }
            if (b.drinking) {
                if (creature::consious(state, c))
                    g.suck_counter += creature::is(state.world, b.drink, creature::type::moth_grub) ? -1 : 1;
                float const num7 = rw::distance(vector, dp);
                seg[n - 1][0] = dp;
                for (uint32_t k = 1; k + 1 < n; ++k) {
                    float const num8 = mathf::inverse_lerp(0.f, float(n - 1), float(k));
                    vec2 const vector5 = rw::lerp(vector, dp, num8);
                    vec2 const a6 = (vector5 - seg[k][0]) * num8;
                    seg[k][0] = rw::lerp(seg[k][0], vector5, num8 * 0.7f + num7 / (400.f * scale));
                    seg[k][2] += a6 * 0.2f;
                }
            }
        }

        // AntennaGraphics.Update (no wind here)
        void antenna_update(sys::game_state& state, dcon::creature_id c, uint32_t index) {
            auto& g = gs(state, c);
            auto const& iv = ivs(state, c);
            float const scale = moth::scale_of(state, c);
            float const grav = state.world.physical_object_get_g(creature::body(state, c));
            uint32_t const n = uint32_t(std::clamp<int32_t>(iv.antenna_length, 1, int32_t(max_antenna_segments)));
            auto& seg = g.antennas[index];
            constexpr float num = 10.f;
            seg[0][1] = seg[0][0];
            vec2 const hp = custom::perpendicular_vector(g.head_dir);
            vec2 const vector = g.head_pos + (g.head_dir * 4.5f + hp * (index == 0 ? -4.f : 4.f) * (0.3f + (1.f - std::fabs(g.face_flip) * 0.7f))) * scale;
            seg[0][0] = vector;
            float const d = ((index == 0) == (g.face_flip > 0.5f)) ? (0.5f - std::fabs(g.face_flip) * 0.1f) : (0.5f + std::fabs(g.face_flip) * 0.1f);
            vec2 const bb = hp * (index == 0 ? 1.f : -1.f) * (0.05f + iv.sympathy * 0.4f);
            vec2 const v0 = cv(state, c, 0);
            for (uint32_t i = 1; i < n; ++i) {
                float const num2 = 1.f - mathf::inverse_lerp(0.f, float(n - 1), float(i));
                seg[i][1] = seg[i][0];
                seg[i][0] += seg[i][2];
                seg[i][2].y -= grav * 0.07f * (1.f - num2);
                seg[i][2] *= 0.9f;
                float const num3 = rw::distance(seg[i - 1][0], seg[i][0]);
                vec2 const b2 = custom::dir_vec(seg[i - 1][0], seg[i][0]) * (num - num3) * d;
                seg[i][0] += b2;
                seg[i][2] += b2;
                seg[i - 1][0] -= b2;
                seg[i - 1][2] -= b2;
                seg[i][2] += v0 * num2 * 0.1f;
                seg[i][2] -= g.head_dir * (num2 * 0.5f + 0.2f) + bb;
            }
            seg[0][0] = vector;
        }

        // LegGraphics.Update
        void leg_update(sys::game_state& state, dcon::creature_id c, uint32_t pair, uint32_t idx) {
            auto& g = gs(state, c);
            auto const& l = bs(state, c).legs[pair][idx];
            float const scale = moth::scale_of(state, c);
            float const flip = state.world.creature_get_payload(c).moth.flip;
            bool const initially = idx == 0;
            g.leg_in_front[pair][idx] = (flip > 0.f ? initially : !initially) ? 1 : 0;
            bool const flag = pair == 0;
            auto& seg = g.legs[pair][idx];
            for (uint32_t i = 0; i < 4; ++i)
                seg[i][1] = seg[i][0];
            seg[0][0] = l.connection_pos;
            seg[3][1] = seg[3][0];
            seg[3][0] = l.foot_pos;
            vec2 const a = custom::perpendicular_vector(custom::dir_vec(seg[0][0], seg[3][0]));
            seg[1][0] -= a * flip * 8.f * (flag ? -1.f : 1.f) * scale;
            seg[2][0] += a * flip * 8.f * (flag ? -1.f : 1.f) * scale;
            vec2 const v = cv(state, c, l.connected_chunk);
            for (int32_t j = 3; j >= 1; --j) {
                seg[j][0] += v * float(3 - j) / 3.f;
                vec2 const a2 = custom::dir_vec(seg[j - 1][0], seg[j][0]);
                float const num = rw::distance(seg[j - 1][0], seg[j][0]);
                float const len = leg_segment_length(pair, uint32_t(j - 1), scale);
                seg[j][0] += a2 * (len - num) * 0.5f;
                seg[j - 1][0] -= a2 * (len - num) * 0.5f;
            }
            seg[0][0] = l.connection_pos;
            for (uint32_t k = 1; k < 3; ++k)
                seg[k][0] = seg[k - 1][0] + custom::dir_vec(seg[k - 1][0], seg[k][0]) * leg_segment_length(pair, k - 1, scale);
            seg[3][0] = l.foot_pos;
        }

        // WingGraphics.Update (no wind here)
        void wing_update(sys::game_state& state, dcon::creature_id c, uint32_t wi) {
            auto& g = gs(state, c);
            auto const& b = bs(state, c);
            auto& wg = g.wings[wi];
            uint32_t const pair = wi / 2;
            uint32_t const index = wi % 2;
            bool const initially = index == 0;
            float const scale = moth::scale_of(state, c);
            float const flip = state.world.creature_get_payload(c).moth.flip;
            uint32_t const n = wing_count(is_small(state, c));
            auto const& wp = b.wings[pair];
            wg.in_front = (flip > 0.f ? initially : !initially) ? 1 : 0;
            vec2 const c1 = cp(state, c, 1), c2 = cp(state, c, 2), c4 = cp(state, c, 4);
            vec2 const vector = custom::dir_vec(c2, c1);
            vec2 const a = custom::perpendicular_vector(vector);
            float d = 1.f;
            if (!wg.in_front)
                d = 1.f - std::fabs(flip) * 1.5f;
            vec2 const vector2 = c1 + (vector * (pair == 0 ? -5.f : 5.f) + a * (index == 0 ? 1.f : -1.f) * (pair == 0 ? 12.f : 7.f) * d) * scale;
            vec2 const a2 = custom::deg_to_vec((wp.flapness * 2.f - 1.f) * 70.f * (index == 0 ? 1.f : -1.f) + (index == 0 ? -90.f : 90.f)
                                               + b.move_direction.x * 40.f * (pair == 1 ? 1.f : 0.7f) * (std::fabs(flip) * 0.5f + 0.5f));
            wg.spread_outness = index == 0 ? -0.7f : 0.7f;
            if (!wg.in_front)
                wg.spread_outness *= mathf::lerp(1.f, 1.f - std::fabs(flip) * 2.f, g.wing_stashness);
            float const num = (pair == 0 ? 110.f : 140.f) * scale;
            wg.tip_pos = vector2 + a2 * num;
            vec2 const a3 = vector2 + rw::normalized(-vector * (pair == 0 ? 1.4f : 1.2f) + a * (index == 0 ? 1.f : -1.f)) * num * 0.8f;
            vec2 const a4 = rw::lerp(vector2, wg.tip_pos, 0.3f) + a * (index == 0 ? 30.f : -30.f) * scale;
            vec2 const a5 = rw::lerp(vector2, wg.tip_pos, 0.7f) + a * (index == 0 ? 10.f : -10.f) * scale;
            vec2 const vector3 = custom::dir_vec(vector2, c4);
            vec2 const a6 = custom::perpendicular_vector(vector3);
            vec2 const bb = vector2 + rw::normalized(rw::normalized(vector3 - vector * 0.4f) - a6 * flip * 0.5f) * num;
            vec2 const b2 = rw::lerp(vector2, c2, 0.8f) + a * (index == 0 ? 40.f : -40.f) * (wg.in_front ? 1.f : -0.8f) * scale;
            vec2 const b3 = rw::lerp(vector2, c4, 0.7f) + vector3 * (index == 0 ? 20.f : -20.f) * (wg.in_front ? 1.f : -0.8f) * scale;
            wg.tip_pos = rw::lerp(wg.tip_pos, rw::lerp(a3, bb, std::fabs(flip)), g.wing_stashness);
            vec2 const ca = rw::lerp(a4, b2, std::fabs(flip));
            vec2 const cb = rw::lerp(a5, b3, std::fabs(flip));
            auto& seg = wg.segments;
            for (uint32_t i = 0; i < n; ++i) {
                seg[i][1] = seg[i][0];
                seg[i][0] += seg[i][2];
                seg[i][2] *= 0.9f;
                float const num2 = mathf::inverse_lerp(0.f, float(n), float(i + 1));
                vec2 const b4 = custom::bezier(vector2, ca, wg.tip_pos, cb, num2);
                vec2 const b5 = (rw::lerp(rw::lerp(vector2, wg.tip_pos, num2), b4, g.wing_stashness) - seg[i][0]) * 0.25f * (1.f - num2 * 0.8f);
                seg[i][0] += b5;
                seg[i][2] += b5;
            }
            float const num3 = num / float(n);
            for (uint32_t j = 1; j < n; ++j) {
                vec2 const a7 = custom::dir_vec(seg[j - 1][0], seg[j][0]);
                if (rw::distance(seg[j - 1][0], seg[j][0]) > num3)
                    seg[j][0] = seg[j - 1][0] + a7 * num3;
            }
            seg[0][0] = vector2;
            seg[1][0] = rw::lerp(seg[1][0], vector2, 0.5f);
        }
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        auto& g = gs(state, c);
        auto const& b = bs(state, c);
        bool const small = is_small(state, c);
        vec2 const c0 = cp(state, c, 0), c1 = cp(state, c, 1);
        g.head_pos = c0;
        g.last_head_pos = c0;
        for (uint32_t i = 0; i < 2; ++i)
            for (uint32_t j = 0; j < max_antenna_segments; ++j) {
                g.antennas[i][j][0] = c0 + vec2{0.f, float(j * 2)};
                g.antennas[i][j][1] = g.antennas[i][j][0];
                g.antennas[i][j][2] = vec2{0.f, 0.f};
            }
        for (uint32_t k = 0; k < 2; ++k)
            for (uint32_t l = 0; l < 2; ++l)
                for (uint32_t m = 0; m < 4; ++m) {
                    g.legs[k][l][m][0] = cp(state, c, b.legs[k][l].connected_chunk) + vec2{float(-5 + int32_t(k) * 5), float(-5 + int32_t(l) * 5 - int32_t(m) * 2)};
                    g.legs[k][l][m][1] = g.legs[k][l][m][0];
                }
        for (uint32_t n = 0; n < 4; ++n)
            for (uint32_t s = 0; s < max_wing_segments; ++s) {
                g.wings[n].segments[s][0] = c1 + vec2{0.f, -float(s) * 2.f};
                g.wings[n].segments[s][1] = g.wings[n].segments[s][0];
                g.wings[n].segments[s][2] = vec2{0.f, 0.f};
            }
        for (uint32_t t = 0; t < tongue_count(small); ++t) {
            g.tongue[t][0] = c0;
            g.tongue[t][1] = c0;
            g.tongue[t][2] = vec2{0.f, 0.f};
        }
    }

    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room) {
        auto& g = gs(state, c);
        auto const& b = bs(state, c);
        auto const& iv = ivs(state, c);
        float const scale = moth::scale_of(state, c);
        float const flip = state.world.creature_get_payload(c).moth.flip;
        g.last_head_pos = g.head_pos;
        g.last_head_dir = g.head_dir;
        g.last_eye_openness = g.eye_openness;
        g.last_face_flip = g.face_flip;
        // (the blink rolls are the simulation's: moth.cpp)
        g.eye_openness = custom::lerp_and_tick(g.eye_openness, b.blink_counter > 0 ? 0.4f : 1.f, 0.f, 0.3f);
        g.wing_stashness = custom::lerp_and_tick(g.wing_stashness, b.want_to_fly ? 0.f : 1.f, 0.f, b.want_to_fly ? 0.1f : 0.025f);
        vec2 const look = b.look_pos;
        vec2 const c0 = cp(state, c, 0), c1 = cp(state, c, 1);
        vec2 const vector = custom::dir_vec(c1, c0);
        vec2 const a = custom::perpendicular_vector(vector);
        vec2 normalized = rw::normalized(vec2{0.f, -1.f} * 1.9f + vec2{flip, 0.f} - a * flip);
        normalized = rw::normalized(rw::lerp(normalized, custom::dir_vec(c0, look), mathf::min(rw::distance(c0, look) / 700.f, 0.8f)));
        if (rw::dot(normalized, g.head_dir) > 0.4f)
            g.head_dir = normalized;
        else
            g.head_dir = rw::slerp(g.head_dir, normalized, 0.05f);
        g.head_pos = c0 - g.head_dir * 5.f * scale + vector * 2.f * scale;
        g.face_flip = rw::dot(-a, g.head_dir);
        // BigMoth.Act's shake of the antennas
        if (b.shake > 0.01f && creature::consious(state, c)) {
            auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
            uint32_t const n = uint32_t(std::clamp<int32_t>(iv.antenna_length, 1, int32_t(max_antenna_segments)));
            for (uint32_t m = 0; m < 2; ++m) {
                vec2 const aa = custom::rnv(rng) * std::sqrt(rng.value()) * b.shake * 5.f * scale;
                for (uint32_t k = 0; k < n; ++k) {
                    float const num8 = mathf::pow(mathf::inverse_lerp(0.f, float(n - 1), float(k)), 2.f);
                    g.antennas[m][k][2] += aa * (num8 + 0.1f);
                }
            }
        }
        tongue_update_impl(state, c, room);
        for (uint32_t i = 0; i < 2; ++i)
            antenna_update(state, c, i);
        for (uint32_t j = 0; j < 2; ++j)
            for (uint32_t k = 0; k < 2; ++k)
                leg_update(state, c, j, k);
        for (uint32_t l = 0; l < 4; ++l)
            wing_update(state, c, l);
        float const y = c0.y;
        if (!g.shadow_mode && y > room.pixel_height() + 400.f)
            g.shadow_mode = 1;
        else if (g.shadow_mode && y < room.pixel_height() + 200.f)
            g.shadow_mode = 0;
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip, std::vector<sprites::rope_point>& ropes) {
        auto const& g = gs(state, c);
        if (g.shadow_mode)
            return;
        auto const& b = bs(state, c);
        auto const& iv = ivs(state, c);
        bool const small = is_small(state, c);
        float const scale = moth::scale_of(state, c);
        float const flip = state.world.creature_get_payload(c).moth.flip;
        float const t = alpha;
        colors const col = make_colors(iv, pal);
        // the sprites in their slots (TotalSprites 34), emitted in order:
        // what's in front of what swaps with the side the moth faces
        constexpr int32_t first_behind_wing = 0, first_behind_leg = 4, abdomen_mesh = 12, body_mesh = 13, first_front_leg = 14, first_front_wing = 22,
                          tongue_sprite = 26, head_mesh = 32;
        auto const antenna_sprite = [](int32_t i) { return head_mesh - 5 + i * 2; };
        auto const eye_sprite = [](int32_t i) { return head_mesh - 1 + i * 2; };
        static thread_local std::vector<std::pair<int32_t, sprites::draw_cmd>> slots;
        slots.clear();
        static thread_local std::vector<sprites::draw_cmd> tmp;
        static thread_local std::vector<sprites::draw_cmd> shin;
        tmp.clear();
        auto const put = [&](int32_t slot, std::vector<sprites::draw_cmd>& v) {
            for (auto& k : v)
                slots.emplace_back(slot, k);
            v.clear();
        };
        auto const put_sprite = [&](int32_t slot, sprite const& s) {
            sprites::add(tmp, s);
            put(slot, tmp);
        };
        // MakeLongMesh with four vertices a segment (custom colours)
        auto const put_mesh = [&](int32_t slot, vec2 const* v, vec4 const* cs, size_t n) {
            sprites::long_mesh(tmp, strip, v, cs, n);
            put(slot, tmp);
        };

        vec2 arr[5];
        for (uint32_t i = 0; i < 5; ++i)
            arr[i] = cl(state, c, i, t);

        // the thorax (BodyMesh)
        {
            vec2 vs[32];
            vec4 cs[32];
            vec2 a = arr[0] + custom::dir_vec(arr[1], arr[0]) * 7.f * scale;
            vec2 vector = arr[2] + custom::dir_vec(arr[2], arr[3]) * 20.f * scale;
            vec2 vector2 = arr[1] + custom::perpendicular_vector(custom::dir_vec(arr[2], arr[0])) * flip * 7.f * scale;
            vec3 const bcol = col.with_depth(0.f);
            vec3 const a2 = col.with_depth(mathf::lerp(0.2f, 0.5f, std::fabs(flip)));
            float num = 0.f;
            vec2 vector3 = vector + custom::dir_vec(arr[2], arr[3]);
            uint32_t const n = small ? 3 : 8;
            for (uint32_t j = 0; j < n; ++j) {
                float num3 = 1.f - mathf::inverse_lerp(0.f, float(n - 1), float(j));
                num3 = mathf::lerp(std::tanh((num3 - 0.5f) * mathf::pi * 2.f) * 0.5f + 0.5f, num3, 0.5f);
                vec2 const vector4 = custom::bezier(a, vector2, vector, arr[1], num3);
                vec2 const a3 = custom::perpendicular_vector(vector4, vector3);
                float const num4 = mathf::lerp(1.f, 9.f + iv.chub * 4.f, mathf::pow(mathf::clamp01(std::sin(mathf::pow(num3, 1.4f) * mathf::pi)), 0.2f))
                                 * mathf::lerp(1.f, 1.25f, num3) * scale;
                vs[j * 4] = (vector3 + vector4) / 2.f - a3 * (num4 + num) * 0.5f;
                vs[j * 4 + 1] = (vector3 + vector4) / 2.f + a3 * (num4 + num) * 0.5f;
                vs[j * 4 + 2] = vector4 - a3 * num4;
                vs[j * 4 + 3] = vector4 + a3 * num4;
                vec4 const k1 = v4(lerp3(a2, bcol, 0.3f + mathf::inverse_lerp(0.f, 7.5f, float(j)) * 0.7f));
                vec4 const k2 = v4(lerp3(a2, bcol, 0.3f + mathf::inverse_lerp(0.f, 7.5f, float(j) + 0.5f) * 0.7f));
                cs[j * 4] = k1;
                cs[j * 4 + 1] = k1;
                cs[j * 4 + 2] = k2;
                cs[j * 4 + 3] = k2;
                vector3 = vector4;
                num = num4;
            }
            put_mesh(body_mesh, vs, cs, n * 4);
            // the abdomen (AbdomenMesh)
            a = arr[2] + custom::dir_vec(arr[2], arr[1]) * 10.f * scale;
            vector = arr[4] + custom::dir_vec(arr[3], arr[4]) * 10.f * scale;
            vector2 = arr[3];
            num = 0.f;
            vector3 = vector + custom::dir_vec(arr[3], arr[4]) * 1.5f * scale;
            for (uint32_t k = 0; k < 8; ++k) {
                float num5 = 1.f - mathf::inverse_lerp(0.f, 7.f, float(k));
                num5 = mathf::lerp(std::tanh((num5 - 0.5f) * mathf::pi * 2.f) * 0.5f + 0.5f, num5, 0.5f);
                vec2 const vector5 = custom::bezier(a, vector2, vector, vector2, num5);
                vec2 const a4 = custom::perpendicular_vector(vector5, vector3);
                float const num6 = mathf::lerp(1.f, 13.f + 6.f * iv.chub, mathf::pow(mathf::clamp01(std::sin(mathf::pow(num5, 1.4f) * mathf::pi)), 0.5f)) * scale;
                vs[k * 4] = (vector3 + vector5) / 2.f - a4 * (num6 + num) * 0.5f;
                vs[k * 4 + 1] = (vector3 + vector5) / 2.f + a4 * (num6 + num) * 0.5f;
                vs[k * 4 + 2] = vector5 - a4 * num6;
                vs[k * 4 + 3] = vector5 + a4 * num6;
                vec4 const k3 = v4(lerp3(a2, bcol, mathf::inverse_lerp(0.f, 7.5f, float(k)) * 0.5f));
                vec4 const k4 = v4(lerp3(a2, bcol, mathf::inverse_lerp(0.f, 7.5f, float(k) + 0.5f) * 0.5f));
                cs[k * 4] = k3;
                cs[k * 4 + 1] = k3;
                cs[k * 4 + 2] = k4;
                cs[k * 4 + 3] = k4;
                vector3 = vector5;
                num = num6;
            }
            put_mesh(abdomen_mesh, vs, cs, 32);
        }

        // the head (HeadMesh) and the eyes
        vec2 const a5 = rw::lerp(g.last_head_pos, g.head_pos, t);
        vec2 const vector6 = rw::slerp(g.last_head_dir, g.head_dir, t);
        vec2 const a6 = custom::perpendicular_vector(vector6);
        {
            float const dd = 27.f * scale;
            float const num7 = 8.f * (1.f - std::fabs(g.face_flip) * 0.25f) * scale;
            uint32_t const n = small ? 4 : 10;
            vec2 vs[20];
            vec4 cs[20];
            for (uint32_t l = 0; l < n; ++l) {
                float num8 = mathf::inverse_lerp(0.f, float(n - 1), float(l));
                num8 = mathf::pow(num8, 2.f);
                float const d2 = mathf::pow(mathf::clamp01(std::sin(mathf::pow(num8, 0.5f) * mathf::pi)), 0.7f) * num7;
                vs[l * 2] = a5 + vector6 * dd * num8 - a6 * d2;
                vs[l * 2 + 1] = a5 + vector6 * dd * num8 + a6 * d2;
                cs[l * 2] = v4(col.body);
                cs[l * 2 + 1] = v4(col.body);
            }
            put_mesh(head_mesh, vs, cs, n * 2);
            float const eye = mathf::lerp(g.last_eye_openness, g.eye_openness, t);
            for (int32_t m = 0; m < 2; ++m) {
                vec2 const vector7 = a5
                    + (vector6 * 18.f * (m == 1 ? (0.8f + (1.f - std::fabs(g.face_flip)) * 0.2f) : 1.f)
                        + a6 * 6.5f * (m == 0 ? 1.f : -1.f) * (1.f - std::fabs(g.face_flip) * 0.25f) * (g.face_flip > 0.f ? 1.f : -1.f))
                          * scale;
                float const rot = custom::vec_to_deg(rw::normalized(vector6 + a6 * (m == 0 ? -1.f : 1.f) * (g.face_flip >= 0.f ? 1.f : -1.f) * 0.2f));
                put_sprite(eye_sprite(m),
                    sprite{"Circle20", vector7, rot, vec2{0.21f * eye * (0.5f + 0.5f * scale), 0.3f * (0.5f + 0.5f * scale)}, vec2{0.5f, 0.5f}, col.black});
            }
        }

        // the tongue (TongueGraphics.DrawSprites): MakeLongMesh's four
        // vertices a segment as two rope points, a third of the way in from
        // either end (like the loach's tentacles), straight between them
        {
            uint32_t const n = tongue_count(small);
            auto const& seg = g.tongue;
            constexpr float bmin = 0.25f;
            float const outness = b.tongue_outness;
            float const num = mathf::max(1.2f * outness, bmin);
            vec2 vector = lp(seg[n - 1], t);
            vector += custom::dir_vec(lp(seg[n - 2], t), vector) * 1.5f;
            float num2 = num;
            size_t const first = ropes.size();
            for (uint32_t i = 0; i < n; ++i) {
                uint32_t const num3 = n - 1 - i;
                float const num4 = 1.f - mathf::inverse_lerp(0.f, float(n - 1), float(i)) * outness;
                float num5 = mathf::lerp(num, bmin, num4) * scale;
                num5 += mathf::max(std::sin(float(g.suck_counter) * 0.15f + num4 * 12.f), 0.f) * 0.5f * scale * g.glug_size;
                vec2 const vector2 = lp(seg[num3], t);
                vec2 const nn = rw::normalized(vector2 - vector);
                float const dd = rw::distance(vector2, vector) / 5.f;
                vec4 const k = v4(lerp3(col.tongue_base, col.tongue_tip, num4));
                if (i == 0)
                    ropes.push_back(sprites::rope_point{vector, num5, k});
                else
                    ropes.push_back(sprites::rope_point{vector + nn * dd, (num5 + num2) * 0.5f, k});
                ropes.push_back(sprites::rope_point{vector2 - nn * dd, num5, k});
                vector = vector2;
                num2 = num5;
            }
            sprites::add_rope(tmp, ropes, first, false);
            put(tongue_sprite, tmp);
        }

        // the antennas (AntennaGraphics.DrawSprites): the wide mesh and the
        // thin one over it
        {
            uint32_t const n = uint32_t(std::clamp<int32_t>(iv.antenna_length, 1, int32_t(max_antenna_segments)));
            float const ff = mathf::lerp(g.last_face_flip, g.face_flip, t);
            float const d = 1.f - 0.24f * ff;
            float const d2 = 1.f + 0.24f * ff;
            vec4 const wide_col = v4(lerp3(col.body, col.black, 0.1f));
            vec4 const thin_col = v4(col.body);
            for (uint32_t index = 0; index < 2; ++index) {
                auto const& seg = g.antennas[index];
                int32_t const slot = antenna_sprite(g.face_flip < 0.f ? int32_t(index) : 1 - int32_t(index));
                vec2 wide[max_antenna_segments * 4];
                vec2 thin[max_antenna_segments * 4];
                vec4 wc[max_antenna_segments * 4];
                vec4 tc[max_antenna_segments * 4];
                vec2 vector = lp(seg[0], t);
                float num3 = 0.f;
                float num4 = 0.f;
                for (uint32_t i = 0; i < n; ++i) {
                    float const num5 = mathf::inverse_lerp(0.f, float(n - 1), float(i));
                    float const num6 = mathf::lerp(1.5f, 0.5f, num5) * scale;
                    float const num7 = std::sin(num5 * mathf::pi) * iv.antenna_width + num6;
                    vec2 const vector2 = lp(seg[i], t);
                    vec2 const nn = rw::normalized(vector2 - vector);
                    vec2 const a = custom::perpendicular_vector(nn);
                    float const d3 = rw::distance(vector2, vector) / 5.f;
                    vec2 const off = i == 0 ? vec2{0.f, 0.f} : nn * d3;
                    wide[i * 4] = vector - a * (num7 + num4) * 0.5f * d + off;
                    wide[i * 4 + 1] = vector + a * (num7 + num4) * 0.5f * d2 + off;
                    thin[i * 4] = vector - a * (num6 + num3) * 0.5f * d + off;
                    thin[i * 4 + 1] = vector + a * (num6 + num3) * 0.5f * d2 + off;
                    wide[i * 4 + 2] = vector2 - a * num7 * d - nn * d3;
                    wide[i * 4 + 3] = vector2 + a * num7 * d2 - nn * d3;
                    thin[i * 4 + 2] = vector2 - a * num6 * d - nn * d3;
                    thin[i * 4 + 3] = vector2 + a * num6 * d2 - nn * d3;
                    for (uint32_t q = 0; q < 4; ++q) {
                        wc[i * 4 + q] = wide_col;
                        tc[i * 4 + q] = thin_col;
                    }
                    vector = vector2;
                    num3 = num6;
                    num4 = num7;
                }
                put_mesh(slot, wide, wc, n * 4);
                put_mesh(slot + 1, thin, tc, n * 4);
            }
        }

        // the wings (WingGraphics.DrawSprites)
        {
            uint32_t const n = wing_count(small);
            for (uint32_t wi = 0; wi < 4; ++wi) {
                auto const& wg = g.wings[wi];
                uint32_t const pair = wi / 2;
                uint32_t const index = wi % 2;
                int32_t const num = (wg.in_front ? first_front_wing : first_behind_wing) + int32_t(pair) * 2;
                auto const& seg = wg.segments;
                // WingRadiusAlong
                auto const radius = [&](float f) {
                    return mathf::lerp(0.25f, 15.f, mathf::pow(mathf::clamp01(std::sin(mathf::pow((1.f - f) * 0.95f, 2.f) * mathf::pi)), 0.5f)) * scale;
                };
                vec2 const vector = lp(seg[0], t);
                vec2 const vector2 = custom::dir_vec(lp(seg[1], t), vector);
                vec2 v = vector + vector2 * 10.f * scale;
                float const num2 = index == 1 ? -1.f : 1.f;
                vec2 const vector3 = vector + custom::perpendicular_vector(vector2) * radius(0.f) * wg.spread_outness;
                float const a = 0.5f + wg.spread_outness * num2 * -1.f * 0.5f;
                float const tt = std::fabs(flip) * (0.2f + g.wing_stashness * 0.8f);
                float num3;
                float bb;
                if (pair == 0) {
                    num3 = wg.in_front ? mathf::lerp(0.2f, 0.15f, tt) : mathf::lerp(0.2f, 0.4f, tt);
                    bb = wg.in_front ? mathf::lerp(0.5f, 0.2f, tt) : mathf::lerp(0.5f, 0.8f, tt);
                } else {
                    num3 = wg.in_front ? 0.1f : mathf::lerp(0.1f, 0.7f, tt);
                    bb = wg.in_front ? mathf::lerp(0.3f, 0.f, tt) : mathf::lerp(0.3f, 0.95f, tt);
                }
                put_sprite(num, sprite{"Circle20", vector3, custom::vec_to_deg(vector2), vec2{scale, 0.5f * scale}, vec2{0.5f, 0.5f}, col.with_depth(num3)});
                vec2 vs[max_wing_segments * 2];
                vec4 cs[max_wing_segments * 2];
                for (uint32_t i = 0; i < n; ++i) {
                    float const num4 = mathf::inverse_lerp(0.f, float(n - 1), float(i));
                    float const depth = mathf::lerp(num3, bb, num4);
                    vec2 const vector4 = lp(seg[i], t);
                    vec2 const a2 = custom::perpendicular_vector(vector4, v) * num2;
                    float const dd = radius(num4);
                    float const num5 = wg.spread_outness * num2;
                    vs[i * 2] = vector4 - a2 * dd * (1.f + num5) * 1.5f;
                    vs[i * 2 + 1] = vector4 + a2 * dd * (1.f - num5) * 1.5f;
                    float const num6 = std::sin(num4 * mathf::pi * 0.5f);
                    // (the MothWing shader isn't ported: the colours with their alpha)
                    cs[i * 2] = v4(col.with_depth(depth, num6 * wg.spread_outness * num2), a);
                    cs[i * 2 + 1] = v4(col.with_depth(depth, num6 * -wg.spread_outness * num2), a);
                    v = vector4;
                }
                put_mesh(num + 1, vs, cs, n * 2);
            }
        }

        // the legs (LegGraphics.DrawSprites)
        {
            uint32_t const n = small ? 3 : 7;
            float const tf = std::fabs(flip);
            for (uint32_t pair = 0; pair < 2; ++pair)
                for (uint32_t idx = 0; idx < 2; ++idx) {
                    bool const in_front = g.leg_in_front[pair][idx] != 0;
                    int32_t const num = (in_front ? first_front_leg : first_behind_leg) + (pair == 0 ? 0 : 4);
                    auto const& sg = g.legs[pair][idx];
                    vec2 ar[4];
                    for (uint32_t i = 0; i < 4; ++i)
                        ar[i] = lp2(sg[i], t);
                    float num2;
                    if (pair == 0)
                        num2 = in_front ? mathf::lerp(0.15f, 0.2f, tf) : mathf::lerp(0.25f, 0.6f, tf);
                    else
                        num2 = in_front ? mathf::lerp(0.1f, 0.15f, tf) : mathf::lerp(0.2f, 0.5f, tf);
                    vec3 const color = col.with_depth(num2);
                    vec3 const color2 = col.with_depth(num2, iv.knee_colorness * 0.25f);
                    put_sprite(num, sprite{"Circle20", ar[2], 0.f, vec2{0.25f * scale, 0.25f * scale}, vec2{0.5f, 0.5f}, color2});
                    float const len2 = leg_segment_length(pair, 2, scale);
                    float const num3 = rw::distance(ar[2], ar[3]);
                    vec2 const vector = rw::lerp(ar[2], ar[3], 0.7f)
                                      + custom::perpendicular_vector(custom::dir_vec(ar[2], ar[3])) * flip * mathf::min(num3 - len2 * 3.2f, 0.f) * 0.44f * scale;
                    vec2 vs[28];
                    vec4 cs[28];
                    // the shin (sprite num + 2)
                    float num4 = 0.f;
                    vec2 vector2 = ar[3] + custom::dir_vec(ar[3], ar[2]) * scale;
                    for (uint32_t j = 0; j < n; ++j) {
                        float const num6 = 1.f - mathf::inverse_lerp(0.f, float(n - 1), float(j));
                        vec2 const vector3 = rw::lerp(rw::lerp(ar[2], vector, num6), rw::lerp(vector, ar[3], num6), num6);
                        vec2 const a = custom::perpendicular_vector(vector3, vector2);
                        float const num7 = (mathf::lerp(2.5f, 0.5f, num6)
                                               + mathf::max(1.f - mathf::pow(num6 * 4.f - 1.f, 2.f), 0.f) * 0.7f * (1.f + iv.leg_puff * 1.8f))
                                         * scale;
                        vs[j * 4] = (vector2 + vector3) / 2.f - a * (num7 + num4) * 0.5f;
                        vs[j * 4 + 1] = (vector2 + vector3) / 2.f + a * (num7 + num4) * 0.5f;
                        vs[j * 4 + 2] = vector3 - a * num7;
                        vs[j * 4 + 3] = vector3 + a * num7;
                        vec4 const k = v4(lerp3(color2, color, num6));
                        for (uint32_t q = 0; q < 4; ++q)
                            cs[j * 4 + q] = k;
                        vector2 = vector3;
                        num4 = num7;
                    }
                    shin.clear();
                    sprites::long_mesh(shin, strip, vs, cs, n * 4);
                    // the thigh (sprite num + 1)
                    num4 = 0.f;
                    vector2 = ar[2] + custom::dir_vec(ar[1], ar[2]) * scale;
                    for (uint32_t k = 0; k < n; ++k) {
                        float const num8 = 1.f - mathf::inverse_lerp(0.f, float(n - 1), float(k));
                        vec2 const vector4 = custom::bezier(ar[0], ar[1], ar[2], ar[1], num8);
                        vec2 const a2 = custom::perpendicular_vector(vector4, vector2);
                        float const num9 = (mathf::max(1.f - mathf::pow(num8 * 4.f - 1.f, 2.f), 0.f) * 0.7f
                                               + mathf::max(1.f - mathf::pow(num8 * 4.f - 3.f, 2.f), 0.f) * 0.95f * (1.f + iv.leg_puff * 2.5f)
                                               + mathf::lerp(5.f, 2.5f, num8))
                                         * scale;
                        vs[k * 4] = (vector2 + vector4) / 2.f - a2 * (num9 + num4) * 0.5f;
                        vs[k * 4 + 1] = (vector2 + vector4) / 2.f + a2 * (num9 + num4) * 0.5f;
                        vs[k * 4 + 2] = vector4 - a2 * num9;
                        vs[k * 4 + 3] = vector4 + a2 * num9;
                        vec4 const kk = v4(lerp3(color, color2, num8));
                        for (uint32_t q = 0; q < 4; ++q)
                            cs[k * 4 + q] = kk;
                        vector2 = vector4;
                        num4 = num9;
                    }
                    put_mesh(num + 1, vs, cs, n * 4);
                    put(num + 2, shin);
                    // the tag (a "pixel" on the tagged leg)
                    if (int32_t(pair * 2 + idx) == iv.tagged_leg) {
                        vec2 const vector5 = rw::lerp(ar[1], ar[2], 0.7f);
                        vec3 const tag = iv.leg_tag_color;
                        vec3 const tb = lerp3(lerp3(tag, col.depth, 0.2f), col.fog, 0.7f);
                        put_sprite(num + 3, sprite{"pixel", vector5, custom::vec_to_deg(custom::dir_vec(ar[1], ar[2])), vec2{9.5f * scale, 6.f * scale}, vec2{0.5f, 0.5f},
                                                lerp3(tag, tb, num2)});
                    }
                }
        }

        std::stable_sort(slots.begin(), slots.end(), [](auto const& x, auto const& y) { return x.first < y.first; });
        for (auto const& s : slots)
            out.push_back(s.second);
    }
}
