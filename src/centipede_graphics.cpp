#include <algorithm>
#include <cmath>

#include "centipede_graphics.hpp"
#include "body_parts.hpp"
#include "centipede.hpp"

namespace centipede_graphics {
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using world::room_view;
    using player::limb_mode;
    using sprites::sprite;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        // Limb(rad 2, surface friction 0.5, air friction 0.9, hunt speed 7, quickness 0.8)
        constexpr body_parts::limb_params leg{2.f, 0.5f, 0.9f, 7.f, 0.8f};
        // GenericBodyPart(rad 1, surface friction 0.5, air friction 0.9)
        constexpr float whisker_rad = 1.f, whisker_surface_friction = 0.5f, whisker_air_friction = 0.9f;

        bool consious(dcon::data_container const& w, dcon::creature_id c) {
            return w.creature_get_stun(c) <= 0 && !w.creature_get_dead(c);
        }

        vec2 chunk_pos(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_pos(centipede::chunk(state, c, i));
        }
        // ChunkDrawPos
        vec2 chunk_draw_pos(sys::game_state const& state, dcon::creature_id c, uint32_t i, float t) {
            auto const k = centipede::chunk(state, c, i);
            return rw::lerp(state.world.body_chunk_get_last_pos(k), state.world.body_chunk_get_pos(k), t);
        }

        // BodyPart.Reset
        template<typename P>
        void reset_part(P& bp, vec2 at, rw::rng& rng) {
            bp.pos = at + custom::deg_to_vec(rng.value() * 360.f);
            bp.last_pos = bp.pos;
            bp.vel = vec2{0.f, 0.f};
        }

        float whisker_length(float size, int part) {
            return (part == 0 ? 17.f : 43.f) * mathf::lerp(0.5f, 1.5f, size);
        }

        // RotatAtChunk
        vec2 rotat_at_chunk(centipede::graphics_state const& g, uint32_t n, uint32_t chunk, float t) {
            auto const rot = [&](int i) { return rw::slerp(g.body_rotations[i][1], g.body_rotations[i][0], t); };
            uint32_t const half = n / 2;
            if (chunk <= half)
                return rw::slerp(rot(0), rot(1), mathf::inverse_lerp(0.f, float(half), float(chunk)));
            return rw::slerp(rot(1), rot(2), mathf::inverse_lerp(float(half), float(n - 1), float(chunk)));
        }

        // WhiskerDir
        vec2 whisker_dir(sys::game_state const& state, dcon::creature_id c, centipede::graphics_state const& g, uint32_t n, int end, int side, int part, float t) {
            vec2 dir, rot;
            if (end == 0) {
                dir = custom::dir_vec(chunk_draw_pos(state, c, 1, t), chunk_draw_pos(state, c, 0, t));
                rot = rotat_at_chunk(g, n, 0, t);
            } else {
                dir = custom::dir_vec(chunk_draw_pos(state, c, n - 2, t), chunk_draw_pos(state, c, n - 1, t));
                rot = rotat_at_chunk(g, n, n - 1, t);
            }
            vec2 const a = custom::perpendicular_vector(dir) * (end == 0 ? -1.f : 1.f);
            return rw::normalized(dir + rw::slerp(a * (side == 0 ? -1.f : 1.f) * rot.y * (part == 0 ? 0.4f : 1.4f),
                a * rot.x * (part == 0 ? 0.25f : -0.5f), std::fabs(rot.x)));
        }

