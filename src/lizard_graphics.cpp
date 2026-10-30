#include <cmath>
#include <cstdio>
#include <string>

#include "lizard_graphics.hpp"
#include "body_parts.hpp"
#include "lizard.hpp"
#include "lizard_ai.hpp"
#include "lizard_cosmetics.hpp"
#include "player.hpp"
#include "tracker.hpp"

// LizardGraphics. The cosmetics (LizardCosmetics, lizard_cosmetics.cpp) hook
// in at three places, marked "cosmetics:" below:
//   - reset: the constructor's AddCosmetic calls come between the
//     blackSalamander roll and the blink/breath rolls (vanilla's random
//     draws are in that order), and Template.Reset where LizardGraphics.Reset
//     calls it (after the limbs, before the head)
//   - update: Template.Update for every cosmetic after the head light-up /
//     camouflage bookkeeping and before whiteFlicker counts down
//   - build: Template.DrawSprites (and ApplyPalette) by
//     Template.spritesOverlap exactly where AddToContainer puts them: Behind
//     first, BodySurface after the tail, BehindHead before the jaw, InFront
//     after the eyes, then HUD
// Their state is lizard::graphics_state::cosmetics.

namespace lizard_graphics {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using world::room_view;
    using player::limb_mode;
    using sprites::sprite;
    using lizard::breed;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        // LizardLimb: Limb(rad 2.5, surface friction 0.7, air friction 0.99)
        constexpr float limb_rad = 2.5f;
        constexpr float limb_surface_friction = 0.7f;
        constexpr float limb_air_friction = 0.99f;

        vec2 chunk_pos(sys::game_state const& state, dcon::creature_id l, uint32_t i) {
            return state.world.body_chunk_get_pos(lizard::chunk(state, l, i));
        }
        vec2 chunk_vel(sys::game_state const& state, dcon::creature_id l, uint32_t i) {
            return state.world.body_chunk_get_vel(lizard::chunk(state, l, i));
        }
        float chunk_rad(sys::game_state const& state, dcon::creature_id l, uint32_t i) {
            return state.world.body_chunk_get_rad(lizard::chunk(state, l, i));
        }
        // BodyChunk.rotationChunk (as the BodyChunkConnections leave it): the
        // head's and the middle's is the hips, the hips' the head
        uint32_t rotation_chunk(uint32_t i) {
            return i == 2 ? 0u : 2u;
        }

        // Color.Lerp
        vec3 mix(vec3 a, vec3 b, float t) {
            return a + (b - a) * mathf::clamp01(t);
        }

        bool stunned(sys::game_state const& state, dcon::creature_id l) {
            return state.world.creature_get_stun(l) > 0;
        }

        // BodyPart.Reset
        void reset_part(player::body_part& bp, vec2 at) {
            bp.pos = bp.last_pos = at;
            bp.vel = vec2{0.f, 0.f};
        }

        // LizardGraphics.BodyChunkDisplayRad
        float display_rad(sys::game_state const& state, dcon::creature_id l, uint32_t i) {
            return chunk_rad(state, l, i) * (1.f / lizard::params_of(state, l).body_rad_fac);
        }

        // LizardGraphics.HeadLightsUpFromNoise (no rot here: the Black lizard)
        bool head_lights_up_from_noise(breed b) {
            return b == breed::black;
        }

        // LizardLimb.health
        float limb_health(sys::game_state const& state, dcon::creature_id l, int32_t i) {
            return lizard::consious(state, l) ? state.world.creature_get_payload(l).lizard.limb_health.v[i] : 0.f;
        }

        // Limb.Update (with Limb.retract and BodyPart.terrainContact)
        void limb_base_update(lizard::limb& lb, vec2 conn, vec2 conn_vel, vec2 rot, room_view const& room, float default_speed, float default_quickness) {
            auto& h = lb.h;
            h.last_pos = h.pos;
            if (lb.retract && h.mode != limb_mode::retracted) {
                h.mode = limb_mode::hunt_absolute_position;
                h.absolute_hunt_pos = conn;
                if (custom::dist_less(h.absolute_hunt_pos, h.pos, h.hunt_speed))
                    h.mode = limb_mode::retracted;
            }
            if (h.mode == limb_mode::hunt_relative_position)
                h.absolute_hunt_pos = conn + body_parts::rotate_around_origo(h.relative_hunt_pos, custom::aim(rot, conn));
            if (h.mode == limb_mode::hunt_relative_position || h.mode == limb_mode::hunt_absolute_position) {
                if (custom::dist_less(h.absolute_hunt_pos, h.pos, h.hunt_speed)) {
                    h.vel = h.absolute_hunt_pos - h.pos;
                    h.reached_snap_position = 1;
                } else {
                    h.vel = rw::lerp(h.vel, custom::dir_vec(h.pos, h.absolute_hunt_pos) * h.hunt_speed, h.quickness);
                    h.reached_snap_position = 0;
                }
            } else if (h.mode == limb_mode::retracted) {
                h.vel = conn_vel;
                h.pos = conn;
                h.reached_snap_position = 1;
            } else {
                h.reached_snap_position = 0;
            }
            h.quickness = default_quickness;
            h.hunt_speed = default_speed;
            if (h.mode != limb_mode::retracted) {
                h.pos += h.vel;
                if (h.mode == limb_mode::hunt_relative_position)
                    h.pos += conn_vel;
                h.vel *= limb_air_friction;
                body_parts::as_body_part(h, limb_rad, [&](player::body_part& bp) { lb.terrain_contact = body_parts::push_out_of_terrain(bp, room, limb_surface_friction); });
            }
        }

        // Limb.OverLappingHuntPos
        bool over_lapping_hunt_pos(lizard::limb const& lb) {
            return lb.h.reached_snap_position || custom::dist_less(lb.h.pos, lb.h.absolute_hunt_pos, limb_rad + 1.f);
        }

        // LizardLimb.Update
        void limb_update(sys::game_state& state, dcon::creature_id l, room_view const& room, int32_t n) {
            auto& w = state.world;
            auto& g = w.lizard_data_get_graphics(creature::lizard_data(w, l));
            auto& rng = w.creature_get_graphics_random(l);
            auto const& p = lizard::params_of(state, l);
            auto const& z = w.lizard_data_get_core(creature::lizard_data(w, l));
            auto& lb = g.limbs[n];
            auto& h = lb.h;
            lizard::limb* const other = n % 2 == 1 ? &g.limbs[n - 1] : nullptr;
            uint32_t const ci = lizard::breed_of(state, l) == breed::caramel ? uint32_t(n / 2) : (n < 2 ? 0u : 2u);
            vec2 const conn = chunk_pos(state, l, ci), conn_vel = chunk_vel(state, l, ci);
            vec2 const rot = chunk_pos(state, l, rotation_chunk(ci));
            float const health = limb_health(state, l, n);
            if ((!lb.currently_disabled && rng.value() > health && rng.value() < 0.02f) || stunned(state, l))
                lb.currently_disabled = true;
            else if ((lb.currently_disabled && rng.value() < health && rng.value() < 0.02f) || health == 1.f)
                lb.currently_disabled = false;
            h.hunt_speed = p.limb_speed * mathf::lerp(0.5f, 1.f, health);
            h.quickness = mathf::lerp(0.1f, p.limb_quickness, health);
            float const step_length = mathf::lerp(-0.5f, 0.5f, p.step_length * health);
            auto const sound = [&](bool grab) {
                // (the foot grab sounds are commented out of vanilla's sounds.txt)
                if (grab)
                    return;
                auto const b = lizard::breed_of(state, l);
                state.play_sound(b == breed::green ? sound::sound_id::lizard_green_foot_release
                    : p.wall_climber ? sound::sound_id::lizard_bluewhite_foot_release : sound::sound_id::lizard_pinkyellowred_foot_release,
                    room.id, h.pos);
            };
            if (lb.currently_disabled || z.swim > 0.5f) {
                h.mode = limb_mode::dangle;
                h.vel.y -= 0.9f;
                lb.retract = false;
                lb.grip_counter = 0;
                lb.reaching_for_terrain = false;
            } else {
                if (h.mode == limb_mode::hunt_absolute_position)
                    h.hunt_speed = p.limb_speed + rw::magnitude(conn_vel);
                vec2 v = custom::dir_vec(rot, conn);
                if (ci == 2)
                    v *= -1.f;
                v = rw::normalized(rw::lerp(v, custom::dir_vec(conn, z.limbs_aim_for), 0.4f));
                float const num = custom::distance_to_line(h.pos, conn, conn + custom::perpendicular_vector(v));
                if (!lb.reaching_for_terrain) {
                    h.mode = limb_mode::hunt_absolute_position;
                    h.absolute_hunt_pos = rw::lerp(h.pos, conn, p.lift_feet) + v * (lb.joint_dist + 1.f);
                    if (lb.extra_long_step && other) {
                        if (other->currently_disabled)
                            lb.extra_long_step = false;
                        if (other->grip_counter > p.limb_grip_delay)
                            lb.extra_long_step = false;
                    }
                    if (!lb.extra_long_step && num < lb.joint_dist * -step_length)
                        lb.reaching_for_terrain = true;
                } else {
                    if (!over_lapping_hunt_pos(lb)) {
                        v += custom::perpendicular_vector(v) * (6.f - 12.f * float(n % 2)) * 0.2f;
                        v.y -= 0.3f * p.feet_down;
                        v.x += (n % 2 == 0 ? -1.f : 1.f) * p.leg_pair_displacement * lb.flip;
                        // Limb.FindGrip (+ LizardLimb.GrabbedTerrain)
                        limb_mode const before = h.mode;
                        vec2 const before_pos = h.absolute_hunt_pos;
                        h.mode = limb_mode::dangle;
                        body_parts::find_grip(h, room, conn, conn, lb.joint_dist - 1.f, conn + v * 50.f, 2, 2, lizard::is_wall_climber(state, l));
                        if (h.mode == limb_mode::hunt_absolute_position) {
                            lb.grab_pos = h.absolute_hunt_pos;
                            lb.reaching_for_terrain = true;
                        } else {
                            h.mode = before;
                            h.absolute_hunt_pos = before_pos;
                        }
                    } else {
                        if (lb.sound_bool) {
                            sound(true);
                            lb.sound_bool = false;
                        }
                        if (num > lb.joint_dist * -0.5f * (p.step_length + 0.1f) && !custom::dist_less(h.pos, conn, lb.joint_dist - 1.f)
                        && !custom::dist_less(h.absolute_hunt_pos, conn, lb.joint_dist)) {
                            lb.extra_long_step = other && p.smoothen_leg_movement && other->grip_counter < 1;
                            lb.reaching_for_terrain = false;
                        }
                        if (w.lizard_data_get_ai(creature::lizard_data(w, l)).run_speed > 0.1f && p.smoothen_leg_movement) {
                            bool all = true;
                            for (int32_t i = 0; i < g.limb_count; ++i)
                                if (g.limbs[i].grip_counter > lb.grip_counter || g.limbs[i].grip_counter == 0) {
                                    all = false;
                                    break;
                                }
                            if (all)
                                lb.reaching_for_terrain = false;
                        }
                        if (!lb.reaching_for_terrain && !lb.sound_bool) {
                            sound(false);
                            lb.sound_bool = true;
                        }
                    }
                    if (h.mode == limb_mode::dangle)
                        lb.reaching_for_terrain = false;
                }
            }
            if (h.mode == limb_mode::dangle)
                h.vel += custom::dir_vec(h.pos, conn + body_parts::rotate_around_origo(vec2{30.f - 60.f * float(n % 2), 10.f}, custom::aim(rot, conn))) * 0.6f;
            if (lb.retract && h.reached_snap_position) {
                h.mode = limb_mode::hunt_absolute_position;
                lb.reaching_for_terrain = false;
                lb.retract = false;
            }
            if (other && custom::dist_less(h.pos, other->h.pos, limb_rad * 3.f)) {
                float const d = rw::distance(h.pos, other->h.pos);
                vec2 const a = custom::dir_vec(h.pos, other->h.pos);
                h.vel -= a * (limb_rad * 3.f - d) * 0.5f;
                other->h.vel += a * (limb_rad * 3.f - d) * 0.5f;
            }
            limb_base_update(lb, conn, conn_vel, rot, room, h.hunt_speed, h.quickness);
            body_parts::as_body_part(h, limb_rad, [&](player::body_part& bp) { body_parts::connect_to_point(bp, conn, lb.joint_dist, false, 0.f, conn_vel, 0.f, 0.f); });
            if (h.mode == limb_mode::hunt_absolute_position && lb.reaching_for_terrain && (h.reached_snap_position || (over_lapping_hunt_pos(lb) && lb.terrain_contact)))
                ++lb.grip_counter;
            else
                lb.grip_counter = 0;
        }

