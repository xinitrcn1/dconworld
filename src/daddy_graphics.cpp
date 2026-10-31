#include <algorithm>
#include <cmath>
#include <vector>

#include "daddy.hpp"
#include "daddy_graphics.hpp"
#include "daddy_internal.hpp"
#include "body_parts.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "player.hpp"

namespace daddy_graphics {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using world::room_view;
    using daddy::tube_kind;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // PlayerGraphics.DefaultSlugcatColor(Red) greyed: the Hunter long legs' body
        vec3 hunter_color() {
            vec3 const red{1.f, 0.4509804f, 0.4509804f};
            return red + (vec3{0.5f, 0.5f, 0.5f} - red) * 0.4f;
        }

        vec3 mix(vec3 a, vec3 b, float t) { return a + (b - a) * mathf::clamp01(t); }

        // Custom.VerticalCrossPoint / HorizontalCrossPoint
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

        // Custom.RectCollision's corner: where the way from `last` to `pos`
        // first meets the rectangle's edges (`pos` when it doesn't)
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

        dcon::body_chunk_id chunk(sys::game_state const& state, dcon::creature_id d, uint32_t i) { return daddy::chunk(state, d, i); }

        // GenericBodyPart.Update; BodyPart.PushOutOfTerrain only looks at the
        // tile and its eight neighbours, none solid three steps from the terrain
        void generic_update(sys::game_state& state, player::body_part& bp, room_view const& room, float air_friction, float surface_friction) {
            bp.last_pos = bp.pos;
            bp.pos += bp.vel;
            bp.vel *= air_friction;
            ivec2 const t = room_view::get_tile_position(bp.pos);
            auto const& prox = pathing::ai_terrain_proximity_map(state, room.id);
            if (!room.inside(t.x, t.y) || prox[size_t(t.y) * size_t(room.width) + size_t(t.x)] < 3)
                body_parts::push_out_of_terrain(bp, room, surface_friction);
        }

        float bump_size(sys::game_state const& state, dcon::creature_id d) { return daddy::size_class(state, d) ? 1.f : 0.8f; }

        //
        // RopeGraphic (AlignAndConnect, Segment.Update) with
        // DaddyTubeGraphic.MoveSegment
        //
        void tube_move_segment(room_view const& room, daddy::rope_segment& s, vec2 goal, vec2 smoothed) {
            s.vel = vec2{0.f, 0.f};
            if (room.get_tile(smoothed).solid() && !room.get_tile(goal).solid()) {
                ivec2 const t = room_view::get_tile_position(smoothed);
                s.pos = rect_collision(smoothed, goal, float(t.x) * 20.f - 3.f, float(t.y) * 20.f - 3.f, float(t.x) * 20.f + 23.f, float(t.y) * 20.f + 23.f);
                return;
            }
            s.pos = smoothed;
        }

        void align_and_connect(room_view const& room, daddy::rope_segment* seg, uint32_t n, std::vector<vec2> const& list, uint32_t count) {
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
            // the unclaimed segments between two claimed ones share their
            // Bezier's control points: worked out once per gap
            int32_t gi = -1, gj = -1;
            vec2 v{0.f, 0.f}, v2{0.f, 0.f};
            for (uint32_t idx = 0; idx < n; ++idx) {
                auto& s = seg[idx];
                if (s.bend_index > -1)
                    continue;
                s.last_pos = s.pos;
                s.pos += s.vel;
                s.vel *= 0.f; // airFricition is 0
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
                // (an unclaimed end moves within this loop: no sharing then)
                if (i != gi || j != gj || seg[i].bend_index < 0 || seg[j].bend_index < 0) {
                    gi = i;
                    gj = j;
                    v = seg[i].pos;
                    v2 = seg[j].pos;
                    float const dist = rw::distance(seg[i].pos, seg[j].pos);
                    if (seg[i].bend_index > 0)
                        v += rw::normalized(custom::dir_vec(list[size_t(seg[i].bend_index - 1)], v) + custom::dir_vec(seg[i].pos, seg[j].pos)) * dist * 0.33333334f;
                    if (seg[j].bend_index < int32_t(count) - 1)
                        v2 += rw::normalized(custom::dir_vec(list[size_t(seg[j].bend_index + 1)], v2) + custom::dir_vec(seg[j].pos, seg[i].pos)) * dist * 0.33333334f;
                }
                float const t = mathf::inverse_lerp(float(i), float(j), float(idx));
                tube_move_segment(room, s, rw::lerp(seg[i].pos, seg[j].pos, t), custom::bezier(seg[i].pos, v, seg[j].pos, v2, t));
            }
            for (uint32_t k = 0; k < n; ++k)
                seg[k].bend_index = -1;
        }

        // DaddyLegGraphic.Update: the rope positions of the tentacle's chunks
        void leg_update(sys::game_state& state, dcon::creature_id d, dcon::creature_part_id t, room_view const& room, rw::rng& r) {
            auto& w = state.world;
            auto const tent_part = daddy::tentacle_part(state, d, w.creature_part_get_index(t));
            auto const tent = daddy::tentacle_data(w, tent_part);
            auto const& tt = w.daddy_tentacle_get_t(tent);
            static thread_local std::vector<vec2> list;
            uint32_t count = 0;
            auto const add = [&](vec2 p) {
                if (list.size() <= count)
                    list.push_back(p);
                else
                    list[count] = p;
                ++count;
            };
            add(w.body_chunk_get_pos(chunk(state, d, tt.connected_chunk)));
            float const shake = mathf::inverse_lerp(4.f, 14.f, float(w.creature_part_get_stun(tent_part))) * 4.f;
            for (auto const& c : w.daddy_tentacle_get_chunks(tent)) {
                uint32_t const total = 2u + c.rope.bend_count;
                for (uint32_t j = 1; j < total; ++j) {
                    vec2 const p = j - 1 >= c.rope.bend_count ? c.rope.b : c.rope.bends[j - 1].pos;
                    // (a stunned tentacle's shake: the rolls are skipped while it is still)
                    if (shake > 0.f) {
                        vec2 const rnv = custom::rnv(r);
                        add(p + rnv * shake * r.value());
                    } else {
                        add(p);
                    }
                }
            }
            auto segs = w.daddy_tube_get_segments(daddy::tube_data(w, t));
            align_and_connect(room, segs.begin(), segs.size(), list, count);
        }

        // DaddyDangleTube.Connection.Pos
        vec2 connection_pos(sys::game_state const& state, dcon::creature_id d, daddy::dangle_connection const& c, float alpha) {
            if (c.chunk >= 0) {
                auto const k = chunk(state, d, uint32_t(c.chunk));
                return rw::lerp(state.world.body_chunk_get_last_pos(k), state.world.body_chunk_get_pos(k), alpha);
            }
            return tube_pos(state, daddy::tube_of(state, d, uint32_t(c.leg)), c.leg_pos, alpha);
        }

