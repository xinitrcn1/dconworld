#include <cmath>
#include <limits>
#include <vector>

#include "pole_mimic_graphics.hpp"
#include "creature.hpp"
#include "pole_mimic.hpp"

namespace pole_mimic_graphics {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using world::room_view;
    using pole_mimic::leaf;
    using pole_mimic::rope_segment;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // LizardScaleA0 / A3 (and the B ones): sourceSize.y
        constexpr float scale_height = 18.f;

        vec3 mix(vec3 a, vec3 b, float t) { return a + (b - a) * mathf::clamp01(t); }

        dcon::pole_mimic_data_id data(sys::game_state const& state, dcon::creature_id c) { return creature::pole_mimic_data(state.world, c); }

        // Rope.TotalPositions, Rope.GetPosition
        uint32_t total_positions(world::rope_state const& r) { return 2u + r.bend_count; }
        vec2 rope_position(world::rope_state const& r, uint32_t i) {
            if (i == 0)
                return r.a;
            if (i - 1 >= r.bend_count)
                return r.b;
            return r.bends[i - 1].pos;
        }

        // the layout InitiateSprites works out of the stalk's length
        struct layout {
            int32_t leaf_pairs;
            int32_t decorated;
        };

        // LeafLength, LeafWidth, LeafPerpFac, LeafForwardFac
        float leaf_frac(pole_mimic::graphics_state const& g, int32_t pair) { return float(pair) / float(std::max(1, g.leaf_pairs - 1)); }
        float leaf_length(pole_mimic::graphics_state const& g, int32_t pair) {
            float num = leaf_frac(g, pair);
            if (num < 0.75f) {
                num = mathf::inverse_lerp(0.f, 0.6f, num);
                return 4.f
                    + mathf::lerp(1.f - mathf::pow(num, 1.2f), std::sin(mathf::inverse_lerp(0.f, 0.75f, mathf::pow(num, 0.6f)) * mathf::pi), mathf::lerp(0.8f, 0.3f, num))
                    * 42.f * g.leaf_size_fac;
            }
            num = mathf::inverse_lerp(0.75f, 1.f, num);
            return 4.f + mathf::pow(num, 1.5f) * 20.f * g.leaf_size_fac;
        }
        float leaf_width(pole_mimic::graphics_state const& g, int32_t pair) { return mathf::lerp(1.f, 0.5f, mathf::pow(leaf_frac(g, pair), 0.5f)) * g.leaf_size_fac; }
        float leaf_perp_fac(pole_mimic::graphics_state const& g, int32_t pair) {
            float const num = leaf_frac(g, pair);
            return mathf::lerp(0.15f + 0.85f * std::sin(mathf::inverse_lerp(0.f, 0.75f, mathf::pow(num, 0.2f)) * mathf::pi), mathf::inverse_lerp(0.6f, 1.f, num), num);
        }
        float leaf_forward_fac(pole_mimic::graphics_state const& g, int32_t pair) {
            float const f = leaf_frac(g, pair);
            return mathf::lerp(-0.2f, 1.f - std::sin(mathf::pow(f, 1.8f) * mathf::pi), mathf::pow(f, 0.3f));
        }
        // LeafLookLikePole, StemLookLikePole
        float leaf_look_like_pole(leaf const* lv, int32_t pair, int32_t side, float alpha) {
            auto const& l = lv[pair * 2 + side];
            return mathf::lerp(l.m[1], l.m[0], alpha);
        }
        float stem_look_like_pole(pole_mimic::graphics_state const& g, leaf const* lv, float stem_pos, float alpha) {
            int32_t const pair = custom::int_clamp(int32_t(stem_pos * float(g.leaf_pairs - 1)), 0, g.leaf_pairs - 1);
            return 1.f - (1.f - leaf_look_like_pole(lv, pair, 0, alpha)) * (1.f - leaf_look_like_pole(lv, pair, 1, alpha));
        }