        // TailSegment.Update
        void tail_segment_update(lizard::graphics_state& g, int32_t i, vec2 connected_point, room_view const& room) {
            auto& t = g.tail[i];
            auto& bp = t.bp;
            bp.last_pos = bp.pos;
            bp.pos += bp.vel;
            bp.vel *= 1.f; // airFriction 1
            bp.stretched = 1.f;
            constexpr float affect_previous = 0.4f;
            vec2 const target = i > 0 ? g.tail[i - 1].bp.pos : connected_point;
            if (!custom::dist_less(bp.pos, target, t.connection_rad)) {
                float const d = rw::distance(bp.pos, target);
                vec2 const a = custom::dir_vec(bp.pos, target);
                if (i > 0) {
                    bp.pos -= a * (t.connection_rad - d) * (1.f - affect_previous);
                    bp.vel -= a * (t.connection_rad - d) * (1.f - affect_previous);
                    g.tail[i - 1].bp.vel += a * (t.connection_rad - d) * affect_previous;
                } else {
                    bp.pos -= a * (t.connection_rad - d);
                    bp.vel -= a * (t.connection_rad - d);
                }
                bp.stretched = mathf::clamp((t.connection_rad / (d * 0.5f) + 2.f) / 3.f, 0.2f, 1.f);
            }
            t.terrain_contact = body_parts::push_out_of_terrain(bp, room, 0.85f);
        }

        // LizardGraphics.UpdateTailSegment
        void update_tail_segment(sys::game_state& state, dcon::creature_id l, room_view const& room, int32_t i, float stiffness) {
            auto& w = state.world;
            auto& g = w.lizard_data_get_graphics(creature::lizard_data(w, l));
            auto const& p = lizard::params_of(state, l);
            auto const& z = w.lizard_data_get_core(creature::lizard_data(w, l));
            int32_t const n = p.tail_segments;
            tail_segment_update(g, i, g.draw_positions[2][0], room);
            auto& bp = g.tail[i].bp;
            if (room.point_submerged(bp.pos))
                bp.vel *= 0.8f;
            else
                bp.vel.y -= 0.9f * mathf::pow(float(i) / float(n - 1), 3.f);
            if (!custom::dist_less(bp.pos, g.draw_positions[2][0], 15.f * float(i + 1)))
                bp.pos = chunk_pos(state, l, 2) + custom::dir_vec(g.draw_positions[2][0], bp.pos) * 15.f * float(i + 1);
            vec2 v = g.draw_positions[1][0];
            if (i == 1)
                v = g.draw_positions[2][0];
            else if (i > 1)
                v = g.tail[i - 2].bp.pos;
            v = rw::lerp(v, g.draw_positions[1][0], 0.2f);
            bp.vel += custom::dir_vec(v, bp.pos) * stiffness * mathf::pow(p.tail_stiffness_decline, float(i)) / rw::distance(v, bp.pos);
            if (i == 0)
                bp.vel += g.tail_direction;
            if (lizard::consious(state, l) && z.swim > 0.5f) {
                if (!p.swimmer) {
                    bp.vel *= 1.f - 0.5f * mathf::inverse_lerp(0.5f, 1.f, z.swim);
                    bp.vel += (custom::dir_vec(g.draw_positions[1][0], g.draw_positions[2][0])
                        + custom::perpendicular_vector(rw::normalized(bp.pos - v)) * std::sin((g.breath * 5.f - float(i) / float(n)) * 0.3f * mathf::pi))
                        * mathf::inverse_lerp(0.5f, 1.f, z.swim) * mathf::pow(0.85f, float(i)) * 7.f;
                    return;
                }
                if (!z.salamander_lurk) {
                    bp.vel += (custom::dir_vec(g.draw_positions[1][0], g.draw_positions[2][0]) * 0.2f
                        + custom::perpendicular_vector(rw::normalized(bp.pos - v)) * std::sin((g.breath * 7.f - float(i) / float(n)) * 1.5f * mathf::pi))
                        * mathf::inverse_lerp(0.5f, 1.f, z.swim) * mathf::pow(0.85f, float(i)) * 4.f;
                    return;
                }
            } else if (g.show_dominance > 0.f) {
                auto& rng = state.world.creature_get_graphics_random(l);
                bp.vel += custom::rnv(rng) * g.show_dominance * rng.value() * 2.f;
            }
        }

        // LizardGraphics.effectColor
        vec3 effect_color(sys::game_state const& state, dcon::creature_id l, palette const& pal) {
            return lizard::breed_of(state, l) == breed::black ? pal.black : state.world.creature_get_payload(l).lizard.effect_color;
        }

        // ApplyPalette's own blackColor (the Blizzard's and Indigo's)
        palette breed_palette(sys::game_state const& state, dcon::creature_id l, palette pal) {
            auto const b = lizard::breed_of(state, l);
            if (b == breed::blizzard) {
                pal.black = vec3{0.8f, 0.81f, 0.84f};
            } else if (b == breed::indigo) {
                vec3 const hsl = custom::rgb2hsl(state.world.creature_get_payload(l).lizard.effect_color);
                pal.black = mix(custom::hsl2rgb(hsl.x, hsl.y, 0.4f), pal.black, 0.95f);
            }
            return pal;
        }

        // the White lizard's whiteCamoColor: what it picked up from behind,
        // tinted by its dominance display and glitches
        vec3 white_camo_color(lizard::graphics_state const& g, palette const& pal) {
            return mix(pal.pick_up, g.white_tint, g.white_tint_amount);
        }
        float white_camo_amount(lizard::graphics_state const& g) {
            return g.white_camo_color_amount == -1.f ? 1.f : g.white_camo_color_amount;
        }

        // LizardGraphics.SalamanderColor
        vec3 salamander_color(sys::game_state const& state, dcon::creature_id l, palette const& pal) {
            if (state.world.lizard_data_get_graphics(creature::lizard_data(state.world, l)).black_salamander)
                return mix(pal.black, effect_color(state, l, pal), 0.1f);
            return mix(vec3{0.9f, 0.9f, 0.95f}, effect_color(state, l, pal), 0.06f);
        }

        // LizardGraphics.DynamicBodyColor
        vec3 dynamic_body_color(sys::game_state const& state, dcon::creature_id l, palette const& pal) {
            auto const& g = state.world.lizard_data_get_graphics(creature::lizard_data(state.world, l));
            auto const b = lizard::breed_of(state, l);
            if (b == breed::white)
                return mix(vec3{1.f, 1.f, 1.f}, white_camo_color(g, pal), white_camo_amount(g));
            if (b == breed::salamander)
                return salamander_color(state, l, pal);
            // (the Zoop's SnowAccumulation is a cosmetic)
            return pal.black;
        }

        vec3 head_color_impl(sys::game_state const& state, dcon::creature_id l, float alpha, palette const& pal);

        // the colour ApplyPalette and DrawSprites give the body (ColorBody)
        vec3 body_sprite_color(sys::game_state const& state, dcon::creature_id l, float alpha, palette const& pal) {
            auto const b = lizard::breed_of(state, l);
            if (b == breed::white || b == breed::zoop)
                return b == breed::white ? dynamic_body_color(state, l, pal) : state.world.lizard_data_get_graphics(creature::lizard_data(state.world, l)).ivar_body_color;
            if (b == breed::basilisk)
                return head_color(state, l, alpha, pal);
            if (b == breed::caramel)
                return state.world.lizard_data_get_graphics(creature::lizard_data(state.world, l)).ivar_body_color;
            if (b == breed::salamander)
                return salamander_color(state, l, pal);
            return pal.black;
        }