        // DaddyDangleTube.Update
        void dangler_update(sys::game_state& state, dcon::creature_id d, dcon::creature_part_id t, room_view const& room) {
            auto& w = state.world;
            auto parts = w.daddy_tube_get_parts(daddy::tube_data(w, t));
            uint32_t const n = parts.size();
            float const g = room.gravity;
            for (uint32_t i = 0; i < n; ++i) {
                parts.at(i).vel.y -= 0.5f * g;
                generic_update(state, parts.at(i), room, 0.99f, 0.5f);
                if (i > 1) {
                    parts.at(i - 2).vel += custom::dir_vec(parts.at(i).pos, parts.at(i - 2).pos);
                    parts.at(i).vel += custom::dir_vec(parts.at(i - 2).pos, parts.at(i).pos);
                }
                if (i + 2 < n) {
                    parts.at(i + 2).vel += custom::dir_vec(parts.at(i).pos, parts.at(i + 2).pos);
                    parts.at(i).vel += custom::dir_vec(parts.at(i + 2).pos, parts.at(i).pos);
                }
            }
            vec2 const a = connection_pos(state, d, w.creature_part_get_payload(t).daddy_tube.a_con, 1.f);
            vec2 const b = connection_pos(state, d, w.creature_part_get_payload(t).daddy_tube.b_con, 1.f);
            float const num = mathf::max(10.f, rw::distance(a, b) / float(n));
            parts.at(0).pos = a;
            for (uint32_t j = 1; j < n; ++j) {
                auto& p = parts.at(j);
                auto& q = parts.at(j - 1);
                vec2 const dir = custom::dir_vec(p.pos, q.pos);
                float const num2 = rw::distance(p.pos, q.pos);
                p.pos += dir * (num2 - num) * 0.5f;
                p.vel += dir * (num2 - num) * 0.5f;
                q.pos -= dir * (num2 - num) * 0.5f;
                q.vel -= dir * (num2 - num) * 0.5f;
            }
            parts.at(n - 1).pos = b;
            for (int32_t k = int32_t(n) - 2; k >= 0; --k) {
                auto& p = parts.at(uint32_t(k));
                auto& q = parts.at(uint32_t(k + 1));
                vec2 const dir = custom::dir_vec(p.pos, q.pos);
                float const num4 = rw::distance(p.pos, q.pos);
                p.pos += dir * (num4 - num) * 0.5f;
                p.vel += dir * (num4 - num) * 0.5f;
                q.pos -= dir * (num4 - num) * 0.5f;
                q.vel -= dir * (num4 - num) * 0.5f;
            }
            parts.at(0).pos = a;
            parts.at(n - 1).pos = b;
            auto segs = w.daddy_tube_get_segments(daddy::tube_data(w, t));
            for (uint32_t l = 0; l < segs.size() && l < n; ++l) {
                segs.at(l).last_pos = segs.at(l).pos;
                segs.at(l).pos = parts.at(l).pos;
            }
        }

        // DaddyDeadLeg.Update
        void dead_leg_update(sys::game_state& state, dcon::creature_id d, dcon::creature_part_id t, room_view const& room, vec2 push_from) {
            auto& w = state.world;
            auto parts = w.daddy_tube_get_parts(daddy::tube_data(w, t));
            uint32_t const n = parts.size();
            vec2 const con = w.body_chunk_get_pos(chunk(state, d, w.creature_part_get_payload(t).daddy_tube.chunk));
            float const con_rad = w.body_chunk_get_rad(chunk(state, d, w.creature_part_get_payload(t).daddy_tube.chunk));
            vec2 const sec = w.body_chunk_get_pos(chunk(state, d, w.creature_part_get_payload(t).daddy_tube.secondary_chunk));
            for (uint32_t i = 0; i < n; ++i) {
                auto& bp = parts.at(i);
                bp.vel += rw::lerp(custom::dir_vec(push_from, rw::lerp(con, sec, 0.3f)) * 3.f, vec2{0.f, -0.5f * room.gravity}, mathf::inverse_lerp(0.f, 4.f, float(i)));
                vec2 p = push_from;
                if (i == 1) {
                    p = con;
                } else if (i > 1) {
                    parts.at(i - 2).vel += custom::dir_vec(bp.pos, parts.at(i - 2).pos);
                    p = parts.at(i - 2).pos;
                }
                bp.vel += custom::dir_vec(p, bp.pos);
                generic_update(state, bp, room, 0.99f, 0.5f);
            }
            {
                auto& b0 = parts.at(0);
                vec2 const a = custom::dir_vec(b0.pos, con);
                float const num = rw::distance(b0.pos, con);
                b0.pos += a * (num - con_rad);
                b0.vel += a * (num - con_rad);
            }
            for (uint32_t j = 1; j < n; ++j) {
                auto& p = parts.at(j);
                auto& q = parts.at(j - 1);
                vec2 const a = custom::dir_vec(p.pos, q.pos);
                float const num = rw::distance(p.pos, q.pos);
                p.pos += a * (num - 10.f) * 0.5f;
                p.vel += a * (num - 10.f) * 0.5f;
                q.pos -= a * (num - 10.f) * 0.5f;
                q.vel -= a * (num - 10.f) * 0.5f;
            }
            auto segs = w.daddy_tube_get_segments(daddy::tube_data(w, t));
            segs.at(0).last_pos = segs.at(0).pos;
            segs.at(0).pos = con;
            for (uint32_t k = 1; k < segs.size() && k - 1 < n; ++k) {
                segs.at(k).last_pos = segs.at(k).pos;
                segs.at(k).pos = parts.at(k - 1).pos;
            }
        }

        // DaddyGraphics.Eye.FindNewLookObject
        void find_new_look_object(sys::game_state& state, dcon::creature_id d, uint32_t index, rw::rng& r) {
            auto& w = state.world;
            auto& g = w.daddy_data_get_graphics(creature::daddy_data(w, d));
            auto const& ai = w.daddy_data_get_ai(creature::daddy_data(w, d));
            uint32_t const n = daddy::chunk_count(state, d);
            auto& e = g.eyes[index];
            bool flag = false;
            if (ai.seen) {
                flag = true;
                r.range(0, 1);
                for (uint32_t k = 0; k < n && flag; ++k)
                    if (g.eyes[k].creature_seen == ai.seen)
                        flag = false;
                if (flag) {
                    e.creature_seen = ai.seen;
                    e.light = mathf::max(0.75f, e.light);
                }
            }
            if (daddy::size_class(state, d) && !flag && ai.noise.count > 0) {
                flag = true;
                auto const& s = ai.noise.sources[r.range(0, ai.noise.count)];
                for (uint32_t k = 0; k < n && flag; ++k)
                    if (g.eyes[k].sound_source == s.id)
                        flag = false;
                if (flag) {
                    e.sound_source = s.id;
                    e.light = mathf::max(0.5f, e.light);
                }
            }
        }

