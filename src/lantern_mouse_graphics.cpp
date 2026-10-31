#include <algorithm>
#include <cmath>
#include <string_view>

#include "lantern_mouse_graphics.hpp"
#include "body_parts.hpp"
#include "creature.hpp"
#include "lantern_mouse.hpp"
#include "lantern_mouse_types.hpp"
#include "particles.hpp"
#include "player.hpp"
#include "tracker.hpp"

namespace lantern_mouse_graphics {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using world::room_view;
    using sprites::sprite;
    using lantern_mouse::graphics_state;
    using player::limb_mode;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        // Limb(owner, chunk, num, rad 1, surfaceFric 0.5, airFric 0.9,
        // huntSpeed 8, quickness 0.9)
        constexpr body_parts::limb_params limb_params{1.f, 0.5f, 0.9f, 8.f, 0.9f};
        constexpr std::string_view head_elements[5] = {"mouseHead0", "mouseHead1", "mouseHead2", "mouseHead3", "mouseHead4"};
        constexpr std::string_view eye_a_elements[6] = {"mouseEyeA0", "mouseEyeA1", "mouseEyeA2", "mouseEyeA3", "mouseEyeA4", "mouseEyeA5"};
        constexpr std::string_view eye_b_elements[6] = {"mouseEyeB0", "mouseEyeB1", "mouseEyeB2", "mouseEyeB3", "mouseEyeB4", "mouseEyeB5"};
        // TotalSprites; RopeSprite 0..2, BodySprite 3..4, LimbSprite 5..8,
        // (9 is an unused pixel), BackSpotSprite 10..13, HeadSprite 14,
        // EyeASprite 15..16, EyeBSprite 17..18, EyeCSprite 19..20
        constexpr uint32_t total_sprites = 21;

        dcon::lantern_mouse_data_id data(sys::game_state const& state, dcon::creature_id c) {
            return dcon::lantern_mouse_data_id{dcon::lantern_mouse_data_id::value_base_t(state.world.creature_get_payload(c).lantern_mouse.data)};
        }
        graphics_state& gs(sys::game_state& state, dcon::creature_id c) {
            return state.world.lantern_mouse_data_get_graphics(data(state, c));
        }
        graphics_state const& gs(sys::game_state const& state, dcon::creature_id c) {
            return state.world.lantern_mouse_data_get_graphics(data(state, c));
        }
        dcon::body_chunk_id ch(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return lantern_mouse::chunk(state, c, i);
        }

        // HSLColor and HSLColor.Lerp (component wise)
        struct hsl {
            float h, s, l;
        };
        hsl lerp_hsl(hsl a, hsl b, float t) {
            return hsl{mathf::lerp(a.h, b.h, t), mathf::lerp(a.s, b.s, t), mathf::lerp(a.l, b.l, t)};
        }
        vec3 rgb(hsl c) { return custom::hsl2rgb(c.h, c.s, c.l); }
        vec3 lerp3(vec3 a, vec3 b, float t) {
            t = mathf::clamp01(t);
            return a + (b - a) * t;
        }

        // MouseGraphics.BodyColor, DecalColor, EyesColor
        vec3 body_color(graphics_state const& g, float hue, float batt, vec3 black, float lit_room) {
            hsl from{hue, 1.f - g.flicker * 0.3f, 0.9f - g.flicker * 0.5f};
            if (g.charging > 0.f)
                from = lerp_hsl(from, hsl{hue, 0.05f, 0.1f}, g.charging);
            if (batt < 1.f)
                from = lerp_hsl(from, hsl{hue, 0.f, 0.1f}, 1.f - batt);
            return lerp3(rgb(from), black, lit_room * 0.6f);
        }
        vec3 decal_color(graphics_state const& g, float hue, float batt, vec3 black, float lit_room) {
            hsl from{hue, 0.3f, 0.45f * (1.f - g.flicker)};
            if (g.charging > 0.f) {
                float const cc = mathf::clamp01(g.charging);
                from = lerp_hsl(from, hsl{hue, cc, 0.f}, mathf::inverse_lerp(0.f, 0.3f, g.charging));
                if (g.charging > 0.3f)
                    from = lerp_hsl(from, hsl{hue, cc, 0.5f}, mathf::inverse_lerp(0.3f, 1.f, g.charging));
            }
            if (batt < 1.f)
                from = lerp_hsl(from, hsl{hue, 0.6f, 0.08f}, 1.f - batt);
            return lerp3(rgb(from), black, lit_room * 0.7f);
        }
        vec3 eyes_color(graphics_state const& g, vec3 black) {
            if (g.charging == 0.f)
                return black;
            return lerp3(black, vec3{1.f, 1.f, 1.f}, mathf::inverse_lerp(0.f, 0.25f, g.charging));
        }

