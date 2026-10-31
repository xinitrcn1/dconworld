#include <cmath>

#include "needle_worm_graphics.hpp"
#include "body_parts.hpp"
#include "creature.hpp"
#include "needle_worm.hpp"

namespace needle_worm_graphics {
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using world::room_view;
    using needle_worm::graphics_state;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        // GenericBodyPart(rad, surface friction, air friction): the snout's,
        // the wings', the legs'
        constexpr float snout_sf = 0.5f, snout_af = 0.99f;
        constexpr float wing_sf = 0.5f, wing_af = 0.95f;
        constexpr float leg_sf = 0.5f, leg_af = 0.999f;

        graphics_state& graphics(sys::game_state& state, dcon::creature_id c) {
            return state.world.needle_worm_data_get_graphics(creature::needle_worm_data(state.world, c));
        }
        graphics_state const& graphics(sys::game_state const& state, dcon::creature_id c) {
            return state.world.needle_worm_data_get_graphics(creature::needle_worm_data(state.world, c));
        }
        vec2 chunk_pos(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_pos(creature::chunk(state, c, i));
        }
        vec2 chunk_vel(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_vel(creature::chunk(state, c, i));
        }
        vec2 lerp_part(player::body_part const& b, float alpha) {
            return rw::lerp(b.last_pos, b.pos, alpha);
        }
        // Color.Lerp
        vec3 lerp(vec3 a, vec3 b, float t) {
            t = mathf::clamp01(t);
            return a + (b - a) * t;
        }
        vec4 rgba(vec3 c, float a = 1.f) {
            return vec4{c.x, c.y, c.z, a};
        }
        // the connection chunks of the body parts
        uint32_t wing_chunk(bool small, uint32_t i) { return small ? i : 1 + i; }
        uint32_t leg_chunk(bool small, uint32_t i) { return small ? 1 : i + 1; }
        uint32_t snout_count(bool small) { return small ? 3u : 5u; }
        uint32_t legs_per_side(bool small) { return small ? 1u : 3u; }

        // LegConPos, LegConBodyDir, LegConDir
        float leg_f(uint32_t i) { return custom::lerp_map(float(i), 0.f, 2.f, 0.03f, 0.1f, 2.f); }
        vec2 leg_con_body_dir(sys::game_state const& state, dcon::creature_id c, uint32_t i, float alpha) {
            return needle_worm::on_body_dir(state, c, leg_f(i), alpha);
        }
        vec2 leg_con_pos(sys::game_state const& state, dcon::creature_id c, graphics_state const& g, uint32_t s, uint32_t i, float alpha) {
            float const f = leg_f(i);
            return needle_worm::on_body_pos(state, c, f, alpha)
                + custom::perpendicular_vector(needle_worm::on_body_dir(state, c, f, alpha)) * needle_worm::on_body_rad(state, c, f) * (s == 0 ? -1.f : 1.f)
                * rw::slerp(g.last_z_rot, g.z_rot, alpha).y;
        }
        vec2 leg_con_dir(sys::game_state const& state, dcon::creature_id c, graphics_state const& g, uint32_t s, uint32_t i, float alpha) {
            return custom::perpendicular_vector(leg_con_body_dir(state, c, i, alpha)) * (s == 0 ? -1.f : 1.f) * rw::slerp(g.last_z_rot, g.z_rot, alpha).y;
        }

