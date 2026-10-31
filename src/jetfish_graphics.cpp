#include <cmath>
#include <string_view>

#include "jetfish_graphics.hpp"
#include "body_parts.hpp"
#include "creature.hpp"
#include "jetfish.hpp"
#include "physics.hpp"

namespace jetfish_graphics {
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using world::room_view;
    using sprites::sprite;
    using jetfish::graphics_state;
    using jetfish::tail_segments;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        // TailSegment(rad, 15, prev, surface friction 0.5, air friction 0.99,
        // affectPrevious 0.4, no pullInPreviousPosition)
        constexpr float tail_surface_friction = 0.5f, tail_air_friction = 0.99f, tail_affect_previous = 0.4f;
        // GenericBodyPart: flippers (1, 0.7, 0.99), whiskers (1, 0.6, 0.9)
        constexpr float flipper_surface_friction = 0.7f, flipper_air_friction = 0.99f;
        constexpr float whisker_surface_friction = 0.6f, whisker_air_friction = 0.9f;
        // the JetFishFlipper0..4 elements' sourcePixelSize, and
        // flipperGraphConPointHeights
        constexpr float flipper_width[5] = {22.f, 22.f, 21.f, 23.f, 19.f};
        constexpr float flipper_height[5] = {17.f, 15.f, 16.f, 18.f, 14.f};
        constexpr float flipper_con_point_height[5] = {10.f, 10.f, 11.f, 17.f, 9.f};
        constexpr std::string_view body_elements[9] = {"JetFish0", "JetFish1", "JetFish2", "JetFish3", "JetFish4", "JetFish5", "JetFish6", "JetFish7", "JetFish8"};
        constexpr std::string_view flipper_elements[5] = {"JetFishFlipper0", "JetFishFlipper1", "JetFishFlipper2", "JetFishFlipper3", "JetFishFlipper4"};

        graphics_state& gs(sys::game_state& state, dcon::creature_id c) {
            return state.world.jetfish_data_get_graphics(creature::jetfish_data(state.world, c));
        }
        graphics_state const& gs(sys::game_state const& state, dcon::creature_id c) {
            return state.world.jetfish_data_get_graphics(creature::jetfish_data(state.world, c));
        }
        dcon::body_chunk_id ch(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return jetfish::chunk(state, c, i);
        }

        // BodyPart.Reset
        void reset_part(player::body_part& bp, vec2 at, rw::rng& rng) {
            bp.pos = at + custom::rnv(rng);
            bp.last_pos = bp.pos;
            bp.vel = vec2{0.f, 0.f};
        }

        // TailSegment.Update: after the segment before it, or the point
        void tail_update(player::body_part& bp, float connection_rad, player::body_part* prev, vec2 point, room_view const& room) {
            bp.last_pos = bp.pos;
            bp.pos += bp.vel;
            bp.vel *= tail_air_friction;
            bp.stretched = 1.f;
            if (prev) {
                if (!custom::dist_less(bp.pos, prev->pos, connection_rad)) {
                    float const num = rw::distance(bp.pos, prev->pos);
                    vec2 const a = custom::dir_vec(bp.pos, prev->pos);
                    bp.pos -= a * (connection_rad - num) * (1.f - tail_affect_previous);
                    bp.vel -= a * (connection_rad - num) * (1.f - tail_affect_previous);
                    prev->vel += a * (connection_rad - num) * tail_affect_previous;
                    bp.stretched = mathf::clamp((connection_rad / (num * 0.5f) + 2.f) / 3.f, 0.2f, 1.f);
                }
            } else if (!custom::dist_less(bp.pos, point, connection_rad)) {
                float const num = rw::distance(bp.pos, point);
                vec2 const a = custom::dir_vec(bp.pos, point);
                bp.pos -= a * (connection_rad - num);
                bp.vel -= a * (connection_rad - num);
                bp.stretched = mathf::clamp((connection_rad / (num * 0.5f) + 2.f) / 3.f, 0.2f, 1.f);
            }
            body_parts::push_out_of_terrain(bp, room, tail_surface_friction);
        }

        // JetFishGraphics.whiskerDir
        vec2 whisker_dir(graphics_state const& g, uint32_t side, uint32_t m, vec2 z_rot, vec2 body_dir) {
            vec2 const v{(side == 0 ? -1.f : 1.f) * std::fabs(z_rot.y) * g.whisker_directions[m].x + z_rot.x * g.whisker_props[m][1], g.whisker_directions[m].y};
            return body_parts::rotate_around_origo(rw::normalized(v), custom::vec_to_deg(body_dir));
        }

