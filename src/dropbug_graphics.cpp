#include <cmath>

#include "dropbug_graphics.hpp"
#include "body_parts.hpp"
#include "dropbug.hpp"
#include "pathing.hpp"
#include "player.hpp"

namespace dropbug_graphics {
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using world::room_view;
    using player::limb_mode;
    using sprites::sprite;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        // Limb(rad 0.1, surface friction 0.7, air friction 0.99, hunt speed 22, quickness 0.95)
        constexpr body_parts::limb_params leg{0.1f, 0.7f, 0.99f, 22.f, 0.95f};
        // CentipedeLegA's sourcePixelSize.y, what the mandibles are scaled by
        constexpr float mandible_element_height = 28.f;

        vec2 chunk_pos(sys::game_state const& state, dcon::dropbug_id d, uint32_t i) {
            return state.world.body_chunk_get_pos(dropbug::chunk(state, d, i));
        }
        vec2 chunk_vel(sys::game_state const& state, dcon::dropbug_id d, uint32_t i) {
            return state.world.body_chunk_get_vel(dropbug::chunk(state, d, i));
        }

        // BodyPart.Reset
        template<typename P>
        void reset_part(P& bp, vec2 at, rw::rng& rng) {
            bp.pos = at + custom::rnv(rng);
            bp.last_pos = bp.pos;
            bp.vel = vec2{0.f, 0.f};
        }

        // Color.Lerp
        vec3 mix(vec3 a, vec3 b, float t) {
            return a + (b - a) * mathf::clamp01(t);
        }

