#include <array>
#include <cmath>
#include <string>

#include "player_graphics.hpp"
#include "physics.hpp"
#include "player.hpp"

namespace player_graphics {
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    using world::terrain;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        // GenericBodyPart / TailSegment constants (PlayerGraphics constructor)
        constexpr float head_rad = 4.f, head_surface_friction = 0.8f, head_air_friction = 0.99f;
        constexpr float legs_rad = 1.f, legs_surface_friction = 0.8f, legs_air_friction = 0.99f;
        constexpr float tail_rad[4] = {6.f, 4.f, 2.5f, 1.f};
        constexpr float tail_connection_rad[4] = {4.f, 7.f, 7.f, 7.f};
        constexpr float tail_affect_previous[4] = {1.f, 0.5f, 0.5f, 0.5f};
        constexpr float tail_surface_friction = 0.85f, tail_air_friction = 1.f;
        constexpr ivec2 eight_directions_and_zero[9] = {
            {0, 0}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}
        };

        // Custom.AimFromOneVectorToAnother: degrees, clockwise from up
        float aim(vec2 a, vec2 b) {
            return std::atan2(b.x - a.x, b.y - a.y) / 6.2831855f * 360.f;
        }

        // element names with stable storage (sprites refer to them by view)
        template<size_t N>
        std::array<std::string, N> make_names(char const* prefix) {
            std::array<std::string, N> r;
            for (size_t i = 0; i < N; ++i)
                r[i] = prefix + std::to_string(i);
            return r;
        }
        auto const head_names = make_names<18>("HeadA");
        auto const face_a_names = make_names<9>("FaceA");
        auto const face_b_names = make_names<9>("FaceB");
        auto const legs_names = make_names<7>("LegsA");
        auto const legs_crawling_names = make_names<6>("LegsACrawling");
        auto const legs_climbing_names = make_names<7>("LegsAClimbing");
        auto const legs_on_pole_names = make_names<7>("LegsAOnPole");

        // BodyPart.ConnectToPoint
        void connect_to_point(player::body_part& bp, vec2 pnt, float connection_rad, bool push, float elastic, vec2 host_vel, float adapt, float exaggerate) {
            if (elastic > 0.f)
                bp.vel += custom::dir_vec(bp.pos, pnt) * rw::distance(bp.pos, pnt) * elastic;
            bp.vel += host_vel * exaggerate;
            if (push || !custom::dist_less(bp.pos, pnt, connection_rad)) {
                float const d = rw::distance(bp.pos, pnt);
                vec2 const a = custom::dir_vec(bp.pos, pnt);
                bp.pos -= a * (connection_rad - d);
                bp.vel -= a * (connection_rad - d);
            }
            bp.vel -= host_vel;
            bp.vel *= 1.f - adapt;
            bp.vel += host_vel;
        }

        // BodyPart.PushOutOfTerrain (tile terrain only)
        void push_out_of_terrain(player::body_part& bp, room_view const& room, float surface_friction) {
            for (auto const d : eight_directions_and_zero) {
                ivec2 const t = room_view::get_tile_position(bp.pos) + d;
                auto const tt = room.get_terrain(t.x, t.y);
                if (tt == terrain::solid) {
                    vec2 const m = room_view::middle_of_tile(t);
                    float nx = 0.f, ny = 0.f;
                    if (bp.pos.y >= m.y - 10.f && bp.pos.y <= m.y + 10.f) {
                        if (bp.last_pos.x < m.x) {
                            if (bp.pos.x > m.x - 10.f - bp.rad && room.get_terrain(t.x - 1, t.y) != terrain::solid)
                                nx = m.x - 10.f - bp.rad;
                        } else if (bp.pos.x < m.x + 10.f + bp.rad && room.get_terrain(t.x + 1, t.y) != terrain::solid) {
                            nx = m.x + 10.f + bp.rad;
                        }
                    }
                    if (bp.pos.x >= m.x - 10.f && bp.pos.x <= m.x + 10.f) {
                        if (bp.last_pos.y < m.y) {
                            if (bp.pos.y > m.y - 10.f - bp.rad && room.get_terrain(t.x, t.y - 1) != terrain::solid)
                                ny = m.y - 10.f - bp.rad;
                        } else if (bp.pos.y < m.y + 10.f + bp.rad && room.get_terrain(t.x, t.y + 1) != terrain::solid) {
                            ny = m.y + 10.f + bp.rad;
                        }
                    }
                    if (std::fabs(bp.pos.x - nx) < std::fabs(bp.pos.y - ny) && nx != 0.f) {
                        bp.pos.x = nx;
                        bp.vel.x = nx - bp.pos.x;
                        bp.vel.y *= surface_friction;
                    } else if (ny != 0.f) {
                        bp.pos.y = ny;
                        bp.vel.y = ny - bp.pos.y;
                        bp.vel.x *= surface_friction;
                    } else {
                        vec2 const c{mathf::clamp(bp.pos.x, m.x - 10.f, m.x + 10.f), mathf::clamp(bp.pos.y, m.y - 10.f, m.y + 10.f)};
                        if (custom::dist_less(bp.pos, c, bp.rad)) {
                            float const dist = rw::distance(bp.pos, c);
                            vec2 const a = custom::dir_vec(bp.pos, c);
                            bp.vel *= surface_friction;
                            bp.pos -= a * (bp.rad - dist);
                            bp.vel -= a * (bp.rad - dist);
                        }
                    }
                } else if (d.x == 0 && tt == terrain::slope) {
                    vec2 const m = room_view::middle_of_tile(t);
                    auto const slope = room.identify_slope(t);
                    using world::slope_direction;
                    float target = 0.f;
                    bool hit = false;
                    if (slope == slope_direction::up_left) {
                        target = m.y - (m.x - bp.pos.x) + bp.rad;
                        hit = bp.pos.y < target;
                    } else if (slope == slope_direction::up_right) {
                        target = m.y + (m.x - bp.pos.x) + bp.rad;
                        hit = bp.pos.y < target;
                    } else if (slope == slope_direction::down_left) {
                        target = m.y + (m.x - bp.pos.x) - bp.rad;
                        hit = bp.pos.y > target;
                    } else if (slope == slope_direction::down_right) {
                        target = m.y - (m.x - bp.pos.x) - bp.rad;
                        hit = bp.pos.y > target;
                    }
                    if (hit) {
                        bp.pos.y = target;
                        bp.vel.y = 0.f;
                        bp.vel.x *= surface_friction;
                    }
                }
            }
        }

        // GenericBodyPart.Update
        void generic_update(player::body_part& bp, room_view const& room, float air_friction, float surface_friction) {
            bp.last_pos = bp.pos;
            bp.pos += bp.vel;
            bp.vel *= air_friction;
            push_out_of_terrain(bp, room, surface_friction);
        }

        // TailSegment.Update; segment 0 hangs from `point`
        void tail_update(player::graphics_state& g, int i, vec2 point, room_view const& room) {
            auto& t = g.tail[i];
            t.last_pos = t.pos;
            t.pos += t.vel;
            t.vel *= tail_air_friction;
            t.stretched = 1.f;
            float const cr = tail_connection_rad[i];
            if (i > 0) {
                auto& prev = g.tail[i - 1];
                if (!custom::dist_less(t.pos, prev.pos, cr)) {
                    float const d = rw::distance(t.pos, prev.pos);
                    vec2 const a = custom::dir_vec(t.pos, prev.pos);
                    float const ap = tail_affect_previous[i];
                    t.pos -= a * ((cr - d) * (1.f - ap));
                    t.vel -= a * ((cr - d) * (1.f - ap));
                    prev.pos += a * ((cr - d) * ap);
                    prev.vel += a * ((cr - d) * ap);
                    t.stretched = mathf::clamp((cr / (d * 0.5f) + 2.f) / 3.f, 0.2f, 1.f);
                }
            } else if (!custom::dist_less(t.pos, point, cr)) {
                float const d = rw::distance(t.pos, point);
                vec2 const a = custom::dir_vec(t.pos, point);
                t.pos -= a * (cr - d);
                t.vel -= a * (cr - d);
                t.stretched = mathf::clamp((cr / (d * 0.5f) + 2.f) / 3.f, 0.2f, 1.f);
            }
            push_out_of_terrain(t, room, tail_surface_friction);
        }

        bool consious(dcon::data_container const& w, dcon::player_id p) {
            return w.player_get_stun(p) <= 0 && !w.player_get_dead(p);
        }
    }

    void reset(sys::game_state& state, dcon::player_id p) {
        auto& w = state.world;
        auto& g = w.player_get_graphics(p);
        vec2 const c0 = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
        vec2 const c1 = w.body_chunk_get_pos(player::body_chunk(state, p, 1));
        g = player::graphics_state{};
        g.draw_pos[0] = g.last_draw_pos[0] = c0;
        g.draw_pos[1] = g.last_draw_pos[1] = c1;
        auto const place = [](player::body_part& bp, vec2 at, float rad) {
            bp.pos = bp.last_pos = at;
            bp.vel = vec2{0.f, 0.f};
            bp.rad = rad;
            bp.stretched = 1.f;
        };
        place(g.head, c0, head_rad);
        place(g.legs, c1, legs_rad);
        for (int i = 0; i < 4; ++i)
            place(g.tail[i], c1 - vec2{0.f, float(i + 1) * 4.f}, tail_rad[i]);
        g.initialized = 1;
    }

    void update(sys::game_state& state, dcon::player_id p, room_view const& room) {
        auto& w = state.world;
        auto& g = w.player_get_graphics(p);
        if (!g.initialized)
            reset(state, p);
        auto const b0 = player::body_chunk(state, p, 0);
        auto const b1 = player::body_chunk(state, p, 1);
        vec2 const c0 = w.body_chunk_get_pos(b0);
        vec2 const c1 = w.body_chunk_get_pos(b1);
        vec2 const v0 = w.body_chunk_get_vel(b0);
        vec2 const v1 = w.body_chunk_get_vel(b1);
        auto const mode = w.player_get_body_mode(p);
        auto const anim = w.player_get_animation(p);
        int32_t const flip = w.player_get_flip_direction(p);
        float const frame = float(w.player_get_animation_frame(p));
        float const sub1 = physics::submersion(state, room, b1);
        float const gravity = room.gravity;

        for (int i = 0; i < 2; ++i)
            g.last_draw_pos[i] = g.draw_pos[i];
        g.draw_pos[0] = c0;
        g.draw_pos[1] = c1;

        // per body mode bobbing of the draw positions
        bool running = false;
        float tail_stiffness = 1.f;
        if (mode == player::body_mode_index::stand) {
            g.draw_pos[0].x += float(flip) * 6.f * mathf::clamp(std::fabs(v1.x) - 0.2f, 0.f, 1.f);
            g.draw_pos[0].y += std::cos(frame / 6.f * 2.f * mathf::pi) * 2.f;
            g.draw_pos[1].x -= float(flip) * (1.5f - frame / 6.f);
            g.draw_pos[1].y += 2.f + std::sin(frame / 6.f * 2.f * mathf::pi) * 4.f;
            running = std::fabs(v0.x) > 2.f && std::fabs(v1.x) > 2.f;
            tail_stiffness = 1.f - mathf::clamp((std::fabs(v1.x) - 1.f) * 0.5f, 0.f, 1.f);
        } else if (mode == player::body_mode_index::crawl) {
            float const s = std::sin(frame / 21.f * 2.f * mathf::pi);
            float const c = std::cos(frame / 14.f * 2.f * mathf::pi);
            float const launch = w.player_get_super_launch_jump(p) > 19 ? 0.f : 1.f;
            g.draw_pos[0].x += c * float(flip) * 2.f;
            g.draw_pos[0].y -= s * -1.5f - 3.f;
            g.head.vel.y -= s * -0.5f - 0.5f;
            g.head.vel.x += c0.x < c1.x ? -1.f : 1.f;
            g.draw_pos[1].x += -3.f * s * float(flip);
            g.draw_pos[1].y -= c * 1.5f - 7.f + 3.f * launch;
        } else if (mode == player::body_mode_index::climbing_on_beam && anim == player::animation_index::stand_on_beam) {
            g.draw_pos[1].y += 3.f;
        }

        // tail
        vec2 pull = c0;
        if (running) {
            pull = c1;
            pull.y -= 4.f;
            pull.x += float(flip) * 16.f * mathf::clamp(std::fabs(v1.x) - 0.2f, 0.f, 1.f);
        }
        vec2 prev = c1;
        float force = 28.f;
        for (int l = 0; l < 4; ++l) {
            auto& t = g.tail[l];
            tail_update(g, l, g.draw_pos[1], room);
            t.vel *= mathf::lerp(0.75f, 0.95f, tail_stiffness * (1.f - sub1));
            t.vel.y -= mathf::lerp(0.1f, 0.5f, tail_stiffness) * (1.f - sub1) * gravity;
            tail_stiffness = (tail_stiffness * 10.f + 1.f) / 11.f;
            float const max_d = 9.f * float(l + 1);
            if (!custom::dist_less(t.pos, c1, max_d))
                t.pos = c1 + custom::dir_vec(c1, t.pos) * max_d;
            float const d = rw::distance(pull, t.pos);
            if (d > 0.f)
                t.vel += custom::dir_vec(pull, t.pos) * force / d;
            force *= 0.5f;
            pull = prev;
            prev = t.pos;
        }

        // head
        vec2 offset = custom::dir_vec(g.draw_pos[1], g.draw_pos[0]) * 3.f;
        if (mode == player::body_mode_index::crawl)
            offset.x *= 2.5f;
        else if (mode == player::body_mode_index::corridor_climb && offset.y < 0.f)
            offset.y *= 2.f;
        g.head.vel += g.look_direction;
        generic_update(g.head, room, head_air_friction, head_surface_friction);
        connect_to_point(g.head, rw::lerp(g.draw_pos[0], g.draw_pos[1], 0.2f) + offset,
            anim == player::animation_index::hang_from_beam ? 0.f : 3.f, false, 0.2f, v0, 0.7f, 0.1f);

        // legs
        generic_update(g.legs, room, legs_air_friction, legs_surface_friction);
        if (mode == player::body_mode_index::corridor_climb) {
            connect_to_point(g.legs, c1 + custom::dir_vec(c0, c1) * 4.f, 2.f, false, 0.25f, v1, 0.5f, 0.1f);
            g.legs_direction += custom::deg_to_vec(aim(c0, c1));
        } else if (w.body_chunk_get_contact_point(b1).y == -1 || anim == player::animation_index::stand_on_beam) {
            connect_to_point(g.legs, c1 + vec2{g.legs_direction.x * 8.f, 1.f}, 5.f, false, 0.25f, vec2{v1.x, -10.f}, 0.5f, 0.1f);
            g.legs_direction.x -= float(w.body_chunk_get_on_slope(b1));
            g.legs_direction.y -= 1.f;
        } else if (anim == player::animation_index::beam_tip) {
            connect_to_point(g.legs, c1 + vec2{0.f, -8.f}, 0.f, false, 0.25f, vec2{0.f, -10.f}, 0.5f, 0.1f);
            g.legs_direction += custom::dir_vec(g.draw_pos[0], room_view::middle_of_tile(c1) + vec2{0.f, -10.f});
        } else if (anim == player::animation_index::climb_on_beam) {
            vec2 const b{float(-flip) * (5.f - std::sin(frame / 20.f * mathf::pi * 2.f)), -16.f - 5.f * std::cos(frame / 20.f * mathf::pi * 2.f)};
            connect_to_point(g.legs, c0 + b, 0.f, false, 0.25f, vec2{0.f, 0.f}, 0.5f, 0.1f);
            g.legs_direction.y -= 1.f;
        } else {
            connect_to_point(g.legs, c1 + vec2{g.legs_direction.x * 8.f, anim == player::animation_index::hang_from_beam ? -5.f : -2.f},
                4.f, false, 0.25f, vec2{v1.x, -10.f}, 0.5f, 0.1f);
            g.legs_direction += v1 * 0.01f;
            g.legs_direction.y -= 0.05f;
        }
        g.legs_direction = rw::normalized(g.legs_direction);

        // blinking
        g.last_look_direction = g.look_direction;
        --g.blink;
        if (g.blink < -g.random.range(2, 1800))
            g.blink = g.random.range(3, g.random.range(3, 10));
    }

    void build(sys::game_state const& state, dcon::player_id p, float alpha, std::vector<draw_cmd>& out) {
        auto const& w = state.world;
        auto const& g = w.player_get_graphics(p);
        if (!g.initialized)
            return;
        rw::vec3 const body_color = w.player_get_stats(p).body_color;
        rw::vec3 const eye_color{0.01f, 0.01f, 0.01f};
        auto const mode = w.player_get_body_mode(p);
        auto const anim = w.player_get_animation(p);
        int32_t const flip = w.player_get_flip_direction(p);
        int32_t const frame = w.player_get_animation_frame(p);
        bool const awake = consious(w, p);
        auto const lerp_part = [&](player::body_part const& bp) { return rw::lerp(bp.last_pos, bp.pos, alpha); };
        vec2 const v = rw::lerp(g.last_draw_pos[0], g.draw_pos[0], alpha);  // upper body
        vec2 const v2 = rw::lerp(g.last_draw_pos[1], g.draw_pos[1], alpha); // lower body
        vec2 v3 = lerp_part(g.head);
        auto const push = [&](std::string_view element, vec2 pos, float rot, vec2 scale, vec2 anchor, rw::vec3 color) {
            draw_cmd c;
            c.spr = sprite{element, pos, rot, scale, anchor, color};
            out.push_back(c);
        };

        // 0 body, 1 hips
        push("BodyA", v, aim(v2, v), vec2{1.f, 1.f}, vec2{0.5f, 0.7894737f}, body_color);
        push("HipsA", (v2 * 2.f + v) / 3.f, aim(v, lerp_part(g.tail[0])), vec2{1.f, 1.f}, vec2{0.5f, 0.5f}, body_color);

        // 2 tail TriangleMesh (15 vertices, 13 triangles)
        {
            vec2 verts[15];
            vec2 base = (v2 * 3.f + v) / 4.f;
            float width = 6.f;
            for (int i = 0; i < 4; ++i) {
                vec2 const sp = lerp_part(g.tail[i]);
                vec2 const n = rw::normalized(sp - base);
                vec2 const a = custom::perpendicular_vector(n);
                float const d3 = i == 0 ? 0.f : rw::distance(sp, base) / 5.f;
                verts[i * 4] = base - a * width + n * d3;
                verts[i * 4 + 1] = base + a * width + n * d3;
                float const r = g.tail[i].rad * g.tail[i].stretched;
                if (i < 3) {
                    verts[i * 4 + 2] = sp - a * r - n * d3;
                    verts[i * 4 + 3] = sp + a * r - n * d3;
                } else {
                    verts[i * 4 + 2] = sp;
                }
                width = r;
                base = sp;
            }
            static constexpr int tris[13][3] = {
                {0, 1, 2}, {1, 2, 3}, {4, 5, 6}, {5, 6, 7}, {8, 9, 10}, {9, 10, 11}, {12, 13, 14},
                {2, 3, 4}, {3, 4, 5}, {6, 7, 8}, {7, 8, 9}, {10, 11, 12}, {11, 12, 13},
            };
            for (auto const& t : tris) {
                draw_cmd c;
                c.is_triangle = true;
                c.tri[0] = verts[t[0]];
                c.tri[1] = verts[t[1]];
                c.tri[2] = verts[t[2]];
                c.color = body_color;
                out.push_back(c);
            }
        }

        // head direction picks one of the 18 rotated head elements
        float head_rot = aim(rw::lerp(v2, v, 0.5f), v3);
        int32_t head_index = int32_t(std::round(std::fabs(head_rot / 360.f * 34.f)));
        vec2 look = rw::lerp(g.last_look_direction, g.look_direction, alpha) * 3.f;
        std::string_view face_element;
        float face_rot = 0.f;
        float face_scale_x = 1.f;
        auto const default_face = [&](int idx) -> std::string_view {
            idx = custom::int_clamp(idx, 0, 8);
            return g.blink <= 0 ? std::string_view{face_a_names[size_t(idx)]} : std::string_view{face_b_names[size_t(idx)]};
        };
        if (awake) {
            if ((mode == player::body_mode_index::stand && w.player_get_input(p)[0].x != 0) || mode == player::body_mode_index::crawl) {
                if (mode == player::body_mode_index::crawl) {
                    head_index = 7;
                    face_scale_x = mathf::sign(v.x - v2.x);
                } else {
                    head_index = 6;
                    face_scale_x = head_rot < 0.f ? -1.f : 1.f;
                }
                look.x = 0.f;
                face_element = default_face(4);
            } else {
                vec2 d = v3 - v2;
                d.x *= 1.f - rw::magnitude(look) / 3.f;
                d = rw::normalized(d);
                face_scale_x = std::fabs(look.x) < 0.1f ? (head_rot < 0.f ? -1.f : 1.f) : mathf::sign(look.x);
                face_element = default_face(int32_t(std::round(std::fabs(aim(vec2{0.f, 0.f}, d) / 22.5f))));
            }
        } else {
            look = vec2{0.f, 0.f};
            head_index = 0;
            face_element = w.player_get_dead(p) ? "FaceDead" : "FaceStunned";
            face_rot = head_rot;
        }
        head_index = custom::int_clamp(head_index, 0, 17);

        // 3 head
        push(head_names[size_t(head_index)], v3, head_rot, vec2{head_rot < 0.f ? -1.f : 1.f, 1.f}, vec2{0.5f, 0.5f}, body_color);

        // 4 legs
        {
            vec2 legs_pos = lerp_part(g.legs);
            float scale_x = 1.f;
            std::string_view element = "LegsAAir0";
            bool visible = true;
            float const facing = flip > 0 ? 1.f : -1.f;
            if (mode == player::body_mode_index::stand) {
                element = legs_names[size_t(custom::int_clamp(frame, 0, 6))];
                scale_x = facing;
            } else if (mode == player::body_mode_index::crawl) {
                element = legs_crawling_names[size_t(custom::int_clamp(frame / 2, 0, 5))];
                scale_x = facing;
            } else if (mode == player::body_mode_index::corridor_climb) {
                int32_t f = frame;
                if (f > 6) {
                    f %= 6;
                    scale_x = -1.f;
                }
                element = legs_climbing_names[size_t(custom::int_clamp(f, 0, 6))];
            } else if (mode == player::body_mode_index::climbing_on_beam) {
                if (anim == player::animation_index::stand_on_beam) {
                    element = legs_on_pole_names[size_t(frame < 7 ? frame : 0)];
                    scale_x = facing;
                } else if (anim == player::animation_index::beam_tip) {
                    element = "LegsAPole";
                } else if (anim == player::animation_index::climb_on_beam) {
                    element = "LegsAVerticalPole";
                    scale_x = facing;
                    legs_pos.y = mathf::clamp(legs_pos.y, v2.y - 6.f, v2.y + 4.f);
                }
            } else if (mode == player::body_mode_index::wall_climb) {
                element = "LegsAWall";
                scale_x = facing;
            } else if (mode == player::body_mode_index::default_) {
                if (anim == player::animation_index::ledge_grab) {
                    element = "LegsAWall";
                    scale_x = facing;
                }
            } else if (mode == player::body_mode_index::swimming && anim == player::animation_index::deep_swim) {
                visible = false;
            }
            if (visible)
                push(element, legs_pos, aim(g.legs_direction, vec2{0.f, 0.f}), vec2{scale_x, 1.f}, vec2{0.5f, 0.25f}, body_color);
        }

        // 9 face
        push(face_element, v3 + look + vec2{0.f, -2.f}, face_rot, vec2{face_scale_x, 1.f}, vec2{0.5f, 0.5f}, eye_color);
    }
}