        // PoleMimicRopeGraphics: OneDimensionalTubePos, OnTubePos, OnTubeDir
        vec2 tube_pos(rope_segment const* seg, uint32_t n, float f, float alpha) {
            int32_t const a = custom::int_clamp(int32_t(std::floor(f * float(n - 1))), 0, int32_t(n) - 1);
            int32_t const b = custom::int_clamp(a + 1, 0, int32_t(n) - 1);
            float const t = mathf::inverse_lerp(float(a), float(b), f * float(n - 1));
            return rw::lerp(rw::lerp(seg[a].last_pos, seg[b].last_pos, t), rw::lerp(seg[a].pos, seg[b].pos, t), alpha);
        }
        vec2 on_tube_pos(rope_segment const* seg, uint32_t n, vec2 p, float alpha) {
            vec2 const a = tube_pos(seg, n, p.y - 1.f / float(n), alpha);
            vec2 const b = tube_pos(seg, n, p.y + 1.f / float(n), alpha);
            return tube_pos(seg, n, p.y, alpha) + custom::perpendicular_vector(custom::dir_vec(a, b)) * p.x;
        }
        vec2 on_tube_dir(rope_segment const* seg, uint32_t n, float f, float alpha) {
            return custom::dir_vec(tube_pos(seg, n, f - 1.f / float(n), alpha), tube_pos(seg, n, f + 1.f / float(n), alpha));
        }

        // Custom.VerticalCrossPoint / HorizontalCrossPoint / RectCollision's
        // corner (as daddy_graphics.cpp's)
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

        // PoleMimicRopeGraphics.MoveSegment
        void move_segment(room_view const& room, rope_segment& s, vec2 goal, vec2 smoothed) {
            s.vel = vec2{0.f, 0.f};
            if (room.get_tile(smoothed).solid() && !room.get_tile(goal).solid()) {
                ivec2 const t = room_view::get_tile_position(smoothed);
                s.pos = rect_collision(smoothed, goal, float(t.x) * 20.f - 3.f, float(t.y) * 20.f - 3.f, float(t.x) * 20.f + 23.f, float(t.y) * 20.f + 23.f);
                return;
            }
            s.pos = smoothed;
        }

