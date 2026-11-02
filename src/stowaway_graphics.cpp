#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "stowaway_graphics.hpp"
#include "creature.hpp"
#include "stowaway.hpp"
#include "stowaway_types.hpp"

namespace stowaway_graphics {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using sprites::sprite;
    using stowaway::head_chunks;
    using stowaway::head_count;
    using stowaway::rope_segment;
    using stowaway::rope_segments;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        dcon::stowaway_data_id data(sys::game_state const& state, dcon::creature_id c) { return creature::stowaway_data(state.world, c); }

        vec3 mix(vec3 a, vec3 b, float t) { return a + (b - a) * mathf::clamp01(t); }
        vec4 opaque(vec3 c) { return vec4{c.x, c.y, c.z, 1.f}; }

        // Rope.TotalPositions, Rope.GetPosition
        uint32_t total_positions(world::rope_state const& r) { return 2u + r.bend_count; }
        vec2 rope_position(world::rope_state const& r, uint32_t i) {
            if (i == 0)
                return r.a;
            if (i - 1 >= r.bend_count)
                return r.b;
            return r.bends[i - 1].pos;
        }

        // Custom.VerticalCrossPoint / HorizontalCrossPoint / RectCollision's
        // corner (as pole_mimic_graphics.cpp's)
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

        // StowawayRopeGraphics.MoveSegment
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

