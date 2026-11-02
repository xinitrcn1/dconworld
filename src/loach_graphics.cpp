// LoachGraphics.cs with its LoachTentacleGraphic (RopeGraphic) and Dangler
// (HasDanglers). Everything here is cosmetic and only updates where a
// camera looks. The RotLoach (the Rot Behemoth) has its rot: fifteen
// DaddyGraphics.Eye bulbs over the front of the body and the circles
// (growths and little eyes) along its mouth tentacles, in its region's
// corruption colour.

#include <algorithm>
#include <cmath>
#include <string_view>

#include "loach_graphics.hpp"
#include "body_parts.hpp"
#include "creature.hpp"
#include "loach.hpp"
#include "player.hpp"
#include "rooms.hpp"
#include "tracker.hpp"

namespace loach_graphics {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;
    using loach::dangler_segment;
    using loach::graphics_state;

    namespace {
        // LoachGraphics.legGraphicAnchors[1, ...] / [0, ...]: the leg
        // sprites' anchors (the rain deer's), A and B legs, upper and lower part
        constexpr vec2 leg_anchor[2][2] = {
            {vec2{8.f / 19.f, 5.f / 44.f}, vec2{6.f / 13.f, 4.f / 62.f}},
            {vec2{3.5f / 32.f, 3.f / 69.f}, vec2{6.f / 13.f, 3.f / 56.f}},
        };
        constexpr std::string_view leg_elements[2][2] = {{"deerLegA1", "deerLegA2"}, {"deerLegB1", "deerLegB2"}};
        // Loach.bodyChunkConnections' distances
        constexpr float connection_distance[loach::body_chunks - 1] = {50.f, 48.f, 48.f, 30.f, 20.f};
        // the chunks' rotationChunk (the connections set them in order)
        constexpr uint32_t rotation_chunk[loach::body_chunks] = {1, 2, 3, 4, 5, 4};
        // LoachGraphics.breathsPerSecond, swayRate
        constexpr float breaths_per_second = 0.33333334f;
        constexpr float sway_rate = 0.5555556f;
        // Dangler.DanglerProps()
        constexpr float dangler_gravity = 0.9f;
        constexpr float dangler_air_friction = 0.98f;
        constexpr float dangler_water_gravity = -0.2f;
        constexpr float dangler_water_friction = 0.8f;
        constexpr float dangler_elasticity = 0.85f;
        constexpr float dangler_weight_symmetry = 0.7f;
        // Loach's effectColor (Color.gray); a RotLoach's without its
        // region's corruption colours (the vanilla fallback)
        constexpr vec3 effect_gray{0.5f, 0.5f, 0.5f};
        constexpr vec3 effect_rot{0.f, 0.f, 1.f};

        dcon::loach_data_id data(sys::game_state const& state, dcon::creature_id c) { return creature::loach_data(state.world, c); }

        vec2 chunk_pos(sys::game_state const& state, dcon::creature_id c, uint32_t i, float alpha) {
            auto const k = creature::chunk(state, c, i);
            return rw::lerp(state.world.body_chunk_get_last_pos(k), state.world.body_chunk_get_pos(k), alpha);
        }
        float chunk_rad(sys::game_state const& state, dcon::creature_id c, uint32_t i) { return state.world.body_chunk_get_rad(creature::chunk(state, c, i)); }
        // BodyChunk.Rotation
        vec2 chunk_rotation(sys::game_state const& state, dcon::creature_id c, uint32_t i, float alpha) {
            return rw::normalized(chunk_pos(state, c, i, alpha) - chunk_pos(state, c, rotation_chunk[i], alpha));
        }

        // the limbs by kind and number (none for a missing one)
        void limbs_of(sys::game_state const& state, dcon::creature_id c, loach::limb const* legs[loach::leg_count], loach::limb const* tents[loach::tentacle_count]) {
            auto const& w = state.world;
            for (uint32_t i = 0; i < loach::leg_count; ++i)
                legs[i] = nullptr;
            for (uint32_t i = 0; i < loach::tentacle_count; ++i)
                tents[i] = nullptr;
            for (auto const o : creature::parts(w, c)) {
                auto const p = w.part_ownership_get_part(o);
                auto const kind = w.creature_part_get_kind(p);
                uint32_t const n = w.creature_part_get_index(p);
                auto const id = dcon::loach_limb_id{dcon::loach_limb_id::value_base_t(w.creature_part_get_payload(p).loach_limb.data)};
                if (kind == creature::part_kind::loach_leg && n < loach::leg_count)
                    legs[n] = &w.loach_limb_get_t(id);
                else if (kind == creature::part_kind::loach_tentacle && n < loach::tentacle_count)
                    tents[n] = &w.loach_limb_get_t(id);
            }
        }
        // Tentacle.CurrentLength of a limb (its segments' tiles)
        float limb_length(sys::game_state const& state, dcon::creature_id c, bool mouth, uint32_t n) {
            auto const& w = state.world;
            for (auto const o : creature::parts(w, c)) {
                auto const p = w.part_ownership_get_part(o);
                auto const kind = w.creature_part_get_kind(p);
                if (w.creature_part_get_index(p) != n || kind != (mouth ? creature::part_kind::loach_tentacle : creature::part_kind::loach_leg))
                    continue;
                auto const segs = w.loach_limb_get_segments(dcon::loach_limb_id{dcon::loach_limb_id::value_base_t(w.creature_part_get_payload(p).loach_limb.data)});
                float num = float(segs.size()) * 20.f;
                for (uint32_t i = 0; i + 2 < segs.size(); ++i) {
                    ivec2 const a = segs.at(i), b = segs.at(i + 2);
                    if (a.x != b.x && a.y != b.y && (std::abs(a.x - b.x) < 2 || std::abs(a.y - b.y) < 2))
                        num -= 5.9000006f;
                }
                return num;
            }
            return 0.f;
        }

        // the first segment of dangler `k` among loach_data.danglers
        uint32_t dangler_start(graphics_state const& g, uint32_t k) {
            uint32_t s = 0;
            for (uint32_t i = 0; i < k; ++i)
                s += g.danglers[i].segs;
            return s;
        }

