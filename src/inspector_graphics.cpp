// InspectorGraphics.cs (with its InspectorHeadRopeGraphics and the
// CoralBrain Mycelium it owns). See inspector_graphics.hpp.

#include <algorithm>
#include <cmath>
#include <limits>

#include "inspector_graphics.hpp"
#include "body_parts.hpp"
#include "creature.hpp"
#include "inspector.hpp"
#include "inspector_types.hpp"
#include "particles.hpp"
#include "pathing.hpp"
#include "player_types.hpp"

namespace inspector_graphics {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using sprites::sprite;
    using world::room_view;
    using inspector::head_chunks;
    using inspector::head_count;
    using inspector::rope_segment;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        constexpr uint32_t n_segs = inspector::rope_segments;
        // wingBodyPartDistance
        constexpr float wing_distance = 75.f;

        inspector::body_payload const& pl(sys::game_state const& state, dcon::creature_id c) { return state.world.creature_get_payload(c).inspector; }
        dcon::inspector_data_id data(sys::game_state const& state, dcon::creature_id c) { return inspector::data_of(state.world, c); }
        vec2 body_pos(sys::game_state const& state, dcon::creature_id c) { return state.world.body_chunk_get_pos(creature::chunk(state, c, 0)); }
        vec2 body_vel(sys::game_state const& state, dcon::creature_id c) { return state.world.body_chunk_get_vel(creature::chunk(state, c, 0)); }
        // game.SeededRandom(seed)
        float seeded(int32_t seed) {
            rw::unity_random r{seed};
            return r.value();
        }
        int32_t seed_of(sys::game_state const& state, dcon::creature_id c) { return state.world.inspector_data_get_seed(data(state, c)); }
        uint32_t wing_count(sys::game_state const& state, dcon::creature_id c) {
            return std::min<uint32_t>(pl(state, c).wing_number, inspector::max_wings);
        }

        // Custom.VerticalCrossPoint / HorizontalCrossPoint / RectCollision's
        // point (as tentacle_plant_graphics.cpp's)
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

        // InspectorHeadRopeGraphics.MoveSegment: kept out of the terrain
        void move_segment(room_view const& room, rope_segment& s, vec2 goal, vec2 smoothed) {
            s.vel = vec2{0.f, 0.f};
            if (room.get_tile(smoothed).solid() && !room.get_tile(goal).solid()) {
                ivec2 const t = room_view::get_tile_position(smoothed);
                s.pos = rect_collision(smoothed, goal, float(t.x) * 20.f - 3.f, float(t.y) * 20.f - 3.f, float(t.x) * 20.f + 23.f, float(t.y) * 20.f + 23.f);
                return;
            }
            s.pos = smoothed;
        }