        // JetFishGraphics.TentacleContour
        float tentacle_contour(uint8_t contour, float f, uint32_t i) {
            if (contour == 0)
                return mathf::lerp(5.f, 1.f, mathf::pow(f, 0.5f));
            if (contour != 1)
                return mathf::lerp(4.f, 1.f, mathf::pow(f, 1.5f));
            if (i == tail_segments - 2)
                return 3.5f;
            return mathf::lerp(4.f, 1.f, mathf::pow(f, 0.5f));
        }

        vec3 lerp3(vec3 a, vec3 b, float t) {
            t = mathf::clamp01(t);
            return a + (b - a) * t;
        }
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        auto& g = gs(state, c);
        auto const& iv = jetfish::ivars(state, c);
        float const keep_swim = g.swim;
        vec2 const keep_z = g.z_rotation;
        float const keep_eyes = g.air_eyes;
        g = graphics_state{};
        g.swim = keep_swim;
        g.z_rotation = g.last_z_rotation = keep_z;
        g.air_eyes = keep_eyes;
        g.flipper_graph_width = flipper_width[iv.flipper % 5];
        // the constructor: the tentacles' radii and connections, the whiskers'
        // layout from iVars.whiskerSeed (its parts' Reset rolls come first)
        for (uint32_t i = 0; i < 2; ++i)
            for (uint32_t j = 0; j < tail_segments; ++j) {
                g.tails[i][j].rad = mathf::lerp(3.f, 1.f, float(j) / float(tail_segments - 1));
                g.tail_connection_rad[i][j] = 15.f;
            }
        {
            rw::unity_random r{iv.whisker_seed};
            for (uint32_t k = 0; k < iv.whiskers && k < jetfish::max_whiskers; ++k) {
                r.value();
                r.value();
                g.whisker_directions[k] = custom::deg_to_vec(mathf::lerp(4.f, 80.f, r.value()));
                g.whisker_props[k][0] = mathf::lerp(7.f, 40.f, mathf::pow(r.value(), 2.f));
                g.whisker_props[k][1] = mathf::lerp(-1.f, 0.5f, r.value());
                g.whisker_props[k][2] = mathf::lerp(5.f, 720.f, mathf::pow(r.value(), 1.5f)) / g.whisker_props[k][0];
                g.whisker_props[k][3] = r.value();
                g.whisker_props[k][4] = mathf::lerp(0.6f, 1.6f, mathf::pow(r.value(), 2.f));
            }
        }
        // GraphicsModule.Reset: every body part on the first chunk
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        vec2 const at = w.body_chunk_get_pos(ch(state, c, 0));
        for (uint32_t i = 0; i < 2; ++i) {
            for (uint32_t j = 0; j < tail_segments; ++j)
                reset_part(g.tails[i][j], at, rng);
            reset_part(g.flippers[i], at, rng);
            for (uint32_t k = 0; k < jetfish::max_whiskers; ++k)
                reset_part(g.whiskers[i][k], at, rng);
        }
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto& g = gs(state, c);
        auto const& iv = jetfish::ivars(state, c);
        auto const& x = w.creature_get_payload(c).jetfish;
        bool const awake = jetfish::consious(state, c);
        auto const k0 = ch(state, c, 0), k1 = ch(state, c, 1);
        vec2 const c0 = w.body_chunk_get_pos(k0), c1 = w.body_chunk_get_pos(k1);
        if (awake)
            g.swim -= custom::lerp_map(x.swim_speed, 1.6f, 5.f, 0.033333335f, 0.06666667f);
        vec2 const vector = custom::dir_vec(c1, c0);
        vec2 const a = custom::perpendicular_vector(vector);
        g.last_z_rotation = g.z_rotation;
        if (std::fabs(vector.x) > 0.5f && awake)
            g.z_rotation = rw::lerp(g.z_rotation, vec2{vector.x > 0.f ? -1.f : 1.f, 0.f}, 0.2f);
        else
            g.z_rotation = rw::lerp(g.z_rotation, -vector, 0.5f);
        g.z_rotation = rw::normalized(g.z_rotation);
        if (awake) {
            float const num = 1.f - physics::submersion(state, room, k1);
            if (g.air_eyes < num)
                g.air_eyes = mathf::min(g.air_eyes + 0.1f, num);
            else
                g.air_eyes = mathf::max(g.air_eyes - 0.033333335f, num);
        }
        uint32_t const whiskers = iv.whiskers < jetfish::max_whiskers ? iv.whiskers : jetfish::max_whiskers;
        float const reach = (g.flipper_graph_width + 7.f) * iv.flipper_size;
        for (uint32_t i = 0; i < 2; ++i) {
            auto& f = g.flippers[i];
            f.rad = 1.f;
            body_parts::generic_update(f, room, flipper_air_friction, flipper_surface_friction);
            body_parts::connect_to_point(f, c1, reach, false, 0.f, w.body_chunk_get_vel(k1), 0.3f, 0.f);
            vec2 a2 = a * g.z_rotation.y * (i == 0 ? -1.f : 1.f);
            a2 += vec2{0.f, -0.5f} * std::fabs(g.z_rotation.x);
            a2 += vector * iv.flipper_orientation * 1.5f;
            if (awake) {
                if ((i == 0) == (g.z_rotation.x < 0.f))
                    a2 += vector * std::sin(g.swim * mathf::pi * 2.f) * 0.3f * (1.f - x.jet_active);
                else
                    a2 += vector * std::cos(g.swim * mathf::pi * 2.f) * 0.3f * (1.f - x.jet_active);
                a2 = rw::lerp(a2, -vector, x.jet_active * x.jet_water);
            }
            f.vel += (c1 + a2 * reach - f.pos) / (awake ? 8.f : 16.f);
            if (room.point_submerged(f.pos))
                f.vel *= 0.9f;
            else
                f.vel.y -= 0.6f;
            for (uint32_t j = 0; j < whiskers; ++j) {
                auto& wh = g.whiskers[i][j];
                wh.rad = 1.f;
                wh.vel += whisker_dir(g, i, j, g.z_rotation, vector) * g.whisker_props[j][2];
                if (room.point_submerged(wh.pos))
                    wh.vel *= 0.8f;
                else
                    wh.vel.y -= 0.6f;
                body_parts::generic_update(wh, room, whisker_air_friction, whisker_surface_friction);
                body_parts::connect_to_point(wh, c0 + vector * 5.f + whisker_dir(g, i, j, g.z_rotation, vector) * 5.f, g.whisker_props[j][0], false, 0.f,
                    w.body_chunk_get_vel(k0), 0.f, 0.f);
            }
            for (uint32_t k = 0; k < tail_segments; ++k) {
                auto& t = g.tails[i][k];
                tail_update(t, g.tail_connection_rad[i][k], k > 0 ? &g.tails[i][k - 1] : nullptr, c1, room);
                float const num2 = mathf::inverse_lerp(0.f, float(tail_segments - 1), float(k));
                if (!custom::dist_less(t.pos, c1, 15.f * float(k + 1)))
                    t.pos = c1 + custom::dir_vec(c1, t.pos) * 15.f * float(k + 1);
                float num3 = x.jet_active;
                if (room.point_submerged(t.pos)) {
                    t.vel *= 0.7f;
                    num3 = mathf::lerp(num3, 0.f, 0.5f);
                } else {
                    t.vel.y -= 0.9f * mathf::pow(float(k) / float(tail_segments - 1), 3.f);
                }
                t.vel += a * std::sin((g.swim + float(k) / 5.f) * mathf::pi * 2.f) * (i == 0 ? 1.f : -1.f) * mathf::pow(1.f - num2, 2.f)
                    * custom::lerp_map(x.swim_speed, 1.6f, 5.f, 8.f, 16.f) * (1.f - num3);
                t.vel -= vector * (0.2f * (1.f - num3) + mathf::pow(mathf::inverse_lerp(0.5f, 0.f, num2), 2.f) * mathf::lerp(27.f, 11.f, num3));
                float const num4 = 30.f + std::sin(num2 * mathf::pi * -2.f) * -100.f;
                t.vel -= custom::deg_to_vec(custom::aim(c1, c0) + num4 * (i == 0 ? 1.f : -1.f)) * mathf::lerp(12.f, 6.f, num2) * num3;
                g.tail_connection_rad[i][k] = mathf::lerp(10.f, 0.5f, mathf::lerp(0.f, num3, mathf::pow(num2, 0.2f))) * mathf::lerp(0.5f, 1.5f, iv.tentacle_length);
                t.rad = mathf::lerp(tentacle_contour(iv.tentacle_contour, num2, k), mathf::lerp(8.f, 2.f, num2) * (0.5f + 0.5f * x.jet_water), num3)
                    * mathf::lerp(0.7f, 1.2f, iv.tentacle_fatness);
            }
        }
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip) {
        auto const& w = state.world;
        auto const& g = gs(state, c);
        auto const& iv = jetfish::ivars(state, c);
        bool const albino = w.creature_get_payload(c).jetfish.albino != 0;
        auto const room = world::get_room_view(state, creature::room_of(state, c));
        auto const k0 = ch(state, c, 0), k1 = ch(state, c, 1);
        float const t = alpha;
        // ApplyPalette
        vec3 color = pal.black();
        if (albino)
            color = lerp3(color, vec3{1.f, 1.f, 1.f}, 0.87f);
        vec3 const dark_eye{0.f, 0.003921569f, 0.f};
        uint32_t const whiskers = iv.whiskers < jetfish::max_whiskers ? iv.whiskers : jetfish::max_whiskers;

        vec2 const vector = rw::slerp(g.last_z_rotation, g.z_rotation, t);
        vec2 const p0 = rw::lerp(w.body_chunk_get_last_pos(k0), w.body_chunk_get_pos(k0), t);
        vec2 const p1 = rw::lerp(w.body_chunk_get_last_pos(k1), w.body_chunk_get_pos(k1), t);
        vec2 const vector2 = rw::lerp(p0, p1, 0.3f);
        vec2 const normalized = rw::normalized(p0 - p1);
        vec2 const a = custom::perpendicular_vector(-normalized);
        float const num = custom::aim(normalized, -normalized);
        float const num2 = custom::aim(vec2{0.f, 0.f}, vector);
        int32_t const num3 = custom::int_clamp(8 - int32_t(std::fabs(num2 / 180.f) * 9.f), 0, 8);
        float const num4 = float(8 - num3) * mathf::sign(num2) * 22.5f;
        float const cos2 = std::cos(num2 / 360.f * mathf::pi * 2.f);

        // the eyes' places
        vec2 eye[2];
        for (uint32_t i = 0; i < 2; ++i)
            eye[i] = vector2 + normalized * 13.f + a * cos2 * mathf::lerp(7.5f, 5.f, g.air_eyes) * (((i == 0) == (num2 > 0.f)) ? -1.f : 1.f);
        float const eye_a_scale = mathf::lerp(1.f, 0.4f, g.air_eyes);
        float const eye_b_scale = mathf::lerp(1.f, 0.f, mathf::pow(g.air_eyes, 0.4f));

        // BehindEyeSprite
        sprites::add(out, sprite{"JetFishEyeA", eye[0], 0.f, vec2{eye_a_scale, eye_a_scale}, vec2{0.5f, 0.5f},
                              room.point_submerged(eye[0] + vec2{0.f, -5.f}) ? dark_eye : iv.eye_color});
        // BodySprite ("pixel" scaled by the fatness, then the element)
        sprites::add(out, sprite{body_elements[num3], vector2, num - num4, vec2{num2 > 0.f ? -1.f : 1.f, mathf::lerp(0.8f, 1.f, iv.fatness)}, vec2{0.5f, 0.5f}, color});

        // WhiskerSprite(side, whisker): MakeLongMesh(4, pointy)
        for (uint32_t k = 0; k < whiskers; ++k)
            for (uint32_t l = 0; l < 2; ++l) {
                vec2 vs[15];
                vec4 cols[15];
                vec2 const v6 = rw::lerp(g.whiskers[l][k].last_pos, g.whiskers[l][k].pos, t);
                vec2 a3 = whisker_dir(g, l, k, vector, normalized);
                vec2 const v7 = p0 + normalized * mathf::lerp(10.f, 5.f, g.whisker_props[k][3]) + a3 * 5.f * g.whisker_props[k][3];
                a3 = rw::normalized(a3 + normalized);
                vec2 v4 = v7;
                float num5 = g.whisker_props[k][4];
                float num6 = 1.f;
                for (uint32_t m = 0; m < 4; ++m) {
                    vec2 v8;
                    if (m < 3) {
                        v8 = rw::lerp(v7, v6, float(m + 1) / 4.f);
                        v8 += a3 * num6 * g.whisker_props[k][0] * 0.2f;
                    } else {
                        v8 = v6;
                    }
                    num6 *= 0.7f;
                    vec2 const n3 = rw::normalized(v8 - v4);
                    vec2 const a4 = custom::perpendicular_vector(n3);
                    float const d2 = rw::distance(v8, v4) / (m == 0 ? 1.f : 5.f);
                    float const num7 = custom::lerp_map(float(m), 0.f, 3.f, g.whisker_props[k][4], 0.5f);
                    vs[m * 4] = v4 - a4 * (num7 + num5) * 0.5f + n3 * d2;
                    vs[m * 4 + 1] = v4 + a4 * (num7 + num5) * 0.5f + n3 * d2;
                    if (m < 3) {
                        vs[m * 4 + 2] = v8 - a4 * num7 - n3 * d2;
                        vs[m * 4 + 3] = v8 + a4 * num7 - n3 * d2;
                    } else {
                        vs[m * 4 + 2] = v8 + n3 * 2.1f;
                    }
                    num5 = num7;
                    v4 = v8;
                }
                for (uint32_t v = 0; v < 15; ++v) {
                    vec3 col = color;
                    if (albino)
                        col = lerp3(color, vec3{1.f, 0.f, 0.f}, mathf::pow(mathf::inverse_lerp(3.f, 14.f, float(v)), 0.5f) * 0.65f);
                    cols[v] = vec4{col.x, col.y, col.z, 1.f};
                }
                sprites::long_mesh(out, strip, vs, cols, 15);
            }

        // EyeSprite(eye, part): A coloured, B the pupil in the palette's black
        for (uint32_t i = 0; i < 2; ++i) {
            if (i != 0 || std::fabs(cos2) > 0.9f)
                sprites::add(out, sprite{"JetFishEyeA", eye[i], 0.f, vec2{eye_a_scale, eye_a_scale}, vec2{0.5f, 0.5f},
                                      room.point_submerged(eye[i] + vec2{0.f, 5.f}) ? dark_eye : iv.eye_color});
            sprites::add(out, sprite{"JetFishEyeB", eye[i], 0.f, vec2{eye_b_scale, eye_b_scale}, vec2{0.5f, 0.5f}, color});
        }

        // TentacleSprite: MakeLongMesh(6, pointy)
        constexpr uint32_t tn = (tail_segments - 1) * 4 + 3;
        for (uint32_t i = 0; i < 2; ++i) {
            vec2 vs[tn];
            vec4 cols[tn];
            vec2 v4 = p1;
            float num5 = w.body_chunk_get_rad(k1);
            for (uint32_t j = 0; j < tail_segments; ++j) {
                auto const& seg = g.tails[i][j];
                vec2 const v5 = rw::lerp(seg.last_pos, seg.pos, t);
                vec2 const n2 = rw::normalized(v5 - v4);
                vec2 const a2 = custom::perpendicular_vector(n2);
                float const d = rw::distance(v5, v4) / 5.f;
                float const sr = seg.rad * seg.stretched;
                vs[j * 4] = v4 - a2 * (num5 + sr) * 0.5f + n2 * d;
                vs[j * 4 + 1] = v4 + a2 * (num5 + sr) * 0.5f + n2 * d;
                if (j < tail_segments - 1) {
                    vs[j * 4 + 2] = v5 - a2 * sr - n2 * d;
                    vs[j * 4 + 3] = v5 + a2 * sr - n2 * d;
                } else {
                    vs[j * 4 + 2] = v5;
                }
                num5 = sr;
                v4 = v5;
            }
            for (uint32_t v = 0; v < tn; ++v) {
                vec3 col = color;
                if (albino)
                    col = lerp3(color, vec3{1.f, 0.f, 0.f}, mathf::pow(mathf::inverse_lerp(3.f, float(tn - 1), float(v)), 2.f) * 0.5f);
                cols[v] = vec4{col.x, col.y, col.z, 1.f};
            }
            sprites::long_mesh(out, strip, vs, cols, tn);
        }

        // FlipperSprite: anchored at its connection point
        uint32_t const fl = iv.flipper % 5;
        for (uint32_t i = 0; i < 2; ++i) {
            vec2 const fp = rw::lerp(g.flippers[i].last_pos, g.flippers[i].pos, t);
            float const sy = mathf::sign(custom::distance_to_line(fp, vector2 - normalized, vector2 + normalized)) * iv.flipper_size;
            float const sx = mathf::lerp(rw::distance(fp, vector2) / g.flipper_graph_width, iv.flipper_size, 0.5f);
            sprites::add(out, sprite{flipper_elements[fl], fp, custom::aim(fp, vector2) - 90.f, vec2{sx, sy},
                                  vec2{0.f, flipper_con_point_height[fl] / flipper_height[fl]}, color});
        }
    }
}
