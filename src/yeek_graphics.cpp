#include <cmath>
#include <string_view>

#include "yeek_graphics.hpp"
#include "body_parts.hpp"
#include "creature.hpp"
#include "tracker.hpp"
#include "yeek.hpp"
#include "yeek_types.hpp"

namespace yeek_graphics {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using world::room_view;
    using sprites::sprite;
    using yeek::graphics_state;
    using yeek::leg_state;
    using yeek::tail_length;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        // YeekTail.maxDist; YeekLeg.maxLength
        constexpr float tail_max_dist = 5.f;
        constexpr float leg_max_length = 16.f;
        // BodyMeshLength, TailGraphSegments, TotGraphSegments; their long
        // meshes' vertex counts (pointy tips)
        constexpr uint32_t body_mesh_length = 5;
        constexpr uint32_t tot_graph_segments = body_mesh_length + tail_length;
        constexpr uint32_t body_vertices = (tot_graph_segments - 1) * 4 + 3;
        constexpr uint32_t tail_vertices = (tail_length - 1) * 4 + 3;
        constexpr std::string_view scale_a[7] = {"LizardScaleA0", "LizardScaleA1", "LizardScaleA2", "LizardScaleA3", "LizardScaleA4", "LizardScaleA5", "LizardScaleA6"};
        constexpr std::string_view scale_b[7] = {"LizardScaleB0", "LizardScaleB1", "LizardScaleB2", "LizardScaleB3", "LizardScaleB4", "LizardScaleB5", "LizardScaleB6"};

