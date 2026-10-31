// DeerGraphics.cs with its Antlers and Dangler (HasDanglers). The antlers
// are rolled with the deer (deer.cpp's riders climb them); everything
// else here is cosmetic and only updates where a camera looks.

#include <algorithm>
#include <cmath>
#include <string_view>

#include "deer_graphics.hpp"
#include "body_parts.hpp"
#include "creature.hpp"
#include "deer.hpp"
#include "items.hpp"
#include "outposts.hpp"
#include "player.hpp"
#include "rooms.hpp"

namespace deer_graphics {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;
    using deer::dangler_segment;
    using deer::graphics_state;

    namespace {
        // the body chunks' rotationChunk (the chunk connections set them,
        // the constructor ties the head and the antlers together)
        constexpr uint32_t rotation_chunk[6] = {5, 2, 3, 4, 3, 0};
        // DeerGraphics.legGraphicAnchors[1, ...] / [0, ...]: the leg sprites'
        // anchors, front (A) and hind (B) legs, upper and lower part
        constexpr vec2 leg_anchor[2][2] = {
            {vec2{8.f / 19.f, 5.f / 44.f}, vec2{6.f / 13.f, 4.f / 62.f}},
            {vec2{3.5f / 32.f, 3.f / 69.f}, vec2{6.f / 13.f, 3.f / 56.f}},
        };
        constexpr std::string_view leg_elements[2][2] = {{"deerLegA1", "deerLegA2"}, {"deerLegB1", "deerLegB2"}};
        // Dangler.DanglerProps()
        constexpr float dangler_gravity = 0.9f;
        constexpr float dangler_air_friction = 0.98f;
        constexpr float dangler_water_gravity = -0.2f;
        constexpr float dangler_water_friction = 0.8f;
        constexpr float dangler_elasticity = 0.85f;
        constexpr float dangler_weight_symmetry = 0.7f;

        dcon::deer_data_id data(sys::game_state const& state, dcon::creature_id c) { return creature::deer_data(state.world, c); }

        vec2 chunk_pos(sys::game_state const& state, dcon::creature_id c, uint32_t i, float alpha) {
            auto const k = creature::chunk(state, c, i);
            return rw::lerp(state.world.body_chunk_get_last_pos(k), state.world.body_chunk_get_pos(k), alpha);
        }

        // the legs by their number (none for a missing one)
        void legs_of(sys::game_state const& state, dcon::creature_id c, deer::leg const* out[deer::leg_count]) {
            auto const& w = state.world;
            for (uint32_t i = 0; i < deer::leg_count; ++i)
                out[i] = nullptr;
            for (auto const o : creature::parts(w, c)) {
                auto const p = w.part_ownership_get_part(o);
                if (w.creature_part_get_kind(p) != creature::part_kind::deer_leg)
                    continue;
                uint32_t const n = w.creature_part_get_index(p);
                if (n < deer::leg_count)
                    out[n] = &w.deer_leg_get_t(dcon::deer_leg_id{dcon::deer_leg_id::value_base_t(w.creature_part_get_payload(p).deer_leg.data)});
            }
        }

        // the first segment of dangler `k` among deer_data.danglers
        uint32_t dangler_start(graphics_state const& g, uint32_t k) {
            uint32_t s = 0;
            for (uint32_t i = 0; i < k; ++i)
                s += g.dangler_segs[i];
            return s;
        }