        // DaddyGraphics.Eye.Update
        void eye_update(sys::game_state& state, dcon::creature_id d, uint32_t index, rw::rng& r) {
            auto& w = state.world;
            auto& g = w.daddy_data_get_graphics(creature::daddy_data(w, d));
            auto& e = g.eyes[index];
            auto const& z = w.daddy_data_get_core(creature::daddy_data(w, d));
            auto const& ai = w.daddy_data_get_ai(creature::daddy_data(w, d));
            bool const dead = w.creature_get_dead(d) != 0;
            float const deaf = z.deaf == 0 ? 0.f : mathf::pow(mathf::inverse_lerp(0.f, 120.f, float(z.deaf)), 0.2f);
            e.last_dir = e.dir;
            e.last_closed = e.closed;
            float const delay = float(std::max(1, e.eyes_closed_delay));
            e.closed = mathf::max(mathf::lerp(e.closed, mathf::inverse_lerp(0.f, delay, float(z.eyes_closed)), 1.f / delay), deaf);
            if (z.eyes_closed == 0)
                e.eyes_closed_delay = r.range(1, 20);
            vec2 look{0.f, 0.f};
            if (e.sound_source != 0) {
                auto const* s = daddy::detail::source(ai, e.sound_source);
                if (s != nullptr)
                    look = s->pos;
                if (s == nullptr || s->slated_for_deletion)
                    e.sound_source = 0;
            } else if (bool(e.creature_seen)) {
                if (creature::live(state, e.creature_seen) && ai.seen == e.creature_seen) {
                    if (ai.sees)
                        look = creature::main_pos(state, e.creature_seen);
                    else
                        look = room_view::middle_of_tile(room_view::get_tile_position(ai.last_seen_pos));
                } else {
                    e.creature_seen = dcon::creature_id{};
                }
            }
            float num = 0.f;
            if (look.x != 0.f && look.y != 0.f) {
                auto const k = chunk(state, d, index);
                vec2 const at = w.body_chunk_get_pos(k);
                e.dir = rw::slerp(e.dir, custom::dir_vec(at, look) * mathf::inverse_lerp(0.f, 200.f, rw::distance(at, look)), 0.3f);
                num = e.light * mathf::inverse_lerp(0.f, 1.f, rw::distance(e.last_dir, e.dir));
                e.light = mathf::max(dead ? 0.f : 0.2f, e.light - 0.05f);
            } else {
                e.dir *= 0.9f;
                e.light = mathf::max(dead ? 0.f : 0.1f, e.light - 0.05f);
                find_new_look_object(state, d, index, r);
            }
            e.flash = mathf::max(0.f, e.flash - 0.16666667f);
            if (r.value() < num)
                e.get_to_focus = mathf::max(e.get_to_focus, r.value());
            else if (r.value() < 0.014285714f)
                e.get_to_focus = 0.f;
            e.last_focus = e.focus;
            if (e.focus < e.get_to_focus)
                e.focus = mathf::min(e.focus + 0.05f, e.get_to_focus);
            else
                e.focus = mathf::max(e.focus - 0.05f, e.get_to_focus);
        }

        // TailSegment.Update (connectedSegment or connectedPoint)
        void tail_segment_update(player::body_part& t, player::body_part* prev, std::optional<vec2> point, float connection_rad, float affect_previous, room_view const& room) {
            t.last_pos = t.pos;
            t.pos += t.vel;
            t.stretched = 1.f;
            if (prev != nullptr) {
                if (!custom::dist_less(t.pos, prev->pos, connection_rad)) {
                    float const num = rw::distance(t.pos, prev->pos);
                    vec2 const a = custom::dir_vec(t.pos, prev->pos);
                    t.pos -= a * (connection_rad - num) * (1.f - affect_previous);
                    t.vel -= a * (connection_rad - num) * (1.f - affect_previous);
                    prev->pos += a * (connection_rad - num) * affect_previous;
                    prev->vel += a * (connection_rad - num) * affect_previous;
                    t.stretched = mathf::clamp((connection_rad / (num * 0.5f) + 2.f) / 3.f, 0.2f, 1.f);
                }
            } else if (point && !custom::dist_less(t.pos, *point, connection_rad)) {
                float const num = rw::distance(t.pos, *point);
                vec2 const a = custom::dir_vec(t.pos, *point);
                t.pos -= a * (connection_rad - num);
                t.vel -= a * (connection_rad - num);
                t.stretched = mathf::clamp((connection_rad / (num * 0.5f) + 2.f) / 3.f, 0.2f, 1.f);
            }
            body_parts::push_out_of_terrain(t, room, 0.85f);
        }

        constexpr float dummy_tail_rad[4] = {6.f, 4.f, 2.5f, 1.f};
        constexpr float dummy_tail_connection[4] = {4.f, 7.f, 7.f, 7.f};
        constexpr float dummy_tail_affect[4] = {1.f, 0.5f, 0.5f, 0.5f};

        // DaddyGraphics.HunterDummy.Update
        void dummy_update(sys::game_state& state, dcon::creature_id d, room_view const& room) {
            auto& w = state.world;
            auto& m = w.daddy_data_get_graphics(creature::daddy_data(w, d)).dummy;
            vec2 const c0 = w.body_chunk_get_pos(chunk(state, d, 0));
            vec2 const c1 = w.body_chunk_get_pos(chunk(state, d, 1));
            vec2 const v0 = w.body_chunk_get_vel(chunk(state, d, 0));
            vec2 const v1 = w.body_chunk_get_vel(chunk(state, d, 1));
            float const sub = physics::submersion(state, room, chunk(state, d, 0));
            m.last_breath = m.breath;
            m.breath += 0.0125f;
            for (int i = 0; i < 2; ++i)
                m.draw_pos[i][1] = m.draw_pos[i][0];
            m.draw_pos[0][0] = c0;
            m.draw_pos[1][0] = c1;
            float num = 0.f;
            vec2 v = c0, p = c1;
            float num2 = 28.f;
            for (int j = 0; j < 4; ++j) {
                auto& t = m.tail[j];
                tail_segment_update(t, j == 0 ? nullptr : &m.tail[j - 1], j == 0 ? std::optional<vec2>{m.draw_pos[1][0]} : std::nullopt,
                    dummy_tail_connection[j], dummy_tail_affect[j], room);
                t.vel *= mathf::lerp(0.75f, 0.95f, num * (1.f - sub));
                t.vel.y -= mathf::lerp(0.1f, 0.5f, num) * (1.f - sub) * room.gravity;
                num = (num * 10.f + 1.f) / 11.f;
                if (!custom::dist_less(t.pos, c1, 9.f * float(j + 1)))
                    t.pos = c1 + custom::dir_vec(c1, t.pos) * 9.f * float(j + 1);
                t.vel += custom::dir_vec(v, t.pos) * num2 / rw::distance(v, t.pos);
                num2 *= 0.5f;
                v = p;
                p = t.pos;
            }
            vec2 const b = custom::dir_vec(m.draw_pos[1][0], m.draw_pos[0][0]) * 3.f;
            body_parts::generic_update(m.head, room, 0.99f, 0.8f);
            body_parts::connect_to_point(m.head, rw::lerp(m.draw_pos[0][0], m.draw_pos[1][0], 0.2f) + b, 3.f, false, 0.2f, v0, 0.7f, 0.1f);
            body_parts::generic_update(m.legs, room, 0.99f, 0.8f);
            body_parts::connect_to_point(m.legs, c1 + vec2{m.legs_direction.x * 8.f, -2.f}, 4.f, false, 0.25f, vec2{v1.x, -10.f}, 0.5f, 0.1f);
            m.legs_direction += v1 * 0.01f;
            m.legs_direction.y -= 0.05f;
            m.legs_direction = rw::normalized(m.legs_direction);
        }