        // RopeGraphic.AlignAndConnect with its Segment.Update
        void align_and_connect(room_view const& room, rope_segment* seg, uint32_t n, vec2 const* list, uint32_t count) {
            float num = 0.f;
            for (uint32_t i = 1; i < count; ++i)
                num += rw::distance(list[i - 1], list[i]);
            float num2 = 0.f;
            for (uint32_t j = 0; j < count; ++j) {
                if (j > 0)
                    num2 += rw::distance(list[j - 1], list[j]);
                if (j == 0 || list[j - 1] != list[j]) {
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

        // Mycelium.Reset (InspectorGraphics.ResetDir: the body's velocity)
        template<typename R>
        void reset_mycelium(inspector::mycelium const& m, inspector::mycelium_point* pts, vec2 at, vec2 reset_dir, R& r) {
            vec2 const v = at + reset_dir * (m.length * 0.6f);
            float const dist = rw::distance(at, v);
            vec2 const ca = at + rw::normalized(custom::dir_vec(at, v) + custom::rnv(r)) * dist * 0.5f;
            vec2 const cb = v + rw::normalized(custom::dir_vec(v, at) + custom::rnv(r)) * dist * 0.5f;
            for (uint32_t i = 0; i < m.count; ++i) {
                float const f = m.count > 1 ? float(i) / float(m.count - 1) : 0.f;
                pts[i].pos = custom::bezier(at, ca, v, cb, f) + custom::rnv(r);
                pts[i].last_pos = pts[i].pos;
                pts[i].vel = custom::rnv(r);
            }
        }

        // Mycelium.Update (no CoralNeuronSystem: no connections; not
        // culled, moveAwayFromWalls)
        void mycelium_update(inspector::mycelium const& m, inspector::mycelium_point* pts, vec2 connection, room_view const& room,
            std::vector<uint16_t> const& prox) {
            uint32_t const n = m.count;
            vec2 p0[inspector::max_mycelium_points], p2[inspector::max_mycelium_points];
            for (uint32_t i = 0; i < n; ++i) {
                p0[i] = pts[i].pos + pts[i].vel;
                pts[i].last_pos = pts[i].pos;
                p2[i] = pts[i].vel * 0.999f;
            }
            auto const proximity = [&](ivec2 t) -> int32_t {
                if (!room.inside(t.x, t.y))
                    return std::numeric_limits<int32_t>::max();
                return prox[size_t(t.y) * size_t(room.width) + size_t(t.x)];
            };
            for (uint32_t i = 0; i < n; ++i) {
                if (i > 0) {
                    vec2 const f = p0[i] - p0[i - 1];
                    float const num = rw::magnitude(f);
                    vec2 const d = rw::normalized(f) * (m.con_rad - num) * 0.5f;
                    p0[i] += d;
                    p2[i] += d;
                    p0[i - 1] -= d;
                    p2[i - 1] -= d;
                    if (i > 1) {
                        vec2 const s = rw::normalized(p0[i] - p0[i - 2]);
                        p2[i] += s * 0.2f;
                        p2[i - 2] -= s * 0.2f;
                    }
                }
                ivec2 const t = room_view::get_tile_position(p0[i]);
                int32_t const num2 = proximity(t);
                if (num2 < 4) {
                    vec2 f3{0.f, 0.f};
                    for (auto const dj : custom::four_directions) {
                        int32_t num3 = 0;
                        for (auto const dk : custom::four_directions) {
                            ivec2 const q = t + dj + dk;
                            if (room.inside(q.x, q.y))
                                num3 += prox[size_t(q.y) * size_t(room.width) + size_t(q.x)];
                        }
                        f3 += rw::to_vec2(dj) * float(num3);
                    }
                    float const num4 = custom::lerp_map(float(num2), 0.f, 3.f, 2.f, 0.2f);
                    p2[i] += rw::normalized(f3) * num4;
                }
            }
            for (uint32_t i = 0; i < n; ++i) {
                pts[i].pos = p0[i];
                pts[i].vel = p2[i];
            }
            if (n > 0) {
                pts[0].pos = connection;
                pts[0].vel = vec2{0.f, 0.f};
            }
        }

        // the constructor's layout, from the seed (true when laid out now)
        bool build_layout(sys::game_state& state, dcon::creature_id c) {
            auto& w = state.world;
            auto const d = data(state, c);
            auto& g = w.inspector_data_get_graphics(d);
            if (g.built)
                return false;
            g.built = 1;
            int32_t const seed = w.inspector_data_get_seed(d);
            int32_t const count = std::clamp(int32_t(10.f + 15.f * seeded(seed)), 0, int32_t(inspector::max_mycelia));
            g.mycelia_count = uint8_t(count);
            for (int32_t k = 0; k < count; ++k) {
                auto& m = g.mycelia[k];
                m.length = mathf::lerp(120.f, 300.f, seeded(int32_t(uint32_t(seed) + uint32_t(k))));
                float const num = mathf::max(m.length, mathf::lerp(m.length, 40.f, 0.5f));
                m.count = uint8_t(custom::int_clamp(int32_t(num / 15.f), 2, int32_t(inspector::max_mycelium_points)));
                m.con_rad = m.length / float(m.count);
            }
            w.inspector_data_get_mycelium_points(d).resize(uint32_t(count) * inspector::max_mycelium_points);
            vec2 const at = body_pos(state, c);
            for (auto& wp : g.wings)
                wp = inspector::wing_part{at, at, vec2{0.f, 0.f}};
            return true;
        }

        // InspectorGraphics.findWingFlapIntensity
        float wing_flap_intensity(uint32_t wings, float body_rotation, uint32_t wing, vec2 v) {
            float const num = 360.f / float(wings) * float(wing);
            float t = (std::atan2(-v.y, v.x) * 57.295776f) - (num + body_rotation);
            t = mathf::clamp(t - std::floor(t / 360.f) * 360.f, 0.f, 360.f);
            if (t > 180.f)
                t -= 360.f;
            return mathf::inverse_lerp(0.f, 180.f, t) * rw::magnitude(v);
        }

        vec4 rgba(vec3 c, float a) { return vec4{c.x, c.y, c.z, a}; }
        void add_sprite(std::vector<sprites::draw_cmd>& out, sprite const& s, float a) {
            vec4 const col = rgba(s.color, a);
            vec4 const corners[4] = {col, col, col, col};
            sprites::add(out, s, corners);
        }
        // the draw time Random.value of bodyColor's flicker
        float color_roll(sys::game_state const& state, dcon::creature_id c) {
            auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
            return rng.value();
        }
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        build_layout(state, c);
        auto const rid = creature::room_of(state, c);
        if (!rid)
            return;
        auto const room = world::get_room_view(state, rid);
        vec2 const at = body_pos(state, c);
        // InspectorGraphics.Reset: each neck from the body to its tip
        for (uint32_t i = 0; i < head_count; ++i) {
            auto const hd = inspector::head_of(state, c, i);
            auto& hg = w.inspector_head_get_graphics(hd);
            vec2 const list[2] = {at, w.inspector_head_get_t(hd).c[head_chunks - 1].pos};
            align_and_connect(room, hg.rope, n_segs, list, 2);
        }
        auto const d = data(state, c);
        auto const& g = w.inspector_data_get_graphics(d);
        auto pts = w.inspector_data_get_mycelium_points(d);
        if (pts.size() < uint32_t(g.mycelia_count) * inspector::max_mycelium_points)
            return;
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        vec2 const dir = body_vel(state, c);
        for (uint32_t k = 0; k < g.mycelia_count; ++k)
            reset_mycelium(g.mycelia[k], pts.begin() + k * inspector::max_mycelium_points, at, dir, rng);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        if (build_layout(state, c)) {
            reset_positions(state, c);
            return;
        }
        auto const d = data(state, c);
        auto& g = w.inspector_data_get_graphics(d);
        auto const& p = pl(state, c);
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        vec2 const at = body_pos(state, c);
        vec2 const vel = body_vel(state, c);
        uint32_t const nw = wing_count(state, c);
        // the wings' body parts
        bool const narrow = pathing::narrow_space(room, room_view::get_tile_position(at));
        for (uint32_t i = 0; i < nw; ++i) {
            vec2 const a = custom::deg_to_vec(360.f / float(nw) * float(i) + g.body_rotation) * wing_distance;
            auto& wp = g.wings[i];
            player::body_part bp;
            bp.pos = wp.pos;
            bp.last_pos = wp.last_pos;
            bp.vel = wp.vel;
            bp.rad = 15.f;
            body_parts::connect_to_point(bp, at, wing_distance, false, 0.3f, vel + a / 3.f, 0.25f, 0.1f);
            if (rw::distance(at, bp.pos) > wing_distance * 1.1f || narrow)
                bp.pos = at;
            // GenericBodyPart(15, 0.2, 0.1).Update
            body_parts::generic_update(bp, room, 0.1f, 0.2f);
            wp.pos = bp.pos;
            wp.last_pos = bp.last_pos;
            wp.vel = bp.vel;
            // wingflapCounters (DrawSprites' in vanilla)
            g.wingflap[i] += 0.02f;
            g.wingflap[i] += wing_flap_intensity(nw, g.body_rotation, i, vel * -1.f) / 8.f;
            g.wingflap[i] += wing_flap_intensity(nw, g.body_rotation, i, p.flying_power) * 3.f;
        }
        // the mycelia
        {
            auto pts = w.inspector_data_get_mycelium_points(d);
            if (pts.size() >= uint32_t(g.mycelia_count) * inspector::max_mycelium_points) {
                auto const& prox = pathing::ai_terrain_proximity_map(state, room.id);
                for (uint32_t k = 0; k < g.mycelia_count; ++k)
                    mycelium_update(g.mycelia[k], pts.begin() + k * inspector::max_mycelium_points, at, room, prox);
            }
        }
        // the heads: jaws, the necks' ropes, the eyes
        bool const awake = creature::consious(state, c);
        for (uint32_t k = 0; k < head_count; ++k) {
            auto const part = creature::part(w, c, k);
            auto const hd = inspector::head_data(w, part);
            auto const& t = w.inspector_head_get_t(hd);
            auto& hg = w.inspector_head_get_graphics(hd);
            // InspectorHeadRopeGraphics.Update: FloatBase, then the chunks'
            // rope positions
            vec2 list[1 + head_chunks * (world::rope_state::max_bends + 1)];
            uint32_t count = 0;
            list[count++] = at;
            for (uint32_t i = 0; i < head_chunks; ++i) {
                auto const& r = t.c[i].rope;
                uint32_t const total = 2u + r.bend_count;
                for (uint32_t j = 1; j < total; ++j)
                    list[count++] = j - 1 < r.bend_count ? r.bends[j - 1].pos : r.b;
            }
            align_and_connect(room, hg.rope, n_segs, list, count);
            float const health = w.creature_part_get_health(part);
            if (health <= 0.f) {
                hg.jaw_wiggler += rng.value() * 0.01f;
                hg.jaw_angle = mathf::lerp(hg.jaw_angle, hg.jaw_angle + std::sin(hg.jaw_wiggler) * 1.2f, 0.1f);
            } else {
                hg.jaw_wiggler += rng.value() * (0.1f + p.anger);
                if (t.want.valid())
                    hg.jaw_angle = mathf::lerp(hg.jaw_angle, 25.f, 0.15f) + std::sin(hg.jaw_wiggler);
                if (t.grab.valid())
                    hg.jaw_angle = mathf::lerp(hg.jaw_angle, -15.f, 0.45f) + std::sin(hg.jaw_wiggler);
                hg.jaw_angle = mathf::lerp(hg.jaw_angle, -5.f, 0.1f) + std::sin(hg.jaw_wiggler);
            }
            // the eye sprite's colour and scaleX (eased in DrawSprites)
            vec3 const target = (p.active_eye == int8_t(k) && !t.limp) ? vec3{1.f, 1.f, 1.f} : vec3{0.f, 0.02f, 0.2f};
            hg.eye_color = hg.eye_color + (target - hg.eye_color) * 0.2f;
            if (health > 0.f)
                hg.eye_scale_x = mathf::lerp(mathf::lerp(hg.eye_scale_x, mathf::lerp(0.525f, 0.225f, p.anger), 0.15f), 0.1f, t.blink);
            else
                hg.eye_scale_x = mathf::lerp(hg.eye_scale_x, 0.125f, 0.06f);
            // dying: Sparks off the neck (the OverseerEffects as a few more)
            if (p.dying > 0.f) {
                vec3 const col = inspector::body_color(state, c, 0.f);
                for (int m = 0; m < 8; ++m) {
                    uint32_t const n = uint32_t(rng.range(0, int32_t(head_chunks)));
                    particles::spawn(state, room.id, particles::kind::spark, t.c[n].pos, vel * 0.5f + custom::rnv(rng) * 14.f * rng.value(), 2.f,
                        mathf::lerp(14.f, 21.f, rng.value()) / 40.f, col);
                }
            }
        }
        if (awake)
            g.body_rotation += p.flying_power.x * 3.f;
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const&, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip, std::vector<sprites::rope_point>& ropes) {
        auto const& w = state.world;
        auto const d = data(state, c);
        auto const& g = w.inspector_data_get_graphics(d);
        if (!g.built)
            return;
        auto const& p = pl(state, c);
        // (DrawSprites reads the body's pos, not interpolated)
        vec2 const pos = body_pos(state, c);
        float ca = 1.f;
        vec3 const bc = inspector::body_color(state, c, color_roll(state, c), &ca);
        // the core: sprites 1..3 (sprite 0's scale is 1.3 + Sin(lightpulse
        // + 0 / 0) / 4, NaN: it isn't drawn)
        for (int l = 1; l < 4; ++l) {
            float const sc = 1.3f + std::sin(p.lightpulse + 1.f) / 4.f;
            add_sprite(out, sprite{"Circle20", pos, 0.f, vec2{sc, sc}, vec2{0.5f, 0.5f}, bc}, ca);
        }
        // the mycelia (Mycelium.DrawSprites, UpdateColor(bodyColor, 0))
        {
            auto const pts = w.inspector_data_get_mycelium_points(d);
            if (pts.size() >= uint32_t(g.mycelia_count) * inspector::max_mycelium_points) {
                vec3 const tip = custom::hsl2rgb(0.4888889f, 0.5f, 0.2f);
                for (uint32_t k = 0; k < g.mycelia_count; ++k) {
                    auto const& m = g.mycelia[k];
                    auto const* mp = pts.begin() + k * inspector::max_mycelium_points;
                    vec2 mv[inspector::max_mycelium_points * 4];
                    vec4 mc[inspector::max_mycelium_points * 4];
                    // (ConnectionPos: the body)
                    vec2 a = pos;
                    for (uint32_t i = 0; i < m.count; ++i) {
                        vec2 const v = rw::lerp(mp[i].last_pos, mp[i].pos, alpha);
                        vec2 const n = rw::normalized(a - v);
                        vec2 const a2 = custom::perpendicular_vector(n);
                        float const d2 = rw::distance(a, v) / 5.f;
                        mv[i * 4] = a - n * d2 - a2 * 0.5f;
                        mv[i * 4 + 1] = a - n * d2 + a2 * 0.5f;
                        mv[i * 4 + 2] = v + n * d2 - a2 * 0.5f;
                        mv[i * 4 + 3] = v + n * d2 + a2 * 0.5f;
                        a = v;
                    }
                    uint32_t const nv = uint32_t(m.count) * 4;
                    for (uint32_t i = 0; i < nv; ++i) {
                        float const f = float(i) / float(nv - 1);
                        mc[i] = rgba(bc + (tip - bc) * f, ca);
                    }
                    for (uint32_t j = 1; j < 3 && j <= nv; ++j)
                        mc[nv - j] = vec4{0.f, 0.f, 1.f, ca};
                    sprites::long_mesh(out, strip, mv, mc, nv);
                }
            }
        }
        // the wings (CicadaWingA, the CicadaWing shader as the colour at the
        // sprite's alpha)
        {
            uint32_t const nw = wing_count(state, c);
            vec3 const grey = custom::hsl2rgb(0.2f, 0.2f, 0.4f);
            vec3 const wing_color = bc + (grey - bc) * 0.4f;
            float const len = mathf::lerp(3.6f, 6.f, seeded(seed_of(state, c))) * mathf::inverse_lerp(1.f, 0.8f, p.squeeze_fac);
            for (uint32_t j = 0; j < nw; ++j) {
                vec2 const b = custom::deg_to_vec(360.f / float(nw) * float(j) + g.body_rotation) * wing_distance;
                float const num = 360.f / float(nw) * float(j);
                float const sx = mathf::lerp(0.f, len, mathf::inverse_lerp(1.f, 0.55f, rw::distance(g.wings[j].pos, pos + b) / wing_distance));
                float const rot = num + g.body_rotation - 90.f + std::sin(g.wingflap[j]) * 8.f;
                add_sprite(out, sprite{"CicadaWingA", pos, rot, vec2{sx, 0.8f}, vec2{0.f, 0.5f}, wing_color}, 0.3f * ca);
            }
        }
        // the heads
        float const dying = mathf::lerp(p.last_dying, p.dying, alpha);
        float const dying_fac = mathf::pow(mathf::max(0.f, 1.f - dying), 0.2f);
        for (uint32_t m = 0; m < head_count; ++m) {
            auto const part = creature::part(w, c, m);
            auto const hd = inspector::head_data(w, part);
            auto const& t = w.inspector_head_get_t(hd);
            auto const& hg = w.inspector_head_get_graphics(hd);
            // the neck (InspectorHeadRopeGraphics.DrawSprite's long mesh, its
            // OverseerZip shader as the body colour) through the rope pipeline
            {
                rope_segment const* seg = hg.rope;
                vec2 vector = pos;
                vector += custom::dir_vec(rw::lerp(seg[1].last_pos, seg[1].pos, alpha), vector) * 1.f;
                vec4 const col = rgba(bc, ca);
                size_t const first = ropes.size();
                ropes.push_back(sprites::rope_point{vector, inspector::rad(0.f) * dying_fac * 1.7f + 2.f, col});
                for (uint32_t i = 0; i < n_segs; ++i) {
                    float const f = float(i) / float(n_segs - 1);
                    vec2 v2 = rw::lerp(seg[i].last_pos, seg[i].pos, alpha);
                    if (i >= n_segs - 1)
                        v2 += custom::dir_vec(vector, v2) * 1.f;
                    else
                        v2 = rw::lerp(seg[i + 1].last_pos, seg[i + 1].pos, alpha);
                    ropes.push_back(sprites::rope_point{v2, inspector::rad(f) * dying_fac * 1.7f + 2.f, col});
                    vector = v2;
                }
                sprites::add_rope(out, ropes, first, false);
            }
            vec2 const last = t.c[head_chunks - 2].last_pos;
            vec2 const tip = t.c[head_chunks - 1].last_pos;
            float const aim = custom::aim(last, tip);
            vec2 const dir = custom::deg_to_vec(aim);
            // the head
            add_sprite(out, sprite{"Circle20", tip, aim, vec2{0.8f, 1.4f}, vec2{0.5f, 0.3f}, bc}, ca);
            // the jaws (FlyWing, anchorY 0)
            vec2 const jaw = tip + dir * 12.f;
            add_sprite(out, sprite{"FlyWing", jaw, aim + hg.jaw_angle, vec2{0.8f, 1.1f}, vec2{0.5f, 0.f}, bc}, ca);
            add_sprite(out, sprite{"FlyWing", jaw, aim + 180.f - hg.jaw_angle, vec2{0.8f, -1.1f}, vec2{0.5f, 0.f}, bc}, ca);
            vec2 const back = tip + dir * -5.f;
            add_sprite(out, sprite{"FlyWing", back, aim + hg.jaw_angle - 5.f, vec2{-1.3f, 2.2f}, vec2{0.5f, 0.f}, bc}, ca);
            add_sprite(out, sprite{"FlyWing", back, aim + 180.f - hg.jaw_angle + 5.f, vec2{-1.3f, -2.2f}, vec2{0.5f, 0.f}, bc}, ca);
            // the eye
            sprites::add(out, sprite{"Circle20", tip + dir * 10.f, aim, vec2{hg.eye_scale_x, 0.625f}, vec2{0.5f, 0.5f}, hg.eye_color});
        }
    }

    uint32_t lights(sys::game_state const& state, dcon::creature_id c, float alpha, light (&out)[max_lights]) {
        auto const& w = state.world;
        if (w.creature_get_away(c) != 0)
            return 0;
        auto const& p = pl(state, c);
        vec2 const pos = rw::lerp(w.body_chunk_get_last_pos(creature::chunk(state, c, 0)), body_pos(state, c), alpha);
        float ca = 1.f;
        vec3 const bc = inspector::body_color(state, c, color_roll(state, c), &ca);
        float const num = inspector::attention_flicker(state, c);
        float const num2 = mathf::inverse_lerp(1.f, 0.8f, p.squeeze_fac);
        float const s = std::sin(p.lightpulse);
        uint32_t n = 0;
        // myLight
        out[n++] = light{pos, 210.f + num + 20.f * s + 1900.f * p.dying, ((0.9f + s / 10.f) * num2 + p.dying) * ca, bc, false};
        // mySuperGlow (flat)
        vec3 const glow = bc + (vec3{0.f, 0.f, 1.f} - bc) * mathf::clamp01(inspector::glow_mix(state, c));
        out[n++] = light{pos, 50.f + num + 10.f * s + 90.f * p.dying, (num2 + p.dying) * ca, glow, true};
        // the headlights (flat, white), 9 px ahead of each head
        for (uint32_t k = 0; k < head_count && n < max_lights; ++k) {
            auto const& t = w.inspector_head_get_t(inspector::head_of(state, c, k));
            vec2 const v = custom::deg_to_vec(custom::aim(t.c[head_chunks - 2].last_pos, t.c[head_chunks - 1].last_pos)) * 9.f;
            out[n++] = light{t.c[head_chunks - 1].pos + v, 60.f + 10.f * std::sin(p.lightpulse + float(k)) - p.dying, 0.23f * num2 + p.dying, vec3{1.f, 1.f, 1.f}, true};
        }
        return n;
    }
}