        graphics_state& gs(sys::game_state& state, dcon::creature_id c) {
            return state.world.yeek_data_get_graphics(creature::yeek_data(state.world, c));
        }
        graphics_state const& gs(sys::game_state const& state, dcon::creature_id c) {
            return state.world.yeek_data_get_graphics(creature::yeek_data(state.world, c));
        }
        creature::yeek_payload const& pl(sys::game_state const& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).yeek;
        }
        vec2 cpos(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_pos(yeek::chunk(state, c, i));
        }
        vec2 clast(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_last_pos(yeek::chunk(state, c, i));
        }
        vec3 lerp3(vec3 a, vec3 b, float t) {
            t = mathf::clamp01(t);
            return a + (b - a) * t;
        }
        vec4 v4(vec3 c) {
            return vec4{c.x, c.y, c.z, 1.f};
        }
        float frange(rw::rng& r, float lo, float hi) {
            return lo + (hi - lo) * r.value();
        }

        // SideShown, RightSideShown, LeftSideShown
        float side_shown(sys::game_state const& state, dcon::creature_id c, graphics_state const& g, float t) {
            float const x = rw::lerp(g.last_head_draw_dir, g.head_draw_dir, t).x;
            float num = mathf::inverse_lerp(0.2f, 0.9f, x);
            float num2 = mathf::inverse_lerp(-0.2f, -0.9f, x);
            if (state.world.creature_get_dead(c) || creature::held(state, c)) {
                num *= 0.15f;
                num2 *= 0.15f;
            }
            return num - num2;
        }
        float right_shown(sys::game_state const& state, dcon::creature_id c, graphics_state const& g, float t) {
            return mathf::clamp01(side_shown(state, c, g, t));
        }
        float left_shown(sys::game_state const& state, dcon::creature_id c, graphics_state const& g, float t) {
            return mathf::clamp01(-side_shown(state, c, g, t));
        }

        // YeekTail.Update
        void tail_update(sys::game_state& state, dcon::creature_id c, room_view const& room, player::body_part (&parts)[tail_length], rw::rng& rng) {
            auto const& w = state.world;
            auto const enter = w.creature_get_entering_shortcut(c);
            if (enter.x >= 0) {
                for (auto& p : parts) {
                    p.last_pos = p.pos;
                    p.pos = rw::lerp(p.pos, room_view::middle_of_tile(enter), 0.4f);
                }
                return;
            }
            if (!w.creature_get_dead(c))
                for (auto& p : parts)
                    p.vel += vec2{0.f, room.gravity * 0.5f};
            if (rng.value() < 0.4f) {
                vec2 const b = custom::rnv(rng) * 2.f;
                for (uint32_t k = 4; k < tail_length; ++k)
                    parts[k].vel += b;
            }
            for (uint32_t l = 0; l < tail_length; ++l) {
                auto& p = parts[l];
                float const t = float(l) / float(tail_length - 1);
                p.last_pos = p.pos;
                p.pos += p.vel;
                if (room.point_submerged(p.pos)) {
                    p.vel *= custom::lerp_map(rw::magnitude(p.vel), 1.f, 10.f, 1.f, 0.5f, mathf::lerp(1.4f, 0.4f, t));
                    p.vel.y += 0.05f;
                    p.vel += custom::rnv(rng) * 0.1f;
                } else {
                    float const d = rw::distance(p.pos, p.last_pos);
                    p.vel *= custom::lerp_map(d, 1.f, 6.f, 0.999f, 0.8f, mathf::lerp(1.5f, 0.5f, t));
                    p.vel.y -= room.gravity * custom::lerp_map(d, 1.f, 6.f, 0.6f, 0.f);
                    // (SharedPhysics' terrain collision: the room's tiles push it out)
                    if (l % 3 == 2 || l == tail_length - 1) {
                        p.rad = 1.f;
                        body_parts::push_out_of_terrain(p, room, 0.6f);
                    }
                }
            }
            vec2 const c0 = cpos(state, c, 0), c1 = cpos(state, c, 1);
            for (uint32_t m = 0; m < tail_length; ++m) {
                if (m == 0) {
                    parts[0].pos = c0;
                    parts[0].vel = vec2{0.f, 0.f};
                    continue;
                }
                vec2 n = rw::normalized(parts[m].pos - parts[m - 1].pos);
                float const num = rw::distance(parts[m].pos, parts[m - 1].pos);
                float const d = num > tail_max_dist ? 0.5f : 0.25f;
                parts[m].pos += n * (tail_max_dist - num) * d;
                parts[m].vel += n * (tail_max_dist - num) * d;
                parts[m - 1].pos -= n * (tail_max_dist - num) * d;
                parts[m - 1].vel -= n * (tail_max_dist - num) * d;
                if (m > 1) {
                    n = rw::normalized(parts[m].pos - parts[m - 2].pos);
                    parts[m].vel += n * 0.6f;
                    parts[m - 2].vel -= n * 0.6f;
                }
                if (m < 2) {
                    parts[m].pos = parts[m - 1].pos + custom::dir_vec(c1, c0) * tail_max_dist * 0.8f;
                    parts[m].vel = vec2{0.f, 0.f};
                }
            }
        }

        // the creature it looks at (CreatureLooker: the focusCreature, else
        // the most threatening one)
        creature::tracked const* look_creature(sys::game_state const& state, dcon::creature_id c) {
            auto const& ts = tracker::of(state, c);
            if (auto const f = yeek::focus(state, c))
                if (auto const* r = tracker::find(ts, f))
                    return r;
            return tracker::threat(ts);
        }

        // YeekGraphics.UpdateHeadAngle
        void update_head_angle(sys::game_state& state, dcon::creature_id c, graphics_state& g, rw::rng& rng) {
            auto const& x = pl(state, c);
            auto const& pers = yeek::personality_of(state, c);
            vec2 const m = cpos(state, c, 0);
            auto const* look = look_creature(state, c);
            if (look) {
                if (look->visual && creature::live(state, look->who))
                    g.fear_look_loc = custom::dir_vec(m, creature::main_pos(state, look->who));
                else if (tracker::chance_of_finding(*look, 10) * pers.nervous > 0.2f)
                    g.fear_look_loc = custom::dir_vec(m, room_view::middle_of_tile(tracker::best_guess(*look)));
                g.watch_intensity = mathf::lerp(g.watch_intensity, tracker::chance_of_finding(*look, 10), 0.05f + pers.nervous / 10.f);
            } else {
                g.watch_intensity = mathf::lerp(g.watch_intensity, 0.f, 0.15f - pers.nervous / 10.f);
            }
            if (std::fabs(state.world.body_chunk_get_vel(yeek::chunk(state, c, 0)).x) > 1.f)
                g.watch_intensity = mathf::lerp(g.watch_intensity, 0.f, 0.45f);
            vec2 vector{0.f, 0.f};
            float num = mathf::inverse_lerp(-90.f, 90.f, x.head_leading_counter);
            if (yeek::tunnel_mode(state, c)) {
                vector = x.climbing_orientation;
                num = 0.f;
            } else {
                vector = vec2{x.body_direction.x, -3.f * mathf::inverse_lerp(2.f, 0.f, std::fabs(x.body_direction.x))};
            }
            // (no goalFruit: never looking at one)
            vector = rw::lerp(vector, vec2{x.body_direction.x * (0.5f + num / 2.f) * 5.f, 0.f}, num);
            if (yeek::climbing_mode(state, c))
                vector = rw::lerp(vector, vec2{mathf::sign(cpos(state, c, 1).x - m.x), -1.f}, 0.5f);
            else if (num <= 0.75f && g.watch_intensity > 0.5f)
                vector = rw::lerp(vector, g.fear_look_loc, g.watch_intensity);
            vector += custom::rnv(rng) * rng.value() * pers.nervous * tracker::panic(tracker::of(state, c)) * 0.1f;
            float const mag = rw::magnitude(vector);
            if (mag < 1.f)
                vector = rw::lerp(vector, vec2{0.f, -0.6f}, 1.f - mag);
            else
                vector = rw::normalized(vector);
            g.last_head_draw_dir = g.head_draw_dir;
            g.head_draw_dir = rw::lerp(g.head_draw_dir, vector, 0.3f);
        }

        // YeekLeg.maxLegDist
        float max_leg_dist(sys::game_state const& state, dcon::creature_id c) {
            if (yeek::climbing_mode(state, c))
                return 11.f;
            if (yeek::tunnel_mode(state, c))
                return 8.f;
            return leg_max_length;
        }

        // YeekLeg.AtGoal
        bool at_goal(yeek::leg const& l) {
            return rw::distance(l.foot, l.goal) < 1.f;
        }

        // YeekLeg.Update (legs[0] is the right one)
        void leg_update(sys::game_state& state, dcon::creature_id c, graphics_state& g, uint32_t i, leg_state st) {
            auto& l = g.legs[i];
            auto const& other = g.legs[1 - i];
            auto const& x = pl(state, c);
            bool const right = i == 0;
            l.state = st;
            if (st != leg_state::jump)
                l.jump_timer = 0.f;
            l.last_root = l.root;
            l.last_foot = l.foot;
            vec2 const c0 = cpos(state, c, 0);
            float const ls = left_shown(state, c, g, 1.f), rs = right_shown(state, c, g, 1.f);
            vec2 a = right ? c0 + vec2{mathf::lerp(-1.4f, 0.f, ls) + mathf::lerp(0.f, 2.f, rs), 2.f}
                           : c0 + vec2{mathf::lerp(1.4f, 0.f, rs) + mathf::lerp(0.f, -2.f, ls), 2.f};
            l.root = a;
            float const md = max_leg_dist(state, c);
            if (st == leg_state::sit) {
                float const d = md / 2.f;
                if (right) {
                    a = c0 + vec2{2.f, -1.f};
                    l.goal = a + vec2{-1.f, -1.f} * d;
                } else {
                    a = c0 + vec2{-2.f, -1.f};
                    l.goal = a + vec2{1.f, -1.f} * d;
                }
            } else if (st == leg_state::climb || st == leg_state::tunnel) {
                float const num = st == leg_state::climb ? -2.f : 4.f;
                if (at_goal(other) && rw::distance(l.goal, c0) > rw::distance(other.goal, c0)) {
                    vec2 b = custom::perpendicular_vector(x.climbing_orientation) * (right ? num : -num);
                    l.goal = room_view::middle_of_tile(c0) + rw::normalized(x.climbing_orientation) * 2.f + b;
                }
            } else if (st == leg_state::jump) {
                l.jump_timer += 1.f;
                float const t = mathf::clamp01(l.jump_timer / 8.f);
                float const hv = state.world.body_chunk_get_vel(yeek::chunk(state, c, 1)).x;
                l.goal = a + vec2{hv / 2.f * mathf::lerp(2.f, 0.f, t), mathf::lerp(-5.f, -3.f, t)} * md;
            }
            if (rw::distance(l.root, l.goal) > md)
                l.goal = l.root + custom::dir_vec(l.root, l.goal) * md;
            l.foot = custom::move_towards(l.foot, l.goal, 8.f);
            if (rw::distance(l.root, l.foot) > md)
                l.foot = l.root + custom::dir_vec(l.root, l.foot) * md;
        }

        // YeekGraphics.UpdateLegs
        void update_legs(sys::game_state& state, dcon::creature_id c, room_view const& room, graphics_state& g) {
            leg_state st = leg_state::sit;
            if (!yeek::on_ground(state, c, room) || state.world.body_chunk_get_vel(yeek::chunk(state, c, 0)).y > 1.f)
                st = leg_state::jump;
            if (yeek::climbing_mode(state, c))
                st = leg_state::climb;
            if (yeek::tunnel_mode(state, c))
                st = leg_state::tunnel;
            if (!yeek::consious(state, c))
                st = leg_state::dangle;
            leg_update(state, c, g, 0, st);
            leg_update(state, c, g, 1, st);
        }

        // YeekLeg.Reset
        void leg_reset(yeek::leg& l, vec2 at) {
            l.root = at;
            l.goal = at;
        }

        // GraphSegmentPos
        vec2 graph_segment_pos(sys::game_state const& state, dcon::creature_id c, graphics_state const& g, uint32_t i, float t, bool secondary) {
            if (i < body_mesh_length) {
                vec2 const b0 = rw::lerp(clast(state, c, 0) + vec2{0.f, -4.f}, cpos(state, c, 0) + vec2{0.f, -4.f}, t);
                vec2 const h = rw::lerp(clast(state, c, 1), cpos(state, c, 1), t);
                vec2 const a = custom::dir_vec(b0, h);
                return rw::lerp(h + a * 2.f, b0 - a * 10.f, float(i) / float(body_mesh_length));
            }
            auto const& p = g.tails[secondary ? 1 : 0][i - body_mesh_length];
            return rw::lerp(p.last_pos, p.pos, t);
        }
        // GraphSegmentRad
        float graph_segment_rad(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            auto const& w = state.world;
            float const tuft = i >= tot_graph_segments - 3 ? 2.f : 1.f;
            constexpr float fat = 2.1f;
            float const f = float(i) / float(body_mesh_length);
            if (i < body_mesh_length) {
                float const r = mathf::lerp(w.body_chunk_get_rad(yeek::chunk(state, c, 1)) + 1.f, w.body_chunk_get_rad(yeek::chunk(state, c, 0)) + 1.f, f);
                constexpr float knee = 0.477f;
                float rad = mathf::lerp(r * 0.6f, r * fat, mathf::inverse_lerp(0.f, knee, mathf::clamp(f, 0.f, knee)));
                if (f > knee)
                    rad = mathf::lerp(r * fat, 1.2f * 1.35f * tuft, mathf::inverse_lerp(knee, 1.f, mathf::clamp(f, knee, 1.f)));
                return rad * tuft;
            }
            if (i < body_mesh_length + 2)
                return 1.6f * 1.35f * tuft;
            return 1.2f * 1.35f * tuft;
        }
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        auto& g = gs(state, c);
        auto& rng = w.creature_get_graphics_random(c);
        vec2 const at = cpos(state, c, 0);
        // YeekTail.Reset
        for (auto& tail : g.tails)
            for (auto& p : tail) {
                p.pos = at + custom::rnv(rng) * rng.value();
                p.last_pos = p.pos;
                p.vel = vec2{0.f, 0.f};
                p.rad = 1.f;
            }
        for (auto& f : g.feathers) {
            f.root = f.last_root = at;
            f.dir = f.last_dir = vec2{0.f, -1.f};
        }
        for (auto& l : g.legs) {
            leg_reset(l, at);
            l.foot = l.last_foot = l.root;
            l.last_root = l.root;
        }
        g.last_head_draw_dir = g.head_draw_dir;
        g.was_climbing = yeek::climbing_mode(state, c) ? 1 : 0;
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto& g = gs(state, c);
        auto& rng = w.creature_get_graphics_random(c);
        auto const& x = pl(state, c);
        auto const& lk = yeek::looks_of(state, c);
        auto const& pers = yeek::personality_of(state, c);
        // the YeekTails (Yeek.Update's)
        tail_update(state, c, room, g.tails[0], rng);
        tail_update(state, c, room, g.tails[1], rng);
        g.eye_shudder *= 0.98f;
        g.swim_counter += 0.1f;
        --g.blink_start_counter;
        g.last_flip = g.flip;
        vec2 const c0 = cpos(state, c, 0), c1 = cpos(state, c, 1);
        // YeekFeather.Update
        uint32_t const nf = lk.feathers < yeek::max_feathers ? lk.feathers : yeek::max_feathers;
        for (uint32_t i = 0; i < nf; ++i) {
            auto& f = g.feathers[i];
            float const t = float(i) / float(nf);
            vec2 const a = custom::dir_vec(c0, c1);
            f.last_root = f.root;
            f.root = rw::lerp(c1 + a * 4.f, c0 + a * -10.f, t);
            vec2 const v = custom::dir_vec(c1, c0);
            vec2 const back = rw::normalized(g.head_draw_dir * -1.f + v * -0.4f);
            f.last_dir = f.dir;
            // (featherIndex / MaxFeathersInGroup is an integer division: 1
            // for the first only)
            int32_t const index = int32_t(nf - i);
            vec2 const a2 = rw::lerp(back, v, float(index / int32_t(nf)) * 0.3f);
            f.dir = rw::lerp(a2, f.dir, 0.9f);
        }
        if (x.body_direction.x != 0.f)
            g.flip = mathf::sign(x.body_direction.x);
        update_head_angle(state, c, g, rng);
        // StartClimbing: the legs start over
        bool const climbing = yeek::climbing_mode(state, c);
        if (climbing && !g.was_climbing)
            for (auto& l : g.legs)
                leg_reset(l, c0);
        g.was_climbing = climbing ? 1 : 0;
        update_legs(state, c, room, g);
        // EyeShaker, EyeShakerScale (once a tick)
        for (uint32_t e = 0; e < 2; ++e)
            for (uint32_t k = 0; k < 2; ++k) {
                g.eye_shake[e][k] = mathf::lerp(0.f, frange(rng, -2.f, 2.f), g.eye_shudder);
                g.eye_shake_scale[e][k] = mathf::lerp(1.f, frange(rng, 0.8f, 1.1f), g.eye_shudder);
            }
        if (!yeek::consious(state, c)) {
            g.blink_start_counter = 0;
            g.blink_percent = mathf::min(g.blink_percent + 0.5f, 0.8f);
            return;
        }
        if (g.blink_start_counter < 5) {
            g.blink_percent = mathf::min(g.blink_percent + 0.3f, 1.f);
            if (g.blink_start_counter < 0) {
                g.blink_start_counter = rng.range(40, 300) + int32_t(100.f * pers.nervous);
                if (g.more_blinks > 0) {
                    --g.more_blinks;
                    g.blink_start_counter = rng.range(8, 10) + int32_t(10.f * pers.bravery);
                    return;
                }
                if (rng.value() < 0.5f)
                    g.more_blinks = 1;
            }
        } else {
            g.blink_percent = mathf::max(g.blink_percent - 0.18f, 0.f);
        }
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip) {
        auto const& w = state.world;
        auto const& g = gs(state, c);
        auto const& lk = yeek::looks_of(state, c);
        float const t = alpha;
        // ApplyPalette (Room.Darkness: the palette's, without the light sources)
        vec3 const black = pal.black();
        float const darkness = pal.darkness();
        vec3 const color = lerp3(lk.fur, black, 0.2f + darkness * 0.8f);
        vec3 const eye_color = lerp3(lerp3(lk.highlight, black, 0.7f), vec3{0.95f, 0.1f, 0.f}, darkness);
        vec3 const feather_root = lerp3(lk.fur, black, 0.3f + darkness * 0.7f);
        vec3 const feather_tip = lerp3(lk.highlight, black, 0.3f + darkness * 0.7f);
        bool const climbing = yeek::climbing_mode(state, c);
        // the tail's highlighted vertices
        auto const tail_col = [&](uint32_t i, uint32_t n) {
            int32_t const k = int32_t(i), m = int32_t(n);
            return v4(k > m - 16 || (k > m - 24 && k <= m - 20) ? lk.highlight : color);
        };

        // the body feathers (YeekFeather.DrawSprites)
        uint32_t const nf = lk.feathers < yeek::max_feathers ? lk.feathers : yeek::max_feathers;
        uint32_t const pg = lk.plumage < 7 ? lk.plumage : 0;
        for (uint32_t i = 0; i < nf; ++i) {
            auto const& f = g.feathers[i];
            float num = (pg == 4 || pg == 5) ? 1.5f : 2.f;
            float const perc = float(nf - i) / float(nf);
            float const ease = -((1.f - perc) * (1.f - perc)) + 1.f;
            num *= mathf::max(1.f + (0.3f - 1.f) * ease, 0.3f);
            vec2 const rp = rw::lerp(f.last_root, f.root, t);
            float const rot = custom::vec_to_deg(rw::lerp(f.last_dir, f.dir, t));
            sprites::add(out, sprite{scale_a[pg], rp, rot, vec2{1.f, num}, vec2{0.5f, 0.f}, feather_root});
            sprites::add(out, sprite{scale_b[pg], rp, rot, vec2{1.f, num}, vec2{0.5f, 0.f}, feather_tip});
        }

        // the body with the first tail, and the second tail
        vec2 bv[body_vertices];
        vec4 bc[body_vertices];
        vec2 tv[tail_vertices];
        vec4 tc[tail_vertices];
        {
            vec2 vector = graph_segment_pos(state, c, g, 0, t, false);
            vec2 vector2 = graph_segment_pos(state, c, g, 0, t, true);
            float num = graph_segment_rad(state, c, 0);
            for (uint32_t i = 0; i < tot_graph_segments; ++i) {
                vec2 const v3 = graph_segment_pos(state, c, g, i, t, false);
                vec2 const v4p = graph_segment_pos(state, c, g, i, t, true);
                float const num2 = graph_segment_rad(state, c, i);
                vec2 n = rw::normalized(v3 - vector);
                vec2 a = custom::perpendicular_vector(n);
                float d = rw::distance(v3, vector) / 5.f;
                bv[i * 4] = vector - a * ((num + num2) * 0.5f) + n * d;
                bv[i * 4 + 1] = vector + a * ((num + num2) * 0.5f) + n * d;
                if (i < tot_graph_segments - 1) {
                    bv[i * 4 + 2] = v3 - a * num2 - n * d;
                    bv[i * 4 + 3] = v3 + a * num2 - n * d;
                } else {
                    bv[i * 4 + 2] = v3;
                }
                n = rw::normalized(v4p - vector2);
                a = custom::perpendicular_vector(n);
                d = rw::distance(v4p, vector2) / 5.f;
                if (i >= body_mesh_length) {
                    uint32_t const k = i - body_mesh_length;
                    tv[k * 4] = vector2 - a * ((num + num2) * 0.5f) + n * d;
                    tv[k * 4 + 1] = vector2 + a * ((num + num2) * 0.5f) + n * d;
                    if (i < tot_graph_segments - 1) {
                        tv[k * 4 + 2] = v4p - a * num2 - n * d;
                        tv[k * 4 + 3] = v4p + a * num2 - n * d;
                    } else {
                        tv[k * 4 + 2] = v4p;
                    }
                }
                num = num2;
                vector = v3;
                vector2 = v4p;
            }
            for (uint32_t i = 0; i < body_vertices; ++i)
                bc[i] = tail_col(i, body_vertices);
            for (uint32_t i = 0; i < tail_vertices; ++i)
                tc[i] = tail_col(i, tail_vertices);
        }
        sprites::long_mesh(out, strip, bv, bc, body_vertices);

        // DrawHead: the neck mesh, the head, the eyes
        vec2 const b0 = rw::lerp(clast(state, c, 0), cpos(state, c, 0), t);
        vec2 const b1 = rw::lerp(clast(state, c, 1), cpos(state, c, 1), t);
        vec2 vector = rw::lerp(b0, rw::lerp(clast(state, c, 1) + vec2{0.f, -2.f}, cpos(state, c, 1) + vec2{0.f, -2.f}, t), 0.9f);
        vec2 const hd = rw::lerp(g.last_head_draw_dir, g.head_draw_dir, t);
        vector.y -= mathf::inverse_lerp(0.f, 1.f, hd.y) * 6.f;
        vec2 const head_pos = vector;
        float const head_rot = custom::vec_to_deg(hd);
        vec2 const v3 = custom::deg_to_vec(head_rot);
        float const num5 = right_shown(state, c, g, t);
        float const num6 = left_shown(state, c, g, t);
        float const num7 = mathf::inverse_lerp(-0.3f, -1.f, v3.y);
        float const num8 = mathf::inverse_lerp(0.3f, 1.f, v3.y);
        vec2 a2 = custom::dir_vec(b0, b1);
        if (climbing)
            a2 *= 0.13f;
        vec2 const a3 = custom::perpendicular_vector(custom::dir_vec(vec2{0.f, 0.f}, vec2{-num6 * 3.f, 1.f} + vec2{num5 * 3.f, 1.f}));
        {
            vec2 const nv[4] = {b0 + a2 * 3.f + a3 * -10.f, b0 + a3 * 10.f, vector + a2 * 3.f + a3 * -8.f, vector + a2 * 3.f + a3 * 8.f};
            sprites::long_mesh(out, strip, nv, 4, color);
        }
        sprites::long_mesh(out, strip, tv, tc, tail_vertices);

        // the legs (YeekLeg.DrawLeg)
        for (uint32_t i = 0; i < 2; ++i) {
            auto const& l = g.legs[i];
            float const num = rw::distance(l.root, l.foot);
            std::string_view el = "LizardArm_40";
            if (num <= leg_max_length * 0.3f || l.state == leg_state::tunnel)
                el = "LizardArm_10";
            else if (num <= leg_max_length * 0.55f)
                el = "LizardArm_20";
            else if (num <= leg_max_length * 0.8f)
                el = "LizardArm_30";
            vec2 const fp = rw::lerp(l.last_foot, l.foot, t);
            vec2 const rp = rw::lerp(l.last_root, l.root, t);
            float const rot = custom::vec_to_deg(custom::dir_vec(fp, rp)) + 90.f;
            bool const side = l.state != leg_state::climb && l.state != leg_state::sit;
            float sy;
            if (i != 0)
                sy = left_shown(state, c, g, t) > 0.f && side ? -1.f : 1.f;
            else
                sy = right_shown(state, c, g, t) > 0.f && side ? 1.f : -1.f;
            sprites::add(out, sprite{el, fp, rot, vec2{1.f, sy}, vec2{0.5f, 0.5f}, color});
        }

        // the head
        vector += hd * 4.f;
        sprites::add(out, sprite{"SnailShellA", head_pos + vec2{0.f, mathf::lerp(6.f, 0.f, num7)}, head_rot,
                              vec2{mathf::lerp(0.65f, 0.7f, num7), mathf::lerp(0.7f, 0.8f, num7)}, vec2{0.5f, 0.1f}, color});
        // the eyes: the right one, then the left one
        float const blink = 1.f - g.blink_percent;
        if (num5 > 0.14f || num7 > 0.1f) {
            float ex = vector.x + mathf::lerp(0.f, 3.f, num5);
            ex = mathf::lerp(ex, vector.x - 5.f, num7 + num8) + g.eye_shake[0][0];
            float ey = vector.y + mathf::lerp(10.f, 5.f, num5);
            ey = mathf::lerp(ey, vector.y - 1.f, num7) + g.eye_shake[0][1];
            float sx = mathf::lerp(mathf::lerp(0.f, 0.3f, num5), 0.3f, num7) * blink * g.eye_shake_scale[0][0];
            float const sy = mathf::lerp(0.4f, 0.3f, num5) * g.eye_shake_scale[0][1];
            sprites::add(out, sprite{"Circle20", vec2{ex, ey}, head_rot + mathf::lerp(0.f, 20.f, num8), vec2{sx, sy}, vec2{0.5f, 0.5f}, eye_color});
        }
        if (num6 > 0.14f || num7 > 0.1f) {
            float ex = vector.x + mathf::lerp(0.f, -3.f, num6);
            ex = mathf::lerp(ex, vector.x + 5.f, num7 + num8) + g.eye_shake[1][0];
            float ey = vector.y + mathf::lerp(10.f, 5.f, num6);
            ey = mathf::lerp(ey, vector.y - 1.f, num7) + g.eye_shake[1][1];
            float sx = mathf::lerp(mathf::lerp(0.f, 0.3f, num6), 0.3f, num7) * blink * g.eye_shake_scale[1][0];
            float const sy = mathf::lerp(0.4f, 0.3f, num6) * g.eye_shake_scale[1][1];
            sprites::add(out, sprite{"Circle20", vec2{ex, ey}, head_rot + mathf::lerp(0.f, -20.f, num8), vec2{sx, sy}, vec2{0.5f, 0.5f}, eye_color});
        }
        (void)w;
    }
}