        // DaddyTubeGraphic.OnTubeEffectColorFac per kind
        float tube_effect_fac(sys::game_state const& state, dcon::creature_id d, dcon::creature_part_id t, float f) {
            auto const& w = state.world;
            switch (w.creature_part_get_payload(t).daddy_tube.kind) {
            case tube_kind::leg:
                return mathf::pow(f, 1.5f) * 0.4f;
            case tube_kind::dead_leg:
                return mathf::pow(f, 1.5f) * 0.4f * (1.f - w.creature_part_get_payload(t).daddy_tube.deadness);
            case tube_kind::dangler: {
                auto const ca = w.creature_part_get_payload(t).daddy_tube.a_con, cb = w.creature_part_get_payload(t).daddy_tube.b_con;
                float const a = ca.chunk < 0 ? mathf::pow(ca.leg_pos, 1.5f) * 0.4f : 0.f;
                float const b = cb.chunk < 0 ? mathf::pow(cb.leg_pos, 1.5f) * 0.4f : 0.f;
                return mathf::lerp(a, b, f);
            }
            }
            (void)d;
            return 0.f;
        }

        // a jagged disc (the JaggedCircle shader over Futile_White): the rim's
        // dents rolled once per eye, turned with the sprite
        void jagged_circle(std::vector<sprites::draw_cmd>& out, vec2 pos, float rad, float rotation, uint32_t seed, vec3 color) {
            constexpr int segs = 18;
            float radii[segs];
            uint32_t h = seed | 1u;
            for (int k = 0; k < segs; ++k) {
                h ^= h << 13;
                h ^= h >> 17;
                h ^= h << 5;
                radii[k] = rad * (0.84f + 0.16f * float(h & 0xFFFF) / 65535.f);
            }
            for (int k = 0; k < segs; ++k) {
                vec2 const a = custom::deg_to_vec(rotation + float(k) * 360.f / segs) * radii[k];
                vec2 const b = custom::deg_to_vec(rotation + float(k + 1) * 360.f / segs) * radii[(k + 1) % segs];
                sprites::draw_cmd c;
                c.is_triangle = true;
                c.tri[0] = pos;
                c.tri[1] = pos + a;
                c.tri[2] = pos + b;
                c.color = color;
                out.push_back(c);
            }
        }

        // the eye slits' Random.value flicker (a render-time roll)
        float flicker(uint32_t& s) {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            return float(s >> 8) * (1.f / 16777215.f);
        }

        dcon::creature_part_id make_tube(sys::game_state& state, dcon::creature_id d, tube_kind k, uint8_t index, int32_t segments) {
            auto& w = state.world;
            auto const t = creature::add_part(state, d, creature::part_kind::daddy_tube, index);
            auto& pl = w.creature_part_get_payload(t).daddy_tube;
            pl = creature::daddy_tube_payload{};
            pl.kind = k;
            pl.data = uint16_t(w.create_daddy_tube().index());
            w.daddy_tube_get_segments(daddy::tube_data(w, t)).resize(uint32_t(std::max(2, segments)));
            return t;
        }