        // LoachGraphics.DanglerConnection
        vec2 dangler_connection(sys::game_state const& state, dcon::creature_id c, graphics_state const& g, uint32_t index, float alpha) {
            auto const& a = g.danglers[index];
            vec2 const p = chunk_pos(state, c, a.first_chunk, alpha);
            vec2 const q = chunk_pos(state, c, a.first_chunk + 1u, alpha);
            return rw::lerp(p, q, a.lerp) + a.offset;
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

        // LoachGraphics.GetChunkVisualRad
        float chunk_visual_rad(sys::game_state const& state, dcon::creature_id c, graphics_state const& g, uint32_t index, float alpha) {
            float const num = float(index) / float(loach::body_chunks - 1);
            float const num2 = mathf::lerp(g.last_breath_factor, g.breath_factor, alpha);
            return chunk_rad(state, c, index) * (1.f + 0.1f * std::sin(num * mathf::pi) * num2);
        }

        // LoachGraphics.AlongBodyPos
        vec2 along_body_pos(sys::game_state const& state, dcon::creature_id c, graphics_state const& g, float x, float alpha) {
            uint32_t const num = loach::body_chunks - 1;
            float const r0 = chunk_rad(state, c, 0);
            float const rn = chunk_rad(state, c, num);
            if (x <= r0) {
                x -= r0;
                vec2 const v = chunk_pos(state, c, 0, alpha);
                vec2 const a = chunk_pos(state, c, 1, alpha);
                return v + rw::normalized(a - v) * x;
            }
            if (x >= g.body_length - rn) {
                x -= g.body_length - rn;
                vec2 const a2 = chunk_pos(state, c, num, alpha);
                vec2 const b = chunk_pos(state, c, num - 1, alpha);
                return a2 + rw::normalized(a2 - b) * x;
            }
            x -= r0;
            uint32_t num2 = 0;
            while (num2 < loach::body_chunks - 2 && x > connection_distance[num2]) {
                x -= connection_distance[num2];
                ++num2;
            }
            return rw::lerp(chunk_pos(state, c, num2, alpha), chunk_pos(state, c, num2 + 1, alpha), x / connection_distance[num2]);
        }

        // LoachGraphics.AlongBodyRad
        float along_body_rad(sys::game_state const& state, dcon::creature_id c, graphics_state const& g, float x, float alpha, bool use_breath) {
            uint32_t const num = loach::body_chunks - 1;
            float const r0 = chunk_rad(state, c, 0);
            float const rn = chunk_rad(state, c, num);
            float const num2 = use_breath ? chunk_visual_rad(state, c, g, 0, alpha) : r0;
            float const num3 = use_breath ? chunk_visual_rad(state, c, g, num, alpha) : rn;
            if (x < r0) {
                x -= r0;
                return std::sqrt(mathf::max(0.f, 1.f - mathf::pow(mathf::max(x / r0, -1.f), 2.f))) * num2;
            }
            if (x > g.body_length - rn) {
                x -= g.body_length - rn;
                return std::sqrt(mathf::max(0.f, 1.f - mathf::pow(mathf::min(x / num3, 1.f), 2.f))) * num3;
            }
            x -= r0;
            uint32_t num4 = 0;
            while (num4 < loach::body_chunks - 2 && x > connection_distance[num4]) {
                x -= connection_distance[num4];
                ++num4;
            }
            float const a = use_breath ? chunk_visual_rad(state, c, g, num4, alpha) : chunk_rad(state, c, num4);
            float const b = use_breath ? chunk_visual_rad(state, c, g, num4 + 1, alpha) : chunk_rad(state, c, num4 + 1);
            return mathf::lerp(a, b, x / connection_distance[num4]);
        }

        //
        // The RotLoach's rot (DaddyGraphics.Eye through IHaveRotGraphics)
        //
        // Custom.BezierDerivative
        vec2 bezier_derivative(vec2 a, vec2 ca, vec2 b, vec2 cb, float f) {
            return (ca - a) * (3.f * (1.f - f) * (1.f - f)) + (cb - ca) * (6.f * (1.f - f) * f) + (b - cb) * (3.f * f * f);
        }
        // EyeData.IdealPos: along the curve over the front three chunks
        vec2 eye_ideal_pos(sys::game_state const& state, dcon::creature_id c, loach::rot_eye const& e) {
            vec2 a = chunk_pos(state, c, 0, 1.f);
            vec2 const v = chunk_pos(state, c, 1, 1.f);
            a += rw::normalized(a - v) * (chunk_rad(state, c, 0) * 0.9f);
            vec2 const b = chunk_pos(state, c, 2, 1.f);
            vec2 const a2 = custom::bezier(a, v, b, v, e.along_body);
            vec2 const a3 = custom::perpendicular_vector(bezier_derivative(a, v, b, v, e.along_body));
            return a2 + a3 * e.perp_offset;
        }
        // EyeData.PushOutFromPos
        vec2 eye_push_out_from(sys::game_state const& state, dcon::creature_id c, loach::rot_eye const& e, float alpha) {
            vec2 const a = chunk_pos(state, c, 0, alpha);
            vec2 const v = chunk_pos(state, c, 1, alpha);
            vec2 const b = chunk_pos(state, c, 2, alpha);
            if (e.along_body < 0.5f)
                return rw::lerp(a, v, e.along_body * 2.f);
            return rw::lerp(v, b, e.along_body * 2.f - 1.f);
        }
        // where a tracked creature is for an eye (DangerPos in sight, else
        // the best guess's tile); false when it isn't tracked any more
        bool eye_target(sys::game_state const& state, dcon::creature_id c, room_view const& room, dcon::creature_id t, vec2& out) {
            auto const* r = tracker::find(state, c, t);
            if (!r)
                return false;
            out = r->visual && creature::here(state, t, room.id) ? creature::main_pos(state, t) : room_view::middle_of_tile(tracker::best_guess(*r));
            return true;
        }
        // DaddyGraphics.Eye.Update (FindNewLookObject: the tracker's
        // creatures; the loach keeps no NoiseTracker sources)
        void eye_update(sys::game_state& state, dcon::creature_id c, room_view const& room, graphics_state& g, uint32_t index, rw::rng& rng) {
            auto const& w = state.world;
            auto& e = g.eyes[index];
            bool const dead = w.creature_get_dead(c) != 0;
            int32_t const stun = w.creature_get_stun(c);
            e.last_dir = e.dir;
            e.last_closed = e.closed;
            float const delay = float(std::max<int32_t>(1, e.eyes_closed_delay));
            // (RotEyesClose: the stun; Creature.Deaf isn't kept)
            e.closed = mathf::lerp(e.closed, mathf::inverse_lerp(0.f, delay, float(stun)), 1.f / delay);
            if (stun == 0)
                e.eyes_closed_delay = int16_t(rng.range(1, 20));
            vec2 look{0.f, 0.f};
            if (e.creature_seen && !eye_target(state, c, room, e.creature_seen, look))
                e.creature_seen = dcon::creature_id{};
            float num = 0.f;
            if (look.x != 0.f && look.y != 0.f) {
                vec2 const at = e.pos;
                e.dir = rw::slerp(e.dir, custom::dir_vec(at, look) * mathf::inverse_lerp(0.f, 200.f, rw::distance(at, look)), 0.3f);
                num = e.light * mathf::inverse_lerp(0.f, 1.f, rw::distance(e.last_dir, e.dir));
                e.light = mathf::max(dead ? 0.f : 0.2f, e.light - 0.05f);
            } else {
                e.dir *= 0.9f;
                e.light = mathf::max(dead ? 0.f : 0.1f, e.light - 0.05f);
                // FindNewLookObject
                auto const& ts = tracker::of(state, c);
                if (ts.count > 0) {
                    auto const who = ts.reps[uint32_t(rng.range(0, int32_t(ts.count)))].who;
                    bool flag = true;
                    for (uint32_t k = 0; k < g.eye_count && flag; ++k)
                        if (g.eyes[k].creature_seen == who)
                            flag = false;
                    if (flag) {
                        e.creature_seen = who;
                        e.light = mathf::max(0.75f, e.light);
                    }
                }
            }
            e.flash = mathf::max(0.f, e.flash - 0.16666667f);
            if (rng.value() < num)
                e.get_to_focus = mathf::max(e.get_to_focus, rng.value());
            else if (rng.value() < 0.014285714f)
                e.get_to_focus = 0.f;
            e.last_focus = e.focus;
            if (e.focus < e.get_to_focus)
                e.focus = mathf::min(e.focus + 0.05f, e.get_to_focus);
            else
                e.focus = mathf::max(e.focus - 0.05f, e.get_to_focus);
        }
        // LoachGraphics.ReactToCreature: the eye facing it turns to it,
        // preferring one not looking at anything
        void react_to_creature(sys::game_state& state, dcon::creature_id c, room_view const& room, graphics_state& g, dcon::creature_id t, rw::rng& rng) {
            if (g.eye_count == 0)
                return;
            vec2 at;
            if (!eye_target(state, c, room, t, at))
                return;
            auto const* r = tracker::find(state, c, t);
            vec2 const p = room_view::middle_of_tile(tracker::best_guess(*r));
            vec2 const pos = chunk_pos(state, c, 0, 1.f);
            float best = -3.4e38f;
            uint32_t pick = 0;
            for (uint32_t i = 0; i < g.eye_count; ++i) {
                float num3 = rw::dot(custom::dir_vec(pos, g.eyes[i].pos), custom::dir_vec(pos, p));
                if (g.eyes[i].creature_seen)
                    num3 -= 2.f;
                if (num3 > best) {
                    best = num3;
                    pick = i;
                }
            }
            // Eye.ReactToCreature (no Daddy_React_To_Tentacle_Touch sound)
            auto& e = g.eyes[pick];
            e.creature_seen = t;
            e.light = mathf::max(e.light, rng.value());
        }

        // Dangler.Reset
        void reset_danglers(sys::game_state& state, dcon::creature_id c) {
            auto& w = state.world;
            auto const d = data(state, c);
            auto const& g = w.loach_data_get_graphics(d);
            auto segs = w.loach_data_get_danglers(d);
            uint32_t k = 0;
            for (uint32_t i = 0; i < loach::dangler_count; ++i) {
                vec2 at = dangler_connection(state, c, g, i, 1.f);
                for (uint32_t j = 0; j < g.danglers[i].segs && k < segs.size(); ++j, ++k) {
                    auto& s = segs.at(k);
                    at.y -= s.con_rad;
                    s.pos = at;
                    s.last_pos = at;
                    s.vel = vec2{0.f, 0.f};
                }
            }
        }

        // LoachTentacleGraphic.MoveSegment (Custom.RectCollision out of a
        // solid tile: the goal itself)
        void tentacle_move_segment(room_view const& room, daddy::rope_segment& s, vec2 goal, vec2 smoothed) {
            s.vel = vec2{0.f, 0.f};
            if (room.get_tile(smoothed).solid() && !room.get_tile(goal).solid()) {
                s.pos = goal;
                return;
            }
            s.pos = smoothed;
        }

        // RopeGraphic.AlignAndConnect (as daddy_graphics.cpp's)
        void align_and_connect(room_view const& room, daddy::rope_segment* seg, uint32_t n, vec2 const* list, uint32_t count) {
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
                s.vel *= 0.f;
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
                tentacle_move_segment(room, s, rw::lerp(seg[i].pos, seg[j].pos, t), custom::bezier(seg[i].pos, v, seg[j].pos, v2, t));
            }
            for (uint32_t k = 0; k < n; ++k)
                seg[k].bend_index = -1;
        }