        // BestBodyRotatAtChunk: the side of the body facing away from the
        // surface it is on
        vec2 best_body_rotat_at_chunk(sys::game_state const& state, dcon::creature_id c, centipede::graphics_state const& g, uint32_t n, uint32_t chunk, room_view const& room) {
            vec2 v;
            if (chunk == 0)
                v = custom::dir_vec(chunk_pos(state, c, 0), chunk_pos(state, c, 1));
            else if (chunk == n - 1)
                v = custom::dir_vec(chunk_pos(state, c, n - 2), chunk_pos(state, c, n - 1));
            else
                v = custom::dir_vec(chunk_pos(state, c, n / 2 - 1), chunk_pos(state, c, n / 2 + 1));
            v = custom::perpendicular_vector(v);
            vec2 const p = chunk_pos(state, c, chunk);
            float num = 0.f;
            if (room.get_tile(p + v * 20.f).solid())
                num += 1.f;
            if (room.get_tile(p - v * 20.f).solid())
                num -= 1.f;
            if (num == 0.f && (room.get_tile(p).vertical_beam() || room.get_tile(p).horizontal_beam()))
                return rw::normalized(vec2{g.default_rotat * 0.01f, -1.f});
            return rw::slerp(rw::normalized(vec2{-1.f, 0.15f}), rw::normalized(vec2{1.f, 0.15f}), mathf::inverse_lerp(-1.f, 1.f, num + g.default_rotat * 0.01f));
        }
    }

    void reset(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        auto& g = w.centipede_data_get_graphics(creature::centipede_data(w, c));
        uint32_t const n = std::min(centipede::chunk_count(state, c), centipede::max_segments);
        g = centipede::graphics_state{};
        // vanilla seeds these from the creature's ID
        w.creature_get_graphics_random(c) = rw::rng{w.creature_get_random(c).state ^ 0x5bd1e995u};
        auto& rng = w.creature_get_graphics_random(c);
        g.default_rotat = mathf::lerp(-5.f, 5.f, rng.value());
        for (auto& r : g.body_rotations)
            r[0] = r[1] = custom::deg_to_vec(g.default_rotat);
        g.saturation = 0.9f;
        float const size = w.creature_get_payload(c).centipede.size;
        for (uint32_t i = 0; i < n; ++i) {
            float const t = float(i) / float(n - 1);
            g.leg_lengths[i] = mathf::lerp(10.f, 25.f, std::sin(t * mathf::pi)) * mathf::lerp(0.5f, 1.5f, size);
            for (auto& l : g.legs[i]) {
                l = player::hand_state{};
                reset_part(l, chunk_pos(state, c, i), rng);
                l.mode = limb_mode::hunt_relative_position;
                l.hunt_speed = leg.hunt_speed;
                l.quickness = leg.quickness;
                l.absolute_hunt_pos = l.pos;
            }
        }
        for (int end = 0; end < 2; ++end)
            for (auto& part : g.whiskers[end])
                for (auto& wh : part) {
                    wh.rad = whisker_rad;
                    reset_part(wh, chunk_pos(state, c, end == 0 ? 0 : n - 1), rng);
                }
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        auto const keep_random = w.creature_get_graphics_random(c);
        auto const keep = w.centipede_data_get_graphics(creature::centipede_data(w, c));
        reset(state, c);
        auto& g = w.centipede_data_get_graphics(creature::centipede_data(w, c));
        w.creature_set_graphics_random(c, keep_random);
        g.default_rotat = keep.default_rotat;
        for (auto& r : g.body_rotations)
            r[0] = r[1] = custom::deg_to_vec(g.default_rotat);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto& g = w.centipede_data_get_graphics(creature::centipede_data(w, c));
        uint32_t const n = std::min(centipede::chunk_count(state, c), centipede::max_segments);
        if (n < 2)
            return;
        bool const awake = consious(w, c);
        bool const moving = w.creature_get_payload(c).centipede.moving != 0;
        bool const body_direction = w.creature_get_payload(c).centipede.body_direction != 0;
        float const size = w.creature_get_payload(c).centipede.size;
        auto& rng = w.creature_get_graphics_random(c);

        if (moving && awake)
            g.walk_cycle += (body_direction ? -1.f : 1.f) / 10.f;
        g.last_body_dir = g.body_dir;
        g.body_dir = mathf::lerp(g.body_dir, body_direction ? -1.f : 1.f, 0.1f);
        for (int i = 0; i < 3; ++i) {
            g.body_rotations[i][1] = g.body_rotations[i][0];
            uint32_t const chunk = i == 0 ? 0 : (i == 1 ? n / 2 : n - 1);
            g.body_rotations[i][0] = rw::slerp(g.body_rotations[i][0], best_body_rotat_at_chunk(state, c, g, n, chunk, room), moving ? 0.4f : 0.01f);
        }

        // the legs reach for a grip, else dangle out to the sides
        vec2 const p = chunk_pos(state, c, 0) + custom::dir_vec(chunk_pos(state, c, 1), chunk_pos(state, c, 0));
        for (uint32_t j = 0; j < n; ++j) {
            auto const k = centipede::chunk(state, c, j);
            float const t = float(j) / float(n - 1);
            vec2 const pos = w.body_chunk_get_pos(k);
            vec2 const vel = w.body_chunk_get_vel(k);
            float const rad = w.body_chunk_get_rad(k);
            vec2 const dir = custom::dir_vec(p, pos);
            vec2 const perp = custom::perpendicular_vector(dir);
            vec2 const rot = rotat_at_chunk(g, n, j, 1.f);
            float const step = 0.5f + 0.5f * std::sin((g.walk_cycle + float(j) / 10.f) * mathf::pi * 2.f);
            float const len = g.leg_lengths[j];
            for (int s = 0; s < 2; ++s) {
                auto& l = g.legs[j][s];
                float const side = s == 0 ? -1.f : 1.f;
                body_parts::limb_update(l, leg, pos, vel, pos, room);
                vec2 const hip = pos + perp * side * rot.y * rad;
                float f = mathf::lerp(-1.f, 1.f, t);
                f = mathf::lerp(f, body_direction ? -1.f : 1.f, std::fabs(step - 0.5f));
                vec2 const reach = rw::normalized(rw::slerp(dir * f, rw::slerp(perp * side * rot.y, perp * rot.x, std::fabs(rot.x)),
                    mathf::lerp(0.5f + 0.5f * std::sin(t * mathf::pi), 0.f, std::fabs(step - 0.5f) * 2.f)));
                vec2 const goal = hip + reach * len;
                body_parts::as_body_part(l, leg.rad, [&](player::body_part& bp) { body_parts::connect_to_point(bp, hip, len, false, 0.f, vel, 0.1f, 0.f); });
                if (awake && !l.reached_snap_position)
                    body_parts::find_grip(l, room, hip, hip, len * 1.5f, goal, -2, -2, true);
                if (!awake || !custom::dist_less(l.pos, l.absolute_hunt_pos, len * 1.5f)) {
                    l.mode = limb_mode::dangle;
                    l.vel += reach * 13.f;
                    l.vel = rw::lerp(l.vel, goal - l.pos, 0.5f);
                } else {
                    l.vel += reach * 5.f;
                }
            }
        }
        // the feelers, twitching while it stands still
        for (int end = 0; end < 2; ++end) {
            vec2 const root = chunk_pos(state, c, end == 0 ? 0 : n - 1);
            for (int side = 0; side < 2; ++side)
                for (int part = 0; part < 2; ++part) {
                    auto& wh = g.whiskers[end][part][side];
                    float const len = whisker_length(size, part);
                    vec2 const d = whisker_dir(state, c, g, n, end, side, part, 1.f);
                    body_parts::generic_update(wh, room, whisker_air_friction, whisker_surface_friction);
                    body_parts::connect_to_point(wh, root, len, false, 0.f, vec2{0.f, 0.f}, 0.f, 0.f);
                    wh.vel += (root + d * len - wh.pos) / 30.f;
                    wh.vel += d;
                    wh.vel.y -= 0.3f;
                    if (awake && !moving)
                        wh.pos += custom::deg_to_vec(rng.value() * 360.f) * mathf::lerp(0.5f, 1.5f, size) * (body_direction == (end == 0) ? 2.f : 0.8f);
                }
        }
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, vec3 black, float darkness, std::vector<sprites::draw_cmd>& out) {
        auto const& w = state.world;
        auto const& g = w.centipede_data_get_graphics(creature::centipede_data(w, c));
        uint32_t const n = std::min(centipede::chunk_count(state, c), centipede::max_segments);
        if (n < 2)
            return;
        float const size = w.creature_get_payload(c).centipede.size;
        float const hue = w.centipede_data_get_hue(creature::centipede_data(w, c));
        auto const rad_of = [&](uint32_t i) { return w.body_chunk_get_rad(centipede::chunk(state, c, i)); };
        auto const black_to = [&](vec3 col, float t) { return col + (black - col) * mathf::clamp01(t); };
        vec3 const secondary_shell = black_to(custom::hsl2rgb(hue, g.saturation, 0.3f), 0.3f + 0.7f * darkness);
        // the sprites, gathered per kind and added in vanilla sprite order
        sprite secondary[centipede::max_segments], segments[centipede::max_segments], shells[centipede::max_segments], legs[centipede::max_segments][2][2];
        vec2 tube[centipede::max_segments * 4];

        vec2 prev_dir{0.f, 0.f};
        vec2 prev = chunk_draw_pos(state, c, 0, alpha);
        prev += custom::dir_vec(chunk_draw_pos(state, c, 1, alpha), prev) * 10.f;
        for (uint32_t i = 0; i < n; ++i) {
            float const t = float(i) / float(n - 1);
            float const rad = rad_of(i);
            vec2 const rot = rw::normalized(rotat_at_chunk(g, n, i, alpha));
            vec2 const pos = chunk_draw_pos(state, c, i, alpha);
            vec2 const next = i < n - 1 ? chunk_draw_pos(state, c, i + 1, alpha) : pos + custom::dir_vec(prev, pos) * 10.f;
            vec2 const dir = rw::normalized(prev - next);
            vec2 const perp = custom::perpendicular_vector(dir);
            float const d_prev = rw::distance(pos, prev) / 4.f;
            float const d_next = rw::distance(pos, next) / 4.f;
            float const w_prev = i == 0 ? 1.f : 3.f;
            float const w_next = i == 0 ? 3.f : (i == n - 1 ? 1.f : 3.f);
            tube[i * 4] = pos - perp * w_prev + dir * d_prev;
            tube[i * 4 + 1] = pos + perp * w_prev + dir * d_prev;
            tube[i * 4 + 2] = pos - perp * w_next - dir * d_next;
            tube[i * 4 + 3] = pos + perp * w_next - dir * d_next;
            float const flat = mathf::clamp(std::sin(t * mathf::pi), 0.f, 1.f) * mathf::lerp(1.f, 0.5f, size);
            float const deg = custom::vec_to_deg(dir);

            segments[i] = sprite{"CentipedeSegment", pos, deg,
                vec2{rad * mathf::lerp(1.f, mathf::lerp(1.5f, 0.9f, std::fabs(rot.x)), flat) * 2.f * 0.0625f, rad * 1.8f * 0.083333336f}, vec2{0.5f, 0.5f}, black};
            // the back shell while the back faces up, else the belly
            sprite& sh = shells[i];
            sh.rotation = deg;
            if (rot.y > 0.f) {
                float shine = mathf::inverse_lerp(-0.5f, 0.5f, rw::dot(dir, custom::deg_to_vec(30.f) * rot.x));
                shine *= mathf::max(mathf::inverse_lerp(0.3f, 0.05f, std::fabs(-0.5f - rot.x)), mathf::inverse_lerp(0.3f, 0.05f, std::fabs(0.5f - rot.x)));
                shine *= mathf::pow(1.f - darkness, 2.f);
                sh.element = "CentipedeBackShell";
                sh.scale = vec2{rad * mathf::lerp(1.f, mathf::lerp(1.5f, 0.9f, std::fabs(rot.x)), flat) * 1.8f * rot.y * 0.071428575f, rad * 1.5f * 0.09090909f};
                sh.color = black_to(custom::hsl2rgb(hue, g.saturation, 0.5f + 0.25f * shine), darkness);
                sh.pos = pos + perp * rot.x * rad * 1.1f;
            } else {
                sh.element = "CentipedeBellyShell";
                sh.scale = vec2{rad * -1.8f * rot.y * 0.071428575f, rad * 1.3f * 0.09090909f};
                sh.color = secondary_shell;
                sh.pos = pos - perp * rot.x * rad;
            }
            if (i > 0) {
                // a 1x1 pixel stretched over the joint
                secondary[i - 1] = sprite{"pixel", rw::lerp(prev, pos, 0.5f), custom::vec_to_deg(rw::slerp(prev_dir, dir, 0.5f)),
                    vec2{rad * mathf::lerp(0.9f, mathf::lerp(1.1f, 0.8f, std::fabs(rot.x)), flat) * 2.f, rad_of(i - 1)}, vec2{0.5f, 0.5f},
                    black_to(custom::hsl2rgb(hue, 1.f, 0.2f), mathf::lerp(0.4f, 1.f, darkness))};
            }
            prev = pos;
            prev_dir = dir;
            // two piece legs from the hip to the foot
            for (int s = 0; s < 2; ++s) {
                auto const& l = g.legs[i][s];
                vec2 const hip = pos - perp * (s == 0 ? -1.f : 1.f) * rot.y * rad;
                vec2 const foot = rw::lerp(l.last_pos, l.pos, alpha);
                float flip = mathf::lerp(-1.f, 1.f, mathf::clamp(t - g.body_dir * 0.4f, 0.f, 1.f)) * mathf::lerp(s == 0 ? 1.f : -1.f, -rot.x, std::fabs(rot.x));
                flip = mathf::pow(std::fabs(flip), 0.2f) * mathf::sign(flip);
                float const len = g.leg_lengths[i];
                vec2 const knee = custom::inverse_kinematic(hip, foot, len / 2.f, len / 2.f, flip);
                legs[i][s][0] = sprite{"CentipedeLegA", hip, custom::aim(hip, knee), vec2{-mathf::sign(flip), rw::distance(hip, knee) / 27.f}, vec2{0.5f, 0.1f}, black};
                legs[i][s][1] = sprite{"CentipedeLegB", knee, custom::aim(knee, foot), vec2{-mathf::sign(flip), rw::distance(knee, foot) / 25.f}, vec2{0.5f, 0.1f}, black};
            }
        }

        // TubeSprite, SecondarySegmentSprite, SegmentSprite, WhiskerSprite, LegSprite, ShellSprite
        sprites::long_mesh(out, tube, size_t(n) * 4, black);
        for (uint32_t i = 0; i + 1 < n; ++i)
            sprites::add(out, secondary[i]);
        for (uint32_t i = 0; i < n; ++i)
            sprites::add(out, segments[i]);
        for (int end = 0; end < 2; ++end) {
            vec2 const root = chunk_draw_pos(state, c, end == 0 ? 0 : n - 1, alpha);
            vec2 const out_dir = custom::dir_vec(chunk_draw_pos(state, c, end == 0 ? 1 : n - 2, alpha), root);
            for (int side = 0; side < 2; ++side)
                for (int part = 0; part < 2; ++part) {
                    auto const& wh = g.whiskers[end][part][side];
                    vec2 const tip = rw::lerp(wh.last_pos, wh.pos, alpha);
                    vec2 v[15];
                    vec2 at = root;
                    for (int k = 0; k < 4; ++k) {
                        vec2 const b = custom::bezier(root, root + out_dir * rw::distance(root, tip) * 0.7f, tip, tip, float(k) / 3.f);
                        vec2 const d = rw::normalized(b - at);
                        vec2 const a = custom::perpendicular_vector(d);
                        float const along = rw::distance(b, at) / (k == 0 ? 1.f : 5.f);
                        v[k * 4] = at - a + d * along;
                        v[k * 4 + 1] = at + a + d * along;
                        if (k < 3) {
                            v[k * 4 + 2] = b - a - d * along;
                            v[k * 4 + 3] = b + a - d * along;
                        } else {
                            v[k * 4 + 2] = b + d * 2.1f;
                        }
                        at = b;
                    }
                    sprites::long_mesh(out, v, 15, black);
                }
        }
        // CentipedeLegB is a VertexColorSprite: its top corners in the
        // secondary shell colour, the bottom ones black
        vec4 const top_col{secondary_shell.x, secondary_shell.y, secondary_shell.z, 1.f};
        vec4 const bottom_col{black.x, black.y, black.z, 1.f};
        vec4 const leg_b[4] = {top_col, top_col, bottom_col, bottom_col};
        for (uint32_t i = 0; i < n; ++i)
            for (int s = 0; s < 2; ++s) {
                sprites::add(out, legs[i][s][0]);
                sprites::add(out, legs[i][s][1], leg_b);
            }
        for (uint32_t i = 0; i < n; ++i)
            sprites::add(out, shells[i]);
    }
}