        // RopeGraphic.AlignAndConnect (Segment.Update; airFriction 0)
        void align_and_connect(room_view const& room, rope_segment* seg, uint32_t n, std::vector<vec2> const& list) {
            uint32_t const count = uint32_t(list.size());
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
                float const dist = rw::distance(seg[i].pos, seg[j].pos);
                if (seg[i].bend_index > 0)
                    v += rw::normalized(custom::dir_vec(list[size_t(seg[i].bend_index - 1)], v) + custom::dir_vec(seg[i].pos, seg[j].pos)) * dist * 0.33333334f;
                if (seg[j].bend_index > -1 && seg[j].bend_index < int32_t(count) - 1)
                    v2 += rw::normalized(custom::dir_vec(list[size_t(seg[j].bend_index + 1)], v2) + custom::dir_vec(seg[j].pos, seg[i].pos)) * dist * 0.33333334f;
                float const t = mathf::inverse_lerp(float(i), float(j), float(idx));
                move_segment(room, s, rw::lerp(seg[i].pos, seg[j].pos, t), custom::bezier(seg[i].pos, v, seg[j].pos, v2, t));
            }
            for (uint32_t k = 0; k < n; ++k)
                seg[k].bend_index = -1;
        }

        // PoleMimicRopeGraphics.Update: the base, then every chunk rope's
        // positions past its first
        void rope_update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto const d = data(state, c);
            std::vector<vec2> list;
            list.push_back(w.body_chunk_get_pos(creature::chunk(state, c, 1)));
            for (auto const& tc : w.pole_mimic_data_get_chunks(d))
                for (uint32_t j = 1; j < total_positions(tc.rope); ++j)
                    list.push_back(rope_position(tc.rope, j));
            auto rope = w.pole_mimic_data_get_rope(d);
            align_and_connect(room, rope.begin(), rope.size(), list);
        }

        // Tentacle.CurrentLength
        float current_length(sys::game_state const& state, dcon::pole_mimic_data_id d) {
            auto const seg = state.world.pole_mimic_data_get_segments(d);
            float num = float(seg.size()) * 20.f;
            for (uint32_t i = 0; i + 2 < seg.size(); ++i) {
                ivec2 const a = seg.at(i), b = seg.at(i + 2);
                if (a.x != b.x && a.y != b.y && (std::abs(a.x - b.x) < 2 || std::abs(a.y - b.y) < 2))
                    num -= 5.9000006f;
            }
            return num;
        }
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        // PoleMimicGraphics.InitiateSprites
        auto& w = state.world;
        auto const d = data(state, c);
        auto& g = w.pole_mimic_data_get_graphics(d);
        auto const& b = w.pole_mimic_data_get_body(d);
        auto const& p = w.creature_get_payload(c).pole_mimic;
        auto const chunks = w.pole_mimic_data_get_chunks(d);
        if (chunks.size() < 2)
            return;
        bool const dont_spawn_in_pole_mode = p.just_out_of_shortcut != 0;
        g.leaf_pairs = std::max(2, int32_t(b.ideal_length / 8.f));
        g.decorated_leaf_pairs = custom::int_clamp(int32_t(float(g.leaf_pairs) * 0.6f), 1, 80);
        g.leaf_size_fac = custom::lerp_map(mathf::lerp(float(g.leaf_pairs), 65.f, 0.1f), 5.f, 185.f, 0.5f, 2.f);
        // RopeGraphic(leafPairs * 2)
        auto rope = w.pole_mimic_data_get_rope(d);
        rope.resize(uint32_t(std::max(2, g.leaf_pairs * 2)));
        vec2 const root_chunk = w.body_chunk_get_pos(creature::chunk(state, c, 1));
        for (auto& s : rope)
            s = rope_segment{root_chunk, root_chunk, vec2{0.f, 0.f}, -1};
        auto leaves = w.pole_mimic_data_get_leaves(d);
        leaves.resize(uint32_t(g.leaf_pairs * 2));
        for (auto& l : leaves) {
            l = leaf{};
            l.p[0] = root_chunk;
            // (sic: the velocity starts at the position)
            l.p[2] = l.p[0];
            for (int32_t k = 0; k < 5; ++k)
                l.m[k] = dont_spawn_in_pole_mode ? (k > 2 ? -1.f : 0.f) : 1.f;
        }
        g.initiated = 1;
        if (auto const r = creature::room_of(state, c))
            rope_update(state, c, world::get_room_view(state, r));
        if (dont_spawn_in_pole_mode) {
            g.look_like_a_pole = 0.f;
            g.last_look_like_a_pole = 0.f;
            for (auto& l : leaves) {
                l.p[0] = b.root_pos;
                l.p[1] = b.root_pos;
                l.p[2] = vec2{0.f, 0.f};
            }
        } else {
            uint32_t const n = rope.size();
            for (uint32_t i = 0; i < n; ++i) {
                uint32_t const k = std::min(i, chunks.size() - 1);
                rope.at(i).pos = chunks.at(k).pos;
                rope.at(i).last_pos = rope.at(k).pos;
            }
            rope.at(n - 1).pos = b.tip_pos;
            rope.at(n - 1).last_pos = b.tip_pos;
        }
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto const d = data(state, c);
        if (!w.pole_mimic_data_get_graphics(d).initiated)
            reset_positions(state, c);
        auto& g = w.pole_mimic_data_get_graphics(d);
        auto const& b = w.pole_mimic_data_get_body(d);
        auto const& p = w.creature_get_payload(c).pole_mimic;
        auto chunks = w.pole_mimic_data_get_chunks(d);
        auto sticks = w.pole_mimic_data_get_sticks(d);
        auto rope = w.pole_mimic_data_get_rope(d);
        auto leaves = w.pole_mimic_data_get_leaves(d);
        if (!g.initiated || chunks.size() < 2 || leaves.size() != uint32_t(g.leaf_pairs * 2) || rope.size() < 2)
            return;
        auto& rng = w.creature_get_graphics_random(c);
        // a reveal running up the stalk now and then (the first few for
        // the slugcat who never saw one: PoleMimicEverSeen, kept per plant)
        if (g.look_like_a_pole > 0.9f && g.reveal == 0.f && g.last_reveal == 0.f) {
            if (rng.value() < 0.025f && g.first_reveals <= 5) {
                ++g.first_reveals;
                g.reveal = 0.01f;
                g.reveal_fac = 1.f + rng.value();
            } else if (rng.value() < 0.0014285714f) {
                g.reveal = 0.01f;
                g.reveal_fac = rng.value();
            }
        }
        rope_update(state, c, room);
        // (soundLoop: Pole_Mimic_Movement_LOOP has no sample id)
        rope_segment const* seg = rope.begin();
        uint32_t const ns = rope.size();
        uint32_t const nch = chunks.size();
        float const cur_len = mathf::max(1.f, current_length(state, d));
        bool flag = true;
        for (int32_t i = 0; i < g.leaf_pairs; ++i) {
            float const num = leaf_frac(g, i);
            uint32_t const num2 = uint32_t(custom::int_clamp(int32_t(num * float(nch)), 0, int32_t(nch) - 1));
            vec2 const v = tube_pos(seg, ns, num, 1.f);
            vec2 v2 = on_tube_dir(seg, ns, num, 1.f);
            if (num > 0.42857143f && b.float_grab_dest)
                v2 = rw::slerp(v2, custom::dir_vec(v, *b.float_grab_dest), mathf::inverse_lerp(0.42857143f, 1.f, num) * 0.5f * (1.f - p.mimic));
            vec2 const a = custom::perpendicular_vector(v2);
            float const num3 = leaf_length(g, i);
            vec2 const bp = tube_pos(seg, ns, num + num3 / cur_len, 1.f);
            // the stuck chunk at this height, if any
            dcon::body_chunk_id stuck;
            if (num2 < sticks.size()) {
                auto const& s = sticks.at(num2);
                if (s.who && creature::here(state, s.who, room.id) && s.chunk < creature::chunk_count_of(state, s.who))
                    stuck = creature::chunk_of(state, s.who, s.chunk);
            }
            for (int32_t j = 0; j < 2; ++j) {
                auto& l = leaves.at(uint32_t(i * 2 + j));
                l.m[1] = l.m[0];
                l.m[0] = mathf::lerp(l.m[0], l.m[2], 0.1f);
                if (rng.value() < 0.1f)
                    l.m[2] = custom::lerp_and_tick(l.m[2], g.look_like_a_pole, rng.value() * 0.1f, rng.value() / 40.f);
                if (flag && l.m[2] > 0.5f)
                    flag = false;
                vec2 v3 = v + rw::normalized(v2 * leaf_forward_fac(g, i) + a * (j == 0 ? -1.f : 1.f) * leaf_perp_fac(g, i)) * num3;
                if (g.reveal > 0.f && rng.value() > num) {
                    float const num4 = mathf::inverse_lerp(0.35f, 0.1f, std::fabs(num - mathf::clamp(num, g.last_reveal, g.reveal))) * g.reveal_fac;
                    if (num4 > 0.f) {
                        l.m[2] = mathf::lerp(l.m[2], 0.f, num4 * 0.4f * rng.value());
                        l.m[0] = mathf::lerp(l.m[0], 0.f, num4 * 0.04f * rng.value());
                    } else {
                        l.m[2] = mathf::lerp(l.m[2], 1.f, rng.value() * 0.4f * g.reveal_fac);
                        l.m[0] = mathf::lerp(l.m[0], l.m[2], rng.value() * g.reveal_fac);
                    }
                }
                v3 = rw::lerp(v3, bp, l.m[0]);
                l.p[1] = l.p[0];
                l.p[0] += l.p[2];
                l.p[2] *= 0.75f * (1.f - mathf::pow(l.m[2], 3.f));
                l.p[2].y -= 0.3f;
                l.p[0] = rw::lerp(l.p[0], bp, mathf::pow(l.m[2], 3.f));
                if (!custom::dist_less(v3, l.p[0], num3 * 0.5f))
                    v3 = v + custom::dir_vec(v, v3) * num3 * 0.5f;
                if (stuck) {
                    vec2 const sp = w.body_chunk_get_pos(stuck);
                    l.p[0] = sp + custom::dir_vec(sp, l.p[0]) * w.body_chunk_get_rad(stuck);
                    l.p[2] = vec2{0.f, 0.f};
                }
                l.p[2] += (v3 - l.p[0]) / 4.f;
                l.m[4] = l.m[3];
                if (rng.value() < 0.33333334f)
                    l.m[3] = custom::lerp_and_tick(l.m[3], ((num < g.flip_point) == (g.leaves_flip != 0)) ? 1.f : -1.f, 0.1f * (1.f - l.m[2]), 0.25f);
            }
        }
        if ((g.flip_point > 0.f || g.leaves_flip) && g.look_like_a_pole < 0.6f && flag)
            g.flip_point += 1.f / (3.5f * mathf::lerp(float(g.leaf_pairs), 10.f, 0.6f));
        else if ((g.flip_point > 0.f || !g.leaves_flip) && g.look_like_a_pole > 0.4f)
            g.flip_point += 1.f / (10.f * mathf::lerp(float(g.leaf_pairs), 10.f, 0.3f));
        if (g.flip_point >= 1.f) {
            g.flip_point = 0.f;
            g.leaves_flip = g.leaves_flip ? 0 : 1;
        }
        g.last_look_like_a_pole = g.look_like_a_pole;
        g.last_reveal = g.reveal;
        if (g.reveal > 0.f) {
            g.reveal += 1.f / (mathf::lerp(15.f, float(w.pole_mimic_data_get_segments(d).size()), 0.5f) * mathf::lerp(2.5f, 1.5f, g.reveal_fac));
            g.look_like_a_pole = custom::lerp_and_tick(g.look_like_a_pole, 0.9f, 0.04f, 0.016666668f);
            if (g.reveal >= 1.f) {
                g.reveal = 0.f;
                g.last_reveal = 0.f;
            }
        } else {
            g.look_like_a_pole = custom::lerp_and_tick(g.look_like_a_pole, p.mimic, 0.03f, 0.033333335f);
        }
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip) {
        auto const& w = state.world;
        auto const d = data(state, c);
        auto const& g = w.pole_mimic_data_get_graphics(d);
        auto const& b = w.pole_mimic_data_get_body(d);
        auto const rope = w.pole_mimic_data_get_rope(d);
        auto const leaves = w.pole_mimic_data_get_leaves(d);
        if (!g.initiated || rope.size() < 2 || leaves.size() != uint32_t(g.leaf_pairs * 2))
            return;
        rope_segment const* seg = rope.begin();
        leaf const* lv = leaves.begin();
        uint32_t const n = rope.size();
        // ApplyPalette: the mimicked pole's colour (the palette's pole
        // texel, a little fogged) and the black
        float const fog_amount = 1.f - pal.texel(9, 7).x;
        vec3 const mimic_color = mix(pal.texel(4, 3), pal.fog(), fog_amount * 0.13333334f);
        vec3 const color = mix(pal.black(), mimic_color, mathf::lerp(g.last_look_like_a_pole, g.look_like_a_pole, alpha));

        // sprite 0: the stalk (sprite 1, its shadow mesh in the Shadows
        // container, has no renderer layer here)
        {
            std::vector<vec2> v(size_t(n) * 4);
            vec2 at = b.root_pos - b.stick_out_dir * 30.f;
            at += custom::dir_vec(rw::lerp(seg[1].last_pos, seg[1].pos, alpha), at) * 1.f;
            float wa = 2.f;
            for (uint32_t i = 0; i < n; ++i) {
                float const num = float(i) / float(n - 1);
                vec2 const v2 = rw::lerp(seg[i].last_pos, seg[i].pos, alpha);
                vec2 const nrm = rw::normalized(at - v2);
                vec2 const a2 = custom::perpendicular_vector(nrm);
                float const dd = rw::distance(at, v2) / 3.f;
                float const f = stem_look_like_pole(g, lv, num, alpha);
                float const num2 = mathf::lerp(i % 2 == 0 ? mathf::lerp(4.f, 1.5f, num) : mathf::lerp(1.4f, 0.75f, num), 2.f, mathf::pow(f, 0.75f));
                float const wm = mathf::lerp(wa, num2, 0.5f);
                v[i * 4] = at - nrm * dd - a2 * wm;
                v[i * 4 + 1] = at - nrm * dd + a2 * wm;
                v[i * 4 + 2] = v2 + nrm * dd - a2 * num2;
                v[i * 4 + 3] = v2 + nrm * dd + a2 * num2;
                at = v2;
                wa = num2;
            }
            sprites::long_mesh(out, strip, v.data(), v.size(), color);
        }

        // the leaves, then their decorations
        struct leaf_draw {
            vec2 pos;
            float rotation;
            float scale_y;
            float scale_x;
            float num2;
        };
        std::vector<leaf_draw> ld(size_t(g.leaf_pairs) * 2);
        for (int32_t i = 0; i < g.leaf_pairs; ++i) {
            float const num = leaf_frac(g, i);
            for (int32_t j = 0; j < 2; ++j) {
                auto const& l = lv[i * 2 + j];
                vec2 const v = on_tube_pos(seg, n, vec2{j == 0 ? -1.f : 1.f, num}, alpha);
                vec2 const v2 = rw::lerp(l.p[1], l.p[0], alpha);
                float num2 = -mathf::lerp(l.m[4], l.m[3], alpha);
                if (num >= 0.75f)
                    num2 = std::fabs(num2);
                float const sx = (j == 0 ? -1.f : 1.f) * leaf_width(g, i) * mathf::pow(mathf::inverse_lerp(1.f, 0.6f, leaf_look_like_pole(lv, i, j, alpha)), 0.5f) * num2;
                auto& e = ld[size_t(i * 2 + j)];
                e = leaf_draw{v, custom::aim(v, v2), rw::distance(v, v2) / scale_height, sx, num2};
                sprites::sprite s;
                s.element = num < 0.75f ? "LizardScaleA3" : "LizardScaleA0";
                s.pos = e.pos;
                s.rotation = e.rotation;
                s.scale = vec2{e.scale_x, e.scale_y};
                s.anchor = vec2{0.5f, 0.f};
                s.color = color;
                sprites::add(out, s);
            }
        }
        for (int32_t i = 0; i < g.decorated_leaf_pairs && i < g.leaf_pairs; ++i) {
            float const num = leaf_frac(g, i);
            for (int32_t j = 0; j < 2; ++j) {
                auto const& e = ld[size_t(i * 2 + j)];
                sprites::sprite s;
                s.element = num < 0.75f ? "LizardScaleB3" : "LizardScaleB0";
                s.pos = e.pos;
                s.rotation = e.rotation;
                s.scale = vec2{e.scale_x, e.scale_y};
                s.anchor = vec2{0.5f, 0.f};
                vec3 const col = mix(vec3{1.f, 0.f, 0.f}, color, mathf::pow(mathf::inverse_lerp(float(g.decorated_leaf_pairs / 2), float(g.decorated_leaf_pairs), float(i)), 0.6f));
                float const a = mathf::pow(1.f - stem_look_like_pole(g, lv, num, alpha), 0.2f) * (e.num2 > 0.f ? 0.33333334f : 1.f);
                s.color = col;
                vec4 const cc{col.x, col.y, col.z, a};
                vec4 const corners[4] = {cc, cc, cc, cc};
                sprites::add(out, s, corners);
            }
        }
    }
}