        // one TriangleMesh.MakeLongMesh segment: the quad from the last point
        // (`prev`, half width `prev_w`) to `at` (half width `w`)
        void long_mesh_quad(vec2* v, int i, vec2 prev, vec2 at, float prev_w, float w, float k = 0.5f) {
            vec2 const a = custom::perpendicular_vector(at, prev);
            v[i * 4] = (prev + at) / 2.f - a * (w + prev_w) * k;
            v[i * 4 + 1] = (prev + at) / 2.f + a * (w + prev_w) * k;
            v[i * 4 + 2] = at - a * w;
            v[i * 4 + 3] = at + a * w;
        }
    }

    void reset(sys::game_state& state, dcon::dropbug_id d) {
        auto& w = state.world;
        auto& g = w.dropbug_get_graphics(d);
        g = dropbug::graphics_state{};
        // vanilla seeds these from the creature's ID
        g.random = rw::rng{w.dropbug_get_random(d).state ^ 0x5bd1e995u};
        auto& rng = g.random;
        g.leg_length = 45.f;
        g.body_thickness = mathf::lerp(0.6f, 1.4f, rng.value());
        g.legs_thickness = mathf::lerp(0.6f, 1.4f, rng.value());
        g.antennae_length = mathf::lerp(0.5f, 1.5f, rng.value());
        g.pinchers_length = mathf::lerp(40.f, 60.f, rng.value());
        g.colored_antennae = rng.value() < 0.5f ? rng.value() : 0.f;
        g.hue = 0.6722222f + (1.f - custom::clamped_random_variation(0.5f, 0.5f, 0.3f, rng) * 2.f) * 0.2f;
        // Reset
        for (uint32_t i = 0; i < 3; ++i)
            g.draw_positions[i][0] = g.draw_positions[i][1] = chunk_pos(state, d, i);
        if (w.dropbug_get_behavior(d) == dropbug::behavior::sit_in_ceiling
        && room_view::get_tile_position(chunk_pos(state, d, 0)) == w.dropbug_get_ceiling(d).ceiling_pos)
            g.ceiling_mode = g.last_ceiling_mode = g.deep_ceiling_mode = g.last_deep_ceiling_mode = 1.f;
        // the body parts, at the chunks they hang from
        g.tail_end.rad = 3.f;
        reset_part(g.tail_end, chunk_pos(state, d, 2), rng);
        for (int m = 0; m < 2; ++m) {
            g.mandibles[m].rad = 1.f;
            reset_part(g.mandibles[m], chunk_pos(state, d, 0), rng);
            g.pinchers[m].rad = 1.f;
            reset_part(g.pinchers[m], chunk_pos(state, d, 2), rng);
            g.antennae[m].rad = 1.f;
            reset_part(g.antennae[m], chunk_pos(state, d, 0), rng);
            g.mandible_movements[m][0] = g.mandible_movements[m][1] = 0.f;
        }
        for (int s = 0; s < 2; ++s)
            for (int l = 0; l < 2; ++l) {
                auto& lg = g.legs[s][l];
                lg = player::hand_state{};
                reset_part(lg, chunk_pos(state, d, 0), rng);
                lg.mode = limb_mode::hunt_relative_position;
                lg.hunt_speed = leg.hunt_speed;
                lg.quickness = leg.quickness;
                lg.absolute_hunt_pos = lg.pos;
                g.knees[s][l].rad = 1.f;
                reset_part(g.knees[s][l], chunk_pos(state, d, 0), rng);
                g.legs_travel_dirs[s][l] = vec2{0.f, 0.f};
            }
    }

    void reset_positions(sys::game_state& state, dcon::dropbug_id d) {
        auto& w = state.world;
        auto const keep = w.dropbug_get_graphics(d);
        reset(state, d);
        auto& g = w.dropbug_get_graphics(d);
        g.random = keep.random;
        g.body_thickness = keep.body_thickness;
        g.legs_thickness = keep.legs_thickness;
        g.antennae_length = keep.antennae_length;
        g.pinchers_length = keep.pinchers_length;
        g.colored_antennae = keep.colored_antennae;
        g.hue = keep.hue;
    }

    void update(sys::game_state& state, dcon::dropbug_id d, room_view const& room) {
        auto& w = state.world;
        auto& g = w.dropbug_get_graphics(d);
        auto& rng = g.random;
        vec2 const p0 = chunk_pos(state, d, 0), p1 = chunk_pos(state, d, 1), p2 = chunk_pos(state, d, 2);
        vec2 const v0 = chunk_vel(state, d, 0), v1 = chunk_vel(state, d, 1), v2 = chunk_vel(state, d, 2);
        bool const awake = dropbug::consious(state, d);
        bool const footing = dropbug::footing(state, d);
        bool const swimming = w.dropbug_get_swimming(d) != 0;
        float const in_ceiling = w.dropbug_get_in_ceiling_mode(d);
        float const bite = w.dropbug_get_attempt_bite(d);
        float const anticipation = w.dropbug_get_drop_anticipation(d);
        float const run_cycle = w.dropbug_get_run_cycle(d);

        g.last_vibrate = g.vibrate;
        g.vibrate = custom::lerp_and_tick(g.vibrate, (w.dropbug_get_charging(d) > 0.f || bite > 0.f) ? 1.f : 0.f, 0.2f, 0.05f);
        g.last_ceiling_mode = g.ceiling_mode;
        g.ceiling_mode = custom::lerp_and_tick(g.ceiling_mode, in_ceiling, 0.06f, 0.025f);
        g.last_deep_ceiling_mode = g.deep_ceiling_mode;
        g.deep_ceiling_mode = custom::lerp_and_tick(g.deep_ceiling_mode, g.ceiling_mode == 1.f ? 1.f : 0.f, 0.007f, 0.008333334f);
        if (g.ceiling_jump) {
            g.ceiling_jump = 0;
            g.ceiling_mode = mathf::min(g.ceiling_mode, 0.1f);
            g.deep_ceiling_mode = mathf::min(g.deep_ceiling_mode, 0.1f);
        }
        g.last_pinchers_flip = g.pinchers_flip;
        g.pinchers_flip = custom::lerp_and_tick(g.pinchers_flip, g.flip, 0.001f,
            1.f / custom::lerp_map(std::fabs(g.pinchers_flip - g.flip), 0.03f, 0.6f, 400.f, 2.f, 0.35f));
        for (uint32_t i = 0; i < 3; ++i) {
            g.draw_positions[i][1] = g.draw_positions[i][0];
            g.draw_positions[i][0] = chunk_pos(state, d, i);
        }
        if (!awake || !footing || swimming) {
            g.legs_dangle_counter = 30;
        } else if (g.legs_dangle_counter > 0) {
            --g.legs_dangle_counter;
            for (uint32_t j = 0; j < 2; ++j)
                if (pathing::accessible(room, room_view::get_tile_position(chunk_pos(state, d, j)), pathing::drop_bug))
                    g.legs_dangle_counter = 0;
        }
        g.last_flip = g.flip;
        if (in_ceiling > 0.f) {
            // the feet hold on around the ceiling spot
            vec2 const a = room_view::middle_of_tile(w.dropbug_get_ceiling(d).ceiling_pos);
            for (int s = 0; s < 2; ++s)
                for (int l = 0; l < 2; ++l) {
                    g.legs[s][l].mode = limb_mode::hunt_absolute_position;
                    g.legs[s][l].absolute_hunt_pos = a + vec2{(s == 0 ? 1.f : -2.f) * (l == 0 ? 7.f : 14.f), 10.f};
                }
        }
        // the tail end, breathing
        body_parts::generic_update(g.tail_end, room, 0.99f, 0.5f);
        body_parts::connect_to_point(g.tail_end, p2 + custom::dir_vec(p1, p2) * 12.f + custom::perpendicular_vector(p1, p2) * g.flip * 10.f,
            12.f, false, 0.2f, v1, 0.5f, 0.1f);
        g.tail_end.vel.y -= 0.4f;
        g.tail_end.vel += custom::dir_vec(p1, p2) * 0.8f;
        if (!w.dropbug_get_dead(d)) {
            g.tail_end.vel += g.breath_dir * 0.7f * (1.f - g.deep_ceiling_mode);
            g.breath_dir = rw::clamp_magnitude(g.breath_dir + custom::rnv(rng) * rng.value() * 0.1f, 1.f);
        }
        float const body_deg = custom::aim(p1, p0);
        vec2 const fwd = custom::dir_vec(p1, p0);
        std::optional<vec2> held;
        if (auto const p = dropbug::grasped(state, d))
            held = w.body_chunk_get_pos(player::body_chunk(state, p, w.dropbug_get_grasp_chunk(d)));
        float const still = g.deep_ceiling_mode * mathf::pow(1.f - anticipation, 3.f);
        for (int m = 0; m < 2; ++m) {
            float const side = m == 0 ? -1.f : 1.f;
            auto& mand = g.mandibles[m];
            body_parts::generic_update(mand, room, 0.9f, 0.5f);
            body_parts::connect_to_point(mand, p0 + fwd * mathf::lerp(12.f + 4.f * g.mandible_movements[m][0] + 4.f * g.ceiling_mode, 30.f, bite),
                12.f + 4.f * g.mandible_movements[m][0], false, 0.f, v0, 0.1f, 0.f);
            if (awake) {
                g.mandible_movements[m][0] = custom::lerp_and_tick(g.mandible_movements[m][0], g.mandible_movements[m][1], 0.f, 1.f / mathf::lerp(20.f, 80.f, still));
                if (rng.value() < 1.f / mathf::lerp(20.f, 80.f, still)) {
                    g.mandible_movements[m][1] = mathf::clamp(g.mandible_movements[m][1] + mathf::lerp(-1.f, 1.f, rng.value()) * mathf::lerp(0.75f, 0.25f, still), -1.f, 1.f);
                    g.mandible_movements[m][1] = mathf::lerp(g.mandible_movements[m][1], 0.5f, rng.value() * rng.value() * still);
                }
            }
            mand.vel += rw::normalized(fwd + custom::perpendicular_vector(fwd) * mathf::lerp(side, g.flip * 10.f, std::fabs(g.flip) * 0.9f)) * (1.f + 19.f * bite);
            if (held) {
                mand.pos = rw::lerp(mand.pos, *held, 0.5f);
                mand.vel *= 0.7f;
            }
            auto& pinch = g.pinchers[m];
            body_parts::generic_update(pinch, room, 1.f, 0.5f);
            vec2 pd = rw::normalized(custom::dir_vec(p1, p2) + custom::dir_vec(p2, g.tail_end.pos)
                + custom::perpendicular_vector(p1, p2) * side * (1.f - std::fabs(g.flip)) * mathf::lerp(0.2f, 3.f, g.vibrate));
            if (g.ceiling_mode > 0.f)
                pd = rw::slerp(pd, custom::deg_to_vec(180.f + side * mathf::lerp(30.f, 50.f, anticipation)), g.ceiling_mode);
            body_parts::connect_to_point(pinch, p2 + pd * g.pinchers_length * 0.5f, g.pinchers_length * 0.5f, true, 0.f, v2, 0.2f, 0.f);
            pinch.vel += pd * 10.f;
            auto& ant = g.antennae[m];
            body_parts::generic_update(ant, room, 0.9f, 0.5f);
            vec2 ad = rw::normalized(custom::dir_vec(p1, p0) + custom::perpendicular_vector(p1, p0) * g.flip * 0.5f
                + custom::perpendicular_vector(p1, p0) * side * (1.f - std::fabs(g.flip)) * mathf::lerp(0.35f, 1.2f, g.vibrate));
            if (still > 0.f)
                ad = rw::slerp(ad, custom::dir_vec(p1, p0), still * 0.8f);
            float const reach = mathf::lerp(50.f, 40.f, still) * g.antennae_length;
            body_parts::connect_to_point(ant, p0, reach, false, 0.f, v0, 0.05f, 0.f);
            ant.vel += ad * custom::lerp_map(rw::distance(ant.pos, p0 + ad * reach), 10.f, 150.f, 0.f, 14.f, 0.7f);
            if (awake && rng.value() > still)
                ant.vel += custom::rnv(rng) * rng.value() * mathf::lerp(1.f, 0.1f, still);
            if (g.vibrate > 0.f) {
                mand.vel += custom::rnv(rng) * g.vibrate * rng.value() * 2.f;
                mand.pos += custom::rnv(rng) * g.vibrate * rng.value() * 2.f;
                ant.vel -= fwd * 5.f * g.vibrate;
            }
        }
        // the body rolls towards the side its feet are on
        float spread = 0.f;
        for (auto const& side : g.legs)
            for (auto const& lg : side)
                spread += custom::distance_to_line(lg.pos, p1, p0);
        if (!std::isnan(spread)) {
            spread = mathf::lerp(spread, 0.f, in_ceiling);
            g.flip = custom::lerp_and_tick(g.flip, mathf::clamp(spread / 40.f, -1.f, 1.f), 0.07f, 0.1f);
        }
        float wall = 0.f;
        if (in_ceiling == 0.f) {
            if (awake) {
                if (room.get_tile(p0 + custom::perpendicular_vector(p0, p1) * 20.f).solid())
                    wall += 1.f;
                if (room.get_tile(p0 - custom::perpendicular_vector(p0, p1) * 20.f).solid())
                    wall -= 1.f;
            }
            if (wall != 0.f)
                g.flip = custom::lerp_and_tick(g.flip, wall, 0.07f, 0.05f);
        }
        // the legs: stepping along the travel direction, gripping the terrain
        int32_t n = 0;
        for (int l = 0; l < 2; ++l)
            for (int s = 0; s < 2; ++s) {
                auto& lg = g.legs[s][l];
                auto& knee = g.knees[s][l];
                float const t = float(l);
                float const step = 0.5f + 0.5f * std::sin((run_cycle + float(n) * 0.25f) * mathf::pi);
                g.legs_travel_dirs[s][l] = rw::lerp(g.legs_travel_dirs[s][l], w.dropbug_get_travel_dir(d), mathf::pow(rng.value(), 1.f - 0.9f * step));
                if (w.dropbug_get_charging(d) > 0.f)
                    g.legs_travel_dirs[s][l] *= 0.f;
                body_parts::limb_update(lg, leg, p0, v0, p1, room);
                if (lg.mode == limb_mode::hunt_relative_position || g.legs_dangle_counter > 0)
                    lg.mode = limb_mode::dangle;
                vec2 const out = custom::deg_to_vec(body_deg + mathf::lerp(40.f, 160.f, t) * (wall != 0.f ? -wall : (s == 0 ? 1.f : -1.f)));
                vec2 const hip = p0 + rw::slerp(g.legs_travel_dirs[s][l], out, 0.1f) * g.leg_length * 0.85f * mathf::pow(step, 0.5f);
                body_parts::as_body_part(lg, leg.rad, [&](player::body_part& bp) {
                    body_parts::connect_to_point(bp, hip, g.leg_length, false, 0.f, v0, 0.1f, 0.f);
                    body_parts::connect_to_point(bp, p0, g.leg_length, false, 0.f, v0, 0.1f, 0.f);
                });
                body_parts::generic_update(knee, room, 0.99f, 0.5f);
                vec2 delta = custom::dir_vec(hip, knee.pos) * (g.leg_length * 0.55f - rw::distance(knee.pos, hip)) * 0.6f;
                knee.vel += delta;
                knee.pos += delta;
                delta = custom::dir_vec(lg.pos, knee.pos) * (g.leg_length * 0.55f - rw::distance(knee.pos, lg.pos)) * 0.6f;
                knee.vel += delta;
                knee.pos += delta;
                if (custom::dist_less(knee.pos, p0, 15.f)) {
                    delta = custom::dir_vec(p0, knee.pos) * (15.f - rw::distance(knee.pos, p0));
                    knee.vel += delta;
                    knee.pos += delta;
                }
                knee.vel = rw::lerp(knee.vel, v0, 0.8f);
                knee.vel += custom::perpendicular_vector(p1, p0) * mathf::lerp(s == 0 ? -1.f : 1.f, mathf::sign(g.flip), std::fabs(g.flip)) * 9.f;
                knee.vel += custom::rnv(rng) * 4.f * g.vibrate * rng.value();
                if (awake) {
                    vec2 (&dp)[3][2] = g.draw_positions;
                    dp[0][0] += custom::dir_vec(lg.pos, dp[0][0]) * 4.f * step;
                    dp[1][0] += custom::dir_vec(lg.pos, dp[1][0]) * 3.f * step;
                    dp[2][0] += custom::dir_vec(knee.pos, dp[2][0]) * 1.f * (1.f - step);
                }
                if (!custom::dist_less(knee.pos, hip, 200.f))
                    knee.pos = hip + custom::rnv(rng) * rng.value();
                if (g.legs_dangle_counter > 0 || step < 0.1f) {
                    vec2 a = hip + out * g.leg_length * 0.5f;
                    if (!awake)
                        a = hip + g.legs_travel_dirs[s][l] * g.leg_length * 0.5f;
                    else if (swimming)
                        a += custom::dir_vec(g.draw_positions[2][0], g.draw_positions[0][0]) * mathf::lerp(-18.f, 18.f, step);
                    lg.vel = rw::lerp(lg.vel, a - lg.pos, swimming ? 0.5f : 0.05f);
                    lg.vel.y -= 0.4f;
                } else {
                    vec2 goal = hip + out * g.leg_length;
                    // not on top of the opposite leg's foothold
                    for (int s2 = 0; s2 < 2; ++s2)
                        for (int l2 = 0; l2 < 2; ++l2) {
                            vec2 const other = g.legs[s2][l2].absolute_hunt_pos;
                            if (s2 != s && l2 != l && custom::dist_less(goal, other, g.leg_length * 0.1f))
                                goal = other + custom::dir_vec(other, goal) * g.leg_length * 0.1f;
                        }
                    constexpr float reach = 1.2f;
                    if (in_ceiling == 0.f) {
                        if (!lg.reached_snap_position)
                            body_parts::find_grip(lg, room, hip, hip, g.leg_length * reach, goal, -2, -2, true);
                        else if (!custom::dist_less(hip, lg.absolute_hunt_pos, g.leg_length * reach * mathf::pow(1.f - step, 0.2f)))
                            lg.mode = limb_mode::dangle;
                    }
                }
                ++n;
            }
        if (swimming) {
            vec2 const perp = custom::perpendicular_vector(g.draw_positions[0][0], g.draw_positions[2][0]);
            g.draw_positions[1][0] += perp * 5.f * std::sin(run_cycle * mathf::pi * 2.f);
            g.draw_positions[2][0] += perp * 5.f * std::sin((run_cycle + 0.25f) * mathf::pi * 2.f);
        }
        // hissing
        if (w.dropbug_get_voice(d) > 0) {
            g.draw_positions[0][0] += custom::rnv(rng) * rng.value() * 4.f;
            for (auto& a : g.antennae)
                a.pos += custom::rnv(rng) * rng.value() * 4.f;
        }
    }

    void build(sys::game_state const& state, dcon::dropbug_id d, float alpha, palette const& pal, std::vector<sprites::draw_cmd>& out) {
        auto const& w = state.world;
        auto const& g = w.dropbug_get_graphics(d);
        // UnityEngine.Random in DrawSprites: the body's shiver
        rw::rng rng{(state.tick * 2654435761u) ^ (uint32_t(d.index()) * 40503u) ^ 0x9E3779B9u};
        float const flip = mathf::lerp(g.last_flip, g.flip, alpha);
        float const vib = mathf::lerp(g.last_vibrate, g.vibrate, alpha);
        float const deep = mathf::lerp(g.last_deep_ceiling_mode, g.deep_ceiling_mode, alpha);
        float const ceiling = mathf::lerp(g.last_ceiling_mode, g.ceiling_mode, alpha);

        // ApplyPalette + RefreshColor (the darkness isn't lowered by light sources)
        vec3 const shine = mix(custom::hsl2rgb(g.hue, 0.55f, 0.65f), pal.fog, 0.25f + 0.75f * mathf::inverse_lerp(0.5f, 1.f, pal.darkness));
        vec3 const camo = mix(pal.black, mix(pal.camo, pal.fog, pal.fog_amount * 0.13333334f), 0.5f);
        float const camo_amount = custom::s_curve(mathf::inverse_lerp(0.1f, 0.5f, deep), 0.4f);
        vec3 const skin = mix(pal.black, camo, camo_amount);
        auto const rgba = [](vec3 c) { return vec4{c.x, c.y, c.z, 1.f}; };

        vec2 const head = rw::lerp(g.draw_positions[0][1], g.draw_positions[0][0], alpha);
        vec2 const mid = rw::lerp(g.draw_positions[1][1], g.draw_positions[1][0], alpha) + custom::rnv(rng) * rng.value() * 10.f * vib;
        vec2 const tail = rw::lerp(g.draw_positions[2][1], g.draw_positions[2][0], alpha);
        vec2 const dir = custom::dir_vec(mid, head);
        vec2 const perp = custom::perpendicular_vector(dir);
        vec2 const tail_end = rw::lerp(g.tail_end.last_pos, g.tail_end.pos, alpha);
        vec2 light = rw::normalized(pal.light_angle);
        light.y *= -1.f;

        // MeshSprite and ShineMeshSprite along a bezier from the head to the
        // tail end, the SegmentSprites across it
        vec2 mesh[48], shine_mesh[44];
        vec4 shine_cols[44];
        sprite segments[9];
        {
            vec2 prev = head + dir;
            float prev_w = 0.f, prev_shine_w = 0.f;
            vec2 prev_shine = head;
            for (int i = 0; i < 12; ++i) {
                float const f = mathf::inverse_lerp(0.f, 11.f, float(i));
                vec2 const p = custom::bezier(head + dir * 3.f, mid, tail_end, tail, f);
                float const wd = mathf::lerp(6.f, 2.f, f)
                    + mathf::pow(mathf::clamp01(std::sin(mathf::pow(f, 1.7f) * mathf::pi)), 0.75f) * mathf::lerp(7.f, 5.f, std::fabs(flip)) * (1.f + ceiling) * g.body_thickness;
                float sw = mathf::lerp(0.5f + mathf::pow(mathf::clamp01(std::sin(mathf::inverse_lerp(0.f, 10.f, float(i)) * mathf::pi)), 0.25f) * 2.f, wd * 0.5f, f * 0.5f);
                sw = mathf::lerp(sw, mathf::max(sw, wd * 0.5f), std::fabs(rw::dot(rw::normalized(p - prev), light)));
                vec2 const sp = p - light * (wd - sw);
                vec2 const a = custom::perpendicular_vector(p, prev);
                long_mesh_quad(mesh, i, prev, p, prev_w, wd);
                if (i < 11) {
                    shine_mesh[i * 4] = (prev_shine + sp) / 2.f - a * (sw + prev_shine_w) * 0.25f;
                    shine_mesh[i * 4 + 1] = (prev_shine + sp) / 2.f + a * (sw + prev_shine_w) * 0.25f;
                    shine_mesh[i * 4 + 2] = sp - a * sw;
                    shine_mesh[i * 4 + 3] = sp + a * sw;
                    if (i > 1) {
                        vec2 const l = p - a * wd, r = p + a * wd;
                        segments[i - 2] = sprite{"pixel", l, custom::aim(l, r), vec2{1.f, rw::distance(l, r)}, vec2{0.5f, 0.f}, skin};
                    }
                }
                prev = p;
                prev_w = wd;
                prev_shine = sp;
                prev_shine_w = sw;
            }
            for (int j = 0; j < 44; ++j)
                shine_cols[j] = rgba(mix(skin, shine, 0.25f * mathf::pow(mathf::clamp01(std::sin(mathf::pow(mathf::inverse_lerp(0.f, 43.f, float(j)), 4.f) * mathf::pi)), 0.5f) * (1.f - ceiling)));
        }

        // LegSprites: hip, knee and foot through two beziers
        vec2 legs[2][2][48];
        for (int s = 0; s < 2; ++s)
            for (int l = 0; l < 2; ++l) {
                float const t = float(l);
                vec2 hip = rw::lerp(head, mid, 0.3f);
                hip += perp * (s == 0 ? 1.f : -1.f) * 3.f * (1.f - std::fabs(flip));
                hip += dir * mathf::lerp(5.f, -11.f, t);
                vec2 const foot = rw::lerp(g.legs[s][l].last_pos, g.legs[s][l].pos, alpha);
                vec2 const knee = rw::lerp(g.knees[s][l].last_pos, g.knees[s][l].pos, alpha);
                vec2 upper = rw::lerp(hip, knee, 0.5f);
                vec2 lower = rw::lerp(knee, foot, 0.5f);
                constexpr float joint = 5.f;
                vec2 const c = rw::lerp(upper, lower, 0.5f);
                upper = c + custom::dir_vec(c, upper) * joint / 2.f;
                lower = c + custom::dir_vec(c, lower) * joint / 2.f;
                vec2 prev = hip;
                float prev_w = 2.f;
                for (int k = 0; k < 12; ++k) {
                    float const f = mathf::inverse_lerp(0.f, 11.f, float(k));
                    vec2 p;
                    if (f < 0.5f)
                        p = custom::bezier(hip, hip + custom::dir_vec(hip, foot) * 10.f, (lower + upper) / 2.f, upper + custom::dir_vec(lower, upper) * 7.f, mathf::inverse_lerp(0.f, 0.5f, f));
                    else
                        p = custom::bezier((lower + upper) / 2.f, lower + custom::dir_vec(upper, lower) * 7.f, foot, foot + custom::dir_vec(foot, hip) * 14.f, mathf::inverse_lerp(0.5f, 1.f, f));
                    float const wd = (mathf::lerp(4.f, 0.5f, mathf::pow(f, 0.25f)) + std::sin(mathf::pow(f, 2.5f) * mathf::pi) * 1.5f) * g.legs_thickness;
                    long_mesh_quad(legs[s][l], k, prev, p, prev_w, wd);
                    prev = p;
                    prev_w = wd;
                }
            }

        // MandibleSprites, WingSprites, PincherSprites, AntennaSprites
        sprite mandibles[2][2], wings[2];
        bool wing_visible[2];
        vec2 pinchers[2][32], antennae[2][32];
        vec4 antenna_cols[2][32];
        float const pinchers_flip = mathf::lerp(g.last_pinchers_flip, g.pinchers_flip, alpha);
        vec2 const tail_perp = custom::perpendicular_vector(tail, tail_end);
        for (int m = 0; m < 2; ++m) {
            float const side = m == 0 ? -1.f : 1.f;
            float const bend = mathf::lerp(-side, flip, mathf::pow(std::fabs(flip), 2.f));
            vec2 const root = head + dir * 4.f + perp * bend * -3.f;
            vec2 const tip = rw::lerp(g.mandibles[m].last_pos, g.mandibles[m].pos, alpha);
            vec2 const joint = custom::inverse_kinematic(root, tip, 16.f, 18.f, bend);
            mandibles[m][0] = sprite{"CentipedeLegA", root, custom::aim(root, joint), vec2{-mathf::sign(bend) * 2.f, rw::distance(root, joint) / mandible_element_height}, vec2{0.5f, 0.f}, skin};
            mandibles[m][1] = sprite{"CentipedeLegB", joint, custom::aim(joint, tip), vec2{-mathf::sign(bend) * 1.2f, rw::distance(joint, tip) / mandible_element_height}, vec2{0.5f, 0.f}, skin};

            vec2 const wv = custom::deg_to_vec(90.f * flip + side * 34.f);
            wing_visible[m] = wv.y >= 0.f;
            if (wing_visible[m]) {
                float const val = std::fabs(rw::dot(dir, custom::deg_to_vec(custom::vec_to_deg(light) - 90.f + (90.f * flip + side * 24.f) * 0.4f)));
                wings[m] = sprite{"CentipedeBackShell", rw::lerp(head, mid, 0.2f) - perp * 6.f * wv.x, custom::vec_to_deg(dir), vec2{wv.y * 0.4f, 0.8f}, vec2{0.5f, 0.5f},
                    mix(skin, shine, mathf::lerp(0.15f + custom::lerp_map(val, 0.55f, 1.f, 0.f, 0.25f, 3.f), 0.07f, deep))};
            }

            vec2 const pa = rw::lerp(tail, tail_end, 0.8f) + tail_perp * side * 2.f * (1.f - std::fabs(pinchers_flip));
            vec2 const pb = rw::lerp(g.pinchers[m].last_pos, g.pinchers[m].pos, alpha);
            vec2 prev = tail_end;
            float prev_w = 2.f;
            for (int k = 0; k < 8; ++k) {
                float const f = mathf::inverse_lerp(0.f, 7.f, float(k));
                vec2 p = rw::lerp(pa, pb, f);
                p += custom::perpendicular_vector(pa, pb) * side * std::sqrt(1.f - mathf::pow(1.f - f * 2.f, 2.f)) * custom::lerp_map(f, 0.1f, 0.4f, 0.f, 15.f) * (1.f - std::fabs(pinchers_flip));
                p += custom::perpendicular_vector(pa, pb) * pinchers_flip * mathf::inverse_lerp(0.5f, 1.f, f) * -7.f;
                float const wd = mathf::lerp(2.5f, 0.1f, f)
                    + mathf::pow(mathf::clamp01(std::sin(mathf::pow(f, mathf::lerp(3.f, 1.f, std::fabs(pinchers_flip))) * mathf::pi)), 3.f) * mathf::lerp(2.f, 1.f, std::fabs(pinchers_flip));
                long_mesh_quad(pinchers[m], k, prev, p, prev_w, wd);
                prev = p;
                prev_w = wd;
            }

            vec2 const at = rw::lerp(g.antennae[m].last_pos, g.antennae[m].pos, alpha);
            vec2 const ad = rw::normalized(custom::dir_vec(mid, head) + custom::perpendicular_vector(mid, head) * flip * 0.5f
                + custom::perpendicular_vector(mid, head) * side * (1.f - std::fabs(flip)) * 0.35f);
            prev = head;
            prev_w = 3.f;
            for (int k = 0; k < 8; ++k) {
                float const f = mathf::inverse_lerp(0.f, 7.f, float(k));
                vec2 const p = custom::bezier(head, head + ad * 30.f * g.antennae_length, at, at, f);
                float const wd = mathf::lerp(1.f, 0.5f, f);
                long_mesh_quad(antennae[m], k, prev, p, prev_w, wd);
                prev = p;
                prev_w = wd;
            }
            for (int k = 0; k < 32; ++k)
                antenna_cols[m][k] = rgba(mix(skin, shine, mathf::inverse_lerp(16.f, 31.f, float(k)) * g.colored_antennae * mathf::lerp(1.f, 0.75f, camo_amount)));
        }

        // in vanilla sprite order: LegSprites (0-3), MandibleSprites (4-7),
        // HeadSprite, MeshSprite, ShineMeshSprite, SegmentSprites (the
        // first one is never placed), PincherSprites, WingSprites, AntennaSprites
        for (int s = 0; s < 2; ++s)
            for (int l = 0; l < 2; ++l)
                sprites::long_mesh(out, legs[s][l], 48, skin);
        for (int m = 0; m < 2; ++m)
            for (int part = 0; part < 2; ++part)
                sprites::add(out, mandibles[m][part]);
        sprites::add(out, sprite{"Circle20", head, custom::vec_to_deg(dir), vec2{0.7f, 0.8f}, vec2{0.5f, 0.5f}, skin});
        sprites::long_mesh(out, mesh, 48, skin);
        sprites::long_mesh(out, shine_mesh, shine_cols, 44);
        for (auto const& sg : segments)
            sprites::add(out, sg);
        for (int m = 0; m < 2; ++m)
            sprites::long_mesh(out, pinchers[m], 32, skin);
        for (int m = 0; m < 2; ++m)
            if (wing_visible[m])
                sprites::add(out, wings[m]);
        for (int m = 0; m < 2; ++m) {
            if (g.colored_antennae > 0.f)
                sprites::long_mesh(out, antennae[m], antenna_cols[m], 32);
            else
                sprites::long_mesh(out, antennae[m], 32, skin);
        }
    }
}