        // MouseSpark: a fleck of the mouse's colour (particles.hpp's spark)
        void spark(sys::game_state& state, dcon::room_id room, vec2 pos, vec2 vel, float life, float hue) {
            particles::spawn(state, room, particles::kind::spark, pos, vel, 1.5f, life / 40.f, custom::hsl2rgb(hue, 1.f, 0.9f));
        }

        // MouseGraphics.ShouldThisLimbRetract
        bool should_retract(sys::game_state const& state, dcon::creature_id c, graphics_state const& g, uint32_t k) {
            bool const rope = lantern_mouse::hanging(state, c);
            bool const sitting = state.world.creature_get_payload(c).lantern_mouse.sitting != 0;
            return (rope && (k == 1 || !g.hands_out_while_hanging)) || (sitting && k == 0);
        }

        // Limb.Update: `retract` hunts the connection, then Retracted
        void limb_update(player::hand_state& h, uint8_t retract, vec2 conn, vec2 conn_vel, room_view const& room) {
            if (retract && h.mode != limb_mode::retracted) {
                h.mode = limb_mode::hunt_absolute_position;
                h.absolute_hunt_pos = conn;
                if (custom::dist_less(h.absolute_hunt_pos, h.pos, h.hunt_speed))
                    h.mode = limb_mode::retracted;
            }
            body_parts::limb_update(h, limb_params, conn, conn_vel, conn, room);
        }
    }

    void reset_unconsious_profile(sys::game_state& state, dcon::creature_id c) {
        auto& g = gs(state, c);
        auto r = rw::random(state.seed, state.tick, uint32_t(c.value));
        if (r.value() < 0.5f)
            g.unconsious_profile_fac = 0.f;
        else
            g.unconsious_profile_fac = mathf::lerp(-1.f, 1.f, r.value());
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        auto& g = gs(state, c);
        auto r = rw::random(state.seed, state.tick, uint32_t(c.value));
        vec2 const at = w.body_chunk_get_pos(ch(state, c, 0));
        // GraphicsModule.Reset: every body part on the main chunk
        auto const reset = [&](vec2& pos, vec2& last, vec2& vel) {
            vel = vec2{0.f, 0.f};
            pos = at;
            last = pos;
        };
        reset(g.head.pos, g.head.last_pos, g.head.vel);
        reset(g.tail.pos, g.tail.last_pos, g.tail.vel);
        g.head.rad = 0.f;
        g.tail.rad = 1.f;
        for (auto& row : g.limbs)
            for (auto& l : row) {
                reset(l.pos, l.last_pos, l.vel);
                l.hunt_speed = limb_params.hunt_speed;
                l.quickness = limb_params.quickness;
            }
        for (auto& rp : g.rope_positions)
            rp[0] = rp[1] = at;
        // the constructor's rolls, once
        if (!g.made) {
            g.made = 1;
            for (auto& row : g.limbs)
                for (auto& l : row)
                    l.mode = limb_mode::dangle;
            reset_unconsious_profile(state, c);
            g.hands_out_while_hanging = r.value() < 0.125f ? 1 : 0;
            g.hang_backwards = r.value() < 0.05882353f ? 1 : 0;
        }
    }

    void spawn_as_hanging(sys::game_state& state, dcon::creature_id c) {
        auto& g = gs(state, c);
        g.light_mode = 1.f;
        g.charging = 0.f;
        g.super_charge = 0.f;
    }

    void ouch(sys::game_state& state, dcon::creature_id c, int32_t ticks) {
        auto& g = gs(state, c);
        g.ouch_eyes = std::max(g.ouch_eyes, ticks);
    }

    void shove_head(sys::game_state& state, dcon::creature_id c, vec2 by) {
        gs(state, c).head.pos += by;
    }

    void jostle(sys::game_state& state, dcon::creature_id c, rw::rng& r) {
        auto& g = gs(state, c);
        auto const j = [&](vec2& pos, vec2& vel) {
            pos += custom::deg_to_vec(r.value() * 360.f) * 3.f * r.value();
            vel += custom::deg_to_vec(r.value() * 360.f) * 6.f * r.value();
        };
        for (auto& row : g.limbs)
            for (auto& l : row)
                j(l.pos, l.vel);
        j(g.head.pos, g.head.vel);
        j(g.tail.pos, g.tail.vel);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto& g = gs(state, c);
        auto r = rw::random(state.seed, state.tick, uint32_t(c.value));
        auto const& x = w.creature_get_payload(c).lantern_mouse;
        auto const d = data(state, c);
        auto const& rope = w.lantern_mouse_data_get_rope(d);
        bool const awake = lantern_mouse::consious(state, c);
        bool const dead = w.creature_get_dead(c) != 0;
        bool const has_rope = rope.bend_count > 0;
        bool const asleep = lantern_mouse::sleeping(state, c);
        float const fear = lantern_mouse::fear(state, c);
        float const batt = lantern_mouse::battery(state, c);
        float const hue = lantern_mouse::hue(state, c);
        auto const k0 = ch(state, c, 0), k1 = ch(state, c, 1);
        vec2 const c0 = w.body_chunk_get_pos(k0), c1 = w.body_chunk_get_pos(k1);
        vec2 const v0 = w.body_chunk_get_vel(k0), v1 = w.body_chunk_get_vel(k1);
        // (Mouse_Light_On_LOOP, Mouse_Charge_LOOP: no sound ids)
        g.last_profile_fac = g.profile_fac;
        if (awake) {
            g.profile_fac = x.profile_fac;
            if (g.charging > 0.f) {
                // charging up on the rope, then the light switches on
                g.charging += 1.f / mathf::lerp(60.f, 200.f, r.value());
                if (g.charging >= 2.f) {
                    g.charging = 0.f;
                    g.light_mode = 1.f;
                    g.super_charge = 1.f;
                    g.flickering_fac = r.value() < 0.5f ? 0.f : 1.f;
                    g.flicker_duration = mathf::lerp(30.f, 220.f, r.value());
                    for (int i = 0; i < 20; ++i) {
                        vec2 const a = custom::deg_to_vec(360.f * r.value());
                        spark(state, room.id, c0 + a * 9.f, v0 + a * 36.f * r.value(), 20.f, hue);
                    }
                }
            } else if (has_rope && g.light_mode < 0.5f) {
                g.charging = 0.001f;
            }
            if (!has_rope) {
                g.light_mode = mathf::lerp(g.light_mode, 0.f, 0.05f);
                if (r.value() < mathf::pow(fear * x.run_speed, 3.f))
                    spark(state, room.id, c0, v0 + custom::deg_to_vec(360.f * r.value()) * 6.f * r.value(), 40.f, hue);
            }
            if (!asleep && r.value() < 0.0016666667f) {
                g.hands_out_while_hanging = r.value() < 0.125f ? 1 : 0;
                g.hang_backwards = r.value() < 0.05882353f ? 1 : 0;
            }
        } else {
            if (g.last_consious)
                reset_unconsious_profile(state, c);
            g.profile_fac = g.unconsious_profile_fac;
            g.charging = mathf::lerp(g.charging, 0.f, 0.033333335f);
            if (!dead && !x.carried) {
                g.flickering_fac = 1.f;
                g.flicker_duration = mathf::lerp(10.f, 30.f, r.value());
                if (r.value() < 0.1f)
                    g.flicker = mathf::max(g.flicker, r.value());
                g.light_mode = mathf::lerp(g.light_mode, 0.3f, 0.008333334f);
            } else {
                g.light_mode = mathf::lerp(g.light_mode, mathf::lerp(0.3f, 0.7f, batt), 0.008333334f);
            }
        }
        g.last_consious = awake ? 1 : 0;
        // flickering (Mouse_Light_Flicker: silent)
        if (g.flickering_fac > 0.f) {
            g.flickering_fac = mathf::max(g.flickering_fac - 1.f / g.flicker_duration, 0.f);
            if (r.value() < 0.06666667f && r.value() < g.flickering_fac)
                g.flicker = mathf::pow(r.value(), 1.f - g.flickering_fac);
        } else if (!dead && r.value() < 0.0033333334f) {
            g.flickering_fac = r.value();
            g.flicker_duration = mathf::lerp(30.f, 120.f, r.value());
        }
        if (g.flicker > 0.f)
            g.flicker = mathf::max(g.flicker - 0.06666667f, 0.f);
        if (!dead)
            g.breath += 1.f / mathf::lerp(mathf::lerp(40.f, 20.f, fear), 180.f, x.fall_asleep);
        g.last_run_cycle = g.run_cycle;
        g.run_cycle = x.run_cycle;
        g.last_back_to_cam = g.back_to_cam;
        if ((has_rope && !g.hang_backwards) || x.climbing_corridor)
            g.back_to_cam = mathf::min(g.back_to_cam + 0.05f, 1.f);
        else
            g.back_to_cam = mathf::max(g.back_to_cam - 0.05f, 0.f);
        if (g.back_to_cam > 0.f)
            g.profile_fac = std::sin(g.back_to_cam * mathf::pi);
        if (g.ouch_eyes > 0)
            --g.ouch_eyes;

        // CreatureLooker, simplified: it looks at what it fears most
        auto const& ts = tracker::of(state, c);
        g.look_at = ts.threat;
        g.last_look_dir = g.look_dir;
        auto const* rep = g.look_at ? tracker::find(ts, g.look_at) : nullptr;
        if (awake && rep && creature::live(state, g.look_at)) {
            vec2 const target = rep->visual ? creature::main_pos(state, g.look_at) : room_view::middle_of_tile(tracker::best_guess(*rep));
            g.look_dir = rw::clamp_magnitude((target - g.head.pos) / 70.f, 1.f);
        } else {
            g.look_dir *= 0.9f;
        }
        g.look_dir *= 1.f - x.fall_asleep;

        // the head, ahead of the main chunk
        g.head.last_pos = g.head.pos;
        g.head.pos += g.head.vel;
        vec2 vec = c0 + custom::dir_vec(c1, c0) * (3.f + 3.f * std::fabs(g.profile_fac));
        body_parts::connect_to_point(g.head, vec, 4.f, false, 0.f, v0, 0.5f, 0.1f);
        g.head.vel += (vec - g.head.pos) / 6.f;
        g.head.vel += g.look_dir;
        if (!awake)
            g.head.vel.y -= 0.6f;
        // the tail, along the rope when hanging
        g.tail.last_pos = g.tail.pos;
        if (has_rope) {
            g.tail.pos = c1 + rw::clamp_magnitude(rope.bends[rope.bend_count - 1] - c1, mathf::lerp(12.f, 30.f, x.rope_stretch));
            g.tail.vel = vec2{0.f, 0.f};
        } else {
            g.tail.pos += g.tail.vel;
            vec = c1 + custom::dir_vec(c0, c1) * 8.f;
            body_parts::connect_to_point(g.tail, vec, 7.f, false, 0.f, v1, 0.1f, 0.f);
            if (!x.sitting)
                g.tail.vel += (vec - g.tail.pos) / 46.f;
            else
                g.tail.vel.x += mathf::sign(g.tail.pos.x - c1.x) * 7.f;
            if (awake && !x.sitting)
                g.tail.pos += custom::deg_to_vec(r.value() * 360.f) * 2.f * r.value();
            else
                g.tail.vel.y -= 0.6f;
            body_parts::push_out_of_terrain(g.tail, room, 0.f);
        }
        --g.blink;
        if ((g.blink < 0 && r.value() < 0.2f && r.value() > x.fall_asleep) || g.blink < -15)
            g.blink = r.range(3, 4 + int32_t(100.f * (1.f - x.fall_asleep)));
        if (x.voice_counter > 0)
            g.blink = std::max(g.blink, 1);
        for (uint32_t j = 0; j < 4; ++j) {
            g.rope_positions[j][1] = g.rope_positions[j][0];
            if (!has_rope || j >= rope.bend_count)
                g.rope_positions[j][0] = g.tail.pos;
            else
                g.rope_positions[j][0] = rope.bends[j];
        }

        // the legs: running steps, grips, dangling, tucked in on the rope
        vec2 const vector2 = custom::dir_vec(c1, c0);
        vec2 const a2 = custom::perpendicular_vector(vector2) * mathf::lerp(1.f, -1.f, g.back_to_cam);
        for (uint32_t k = 0; k < 2; ++k)
            for (uint32_t l = 0; l < 2; ++l) {
                auto& lb = g.limbs[k][l];
                float const side = l == 0 ? -1.f : 1.f;
                vec2 const ck = k == 0 ? c0 : c1;
                vec2 const vk = k == 0 ? v0 : v1;
                limb_update(lb, g.retract[k][l], ck, vk, room);
                body_parts::as_body_part(lb, limb_params.rad, [&](player::body_part& bp) { body_parts::connect_to_point(bp, ck, 12.f, false, 0.f, vk, 0.f, 0.f); });
                lb.vel.y -= 0.6f;
                if (!g.retract[k][l])
                    lb.mode = limb_mode::dangle;
                float const limb_number = float(k + l + l);
                if (awake && !has_rope && !x.sitting) {
                    float const num = std::sin((g.run_cycle + limb_number / 4.f) * mathf::pi * 2.f);
                    vec2 const goal = ck + vector2 * 8.f * (0.3f + 0.7f * num) + a2 * 4.f * side;
                    body_parts::find_grip(lb, room, ck, ck, 15.f, goal, 2, 2, false);
                    lb.pos += vector2 * (2.f + num * 8.f) * mathf::lerp(0.5f, 2.f, fear);
                    lb.pos -= a2 * side * 3.f * std::cos((g.run_cycle + limb_number / 4.f) * mathf::pi * 2.f) * mathf::lerp(0.5f, 2.f, fear);
                }
                bool const retract_now = should_retract(state, c, g, k);
                if (!retract_now) {
                    g.retract[k][l] = 0;
                    if (x.sitting) {
                        lb.vel.y -= 2.f;
                        lb.vel -= a2 * side * 7.f;
                    }
                }
                if (lb.mode == limb_mode::dangle) {
                    if (awake && fear > 0.1f) {
                        lb.vel += vector2 * r.value() * 4.f + custom::deg_to_vec(r.value() * 360.f) * fear * 8.f;
                        lb.pos += custom::deg_to_vec(r.value() * 360.f) * fear * 8.f;
                    } else if (retract_now) {
                        g.retract[k][l] = 1;
                    } else if (has_rope) {
                        lb.vel += a2 * 0.6f * side;
                    } else {
                        lb.vel += a2 * 0.6f * side;
                        lb.vel += vector2 * 0.6f * (k == 0 ? -1.f : 1.f);
                    }
                }
            }
    }

    bool light_of(sys::game_state const& state, dcon::creature_id c, float alpha, float darkness, light& out) {
        auto const& w = state.world;
        // the LightSource is only kept while the room is dark
        if (darkness <= 0.f || w.creature_get_away(c) != 0)
            return false;
        // (nor while carried through a pipe)
        uint32_t gi = 0;
        if (auto const h = creature::holder(state, c, &gi); h && creature::is_slugcat(w, h)) {
            auto const p = creature::player_of(state, h);
            if (p && player::in_shortcut(state, p))
                return false;
        }
        auto const& g = gs(state, c);
        auto const& x = w.creature_get_payload(c).lantern_mouse;
        float const batt = lantern_mouse::battery(state, c);
        auto const k0 = ch(state, c, 0), k1 = ch(state, c, 1);
        vec2 const p0 = rw::lerp(w.body_chunk_get_last_pos(k0), w.body_chunk_get_pos(k0), alpha);
        vec2 const p1 = rw::lerp(w.body_chunk_get_last_pos(k1), w.body_chunk_get_pos(k1), alpha);
        float const charge = mathf::clamp01(g.charging);
        out.pos = rw::lerp(p0, p1, 0.3f);
        out.rad = mathf::lerp(mathf::lerp(80.f, 280.f, g.light_mode * (1.f - g.flicker)), 40.f, charge)
            * (1.f + std::sin(g.breath * mathf::pi * 2.f) * mathf::lerp(0.05f, 0.2f, x.fall_asleep)) * batt;
        // MouseGraphics.LightStrength
        float const strength = (1.f - charge * 0.6f) * batt;
        out.alpha = mathf::lerp(mathf::lerp(0.8f, 0.3f, g.light_mode), 1.f, custom::s_curve(g.super_charge, 0.3f) * 0.5f) * strength * (1.f - g.flicker * 0.4f);
        out.color = custom::hsl2rgb(lantern_mouse::hue(state, c), 1.f * batt, 0.8f - 0.2f * g.flicker);
        return out.rad > 0.f && out.alpha > 0.f;
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>&) {
        auto const& w = state.world;
        auto const& g = gs(state, c);
        auto const& x = w.creature_get_payload(c).lantern_mouse;
        auto const& rope = w.lantern_mouse_data_get_rope(data(state, c));
        bool const has_rope = rope.bend_count > 0;
        bool const awake = lantern_mouse::consious(state, c);
        bool const dead = w.creature_get_dead(c) != 0;
        float const t = alpha;
        float const hue = lantern_mouse::hue(state, c);
        float const batt = lantern_mouse::battery(state, c);
        // ApplyPalette, and Room.Darkness for litRoom
        vec3 const black = pal.black();
        float const lit_room = mathf::pow(1.f - mathf::inverse_lerp(0.f, 0.5f, pal.darkness()), 3.f);
        vec3 const body = body_color(g, hue, batt, black, lit_room);
        vec3 const decal = decal_color(g, hue, batt, black, lit_room);
        vec3 const eyes = eyes_color(g, black);
        // DrawSprites' UnityEngine.Random (the shakes), not the simulation's
        auto r = rw::random(state.seed, state.tick, uint32_t(c.value));

        sprites::draw_cmd cmds[total_sprites];
        bool vis[total_sprites] = {};
        auto const put = [&](uint32_t i, sprite const& s) {
            cmds[i] = sprites::draw_cmd{};
            cmds[i].spr = s;
            vis[i] = true;
        };

        auto const k0 = ch(state, c, 0), k1 = ch(state, c, 1);
        vec2 v = rw::lerp(w.body_chunk_get_last_pos(k1), w.body_chunk_get_pos(k1), t);
        vec2 v2 = rw::lerp(w.body_chunk_get_last_pos(k0), w.body_chunk_get_pos(k0), t);
        float const num = mathf::lerp(g.last_run_cycle, g.run_cycle, t);
        if (g.charging > 0.f) {
            v += custom::deg_to_vec(r.value() * 360.f) * 1.5f * mathf::clamp01(g.charging);
            v2 += custom::deg_to_vec(r.value() * 360.f) * 1.5f * mathf::clamp01(g.charging);
        } else if (x.shrinking_rope) {
            v.x += std::sin(num * mathf::pi * 5.f) * 3.f;
        }
        vec2 const v3 = rw::lerp(g.tail.last_pos, g.tail.pos, t);
        vec2 v4 = rw::lerp(g.head.last_pos, g.head.pos, t);
        if (awake && x.voice_counter > 0)
            v4 += custom::rnv(r) * r.value() * 2.f;
        if (x.sitting)
            v.y -= 4.f;
        float const num2 = mathf::lerp(g.last_profile_fac, g.profile_fac, t);
        float const num3 = mathf::lerp(g.last_back_to_cam, g.back_to_cam, t);
        vec2 const v5 = custom::dir_vec(v, v2);
        vec2 const a = custom::perpendicular_vector(v5);
        if (!has_rope) {
            v += a * g.profile_fac * std::sin(num * mathf::pi * 2.f) * 4.f * x.run_speed;
            v += v5 * g.profile_fac * std::sin(num * mathf::pi * 2.f) * 3.f;
            v2 += v5 * std::cos(num * mathf::pi * 2.f) * 4.f * x.run_speed;
            v2 += a * std::sin(num * mathf::pi * 2.f) * 4.f * (1.f - std::fabs(g.profile_fac));
        }
        float const rotation = custom::aim(v, v2);

        // the legs and the back spots
        for (uint32_t k = 0; k < 2; ++k)
            for (uint32_t l = 0; l < 2; ++l) {
                float const side = l == 0 ? -1.f : 1.f;
                auto const& lb = g.limbs[k][l];
                vec2 v6 = rw::lerp(lb.last_pos, lb.pos, t);
                vec2 v7 = k == 0 ? v2 : v;
                v7 += a * side * 3.f * (1.f - std::fabs(num2));
                if (k == 1)
                    v7 -= v5 * 2.f;
                if (!custom::dist_less(v6, v7, 19.f))
                    v6 = v7 + custom::dir_vec(v7, v6) * 19.f;
                float sx;
                if (has_rope)
                    sx = ((l == 0) != (g.hang_backwards != 0)) ? -1.f : 1.f;
                else
                    sx = mathf::sign(custom::distance_to_line(v6, v2 - a * side, v - a * side));
                if (lb.mode != limb_mode::retracted)
                    put(5 + k + l + l, sprite{k == 1 ? std::string_view{"mouseHindLeg"} : std::string_view{"mouseFrontLeg"}, v6, custom::aim(v6, v7),
                                         vec2{sx, mathf::lerp(rw::distance(v6, v7) / 18.f, 1.f, 0.2f)}, vec2{0.5f, 0.1f}, body});
                float num4 = std::fabs(num2);
                float num5 = mathf::max(num4, mathf::inverse_lerp(0.5f, 0.7f, num3));
                num4 = mathf::lerp(mathf::lerp(1.2f, 0.8f, num4), num4, num3);
                if (num5 < 0.1f)
                    continue;
                vec2 v8 = rw::lerp(v2, v, 0.5f);
                v8 = k == 0 ? rw::lerp(v8, v4, 0.3f) : rw::lerp(v8, v3, 0.3f);
                v8 += a * side * (k == 1 ? 4.f : 3.f) * (1.f - num4);
                if (num3 > 0.01f && num3 < 0.99f) {
                    float const num6 = std::sin(num3 * mathf::pi);
                    num4 = mathf::lerp(num4, 1.f, num6);
                    num5 *= 1.f - num6;
                }
                if ((l == 0) == (num2 < 0.f)) {
                    v8 += a * mathf::pow(num4, 0.5f) * mathf::sign(num2) * (k == 1 ? 6.f : 5.f);
                    num5 *= 1.f - mathf::pow(num4, 1.5f);
                } else {
                    v8 += a * mathf::pow(num4, 1.2f) * mathf::sign(num2) * (k == 1 ? 4.f : 3.f);
                }
                put(10 + l + k + k, sprite{"mouseSpot", v8, rotation, vec2{num5 * (l == 1 ? -1.f : 1.f), 1.f}, vec2{0.5f, 0.5f}, decal});
            }

        // the body: the rear (anchored at its bottom, reaching for the tail)
        // and the front
        {
            vec2 const from = rw::lerp(v, v2, 0.34f);
            put(4, sprite{"mouseBodyB", rw::lerp(v, v2, 0.5f), custom::aim(from, v3),
                       vec2{1.f / (1.f + x.rope_stretch * 0.2f), (rw::distance(from, v3) + 5.f) / 24.f}, vec2{0.5f, 0.f}, body});
        }
        float num10 = custom::aim(rw::lerp(v, v2, 0.5f), v4) - 180.f * num3;
        num10 -= 90.f * num2;
        vec2 const v12 = rw::lerp(g.last_look_dir, g.look_dir, t);
        vec2 const v13 = body_parts::rotate_around_origo(v12, -num10);
        put(3, sprite{"mouseBodyA", rw::lerp(v, v2, 0.75f), custom::aim(rw::lerp(v2, v4, 0.3f), v),
                   vec2{1.f, mathf::lerp(mathf::lerp(1.2f, 0.8f, num3), mathf::lerp(0.8f, 1.2f, num3), mathf::inverse_lerp(1.f, -1.f, v13.y))}, vec2{0.5f, 0.5f}, body});

        // the head
        {
            int32_t const num11 = int32_t(std::floor(std::fabs(num2) * 4.9f));
            std::string_view el = head_elements[0];
            if (num11 > 0)
                el = head_elements[num11 < 5 ? num11 : 4];
            else if (v13.y > 0.75f)
                el = "mouseHeadUp";
            else if (v13.y < -0.75f)
                el = "mouseHeadDown";
            put(14, sprite{el, v4, num10, vec2{num2 > 0.f ? 1.f : -1.f, 1.f}, vec2{0.5f, 0.5f}, body});
        }

        // the eyes
        for (uint32_t e = 0; e < 2; ++e) {
            bool flag = true, flag2 = true;
            float num13 = num10;
            vec2 vec{4.f, -2.f};
            float const flip = e == 1 ? -1.f : 1.f;
            float sx = flip, sy = 1.f;
            float const ap = std::fabs(num2);
            if ((e == 0) == (v13.x < 0.f)) {
                sx = mathf::lerp(1.f, 0.f, mathf::inverse_lerp(0.1f, 0.4f, ap)) * flip;
                vec.x += 5.f * ap;
                vec.y += 2.5f * ap;
                num13 += mathf::sign(v13.x) * mathf::inverse_lerp(0.1f, 0.4f, ap) * -25.f;
                flag = ap < 0.1f;
                flag2 = ap < 0.2f;
            } else {
                if (ap > 0.1f) {
                    sx = mathf::lerp(1.f, 0.85714f, ap) * flip;
                    flag = false;
                }
                vec.x -= 4.5f * mathf::pow(ap, 0.6f);
            }
            vec.x *= e == 0 ? -1.f : 1.f;
            if (std::fabs(v13.y) > 0.75f) {
                sy = 0.85714f;
                flag = false;
                vec.y += v13.y * (v13.y > 0.f ? 2.f : 1.f);
            }
            vec2 const v14 = v4 + v12 + body_parts::rotate_around_origo(vec, num10);
            int32_t num14;
            if (awake) {
                if (g.ouch_eyes > 0) {
                    num14 = 2;
                } else if (lantern_mouse::sleeping(state, c)) {
                    num14 = 4;
                } else if (x.wake_up < 1.f) {
                    num14 = x.wake_up < 0.75f ? 4 : 3;
                    if (rw::magnitude(g.look_dir) > 0.3f) {
                        if ((v14.x < v4.x) == (g.look_dir.x < 0.f))
                            num14 = 1;
                    } else {
                        num14 = 3;
                    }
                } else {
                    num14 = g.blink > 0 ? 1 : 3;
                }
            } else {
                num14 = dead ? 5 : 2;
            }
            float const rot = flag && num14 == 1 ? 0.f : num13;
            put(15 + e, sprite{eye_a_elements[num14], v14, rot, vec2{sx, sy}, vec2{0.5f, 0.5f}, decal});
            put(17 + e, sprite{eye_b_elements[num14], v14, rot, vec2{sx, sy}, vec2{0.5f, 0.5f}, eyes});
            if (flag2 && num14 == 1)
                put(19 + e, sprite{"pixel", v14 + vec2{-1.f, 1.f}, 0.f, vec2{1.f, 1.f}, vec2{0.5f, 0.5f}, vec3{1.f, 1.f, 1.f}});
        }

        // the rope: three quads from the knot down, fading from the
        // palette's black into the body colour over its length
        {
            float const rope_length = x.rope_length;
            float num7 = 0.f;
            vec2 v9 = rw::lerp(g.rope_positions[0][1], g.rope_positions[0][0], t);
            for (uint32_t m = 0; m < 3; ++m) {
                vec2 const v10 = rw::lerp(g.rope_positions[m + 1][1], g.rope_positions[m + 1][0], t);
                float const num8 = mathf::inverse_lerp(0.f, rope_length, num7);
                num7 += rw::distance(v10, v9);
                float const num9 = mathf::inverse_lerp(0.f, rope_length, num7);
                // (a segment of no length is a quad of no size in vanilla)
                if (v9 != v10) {
                    vec2 const v11 = custom::dir_vec(v9, v10);
                    vec2 const a2 = custom::perpendicular_vector(v11);
                    vec2 const corners[4] = {v10 + v11 - a2 * 0.5f, v10 + v11 + a2 * 0.5f, v9 - v11 + a2 * 0.5f, v9 - v11 - a2 * 0.5f};
                    vec4 cols[4];
                    for (int n = 0; n < 4; ++n) {
                        vec3 const col = lerp3(black, body, n < 2 ? num9 : num8);
                        cols[n] = vec4{col.x, col.y, col.z, 1.f};
                    }
                    cmds[m] = sprites::draw_cmd{};
                    cmds[m].spr.element = "pixel";
                    cmds[m].is_quad = true;
                    cmds[m].vertex_colors = true;
                    for (int n = 0; n < 4; ++n) {
                        cmds[m].quad[n] = corners[n];
                        cmds[m].vcol[n] = cols[n];
                    }
                    vis[m] = true;
                }
                v9 = v10;
            }
        }

        for (uint32_t i = 0; i < total_sprites; ++i)
            if (vis[i])
                out.push_back(cmds[i]);
    }
}