        // DaddyLegGraphic / DaddyDeadLeg bumps
        void add_bumps(sys::game_state& state, dcon::creature_part_id t, int32_t num, float deadness, bool dead_leg, float size, rw::unity_random& r) {
            auto bumps = state.world.daddy_tube_get_bumps(daddy::tube_data(state.world, t));
            int32_t const count = num / 2 + r.range(5, 8);
            for (int32_t i = 0; i < count; ++i) {
                float num2 = mathf::pow(r.value(), dead_leg ? 0.5f : 0.3f);
                if (i == 0)
                    num2 = 1.f;
                daddy::bump b;
                b.pos.x = mathf::lerp(-1.f, 1.f, r.value()) * 3.f * num2;
                b.pos.y = mathf::lerp(mathf::inverse_lerp(0.f, float(num), float(num - 20)), 1.f, num2);
                float const s0 = r.value();
                float const s1 = r.value();
                b.size = mathf::lerp(s0, num2, s1) * size;
                float const roll = r.value() * (dead_leg ? 1.f + deadness : 1.f);
                b.eye_size = roll < mathf::lerp(0.f, 0.6f, num2) ? mathf::lerp(0.2f, 0.8f, mathf::pow(r.value(), mathf::lerp(1.5f, 0.5f, num2))) : 0.f;
                bumps.push_back(b);
            }
        }

    }

    vec3 body_color(sys::game_state const& state, dcon::creature_id d, palette const& pal) {
        return daddy::hunter(state, d) ? hunter_color() : pal.black;
    }

    vec3 tube_color(sys::game_state const& state, dcon::creature_id d, dcon::creature_part_id t, float float_pos, palette const& pal) {
        return mix(body_color(state, d, pal), state.world.creature_get_payload(d).daddy.effect_color, tube_effect_fac(state, d, t, float_pos));
    }

    // DaddyTubeGraphic.OneDimensionalTubePos
    vec2 tube_pos(sys::game_state const& state, dcon::creature_part_id t, float float_pos, float alpha) {
        auto const segs = state.world.daddy_tube_get_segments(daddy::tube_data(state.world, t));
        int32_t const n = int32_t(segs.size());
        int32_t const num = custom::int_clamp(int32_t(std::floor(float_pos * float(n - 1))), 0, n - 1);
        int32_t const num2 = custom::int_clamp(num + 1, 0, n - 1);
        float const f = mathf::inverse_lerp(float(num), float(num2), float_pos * float(n - 1));
        auto const& a = segs.at(uint32_t(num));
        auto const& b = segs.at(uint32_t(num2));
        return rw::lerp(rw::lerp(a.last_pos, b.last_pos, f), rw::lerp(a.pos, b.pos, f), alpha);
    }

    // DaddyTubeGraphic.OnTubePos
    vec2 on_tube_pos(sys::game_state const& state, dcon::creature_part_id t, vec2 pos, float alpha) {
        float const step = 1.f / float(state.world.daddy_tube_get_segments(daddy::tube_data(state.world, t)).size());
        vec2 const p = tube_pos(state, t, pos.y - step, alpha);
        vec2 const p2 = tube_pos(state, t, pos.y + step, alpha);
        return tube_pos(state, t, pos.y, alpha) + custom::perpendicular_vector(custom::dir_vec(p, p2)) * pos.x;
    }

    void create(sys::game_state& state, dcon::creature_id d) {
        auto& w = state.world;
        rw::unity_random r{w.creature_get_payload(d).daddy.graphics_seed};
        bool const sc = daddy::size_class(state, d);
        bool const hd = daddy::hunter(state, d);
        int32_t const n = int32_t(daddy::chunk_count(state, d));
        int32_t const nt = w.creature_get_payload(d).daddy.tentacle_count;
        float const bs = bump_size(state, d);
        vec2 const main = w.body_chunk_get_pos(chunk(state, d, 0));
        // DaddyLegGraphic per tentacle
        for (int32_t i = 0; i < nt; ++i) {
            int32_t const num = int32_t(w.daddy_tentacle_get_t(daddy::tentacle_of(state, d, uint32_t(i))).ideal_length / 10.f);
            auto const t = make_tube(state, d, tube_kind::leg, uint8_t(i), num);
            add_bumps(state, t, num, 0.f, false, bs, r);
        }
        int32_t const num3 = sc ? r.range(5, 15) : r.range(3, 8);
        int32_t dead_count = 0;
        if (!hd) {
            int32_t const inner = r.range(sc ? 2 : 1, num3);
            dead_count = r.range(sc ? 2 : 1, inner);
        }
        dead_count = std::min(dead_count, int32_t(daddy::max_tubes));
        // DaddyDeadLeg: hanging off a chunk, pushed out from the middle of
        // the body towards a second one
        std::vector<int32_t> list;
        for (int32_t j = 0; j < dead_count; ++j) {
            if (list.empty())
                for (int32_t k = 0; k < n; ++k)
                    list.push_back(k);
            int32_t const inner = r.range(8, sc ? 17 : 8);
            int32_t const pn = r.range(4, inner);
            int32_t const con = list[size_t(r.range(0, int32_t(list.size())))];
            int32_t const sec = r.range(0, n);
            auto const t = make_tube(state, d, tube_kind::dead_leg, uint8_t(j), pn + 1);
            w.creature_part_get_payload(t).daddy_tube.chunk = (uint8_t(con));
            w.creature_part_get_payload(t).daddy_tube.secondary_chunk = (uint8_t(sec));
            vec2 const cp = w.body_chunk_get_pos(chunk(state, d, uint32_t(con)));
            auto bps = w.daddy_tube_get_parts(daddy::tube_data(w, t));
            for (int32_t i = 0; i < pn; ++i) {
                player::body_part bp;
                bp.pos = bp.last_pos = cp;
                bp.rad = 3.f;
                bps.push_back(bp);
            }
            float const deadness = r.value();
            w.creature_part_get_payload(t).daddy_tube.deadness = (deadness);
            add_bumps(state, t, pn, deadness, true, bs, r);
            list.erase(std::find(list.begin(), list.end(), con));
        }
        // DaddyDangleTube: slung between chunks and points along the legs.
        // Vanilla shares the Connection objects between neighbouring
        // danglers and sets their legPos as it goes, so the final values are
        // only known at the end
        std::vector<daddy::dangle_connection> cons;
        auto const gen = [&](bool other_is_chunk) {
            daddy::dangle_connection c;
            if (other_is_chunk)
                c.leg = int8_t(r.range(0, nt));
            else
                c.chunk = int8_t(r.range(0, n));
            cons.push_back(c);
            return cons.size() - 1;
        };
        auto const leg_segments = [&](int32_t leg) { return float(w.daddy_tube_get_segments(daddy::tube_data(w, daddy::tube_of(state, d, uint32_t(leg)))).size()); };
        size_t a = gen(r.value() < 0.5f);
        size_t b = gen(cons[a].chunk >= 0);
        int32_t const dangler_count = std::min(num3 - dead_count, int32_t(daddy::max_tubes) - dead_count);
        bool flag = r.value() < 0.5f;
        std::vector<std::pair<size_t, size_t>> pairs;
        for (int32_t l = 0; l < dangler_count; ++l) {
            if (r.value() < 0.2f) {
                a = gen(r.value() < 0.5f);
                b = gen(cons[a].chunk >= 0);
            } else {
                if (flag)
                    a = gen(cons[b].chunk >= 0 || r.value() < 0.2f);
                else
                    b = gen(cons[a].chunk >= 0 || r.value() < 0.2f);
                flag = !flag;
            }
            if (cons[a].leg >= 0)
                cons[a].leg_pos = mathf::lerp(0.1f, mathf::min(0.7f, 20.f / leg_segments(cons[a].leg)), mathf::pow(r.value(), 1.2f));
            if (cons[b].leg >= 0)
                cons[b].leg_pos = mathf::lerp(0.1f, mathf::min(0.7f, 20.f / leg_segments(cons[b].leg)), mathf::pow(r.value(), 1.2f));
            int32_t const pn = r.range(2, sc ? 25 : 12);
            auto const t = make_tube(state, d, tube_kind::dangler, uint8_t(l), pn);
            auto bps = w.daddy_tube_get_parts(daddy::tube_data(w, t));
            for (int32_t i = 0; i < pn; ++i) {
                player::body_part bp;
                bp.pos = bp.last_pos = main;
                bp.rad = 3.f;
                bps.push_back(bp);
            }
            auto bumps = w.daddy_tube_get_bumps(daddy::tube_data(w, t));
            int32_t const nb = r.range(5, 18);
            for (int32_t j = 0; j < nb; ++j) {
                daddy::bump bu;
                bu.pos.x = mathf::lerp(-1.f, 1.f, r.value()) * 2.f;
                bu.pos.y = r.value();
                bu.size = mathf::pow(r.value(), 1.4f) * bs;
                bumps.push_back(bu);
            }
            pairs.push_back({a, b});
        }
        w.creature_get_payload(d).daddy.dead_leg_count = uint8_t(dead_count);
        w.creature_get_payload(d).daddy.dangler_count = uint8_t(dangler_count);
        for (int32_t l = 0; l < dangler_count; ++l) {
            auto const t = daddy::tube_of(state, d, uint32_t(nt + dead_count + l));
            w.creature_part_get_payload(t).daddy_tube.a_con = (cons[pairs[size_t(l)].first]);
            w.creature_part_get_payload(t).daddy_tube.b_con = (cons[pairs[size_t(l)].second]);
        }
        // DaddyGraphics.Eye per chunk
        daddy::graphics_state g;
        for (int32_t m = 0; m < n && m < int32_t(daddy::max_chunks); ++m) {
            auto& e = g.eyes[m];
            e.rad = w.body_chunk_get_rad(chunk(state, d, uint32_t(m)));
            e.rotats[0] = r.value() * 360.f;
            e.rotats[1] = r.value();
            e.jag_seed = uint32_t(w.creature_get_payload(d).daddy.graphics_seed) * 2654435761u + uint32_t(m) * 40503u;
        }
        w.daddy_data_set_graphics(creature::daddy_data(w, d), g);
        w.creature_set_graphics_random(d, rw::rng{uint32_t(w.creature_get_payload(d).daddy.graphics_seed) | 1u});
        reset(state, d);
    }

    void reset(sys::game_state& state, dcon::creature_id d) {
        auto& w = state.world;
        vec2 const main = w.body_chunk_get_pos(chunk(state, d, 0));
        for (uint32_t i = 0; i < daddy::tube_count(state, d); ++i) {
            auto const t = daddy::tube_of(state, d, i);
            for (auto& s : w.daddy_tube_get_segments(daddy::tube_data(w, t))) {
                s.pos = s.last_pos = main;
                s.vel = vec2{0.f, 0.f};
            }
            // GraphicsModule.Reset: the body parts to the first chunk
            for (auto& bp : w.daddy_tube_get_parts(daddy::tube_data(w, t))) {
                bp.pos = bp.last_pos = main;
                bp.vel = vec2{0.f, 0.f};
            }
        }
        if (daddy::hunter(state, d)) {
            // HunterDummy.Reset
            auto& m = w.daddy_data_get_graphics(creature::daddy_data(w, d)).dummy;
            vec2 const c1 = w.body_chunk_get_pos(chunk(state, d, 1));
            for (int i = 0; i < 2; ++i) {
                vec2 const p = w.body_chunk_get_pos(chunk(state, d, uint32_t(i)));
                m.draw_pos[i][0] = m.draw_pos[i][1] = p;
            }
            for (int j = 0; j < 4; ++j) {
                m.tail[j] = player::body_part{};
                m.tail[j].pos = m.tail[j].last_pos = c1;
                m.tail[j].rad = dummy_tail_rad[j];
            }
            m.head = player::body_part{};
            m.head.pos = m.head.last_pos = main;
            m.head.rad = 4.f;
            m.legs = player::body_part{};
            m.legs.pos = m.legs.last_pos = c1;
            m.legs.rad = 1.f;
        }
    }

    void update(sys::game_state& state, dcon::creature_id d, room_view const& room) {
        auto& w = state.world;
        auto& r = w.creature_get_graphics_random(d);
        uint32_t const nt = w.creature_get_payload(d).daddy.tentacle_count;
        for (uint32_t i = 0; i < daddy::tube_count(state, d); ++i) {
            auto const t = daddy::tube_of(state, d, i);
            if (i < nt)
                leg_update(state, d, t, room, r);
        }
        // (TubeDeadLegPushOutFromPos: the middle of the body)
        vec2 const middle = daddy::middle_of_body(state, d);
        for (uint32_t i = nt; i < daddy::tube_count(state, d); ++i) {
            auto const t = daddy::tube_of(state, d, i);
            if (w.creature_part_get_payload(t).daddy_tube.kind == tube_kind::dead_leg)
                dead_leg_update(state, d, t, room, middle);
        }
        for (uint32_t i = nt; i < daddy::tube_count(state, d); ++i) {
            auto const t = daddy::tube_of(state, d, i);
            if (w.creature_part_get_payload(t).daddy_tube.kind == tube_kind::dangler)
                dangler_update(state, d, t, room);
        }
        uint32_t const n = std::min(daddy::chunk_count(state, d), daddy::max_chunks);
        for (uint32_t l = 0; l < n; ++l)
            eye_update(state, d, l, r);
        if (daddy::hunter(state, d))
            dummy_update(state, d, room);
        auto& g = w.daddy_data_get_graphics(creature::daddy_data(w, d));
        // digestLoop: Daddy_Digestion_LOOP for the big ones, Bro_ for the brother
        state.play_loop(sound::loop_key(sound::loop_owner::creature, uint32_t(d.index())),
            daddy::size_class(state, d) ? sound::sound_id::daddy_digestion_loop : sound::sound_id::bro_digestion_loop,
            room.id, middle, g.digest_loop_volume);
        g.digest_loop_volume = mathf::lerp(g.digest_loop_volume, mathf::pow(g.digesting, 0.8f), 0.2f);
        if (g.reaction_sound_delay > 0)
            --g.reaction_sound_delay;
        if (g.feel_something_reaction_delay > 0)
            --g.feel_something_reaction_delay;
        float most = 0.f;
        auto const& z = w.daddy_data_get_core(creature::daddy_data(w, d));
        for (uint32_t i = 0; i < z.eat_count; ++i)
            most = mathf::max(most, z.eats[i].progression);
        g.digesting = mathf::lerp(g.digesting, mathf::clamp(mathf::pow(most, 0.5f), 0.f, 1.f), 0.1f);
    }

    namespace {
        // the eye a reaction goes to: the one facing it, preferring free ones
        uint32_t pick_eye(sys::game_state const& state, dcon::creature_id d, vec2 at) {
            auto const& w = state.world;
            auto const& g = w.daddy_data_get_graphics(creature::daddy_data(w, d));
            vec2 const middle = daddy::middle_of_body(state, d);
            uint32_t const n = std::min(daddy::chunk_count(state, d), daddy::max_chunks);
            float best = std::numeric_limits<float>::lowest();
            uint32_t idx = 0;
            for (uint32_t i = 0; i < n; ++i) {
                float num3 = rw::dot(custom::dir_vec(middle, w.body_chunk_get_pos(chunk(state, d, i))), custom::dir_vec(middle, at));
                if (g.eyes[i].sound_source != 0)
                    num3 -= 1.f;
                if (bool(g.eyes[i].creature_seen))
                    num3 -= 2.f;
                if (num3 > best) {
                    best = num3;
                    idx = i;
                }
            }
            return idx;
        }
    }

    void feel_something(sys::game_state& state, dcon::creature_id d, dcon::creature_id felt, vec2 feel_pos) {
        auto& w = state.world;
        auto& g = w.daddy_data_get_graphics(creature::daddy_data(w, d));
        if (g.feel_something_reaction_delay > 0)
            return;
        g.feel_something_reaction_delay = w.creature_get_graphics_random(d).range(10, 40);
        // (Bro_React_To_Tentacle_Touch and Daddy_React_To_Tentacle_Touch have
        // no sound in sounds.txt)
        if (!daddy::size_class(state, d))
            return;
        auto& e = g.eyes[pick_eye(state, d, feel_pos)];
        // Eye.ReactToCreature
        e.sound_source = 0;
        e.creature_seen = felt;
        e.light = mathf::max(e.light, w.creature_get_graphics_random(d).value());
    }

    void react_to_noise(sys::game_state& state, dcon::creature_id d, uint16_t source, vec2 noise_pos, float strength) {
        auto& w = state.world;
        if (!daddy::size_class(state, d))
            return;
        auto& g = w.daddy_data_get_graphics(creature::daddy_data(w, d));
        auto const& ai = w.daddy_data_get_ai(creature::daddy_data(w, d));
        uint32_t const idx = pick_eye(state, d, noise_pos);
        auto& e = g.eyes[idx];
        // Eye.ReactToSound
        e.creature_seen = dcon::creature_id{};
        e.sound_source = source;
        e.light = 1.f;
        e.flash = 1.f;
        auto const* s = daddy::detail::source(ai, source);
        dcon::creature_id const rep = s != nullptr ? s->who : dcon::creature_id{};
        float num4 = 0.5f;
        if (daddy::behavior(w.creature_get_behavior(d)) == daddy::behavior::hunt && ai.seen)
            num4 = ai.seen == rep ? 1.f : 0.2f;
        else if (daddy::behavior(w.creature_get_behavior(d)) == daddy::behavior::examine_sound && ai.noise.sound_to_examine != 0)
            num4 = ai.noise.sound_to_examine == source ? 1.f : 0.2f;
        num4 = mathf::clamp01((num4 + mathf::inverse_lerp(150.f, 400.f, strength) + num4) / 3.f);
        if (rep && ai.sees)
            num4 *= 0.1f;
        if (num4 * 40.f > float(g.reaction_sound_delay)) {
            // (the DaddyBubbles and DaddyRipple it lets out aren't ported, nor
            // Daddy_React_To_Noise, which has no sound)
            w.creature_get_graphics_random(d).value();
            g.reaction_sound_delay = std::max(g.reaction_sound_delay, w.creature_get_graphics_random(d).range(10, 40));
        }
    }

    namespace {
        // DaddyTubeGraphic.DrawSprite + ApplyPalette: the tube's long mesh as
        // one strip, then its bumps and their eyes
        void tube_cmds(sys::game_state const& state, dcon::creature_id d, dcon::creature_part_id t, float alpha, palette const& pal,
            std::vector<sprites::draw_cmd>& out, std::vector<sprites::strip_vertex>& strip) {
            auto const& w = state.world;
            auto const segs = w.daddy_tube_get_segments(daddy::tube_data(w, t));
            uint32_t const ns = segs.size();
            if (ns < 2)
                return;
            auto const& tp = w.creature_part_get_payload(t).daddy_tube;
            vec3 const body = body_color(state, d, pal);
            vec3 const effect = w.creature_get_payload(d).daddy.effect_color;
            float const rad = daddy::size_class(state, d) ? 2.f : 1.7f;
            // OnTubeEffectColorFac with the kind's constants taken once:
            // pow(f, 1.5) * k along a leg, lerp(a, b, f) along a dangler
            float k = 0.4f, fa = 0.f, fb = 0.f;
            if (tp.kind == tube_kind::dead_leg)
                k = 0.4f * (1.f - tp.deadness);
            bool const dangler = tp.kind == tube_kind::dangler;
            if (dangler) {
                fa = tp.a_con.chunk < 0 ? mathf::pow(tp.a_con.leg_pos, 1.5f) * 0.4f : 0.f;
                fb = tp.b_con.chunk < 0 ? mathf::pow(tp.b_con.leg_pos, 1.5f) * 0.4f : 0.f;
            }
            auto const color = [&](float f) { return mix(body, effect, dangler ? mathf::lerp(fa, fb, f) : mathf::pow(f, 1.5f) * k); };
            auto const at = [&](uint32_t i) { return rw::lerp(segs.at(i).last_pos, segs.at(i).pos, alpha); };
            // TriangleMesh.MakeLongMesh: four vertices per segment
            uint32_t const nv = ns * 4;
            sprites::draw_cmd c;
            c.is_strip = true;
            c.strip_first = uint32_t(strip.size());
            c.strip_count = nv;
            strip.resize(strip.size() + nv);
            sprites::strip_vertex* sv = strip.data() + c.strip_first;
            float const step = 1.f / float(nv - 1);
            auto const put = [&](uint32_t i, vec2 p) {
                vec3 const col = color(mathf::inverse_lerp(0.3f, 1.f, float(i) * step));
                sv[i] = sprites::strip_vertex{p, rw::vec4{col.x, col.y, col.z, 1.f}};
            };
            vec2 v = at(0);
            v += custom::dir_vec(at(1), v) * 1.f;
            for (uint32_t i = 0; i < ns; ++i) {
                vec2 const v2 = at(i);
                vec2 const a = custom::perpendicular_vector(rw::normalized(v - v2)) * rad;
                put(i * 4, v - a);
                put(i * 4 + 1, v + a);
                put(i * 4 + 2, v2 - a);
                put(i * 4 + 3, v2 + a);
                v = v2;
            }
            out.push_back(c);
            // the bumps' places along the tube, worked out once for both passes
            auto const bumps = w.daddy_tube_get_bumps(daddy::tube_data(w, t));
            static thread_local std::vector<vec2> places;
            places.resize(bumps.size());
            for (uint32_t i = 0; i < bumps.size(); ++i)
                places[i] = on_tube_pos(state, t, bumps.at(i).pos, alpha);
            for (uint32_t i = 0; i < bumps.size(); ++i) {
                auto const& b = bumps.at(i);
                float const s = mathf::lerp(2.f, 6.f, b.size) / 10.f;
                sprites::add(out, sprites::sprite{"Circle20", places[i], 0.f, vec2{s, s}, vec2{0.5f, 0.5f}, color(b.pos.y)});
            }
            // the bumps' eyes: DaddyGraphics.TubeBumpColor
            vec3 eye = body;
            if (w.creature_get_payload(d).daddy.color_class != 0)
                eye = tp.kind == tube_kind::dead_leg ? effect * mathf::lerp(0.5f, 0.2f, tp.deadness) : effect;
            for (uint32_t i = 0; i < bumps.size(); ++i) {
                auto const& b = bumps.at(i);
                if (b.eye_size <= 0.f)
                    continue;
                float const s = mathf::lerp(2.f, 6.f, b.size) * b.eye_size / 10.f;
                sprites::add(out, sprites::sprite{"Circle20", places[i], 0.f, vec2{s, s}, vec2{0.5f, 0.5f}, eye});
            }
        }

        // Eye.BulgeVertex
        vec2 bulge(vec2 v, vec2 dir, float rad) {
            return rw::lerp(v, rw::clamp_magnitude(v + dir * rad, rad), rw::magnitude(dir));
        }

        // HunterDummy.DrawSprites + ApplyPalette
        void dummy_cmds(sys::game_state const& state, dcon::creature_id d, float alpha, palette const& pal, std::vector<sprites::draw_cmd>& out) {
            auto const& m = state.world.daddy_data_get_graphics(creature::daddy_data(state.world, d)).dummy;
            vec3 const color = hunter_color();
            float const num = 0.5f + 0.5f * std::sin(mathf::lerp(m.last_breath, m.breath, alpha) * mathf::pi * 2.f);
            vec2 const v = rw::lerp(m.draw_pos[0][1], m.draw_pos[0][0], alpha);
            vec2 const v2 = rw::lerp(m.draw_pos[1][1], m.draw_pos[1][0], alpha);
            vec2 const v3 = rw::lerp(m.head.last_pos, m.head.pos, alpha);
            float const num2 = mathf::inverse_lerp(0.3f, 0.5f, std::fabs(custom::dir_vec(v2, v).y));
            sprites::add(out, sprites::sprite{"BodyA", vec2{v.x, v.y + 0.5f * num * (1.f - num2)}, custom::aim(v2, v),
                vec2{1.f + mathf::lerp(-0.15f, 0.05f, num) * num2, 1.f}, vec2{0.5f, 0.7894737f}, color});
            vec2 const tail0 = rw::lerp(m.tail[0].last_pos, m.tail[0].pos, alpha);
            sprites::add(out, sprites::sprite{"HipsA", (v2 * 2.f + v) / 3.f, custom::aim(v, tail0), vec2{1.f + 0.05f * num - 0.05f, 1.f}, vec2{0.5f, 0.5f}, color});
            // the tail TriangleMesh: 15 vertices, the triangles of HunterDummy.InitiateSprites
            vec2 verts[15];
            vec2 v4 = (v2 * 3.f + v) / 4.f;
            float d2 = 6.f;
            for (int i = 0; i < 4; ++i) {
                vec2 const v5 = rw::lerp(m.tail[i].last_pos, m.tail[i].pos, alpha);
                vec2 const nrm = rw::normalized(v5 - v4);
                vec2 const a = custom::perpendicular_vector(nrm);
                float d3 = rw::distance(v5, v4) / 5.f;
                if (i == 0)
                    d3 = 0.f;
                verts[i * 4] = v4 - a * d2 * 0.8f + nrm * d3;
                verts[i * 4 + 1] = v4 + a * d2 * 0.8f + nrm * d3;
                float const sr = m.tail[i].rad * m.tail[i].stretched;
                if (i < 3) {
                    verts[i * 4 + 2] = v5 - a * sr * 0.8f - nrm * d3;
                    verts[i * 4 + 3] = v5 + a * sr * 0.8f - nrm * d3;
                } else {
                    verts[i * 4 + 2] = v5;
                }
                d2 = sr;
                v4 = v5;
            }
            static constexpr int tris[13][3] = {
                {0, 1, 2}, {1, 2, 3}, {4, 5, 6}, {5, 6, 7}, {8, 9, 10}, {9, 10, 11}, {12, 13, 14},
                {2, 3, 4}, {3, 4, 5}, {6, 7, 8}, {7, 8, 9}, {10, 11, 12}, {11, 12, 13},
            };
            for (auto const& tr : tris) {
                sprites::draw_cmd c;
                c.is_triangle = true;
                for (int k = 0; k < 3; ++k)
                    c.tri[k] = verts[tr[k]];
                c.color = color;
                out.push_back(c);
            }
            float const num3 = custom::aim(rw::lerp(v2, v, 0.5f), v3);
            sprites::add(out, sprites::sprite{"HeadA0", v3, num3, vec2{num3 >= 0.f ? 1.f : -1.f, 1.f}, vec2{0.5f, 0.5f}, color});
            vec2 const v7 = rw::lerp(m.legs.last_pos, m.legs.pos, alpha);
            sprites::add(out, sprites::sprite{"LegsAAir0", v7, custom::aim(m.legs_direction, vec2{0.f, 0.f}), vec2{1.f, 1.f}, vec2{0.5f, 0.25f}, color});
            sprites::add(out, sprites::sprite{"FaceDead", vec2{v3.x, v3.y - 2.f}, num3, vec2{1.f, 1.f}, vec2{0.5f, 0.5f}, pal.black});
        }
    }

    void build(sys::game_state const& state, dcon::creature_id d, float alpha, palette const& pal, std::vector<sprites::draw_cmd>& out, std::vector<sprites::strip_vertex>& strip) {
        auto const& w = state.world;
        auto const& g = w.daddy_data_get_graphics(creature::daddy_data(w, d));
        bool const hd = daddy::hunter(state, d);
        uint32_t const n = std::min(daddy::chunk_count(state, d), daddy::max_chunks);
        vec3 const body = body_color(state, d, pal);
        vec3 const eye_color = w.creature_get_payload(d).daddy.eye_color;
        float const bump = bump_size(state, d);
        // InitiateSprites' order: the legs, dead legs and danglers, then the eyes
        for (uint32_t i = 0; i < daddy::tube_count(state, d); ++i)
            tube_cmds(state, d, daddy::tube_of(state, d, i), alpha, pal, out, strip);
        // EyePushOutFromPos: the body's centre of mass between ticks
        vec2 middle{0.f, 0.f};
        float total = 0.f;
        for (uint32_t i = 0; i < n; ++i) {
            auto const k = chunk(state, d, i);
            middle += rw::lerp(w.body_chunk_get_last_pos(k), w.body_chunk_get_pos(k), alpha) * w.body_chunk_get_mass(k);
            total += w.body_chunk_get_mass(k);
        }
        middle /= total;
        uint32_t roll = state.tick * 2654435761u + uint32_t(d.index()) * 97u + 1u;
        for (uint32_t j = 0; j < n; ++j) {
            auto const& e = g.eyes[j];
            auto const k = chunk(state, d, j);
            vec2 const rnd = custom::deg_to_vec(flicker(roll) * 360.f);
            vec2 const pos = rw::lerp(w.body_chunk_get_last_pos(k), w.body_chunk_get_pos(k), alpha) + rnd * g.digesting * 4.f * flicker(roll);
            if (hd && j < 2)
                continue;
            float const rotation = custom::aim(pos, middle) + e.rotats[0];
            // the bulb: JaggedCircle over Futile_White, (rad * 1.1 + 2) / 8 of 16 pixels
            jagged_circle(out, pos, e.rad * 1.1f + 2.f, rotation, e.jag_seed, body);
            // Eye.RenderSlits
            float const num = mathf::pow(mathf::max(0.f, mathf::lerp(e.last_closed, e.closed, alpha)), 0.6f);
            float const num2 = bump * (1.f - num);
            vec2 const b = rw::lerp(e.last_dir, e.dir, alpha);
            float num3 = mathf::lerp(e.last_focus, e.focus, alpha) * mathf::pow(mathf::inverse_lerp(-1.f, 1.f, rw::dot(custom::dir_vec(middle, pos), rw::normalized(b))), 0.7f);
            num3 = mathf::max(num3, num);
            float num4 = mathf::inverse_lerp(0.f, mathf::lerp(30.f, 50.f, e.rotats[1]), rw::distance(middle, pos + custom::dir_vec(middle, pos) * e.rad)) * 0.9f;
            num4 = mathf::lerp(num4, 1.f, 0.5f * num3);
            vec2 const a = rw::lerp(custom::dir_vec(middle, pos) * num4, b, rw::magnitude(b) * 0.5f);
            vec3 render = mix(eye_color, vec3{1.f, 1.f, 1.f}, mathf::lerp(flicker(roll) * e.light, 1.f, num));
            if (num > 0.f)
                render = mix(render, body, num);
            render = mix(render, vec3{1.f, 1.f, 1.f}, e.flash);
            float const squint = mathf::lerp(1.f, 0.6f, num3);
            for (int i = 0; i < 2; ++i) {
                vec2 const v = custom::deg_to_vec(rotation + 90.f * float(i));
                vec2 const a2 = custom::perpendicular_vector(v);
                vec2 verts[10];
                verts[0] = pos + bulge(v * e.rad * 0.9f * squint, a, e.rad);
                verts[9] = pos + bulge(v * -e.rad * 0.9f * squint, a, e.rad);
                for (int jj = 1; jj < 5; ++jj)
                    for (int kk = 0; kk < 2; ++kk) {
                        float const dd = e.rad * (jj < 3 ? 0.7f : 0.25f) * (kk == 0 ? 1.f : -1.f) * squint;
                        int const num5 = kk == 0 ? jj : 9 - jj;
                        float const d2 = num2 * (jj < 3 ? 0.5f : 1.f) * (num5 % 2 == 0 ? 1.f : -1.f) * mathf::lerp(1.f, 2.5f, num3);
                        verts[num5] = pos + bulge(v * dd + a2 * d2, a, e.rad);
                    }
                sprites::long_mesh(out, strip, verts, 10, render);
            }
            if (hd) {
                float const s = 0.0625f * e.rad * 2.f;
                sprites::add(out, sprites::sprite{"CorruptGrad", pos, 0.f, vec2{s, s}, vec2{0.5f, 0.5f}, mix(render, vec3{0.f, 0.f, 0.f}, 0.4f)});
            }
        }
        if (hd)
            dummy_cmds(state, d, alpha, pal, out);
    }
}
