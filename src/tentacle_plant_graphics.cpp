// TentaclePlantGraphics: the tentacle drawn as one black tube
// (TentaclePlantRopeGraphics, a RopeGraphic laid along the tentacle chunks'
// ropes) and fifty Danglers, the leaves hanging off it, coloured from dark
// red at the root to bright at the tip.

#include <algorithm>
#include <cmath>
#include <limits>

#include "tentacle_plant_graphics.hpp"
#include "body_parts.hpp"
#include "creature.hpp"
#include "tentacle_plant.hpp"

namespace tentacle_plant_graphics {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using world::room_view;
    using tentacle_plant::dangler;
    using tentacle_plant::dangler_segment;
    using tentacle_plant::rope_segment;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        constexpr uint32_t n_segs = tentacle_plant::rope_segments;
        // Dangler.DanglerProps' defaults (TentaclePlantGraphics.danglerVals)
        constexpr float d_gravity = 0.9f, d_air_friction = 0.98f, d_water_gravity = -0.2f, d_water_friction = 0.8f, d_elasticity = 0.85f,
                        d_weight_symmetry = 0.7f;

        dcon::tentacle_plant_data_id data(sys::game_state const& state, dcon::creature_id c) { return creature::tentacle_plant_data(state.world, c); }
        creature::tentacle_plant_payload const& pl(sys::game_state const& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).tentacle_plant;
        }

        // Custom.VerticalCrossPoint / HorizontalCrossPoint / RectCollision's
        // point (as daddy_graphics.cpp's)
        vec2 vertical_cross_point(vec2 a, vec2 b, float x) {
            if (a.y == b.y)
                return vec2{x, a.y};
            float const k = (a.y - b.y) / (a.x - b.x);
            return vec2{x, a.y - a.x * k + k * x};
        }
        vec2 horizontal_cross_point(vec2 a, vec2 b, float y) {
            if (a.x == b.x)
                return vec2{a.x, y};
            float const k = (a.y - b.y) / (a.x - b.x);
            return vec2{(y - (a.y - a.x * k)) / k, y};
        }
        vec2 rect_collision(vec2 pos, vec2 last, float left, float bottom, float right, float top) {
            vec2 const v1 = vertical_cross_point(last, pos, left);
            vec2 const v2 = vertical_cross_point(last, pos, right);
            vec2 v3 = horizontal_cross_point(last, pos, bottom);
            vec2 v4 = horizontal_cross_point(last, pos, top);
            if (last.x == pos.x) {
                v3 = vec2{last.x, bottom};
                v4 = vec2{last.x, top};
            }
            bool const inside = last.x > left && last.x < right && last.y > bottom && last.y < top;
            float dst = inside ? std::numeric_limits<float>::max() : rw::distance(last, pos);
            vec2 r = pos;
            if (v1.y >= bottom && v1.y <= top && custom::dist_less(last, v1, dst)) {
                r = v1;
                dst = rw::distance(last, v1);
            }
            if (v2.y >= bottom && v2.y <= top && custom::dist_less(last, v2, dst)) {
                r = v2;
                dst = rw::distance(last, v2);
            }
            if (v3.x >= left && v3.x <= right && custom::dist_less(last, v3, dst)) {
                r = v3;
                dst = rw::distance(last, v3);
            }
            if (v4.x >= left && v4.x <= right && custom::dist_less(last, v4, dst))
                r = v4;
            return r;
        }

        // TentaclePlantRopeGraphics.MoveSegment: kept out of the terrain
        void move_segment(room_view const& room, rope_segment& s, vec2 goal, vec2 smoothed) {
            s.vel = vec2{0.f, 0.f};
            if (room.get_tile(smoothed).solid() && !room.get_tile(goal).solid()) {
                ivec2 const t = room_view::get_tile_position(smoothed);
                s.pos = rect_collision(smoothed, goal, float(t.x) * 20.f - 3.f, float(t.y) * 20.f - 3.f, float(t.x) * 20.f + 23.f, float(t.y) * 20.f + 23.f);
                return;
            }
            s.pos = smoothed;
        }

        // RopeGraphic.AlignAndConnect (airFricition 0) with its Segment.Update
        void align_and_connect(room_view const& room, rope_segment* seg, uint32_t n, vec2 const* list, uint32_t count) {
            float num = 0.f;
            for (uint32_t i = 1; i < count; ++i)
                num += rw::distance(list[i - 1], list[i]);
            float num2 = 0.f;
            for (uint32_t j = 0; j < count; ++j) {
                if (j > 0)
                    num2 += rw::distance(list[j - 1], list[j]);
                if (j == 0 || list[j - 1] != list[j]) {
                    // RopeGraphic.AlignRope
                    float const f = num > 0.f ? num2 / num : 0.f;
                    int32_t const k = custom::int_clamp(int32_t(f * float(n - 1)), 0, int32_t(n) - 1);
                    if (seg[k].bend_index <= -1)
                        seg[k].last_pos = seg[k].pos;
                    seg[k].pos = list[j];
                    seg[k].vel = vec2{0.f, 0.f};
                    seg[k].bend_index = int32_t(j);
                }
            }
            for (uint32_t idx = 0; idx < n; ++idx) {
                auto& s = seg[idx];
                if (s.bend_index > -1)
                    continue;
                s.last_pos = s.pos;
                s.pos += s.vel;
                s.vel = vec2{0.f, 0.f};
                int32_t i = int32_t(idx), j = int32_t(idx);
                while (i > 0) {
                    --i;
                    if (seg[i].bend_index > -1)
                        break;
                }
                while (j < int32_t(n) - 1) {
                    ++j;
                    if (seg[j].bend_index > -1)
                        break;
                }
                vec2 v = seg[i].pos;
                vec2 v2 = seg[j].pos;
                float const t = mathf::inverse_lerp(float(i), float(j), float(idx));
                float const dist = rw::distance(seg[i].pos, seg[j].pos);
                if (seg[i].bend_index > 0)
                    v += rw::normalized(custom::dir_vec(list[size_t(seg[i].bend_index - 1)], v) + custom::dir_vec(seg[i].pos, seg[j].pos)) * dist * 0.33333334f;
                if (seg[j].bend_index < int32_t(count) - 1)
                    v2 += rw::normalized(custom::dir_vec(list[size_t(seg[j].bend_index + 1)], v2) + custom::dir_vec(seg[j].pos, seg[i].pos)) * dist * 0.33333334f;
                move_segment(room, s, rw::lerp(seg[i].pos, seg[j].pos, t), custom::bezier(seg[i].pos, v, seg[j].pos, v2, t));
            }
            for (uint32_t k = 0; k < n; ++k)
                seg[k].bend_index = -1;
        }

        // TentaclePlantRopeGraphics.OneDimensionalTubePos, OnTubeDir, OnTubePos
        vec2 tube_pos_1d(rope_segment const* seg, uint32_t n, float f, float ts) {
            int32_t const a = custom::int_clamp(int32_t(std::floor(f * float(n - 1))), 0, int32_t(n) - 1);
            int32_t const b = custom::int_clamp(a + 1, 0, int32_t(n) - 1);
            float const t = mathf::inverse_lerp(float(a), float(b), f * float(n - 1));
            return rw::lerp(rw::lerp(seg[a].last_pos, seg[b].last_pos, t), rw::lerp(seg[a].pos, seg[b].pos, t), ts);
        }
        vec2 on_tube_dir(rope_segment const* seg, uint32_t n, float f, float ts) {
            return custom::dir_vec(tube_pos_1d(seg, n, f - 1.f / float(n), ts), tube_pos_1d(seg, n, f + 1.f / float(n), ts));
        }
        vec2 on_tube_pos(rope_segment const* seg, uint32_t n, vec2 pos, float ts) {
            vec2 const p = tube_pos_1d(seg, n, pos.y - 1.f / float(n), ts);
            vec2 const p2 = tube_pos_1d(seg, n, pos.y + 1.f / float(n), ts);
            return tube_pos_1d(seg, n, pos.y, ts) + custom::perpendicular_vector(custom::dir_vec(p, p2)) * pos.x;
        }
        // TentaclePlantGraphics.DanglerConnection
        vec2 dangler_connection(rope_segment const* seg, dangler const& dg, float ts) {
            return on_tube_pos(seg, n_segs, vec2{dg.props[1], dg.props[0]}, ts);
        }

        // the TentaclePlantGraphics constructor's layout, from the seed
        // (true when it was laid out now)
        bool build_layout(sys::game_state& state, dcon::creature_id c) {
            auto& w = state.world;
            auto const d = data(state, c);
            auto& g = w.tentacle_plant_data_get_graphics(d);
            if (g.built)
                return false;
            g.built = 1;
            w.tentacle_plant_data_get_rope(d).resize(n_segs);
            auto dgs = w.tentacle_plant_data_get_danglers(d);
            dgs.resize(tentacle_plant::dangler_count);
            rw::unity_random r{w.tentacle_plant_data_get_seed(d)};
            float nums[tentacle_plant::dangler_count][2];
            uint32_t total = 0;
            for (uint32_t i = 0; i < tentacle_plant::dangler_count; ++i) {
                int32_t segs;
                if (i < 15) {
                    segs = r.range(4, 12);
                } else {
                    // Random.Range(2, Random.Range(4, Random.Range(4, Random.Range(4, 12))))
                    int32_t const z = r.range(4, 12);
                    int32_t const y = r.range(4, z);
                    int32_t const x = r.range(4, y);
                    segs = r.range(2, x);
                }
                segs = std::max(segs, 2);
                dangler dg;
                dg.props[0] = mathf::pow(r.value(), 0.6f);
                float const rd = tentacle_plant::rad(dg.props[0]);
                dg.props[1] = mathf::lerp(-rd, rd, r.value());
                dg.props[2] = r.value();
                nums[i][0] = mathf::lerp(rd, 4.f, 0.5f) * mathf::lerp(0.5f, 1.5f, r.value());
                nums[i][1] = mathf::lerp(0.5f, 1.5f, r.value());
                dg.first = uint16_t(total);
                dg.count = uint8_t(segs);
                dgs.at(i) = dg;
                total += uint32_t(segs);
            }
            auto ss = w.tentacle_plant_data_get_dangler_segments(d);
            ss.resize(total);
            for (uint32_t i = 0; i < tentacle_plant::dangler_count; ++i) {
                auto const dg = dgs.at(i);
                for (uint32_t j = 0; j < dg.count; ++j) {
                    float const n3 = float(j) / float(dg.count - 1);
                    auto& q = ss.at(dg.first + j);
                    q = dangler_segment{};
                    q.rad = mathf::lerp(mathf::lerp(1.f, 0.5f, mathf::pow(n3, 0.7f)), 0.5f + std::sin(mathf::pow(n3, 2.5f) * mathf::pi) * 0.5f, n3) * nums[i][0];
                    q.stretched_rad = q.rad;
                    q.con_rad = mathf::lerp(30.f, 5.f, n3) * nums[i][1];
                }
            }
            return true;
        }

        // BodyPart.OnOtherSideOfTerrain
        bool on_other_side_of_terrain(room_view const& room, vec2 pos, vec2 con, float min_affect) {
            if (custom::dist_less(pos, con, min_affect))
                return false;
            if (room.get_tile(pos).solid())
                return true;
            ivec2 v = custom::clamp_at_one(room_view::get_tile_position(con) - room_view::get_tile_position(pos));
            if (v.x != 0 && v.y != 0) {
                if (std::fabs(con.x - pos.x) > std::fabs(con.y - pos.y))
                    v.y = 0;
                else
                    v.x = 0;
            }
            return room.get_tile(room_view::get_tile_position(pos) + v).solid();
        }
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        build_layout(state, c);
        auto const d = data(state, c);
        auto const rid = creature::room_of(state, c);
        if (!rid)
            return;
        auto const room = world::get_room_view(state, rid);
        auto const& p = pl(state, c);
        auto rope = w.tentacle_plant_data_get_rope(d);
        // TentaclePlantGraphics.Reset: the rope from the root to the tip
        vec2 const list[2] = {p.root_pos, w.tentacle_plant_data_get_chunks(d).c[tentacle_plant::chunk_count - 1].pos};
        align_and_connect(room, rope.begin(), rope.size(), list, 2);
        // Dangler.Reset: hanging straight down from where it grows
        auto dgs = w.tentacle_plant_data_get_danglers(d);
        auto ss = w.tentacle_plant_data_get_dangler_segments(d);
        for (uint32_t i = 0; i < dgs.size(); ++i) {
            auto const& dg = dgs.at(i);
            vec2 at = dangler_connection(rope.begin(), dg, 1.f);
            for (uint32_t j = 0; j < dg.count; ++j) {
                auto& s = ss.at(dg.first + j);
                at.y -= s.con_rad;
                s.pos = at;
                s.last_pos = at;
                s.vel = vec2{0.f, 0.f};
            }
        }
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        if (build_layout(state, c)) {
            reset_positions(state, c);
            return;
        }
        auto const d = data(state, c);
        auto const& p = pl(state, c);
        auto const& cs = w.tentacle_plant_data_get_chunks(d).c;
        // TentaclePlantRopeGraphics.Update: FloatBase, then the chunks' rope positions
        vec2 list[1 + tentacle_plant::chunk_count * (world::rope_state::max_bends + 1)];
        uint32_t count = 0;
        list[count++] = w.body_chunk_get_pos(creature::chunk(state, c, 1));
        for (uint32_t i = 0; i < tentacle_plant::chunk_count; ++i) {
            auto const& r = cs[i].rope;
            uint32_t const total = 2u + r.bend_count;
            for (uint32_t j = 1; j < total; ++j)
                list[count++] = j - 1 < r.bend_count ? r.bends[j - 1].pos : r.b;
        }
        auto rope = w.tentacle_plant_data_get_rope(d);
        align_and_connect(room, rope.begin(), rope.size(), list, count);
        rope_segment const* seg = rope.begin();
        // the danglers
        auto dgs = w.tentacle_plant_data_get_danglers(d);
        auto ss = w.tentacle_plant_data_get_dangler_segments(d);
        auto& rng = w.creature_get_graphics_random(c);
        dcon::body_chunk_id const body[2] = {creature::chunk(state, c, 0), creature::chunk(state, c, 1)};
        for (uint32_t i = 0; i < dgs.size(); ++i) {
            auto const& dg = dgs.at(i);
            dangler_segment* s = ss.begin() + dg.first;
            uint32_t const n = dg.count;
            // Dangler.Update: DanglerSegment.Update for each
            vec2 const con = dangler_connection(seg, dg, 1.f);
            float from_base = 0.f;
            for (uint32_t k = 0; k < n; ++k) {
                auto& q = s[k];
                q.last_pos = q.pos;
                q.pos += q.vel;
                if (room.point_submerged(q.pos)) {
                    q.vel.y -= d_water_gravity;
                    q.vel *= d_water_friction;
                } else {
                    q.vel *= d_air_friction;
                    q.vel.y -= d_gravity;
                }
                if (k == 0) {
                    vec2 const a = custom::dir_vec(q.pos, con);
                    float const num = rw::distance(q.pos, con);
                    q.pos -= a * (q.con_rad - num);
                    q.vel -= a * (q.con_rad - num);
                    q.stretched_rad = q.rad * mathf::clamp(mathf::lerp(mathf::pow(q.con_rad / mathf::max(num, 0.0001f), 0.5f), 1.f, 0.5f), 0.2f, 1.8f);
                } else {
                    auto& pr = s[k - 1];
                    vec2 const a2 = custom::dir_vec(q.pos, pr.pos);
                    float const num3 = rw::distance(q.pos, pr.pos);
                    float num5 = pr.rad / (q.rad + pr.rad);
                    num5 = mathf::lerp(num5, d_weight_symmetry, 0.5f);
                    q.pos -= a2 * (q.con_rad - num3) * d_elasticity * num5;
                    q.vel -= a2 * (q.con_rad - num3) * d_elasticity * num5;
                    pr.pos += a2 * (q.con_rad - num3) * d_elasticity * (1.f - num5);
                    pr.vel += a2 * (q.con_rad - num3) * d_elasticity * (1.f - num5);
                    q.stretched_rad = q.rad * mathf::clamp(mathf::lerp(mathf::pow(q.con_rad / mathf::max(num3, 0.0001f), 0.5f), 1.f, 0.5f), 0.2f, 1.8f);
                }
                if (k > 1) {
                    // BodyPart.PushFromPoint off the plant's chunks
                    for (auto const b : body) {
                        vec2 const bp = w.body_chunk_get_pos(b);
                        float const br = w.body_chunk_get_rad(b);
                        if (custom::dist_less(q.pos, bp, br)) {
                            float const num = rw::distance(q.pos, bp);
                            vec2 const a = custom::dir_vec(q.pos, bp);
                            q.pos -= a * (br - num);
                            q.vel -= a * (br - num);
                        }
                    }
                }
                from_base += q.con_rad;
                if (!on_other_side_of_terrain(room, q.pos, con, from_base * 1.2f)) {
                    // BodyPart.PushOutOfTerrain (BodyPart.rad and surfaceFric are 0)
                    player::body_part bp;
                    bp.pos = q.pos;
                    bp.last_pos = q.last_pos;
                    bp.vel = q.vel;
                    bp.rad = 0.f;
                    body_parts::push_out_of_terrain(bp, room, 0.f);
                    q.pos = bp.pos;
                    q.vel = bp.vel;
                }
            }
            // TentaclePlantGraphics.Update's push along the tube, the shake
            vec2 const v = on_tube_dir(seg, n_segs, dg.props[0], 1.f);
            s[0].vel += v * mathf::lerp(-1.f, 1.f, dg.props[0]) + custom::deg_to_vec(custom::vec_to_deg(v) + dg.props[2] * 360.f) / s[0].rad;
            if (p.attack > 0.5f && p.attack < 1.f)
                for (uint32_t j = 0; j < std::min(4u, n); ++j)
                    s[j].vel += custom::rnv(rng) * mathf::inverse_lerp(0.5f, 1.f, p.attack) * 15.f * dg.props[0];
            if (p.extended < 0.25f) {
                vec2 const at = dangler_connection(seg, dg, 1.f);
                for (uint32_t k = 0; k < n; ++k)
                    s[k].pos = rw::lerp(s[k].pos, at, mathf::inverse_lerp(0.25f, 0.f, p.extended));
            }
        }
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip) {
        auto const& w = state.world;
        auto const d = data(state, c);
        if (!w.tentacle_plant_data_get_graphics(d).built)
            return;
        auto const& p = pl(state, c);
        auto const rope = w.tentacle_plant_data_get_rope(d);
        if (rope.size() < 2)
            return;
        rope_segment const* seg = rope.begin();
        uint32_t const n = rope.size();
        vec3 const black = pal.black();
        // sprite 0: TentaclePlantRopeGraphics.DrawSprite, black (the
        // JaggedSquare shader's ragged edge isn't reproduced)
        {
            static thread_local std::vector<vec2> v;
            v.resize(size_t(n) * 4);
            vec2 vector = p.root_pos - p.stick_out_dir * 30.f;
            vector += custom::dir_vec(rw::lerp(seg[1].last_pos, seg[1].pos, alpha), vector) * 1.f;
            float a = tentacle_plant::rad(0.f) * 1.7f + 2.f;
            for (uint32_t i = 0; i < n; ++i) {
                float const f = float(i) / float(n - 1);
                vec2 const v2 = rw::lerp(seg[i].last_pos, seg[i].pos, alpha);
                vec2 const a2 = custom::perpendicular_vector(rw::normalized(vector - v2));
                float const num = tentacle_plant::rad(f) * 1.7f + 2.f;
                v[i * 4] = vector - a2 * mathf::lerp(a, num, 0.5f);
                v[i * 4 + 1] = vector + a2 * mathf::lerp(a, num, 0.5f);
                v[i * 4 + 2] = v2 - a2 * num;
                v[i * 4 + 3] = v2 + a2 * num;
                vector = v2;
                a = num;
            }
            sprites::long_mesh(out, strip, v.data(), v.size(), black);
        }
        // sprites 1..50: Dangler.DrawSprite, in TentaclePlantGraphics.ApplyPalette's
        // colours (the region's kelpColor and palette 13's aren't known
        // here; the TentaclePlant shader is drawn as its flat colour)
        auto const dgs = w.tentacle_plant_data_get_danglers(d);
        auto const ss = w.tentacle_plant_data_get_dangler_segments(d);
        float const dark = pal.darkness();
        static thread_local std::vector<vec2> v;
        for (uint32_t i = 0; i < dgs.size(); ++i) {
            auto const& dg = dgs.at(i);
            dangler_segment const* s = ss.begin() + dg.first;
            uint32_t const m = dg.count;
            vec3 const a = custom::hsl2rgb(custom::decimal(mathf::lerp(0.9f, 1.02f, mathf::pow(dg.props[0], 1.6f))), 1.f, mathf::lerp(0.f, 0.5f, dg.props[0]));
            vec3 const col = a + (black - a) * mathf::clamp01(dark);
            v.resize(size_t(m) * 4);
            vec2 vector = on_tube_pos(seg, n, vec2{dg.props[1], dg.props[0]}, alpha);
            float sr = s[0].stretched_rad;
            for (uint32_t k = 0; k < m; ++k) {
                vec2 const v2 = rw::lerp(s[k].last_pos, s[k].pos, alpha);
                vec2 const nrm = rw::normalized(v2 - vector);
                vec2 const pa = custom::perpendicular_vector(nrm);
                float const dd = rw::distance(v2, vector) / 5.f;
                vec2 const off = k == 0 ? vec2{0.f, 0.f} : nrm * dd;
                v[k * 4] = vector - pa * (sr + s[k].stretched_rad) * 0.5f + off;
                v[k * 4 + 1] = vector + pa * (sr + s[k].stretched_rad) * 0.5f + off;
                v[k * 4 + 2] = v2 - pa * s[k].stretched_rad - nrm * dd;
                v[k * 4 + 3] = v2 + pa * s[k].stretched_rad - nrm * dd;
                sr = s[k].stretched_rad;
                vector = v2;
            }
            sprites::long_mesh(out, strip, v.data(), v.size(), col);
        }
    }
}