        // the body orbs (the constructor's layout from ID.RandomSeed)
        struct orbs {
            uint32_t count = 0;
            vec2 pos[8];
            float radius[8];
        };
        orbs body_orbs(int32_t seed, vec2 placed) {
            orbs o;
            rw::unity_random r{seed};
            int32_t const num = r.range(5, 9);
            o.count = uint32_t(num);
            for (int32_t j = 0; j < num; ++j) {
                float const f = float(j) / float(num);
                float const spread = r.value() * 10.f + mathf::lerp(0.f, 25.f, f);
                float const side = r.value() < 0.5f ? -1.f : 1.f;
                vec2 const a = placed + custom::perpendicular_vector(placed) * spread * side;
                o.pos[j] = a + placed * mathf::lerp(-5.f, -19.f, f);
                o.radius[j] = float(r.range(8, 12));
            }
            return o;
        }
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        auto const d = data(state, c);
        auto rope = w.stowaway_data_get_rope(d);
        rope.resize(head_count * rope_segments);
        auto const chunks = w.stowaway_data_get_chunks(d);
        vec2 const base = w.body_chunk_get_pos(creature::chunk(state, c, 0));
        for (uint32_t h = 0; h < head_count; ++h) {
            // AddToPositionsList(0, the body), (1, the tip), AlignAndConnect(2)
            vec2 const tip = chunks.size() == head_count * head_chunks ? chunks.at(h * head_chunks + head_chunks - 1).pos : base;
            for (uint32_t i = 0; i < rope_segments; ++i) {
                rope_segment s;
                s.pos = s.last_pos = rw::lerp(base, tip, float(i) / float(rope_segments - 1));
                rope.at(h * rope_segments + i) = s;
            }
        }
        auto& g = w.stowaway_data_get_graphics(d);
        g = stowaway::graphics_state{};
        g.initiated = 1;
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto const d = data(state, c);
        if (w.stowaway_data_get_rope(d).size() != head_count * rope_segments || !w.stowaway_data_get_graphics(d).initiated)
            reset_positions(state, c);
        auto& g = w.stowaway_data_get_graphics(d);
        auto const& p = w.creature_get_payload(c).stowaway;
        // the bite's overshoot (cosmetic)
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        if (p.biting > 1.f && p.biting < 1.3f && g.over_shoot_counter == 0.f) {
            g.over_shoot_counter = 0.01f;
            g.over_shoot_scale = rng.value() * 12.f;
        }
        if (g.over_shoot_counter > 0.f) {
            g.over_shoot_counter += 0.09f;
            g.over_shoot = std::sin(g.over_shoot_counter * mathf::pi) * g.over_shoot_scale;
        }
        if (g.over_shoot_counter >= 1.f)
            g.over_shoot_counter = 0.f;
        // StowawayRopeGraphics.Update: FloatBase, then every chunk rope's
        // positions past its first
        auto const chunks = w.stowaway_data_get_chunks(d);
        if (chunks.size() != head_count * head_chunks)
            return;
        auto rope = w.stowaway_data_get_rope(d);
        vec2 const base = w.body_chunk_get_pos(creature::chunk(state, c, 0));
        static thread_local std::vector<vec2> list;
        for (uint32_t h = 0; h < head_count; ++h) {
            list.clear();
            list.push_back(base);
            for (uint32_t i = 0; i < head_chunks; ++i) {
                auto const& r = chunks.at(h * head_chunks + i).rope;
                for (uint32_t j = 1; j < total_positions(r); ++j)
                    list.push_back(rope_position(r, j));
            }
            align_and_connect(room, &rope.at(h * rope_segments), rope_segments, list);
        }
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip, std::vector<sprites::rope_point>& ropes) {
        auto const& w = state.world;
        auto const d = data(state, c);
        auto const& p = w.creature_get_payload(c).stowaway;
        auto const& b = w.stowaway_data_get_body(d);
        auto const& g = w.stowaway_data_get_graphics(d);
        auto const chunks = w.stowaway_data_get_chunks(d);
        auto const feelers = w.stowaway_data_get_feelers(d);
        auto const rope = w.stowaway_data_get_rope(d);
        float const t = alpha;

        // ApplyPalette (RoomCamera.PixelColorAtCoordinate(colorPickPos): the
        // level image isn't read, the palette's colour of the walls behind
        // the play layer stands in, as for the White lizard)
        vec3 const black = pal.black();
        float const darkness = pal.darkness();
        vec3 const pixel = pal.texel(12, 9);
        vec3 const color = mix(pal.texel(12, 2), black, darkness);
        vec3 const color2 = mix(pal.texel(5, 0), black, darkness);
        vec3 const color3 = pal.texel(4, 3);

        auto const k0 = creature::chunk(state, c, 0), k1 = creature::chunk(state, c, 1);
        vec2 const c0 = w.body_chunk_get_pos(k0), c1 = w.body_chunk_get_pos(k1);
        vec2 const l0 = w.body_chunk_get_last_pos(k0), l1 = w.body_chunk_get_last_pos(k1);
        vec2 const vector = rw::lerp(l0, c0, t);
        vec2 const vector2 = rw::lerp(l1, c1, t);
        vec2 const vector3 = rw::lerp(custom::dir_vec(l0, l1), custom::dir_vec(c0, c1), t);
        vec2 const placed = b.placed_direction;
        float const mouth_open = p.mouth_open;
        // mouthExtension
        float const mouth_ext = std::sin(p.biting * 1.5707964f) * mouth_open;
        float const mo_me = mouth_open - mouth_ext;
        bool const killer = p.bite_is_killer != 0;

        //
        // Background: the heads
        //
        if (chunks.size() == head_count * head_chunks && rope.size() == head_count * rope_segments) {
            for (uint32_t k = 0; k < head_count; ++k) {
                auto const& h = b.heads[k];
                if (h.retract_fac >= 0.95f)
                    continue;
                auto const& tip = chunks.at(k * head_chunks + head_chunks - 1);
                auto const& before = chunks.at(k * head_chunks + head_chunks - 2);
                auto const& before2 = chunks.at(k * head_chunks + head_chunks - 3);
                vec2 const last_pos = tip.last_pos;
                vec2 vector5 = custom::dir_vec(before.last_pos, tip.last_pos);
                vector5 = rw::lerp(vector5, placed, mathf::pow(h.retract_fac, 6.f));
                vec2 const vector6 = tip.last_pos - vector5 * 5.f;
                vec2 const v = custom::dir_vec(before2.last_pos, tip.last_pos);
                // StowawayRopeGraphics.DrawSprite (timeStacker 0: the
                // segments' last positions; the body where it is now)
                rope_segment const* seg = &rope.at(k * rope_segments);
                uint32_t const n = rope_segments;
                size_t const first = ropes.size();
                vec2 v0 = c0;
                v0 += custom::dir_vec(seg[1].last_pos, v0) * 1.f;
                float const nverts = float(n * 4 - 1);
                ropes.push_back(sprites::rope_point{v0, stowaway::rad(0.f) * 1.7f + 2.f, opaque(color3)});
                vec2 prev = v0;
                for (uint32_t i = 0; i < n; ++i) {
                    float const f = float(i) / float(n - 1);
                    vec2 v2;
                    if (i >= n - 1)
                        v2 = seg[i].last_pos + custom::dir_vec(prev, seg[i].last_pos) * 1.f;
                    else
                        v2 = seg[i + 1].last_pos;
                    // the quad's far end: vertices i * 4 + 2, + 3
                    float const ct = (float(i * 4) + 2.5f) / nverts;
                    ropes.push_back(sprites::rope_point{v2, stowaway::rad(f) * 1.7f + 2.f, opaque(mix(color3, pixel, ct))});
                    prev = v2;
                }
                sprites::add_rope(out, ropes, first);
                sprites::add(out, sprite{"TrapHook", last_pos, custom::vec_to_deg(vector5), vec2{0.6f, 1.2f}, vec2{0.5f, 0.1f}, pixel});
                sprites::add(out, sprite{"mouseBodyA", vector6, custom::vec_to_deg(v), vec2{0.6f, 0.6f}, vec2{0.5f, 0.5f}, pixel});
            }
        }

        //
        // Midground: the body, the feelers, the teeth (sprites[0], a
        // mouseBodyA on the anchor, is invisible)
        //
        sprites::add(out, sprite{"mouseBodyB", vector2, custom::vec_to_deg(vector3), vec2{1.f, 1.f}, vec2{0.5f, 0.1f}, color3});
        {
            vec2 const a0 = stowaway::attach_pos(state, c, t);
            uint32_t at = 0;
            for (uint32_t i = 0; i < b.feeler_count; ++i) {
                uint32_t const n = b.feeler_len[i];
                if (at + n > feelers.size())
                    break;
                // the long mesh's quads from the attachment through every
                // point (half width 0.5; vertex colours by their index)
                size_t const first = ropes.size();
                float const nverts = float(mathf::max(1.f, float(n * 4 - 1)));
                ropes.push_back(sprites::rope_point{a0, 0.25f, opaque(color)});
                for (uint32_t j = 0; j < n; ++j) {
                    auto const& q = feelers.at(at + j);
                    float const ct = (float(j * 4) + 2.5f) / nverts;
                    vec3 const col = mix(color3, color, 1.f - mathf::pow(mathf::clamp01(ct), 0.06f));
                    ropes.push_back(sprites::rope_point{rw::lerp(q.last_pos, q.pos, t), 0.5f, opaque(col)});
                }
                sprites::add_rope(out, ropes, first);
                at += n;
            }
        }
        float const bb = (killer ? 47.f : 35.f) * (1.f - p.sleep_scale) + g.over_shoot;
        vec2 const vector7 = custom::perpendicular_vector(vector3);
        float const num3 = mathf::lerp(6.f, killer ? 25.f : 15.f, mo_me);
        float const num4 = num3 * 0.95f;
        {
            int32_t const tc = std::max(1, b.teeth_count);
            float const half = float(tc) / 2.f;
            // InitiateSprites: the teeth's lengths (Random from teethSeed,
            // with the index stepping by two, as vanilla's)
            float scale_y[8] = {};
            rw::unity_random r{b.teeth_seed};
            for (int32_t k = 0; k < tc * 2 && k / 2 < 8; k += 2) {
                float const num2 = float(k) < half ? mathf::inverse_lerp(0.f, half, float(k)) : mathf::inverse_lerp(float(tc), half, float(k));
                scale_y[k / 2] = r.range(0.8f, 1.2f) + (1.f - num2) / 20.f;
            }
            vec2 const vector8 = rw::lerp(vector, vector2 + vector3 * mathf::lerp(10.f + g.over_shoot, bb, mouth_ext), mouth_open * (1.f - p.sleep_scale));
            bool const visible = mouth_open * (1.f - p.sleep_scale) > 0.f;
            for (int32_t l = 0; l < tc && l < 8 && visible; ++l) {
                float const num5 = mathf::lerp(24.f, 0.f, mo_me);
                float const num6 = float(l) < half ? mathf::inverse_lerp(0.f, half, float(l)) : mathf::inverse_lerp(float(tc), half, float(l));
                vec2 const vector9 = vector7 * num4 - vector7 * mathf::lerp(0.f, num4 * 2.f, float(l) / (float(tc) - 1.f));
                float const sx = float(l) < half ? 1.f : -1.f;
                float const num7 = (1.f - num6) * mathf::lerp(-10.f + num5, -51.f, mo_me) * sx;
                float const rot = custom::vec_to_deg(vector3) + num7 + 2.5f;
                vec2 const tp = vector8 + vector9;
                sprites::add(out, sprite{"SpiderLeg0A", tp, rot, vec2{0.4f * sx, scale_y[l]}, vec2{0.5f, 0.1f}, color});
                sprites::add(out, sprite{"SpiderLeg0B", tp, rot, vec2{1.f * sx, scale_y[l]}, vec2{0.5f, 0.1f}, pixel});
            }
        }

        //
        // Items: the hood, its lips, the lumps on the ceiling
        //
        {
            constexpr uint32_t num8 = 10;
            vec2 const vector10 = vector - placed * 20.f;
            vec2 a3 = vector10;
            vec2 b2{0.f, 0.f};
            vec2 const a4 = custom::perpendicular_vector(placed);
            vec2 vs[num8 * 4];
            for (uint32_t m = 0; m < num8; ++m) {
                float const num9 = float(m) / (float(num8) - 1.f);
                vec2 const vector11 = rw::lerp(vector10, vector2 + vector3 * mathf::lerp(10.f, bb, mouth_ext) + vector3 * mathf::lerp(-5.f, 4.f, mouth_open), num9);
                float num10 = num3 * (std::sin(num9 * 5.f) / 1.4f + mathf::pow(num9, 2.f) * 1.4f + mouth_open * mathf::pow(num9, 2.f)) + 0.3f;
                num10 *= 1.f - num9 / 2.f;
                num10 += mathf::lerp(8.f, 18.f, mouth_open);
                vec2 vector12 = vector7 * num10;
                num10 = mathf::lerp(42.f, num10, num9 * 2.f);
                vector12 = rw::lerp(a4 * num10, vector12, num9);
                vs[m * 4] = a3 - b2;
                vs[m * 4 + 1] = a3 + b2;
                vs[m * 4 + 2] = vector11 - vector12;
                vs[m * 4 + 3] = vector11 + vector12;
                a3 = vector11;
                b2 = vector12;
            }
            sprites::long_mesh(out, strip, vs, num8 * 4, color);
            // (the second hood mesh through the TubeWorm shader, in color2,
            // isn't ported: its rings would need the shader)
            vec2 const v8 = vector2 + vector3 * mathf::lerp(10.f + g.over_shoot, bb, mouth_ext) + vector3 * mathf::lerp(-5.f, 4.f, mouth_open);
            float const num11 = mathf::lerp(2.f, 16.f, mo_me);
            vec2 const lip_scale{mathf::lerp(0.6f, 0.5f, mo_me), mathf::lerp(1.3f, 1.9f, mo_me)};
            sprites::add(out, sprite{"DangleFruit0A", v8 + vector7 * -num11, custom::vec_to_deg(vector3) - 100.f, lip_scale, vec2{0.5f, 0.5f}, color});
            sprites::add(out, sprite{"DangleFruit0A", v8 + vector7 * num11, custom::vec_to_deg(vector3) + 180.f - 80.f, lip_scale, vec2{0.5f, 0.5f}, color});
        }
        {
            orbs const o = body_orbs(w.stowaway_data_get_seed(d), placed);
            // InitiateSprites' masses' rotations: Custom.RNV() after the
            // teeth's rolls from teethSeed
            float mass_rot[8] = {};
            {
                rw::unity_random r{b.teeth_seed};
                for (int32_t k = 0; k < b.teeth_count * 2; k += 2)
                    (void)r.range(0.8f, 1.2f);
                for (uint32_t l = 0; l < o.count; ++l)
                    mass_rot[l] = custom::vec_to_deg(custom::rnv(r));
            }
            float const nf = float(o.count);
            for (uint32_t n = 0; n < o.count; ++n)
                sprites::add(out, sprite{"LizardScaleA6", b.original_pos + o.pos[n] + placed * o.radius[n], custom::vec_to_deg(placed), vec2{1.1f, 1.5f},
                                      vec2{0.5f, 0.f}, mix(color3, color2, float(n) / nf)});
            for (uint32_t n = 0; n < o.count; ++n) {
                vec2 const vector13 = rw::normalized(o.pos[n]) * 5.f * mo_me;
                float const sc = o.radius[n] / 7.f;
                sprites::add(out, sprite{"KrakenShield0", b.original_pos + vector13 + o.pos[n], mass_rot[n], vec2{sc, sc}, vec2{0.5f, 0.5f},
                                      mix(color, color2, float(n) / nf)});
            }
        }
    }
}