        // a hash in [0, 1] for what DrawSprites rolls (its UnityEngine.Random
        // can't be rolled from a const draw)
        float hash01(uint32_t a, uint32_t b) {
            uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
            h ^= h >> 15;
            h *= 0x2C1B3C6Du;
            h ^= h >> 12;
            return float(h >> 8) * (1.f / 16777215.f);
        }
    }

    void init(sys::game_state& state, dcon::creature_id c) {
        auto& g = graphics(state, c);
        bool const small = needle_worm::is_small(state, c);
        // UnityEngine.Random.InitState(ID.RandomSeed)
        rw::unity_random r{state.world.needle_worm_data_get_seed(creature::needle_worm_data(state.world, c))};
        g.wings_size = (small ? 0.5f : 1.f) * mathf::lerp(0.8f, 1.2f, custom::clamped_random_variation(0.5f, 0.5f, 0.4f, r));
        g.legs_fac = (small ? 0.8f : 1.f) * mathf::lerp(0.6f, 1.4f, r.value());
        g.fatness = custom::clamped_random_variation(0.5f, 0.5f, 0.4f, r);
        g.snout_length = mathf::lerp(0.5f, 1.5f, r.value());
        g.hue = custom::wrapped_random_variation(0.5f, 0.08f, 0.2f, r);
        g.cos_bools[0] = r.value() < 0.5f;
        g.cos_bools[1] = r.value() < (g.cos_bools[0] ? 0.25f : 0.75f);
        g.cos_bools[2] = r.value() < 0.5f;
        g.cos_bools[3] = r.value() < 0.25f;
        if (!g.cos_bools[2] && g.cos_bools[3])
            g.cos_bools[1] = 0;
        if (small) {
            g.lightness = mathf::lerp(0.3f, 1.f, mathf::pow(custom::clamped_random_variation(0.5f, 0.5f, 0.4f, r), 0.4f));
            g.cos_bools[0] = 0;
            g.cos_bools[1] = 0;
            g.cos_bools[3] = 1;
            g.hue_div = 0.f;
        } else {
            g.lightness = 0.4f;
            if (r.value() < 0.33333334f)
                g.lightness = 0.4f * mathf::pow(r.value(), 5.f);
            else if (r.value() < 0.05882353f)
                g.lightness = 1.f - 0.6f * mathf::pow(r.value(), 5.f);
            g.hue_div = mathf::lerp(-1.f, 1.f, r.value()) * custom::lerp_map(std::fabs(0.5f - g.hue), 0.f, 0.08f, 0.06f, 0.3f);
        }
        if (!small && g.lightness < 0.4f && r.value() > g.lightness && r.value() < 0.1f)
            g.hue = r.value();
        g.whiplash = -1.f;
        g.last_whiplash = -1.f;
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        auto& g = graphics(state, c);
        bool const small = needle_worm::is_small(state, c);
        auto const reset = [](player::body_part& b, vec2 at) {
            b.pos = at;
            b.last_pos = at;
            b.vel = vec2{0.f, 0.f};
        };
        for (uint32_t i = 0; i < snout_count(small); ++i)
            reset(g.snout[i], chunk_pos(state, c, 0));
        for (uint32_t j = 0; j < 2; ++j)
            for (uint32_t k = 0; k < 2; ++k)
                reset(g.wings[j][k], chunk_pos(state, c, wing_chunk(small, k)));
        for (uint32_t l = 0; l < 2; ++l)
            for (uint32_t m = 0; m < legs_per_side(small); ++m)
                reset(g.legs[l][m], chunk_pos(state, c, leg_chunk(small, m)));
        g.snout[0].rad = g.snout[1].rad = g.snout[2].rad = g.snout[3].rad = g.snout[4].rad = 1.f;
        for (auto& side : g.wings)
            for (auto& wg : side)
                wg.rad = 2.f;
        for (auto& side : g.legs)
            for (auto& lg : side)
                lg.rad = 1.f;
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto& g = graphics(state, c);
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        auto const d = creature::needle_worm_data(w, c);
        auto const& p = w.creature_get_payload(c).needle_worm;
        auto const& a = w.needle_worm_data_get_attack(d);
        bool const small = needle_worm::is_small(state, c);
        uint32_t const n = needle_worm::chunk_count(small);
        uint32_t const ns = snout_count(small);
        bool const consious = creature::consious(state, c);
        // (the wings' sound loops aren't ported: no noodlefly sounds)
        vec2 const vector = custom::dir_vec(chunk_pos(state, c, n - 1), chunk_pos(state, c, 0));
        g.last_z_rot = g.z_rot;
        g.z_rot = vector;
        g.last_wing_flap = g.wing_flap;
        g.wing_flap += (0.4f + rng.value() * 0.05f) * g.flying;
        g.last_flying = g.flying;
        g.flying = p.flying;
        if (!small) {
            g.last_fang_out = g.fang_out;
            if (a.has_swish || a.in_wall || a.impale)
                g.fang_out = 1.f;
            else
                g.fang_out = custom::s_curve(mathf::inverse_lerp(0.f, 0.75f, a.attack_ready), 0.6f);
            if (a.has_swish)
                g.thin_tail = 1.f;
            else
                g.thin_tail = custom::lerp_and_tick(g.thin_tail, mathf::inverse_lerp(0.5f, 1.f, a.charging_attack), 0.07f, 0.05f);
            if (g.fang_out == 0.f && g.last_fang_out == 0.f)
                g.fang_black = 0.f;
            else
                g.fang_black = custom::lerp_and_tick(g.fang_black, mathf::inverse_lerp(0.5f, 1.f, g.fang_out), 0.002f, 0.0038461538f);
        }
        // the snout
        vec2 const head = custom::dir_vec(chunk_pos(state, c, 1), chunk_pos(state, c, 0));
        vec2 const tip = chunk_pos(state, c, 0) + head * (w.body_chunk_get_rad(creature::chunk(state, c, 0)) + 5.f);
        for (uint32_t i = 0; i < ns; ++i) {
            float const f = mathf::inverse_lerp(0.f, float(ns - 1), float(i));
            body_parts::generic_update(g.snout[i], room, snout_af, snout_sf);
            g.snout[i].vel += rw::slerp(head * 3.f, vec2{0.f, -0.9f}, mathf::pow(f, 0.6f) * mathf::inverse_lerp(0.25f, 0.f, g.fang_out) * (1.f - p.screaming));
        }
        if (p.screaming > 0.f)
            for (uint32_t j = 0; j < ns; ++j) {
                g.snout[j].vel += custom::rnv(rng);
                g.snout[j].pos += custom::rnv(rng);
            }
        float const len = (3.f + 2.f * mathf::inverse_lerp(0.5f, 0.f, g.fang_out)) * g.snout_length;
        {
            vec2 dir = custom::dir_vec(g.snout[0].pos, tip);
            float dist = rw::distance(g.snout[0].pos, tip);
            g.snout[0].vel += dir * (dist - len);
            g.snout[0].pos += dir * (dist - len);
            for (uint32_t k = 1; k < ns; ++k) {
                dir = custom::dir_vec(g.snout[k].pos, g.snout[k - 1].pos);
                dist = rw::distance(g.snout[k].pos, g.snout[k - 1].pos);
                g.snout[k].vel += dir * (dist - len) * 0.5f;
                g.snout[k].pos += dir * (dist - len) * 0.5f;
                g.snout[k - 1].vel -= dir * (dist - len) * 0.5f;
                g.snout[k - 1].pos -= dir * (dist - len) * 0.5f;
            }
        }
        if (small) {
            uint32_t seg = 0;
            if (auto const m = needle_worm::hanging_on_mother(state, c, &seg)) {
                g.snout[ns - 1].vel = vec2{0.f, 0.f};
                g.snout[ns - 1].pos = needle_worm::segment_pos(state, m, needle_worm::chunk_count(false) + seg, 1.f);
            }
        } else if (g.fang_out > 0.f) {
            for (uint32_t l = 0; l < ns; ++l) {
                g.snout[l].pos = rw::lerp(g.snout[l].pos, chunk_pos(state, c, 0) + head * float(l) * 3.f, mathf::inverse_lerp(0.f, 0.5f, g.fang_out));
                g.snout[l].vel *= mathf::inverse_lerp(0.5f, 0.f, g.fang_out);
            }
        }
        // the legs
        for (uint32_t m = 0; m < 2; ++m)
            for (uint32_t k = 0; k < legs_per_side(small); ++k) {
                auto& lg = g.legs[m][k];
                body_parts::generic_update(lg, room, leg_af, leg_sf);
                body_parts::connect_to_point(lg, leg_con_pos(state, c, g, m, k, 1.f), (k == 1 ? 16.f : 11.f) * g.legs_fac, false, 0.f,
                    chunk_vel(state, c, leg_chunk(small, k)), 0.f, 0.f);
                lg.vel.y -= 0.9f;
                lg.vel += leg_con_dir(state, c, g, m, k, 1.f) * 0.55f;
                if (consious && rng.value() < 0.11111111f)
                    lg.vel += custom::rnv(rng) * rng.value() * 2.f * g.legs_fac;
            }
        // the wings: beating while it flies, folded back along the body else
        for (uint32_t j = 0; j < 2; ++j)
            for (uint32_t k = 0; k < 2; ++k) {
                auto& wg = g.wings[j][k];
                uint32_t const ci = wing_chunk(small, k);
                body_parts::generic_update(wg, room, wing_af, wing_sf);
                body_parts::connect_to_point(wg, chunk_pos(state, c, ci), 30.f * g.wings_size, true, (!small && a.swish_counter > 0) ? 0.6f : 0.f,
                    chunk_vel(state, c, ci), 0.3f, 0.1f);
                vec2 const sd = needle_worm::segment_dir(state, c, ci, 1.f);
                float const sj = j == 0 ? -1.f : 1.f;
                float const sk = k == 0 ? -45.f : 45.f;
                if (g.flying > 0.f) {
                    vec2 v3 = custom::deg_to_vec((90.f + sk * vector.y) * sj * mathf::sign(vector.y));
                    v3.y = mathf::lerp(v3.y, std::fabs(v3.y), 0.4f);
                    vec2 v4 = custom::deg_to_vec(custom::vec_to_deg(sd) + (90.f + sk) * sj);
                    v4.y = std::fabs(v4.y);
                    vec2 v5 = rw::normalized(rw::lerp(v4, v3, std::fabs(vector.y) * 0.6f));
                    if (!small)
                        v5 = rw::slerp(v5, vector, 0.7f * mathf::pow(a.charging_attack, 0.5f));
                    wg.vel += v5 * 10.f * g.wings_size * g.flying;
                }
                if (consious)
                    wg.vel -= (sd + custom::perpendicular_vector(sd) * sj * 0.1f) * (1.f - g.flying) * 4.f * g.wings_size;
                else if (room.point_submerged(wg.pos))
                    wg.vel.y -= 0.2f;
            }
        if (!small) {
            // the whiplash down the body as the charge peaks
            g.last_whiplash = g.whiplash;
            if (g.whiplash == -1.f && a.charging_attack > 0.7f)
                g.whiplash += 0.01f;
            if (g.whiplash > -1.f) {
                g.whiplash += 0.1f;
                if (g.whiplash > 2.f && g.last_whiplash > 2.f) {
                    g.whiplash = -1.f;
                    g.last_whiplash = -1.f;
                }
            }
        }
    }

    namespace {
        struct colors {
            vec3 black, fog, body, highlight, eye, details;
        };
        // NeedleWormGraphics.ApplyPalette
        colors palette_colors(graphics_state const& g, sprites::room_palette const& pal) {
            colors k;
            k.black = pal.black();
            k.fog = pal.fog();
            float num = g.hue + 0.478f;
            k.body = custom::hsl2rgb(num, custom::lerp_map(g.lightness, 0.5f, 1.f, 0.9f, 0.5f), mathf::lerp(0.1f, 0.8f, mathf::pow(g.lightness, 2.f)));
            k.highlight = custom::hsl2rgb(num, custom::lerp_map(g.lightness, 0.5f, 1.f, 0.5f, 1.f), mathf::lerp(0.2f, 1.f, g.lightness));
            num += mathf::inverse_lerp(0.5f, 0.6f, g.lightness) * 0.5f;
            if (g.cos_bools[2]) {
                k.eye = custom::hsl2rgb(num + 0.5f - g.hue_div, 1.f, mathf::lerp(0.7f, 0.3f, mathf::pow(g.lightness, 1.5f)));
                k.details = custom::hsl2rgb(num + 0.5f + g.hue_div, 0.8f, 0.4f);
            } else {
                k.eye = custom::hsl2rgb(num + 0.5f, 1.f, mathf::lerp(0.7f, 0.3f, mathf::pow(g.lightness, 1.5f)));
                k.details = g.cos_bools[3] ? custom::hsl2rgb(num + 0.5f, 1.f, 0.5f) : custom::hsl2rgb(num, 1.f, 0.5f);
            }
            if (g.lightness < 0.5f) {
                k.body = lerp(k.body, k.black, mathf::pow(mathf::inverse_lerp(0.5f, 0.f, g.lightness), 0.5f));
                k.highlight = lerp(k.highlight, lerp(k.black, k.fog, 0.4f), mathf::pow(mathf::inverse_lerp(0.5f, 0.f, g.lightness), 2.f));
            } else if (g.lightness > 0.5f) {
                k.body = lerp(k.body, k.fog, mathf::inverse_lerp(0.5f, 1.f, g.lightness) * 0.2f);
                k.highlight = lerp(k.body, vec3{1.f, 1.f, 1.f}, mathf::inverse_lerp(0.5f, 1.f, g.lightness));
            }
            return k;
        }

        // DrawSprites' view of the worm
        struct draw_ctx {
            sys::game_state const& state;
            dcon::creature_id c;
            graphics_state const& g;
            float alpha;
            bool small;
            uint32_t n, total, ns;
            float eaten;
            // the mother whose tail the young hangs on, and the segment
            dcon::creature_id mom;
            uint32_t mom_seg = 0;
        };

        // GraphSegmentPos, WhiplashGraphSegmentPos, EatenGraphSegmentPos
        vec2 plain_graph_segment_pos(draw_ctx const& x, uint32_t i) {
            if (i < x.ns)
                return lerp_part(x.g.snout[x.ns - 1 - i], x.alpha);
            return needle_worm::segment_pos(x.state, x.c, i - x.ns, x.alpha);
        }
        vec2 graph_segment_pos(draw_ctx const& x, uint32_t i) {
            if (x.g.whiplash > -1.f && x.g.last_whiplash > -1.f) {
                if (i < x.ns)
                    return lerp_part(x.g.snout[x.ns - 1 - i], x.alpha);
                float const num = mathf::lerp(x.g.last_whiplash, x.g.whiplash, x.alpha);
                uint32_t const s = i - x.ns;
                vec2 const sp = needle_worm::segment_pos(x.state, x.c, s, x.alpha);
                float const f = mathf::inverse_lerp(0.f, float(x.total), float(s));
                float const d = (mathf::pow(mathf::clamp01(std::sin(f * mathf::pi)), 0.5f) + 0.1f * f) * mathf::lerp(30.f, 80.f, f);
                return sp + custom::perpendicular_vector(needle_worm::segment_dir(x.state, x.c, s, x.alpha))
                    * mathf::pow(mathf::inverse_lerp(0.4f + mathf::clamp01(num) * 0.4f, 0.f, std::fabs(f - num)), 2.f)
                    * std::sin((num * 1.5f + f) * mathf::pi * (2.f + mathf::clamp01(num) * 6.f)) * d;
            }
            if (x.eaten < 1.f) {
                uint32_t const tot = x.total + x.ns;
                if (mathf::inverse_lerp(0.f, float(tot - 1), float(i)) > x.eaten)
                    i = uint32_t(custom::int_clamp(int32_t(std::round(x.eaten * float(tot - 1))), 0, int32_t(tot - 1)));
            }
            return plain_graph_segment_pos(x, i);
        }
        // GraphSegmentRad
        float graph_segment_rad(draw_ctx const& x, uint32_t i) {
            auto const& g = x.g;
            if (i < x.ns)
                return 1.f + mathf::pow(mathf::inverse_lerp(0.f, 0.5f, g.fang_out), 0.6f) * 1.5f;
            uint32_t const s = i - x.ns;
            float const fat = mathf::lerp(0.75f, 1.35f, g.fatness);
            if (s < x.n)
                return needle_worm::segment_rad_for_collision(x.state, x.c, s) * fat;
            return custom::lerp_map(float(s), float(x.n - 1), float(x.total), needle_worm::segment_rad_for_collision(x.state, x.c, x.n - 1) * fat, 0.7f,
                (1.4f - 1.35f * g.thin_tail) * mathf::lerp(1.8f, 0.2f, g.fatness));
        }
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip) {
        auto const& w = state.world;
        auto const& g = graphics(state, c);
        auto const d = creature::needle_worm_data(w, c);
        auto const& p = w.creature_get_payload(c).needle_worm;
        auto const& atk = w.needle_worm_data_get_attack(d);
        auto const& young = w.needle_worm_data_get_young(d);
        bool const small = needle_worm::is_small(state, c);
        draw_ctx x{state, c, g, alpha, small, needle_worm::chunk_count(small), needle_worm::total_segments(small), snout_count(small),
            small ? custom::lerp_map(float(young.bites), 4.f, 1.f, 1.f, 0.4f) : 1.f, dcon::creature_id{}, 0};
        if (small)
            x.mom = needle_worm::hanging_on_mother(state, c, &x.mom_seg);
        colors const k = palette_colors(g, pal);
        uint32_t const tot_graph = x.total + x.ns;
        uint32_t const highlight_segs = x.total * 2 / 3;
        float const sz = small ? 0.65f : 1.f;

        float const num = mathf::lerp(g.last_flying, g.flying, alpha);
        vec2 const vector = custom::dir_vec(needle_worm::segment_pos(state, c, x.n / 2, alpha), needle_worm::segment_pos(state, c, 0, alpha));
        vec2 const vector2 = rw::slerp(g.last_z_rot, g.z_rot, alpha);

        // the eyes
        sprites::sprite eyes[2];
        {
            vec2 v3 = needle_worm::segment_pos(state, c, 0, alpha);
            vec2 const v4 = custom::dir_vec(v3, lerp_part(g.snout[1], alpha));
            v3 = rw::lerp(v3, lerp_part(g.snout[0], alpha), 0.4f);
            float const t = mathf::inverse_lerp(0.f, 0.7f, rw::dot(v4, vector));
            for (int i = 0; i < 2; ++i) {
                vec2 const v5 = v3 + custom::perpendicular_vector(v4) * 4.f * sz * (((i == 0) != (v4.x < 0.f)) ? -1.f : 1.f) * v4.y;
                eyes[i] = sprites::sprite{"JetFishEyeB", v5, custom::vec_to_deg(rw::normalized(v4 + vector)),
                    vec2{mathf::lerp(0.8f, 0.6f, t) * sz, mathf::lerp(1.1f, 1.5f, t) * sz}, vec2{0.5f, 0.5f}, k.eye};
            }
        }

        // the body: a long mesh from the snout's tip to the tail's
        std::vector<vec2> bv(tot_graph * 4 - 1);
        std::vector<vec4> bc(bv.size());
        {
            vec2 v6 = graph_segment_pos(x, 0);
            float r2 = graph_segment_rad(x, 0);
            float num3 = 1.f;
            if (!small)
                num3 *= custom::lerp_map(rw::distance(graph_segment_pos(x, x.ns), graph_segment_pos(x, x.ns + x.n - 1)), 50.f, 80.f, 1.f, 0.85f);
            for (uint32_t j = 0; j < tot_graph; ++j) {
                vec2 v7 = graph_segment_pos(x, j);
                if (x.mom && j < x.ns)
                    v7 = rw::lerp(v7, needle_worm::segment_pos(state, x.mom, needle_worm::chunk_count(false) + x.mom_seg, alpha),
                        mathf::inverse_lerp(float(x.ns - 1), 0.f, float(j)));
                float r4 = graph_segment_rad(x, j) * num3;
                if (p.screaming > 0.f)
                    r4 += mathf::pow(p.screaming, 0.7f) * 4.f * hash01(c.index() * 31u + j, uint32_t(g.wing_flap * 1000.f))
                        * mathf::inverse_lerp(float(x.ns + x.n), float(x.ns), float(j));
                vec2 const nrm = rw::normalized(v7 - v6);
                vec2 const side = custom::perpendicular_vector(nrm);
                float const dd = rw::distance(v7, v6) / 5.f;
                bv[j * 4] = v6 - side * (r2 + r4) * 0.5f + nrm * dd;
                bv[j * 4 + 1] = v6 + side * (r2 + r4) * 0.5f + nrm * dd;
                if (j < tot_graph - 1) {
                    bv[j * 4 + 2] = v7 - side * r4 - nrm * dd;
                    bv[j * 4 + 3] = v7 + side * r4 - nrm * dd;
                } else {
                    bv[j * 4 + 2] = v7;
                }
                r2 = r4;
                v6 = v7;
            }
            for (size_t l = 0; l < bc.size(); ++l) {
                if (small) {
                    bc[l] = rgba(k.body);
                    continue;
                }
                // the adult's body darkens towards the tail
                float const f = mathf::inverse_lerp(0.f, float(bc.size() - 1), float(l));
                bc[l] = rgba(lerp(k.body, lerp(k.details, k.black, g.cos_bools[0] ? mathf::pow(f, 2.f) * 0.85f : 1.f),
                    mathf::pow(mathf::inverse_lerp(0.3f, 1.f, f), 2.f) * (g.cos_bools[0] ? 1.f : 0.6f)));
            }
        }

        // the highlight along the back
        std::vector<vec2> hv(highlight_segs * 4 - 1);
        std::vector<vec4> hc(hv.size());
        {
            vec2 v6 = graph_segment_pos(x, x.ns - 1);
            float r2 = 0.f;
            for (uint32_t j = 0; j < highlight_segs; ++j) {
                vec2 v8 = graph_segment_pos(x, j + x.ns);
                float r5 = graph_segment_rad(x, j + x.ns);
                v8 += vec2{-1.f, 1.f} * r5 / 3.f;
                r5 /= 3.2f;
                vec2 const nrm = rw::normalized(v8 - v6);
                vec2 const side = custom::perpendicular_vector(nrm);
                float const dd = rw::distance(v8, v6) / 5.f;
                hv[j * 4] = v6 - side * (r2 + r5) * 0.5f + nrm * dd;
                hv[j * 4 + 1] = v6 + side * (r2 + r5) * 0.5f + nrm * dd;
                if (j < highlight_segs - 1) {
                    hv[j * 4 + 2] = v8 - side * r5 - nrm * dd;
                    hv[j * 4 + 3] = v8 + side * r5 - nrm * dd;
                } else {
                    hv[j * 4 + 2] = v8;
                }
                r2 = r5;
                v6 = v8;
            }
            for (size_t l = 0; l < hc.size(); ++l) {
                float const f = mathf::inverse_lerp(0.f, float(hc.size() - 1), float(l));
                hc[l] = rgba(lerp(k.body, k.highlight, std::sin(mathf::pow(f, 0.4f) * mathf::pi)));
            }
        }

        // the wings (sprite side by the turn of the body) and the lumps at their roots
        vec2 wing_quads[2][2][4];
        sprites::sprite lumps[2][2];
        {
            float const lf = mathf::lerp(g.last_wing_flap, g.wing_flap, alpha);
            for (uint32_t l = 0; l < 2; ++l)
                for (uint32_t m = 0; m < 2; ++m) {
                    uint32_t const ci = wing_chunk(small, m);
                    vec2 v9 = graph_segment_pos(x, ci + x.ns);
                    if (small && m == 0)
                        v9 = graph_segment_pos(x, 1 + x.ns);
                    vec2 v10 = lerp_part(g.wings[l][m], alpha);
                    vec2 const sd = needle_worm::segment_dir(state, c, ci, alpha);
                    float const sl = l == 0 ? -1.f : 1.f;
                    v9 -= custom::perpendicular_vector(sd) * g.wings_size * 5.f * std::fabs(vector2.y) * sl;
                    v10.y -= (18.f + 18.f * std::sin((lf + (m == 0 ? 0.33f : 0.f)) * mathf::pi * 2.f)) * num * g.wings_size;
                    v10 = v9 + custom::dir_vec(v9, v10) * mathf::lerp(40.f, 60.f, num) * g.wings_size;
                    vec2 const a3 = rw::slerp(custom::perpendicular_vector(sd) * sl, vec2{sl, 0.f}, num);
                    uint32_t const side = ((l == 0) == (vector2.x > 0.f)) ? 0u : 1u;
                    auto& q = wing_quads[side][m];
                    q[1] = v10 + a3 * 2.f * g.wings_size;
                    q[0] = v10 - a3 * 2.f * g.wings_size;
                    q[2] = v9 + a3 * 2.f * g.wings_size;
                    q[3] = v9 - a3 * 2.f * g.wings_size;
                    if (!small) {
                        v9 += custom::dir_vec(v9, v10) * 0.5f;
                        lumps[side][m] = sprites::sprite{"JetFishEyeB", v9, custom::vec_to_deg(sd), vec2{0.9f, 1.2f}, vec2{0.5f, 0.5f},
                            side == 0 ? k.body : lerp(k.body, k.highlight, std::fabs(vector2.x) * 0.6f)};
                    }
                }
        }
        bool const wings_visible = !small || young.bites > 4;
        // (the CicadaWing shader isn't ported: the element tinted by the
        // vertex colours, see through)
        vec3 const wing_root = lerp(k.fog, k.details, 0.5f);
        vec3 const wing_tip = lerp(g.cos_bools[1] ? k.eye : k.fog, vec3{1.f, 1.f, 1.f}, g.cos_bools[1] ? 0.35f : 0.5f);
        vec4 const wing_cols[4] = {rgba(wing_tip, 0.5f), rgba(wing_tip, 0.5f), rgba(wing_root, 0.8f), rgba(wing_root, 0.8f)};

        // the legs
        vec2 leg_quads[2][3][4];
        vec4 leg_cols[2][3][4];
        for (uint32_t s = 0; s < 2; ++s)
            for (uint32_t i = 0; i < legs_per_side(small); ++i) {
                vec2 const v11 = leg_con_pos(state, c, g, s, i, alpha);
                vec2 const v12 = lerp_part(g.legs[s][i], alpha);
                vec2 const a4 = custom::perpendicular_vector(v11, v12);
                float const f = custom::distance_to_line(v12, v11, v11 - leg_con_body_dir(state, c, i, alpha));
                auto& q = leg_quads[s][i];
                q[3] = v12;
                q[2] = (v11 + v12) / 2.f + a4 * 5.f * g.legs_fac * mathf::sign(f);
                q[1] = v11;
                q[0] = (v11 + v12) / 2.f - a4 * 5.f * g.legs_fac * mathf::sign(f);
                auto& col = leg_cols[s][i];
                col[2] = rgba(k.body);
                col[3] = rgba(lerp(k.body, k.details, mathf::inverse_lerp(5.f * g.legs_fac, 9.f * g.legs_fac, std::fabs(f))));
                col[0] = rgba(k.body);
                col[1] = rgba(k.body);
            }

        // the adult's fang: out as it readies its attack, red going black
        std::vector<vec2> fv;
        std::vector<vec4> fc;
        if (!small) {
            float const num8 = mathf::inverse_lerp(0.5f, 1.f, mathf::lerp(g.last_fang_out, g.fang_out, alpha));
            if (num8 > 0.f) {
                bool stuck = false;
                vec2 stuck_at{0.f, 0.f};
                if (atk.in_wall) {
                    stuck = true;
                    stuck_at = atk.stuck_in_wall_pos + atk.stuck_dir * needle_worm::fang_length * 1.2f;
                } else if (atk.impale && creature::live(state, atk.impale) && atk.impale_chunk < creature::chunk_count_of(state, atk.impale)) {
                    stuck = true;
                    auto const ik = creature::chunk_of(state, atk.impale, atk.impale_chunk);
                    stuck_at = rw::lerp(w.body_chunk_get_last_pos(ik), w.body_chunk_get_pos(ik), alpha);
                }
                fv.resize(5 * 4 - 1);
                fc.resize(fv.size());
                vec3 const a5 = lerp(vec3{1.f, 0.f, 0.f}, k.black, mathf::pow(g.fang_black, 3.f));
                vec3 const b = lerp(vec3{1.f, 1.f, 1.f}, k.black, mathf::pow(mathf::inverse_lerp(0.4f, 0.55f, g.fang_black), 0.8f));
                float const pw = 4.f - 3.95f * (num8 * 3.f + g.fang_black) * 0.25f;
                for (size_t i = 0; i < fc.size(); ++i) {
                    float const v = mathf::inverse_lerp(0.f, float(fc.size() - 1), float(i));
                    fc[i] = rgba(lerp(a5, b, mathf::pow(mathf::inverse_lerp(0.1f, 0.35f + 0.65f * g.fang_black, v), pw)));
                }
                vec2 v14 = custom::dir_vec(needle_worm::segment_pos(state, c, 1, alpha), needle_worm::segment_pos(state, c, 0, alpha));
                vec2 v6 = needle_worm::segment_pos(state, c, 0, alpha);
                float r2 = 1.f;
                for (uint32_t i = 0; i < 5; ++i) {
                    float const f = mathf::inverse_lerp(0.f, 4.f, float(i));
                    v14 = rw::normalized(v14
                        + custom::perpendicular_vector(v14) * std::sin(num8 * mathf::pi) * mathf::lerp(-0.3f + 1.3f * mathf::pow(f, 0.5f), f, num8) * -0.2f
                            * vector2.x);
                    vec2 v15 = v6 + v14 * (needle_worm::fang_length / 3.5f) * mathf::pow(num8, 0.8f);
                    if (stuck)
                        v15 = rw::lerp(v15, rw::lerp(needle_worm::segment_pos(state, c, 0, alpha), stuck_at, f), f);
                    float const r12 = mathf::lerp(0.6f + 0.6f * g.fang_black, 0.5f, f);
                    vec2 const nrm = rw::normalized(v15 - v6);
                    vec2 const side = custom::perpendicular_vector(nrm);
                    float const dd = rw::distance(v15, v6) / 5.f;
                    fv[i * 4] = v6 - side * (r2 + r12) * 0.5f + nrm * dd;
                    fv[i * 4 + 1] = v6 + side * (r2 + r12) * 0.5f + nrm * dd;
                    if (i < 4) {
                        fv[i * 4 + 2] = v15 - side * r12 - nrm * dd;
                        fv[i * 4 + 3] = v15 + side * r12 - nrm * dd;
                    } else {
                        fv[i * 4 + 2] = v15;
                    }
                    r2 = r12;
                    v6 = v15;
                }
            }
        }

        // in vanilla sprite order: WingSprite(0, ..), EyeSprite(0),
        // LumpSprite(0, ..), FangMesh, BodyMesh, LegSprite(.., ..),
        // HighLightMesh, LumpSprite(1, ..), EyeSprite(1), WingSprite(1, ..)
        auto const add_wings = [&](uint32_t side) {
            if (!wings_visible)
                return;
            for (uint32_t m = 0; m < 2; ++m)
                sprites::add_quad(out, "CentipedeWing", wing_quads[side][m], wing_cols);
        };
        add_wings(0);
        sprites::add(out, eyes[0]);
        if (!small) {
            sprites::add(out, lumps[0][0]);
            sprites::add(out, lumps[0][1]);
            if (!fv.empty())
                sprites::long_mesh(out, strip, fv.data(), fc.data(), fv.size());
        }
        sprites::long_mesh(out, strip, bv.data(), bc.data(), bv.size());
        for (uint32_t s = 0; s < 2; ++s)
            for (uint32_t i = 0; i < legs_per_side(small); ++i)
                sprites::add_quad(out, "JetFishFlipper3", leg_quads[s][i], leg_cols[s][i]);
        sprites::long_mesh(out, strip, hv.data(), hc.data(), hv.size());
        if (!small) {
            sprites::add(out, lumps[1][0]);
            sprites::add(out, lumps[1][1]);
        }
        sprites::add(out, eyes[1]);
        add_wings(1);
    }
}