        // DeerGraphics.DanglerConnection
        vec2 dangler_connection(sys::game_state const& state, dcon::creature_id c, graphics_state const& g, uint32_t index, float alpha) {
            auto const& w = state.world;
            if (index < g.body_danglers) {
                uint32_t const a = g.body_dangler_positions[index][0];
                uint32_t const b = g.body_dangler_positions[index][1];
                float const* o = g.body_dangler_orientations[index];
                vec2 const v = chunk_pos(state, c, a, alpha);
                vec2 v2 = chunk_pos(state, c, b, alpha);
                vec2 const av = v + custom::deg_to_vec(custom::aim(v, chunk_pos(state, c, rotation_chunk[a], alpha)) + o[0]) * o[1];
                v2 += custom::deg_to_vec(custom::aim(v2, chunk_pos(state, c, rotation_chunk[b], alpha)) + o[2]) * o[3];
                return rw::lerp(av, v2, o[4]);
            }
            index -= g.body_danglers;
            auto const& idx = w.deer_data_get_antler_index(data(state, c));
            int16_t const part = g.horn_dangler_positions[index][0];
            int16_t const seg = g.horn_dangler_positions[index][1];
            float const side = float(g.horn_dangler_positions[index][2]);
            vec2 const ant = chunk_pos(state, c, 5, alpha);
            if (part < 0 || part >= int16_t(idx.count))
                return ant;
            return deer::antler_point_pos(state, c, uint32_t(idx.start[part]) + uint32_t(std::max<int16_t>(0, seg)), side, chunk_pos(state, c, 0, alpha), ant,
                deer::face_dir(state, c, alpha));
        }

        // DeerGraphics.LegPartLength
        float leg_part_length(float preferred_height, int32_t pos, int32_t part, bool extension) {
            float const k = extension ? preferred_height / 15.f : 1.f;
            if (pos == 0)
                return (part == 0 ? 35.f : 55.f) * k;
            return (part == 0 ? 60.f : 45.f) * k;
        }

        // BodyPart.OnOtherSideOfTerrain
        bool on_other_side_of_terrain(room_view const& room, vec2 pos, vec2 con, float min_affect) {
            if (custom::dist_less(pos, con, min_affect))
                return false;
            if (room.get_tile(pos).solid())
                return true;
            ivec2 iv = room_view::get_tile_position(con) - room_view::get_tile_position(pos);
            iv.x = custom::int_clamp(iv.x, -1, 1);
            iv.y = custom::int_clamp(iv.y, -1, 1);
            if (iv.x != 0 && iv.y != 0) {
                if (std::fabs(con.x - pos.x) > std::fabs(con.y - pos.y))
                    iv.y = 0;
                else
                    iv.x = 0;
            }
            return room.get_tile(room_view::get_tile_position(pos) + iv).solid();
        }

        // Dangler.Reset
        void reset_danglers(sys::game_state& state, dcon::creature_id c) {
            auto& w = state.world;
            auto const d = data(state, c);
            auto const& g = w.deer_data_get_graphics(d);
            auto segs = w.deer_data_get_danglers(d);
            uint32_t k = 0;
            for (uint32_t i = 0; i < uint32_t(g.body_danglers + g.horn_danglers); ++i) {
                vec2 at = dangler_connection(state, c, g, i, 1.f);
                for (uint32_t j = 0; j < g.dangler_segs[i] && k < segs.size(); ++j, ++k) {
                    auto& s = segs[k];
                    at.y -= s.con_rad;
                    s.pos = at;
                    s.last_pos = at;
                    s.vel = vec2{0.f, 0.f};
                }
            }
        }

        // TriangleMesh.MakeLongMesh's four vertices for one point
        // (MoveVertice(k * 4 ...)): around `prev` and around `cur`
        void mesh_quad(std::vector<vec2>& v, vec2 prev, vec2 cur, float r0, float r1, bool first) {
            vec2 const n = rw::normalized(cur - prev);
            vec2 const a = custom::perpendicular_vector(n);
            float const d = rw::distance(cur, prev) / 5.f;
            if (first) {
                v.push_back(prev - a * r0);
                v.push_back(prev + a * r0);
            } else {
                v.push_back(prev - a * r0 + n * d);
                v.push_back(prev + a * r0 + n * d);
            }
            v.push_back(cur - a * r1 - n * d);
            v.push_back(cur + a * r1 - n * d);
        }
    }