        // LoachTentacleGraphic: OneDimensionalTubePos (unused without the
        // rot's circles) is left out
    }

    void create(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        auto const d = data(state, c);
        rw::unity_random r{w.loach_data_get_seed(d)};
        graphics_state g;
        // bodyLength: the end chunks' radii and the connections
        g.body_length = chunk_rad(state, c, 0) + chunk_rad(state, c, loach::body_chunks - 1);
        for (float const cd : connection_distance)
            g.body_length += cd;
        // the danglers
        static thread_local std::vector<dangler_segment> segs;
        segs.clear();
        for (uint32_t j = 0; j < loach::dangler_count; ++j) {
            float num = mathf::lerp(6.f, 12.f, mathf::pow(r.value(), 3.f));
            auto& a = g.danglers[j];
            if (r.value() < 0.25f) {
                float const rad = chunk_rad(state, c, 5);
                a.first_chunk = 4;
                a.lerp = 1.f;
                a.offset = body_parts::rotate_around_origo(vec2{0.f, -rad * 0.7f}, r.range(-70.f, 70.f));
            } else {
                int32_t const num3 = r.range(0, int32_t(loach::body_chunks) - 2);
                float const value = r.value();
                float const num4 = mathf::lerp(chunk_rad(state, c, uint32_t(num3)), chunk_rad(state, c, uint32_t(num3 + 1)), value);
                a.first_chunk = uint8_t(num3);
                a.lerp = value;
                a.offset = vec2{0.f, -num4 * 0.7f};
            }
            float const num5 = (float(a.first_chunk) + a.lerp) / float(loach::body_chunks);
            num *= 0.3f + 0.7f * num5 * (1.f - num5) * 4.f;
            num = mathf::max(num, 3.f);
            // Dangler(this, j, (int)num, 3f, 4f)
            uint32_t const n = uint32_t(int32_t(num));
            a.segs = uint8_t(n);
            for (uint32_t k = 0; k < n; ++k) {
                float const num6 = n > 1 ? float(k) / float(n - 1) : 0.f;
                dangler_segment s;
                s.rad = 3.f * (mathf::pow(1.f - num6, 0.6f) + std::sin((num6 * 0.9f + 0.1f) * mathf::pi));
                s.con_rad = 4.f;
                s.stretched_rad = s.rad;
                segs.push_back(s);
            }
        }
        // GenerateSpots: 35 points in the unit circle (Random.insideUnitCircle),
        // pushed apart
        vec2 pts[loach::spot_count];
        for (auto& v : pts) {
            float const ang = r.value() * 2.f * mathf::pi;
            float const rr = std::sqrt(r.value());
            v = vec2{std::cos(ang), std::sin(ang)} * rr;
            v *= (rw::sqr_magnitude(v) + 1.f) / 2.f;
        }
        float const lim = 0.2f;
        for (int32_t it = 0; it < 200; ++it)
            for (uint32_t k = 0; k < loach::spot_count; ++k)
                for (uint32_t l = k + 1; l < loach::spot_count; ++l) {
                    vec2 const a = pts[l] - pts[k];
                    float const sq = rw::sqr_magnitude(a);
                    if (sq < lim * lim && sq > 0.f) {
                        float const num2 = std::sqrt(sq);
                        vec2 const b = a * ((lim - num2) / num2 * 0.2f);
                        pts[k] -= b;
                        pts[k].x = mathf::max(pts[k].x, -1.f);
                        pts[l] += b;
                        pts[l].x = mathf::max(pts[l].x, -1.f);
                    }
                }
        for (uint32_t m = 0; m < loach::spot_count; ++m) {
            auto& s = g.spots[m];
            s.attachment = pts[m];
            s.lightness = mathf::pow(r.value(), 2.f) * 0.5f + 0.25f;
            s.scale = r.value() * 0.4f + 0.6f;
            s.angle_factor = r.value() * 0.5f + 0.25f;
        }
        // Loach's effectColor and eyeColor: grey, or a RotLoach's region's
        // corruption colour (corruptionEyeColor isn't kept per region here:
        // the effect colour for both)
        bool const rot = loach::rotted(state, c);
        if (rot) {
            auto const region = w.room_get_region_from_region_membership(creature::room_of(state, c));
            g.effect_color = region ? w.region_get_corruption_effect_color(region) : effect_rot;
        } else {
            g.effect_color = effect_gray;
        }
        g.eye_color = g.effect_color;
        // the LoachTentacleGraphics: a RotLoach's tentacle circles
        for (uint32_t l = 0; l < loach::tentacle_count; ++l) {
            g.circle_count[l] = 0;
            if (!rot)
                continue;
            int32_t const num = r.range(8, 11);
            auto* cs = g.circles[l];
            uint32_t n = 0;
            for (int32_t i = 0; i < num; ++i) {
                float const num2 = 1.f - float(i) / float(num - 1);
                vec2 const offset{r.range(-1.f, 1.f) * 5.f * num2, float(i) * 4.f};
                float const a = r.value();
                float const t = r.value();
                float num3 = mathf::lerp(a, num2, t) * 4.f + 1.5f;
                if (i == 0)
                    num3 = mathf::lerp(num3, 5.f, 0.5f);
                cs[n++] = loach::tentacle_circle{offset, num3, 0};
            }
            for (int32_t j = 0; j < num; ++j) {
                float const num4 = 1.f - float(j) / float(num - 1);
                if (r.value() < 0.6f * num4 && n < loach::max_tentacle_circles) {
                    float const radius = cs[j].radius * mathf::lerp(0.2f, 0.8f, mathf::pow(r.value(), mathf::lerp(1.5f, 0.5f, num4)));
                    cs[n++] = loach::tentacle_circle{cs[j].offset, radius, 1};
                }
            }
            g.circle_count[l] = uint8_t(n);
        }
        // a RotLoach's Eyes (DaddyGraphics.Eye) with their EyeData
        g.eye_count = rot ? uint8_t(loach::rot_eye_count) : 0;
        g.reaction_timer = 0;
        float const main_rad = chunk_rad(state, c, 0);
        for (uint32_t m = 0; m < g.eye_count; ++m) {
            auto& e = g.eyes[m];
            e = loach::rot_eye{};
            float const num7 = float(m) / float(loach::rot_eye_count - 1);
            float const a = r.value();
            float const b = r.value();
            e.rad = mathf::lerp(5.f, 13.f, mathf::lerp(1.f - num7, a, b));
            e.rotats[0] = r.value() * 360.f;
            e.rotats[1] = r.value();
            e.along_body = mathf::pow(num7, 2.5f);
            e.perp_offset = main_rad * r.range(-1.f, 1.f) * mathf::lerp(0.5f, 1.f, std::sqrt(num7));
            e.jag_seed = rw::keyed::mix(uint32_t(w.loach_data_get_seed(d)) ^ (m + 1u) * 0x9E3779B1u) | 1u;
        }
        w.loach_data_set_graphics(d, g);
        w.loach_data_get_danglers(d).load_range(segs.data(), segs.data() + segs.size());
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        auto& g = w.loach_data_get_graphics(data(state, c));
        // LoachTentacleGraphic.Reset(mainBodyChunk.pos)
        vec2 const head = w.body_chunk_get_pos(creature::chunk(state, c, 0));
        for (auto& tg : g.tentacle_segments)
            for (auto& s : tg) {
                s.pos = head;
                s.last_pos = head;
                s.vel = vec2{0.f, 0.f};
                s.bend_index = -1;
            }
        reset_danglers(state, c);
        // EyeData.Reset
        for (uint32_t m = 0; m < g.eye_count && m < loach::rot_eye_count; ++m) {
            auto& e = g.eyes[m];
            e.pos = eye_ideal_pos(state, c, e);
            e.last_pos = e.pos;
            e.vel = vec2{0.f, 0.f};
        }
    }

    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room) {
        auto& w = state.world;
        auto const d = data(state, c);
        auto& g = w.loach_data_get_graphics(d);
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        auto const& pl = w.creature_get_payload(c).loach;
        bool const cons = creature::consious(state, c);
        vec2 const head = w.body_chunk_get_pos(creature::chunk(state, c, 0));
        // the breath and the walk sway
        ++g.breath_timer;
        g.last_breath_factor = g.breath_factor;
        if (cons) {
            g.breath_factor = mathf::lerp(g.breath_factor, std::sin(float(g.breath_timer) / 40.f * mathf::pi * 2.f * breaths_per_second), 0.1f);
        } else {
            g.breath_timer = 0;
            g.breath_factor = mathf::lerp(g.breath_factor, 0.f, 0.01f);
        }
        g.last_walk_sway = g.walk_sway;
        if (cons) {
            float const num = mathf::min(1.f, std::fabs(w.body_chunk_get_vel(creature::chunk(state, c, 2)).x) / 2.f);
            g.walk_timer += num * sway_rate / 40.f * mathf::pi * 2.f;
            g.walk_timer = std::fmod(g.walk_timer, 6.2831855f);
            g.walk_sway = mathf::lerp(g.walk_sway, std::sin(g.walk_timer) * num, 0.1f);
        } else {
            g.walk_sway = mathf::lerp(g.walk_sway, 0.f, 0.01f);
        }
        // the tentacles: FloatBase and the chunks (no ropes: one position each)
        loach::limb const* legs[loach::leg_count];
        loach::limb const* tents[loach::tentacle_count];
        limbs_of(state, c, legs, tents);
        for (uint32_t i = 0; i < loach::tentacle_count; ++i) {
            if (!tents[i])
                continue;
            vec2 list[loach::limb_chunks + 1];
            list[0] = w.body_chunk_get_pos(creature::chunk(state, c, tents[i]->connected_chunk));
            for (uint32_t j = 0; j < loach::limb_chunks; ++j)
                list[j + 1] = tents[i]->c[j].pos;
            align_and_connect(room, g.tentacle_segments[i], loach::tentacle_graphic_segments, list, loach::limb_chunks + 1);
        }
        // the danglers
        {
            auto segs = w.loach_data_get_danglers(d);
            uint32_t k = 0;
            for (uint32_t i = 0; i < loach::dangler_count; ++i) {
                vec2 const con = dangler_connection(state, c, g, i, 1.f);
                float from_base = 0.f;
                for (uint32_t j = 0; j < g.danglers[i].segs && k < segs.size(); ++j, ++k) {
                    auto& s = segs.at(k);
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
                        auto& prev = segs.at(k - 1);
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
                        for (uint32_t q = 0; q < loach::body_chunks; ++q) {
                            // BodyPart.PushFromPoint
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
            }
        }
        // a RotLoach's eyes: EyeData.Update, then Eye.Update
        {
            vec2 const main_vel = w.body_chunk_get_vel(creature::chunk(state, c, 0));
            uint32_t const n = std::min<uint32_t>(g.eye_count, loach::rot_eye_count);
            for (uint32_t m = 0; m < n; ++m) {
                auto& e = g.eyes[m];
                e.last_pos = e.pos;
                e.pos += e.vel;
                vec2 const ideal = eye_ideal_pos(state, c, e);
                e.vel += (ideal - e.pos) * 0.5f;
                e.pos += (ideal - e.pos) * 0.5f;
                e.vel = rw::lerp(e.vel, main_vel, 0.2f);
                e.pos = rw::clamp_magnitude(e.pos - ideal, e.rad) + ideal;
            }
            for (uint32_t m = 0; m < n; ++m)
                eye_update(state, c, room, g, m, rng);
            if (g.reaction_timer > 0)
                --g.reaction_timer;
        }
        // the eyes' blink
        if (!cons)
            g.blink = 10;
        --g.blink;
        if (g.blink < -80 && rng.value() < 0.05f)
            g.blink = rng.range(25, 35);
        // LoachGraphics.Update: hunting, an eye turns to the focus creature
        if (g.eye_count > 0 && loach::behavior(w.creature_get_behavior(c)) == loach::behavior::hunt && g.reaction_timer == 0) {
            if (auto const f = loach::focus(state, c)) {
                g.reaction_timer = rng.range(10, 40);
                react_to_creature(state, c, room, g, f, rng);
            }
        }
        // CreatureLooker (simplified): the AI's focus creature, if it still
        // tracks it
        g.look_point[1] = g.look_point[0];
        vec2 look = head + vec2{100.f, -50.f};
        if (auto const f = loach::focus(state, c)) {
            if (auto const* r = tracker::find(state, c, f))
                look = r->visual && creature::here(state, f, room.id) ? creature::main_pos(state, f) : room_view::middle_of_tile(tracker::best_guess(*r));
        }
        g.look_point[0] = rw::clamp_magnitude(look - head, 200.f) / 200.f;
        // the flip, the resting
        g.last_flip = g.flip;
        g.flip = custom::lerp_and_tick(g.flip, pl.bod_dir.x, 0.05f, 0.005f);
        g.last_resting = g.resting;
        g.resting = pl.resting;
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip, std::vector<sprites::rope_point>& ropes) {
        auto const& w = state.world;
        auto const d = data(state, c);
        auto const& g = w.loach_data_get_graphics(d);
        auto const& pl = w.creature_get_payload(c).loach;
        bool const rot = loach::rotted(state, c);
        // ApplyPalette
        vec3 const black = pal.black();
        vec3 const body_color = rot ? black : black + (vec3{1.f, 1.f, 1.f} - black) * 0.1f;
        vec3 const effect = g.effect_color;
        float const num = mathf::lerp(g.last_flip, g.flip, alpha);
        vec2 const a = custom::dir_vec(chunk_pos(state, c, 3, alpha), chunk_pos(state, c, 1, alpha));
        static thread_local std::vector<vec2> verts;
        loach::limb const* legs[loach::leg_count];
        loach::limb const* tents[loach::tentacle_count];
        limbs_of(state, c, legs, tents);
        float const walk_sway = mathf::lerp(g.last_walk_sway, g.walk_sway, alpha);

        // the spots: worked out first, drawn behind everything (containers[0])
        // or in front of the body (containers[1])
        struct spot_draw {
            vec2 pos;
            float rotation;
            vec2 scale;
            bool front;
        };
        spot_draw spots[loach::spot_count];
        {
            float const num6 = num * mathf::pi * 0.5f + walk_sway * mathf::pi * 0.06f;
            float const num7 = g.body_length - 100.f;
            float const num8 = g.body_length - 3.f;
            float const num9 = 2.f;
            vec2 const main = chunk_pos(state, c, 0, alpha);
            for (uint32_t s = 0; s < loach::spot_count; ++s) {
                auto const& sp = g.spots[s];
                float const num11 = mathf::lerp(num7, num8, sp.attachment.x * 0.5f + 0.5f);
                float const num12 = sp.attachment.y * (num8 - num7) / 2.f * num9;
                vec2 a7 = along_body_pos(state, c, g, num11, alpha);
                float const num13 = along_body_rad(state, c, g, num11, alpha, true);
                vec2 v = custom::dir_vec(along_body_pos(state, c, g, num11 - 20.f, alpha), along_body_pos(state, c, g, num11 + 20.f, alpha));
                vec2 const a8 = -custom::perpendicular_vector(v);
                v = rw::slerp(v, a7 - main, sp.angle_factor);
                float const f2 = num12 / along_body_rad(state, c, g, num11, alpha, false) + mathf::pi - num6;
                bool const flag = std::cos(f2) > 0.f;
                a7 += a8 * (std::sin(f2) * num13);
                spots[s] = spot_draw{a7, custom::vec_to_deg(v), vec2{sp.scale * -mathf::sign(num), sp.scale}, flag};
            }
        }
        std::string_view const spot_a = rot ? "GrowthA0" : "GrowthA1";
        std::string_view const spot_b = rot ? "GrowthB0" : "GrowthB1";
        auto const draw_spots = [&](bool front) {
            // (a container's spots: the A sprites first, then the B sprites)
            for (uint32_t s = 0; s < loach::spot_count; ++s)
                if (spots[s].front == front)
                    sprites::add(out, sprites::sprite{spot_a, spots[s].pos, spots[s].rotation, spots[s].scale, vec2{0.5f, 0.f}, body_color});
            for (uint32_t s = 0; s < loach::spot_count; ++s)
                if (spots[s].front == front) {
                    vec3 const col = body_color + (effect - body_color) * g.spots[s].lightness;
                    sprites::add(out, sprites::sprite{spot_b, spots[s].pos, spots[s].rotation, spots[s].scale, vec2{0.5f, 0.f}, col});
                }
        };
        draw_spots(false);

        // the tentacles (LoachTentacleGraphic.DrawSprite): its long mesh as
        // one rope (the GPU makes the ribbon out of its points, like daddy
        // long legs' tubes). MakeLongMesh's four vertices per segment are two
        // points here, a third of the way in from either end, with the half
        // widths the vertex pairs have there; straight segments between them
        // as the mesh's (the sides bisect the bends instead of following each
        // pair's own segment: the same outline where the tentacle is smooth)
        {
            vec3 const tip_color = rot ? effect : effect + (body_color - effect) * 0.75f;
            uint32_t const n = loach::tentacle_graphic_segments;
            // verticeColors, in pairs along the mesh: pair j's
            auto const color = [&](uint32_t j) {
                float const f = float(2 * j) / float(4 * n - 2);
                vec3 const cc = body_color + (tip_color - body_color) * (mathf::pow(mathf::clamp01(f * 1.5f - 0.5f), 2.f) * 0.4f);
                return vec4{cc.x, cc.y, cc.z, 1.f};
            };
            for (uint32_t t = 0; t < loach::tentacle_count; ++t) {
                if (!tents[t])
                    continue;
                auto const& seg = g.tentacle_segments[t];
                size_t const first = ropes.size();
                vec2 vector = chunk_pos(state, c, tents[t]->connected_chunk, alpha);
                vector += custom::dir_vec(rw::lerp(seg[1].last_pos, seg[1].pos, alpha), vector);
                float aw = 2.f;
                for (uint32_t i = 0; i < n; ++i) {
                    float const num1 = float(i) / float(n - 1);
                    vec2 const v2 = rw::lerp(seg[i].last_pos, seg[i].pos, alpha);
                    vec2 const nn = rw::normalized(vector - v2);
                    float const dd = rw::distance(vector, v2) / 3.f;
                    float num2;
                    if (rot)
                        num2 = 2.5f + 1.f * mathf::pow(1.f - num1, 2.f);
                    else
                        num2 = (1.5f + 2.f * mathf::pow(1.f - num1, 2.f)) * mathf::pow(1.f - num1, 0.25f) + 3.f * std::sin(mathf::pi * mathf::pow(num1, 10.f));
                    ropes.push_back(sprites::rope_point{vector - nn * dd, mathf::lerp(aw, num2, 0.5f), color(2 * i)});
                    ropes.push_back(sprites::rope_point{v2 + nn * dd, num2, color(2 * i + 1)});
                    vector = v2;
                    aw = num2;
                }
                sprites::add_rope(out, ropes, first, false);
                // a RotLoach's circles along it (Circle20, radius / 10)
                uint32_t const nc = std::min<uint32_t>(g.circle_count[t], loach::max_tentacle_circles);
                if (nc == 0)
                    continue;
                float const len = mathf::max(limb_length(state, c, true, t), 1.f);
                // LoachTentacleGraphic.OneDimensionalTubePos
                auto const tube_pos = [&](float fp) {
                    int32_t const a = custom::int_clamp(int32_t(std::floor(fp * float(n - 1))), 0, int32_t(n) - 1);
                    int32_t const b = custom::int_clamp(a + 1, 0, int32_t(n) - 1);
                    float const tt = mathf::inverse_lerp(float(a), float(b), fp * float(n - 1));
                    return rw::lerp(rw::lerp(seg[a].last_pos, seg[b].last_pos, tt), rw::lerp(seg[a].pos, seg[b].pos, tt), alpha);
                };
                for (uint32_t j = 0; j < nc; ++j) {
                    auto const& cc = g.circles[t][j];
                    float const y = 1.f - mathf::clamp01(cc.offset.y / len);
                    // OnTubePos
                    vec2 const p = tube_pos(y - 1.f / float(n));
                    vec2 const p2 = tube_pos(y + 1.f / float(n));
                    vec2 const at = tube_pos(y) + custom::perpendicular_vector(custom::dir_vec(p, p2)) * cc.offset.x;
                    float const num2 = nc > 1 ? float(j) / float(nc - 1) : 0.f;
                    vec3 const col = body_color + (effect - body_color) * (cc.eye ? 1.f : 0.4f - 0.1f * num2);
                    float const sc = cc.radius / 10.f;
                    sprites::add(out, sprites::sprite{"Circle20", at, 0.f, vec2{sc, sc}, vec2{0.5f, 0.5f}, col});
                }
            }
        }

        // the danglers (Dangler.DrawSprite)
        {
            auto const segs = w.loach_data_get_danglers(d);
            for (uint32_t i = 0; i < loach::dangler_count; ++i) {
                uint32_t const s0 = dangler_start(g, i);
                if (s0 >= segs.size() || g.danglers[i].segs == 0)
                    continue;
                verts.clear();
                vec2 v = dangler_connection(state, c, g, i, alpha);
                float sr = segs.at(s0).stretched_rad;
                for (uint32_t j = 0; j < g.danglers[i].segs && s0 + j < segs.size(); ++j) {
                    auto const& s = segs.at(s0 + j);
                    vec2 const v2 = rw::lerp(s.last_pos, s.pos, alpha);
                    vec2 const nn = rw::normalized(v2 - v);
                    vec2 const pa = custom::perpendicular_vector(nn);
                    float const dd = rw::distance(v2, v) / 5.f;
                    float const r0 = (sr + s.stretched_rad) * 0.5f;
                    if (j == 0) {
                        verts.push_back(v - pa * r0);
                        verts.push_back(v + pa * r0);
                    } else {
                        verts.push_back(v - pa * r0 + nn * dd);
                        verts.push_back(v + pa * r0 + nn * dd);
                    }
                    verts.push_back(v2 - pa * s.stretched_rad - nn * dd);
                    verts.push_back(v2 + pa * s.stretched_rad - nn * dd);
                    sr = s.stretched_rad;
                    v = v2;
                }
                sprites::long_mesh(out, strip, verts.data(), verts.size(), body_color);
            }
        }

        // the shoulders over the legs (MakeLongMeshAtlased(5))
        for (uint32_t l = 0; l < loach::leg_count; ++l) {
            if (!legs[l])
                continue;
            auto const& lg = *legs[l];
            vec2 const vector = chunk_pos(state, c, lg.connected_chunk, alpha);
            float const rad = chunk_rad(state, c, lg.connected_chunk);
            vec2 const nn = rw::normalized(rw::lerp(lg.c[1].last_pos, lg.c[1].pos, alpha) - vector);
            float const num2 = limb_length(state, c, false, l) / mathf::max(1.f, lg.max_length);
            verts.clear();
            constexpr uint32_t nv = 20;
            for (uint32_t m = 0; m < nv; m += 2) {
                float const num3 = float(m) / float(nv - 2) * 2.f - 1.f;
                vec2 const a3 = body_parts::rotate_around_origo(-nn, num3 * 50.f);
                float const num4 = (1.f - num2 * 0.75f) * (std::cos(num3 * mathf::pi) * 0.5f + 0.5f) * 25.f;
                verts.push_back(vector + a3 * rad * 0.85f);
                verts.push_back(vector + a3 * (rad * 0.85f + num4));
            }
            sprites::long_mesh(out, strip, verts.data(), verts.size(), body_color);
        }

        // the body: the JaggedCircle shader over Futile_White (16 pixels),
        // drawn as Circle20
        for (uint32_t k = 0; k < loach::body_chunks; ++k) {
            vec2 const a2 = chunk_pos(state, c, k, alpha);
            float const sc = chunk_visual_rad(state, c, g, k, alpha) / 8.f * 1.05f * 0.8f;
            sprites::add(out, sprites::sprite{"Circle20", a2, custom::vec_to_deg(chunk_rotation(state, c, k, alpha)) + float(k) * 40.f, vec2{sc, sc}, vec2{0.5f, 0.5f}, body_color});
            if (k < loach::body_chunks - 1) {
                vec2 const b = chunk_pos(state, c, k + 1, alpha);
                float const sc2 = mathf::lerp(chunk_visual_rad(state, c, g, k, alpha), chunk_visual_rad(state, c, g, k + 1, alpha), 0.5f) / 8.f * 1.05f * 0.8f;
                float const rr = custom::vec_to_deg(rw::lerp(chunk_rotation(state, c, k, alpha), chunk_rotation(state, c, k + 1, alpha), 0.5f)) + 120.f + float(k) * 40.f;
                sprites::add(out, sprites::sprite{"Circle20", rw::lerp(a2, b, 0.5f), rr, vec2{sc2, sc2}, vec2{0.5f, 0.5f}, body_color});
            }
        }

        draw_spots(true);

        // the legs
        float const resting = mathf::lerp(g.last_resting, g.resting, alpha);
        float const ph = pl.preferred_height;
        auto const leg_part_length = [&](int32_t pos, int32_t part, bool ext) {
            float const k = ext ? ph / 15.f : 1.f;
            if (pos == 0)
                return (part == 0 ? 35.f : 55.f) * k;
            return (part == 0 ? 60.f : 45.f) * k;
        };
        auto const tc = [&](loach::limb const& l, uint32_t i) { return rw::lerp(l.c[i].last_pos, l.c[i].pos, alpha); };
        vec2 const head = chunk_pos(state, c, 0, alpha);
        for (uint32_t n = 0; n < loach::leg_count; ++n) {
            if (!legs[n])
                continue;
            auto const& lg = *legs[n];
            int32_t const kind = int32_t(n % 2);
            vec2 v3 = chunk_pos(state, c, lg.connected_chunk, alpha);
            vec2 v4 = tc(lg, 1);
            if (n % 2 == 0) {
                v4 = rw::lerp(v4, v3, 0.2f) + a * 20.f;
                v3 = rw::lerp(v3, head, 0.4f) + vec2{0.f, -20.f} + a * 5.f;
            } else {
                v3 = rw::lerp(v3, chunk_pos(state, c, 4, alpha), 0.5f) + vec2{0.f, -5.f};
                v3 -= a * 17.f;
                v4 -= a * 10.f;
            }
            vec2 v5 = custom::inverse_kinematic(v3, v4, leg_part_length(kind, 0, true), leg_part_length(kind, 1, true), (n % 2 == 0 ? 1.f : -1.f) * mathf::sign(num));
            v5 = rw::lerp(v5, v3 + custom::dir_vec(v3, tc(lg, 0)) * leg_part_length(kind, 0, true), 1.f - std::fabs(num));
            vec2 v6 = tc(lg, 4);
            vec2 const v7 = tc(lg, 5);
            v6 = rw::lerp(v6, v7 + custom::dir_vec(v7, tc(lg, 4)) * 40.f, 0.5f);
            float const num17 = rw::distance(v4, v6);
            vec2 v8 = custom::inverse_kinematic(v4, v6, mathf::lerp(num17 / 2.f, 80.f, 0.5f), mathf::lerp(num17 / 2.f, 80.f, 0.5f), -mathf::sign(num));
            v8 = rw::lerp(v8, rw::lerp(tc(lg, 2), tc(lg, 3), 0.5f), mathf::lerp(1.f, 0.5f, std::fabs(num)));
            float const num18 = num < ((n % 2 == 0) ? 0.5f : -0.5f) ? -1.f : 1.f;
            if (n % 2 == 0 && std::fabs(num) < 0.7f && mathf::sign(num) != num18) {
                v3 += custom::perpendicular_vector(rw::normalized(v3 - v5)) * 50.f * num18 * mathf::inverse_lerp(0.f, 0.7f, std::fabs(num));
                v3 = v5 + custom::dir_vec(v5, v3) * leg_part_length(1, 0, true) * (1.f - resting);
            }
            float const l0 = leg_part_length(kind, 0, false);
            float const l1 = leg_part_length(kind, 1, false);
            sprites::add(out, sprites::sprite{leg_elements[kind][0], v5, custom::aim(v5, v3),
                vec2{num18 * mathf::min(1.f, rw::distance(v3, v5) / l0), rw::distance(v3, v5) / l0}, leg_anchor[kind][0], body_color});
            sprites::add(out, sprites::sprite{leg_elements[kind][1], v4, custom::aim(v4, v5),
                vec2{num18 * mathf::min(1.f, rw::distance(v5, v4) / l1), rw::distance(v5, v4) / l1}, leg_anchor[kind][1], body_color});
            // the lower leg's mesh (MakeLongMesh(7))
            verts.clear();
            vec2 v9 = v5;
            for (int32_t k = 0; k < 7; ++k) {
                vec2 v10 = v5;
                float dd = mathf::lerp(5.f, 1.f, float(k) / 6.f);
                float d2 = mathf::lerp(5.f, 1.f, (float(k) + 0.5f) / 6.f);
                switch (k) {
                case 0:
                    v10 = v4 - custom::dir_vec(v5, v8) * 2.5f;
                    dd = 0.f;
                    d2 = 4.f;
                    break;
                case 1:
                    v10 = v4 + custom::dir_vec(v5, v8) * 7.5f;
                    dd = 4.f;
                    d2 = 6.f;
                    break;
                case 2: v10 = v8 - custom::dir_vec(v4, v6) * 7.5f; break;
                case 3:
                    v10 = v8 + custom::dir_vec(v4, v6) * 7.5f;
                    d2 = 4.5f;
                    break;
                case 4: v10 = v6 - custom::dir_vec(v8, v7) * 7.5f; break;
                case 5:
                    v10 = v6 + custom::dir_vec(v8, v7) * 7.5f;
                    dd = 3.f;
                    d2 = 2.f;
                    break;
                default: v10 = v7; break;
                }
                vec2 const nn = rw::normalized(v10 - v9);
                vec2 const a9 = custom::perpendicular_vector(nn);
                float d3 = rw::distance(v10, v9) / 5.f;
                vec2 const p2 = v10 - a9 * d2 - nn * d3;
                vec2 const p3 = v10 + a9 * d2 - nn * d3;
                if (k == 6)
                    d3 = rw::distance(v10, v9);
                verts.push_back(v9 - a9 * dd + nn * d3);
                verts.push_back(v9 + a9 * dd + nn * d3);
                verts.push_back(p2);
                verts.push_back(p3);
                v9 = v10;
            }
            sprites::long_mesh(out, strip, verts.data(), verts.size(), body_color);
        }

        // the peepers (not on a RotLoach)
        if (!rot) {
            vec2 const v2 = head;
            vec2 const b2 = chunk_pos(state, c, 1, alpha);
            vec2 const n2 = rw::normalized(v2 - b2);
            vec2 const a4 = custom::perpendicular_vector(n2);
            float const rad2 = chunk_rad(state, c, 0);
            vec2 const a5 = rw::lerp(g.look_point[1], g.look_point[0], alpha);
            vec3 const eye_color = body_color + (black - body_color) * 0.5f;
            vec3 const pupil_base{0.003921569f, 0.003921569f, 0.003921569f};
            vec3 const pupil_color = pupil_base + (vec3{1.f, 1.f, 1.f} - pupil_base) * 0.1f;
            for (int32_t n = 0; n < 2; ++n) {
                float const f = num + 1.2566371f * (n == 0 ? 1.f : -1.f) + walk_sway * mathf::pi * 0.06f;
                vec2 const a6 = v2 + n2 * 3.f + a4 * std::sin(f) * (rad2 - 3.f);
                float const num5 = std::cos(f);
                if (num5 <= 0.f)
                    continue;
                // (rotation: VecToDeg minus a quarter turn in radians, as vanilla)
                sprites::add(out, sprites::sprite{"LoachEye", a6, custom::vec_to_deg(n2) - 1.5707964f, vec2{1.f, num5}, vec2{0.5f, 0.5f}, eye_color});
                if (g.blink <= 0)
                    sprites::add(out, sprites::sprite{"Circle4", a6 + a5 * 2.f, 0.f, vec2{1.f, mathf::min(1.f, num5 * 3.f)}, vec2{0.5f, 0.5f}, pupil_color});
                else
                    sprites::add(out, sprites::sprite{"pixel", a6 + a5 * 2.f, custom::vec_to_deg(n2), vec2{1.f, 4.f}, vec2{0.5f, 0.5f}, pupil_color});
            }
        }

        // a RotLoach's Eyes (DaddyGraphics.Eye.DrawSprite, as daddy_graphics'):
        // the jagged bulb in the body colour (EyeBulbColor), two slits in the
        // eye colour (RotEyeColor), BumpSize 1, jittering while it digests
        {
            uint32_t const ne = std::min<uint32_t>(g.eye_count, loach::rot_eye_count);
            float const digesting = loach::most_digested(state, c);
            uint32_t roll = state.tick * 2654435761u + uint32_t(c.index()) * 97u + 1u;
            auto const flicker = [&roll]() {
                roll ^= roll << 13;
                roll ^= roll >> 17;
                roll ^= roll << 5;
                return float(roll >> 8) * (1.f / 16777215.f);
            };
            auto const mix = [](vec3 a, vec3 b, float t) { return a + (b - a) * mathf::clamp01(t); };
            // Eye.BulgeVertex
            auto const bulge = [](vec2 v, vec2 dir, float rad) { return rw::lerp(v, rw::clamp_magnitude(v + dir * rad, rad), rw::magnitude(dir)); };
            constexpr float bump = 1.f;
            for (uint32_t j = 0; j < ne; ++j) {
                auto const& e = g.eyes[j];
                vec2 const rnd = custom::deg_to_vec(flicker() * 360.f);
                vec2 const pos = rw::lerp(e.last_pos, e.pos, alpha) + rnd * (digesting * 4.f * flicker());
                vec2 const middle = eye_push_out_from(state, c, e, alpha);
                float const rotation = custom::aim(pos, middle) + e.rotats[0];
                sprites::add_circle(out, pos, e.rad * 1.1f + 2.f, body_color, rotation, 0.16f, e.jag_seed);
                // Eye.RenderSlits
                float const num0 = mathf::pow(mathf::max(0.f, mathf::lerp(e.last_closed, e.closed, alpha)), 0.6f);
                float const num2 = bump * (1.f - num0);
                vec2 const b = rw::lerp(e.last_dir, e.dir, alpha);
                float num3 = mathf::lerp(e.last_focus, e.focus, alpha) * mathf::pow(mathf::inverse_lerp(-1.f, 1.f, rw::dot(custom::dir_vec(middle, pos), rw::normalized(b))), 0.7f);
                num3 = mathf::max(num3, num0);
                float num4 = mathf::inverse_lerp(0.f, mathf::lerp(30.f, 50.f, e.rotats[1]), rw::distance(middle, pos + custom::dir_vec(middle, pos) * e.rad)) * 0.9f;
                num4 = mathf::lerp(num4, 1.f, 0.5f * num3);
                vec2 const a = rw::lerp(custom::dir_vec(middle, pos) * num4, b, rw::magnitude(b) * 0.5f);
                vec3 render = mix(g.eye_color, vec3{1.f, 1.f, 1.f}, mathf::lerp(flicker() * e.light, 1.f, num0));
                if (num0 > 0.f)
                    render = mix(render, body_color, num0);
                render = mix(render, vec3{1.f, 1.f, 1.f}, e.flash);
                float const squint = mathf::lerp(1.f, 0.6f, num3);
                for (int i = 0; i < 2; ++i) {
                    vec2 const v = custom::deg_to_vec(rotation + 90.f * float(i));
                    vec2 const a2 = custom::perpendicular_vector(v);
                    vec2 sv[10];
                    sv[0] = pos + bulge(v * e.rad * 0.9f * squint, a, e.rad);
                    sv[9] = pos + bulge(v * -e.rad * 0.9f * squint, a, e.rad);
                    for (int jj = 1; jj < 5; ++jj)
                        for (int kk = 0; kk < 2; ++kk) {
                            float const dd = e.rad * (jj < 3 ? 0.7f : 0.25f) * (kk == 0 ? 1.f : -1.f) * squint;
                            int const num5 = kk == 0 ? jj : 9 - jj;
                            float const d2 = num2 * (jj < 3 ? 0.5f : 1.f) * (num5 % 2 == 0 ? 1.f : -1.f) * mathf::lerp(1.f, 2.5f, num3);
                            sv[num5] = pos + bulge(v * dd + a2 * d2, a, e.rad);
                        }
                    sprites::long_mesh(out, strip, sv, 10, render);
                }
            }
        }
    }
}