        // GenerateIvars
        lizard::individual_variations generate_ivars(breed b, rw::rng& rng) {
            lizard::individual_variations iv;
            iv.head_size = custom::clamped_random_variation(0.5f, 0.07f, 0.5f, rng) * 2.f;
            if (rng.value() < 0.5f)
                iv.head_size = 1.f;
            iv.fatness = custom::clamped_random_variation(0.5f, 0.12f, 0.5f, rng) * 2.f;
            iv.tail_length = custom::clamped_random_variation(0.5f, 0.2f, 0.3f, rng) * 2.f;
            iv.tail_fatness = custom::clamped_random_variation(0.45f, 0.1f, 0.3f, rng) * 2.f;
            iv.tail_color = 0.f;
            if (b != breed::white && rng.value() > 0.5f)
                iv.tail_color = rng.value();
            if (b == breed::red) {
                iv.fatness = mathf::min(1.f, iv.fatness);
                iv.tail_fatness = mathf::min(1.f, iv.tail_fatness);
            } else if (b == breed::black) {
                iv.fatness = custom::clamped_random_variation(0.45f, 0.06f, 0.5f, rng) * 2.f;
            } else if (b == breed::caramel || b == breed::zoop) {
                iv.fatness = mathf::min(0.8f, iv.fatness);
                iv.tail_fatness = mathf::min(0.9f, iv.tail_fatness);
            }
            return iv;
        }

        // UnityEngine.Random.Range(float, float)
        float range(rw::rng& rng, float a, float b) {
            return mathf::lerp(a, b, rng.value());
        }

        // LizardGraphics.BodyPosition
        vec2 body_position(lizard::graphics_state const& g, int32_t p, float alpha) {
            if (p > 3)
                return vec2{100.f, 100.f};
            auto const dp = [&](int32_t i) { return rw::lerp(g.draw_positions[i][1], g.draw_positions[i][0], alpha); };
            int32_t const num = p < 2 ? 1 : 2;
            if (p % 2 == 1)
                return dp(num);
            vec2 const v = dp(num);
            vec2 const a = num == 1 ? dp(2) : rw::lerp(g.tail[0].bp.last_pos, g.tail[0].bp.pos, alpha);
            vec2 const b = dp(num - 1);
            vec2 d = rw::normalized(rw::slerp(v - b, a - v, 0.35f));
            d *= -0.5f * rw::distance(v, b);
            return v + d;
        }

        // LizardGraphics.HeadColor, with the breed's palette
        vec3 head_color_impl(sys::game_state const& state, dcon::creature_id l, float alpha, palette const& pal) {
            auto const& g = state.world.lizard_data_get_graphics(creature::lizard_data(state.world, l));
            auto const& a = state.world.lizard_data_get_ai(creature::lizard_data(state.world, l));
            auto const b = lizard::breed_of(state, l);
            if (g.white_flicker > 0 && (g.white_flicker > 15 || g.every_second_draw))
                return vec3{1.f, 1.f, 1.f};
            float num = 1.f - mathf::pow(0.5f + 0.5f * std::sin(mathf::lerp(g.last_blink, g.blink, alpha) * 2.f * mathf::pi), 1.5f + a.excitement * 1.5f);
            if (g.head_color_setter != 0.f)
                num = mathf::lerp(num, g.head_color_setter > 0.f ? 1.f : 0.f, std::fabs(g.head_color_setter));
            if (g.flicker > 10)
                num = g.flicker_color;
            num = mathf::lerp(num, mathf::pow(mathf::max(0.f, mathf::lerp(g.last_voice_visualization, g.voice_visualization, alpha)), 0.75f),
                mathf::lerp(g.last_voice_visualization_intensity, g.voice_visualization_intensity, alpha));
            vec3 const effect = effect_color(state, l, pal);
            // HeadColor1 / HeadColor2
            vec3 c1, c2;
            if (b == breed::white) {
                float const amount = white_camo_amount(g);
                c1 = mix(vec3{1.f, 1.f, 1.f}, white_camo_color(g, pal), amount);
                c2 = mix(pal.black, white_camo_color(g, pal), amount);
            } else if (head_lights_up_from_noise(b)) {
                c1 = c2 = mix(pal.black, vec3{0.5f, 0.5f, 0.5f}, g.black_lizard_light_up_head);
            } else if (b == breed::salamander) {
                c1 = c2 = salamander_color(state, l, pal);
            } else {
                c1 = pal.black;
                c2 = effect;
            }
            return mix(c1, c2, num);
        }