    void create(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        auto const d = data(state, c);
        w.creature_get_graphics_random(c) = rw::rng{w.creature_get_random(c).state ^ 0x5DEECE6u};
        rw::unity_random r{w.deer_data_get_seed(d)};
        // Antlers(deer.antlers.rad, 1f)
        static thread_local std::vector<items::antler_point> points;
        int32_t const parts = outposts::generate_antlers(r, w.body_chunk_get_rad(creature::chunk(state, c, 5)), points);
        deer::antler_index idx;
        idx.count = uint8_t(std::min<int32_t>(parts, int32_t(deer::max_antler_parts)));
        for (uint32_t k = 0; k < points.size(); ++k) {
            uint32_t const p = points[k].part;
            if (p >= idx.count)
                continue;
            if (idx.length[p] == 0)
                idx.start[p] = uint16_t(k);
            ++idx.length[p];
        }
        w.deer_data_set_antler_index(d, idx);
        w.deer_data_get_antlers(d).load_range(points.data(), points.data() + points.size());

        graphics_state g;
        for (auto& cr : g.chunks_rotat)
            cr = r.value() * 360.f;
        g.body_danglers = uint8_t(r.range(10, 20));
        g.horn_danglers = uint8_t(r.range(5, 10));
        static thread_local std::vector<dangler_segment> segs;
        segs.clear();
        for (uint32_t j = 0; j < uint32_t(g.body_danglers + g.horn_danglers); ++j) {
            bool const body = j < g.body_danglers;
            // Random.Range(4, Random.Range(6, Random.Range(8, 16)))
            int32_t const inner = r.range(8, 16);
            int32_t const mid = r.range(6, inner);
            int32_t const n = r.range(4, mid);
            g.dangler_segs[j] = uint8_t(n);
            float const num = mathf::lerp(0.5f, 1.f, r.value()) * (body ? 1.f : 0.5f);
            float const num2 = body ? 1.f : mathf::lerp(0.2f, 0.7f, r.value());
            for (int32_t k = 0; k < n; ++k) {
                float const num3 = float(k) / float(n - 1);
                dangler_segment s;
                s.rad = mathf::lerp(mathf::lerp(body ? 11.f : 6.f, 2.5f, mathf::pow(num3, 0.7f)), 2.f + std::sin(mathf::pow(num3, 2.5f) * mathf::pi) * 6.f, num3) * num;
                s.con_rad = mathf::lerp(30.f, 5.f, num3) * num2 * 0.5f;
                s.stretched_rad = s.rad;
                segs.push_back(s);
            }
            if (body) {
                g.body_dangler_positions[j][0] = uint8_t(r.range(0, 5));
                g.body_dangler_positions[j][1] = uint8_t(r.range(1, 5));
                g.body_dangler_orientations[j][0] = mathf::lerp(0.f, 360.f, r.value());
                g.body_dangler_orientations[j][1] = w.body_chunk_get_rad(creature::chunk(state, c, g.body_dangler_positions[j][0])) * r.value() * 0.7f;
                g.body_dangler_orientations[j][2] = mathf::lerp(0.f, 360.f, r.value());
                g.body_dangler_orientations[j][3] = w.body_chunk_get_rad(creature::chunk(state, c, g.body_dangler_positions[j][1])) * r.value() * 0.7f;
                g.body_dangler_orientations[j][4] = r.value();
            } else {
                uint32_t const h = j - g.body_danglers;
                int32_t const part = idx.count > 0 ? r.range(0, int32_t(idx.count)) : 0;
                int32_t const len = idx.count > 0 ? int32_t(idx.length[part]) : 1;
                int32_t const in2 = r.range(1, len);
                g.horn_dangler_positions[h][0] = int16_t(part);
                g.horn_dangler_positions[h][1] = int16_t(std::max(0, len - 1 - r.range(1, in2)));
                g.horn_dangler_positions[h][2] = int16_t(r.value() < 0.5f ? -1 : 1);
            }
        }
        w.deer_data_set_graphics(d, g);
        w.deer_data_get_danglers(d).load_range(segs.data(), segs.data() + segs.size());
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        reset_danglers(state, c);
    }

    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room) {
        auto& w = state.world;
        auto const d = data(state, c);
        auto& g = w.deer_data_get_graphics(d);
        auto& rng = w.creature_get_graphics_random(c);
        auto const& pl = w.creature_get_payload(c).deer;
        auto const& ai = w.deer_data_get_ai(d);
        vec2 const head = w.body_chunk_get_pos(creature::chunk(state, c, 0));
        bool const cons = creature::consious(state, c);
        // the look point (the CreatureLooker simplified: the closest slugcat in view)
        g.look_point[1] = g.look_point[0];
        g.look_point[0] = vec2{0.f, 0.f};
        if (auto const it = deer::going_to_puff_ball(state, c)) {
            g.look_point[0] = rw::clamp_magnitude(w.body_chunk_get_pos(items::chunk(state, it)) - head, 200.f) / 200.f;
        } else {
            float best = 600.f;
            bool found = false;
            vec2 at{0.f, 0.f};
            rooms::for_each_creature(state, room.id, creature::type::slugcat, [&](dcon::creature_id pc) {
                if (!creature::here(state, pc, room.id))
                    return;
                vec2 const q = creature::main_pos(state, pc);
                float const dd = rw::distance(q, head);
                if (dd < best && room.visual_contact(head, q)) {
                    best = dd;
                    at = q;
                    found = true;
                }
            });
            if (found)
                g.look_point[0] = rw::clamp_magnitude(at - head, 200.f) / 200.f;
            else if (ai.spore_room == room.id)
                g.look_point[0] = rw::clamp_magnitude(room_view::middle_of_tile(ai.spore_tile) - head, 200.f) / 200.f;
        }
        --g.blink;
        if (g.blink < -rng.range(5, 14))
            g.blink = rng.range(15, 305);
        if (!cons || ai.close_eyes_counter > 0)
            g.blink = -5;
        // the danglers, kicked by the legs
        deer::leg const* legs[deer::leg_count];
        legs_of(state, c, legs);
        {
            auto segs = w.deer_data_get_danglers(d);
            uint32_t k = 0;
            for (uint32_t i = 0; i < uint32_t(g.body_danglers + g.horn_danglers); ++i) {
                uint32_t const first = k;
                vec2 const con = dangler_connection(state, c, g, i, 1.f);
                float from_base = 0.f;
                for (uint32_t j = 0; j < g.dangler_segs[i] && k < segs.size(); ++j, ++k) {
                    auto& s = segs[k];
                    // DanglerSegment.Update
                    s.last_pos = s.pos;
                    s.pos += s.vel;
                    if (room.point_submerged(s.pos)) {
                        s.vel.y -= dangler_water_gravity;
                        s.vel *= dangler_water_friction;
                    } else {
                        s.vel *= dangler_air_friction;
                        s.vel.y -= dangler_gravity;
                    }
                    if (j == 0) {
                        vec2 const a = custom::dir_vec(s.pos, con);
                        float const num = rw::distance(s.pos, con);
                        s.pos -= a * (s.con_rad - num);
                        s.vel -= a * (s.con_rad - num);
                        s.stretched_rad = s.rad * mathf::clamp(mathf::lerp(mathf::pow(s.con_rad / mathf::max(num, 0.001f), 0.5f), 1.f, 0.5f), 0.2f, 1.8f);
                    } else {
                        auto& prev = segs[k - 1];
                        vec2 const a = custom::dir_vec(s.pos, prev.pos);
                        float const num3 = rw::distance(s.pos, prev.pos);
                        float num5 = prev.rad / (s.rad + prev.rad);
                        num5 = mathf::lerp(num5, dangler_weight_symmetry, 0.5f);
                        s.pos -= a * (s.con_rad - num3) * dangler_elasticity * num5;
                        s.vel -= a * (s.con_rad - num3) * dangler_elasticity * num5;
                        prev.pos += a * (s.con_rad - num3) * dangler_elasticity * (1.f - num5);
                        prev.vel += a * (s.con_rad - num3) * dangler_elasticity * (1.f - num5);
                        s.stretched_rad = s.rad * mathf::clamp(mathf::lerp(mathf::pow(s.con_rad / mathf::max(num3, 0.001f), 0.5f), 1.f, 0.5f), 0.2f, 1.8f);
                    }
                    from_base += s.con_rad;
                    if (j > 1)
                        for (uint32_t q = 0; q < 5; ++q) {
                            // BodyPart.PushFromPoint (the chunks that collide with objects)
                            auto const ch = creature::chunk(state, c, q);
                            vec2 const pp = w.body_chunk_get_pos(ch);
                            float const pr = w.body_chunk_get_rad(ch);
                            if (custom::dist_less(s.pos, pp, pr)) {
                                float const num = rw::distance(s.pos, pp);
                                vec2 const a = custom::dir_vec(s.pos, pp);
                                s.pos -= a * (pr - num);
                                s.vel -= a * (pr - num);
                            }
                        }
                    if (!on_other_side_of_terrain(room, s.pos, con, from_base * 1.2f)) {
                        // BodyPart.PushOutOfTerrain (a dangler's BodyPart.rad
                        // and surfaceFric are 0)
                        player::body_part bp;
                        bp.pos = s.pos;
                        bp.last_pos = s.last_pos;
                        bp.vel = s.vel;
                        bp.rad = 0.f;
                        body_parts::push_out_of_terrain(bp, room, 0.f);
                        s.pos = bp.pos;
                        s.vel = bp.vel;
                    }
                }
                // DeerGraphics.Update: a random leg's second chunk kicks the segments
                for (uint32_t j = first; j < k; ++j) {
                    auto const* lg = legs[rng.range(0, int32_t(deer::leg_count))];
                    if (lg && custom::dist_less(segs[j].pos, lg->c[1].pos, 10.f))
                        segs[j].vel += lg->c[1].vel;
                }
            }
        }
        // the flip and the antlers' random movement
        vec2 const c1 = w.body_chunk_get_pos(creature::chunk(state, c, 1));
        float const num = mathf::clamp(-1.f + 2.f * mathf::inverse_lerp(-20.f, 20.f, head.x - c1.x) + pl.flip_dir, -1.f, 1.f);
        g.last_flip = g.flip;
        if (num < 0.f)
            g.flip = mathf::max(-1.f, g.flip - 1.f / custom::lerp_map(std::fabs(g.flip - num), 0.f, 1.f, 120.f, 20.f));
        else
            g.flip = mathf::min(1.f, g.flip + 1.f / custom::lerp_map(std::fabs(g.flip - num), 0.f, 1.f, 120.f, 20.f));
        g.antler_random_movement_vel *= 0.8f;
        if (cons)
            g.antler_random_movement_vel += mathf::lerp(-1.f, 1.f, rng.value()) * rw::magnitude(w.body_chunk_get_vel(creature::chunk(state, c, 0))) * 0.01f;
        g.antler_random_movement_vel -= g.antler_random_movement * 0.0001f;
        g.last_antler_random_movement = g.antler_random_movement;
        g.antler_random_movement = mathf::clamp(g.antler_random_movement + g.antler_random_movement_vel, -1.f, 1.f);
        g.last_resting = g.resting;
        g.resting = pl.resting;
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip) {
        auto const& w = state.world;
        auto const d = data(state, c);
        auto const& g = w.deer_data_get_graphics(d);
        auto const& pl = w.creature_get_payload(c).deer;
        auto const& idx = w.deer_data_get_antler_index(d);
        auto const points = w.deer_data_get_antlers(d);
        // ApplyPalette
        vec3 const pal54 = pal.texel(5, 4);
        vec3 const black = pal.black();
        vec3 const body_color = black + (pal54 - black) * 0.2f;
        vec3 const fog = pal.fog();
        float const num = deer::face_dir(state, c, alpha);
        // CurrentFoggedHornColor
        vec3 const fogged = body_color + (fog - body_color) * (mathf::clamp(std::fabs(num), 0.f, 1.f) * 0.35f);
        vec2 const head = chunk_pos(state, c, 0, alpha);
        vec2 const ant = chunk_pos(state, c, 5, alpha);
        static thread_local std::vector<vec2> verts;

        // Antlers.DrawSprites, one layer (sprite num) at a time
        float const rotation = custom::aim(head, ant);
        auto const antler_layer = [&](int32_t layer) {
            int32_t const i = num < 0.f ? layer : 1 - layer;
            float const flip = i == 0 ? -1.f : 1.f;
            vec3 const col = layer == 0 ? fogged : body_color;
            for (uint32_t j = 0; j < idx.count; ++j) {
                verts.clear();
                vec2 v{0.f, 0.f};
                float num3 = 0.2f;
                for (uint32_t k = 0; k < idx.length[j]; ++k) {
                    uint32_t const pk = uint32_t(idx.start[j]) + k;
                    if (pk >= points.size())
                        break;
                    auto const& ap = points.at(pk);
                    vec2 const v2 = outposts::transform_to_head_rotat(outposts::transformed_pos(ap, flip), ant, rotation, flip, num);
                    if (k == 0)
                        v = v2;
                    vec2 const n = rw::normalized(v2 - v);
                    vec2 const a = custom::perpendicular_vector(n);
                    float const dd = rw::distance(v2, v) / 5.f;
                    verts.push_back(v - a * (num3 + ap.rad) * 0.5f + n * dd);
                    verts.push_back(v + a * (num3 + ap.rad) * 0.5f + n * dd);
                    verts.push_back(v2 - a * ap.rad - n * dd);
                    verts.push_back(v2 + a * ap.rad - n * dd);
                    num3 = ap.rad;
                    v = v2;
                }
                sprites::long_mesh(out, strip, verts.data(), verts.size(), col);
            }
        };

        // Dangler.DrawSprite
        auto const segs = w.deer_data_get_danglers(d);
        auto const dangler = [&](uint32_t i, vec3 col) {
            uint32_t const s0 = dangler_start(g, i);
            if (s0 >= segs.size() || g.dangler_segs[i] == 0)
                return;
            verts.clear();
            vec2 v = dangler_connection(state, c, g, i, alpha);
            float sr = segs.at(s0).stretched_rad;
            for (uint32_t j = 0; j < g.dangler_segs[i] && s0 + j < segs.size(); ++j) {
                auto const& s = segs.at(s0 + j);
                vec2 const v2 = rw::lerp(s.last_pos, s.pos, alpha);
                mesh_quad(verts, v, v2, (sr + s.stretched_rad) * 0.5f, s.stretched_rad, j == 0);
                sr = s.stretched_rad;
                v = v2;
            }
            sprites::long_mesh(out, strip, verts.data(), verts.size(), col);
        };

        // the far antlers, the horn danglers (the far side fogged in
        // containers[0], the near side in containers[2]: the rider's
        // sprites go between them in vanilla), the near antlers
        antler_layer(0);
        for (uint32_t l = 0; l < g.horn_danglers; ++l)
            if ((g.horn_dangler_positions[l][2] < 0) == (num < 0.f))
                dangler(g.body_danglers + l, fogged);
        for (uint32_t l = 0; l < g.horn_danglers; ++l)
            if ((g.horn_dangler_positions[l][2] < 0) != (num < 0.f))
                dangler(g.body_danglers + l, body_color);
        antler_layer(1);

        // the body: the JaggedCircle shader over Futile_White (16 pixels),
        // drawn as Circle20 ellipses
        vec2 const vector = head + custom::dir_vec(ant, head) * 50.f + custom::perpendicular_vector(custom::dir_vec(ant, head)) * 100.f * num;
        for (uint32_t m = 0; m < 5; ++m) {
            vec2 const v5 = chunk_pos(state, c, m, alpha);
            float const rad = w.body_chunk_get_rad(creature::chunk(state, c, m));
            float const rot = m == 0 ? custom::aim(v5, vector) : custom::aim(chunk_pos(state, c, rotation_chunk[m], alpha), v5) + g.chunks_rotat[m - 1];
            sprites::add(out, sprites::sprite{"Circle20", v5, rot, vec2{rad / 8.f * 1.05f * 0.8f, rad / 8.f * 1.3f * 0.8f}, vec2{0.5f, 0.5f}, body_color});
        }
        for (uint32_t k = 0; k < g.body_danglers; ++k)
            dangler(k, body_color);

        // the legs
        deer::leg const* legs[deer::leg_count];
        legs_of(state, c, legs);
        float const resting = mathf::lerp(g.last_resting, g.resting, alpha);
        float const ph = pl.preferred_height;
        auto const tc = [&](deer::leg const& l, uint32_t i) { return rw::lerp(l.c[i].last_pos, l.c[i].pos, alpha); };
        vec2 const a2 = custom::dir_vec(chunk_pos(state, c, 3, alpha), chunk_pos(state, c, 1, alpha));
        for (uint32_t n = 0; n < deer::leg_count; ++n) {
            if (!legs[n])
                continue;
            auto const& lg = *legs[n];
            int32_t const kind = n < 2 ? 0 : 1;
            vec2 v6 = chunk_pos(state, c, lg.connected_chunk, alpha);
            vec2 v7 = tc(lg, 1);
            if (n < 2) {
                v7 = rw::lerp(v7, v6, 0.2f) + a2 * 20.f;
                v6 = rw::lerp(v6, head, 0.4f) + vec2{0.f, -20.f} + a2 * 5.f;
            } else {
                v6 = rw::lerp(v6, chunk_pos(state, c, 4, alpha), 0.5f) + vec2{0.f, -5.f};
                v6 -= a2 * 17.f;
                v7 -= a2 * 10.f;
            }
            vec2 v8 = custom::inverse_kinematic(v6, v7, leg_part_length(ph, kind, 0, true), leg_part_length(ph, kind, 1, true), (n < 2 ? 1.f : -1.f) * mathf::sign(num));
            v8 = rw::lerp(v8, v6 + custom::dir_vec(v6, tc(lg, 0)) * leg_part_length(ph, kind, 0, true), 1.f - std::fabs(num));
            vec2 v9 = tc(lg, 4);
            vec2 const v10 = tc(lg, 5);
            v9 = rw::lerp(v9, v10 + custom::dir_vec(v10, tc(lg, 4)) * 40.f, 0.5f);
            float const num4 = rw::distance(v7, v9);
            vec2 v11 = custom::inverse_kinematic(v7, v9, mathf::lerp(num4 / 2.f, 80.f, 0.5f), mathf::lerp(num4 / 2.f, 80.f, 0.5f), -mathf::sign(num));
            v11 = rw::lerp(v11, rw::lerp(tc(lg, 2), tc(lg, 3), 0.5f), mathf::lerp(1.f, 0.5f, std::fabs(num)));
            float const num5 = num < ((n % 2 == 0) ? 0.5f : -0.5f) ? -1.f : 1.f;
            if (n >= 2 && std::fabs(num) < 0.7f && mathf::sign(num) != num5) {
                v6 += custom::perpendicular_vector(rw::normalized(v6 - v8)) * 50.f * num5 * mathf::inverse_lerp(0.f, 0.7f, std::fabs(num));
                v6 = v8 + custom::dir_vec(v8, v6) * leg_part_length(ph, 1, 0, true) * (1.f - resting);
            }
            float const l0 = leg_part_length(ph, kind, 0, false);
            float const l1 = leg_part_length(ph, kind, 1, false);
            sprites::add(out, sprites::sprite{leg_elements[kind][0], v8, custom::aim(v8, v6),
                vec2{num5 * mathf::min(1.f, rw::distance(v6, v8) / l0), rw::distance(v6, v8) / l0}, leg_anchor[kind][0], body_color});
            sprites::add(out, sprites::sprite{leg_elements[kind][1], v7, custom::aim(v7, v8),
                vec2{num5 * mathf::min(1.f, rw::distance(v8, v7) / l1), rw::distance(v8, v7) / l1}, leg_anchor[kind][1], body_color});
            // the lower leg's mesh (MakeLongMesh(7)), its colour from the
            // body's to the palette's toward the hoof (ReColorLegs)
            verts.clear();
            vec2 v12 = v8;
            for (int32_t k = 0; k < 7; ++k) {
                vec2 v13 = v8;
                float dd = mathf::lerp(5.f, 1.f, float(k) / 6.f);
                float d2 = mathf::lerp(5.f, 1.f, (float(k) + 0.5f) / 6.f);
                switch (k) {
                case 0:
                    v13 = v7 - custom::dir_vec(v8, v11) * 2.5f;
                    dd = 0.f;
                    d2 = 4.f;
                    break;
                case 1:
                    v13 = v7 + custom::dir_vec(v8, v11) * 7.5f;
                    dd = 4.f;
                    d2 = 6.f;
                    break;
                case 2: v13 = v11 - custom::dir_vec(v7, v9) * 7.5f; break;
                case 3:
                    v13 = v11 + custom::dir_vec(v7, v9) * 7.5f;
                    d2 = 4.5f;
                    break;
                case 4: v13 = v9 - custom::dir_vec(v11, v10) * 7.5f; break;
                case 5:
                    v13 = v9 + custom::dir_vec(v11, v10) * 7.5f;
                    dd = 3.f;
                    d2 = 2.f;
                    break;
                default: v13 = v10; break;
                }
                vec2 const nn = rw::normalized(v13 - v12);
                vec2 const a3 = custom::perpendicular_vector(nn);
                float d3 = rw::distance(v13, v12) / 5.f;
                vec2 const p2 = v13 - a3 * d2 - nn * d3;
                vec2 const p3 = v13 + a3 * d2 - nn * d3;
                if (k == 6)
                    d3 = rw::distance(v13, v12);
                verts.push_back(v12 - a3 * dd + nn * d3);
                verts.push_back(v12 + a3 * dd + nn * d3);
                verts.push_back(p2);
                verts.push_back(p3);
                v12 = v13;
            }
            vec4 cols[28];
            for (uint32_t j = 0; j < 28; ++j) {
                vec3 const cc = body_color + (pal54 - body_color) * (mathf::pow(mathf::inverse_lerp(0.2f, 0.9f, float(j) / 27.f), 1.6f) * (1.f - resting));
                cols[j] = vec4{cc.x, cc.y, cc.z, 1.f};
            }
            sprites::long_mesh(out, strip, verts.data(), cols, verts.size());
        }

        // the eyes
        vec2 const look = rw::lerp(g.look_point[1], g.look_point[0], alpha);
        for (int32_t i = 0; i < 2; ++i) {
            float const num2 = i == 0 ? -1.f : 1.f;
            float const num3 = mathf::pow(mathf::inverse_lerp(0.5f, -0.5f, num * num2), 2.f);
            vec2 const v2 = head;
            vec2 v3 = v2;
            vec2 const v4 = custom::dir_vec(v2, ant);
            vec2 const a = custom::perpendicular_vector(v4);
            v3 += v4 * 10.f * (1.f - num3) + a * num2 * mathf::lerp(std::cos(std::fabs(num) * mathf::pi * 0.5f) * 15.f, w.body_chunk_get_rad(creature::chunk(state, c, 0)), num3)
                + custom::dir_vec(v3, vector) * 10.f * (1.f - num3);
            for (int32_t j = 0; j < 2; ++j) {
                float const rot = custom::aim(rw::lerp(v3, v2 + custom::dir_vec(v2, v3) * 25.f, num3), vector) - 90.f * num2;
                vec2 const at = v3;
                if (g.blink <= 0)
                    v3 = vec2{-1000.f, -1000.f};
                else
                    v3 += look * 2.f;
                if (at.x <= -999.f)
                    continue;
                if (j == 0)
                    sprites::add(out, sprites::sprite{g.blink <= 0 ? "deerEyeA2" : "deerEyeA", at, rot, vec2{1.f, 1.f - num3}, vec2{0.5f, 0.5f}, vec3{1.f, 0.7f, 0.f}});
                else
                    sprites::add(out, sprites::sprite{"deerEyeB", at, rot, vec2{1.f, 1.f - num3}, vec2{0.5f, 0.5f}, body_color});
            }
        }
    }
}