        // LizardGraphics.BodyColor, with the breed's palette
        vec3 body_color_impl(sys::game_state const& state, dcon::creature_id l, float f, palette const& pal) {
            auto const& g = state.world.lizard_data_get_graphics(creature::lizard_data(state.world, l));
            auto const b = lizard::breed_of(state, l);
            auto const& p = lizard::params_of(state, l);
            float const body = g.body_length / (g.body_length + g.tail_length);
            bool const ivar = b == breed::caramel || b == breed::zoop;
            if (ivar && (f < body || g.ivars.tail_color == 0.f))
                return g.ivar_body_color;
            if (b == breed::white)
                return dynamic_body_color(state, l, pal);
            if (b == breed::salamander)
                return salamander_color(state, l, pal);
            if (f < body || g.ivars.tail_color == 0.f)
                return pal.black;
            float num = mathf::clamp(mathf::inverse_lerp(p.tail_coloration_start, 0.95f, mathf::inverse_lerp(body, 1.f, f)), 0.f, 1.f);
            num = mathf::pow(num, p.tail_coloration_exponent) * g.ivars.tail_color;
            return mix(ivar ? g.ivar_body_color : pal.black, effect_color(state, l, pal), num);
        }
    }

    vec3 head_color(sys::game_state const& state, dcon::creature_id l, float alpha, palette const& pal) {
        return head_color_impl(state, l, alpha, breed_palette(state, l, pal));
    }

    vec3 body_color(sys::game_state const& state, dcon::creature_id l, float f, palette const& pal) {
        return body_color_impl(state, l, f, breed_palette(state, l, pal));
    }

    float head_rotation(sys::game_state const& state, dcon::creature_id l, float alpha) {
        auto const& g = state.world.lizard_data_get_graphics(creature::lizard_data(state.world, l));
        auto const& z = state.world.lizard_data_get_core(creature::lizard_data(state.world, l));
        auto const& p = lizard::params_of(state, l);
        float const num = custom::aim(rw::lerp(g.draw_positions[0][1], g.draw_positions[0][0], alpha), rw::lerp(g.head.last_pos, g.head.pos, alpha));
        float const depth = mathf::lerp(g.last_head_depth_rotation, g.head_depth_rotation, alpha);
        float const jaw = mathf::clamp(mathf::lerp(z.last_jaw_open, lizard::jaw_open(state, l), alpha), 0.f, 1.f);
        return num + p.jaw_open_angle * (1.f - p.jaw_open_lower_jaw_fac) * jaw * depth;
    }

    void stun(sys::game_state& state, dcon::creature_id l, int32_t st) {
        state.world.lizard_data_get_graphics(creature::lizard_data(state.world, l)).flicker = custom::int_clamp(st / 3, 3, 15);
    }

    void white_flicker(sys::game_state& state, dcon::creature_id l, int32_t fl) {
        auto& g = state.world.lizard_data_get_graphics(creature::lizard_data(state.world, l));
        if (fl > g.white_flicker)
            g.white_flicker = fl;
        g.every_second_draw = false;
    }

    void creature_spotted(sys::game_state& state, dcon::creature_id l, bool first_spot, dcon::creature_id t) {
        auto& g = state.world.lizard_data_get_graphics(creature::lizard_data(state.world, l));
        if (head_lights_up_from_noise(lizard::breed_of(state, l)))
            g.black_lizard_light_up_head = mathf::min(g.black_lizard_light_up_head + 0.5f, 1.f);
        // CreatureLooker.ReevaluateLookObject: the one just spotted
        (void)first_spot;
        g.look_creature = t;
    }

    void reset_positions(sys::game_state& state, dcon::creature_id l) {
        auto& g = state.world.lizard_data_get_graphics(creature::lizard_data(state.world, l));
        auto const& p = lizard::params_of(state, l);
        for (uint32_t i = 0; i < 3; ++i)
            g.draw_positions[i][0] = g.draw_positions[i][1] = chunk_pos(state, l, i);
        for (int32_t j = 0; j < p.tail_segments && j < lizard::max_tail; ++j)
            reset_part(g.tail[j].bp, chunk_pos(state, l, 1));
        for (int32_t k = 0; k < g.limb_count; ++k) {
            // LizardLimb.Reset
            g.limbs[k].retract = true;
            auto& h = g.limbs[k].h;
            h.pos = h.last_pos = chunk_pos(state, l, 1);
            h.vel = vec2{0.f, 0.f};
        }
        // cosmetics: Template.Reset
        lizard_cosmetics::reset(state, l);
        reset_part(g.head, chunk_pos(state, l, 0));
        for (int32_t i = 0; i < p.tongue_segments && i < lizard::max_tongue; ++i)
            reset_part(g.tongue[i], chunk_pos(state, l, 0));
    }

    void reset(sys::game_state& state, dcon::creature_id l) {
        auto& w = state.world;
        auto& g = w.lizard_data_get_graphics(creature::lizard_data(w, l));
        g = lizard::graphics_state{};
        // vanilla seeds these from the creature's ID
        w.creature_get_graphics_random(l) = rw::rng{w.creature_get_random(l).state ^ 0x6C1Au};
        auto& rng = w.creature_get_graphics_random(l);
        auto const& p = lizard::params_of(state, l);
        auto const b = lizard::breed_of(state, l);
        g.ivars = generate_ivars(b, rng);
        g.limb_count = b == breed::caramel ? 6 : 4;
        float const joint = 25.f * ((p.body_size_fac + 1.f) / 2.f);
        for (int32_t i = 0; i < g.limb_count; ++i) {
            auto& lb = g.limbs[i];
            lb.h.hunt_speed = p.limb_speed;
            lb.h.quickness = p.limb_quickness;
            lb.h.mode = limb_mode::retracted;
            lb.joint_dist = joint;
            uint32_t const ci = b == breed::caramel ? uint32_t(i / 2) : (i < 2 ? 0u : 2u);
            lb.h.pos = lb.h.last_pos = lb.h.absolute_hunt_pos = chunk_pos(state, l, ci);
        }
        int32_t const n = std::min(p.tail_segments, lizard::max_tail);
        for (int32_t j = 0; j < n; ++j) {
            float rad = 8.f * p.body_size_fac;
            rad *= float(p.tail_segments - j) / float(p.tail_segments);
            float conn = ((j > 0 ? 8.f : 16.f) + rad) / 2.f;
            conn *= p.tail_length_factor * g.ivars.tail_length;
            g.tail[j].bp.rad = rad;
            g.tail[j].connection_rad = conn;
        }
        if (b == breed::cyan) {
            for (int32_t k = 0; k < n; ++k) {
                float const t = mathf::inverse_lerp(0.f, float(p.tail_segments - 1), float(k));
                g.tail[k].bp.rad += std::sin(mathf::pow(t, 0.7f) * mathf::pi) * 2.5f;
                g.tail[k].bp.rad *= 1.f - std::sin(mathf::inverse_lerp(0.f, 0.4f, t) * mathf::pi) * 0.5f;
            }
        } else if (b == breed::indigo) {
            for (int32_t k = 0; k < n; ++k) {
                float const t = mathf::inverse_lerp(0.f, float(p.tail_segments - 1), float(k));
                g.tail[k].bp.rad += std::sin(mathf::pow(t, 0.7f) * mathf::pi) * 4.f * (1.f - 0.2f * t);
            }
        }
        g.tail_length = 0.f;
        for (int32_t m = 0; m < n; ++m)
            g.tail_length += g.tail[m].connection_rad;
        for (auto& t : g.tongue) {
            t.rad = 1.f;
        }
        g.head.rad = 6.f * p.head_size;
        g.head_connection_rad = 11.f * p.head_size;
        float const link = 17.f * p.body_length_fac * ((p.body_size_fac + 1.f) / 2.f);
        g.body_length = g.head_connection_rad * 0.5f + link * 2.f + link * (1.f + p.body_stiffnes);
        // (no region here: the chance without one)
        g.black_salamander = rng.value() < 0.33333334f;
        // cosmetics: the constructor's AddCosmetic calls (and their random
        // rolls), before the blink and breath
        lizard_cosmetics::add_cosmetics(state, l);
        g.blink = rng.value();
        g.last_blink = g.blink;
        g.last_breath = g.breath;
        g.breath = rng.value();
        // C# draws left to right: the angle first, then the length
        float const tail_angle = rng.value() * 360.f;
        g.tail_direction = custom::deg_to_vec(tail_angle) * rng.value();
        g.override_head_graphic = -1;
        if (b == breed::caramel)
            g.override_head_graphic = rng.value() < 0.25f ? 3 : 4;
        if (b == breed::caramel) {
            float const light = range(rng, 0.7f, 1.f);
            if (light >= 0.8f) {
                g.ivar_body_color = custom::hsl2rgb(range(rng, 0.075f, 0.125f), range(rng, 0.4f, 0.9f), light);
                w.creature_get_payload(l).lizard.effect_color = custom::hsl2rgb(custom::wrapped_random_variation(0.1f, 0.03f, 0.2f, rng), 0.55f,
                    custom::clamped_random_variation(0.55f, 0.05f, 0.2f, rng));
            } else {
                g.ivar_body_color = custom::hsl2rgb(range(rng, 0.075f, 0.125f), range(rng, 0.3f, 0.5f), light);
            }
        } else if (b == breed::zoop) {
            g.ivar_body_color = vec3{1.f, 1.f, 1.f};
        }
        g.look_pos = chunk_pos(state, l, 0);
        reset_positions(state, l);
    }

    void update(sys::game_state& state, dcon::creature_id l, room_view const& room) {
        auto& w = state.world;
        auto& g = w.lizard_data_get_graphics(creature::lizard_data(w, l));
        auto& rng = w.creature_get_graphics_random(l);
        auto const& p = lizard::params_of(state, l);
        auto const& z = w.lizard_data_get_core(creature::lizard_data(w, l));
        auto const& a = w.lizard_data_get_ai(creature::lizard_data(w, l));
        auto const b = lizard::breed_of(state, l);
        bool const awake = lizard::consious(state, l);
        bool const dead = w.creature_get_dead(l) != 0;
        float const health = w.creature_get_health(l);

        // the voice's visualisation: CreatureVoice.MakingASound and its
        // VisualizedIntensity, from the articulation's volume (the sample's
        // own loudness isn't measured: a sine envelope stands in)
        g.last_voice_visualization_intensity = g.voice_visualization_intensity;
        g.last_voice_visualization = g.voice_visualization;
        float const volume = z.voice_index >= 0 ? std::sin(mathf::clamp01(z.voice_progression) * mathf::pi) * custom::lerp_map(z.voice_intensity, 0.f, 2.f, 0.4f, 3.f) : 0.f;
        g.voice_visualization_intensity = volume > 0.f ? 1.f : 0.f;
        g.voice_visualization = mathf::clamp01(volume * 0.5f);

        // soundLoop (ChunkDynamicSoundLoop on the main chunk): the lunge, else
        // the belly dragging along walls and floors. Neither sound is in
        // sounds.txt, so vanilla never plays them either: wired for mods
        // that add them
        {
            auto const key = sound::loop_key(sound::loop_owner::creature, uint32_t(l.index()));
            vec2 const at = chunk_pos(state, l, 0);
            if (z.anim == lizard::animation::lounge) {
                state.play_loop(key, sound::sound_id::lizard_lounge_attack_loop, room.id, at, 1.f);
            } else {
                float num = 0.f, num2 = 0.f, num3 = 0.f;
                for (uint32_t i = 0; i < 3; ++i) {
                    auto const k = lizard::chunk(state, l, i);
                    vec2 const pos = w.body_chunk_get_pos(k);
                    vec2 const vel = w.body_chunk_get_vel(k);
                    ivec2 const contact = w.body_chunk_get_contact_point(k);
                    float const r = display_rad(state, l, i) + 6.f;
                    if (contact.x != 0 || room.get_tile(pos - vec2{r, 0.f}).solid() || room.get_tile(pos + vec2{r, 0.f}).solid()) {
                        num += (contact.x != 0 ? 1.f : 0.5f) / 3.f;
                        num2 += std::fabs(vel.y) / 3.f;
                    }
                    if (contact.y != 0 || room.get_tile(pos - vec2{0.f, r}).solid() || room.get_tile(pos + vec2{0.f, r}).solid()) {
                        num += (contact.y != 0 ? 1.f : 0.5f) / 3.f;
                        num3 += std::fabs(vel.x) / 3.f;
                    }
                }
                int32_t const tail = std::min(p.tail_segments, lizard::max_tail);
                for (int32_t j = 0; j < tail && num <= 1.f; ++j)
                    if (g.tail[j].terrain_contact)
                        num += 0.5f / float(tail);
                g.belly_drag_volume = mathf::lerp(g.belly_drag_volume, mathf::clamp(num, 0.f, 1.f), 0.2f);
                float const speed = mathf::max(num2, num3);
                state.play_loop(key, sound::sound_id::lizard_belly_drag_loop, room.id, at,
                    g.belly_drag_volume * mathf::inverse_lerp(0.2f, 2.f, speed),
                    mathf::lerp(0.3f, 2.2f, mathf::lerp(mathf::inverse_lerp(0.2f, 8.f, speed), 0.5f, 0.3f)));
            }
        }
        g.show_dominance = mathf::clamp(g.show_dominance - 1.f / mathf::lerp(60.f, 120.f, rng.value()), 0.f, 1.f);
        if (b == breed::white) {
            if (dead) {
                g.white_camo_color_amount = mathf::lerp(white_camo_amount(g), 0.3f, 0.01f);
            } else {
                if (health < 0.6f && rng.value() * 1.5f < health && rng.value() < 1.f / (stunned(state, l) ? 10.f : 40.f))
                    g.white_glitch_fit = int32_t(mathf::lerp(5.f, 40.f, (1.f - health) * rng.value()));
                if (g.white_glitch_fit == 0 && stunned(state, l) && rng.value() < 0.05f)
                    g.white_glitch_fit = 2;
                if (g.white_glitch_fit > 0) {
                    --g.white_glitch_fit;
                    float const f = 1.f - health;
                    if (rng.value() < 0.2f)
                        g.white_camo_color_amount_drag = 1.f;
                    if (rng.value() < 0.2f)
                        g.white_camo_color_amount = 1.f;
                    if (rng.value() < 0.5f) {
                        vec3 const c{rng.value(), rng.value(), rng.value()};
                        float const t = mathf::pow(f, 0.2f) * mathf::pow(rng.value(), 0.1f);
                        g.white_tint = mix(g.white_tint, c, t);
                        g.white_tint_amount = mathf::lerp(g.white_tint_amount, 1.f, t);
                    }
                } else if (g.show_dominance > 0.f) {
                    g.white_dominance_hue += rng.value() * mathf::pow(g.show_dominance, 2.f) * 0.2f;
                    if (g.white_dominance_hue > 1.f)
                        g.white_dominance_hue -= 1.f;
                    float const t = mathf::inverse_lerp(0.5f, 1.f, mathf::pow(g.show_dominance, 0.5f)) * rng.value();
                    g.white_tint = mix(g.white_tint, custom::hsl2rgb(g.white_dominance_hue, 1.f, 0.5f), t);
                    g.white_tint_amount = mathf::lerp(g.white_tint_amount, 1.f, t);
                    g.white_camo_color_amount = mathf::lerp(white_camo_amount(g), 1.f - std::sin(mathf::inverse_lerp(0.f, 1.1f, mathf::pow(g.show_dominance, 0.5f)) * mathf::pi), 0.1f);
                } else {
                    if (z.anim == lizard::animation::shoot_tongue || z.anim == lizard::animation::prepare_to_lounge || z.anim == lizard::animation::lounge)
                        g.white_camo_color_amount_drag = 0.f;
                    else if (rng.value() < 0.1f)
                        // CamoAmountControlled
                        g.white_camo_color_amount_drag = mathf::lerp(g.white_camo_color_amount_drag, mathf::inverse_lerp(0.65f, 0.4f, a.run_speed), rng.value());
                    g.white_camo_color_amount = mathf::clamp(mathf::lerp(white_camo_amount(g), g.white_camo_color_amount_drag, 0.1f * rng.value()), 0.15f, 1.f);
                    // back to the picked up colour (Color.Lerp(.., whitePickUpColor, 0.1))
                    g.white_tint_amount = mathf::lerp(g.white_tint_amount, 0.f, 0.1f);
                }
            }
        } else if (head_lights_up_from_noise(b)) {
            if (z.bubble > 0)
                g.black_lizard_light_up_head = mathf::min(g.black_lizard_light_up_head + 0.1f, 1.f);
            else
                g.black_lizard_light_up_head *= 0.9f;
        }
        // cosmetics: Template.Update for each cosmetic
        lizard_cosmetics::update(state, l, room);
        if (g.white_flicker > 0)
            --g.white_flicker;

        // CreatureLooker (chance to look at nothing 0.3): what it focuses on,
        // else now and then something it tracks (its interest scores aren't
        // ported: one at random) or nothing
        auto const& ts = tracker::of(state, l);
        if (g.look_counter > 0)
            --g.look_counter;
        if (auto const f = lizard::ai::focus(state, l)) {
            g.look_creature = f;
        } else if (g.look_counter < 1 && rng.value() < 1.f / float(p.frames_between_look_focus_change)) {
            g.look_counter = p.frames_between_look_focus_change;
            if (rng.value() > 0.3f) {
                g.look_creature = dcon::creature_id{};
                if (ts.count > 0)
                    if (auto const t = ts.reps[rng.range(0, ts.count)].who; t != lizard::grasped(state, l))
                        g.look_creature = t;
            } else {
                g.look_creature = dcon::creature_id{};
                // LookAtNothing
                float const look_angle = rng.value() * 360.f;
                g.look_pos = g.draw_positions[0][0] + custom::dir_vec(g.draw_positions[1][0], g.draw_positions[0][0]) * 100.f
                    + custom::deg_to_vec(look_angle) * 100.f * rng.value();
            }
        }
        if (z.anim == lizard::animation::prey_spotted || z.anim == lizard::animation::prey_re_spotted)
            if (auto const t = lizard::ai::tracked(state, l))
                g.look_creature = t;
        auto const* look = g.look_creature ? tracker::find(ts, g.look_creature) : nullptr;
        if (!look)
            g.look_creature = dcon::creature_id{};
        if (look) {
            vec2 const target = look->visual && creature::here(state, look->who, room.id) ? creature::main_pos(state, look->who) : look->last_seen;
            g.look_pos = custom::move_towards(g.look_pos, target, mathf::lerp(100.f, 20.f, p.neck_stiffness));
        }
        // (the eyes only aim lizardLaserEyes, a debug setup value: not ported)

        if (z.anim == lizard::animation::prepare_to_lounge) {
            g.head_color_setter = mathf::clamp(g.head_color_setter - 1.f / (float(p.pre_lounge_crouch) / 2.f), -1.f, 1.f);
        } else if (z.anim == lizard::animation::lounge) {
            g.head_color_setter = 1.f;
        } else if (z.anim != lizard::animation::threat_spotted && z.anim != lizard::animation::threat_re_spotted) {
            g.head_color_setter = mathf::lerp(g.head_color_setter, 0.f, 0.2f);
        }
        if (z.post_lounge_stun > 0)
            g.head_color_setter = mathf::lerp(g.head_color_setter, -1.f, 0.8f);
        if (stunned(state, l) && !g.stunned_last_update)
            g.rotate_while_stunned = true;
        g.stunned_last_update = stunned(state, l);
        for (uint32_t n = 0; n < 3; ++n) {
            g.draw_positions[n][1] = g.draw_positions[n][0];
            g.draw_positions[n][0] = chunk_pos(state, l, n);
        }
        float const bob = (4.f + 7.f / p.walk_bob) / 2.f;
        g.front_bob = (g.front_bob * bob + float(g.front_legs_grabbing - 1)) / (bob + 1.f);
        g.hind_bob = (g.hind_bob * bob + float(g.hind_legs_grabbing - 1)) / (bob + 1.f);
        g.draw_positions[0][0].y += g.front_bob * p.walk_bob;
        g.draw_positions[1][0].y += g.front_bob + g.hind_bob * p.walk_bob * 0.5f;
        g.draw_positions[2][0].y += g.hind_bob * p.walk_bob;
        if (z.anim == lizard::animation::shoot_tongue) {
            float const d = z.time_in_animation < z.time_to_remain_in_animation / 2 - 2 ? 2.f : -1.f;
            float const k = float(z.time_in_animation) / float(std::max(1, p.tongue_warm_up)) * 10.f * d;
            g.draw_positions[0][0] += custom::dir_vec(g.draw_positions[0][0], chunk_pos(state, l, 2)) * k;
            g.draw_positions[2][0] += custom::dir_vec(g.draw_positions[2][0], chunk_pos(state, l, 0)) * k;
        }
        if (g.flicker > 0) {
            --g.flicker;
            g.blink = rng.value();
            g.last_blink = rng.value();
        } else {
            g.last_blink = g.blink;
            if (!awake)
                g.blink = mathf::lerp(g.blink - std::floor(g.blink), 0.25f, 0.02f);
            else
                g.blink += mathf::lerp(0.0025f, 0.07f, mathf::max(a.excitement, g.show_dominance)) + rng.value() * 0.001f;
        }
        g.last_breath = g.breath;
        if (!dead)
            g.breath += 0.0125f;
        if (awake && rng.value() < 0.05f) {
            float const tail_angle = rng.value() * 360.f;
            g.tail_direction = custom::deg_to_vec(tail_angle) * rng.value();
        }

        // the legs
        float side = 0.f;
        g.legs_grabbing = 0;
        g.front_legs_grabbing = 0;
        g.hind_legs_grabbing = 0;
        bool none = true;
        for (int32_t i = 0; i < g.limb_count; ++i) {
            auto& lb = g.limbs[i];
            if (awake && z.swim > 0.5f) {
                if (p.swimmer) {
                    if (!z.salamander_lurk) {
                        vec2 const fwd = custom::dir_vec(g.draw_positions[2][0], g.draw_positions[0][0]);
                        vec2 const perp = custom::perpendicular_vector(fwd);
                        float const t = float(i) / float(g.limb_count) + g.breath * 1.5f;
                        if (b == breed::peach) {
                            lb.h.vel -= fwd * 3.f + perp * std::sin(t * mathf::pi * 2.f) * 1.3f;
                        } else {
                            lb.h.vel += fwd * std::sin(t * mathf::pi * 2.f) * 3.f;
                            if (std::sin(t * mathf::pi * 2.f) > 0.f) {
                                lb.h.vel *= 0.7f;
                                lb.h.vel += (custom::closest_point_on_line(g.draw_positions[1][0] - fwd, g.draw_positions[1][0] + fwd, lb.h.pos)
                                    + perp * (i % 2 == 0 ? -5.f : 5.f) - lb.h.pos) / 5.f;
                            } else {
                                lb.h.vel += perp * (0.5f + 0.5f * std::cos(t * mathf::pi * 2.f)) * 1.5f * (i % 2 == 0 ? -1.f : 1.f);
                            }
                        }
                    }
                } else {
                    lb.h.vel.x += std::sin((float(i) / float(g.limb_count) * (p.smoothen_leg_movement ? 1.f : 0.1f) + g.breath * 2.f) * mathf::pi * 2.f) * z.swim * 0.5f;
                }
            }
            limb_update(state, l, room, i);
            if (lb.grip_counter >= p.limb_grip_delay) {
                ++g.legs_grabbing;
                if (i < 2)
                    ++g.front_legs_grabbing;
                else if (i >= g.limb_count - 2)
                    ++g.hind_legs_grabbing;
            }
            if (lb.grip_counter > 0)
                none = false;
            uint32_t const ci = b == breed::caramel ? uint32_t(i / 2) : (i < 2 ? 0u : 2u);
            float d = custom::distance_to_line(lb.h.pos, chunk_pos(state, l, ci), chunk_pos(state, l, rotation_chunk(ci)));
            d *= i > 1 ? 1.f : -1.f;
            if (std::fabs(d) > 10.f)
                side += mathf::sign(d);
            lb.flip = mathf::lerp(lb.flip, d < 0.f ? 1.f : -1.f, 0.3f);
        }
        side = mathf::clamp(side, -1.f, 1.f);
        if (awake)
            side = mathf::lerp(side, g.draw_positions[0][0].x > g.draw_positions[1][0].x ? -1.f : 1.f, z.swim);
        g.last_depth_rotation = g.depth_rotation;
        if (!stunned(state, l) || g.rotate_while_stunned)
            g.depth_rotation = mathf::lerp(g.depth_rotation, side, 0.1f);
        g.last_head_depth_rotation = g.head_depth_rotation;
        float facing = rw::dot(rw::normalized(g.look_pos - g.draw_positions[0][0]), rw::normalized(g.head.pos - g.draw_positions[0][0]));
        facing = mathf::inverse_lerp(0.f, 0.6f, std::fabs(facing));
        g.head_depth_rotation = mathf::lerp(g.head_depth_rotation, g.depth_rotation * facing, 0.5f);
        g.no_grip_counter = none ? g.no_grip_counter + 1 : 0;

        // the tail
        int32_t const tn = std::min(p.tail_segments, lizard::max_tail);
        float const stiffness = mathf::lerp(p.tail_stiffness, 900.f, g.show_dominance);
        if (awake && z.swim > 0.5f && !z.salamander_lurk) {
            for (int32_t i = 0; i < tn; ++i)
                update_tail_segment(state, l, room, i, stiffness);
            if (p.swimmer) {
                vec2 const perp = custom::perpendicular_vector(custom::dir_vec(chunk_pos(state, l, 2), chunk_pos(state, l, 0)));
                g.draw_positions[0][0] += perp * std::sin(g.breath * mathf::pi * 2.f * 3.f) * 3.5f * z.swim;
                g.draw_positions[1][0] += perp * std::sin((g.breath + 0.4f) * mathf::pi * 2.f * 3.f) * 3.f * z.swim;
                g.draw_positions[2][0] += perp * std::sin((g.breath + 0.8f) * mathf::pi * 2.f * 3.f) * 2.f * z.swim;
            }
        } else {
            for (int32_t i = tn - 1; i >= 0; --i)
                update_tail_segment(state, l, room, i, stiffness);
        }

        // the head
        vec2 const head_move = g.head.pos - g.head.last_pos;
        body_parts::generic_update(g.head, room, 0.99f, 0.5f);
        if (auto const held = lizard::grasped(state, l); held && creature::live(state, held)) {
            g.head.pos = w.body_chunk_get_pos(creature::chunk_of(state, held, creature::grasp_chunk(w, l, 0)));
        } else if (awake) {
            vec2 v = custom::dir_vec(g.draw_positions[0][0], g.look_pos);
            if (z.upcoming_count > 0)
                v = rw::slerp(v, custom::dir_vec(g.draw_positions[0][0], room_view::middle_of_tile(z.upcoming[z.upcoming_count - 1].dest)), 0.15f + p.neck_stiffness * 0.15f);
            float num = rw::dot(v, custom::dir_vec(g.draw_positions[1][0], g.draw_positions[0][0]));
            num = 1.f - mathf::pow(1.f - mathf::clamp(-num, 0.f, 1.f), 3.f);
            num *= 1.f - p.neck_stiffness;
            if (num > 0.f)
                v = rw::slerp(v, custom::dir_vec(g.draw_positions[1][0], g.draw_positions[0][0]), num);
            g.head.vel += v * mathf::lerp(6.f, 2.f, p.neck_stiffness);
        }
        auto const& tg = w.lizard_data_get_tongue(creature::lizard_data(w, l));
        if (lizard::tongue_out(state, l))
            g.head.vel = custom::dir_vec(g.draw_positions[0][0], tg.pos) * 5.f;
        body_parts::connect_to_point(g.head, g.draw_positions[0][0] + custom::dir_vec(g.draw_positions[1][0], g.draw_positions[0][0]) * 12.f * p.head_size,
            g.head_connection_rad, false, 0.2f, chunk_vel(state, l, 0), 0.5f, 0.1f);
        (void)head_move; // (the LizardBubbles off a fast head aren't ported)
        if (z.bubble > 0)
            g.head.pos += custom::deg_to_vec(rng.value() * 360.f) * 5.f * (z.bubble_intensity * 0.5f + g.voice_visualization_intensity);

        // the tongue's segments
        if (p.tongue) {
            int32_t const n = std::min(p.tongue_segments, lizard::max_tongue);
            auto const& tp = p.tongue_p;
            float const el = tp.tot_r * tp.elastic_range * tg.reel_in * tg.reel_in, tot = tp.tot_r * tg.reel_in;
            float const stretched = mathf::inverse_lerp(0.f, el, tg.dist);
            float const super = mathf::inverse_lerp(el, tot, tg.dist);
            float const combined = mathf::inverse_lerp(0.f, tp.tot_r, tg.dist);
            for (int32_t i = 0; i < n; ++i) {
                auto& t = g.tongue[i];
                if (!lizard::tongue_out(state, l)) {
                    t.pos = tg.pos;
                    t.last_pos = tg.last_pos;
                    t.vel = vec2{0.f, 0.f};
                    continue;
                }
                body_parts::generic_update(t, room, 0.9f, 1.f);
                float const f = float(i) / float(n - 1);
                float const mid = std::fabs(float(i) - (float(n) - 1.f) / 2.f) / ((float(n) - 1.f) / 2.f);
                float const f2 = tg.state == lizard::tongue_state::retracting ? 1.f : mid;
                vec2 const target = g.draw_positions[0][0]
                    + rw::slerp(custom::dir_vec(g.draw_positions[0][0], tg.graph_pos[0]), custom::dir_vec(g.draw_positions[1][0], g.draw_positions[0][0]),
                        mathf::pow(1.f - f, 1.f + mathf::pow(stretched, 2.f) * 10.f) * 0.5f)
                    * rw::distance(g.draw_positions[0][0], tg.graph_pos[0]) * f;
                t.pos = rw::lerp(t.pos, target, mathf::max(stretched * 0.05f + super * 0.2f, mathf::pow(f2, 3.f)));
                t.vel += (target - t.pos) * (stretched * 0.2f + super * 0.3f);
                if (b == breed::blue || b == breed::cyan || b == breed::peach)
                    t.rad = (2.f + mathf::lerp(3.f, 1.f, mid) * (1.f - mathf::pow(combined, 0.5f))) / (1.f + f * 0.5f + (super + 0.5f) * 0.5f * (1.f - mid));
                else if (b == breed::white)
                    t.rad = (2.f + 0.75f * (1.f - combined) + mathf::lerp(8.f, 1.f, mid) * (1.f - mathf::pow(combined, 0.5f))) / (1.f + f * 0.5f + (super + 0.5f) * 3.f * (1.f - mid));
                else if (b == breed::salamander)
                    t.rad = (3.f + 0.5f * (1.f - combined) + mathf::lerp(8.f, 1.f, mid) * (1.f - mathf::pow(combined, 0.3f))) / (1.f + f * 0.5f + (super + 0.5f) * 2.f * (1.f - mid));
                else if (b == breed::indigo)
                    t.rad = (2.f + mathf::lerp(3.f, 1.f, mid) * (1.f - mathf::pow(combined, 0.5f))) / (1.f + f * 0.5f + (super + 0.5f) * 0.5f * (1.f - mid)) * 1.5f;
                body_parts::push_out_of_terrain(t, room, 1.f);
            }
        }
        if (g.voice_visualization > 0.2f)
            for (uint32_t i = 0; i < 3; ++i)
                g.draw_positions[i][0] += custom::rnv(rng) * 8.f * mathf::pow(mathf::inverse_lerp(0.2f, 1.f, g.voice_visualization), 0.5f) / float(1 + i);
        // DrawSprites' everySecondDraw toggle, once a tick here
        if (rng.value() > 0.025f)
            g.every_second_draw = !g.every_second_draw;
        if (g.flicker > 10)
            g.flicker_color = rng.value();
    }

    void build(sys::game_state const& state, dcon::creature_id l, float alpha, palette const& pal_in, std::vector<sprites::draw_cmd>& out) {
        auto const& w = state.world;
        auto const& g = w.lizard_data_get_graphics(creature::lizard_data(w, l));
        auto const& z = w.lizard_data_get_core(creature::lizard_data(w, l));
        auto const& p = lizard::params_of(state, l);
        auto const b = lizard::breed_of(state, l);
        palette const pal = breed_palette(state, l, pal_in);
        bool const awake = lizard::consious(state, l);
        // the random jitter DrawSprites rolls, from a generator of its own
        rw::rng jitter{uint32_t(l.index() + 1) * 0x9E3779B9u ^ uint32_t(g.breath * 80000.f)};
        auto const dp = [&](int32_t i) { return rw::lerp(g.draw_positions[i][1], g.draw_positions[i][0], alpha); };
        vec3 const effect = effect_color(state, l, pal);
        vec3 const head_col = head_color_impl(state, l, alpha, pal);
        vec3 const body_col = body_sprite_color(state, l, alpha, pal);
        float const depth = mathf::lerp(g.last_depth_rotation, g.depth_rotation, alpha);
        float const head_depth = mathf::lerp(g.last_head_depth_rotation, g.head_depth_rotation, alpha);
        float jaw = mathf::lerp(z.last_jaw_open, lizard::jaw_open(state, l), alpha);
        if (lizard::jaw_ready_for_bite(state, l) && awake)
            jaw += jitter.value() * 0.2f;
        jaw = mathf::lerp(jaw, mathf::lerp(g.last_voice_visualization, g.voice_visualization, alpha) + 0.2f,
            mathf::lerp(g.last_voice_visualization_intensity, g.voice_visualization_intensity, alpha) * 0.8f);
        jaw = mathf::clamp(jaw, 0.f, 1.f);
        // Lizard.SnakeCoil (its elastic easing left out)
        float const coil = b == breed::eel ? std::fabs(std::sin(float(z.snake_ticker) / 60.f * mathf::pi)) : 1.f;

        // the body: four circles (SpriteBodyCirclesStart..) and the mesh between them
        float edge = 5.f;
        vec2 prev = rw::lerp(dp(0), rw::lerp(g.head.last_pos, g.head.pos, alpha), 0.2f);
        float const breath = (std::sin(mathf::lerp(g.last_breath, g.breath, alpha) * mathf::pi * 2.f) + 1.f) * 0.5f * mathf::pow(1.f - w.lizard_data_get_ai(creature::lizard_data(w, l)).run_speed, 2.f);
        vec2 body_mesh[16];
        sprite circles[4];
        for (int32_t j = 0; j < 4; ++j) {
            int32_t const num5 = j < 2 ? 1 : 2;
            vec2 v = body_position(g, j, alpha);
            if (z.anim == lizard::animation::threat_spotted || z.anim == lizard::animation::threat_re_spotted)
                v += custom::rnv(jitter) * jitter.value() * 2.f;
            float rad = display_rad(state, l, uint32_t(num5));
            if (j % 2 == 0)
                rad = (rad + display_rad(state, l, uint32_t(num5 - 1))) / 2.f;
            rad *= 1.f + breath * float(3 - j) * 0.1f * (j == 0 ? 0.5f : 1.f);
            rad *= g.ivars.fatness;
            vec2 const n = rw::normalized(prev - v);
            vec2 const a = custom::perpendicular_vector(n);
            float const d = rw::distance(v, prev);
            body_mesh[j * 4] = v + n * d - a * edge;
            body_mesh[j * 4 + 1] = v + n * d + a * edge;
            body_mesh[j * 4 + 2] = v - a * rad;
            body_mesh[j * 4 + 3] = v + a * rad;
            edge = rad;
            prev = v;
            circles[j] = sprite{"Circle20", v, 0.f, vec2{rad / 10.f, rad / 10.f}, vec2{0.5f, 0.5f}, body_col};
        }

        // the legs: LizardArm_NN and LizardArmColor_NN
        static std::string arm_names[2][55];
        if (arm_names[0][1].empty())
            for (int i = 1; i < 55; ++i) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "LizardArm_%02d", i);
                arm_names[0][i] = buf;
                std::snprintf(buf, sizeof(buf), "LizardArmColor_%02d", i);
                arm_names[1][i] = buf;
            }
        sprite arms[lizard::max_limbs], arm_colors[lizard::max_limbs];
        float arm_alpha[lizard::max_limbs];
        vec3 arm_color[lizard::max_limbs];
        bool arm_visible[lizard::max_limbs];
        for (int32_t k = 0; k < g.limb_count; ++k) {
            auto const& lb = g.limbs[k];
            vec2 const foot = rw::lerp(lb.h.last_pos, lb.h.pos, alpha);
            int32_t ci = k < 2 ? 0 : 2;
            if (g.limb_count > 4)
                ci = std::min(k / 2, 2);
            vec2 root = dp(ci);
            if (k < 2)
                root = rw::lerp(root, dp(1), 0.2f);
            int32_t num9 = custom::int_clamp(int32_t(rw::distance(foot, root) / (4.f * p.limb_size)) + 1, 1, 9);
            num9 += 9 * (2 - int32_t(mathf::clamp(std::fabs(lb.flip) * 3.f, 0.f, 2.f)));
            if (k >= 2)
                num9 += 27;
            num9 = custom::int_clamp(num9, 1, 54);
            float const rot = custom::aim(root, foot) - 90.f;
            vec2 const scale{p.limb_size, mathf::sign(lb.flip) * p.limb_thickness};
            arms[k] = sprite{arm_names[0][num9], foot, rot, scale, vec2{0.5f, 0.5f}, body_col};
            arm_colors[k] = sprite{arm_names[1][num9], foot, rot, scale, vec2{0.5f, 0.5f}, effect};
            arm_visible[k] = !(b == breed::eel && k >= 2);
            arm_alpha[k] = 1.f;
            arm_color[k] = effect;
            if (b == breed::white || head_lights_up_from_noise(b) || b == breed::cyan || b == breed::indigo) {
                arm_alpha[k] = std::sin(g.white_camo_color_amount * mathf::pi) * 0.3f;
                arm_color[k] = pal.black;
            } else if (b == breed::salamander) {
                arm_alpha[k] = k % 2 == 1 ? mathf::lerp(0.3f, 0.1f, std::fabs(depth)) : 0.3f;
                arm_color[k] = g.black_salamander ? effect : pal.black;
            } else if (k % 2 == 0) {
                arm_alpha[k] = mathf::lerp(1.f, 0.3f, std::fabs(depth));
            }
        }

        // the tail mesh, coloured along its length when it has a tail colour
        int32_t const tn = std::min(p.tail_segments, lizard::max_tail);
        vec2 tail_mesh[lizard::max_tail * 4];
        size_t const tail_verts = size_t((tn - 1) * 4 + 3);
        {
            vec2 v = dp(2);
            float w0 = display_rad(state, l, 2) * g.ivars.fatness;
            for (int32_t i = 0; i < tn; ++i) {
                auto const& t = g.tail[i];
                vec2 const tp = rw::lerp(t.bp.last_pos, t.bp.pos, alpha);
                vec2 const n = rw::normalized(tp - v);
                vec2 const a = custom::perpendicular_vector(n);
                float const d = rw::distance(tp, v) / 5.f;
                float const r = t.bp.rad * t.bp.stretched * g.ivars.fatness * g.ivars.tail_fatness;
                tail_mesh[i * 4] = v - a * (w0 + t.bp.rad * t.bp.stretched) * 0.5f * g.ivars.fatness * g.ivars.tail_fatness + n * d;
                tail_mesh[i * 4 + 1] = v + a * (w0 + t.bp.rad * t.bp.stretched) * 0.5f * g.ivars.fatness * g.ivars.tail_fatness + n * d;
                if (i < tn - 1) {
                    tail_mesh[i * 4 + 2] = tp - a * r - n * d;
                    tail_mesh[i * 4 + 3] = tp + a * r - n * d;
                } else {
                    tail_mesh[i * 4 + 2] = tp;
                }
                w0 = r;
                v = tp;
            }
        }
        vec4 tail_colors[lizard::max_tail * 4];
        bool const tail_colored = g.ivars.tail_color > 0.f && b != breed::white;
        if (tail_colored) {
            float const start = g.body_length / (g.body_length + g.tail_length);
            for (size_t j = 0; j < tail_verts; ++j) {
                float const t = float(j / 2) * 2.f / float(tail_verts - 1);
                vec3 const c = body_color_impl(state, l, mathf::lerp(start, 1.f, t), pal);
                tail_colors[j] = vec4{c.x, c.y, c.z, 1.f};
            }
        }

        // the head: LizardJaw, LizardLowerTeeth, LizardUpperTeeth, LizardHead,
        // LizardEyes in one of four angles (by the head's depth rotation)
        vec2 head = rw::lerp(rw::lerp(g.head.last_pos, g.head.pos, alpha), dp(0), 0.2f);
        if (awake) {
            if (z.bubble > 0)
                head += custom::rnv(jitter) * 2.f * (z.bubble_intensity * 0.5f + g.voice_visualization_intensity) * coil;
            if (g.white_flicker > 0)
                head += custom::rnv(jitter) * jitter.value() * 2.f * coil;
            if (z.anim == lizard::animation::prey_spotted || z.anim == lizard::animation::prey_re_spotted || z.anim == lizard::animation::threat_spotted
            || z.anim == lizard::animation::threat_re_spotted)
                head += custom::rnv(jitter) * jitter.value() * (1.f + 2.f * mathf::lerp(g.last_voice_visualization, g.voice_visualization, alpha)) * coil;
        }
        vec2 const neck = dp(0);
        vec2 const head_pos = rw::lerp(g.head.last_pos, g.head.pos, alpha);
        float const head_rot = custom::aim(neck, head_pos);
        vec2 const n3 = custom::perpendicular_vector(neck - head_pos);
        int32_t const angle = 3 - int32_t(std::fabs(head_depth) * 3.9f);
        static char const* const parts[5] = {"LizardJaw", "LizardLowerTeeth", "LizardUpperTeeth", "LizardHead", "LizardEyes"};
        static std::string head_names[5][4][12];
        if (head_names[0][0][0].empty())
            for (int part = 0; part < 5; ++part)
                for (int a = 0; a < 4; ++a)
                    for (int hg = 0; hg < 12; ++hg)
                        head_names[part][a][hg] = std::string{parts[part]} + std::to_string(a) + "." + std::to_string(hg);
        float anchor_y = 0.7f;
        if (b == breed::green || b == breed::blizzard)
            anchor_y = 0.55f;
        else if (b == breed::peach)
            anchor_y = 0.8f;
        sprite heads[5];
        for (int32_t i = 0; i < 5; ++i) {
            int32_t hg = p.head_graphics[i];
            if (i == 3 && g.override_head_graphic >= 0)
                hg = g.override_head_graphic;
            hg = custom::int_clamp(hg, 0, 11);
            float ay = anchor_y;
            if (i == 4 && p.head_graphics[4] == 3)
                ay = 0.75f;
            if (p.head_graphics[i] == 9)
                ay = 0.61f;
            else if (p.head_graphics[i] == 8)
                ay = 0.65f;
            vec2 pos;
            float rot;
            if (i > 1) {
                pos = head + n3 * jaw * head_depth * p.jaw_open_move_jaws_apart * (1.f - p.jaw_open_lower_jaw_fac);
                rot = head_rot + p.jaw_open_angle * (1.f - p.jaw_open_lower_jaw_fac) * jaw * head_depth;
            } else {
                pos = head - n3 * jaw * head_depth * p.jaw_open_move_jaws_apart * p.jaw_open_lower_jaw_fac;
                rot = head_rot - p.jaw_open_angle * p.jaw_open_lower_jaw_fac * jaw * head_depth;
            }
            vec2 const scale{mathf::sign(head_depth) * p.head_size * g.ivars.head_size, p.head_size * g.ivars.head_size};
            heads[i] = sprite{head_names[i][angle][hg], pos, rot, scale, vec2{0.5f, ay}, pal.black};
        }
        // their colours: ApplyPalette, then what DrawSprites sets
        if (b == breed::cyan) {
            heads[4].color = effect;
            heads[1].color = head_col;
            heads[2].color = head_col;
        } else if (b == breed::indigo) {
            vec3 const hsl = custom::rgb2hsl(w.creature_get_payload(l).lizard.effect_color);
            heads[4].color = custom::hsl2rgb(hsl.x, hsl.y, 0.7f);
            heads[1].color = head_col;
        } else if (b == breed::basilisk) {
            vec3 c = mix(head_col, w.creature_get_payload(l).lizard.effect_color, 0.7f);
            if (g.white_flicker > 0 && (g.white_flicker > 15 || g.every_second_draw))
                c = vec3{1.f, 1.f, 1.f};
            heads[0].color = c;
            heads[3].color = c;
        } else {
            if (head_lights_up_from_noise(b))
                heads[2].color = mix(pal.black, vec3{0.5f, 0.5f, 0.5f}, mathf::pow(g.black_lizard_light_up_head, 1.f - 0.95f * jaw));
            heads[0].color = head_col;
            heads[3].color = head_col;
        }
        if (b == breed::salamander && g.black_salamander)
            heads[4].color = effect;
        if (b == breed::blizzard) {
            vec3 const c{0.99f, 1.f, 0.98f};
            heads[4].color = c;
            heads[1].color = c;
            heads[2].color = c;
        }

        // the tongue: a long mesh from the head to the tip, and the tip (Circle20)
        bool const tongue = lizard::tongue_out(state, l);
        vec2 tongue_mesh[lizard::max_tongue * 4];
        vec4 tongue_colors[lizard::max_tongue * 4];
        size_t tongue_verts = 0;
        sprite tongue_tip;
        int32_t const tongue_n = std::min(p.tongue_segments, lizard::max_tongue);
        if (tongue && tongue_n >= 2) {
            auto const& tg = w.lizard_data_get_tongue(creature::lizard_data(w, l));
            int32_t const n = tongue_n;
            tongue_verts = size_t((n - 1) * 4 + 3);
            vec2 const tip = rw::lerp(tg.graph_pos[1], tg.graph_pos[0], alpha);
            vec2 const before = rw::lerp(g.tongue[n - 2].last_pos, g.tongue[n - 2].pos, alpha);
            tongue_tip = sprite{"Circle20", tip, custom::aim(tip, before), vec2{0.05f * (b == breed::blue ? 4.f : 5.f), 0.05f * rw::distance(tip, before)},
                vec2{0.5f, 0.f}, b == breed::indigo ? effect : pal.black};
            vec2 v = dp(0);
            float w0 = 8.f;
            for (int32_t i = 0; i < n; ++i) {
                vec2 const tp = rw::lerp(g.tongue[i].last_pos, g.tongue[i].pos, alpha);
                vec2 const nd = rw::normalized(tp - v);
                vec2 const a = custom::perpendicular_vector(nd);
                float const d = rw::distance(tp, v) / 5.f;
                float const r = g.tongue[i].rad;
                tongue_mesh[i * 4] = v - a * (w0 + r) * 0.5f + nd * d;
                tongue_mesh[i * 4 + 1] = v + a * (w0 + r) * 0.5f + nd * d;
                if (i < n - 1) {
                    tongue_mesh[i * 4 + 2] = tp - a * r - nd * d;
                    tongue_mesh[i * 4 + 3] = tp + a * r - nd * d;
                } else {
                    tongue_mesh[i * 4 + 2] = tp;
                }
                w0 = r;
                v = tp;
            }
            for (size_t k = 0; k < tongue_verts; ++k) {
                float const t = mathf::inverse_lerp(0.f, float(tongue_verts - 1), float(k));
                vec3 c = pal.black;
                if (b == breed::cyan)
                    c = mix(head_col, pal.black, t);
                else if (b == breed::indigo)
                    c = mix(pal.black, effect, t);
                tongue_colors[k] = vec4{c.x, c.y, c.z, 1.f};
            }
        }

        // in AddToContainer's order: (cosmetics: Behind), the even legs, the
        // body mesh, the body circles, the tail, (cosmetics: BodySurface),
        // the odd legs, (cosmetics: BehindHead), the jaw, the lower teeth,
        // the tongue, (what it holds), the upper teeth, the head, the eyes,
        // (cosmetics: InFront, then HUD)
        auto const cosmetics = [&](lizard::sprites_overlap o) { lizard_cosmetics::build(state, l, alpha, pal, o, heads[3], jitter, out); };
        cosmetics(lizard::sprites_overlap::behind);
        auto const add_leg = [&](int32_t k) {
            if (!arm_visible[k])
                return;
            sprites::add(out, arms[k]);
            vec4 const c{arm_color[k].x, arm_color[k].y, arm_color[k].z, arm_alpha[k]};
            vec4 const corners[4] = {c, c, c, c};
            sprites::add(out, arm_colors[k], corners);
        };
        for (int32_t k = 0; k < g.limb_count; k += 2)
            add_leg(k);
        // the body mesh: quads (n, n+1, n+2), (n+1, n+2, n+3) for each circle
        for (int32_t q = 0; q < 4; ++q) {
            sprites::draw_cmd c;
            c.is_triangle = true;
            c.color = body_col;
            c.tri[0] = body_mesh[q * 4];
            c.tri[1] = body_mesh[q * 4 + 1];
            c.tri[2] = body_mesh[q * 4 + 2];
            out.push_back(c);
            c.tri[0] = body_mesh[q * 4 + 1];
            c.tri[1] = body_mesh[q * 4 + 2];
            c.tri[2] = body_mesh[q * 4 + 3];
            out.push_back(c);
        }
        for (auto const& c : circles)
            sprites::add(out, c);
        if (tn > 1) {
            if (tail_colored)
                sprites::long_mesh(out, tail_mesh, tail_colors, tail_verts);
            else
                sprites::long_mesh(out, tail_mesh, tail_verts, body_col);
        }
        cosmetics(lizard::sprites_overlap::body_surface);
        for (int32_t k = 1; k < g.limb_count; k += 2)
            add_leg(k);
        cosmetics(lizard::sprites_overlap::behind_head);
        sprites::add(out, heads[0]);
        sprites::add(out, heads[1]);
        if (tongue_verts > 0) {
            sprites::long_mesh(out, tongue_mesh, tongue_colors, tongue_verts);
            sprites::add(out, tongue_tip);
        }
        sprites::add(out, heads[2]);
        sprites::add(out, heads[3]);
        // (the Black lizard's eyes sprite is hidden)
        if (b != breed::black)
            sprites::add(out, heads[4]);
        cosmetics(lizard::sprites_overlap::in_front);
        cosmetics(lizard::sprites_overlap::hud);
    }

    namespace shared {
        spine_frame make_spine_frame(sys::game_state const& state, dcon::creature_id l, float alpha) {
            auto const& g = state.world.lizard_data_get_graphics(creature::lizard_data(state.world, l));
            auto const& p = lizard::params_of(state, l);
            spine_frame fr;
            for (int32_t i = 0; i < 4; ++i)
                fr.body[i] = body_position(g, i, alpha);
            for (uint32_t i = 1; i < 3; ++i)
                fr.body_rad[i] = display_rad(state, l, i);
            fr.head = rw::lerp(g.head.last_pos, g.head.pos, alpha);
            fr.head_rad = g.head.rad;
            fr.fatness = g.ivars.fatness;
            fr.tail_fatness = g.ivars.tail_fatness;
            fr.tail_count = std::max(2, std::min(p.tail_segments, lizard::max_tail));
            for (int32_t i = 0; i < fr.tail_count; ++i) {
                fr.tail[i] = rw::lerp(g.tail[i].bp.last_pos, g.tail[i].bp.pos, alpha);
                fr.tail_rad[i] = g.tail[i].bp.rad * g.tail[i].bp.stretched;
            }
            fr.tail_end_dir = rw::normalized(g.tail[fr.tail_count - 1].bp.pos - g.tail[fr.tail_count - 2].bp.pos);
            fr.depth_rotation = mathf::lerp(g.last_depth_rotation, g.depth_rotation, alpha);
            fr.log2_depth = fr.depth_rotation != 0.f ? std::log2(std::fabs(fr.depth_rotation)) : 0.f;
            fr.tail_length = g.tail_length;
            fr.body_length = 2.f * 17.f * p.body_length_fac * ((p.body_size_fac + 1.f) / 2.f);
            return fr;
        }

        spine_data spine_position(spine_frame const& fr, float s) {
            int32_t const tn = fr.tail_count;
            float const num = fr.body_length;
            vec2 v, a2, v2;
            float a, b, t;
            if (s < num / (num + fr.tail_length)) {
                float const num2 = mathf::inverse_lerp(0.f, num / (num + fr.tail_length), s);
                int32_t const num3 = int32_t(std::floor(num2 * 4.f - 1.f));
                int32_t const num4 = std::min(int32_t(std::floor(num2 * 4.f)), 3);
                if (num3 < 0) {
                    v = rw::lerp(fr.body[0], fr.head, 0.5f);
                    a = fr.head_rad;
                } else {
                    v = fr.body[num3];
                    a = fr.body_rad[num3 < 2 ? 1 : 2] * fr.fatness;
                }
                a2 = num4 + 1 < 4 ? fr.body[num4 + 1] : fr.tail[0];
                v2 = fr.body[num4];
                b = fr.body_rad[num4 < 2 ? 1 : 2];
                t = mathf::inverse_lerp(float(num3 + 1), float(num4 + 1), num2 * 4.f);
            } else {
                float const num5 = mathf::inverse_lerp(num / (num + fr.tail_length), 1.f, s);
                int32_t const num6 = int32_t(std::floor(num5 * float(tn) - 1.f));
                int32_t const num7 = std::min(int32_t(std::floor(num5 * float(tn))), tn - 1);
                if (num6 < 0) {
                    v = fr.body[3];
                    a = fr.body_rad[2] * fr.fatness;
                } else {
                    v = fr.tail[num6];
                    a = fr.tail_rad[num6] * fr.fatness * fr.tail_fatness;
                }
                a2 = fr.tail[std::min(num7 + 1, tn - 1)];
                v2 = fr.tail[num7];
                b = fr.tail_rad[num7];
                t = mathf::inverse_lerp(float(num6 + 1), float(num7 + 1), num5 * float(tn));
            }
            vec2 n = rw::normalized(rw::lerp(v2 - v, a2 - v2, t));
            if (n.x == 0.f && n.y == 0.f)
                n = fr.tail_end_dir;
            vec2 const perp = custom::perpendicular_vector(n);
            float const rad = mathf::lerp(a, b, t);
            // |depth| ^ Lerp(1.2, 0.3, s ^ 0.5), signed
            float depth = fr.depth_rotation;
            if (depth != 0.f)
                depth = std::exp2(fr.log2_depth * mathf::lerp(1.2f, 0.3f, std::sqrt(s))) * mathf::sign(depth);
            vec2 const pos = rw::lerp(v, v2, t);
            return spine_data{s, pos, pos + perp * depth * rad, n, perp, depth, rad};
        }

        float body_and_tail_length(lizard::graphics_state const& g) {
            return g.body_length + g.tail_length;
        }
        vec3 head_color(sys::game_state const& state, dcon::creature_id l, float alpha, palette const& pal) {
            return head_color_impl(state, l, alpha, pal);
        }
        vec3 body_color(sys::game_state const& state, dcon::creature_id l, float f, palette const& pal) {
            return body_color_impl(state, l, f, pal);
        }
        vec3 dynamic_body_color(sys::game_state const& state, dcon::creature_id l, palette const& pal) {
            return lizard_graphics::dynamic_body_color(state, l, pal);
        }
        vec3 effect_color(sys::game_state const& state, dcon::creature_id l, palette const& pal) {
            return lizard_graphics::effect_color(state, l, pal);
        }
        vec3 white_camo_color(sys::game_state const& state, dcon::creature_id l, palette const& pal) {
            return lizard_graphics::white_camo_color(state.world.lizard_data_get_graphics(creature::lizard_data(state.world, l)), pal);
        }
    }
}
