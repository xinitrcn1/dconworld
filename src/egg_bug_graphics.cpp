#include <cmath>
#include <string_view>

#include "egg_bug_graphics.hpp"
#include "body_parts.hpp"
#include "creature.hpp"
#include "egg_bug.hpp"
#include "particles.hpp"
#include "pathing.hpp"

namespace egg_bug_graphics {
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using world::room_view;
    using player::limb_mode;
    using sprites::sprite;
    using egg_bug::graphics_state;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        // Limb(rad 0.1, surface friction 0.7, air friction 0.99, hunt speed 17, quickness 0.8)
        constexpr body_parts::limb_params leg{0.1f, 0.7f, 0.99f, 17.f, 0.8f};
        // EggBugGraphics.HUE_OFF, HUE_OFF2
        constexpr float hue_off = 1.5f;
        constexpr float hue_off2 = 0.65f;

        graphics_state& gs(sys::game_state& state, dcon::creature_id c) {
            return state.world.egg_bug_data_get_graphics(creature::egg_bug_data(state.world, c));
        }
        graphics_state const& gs(sys::game_state const& state, dcon::creature_id c) {
            return state.world.egg_bug_data_get_graphics(creature::egg_bug_data(state.world, c));
        }
        creature::egg_bug_payload const& pl(sys::game_state const& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).egg_bug;
        }
        vec2 cpos(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_pos(egg_bug::chunk(state, c, i));
        }
        vec2 clast(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_last_pos(egg_bug::chunk(state, c, i));
        }
        vec2 cvel(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_vel(egg_bug::chunk(state, c, i));
        }

        // BodyPart.Reset
        template<typename P>
        void reset_part(P& bp, vec2 at, rw::rng& rng) {
            bp.pos = at + custom::rnv(rng);
            bp.last_pos = bp.pos;
            bp.vel = vec2{0.f, 0.f};
        }

        // Color.Lerp
        vec3 mix(vec3 a, vec3 b, float t) {
            return a + (b - a) * mathf::clamp01(t);
        }
        vec4 rgba(vec3 c, float a = 1.f) {
            return vec4{c.x, c.y, c.z, a};
        }

        // EggBugGraphics.ShowEggs
        bool show_eggs(sys::game_state const& state, dcon::creature_id c) {
            return egg_bug::fire(state, c) ? pl(state, c).eggs_left > 0 : !state.world.creature_get_dead(c);
        }

        // EggBugGraphics.AntennaDir
        vec2 antenna_dir(vec2 c0, vec2 c1, float flip, int s, float side_fac) {
            vec2 const v = custom::dir_vec(c1, c0);
            return rw::normalized(v + custom::perpendicular_vector(v) * mathf::lerp(s == 0 ? -0.7f : 0.7f, flip * -1.4f, std::fabs(flip * 0.7f)) * side_fac);
        }

        // EggBugGraphics.EggAttachPos, from the interpolated positions
        vec2 egg_attach_pos(bool fire, bool show, vec2 c0, vec2 c1, vec2 tail, vec2 z, float flip, int s, int egg) {
            float const t = mathf::inverse_lerp(0.f, 2.f, float(egg));
            vec2 const v = rw::lerp(c1, tail, t);
            vec2 const d = custom::dir_vec(v, c0);
            float num = custom::deg_to_vec(custom::vec_to_deg(z) + (s == 0 ? -45.f : 45.f)).x;
            if (show)
                num *= mathf::lerp(1.5f, 1.f, std::fabs(flip));
            return v + d * mathf::lerp(fire ? 10.f : 2.f, fire ? 0.f : -2.f, t) + custom::perpendicular_vector(d) * num * (egg == 2 ? 3.f : 5.f);
        }
    }

    void egg_colors(vec3 black, vec3 fog, float hue, float darkness, bool fire, vec3 (&out)[3]) noexcept {
        vec3 const a = custom::hsl2rgb(custom::decimal(hue + (fire ? hue_off : 1.5f)), 1.f, 0.5f);
        vec3 const a2 = custom::hsl2rgb(custom::decimal(hue + (fire ? hue_off2 : 1.f)), 1.f, 0.5f);
        vec3 const color = mix(black, mix(a2, fog, 0.3f), 0.1f * (1.f - darkness));
        float const dk = mathf::pow(mathf::inverse_lerp(0.5f, 1.f, darkness), 2.f);
        out[0] = color;
        out[1] = mix(a, color, dk);
        out[2] = mix(a2, color, 0.5f + 0.5f * dk);
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        auto& g = gs(state, c);
        auto& rng = w.creature_get_graphics_random(c);
        if (g.leg_length <= 0.f)
            g = graphics_state{};
        vec2 const c0 = cpos(state, c, 0), c1 = cpos(state, c, 1);
        g.tail_end.rad = 3.f;
        reset_part(g.tail_end, c1, rng);
        for (auto& side : g.antennas)
            for (auto& a : side) {
                a.rad = 1.f;
                reset_part(a, c0, rng);
            }
        for (auto& side : g.legs)
            for (auto& lg : side) {
                lg = player::hand_state{};
                reset_part(lg, c0, rng);
                lg.mode = limb_mode::hunt_relative_position;
                lg.hunt_speed = leg.hunt_speed;
                lg.quickness = leg.quickness;
                lg.absolute_hunt_pos = lg.pos;
            }
        for (auto& side : g.eggs)
            for (auto& e : side) {
                e.rad = 4.f;
                reset_part(e, c1, rng);
            }
        g.last_flip = g.flip;
        g.last_z_rotat = g.z_rotat;
        g.legs_dangle_counter = 30;
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto& g = gs(state, c);
        auto& rng = w.creature_get_graphics_random(c);
        auto const& x = pl(state, c);
        auto const& mv = w.egg_bug_data_get_move(creature::egg_bug_data(w, c));
        bool const fire = egg_bug::fire(state, c);
        bool const awake = egg_bug::consious(state, c);
        bool const footing = egg_bug::footing(state, c);
        auto const& m = egg_bug::movement(state, c);
        vec2 const c0 = cpos(state, c, 0), c1 = cpos(state, c, 1);
        vec2 const v0 = cvel(state, c, 0), v1 = cvel(state, c, 1);
        g.draw_ticker += 1.f;

        if (!awake || !footing) {
            g.legs_dangle_counter = 30;
        } else if (g.legs_dangle_counter > 0) {
            --g.legs_dangle_counter;
            for (uint32_t i = 0; i < 2; ++i)
                if (pathing::accessible(room, room_view::get_tile_position(cpos(state, c, i)), m))
                    g.legs_dangle_counter = 0;
        }
        g.last_flip = g.flip;
        g.last_z_rotat = g.z_rotat;
        if (awake && pathing::tile_access(room, room_view::get_tile_position(c0)) == pathing::access::climb)
            g.z_rotat = rw::slerp(g.z_rotat, custom::deg_to_vec(180.f + 90.f * g.flip), 0.5f);
        else
            g.z_rotat = rw::slerp(g.z_rotat, custom::deg_to_vec(-90.f * g.flip), 0.5f);

        // the tail end
        body_parts::generic_update(g.tail_end, room, 0.99f, 0.5f);
        body_parts::connect_to_point(g.tail_end, c1 + custom::dir_vec(c0, c1) * 7.f + custom::perpendicular_vector(c0, c1) * g.flip * 7.f, 6.f, false, 0.2f, v1, 0.5f, 0.1f);
        g.tail_end.vel.y -= 0.4f;
        g.tail_end.vel += custom::dir_vec(c0, c1) * 0.8f;

        float const num = custom::aim(c1, c0);
        float num2 = 0.f, num3 = 0.f;
        vec2 const travel = rw::normalized(x.travel_dir);
        for (auto const& side : g.legs)
            for (auto const& lg : side) {
                num3 += custom::lerp_map(rw::dot(custom::dir_vec(c0, lg.absolute_hunt_pos), travel), -0.6f, 0.6f, 0.f, 0.25f);
                num2 += custom::distance_to_line(lg.pos, c1, c0);
            }
        num3 *= mathf::inverse_lerp(0.f, 0.1f, rw::magnitude(x.travel_dir));
        if (!std::isnan(num2))
            g.flip = custom::lerp_and_tick(g.flip, mathf::clamp(num2 / 40.f, -1.f, 1.f), 0.07f, 0.1f);
        if (g.legs_dangle_counter > 0)
            num3 = 1.f;
        float num4 = 0.f;
        if (room.get_tile(c0 + custom::perpendicular_vector(c0, c1) * 20.f).solid())
            num4 += 1.f;
        if (room.get_tile(c0 - custom::perpendicular_vector(c0, c1) * 20.f).solid())
            num4 -= 1.f;
        if (num4 != 0.f)
            g.flip = custom::lerp_and_tick(g.flip, num4, 0.07f, 0.05f);

        // the legs: stepping along the travel direction, gripping the terrain
        int32_t n = 0;
        for (int l = 0; l < 2; ++l)
            for (int k = 0; k < 2; ++k) {
                auto& lg = g.legs[l][k];
                body_parts::limb_update(lg, leg, c0, v0, c1, room);
                float const num6 = 0.5f + 0.5f * std::sin((x.run_cycle + float(n) * 0.25f) * mathf::pi);
                if (lg.mode == limb_mode::hunt_relative_position || g.legs_dangle_counter > 0)
                    lg.mode = limb_mode::dangle;
                float const fan = k == 1 ? 45.f : 135.f;
                vec2 a = custom::deg_to_vec(num + fan * (num4 != 0.f ? -num4 : (l == 0 ? 1.f : -1.f)));
                if (awake) {
                    a += x.travel_dir * custom::lerp_map(num3, 0.f, 0.6f, 3.f, 0.f) * mathf::pow(num6, 0.5f);
                    a = rw::normalized(a);
                }
                vec2 const vector = c0 + a * g.leg_length * 0.2f + custom::deg_to_vec(num + fan * (l == 0 ? 1.f : -1.f)) * g.leg_length * 0.1f;
                body_parts::as_body_part(lg, leg.rad, [&](player::body_part& bp) {
                    body_parts::connect_to_point(bp, vector, g.leg_length, false, 0.f, v0, 0.1f, 0.f);
                    body_parts::connect_to_point(bp, c0, g.leg_length, false, 0.f, v0, 0.1f, 0.f);
                });
                if (custom::dist_less(lg.pos, c0, 6.f))
                    lg.pos = c0 + custom::dir_vec(c0, lg.pos) * 6.f;
                if (g.legs_dangle_counter > 0) {
                    vec2 const a2 = vector + a * g.leg_length * 0.7f;
                    lg.vel = rw::lerp(lg.vel, a2 - lg.pos, 0.3f);
                    lg.vel.y -= 0.4f;
                    if (awake)
                        lg.vel += custom::rnv(rng) * 3.f;
                } else {
                    vec2 goal = vector + a * g.leg_length;
                    // not on top of the other legs' footholds
                    for (int l2 = 0; l2 < 2; ++l2)
                        for (int k2 = 0; k2 < 2; ++k2) {
                            vec2 const other = g.legs[l2][k2].absolute_hunt_pos;
                            if (l2 != l && k2 != k && custom::dist_less(goal, other, g.leg_length * 0.3f))
                                goal = other + custom::dir_vec(other, goal) * g.leg_length * 0.3f;
                        }
                    constexpr float reach = 1.5f;
                    if (!lg.reached_snap_position) {
                        body_parts::find_grip(lg, room, vector, vector, g.leg_length * reach, goal, -2, -2, true);
                        if (lg.mode != limb_mode::hunt_absolute_position)
                            body_parts::find_grip(lg, room, vector, vector + a * g.leg_length * 0.5f, g.leg_length * reach, goal, -2, -2, true);
                    }
                    if (!custom::dist_less(lg.pos, lg.absolute_hunt_pos, g.leg_length * reach * mathf::pow(1.f - num6, 0.2f))) {
                        lg.mode = limb_mode::dangle;
                        lg.vel += a * 7.f;
                        lg.vel = rw::lerp(lg.vel, goal - lg.pos, 0.5f);
                    } else {
                        lg.vel += a * 2.f;
                    }
                }
                ++n;
            }

        // the antennae, and the eggs on each side
        float const seg = fire ? 7.5f : 15.f;
        bool const show = show_eggs(state, c);
        for (int s = 0; s < 2; ++s) {
            auto& ant = g.antennas[s];
            for (int k = 0; k < 4; ++k) {
                float const num11 = mathf::inverse_lerp(0.f, 3.f, float(k));
                body_parts::generic_update(ant[k], room, 0.9f, 0.5f);
                ant[k].vel *= 0.9f;
                if (k == 0) {
                    body_parts::connect_to_point(ant[k], c0 + antenna_dir(c0, c1, g.flip, 0, 1.f) * 4.f, fire ? 10.f : 20.f, false, 0.f, vec2{0.f, 0.f}, 0.f, 0.f);
                } else if (!custom::dist_less(ant[k].pos, ant[k - 1].pos, seg)) {
                    vec2 const b = -custom::dir_vec(ant[k].pos, ant[k - 1].pos) * (seg - rw::distance(ant[k].pos, ant[k - 1].pos)) * 0.5f;
                    ant[k].pos += b;
                    ant[k].vel += b;
                    ant[k - 1].pos -= b;
                    ant[k - 1].vel -= b;
                }
                if (k > 1) {
                    vec2 const d = custom::dir_vec(ant[k - 2].pos, ant[k].pos) * 0.8f;
                    ant[k].vel += d;
                    ant[k - 2].vel -= d;
                }
                ant[k].vel += (antenna_dir(c0, c1, g.flip, s, mathf::pow(1.f - num11, 0.5f)) + mv.away_from_terrain_dir * std::sin(num11 * mathf::pi) * 1.7f)
                    * mathf::lerp(6.f, 2.f, num11);
                ant[k].vel.y -= 0.3f;
                if (awake) {
                    ant[k].pos += custom::rnv(rng) * num11 * rng.value() * (1.f + x.fear + x.antenna_attention);
                    if (x.sitting)
                        ant[k].vel += custom::dir_vec(ant[k].pos, mv.antenna_dir) * 11.f * mathf::pow(num11, 0.5f) * rng.value() * x.antenna_attention;
                }
            }
            if (!show)
                continue;
            for (int e = 0; e < 3; ++e) {
                auto& egg = g.eggs[s][e];
                body_parts::generic_update(egg, room, 0.99f, 0.5f);
                vec2 const attach = egg_attach_pos(fire, show, c0, c1, g.tail_end.pos, g.z_rotat, g.flip, s, e);
                body_parts::connect_to_point(egg, attach, 5.f, true, 0.f, v1, 0.1f, 0.f);
                egg.vel.y -= 0.9f;
                egg.vel += custom::dir_vec(rw::lerp(c0, c1, 0.5f), attach) * 0.7f;
                egg.vel += custom::perpendicular_vector(c1, c0) * g.flip * 0.4f;
                // a frightened one leaks (EggBugEgg.LiquidDrip)
                if (rng.value() < 0.05f && (x.shake > 0 || x.no_jumps > 60) && rng.value() < x.fear) {
                    vec2 const dv = custom::dir_vec(attach, egg.pos) * mathf::lerp(3.f, 7.f, rng.value()) + custom::rnv(rng) * rng.value() * 4.f;
                    float const h = x.hue + (fire ? hue_off : 1.5f);
                    particles::spawn(state, room.id, particles::kind::drop, egg.pos, dv, 1.5f, 0.75f, custom::hsl2rgb(custom::decimal(h), 1.f, 0.5f) * 0.6f);
                }
            }
        }
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip) {
        auto const& w = state.world;
        auto const& g = gs(state, c);
        auto const& x = pl(state, c);
        auto const& fs = w.egg_bug_data_get_fire(creature::egg_bug_data(w, c));
        bool const fire = egg_bug::fire(state, c);
        bool const show = show_eggs(state, c);
        float const t = alpha;
        // the shiver (DrawSprites' UnityEngine.Random: a per creature, per tick roll)
        rw::rng rng{(state.tick * 2654435761u) ^ (uint32_t(c.index()) * 40503u) ^ 0x6E6B0Bu};
        rng.next();

        float const num = mathf::lerp(g.last_flip, g.flip, t);
        vec2 const v = rw::slerp(g.last_z_rotat, g.z_rotat, t);
        // ApplyPalette (the darkness isn't lowered by light sources)
        float const darkness = pal.darkness();
        vec3 const black_pal = pal.black(), fog = pal.fog();
        vec3 const color = custom::hsl2rgb(custom::decimal(x.hue + (fire ? hue_off : 1.5f)), 1.f, 0.5f);
        vec3 const color2 = custom::hsl2rgb(custom::decimal(x.hue + (fire ? hue_off2 : 1.f)), 1.f, 0.5f);
        vec3 const black = mix(black_pal, mix(color2, fog, 0.3f), 0.1f * (1.f - darkness));
        vec3 egg_cols[3];
        egg_colors(black_pal, fog, x.hue, darkness, fire, egg_cols);
        vec3 const eye_color = mix(mix(fog, color, 0.5f), black, mathf::inverse_lerp(0.75f, 1.f, darkness));
        vec3 const tip = mix(black, color2, 0.2f * mathf::pow(1.f - darkness, 0.2f));

        vec2 const c0l = clast(state, c, 0), c0p = cpos(state, c, 0), c1l = clast(state, c, 1), c1p = cpos(state, c, 1);
        vec2 vector = rw::lerp(c0l, c0p, t);
        if (x.shake > 0)
            vector += custom::rnv(rng) * rng.value() * 4.f;
        vec2 vector2 = rw::lerp(c1l, c1p, t);
        if (x.shake > 0)
            vector2 += custom::rnv(rng) * rng.value() * 4.f;
        vec2 const vector3 = custom::dir_vec(vector2, vector);
        vec2 const vector4 = custom::perpendicular_vector(vector3);
        vec2 const tail = rw::lerp(g.tail_end.last_pos, g.tail_end.pos, t);
        vec2 const vector5 = tail + custom::dir_vec(vector2, tail) * 3.f;
        // EggAttachPos at this frame's positions (the chunks unshaken)
        vec2 const a0 = rw::lerp(c0l, c0p, t), a1 = rw::lerp(c1l, c1p, t);
        auto const attach = [&](int s, int e) { return egg_attach_pos(fire, show, a0, a1, tail, v, num, s, e); };

        // the sprites by their vanilla index, drawn in that order
        constexpr int max_sprites = 62;
        int const total = fire ? 62 : 50;
        sprites::draw_cmd slot[max_sprites];
        bool visible[max_sprites] = {};
        auto const put = [&](int i, sprite const& s) {
            slot[i] = sprites::draw_cmd{};
            slot[i].spr = s;
            visible[i] = true;
        };
        int const mesh_sprite = fire ? 25 : 19;
        int const head_sprite = fire ? 26 : 20;
        auto const antenna_sprite = [&](int side) { return (fire ? 27 : 21) + side; };
        auto const eye_sprite = [&](int eye) { return eye == 0 ? 0 : (fire ? 29 : 23); };
        auto const leg_sprite = [&](int l, int side, int part) { return (fire ? 30 : 24) + side * 4 + l * 2 + part; };
        auto const back_egg = [&](int s, int e, int part) { return (fire ? 7 : 1) + s * 9 + (2 - e) * 3 + part; };
        auto const front_egg = [&](int s, int e, int part) { return (fire ? 44 : 32) + s * 9 + (2 - e) * 3 + part; };
        auto const back_spear = [](int s, int e) { return 1 + s * 3 + e; };
        auto const front_spear = [](int s, int e) { return 38 + s * 3 + e; };

        // HeadSprite
        put(head_sprite, sprite{"Circle20", vector, custom::vec_to_deg(vector3), vec2{fire ? 0.85f : 0.45f, fire ? 0.95f : 0.55f}, vec2{0.5f, 0.5f}, black});
        // EyeSprites
        for (int i = 0; i < 2; ++i) {
            float const d = (((i == 0) == (num < 0.f)) ? -1.f : 1.f) * (1.f - std::fabs(num));
            vec2 const p = vector + vector3 * 4.f + vector4 * d * 3.f;
            float const sc = fire ? 0.35f : 1.f;
            put(eye_sprite(i), sprite{fire ? "LizardBubble6" : "JetFishEyeB", p, 0.f, vec2{sc, sc}, vec2{0.5f, 0.5f}, eye_color});
        }
        // MeshSprite: MakeLongMesh(7) along the bezier
        {
            vec2 vs[28];
            vec2 v7 = vector + vector3;
            float n2 = 0.f;
            float const b = mathf::lerp(7.f, 5.f, std::fabs(num));
            for (int j = 0; j < 7; ++j) {
                float const f = mathf::inverse_lerp(0.f, 6.f, float(j));
                vec2 const v8 = custom::bezier(vector + vector3 * 3.f, vector2, vector5, vector2, f);
                float const n3 = mathf::lerp(1.5f, b, mathf::pow(mathf::clamp01(std::sin(mathf::pow(f, 0.75f) * mathf::pi)), 0.3f));
                vec2 const a = custom::perpendicular_vector(v8, v7);
                vs[j * 4] = (v7 + v8) / 2.f - a * (n3 + n2) * 0.5f;
                vs[j * 4 + 1] = (v7 + v8) / 2.f + a * (n3 + n2) * 0.5f;
                vs[j * 4 + 2] = v8 - a * n3;
                vs[j * 4 + 3] = v8 + a * n3;
                v7 = v8;
                n2 = n3;
            }
            std::vector<sprites::draw_cmd> tmp;
            sprites::long_mesh(tmp, strip, vs, 28, black);
            if (!tmp.empty()) {
                slot[mesh_sprite] = tmp[0];
                visible[mesh_sprite] = true;
            }
        }
        // LegSprites
        for (int k = 0; k < 2; ++k)
            for (int l = 0; l < 2; ++l) {
                vec2 v9 = rw::lerp(vector, vector2, 0.3f);
                v9 += vector4 * (k == 0 ? -1.f : 1.f) * 3.f * (1.f - std::fabs(num));
                v9 += vector3 * (l == 0 ? -1.f : 1.f) * 4.f;
                auto const& lg = g.legs[k][l];
                vec2 v10 = rw::lerp(lg.last_pos, lg.pos, t);
                if (custom::dist_less(v9, v10, 6.f))
                    v10 = v9 + custom::dir_vec(v9, v10) * 6.f;
                float const f2 = mathf::lerp(k == 0 ? -1.f : 1.f,
                    num * mathf::clamp(custom::distance_to_line(v10, vector2 - vector3 * 20.f, vector2 - vector3 * 20.f + vector4) / -20.f, -1.f, 1.f), std::fabs(num));
                vec2 const v11 = custom::inverse_kinematic(v9, v10, g.leg_length / 3.f, g.leg_length * 0.6666667f, f2);
                put(leg_sprite(l, k, 0), sprite{"CentipedeLegA", v9, custom::aim(v9, v11), vec2{-mathf::sign(g.flip) * 0.8f, rw::distance(v9, v11) / (fire ? 17.f : 27.f)},
                                                vec2{0.5f, 0.1f}, black});
                put(leg_sprite(l, k, 1), sprite{"CentipedeLegB", v11, custom::aim(v11, v10), vec2{-mathf::sign(f2) * 1.f, (rw::distance(v11, v10) + 1.f) / (fire ? 15.f : 25.f)},
                                                vec2{0.5f, 0.1f}, black});
            }
        // AntennaSprites: MakeLongMesh(4, pointy, custom colours)
        for (int m = 0; m < 2; ++m) {
            vec2 vs[15];
            vec4 cols[15];
            vec2 v7 = vector;
            constexpr float d2 = 1.f;
            for (int k = 0; k < 4; ++k) {
                auto const& a = g.antennas[m][k];
                vec2 v12 = rw::lerp(a.last_pos, a.pos, t);
                if (fire && k > 0) {
                    auto const& p = g.antennas[m][k - 1];
                    vec2 const v13 = rw::lerp(p.last_pos, p.pos, t);
                    float const ang = custom::vec_to_deg(custom::dir_vec(v13, v12)) + float(k % 2 == 0 ? -45 : 45);
                    v12 = v13 + custom::deg_to_vec(ang) * rw::distance(v13, v12);
                }
                vec2 const nd = rw::normalized(v12 - v7);
                vec2 const a2 = custom::perpendicular_vector(nd);
                if (k == 0) {
                    vs[0] = v7 - a2 * d2;
                    vs[1] = v7 + a2 * d2;
                    vs[2] = (v12 + v7) / 2.f - a2 * d2;
                    vs[3] = (v12 + v7) / 2.f + a2 * d2;
                } else {
                    float const d4 = rw::distance(v12, v7) / 5.f;
                    vs[k * 4] = v7 - a2 * d2 + nd * d4;
                    vs[k * 4 + 1] = v7 + a2 * d2 + nd * d4;
                    if (k < 3) {
                        vs[k * 4 + 2] = v12 - a2 * d2 - nd * d4;
                        vs[k * 4 + 3] = v12 + a2 * d2 - nd * d4;
                    } else {
                        vs[k * 4 + 2] = v12;
                    }
                }
                v7 = v12;
            }
            for (int i = 0; i < 15; ++i)
                cols[i] = rgba(mix(black, tip, float(i) / 14.f));
            std::vector<sprites::draw_cmd> tmp;
            sprites::long_mesh(tmp, strip, vs, cols, 15);
            if (!tmp.empty()) {
                slot[antenna_sprite(m)] = tmp[0];
                visible[antenna_sprite(m)] = true;
            }
        }

        // the eggs (and the fire bug's spines)
        auto const front_visible = [&](int s) { return custom::deg_to_vec(custom::vec_to_deg(v) + (s == 0 ? -45.f : 45.f)).y > 0.f; };
        // the stub left where an egg was: the egg's eye sprite, small
        auto const stub = [&](int s, int e, std::string_view element, float anchor_y) {
            vec2 const p = attach(s, e);
            vec3 const col = mix(tip, egg_cols[2], 0.5f);
            sprite const sp{element, p, custom::aim(p, vector), vec2{0.2f, 0.35f}, vec2{0.5f, anchor_y}, col};
            put(back_egg(s, e, 2), sp);
            if (front_visible(s))
                put(front_egg(s, e, 2), sp);
        };
        if (fire) {
            vec3 const spear_color = custom::hsl2rgb(custom::decimal(x.hue + hue_off), 1.f, 0.5f);
            for (int s = 0; s < 2; ++s)
                for (int e = 0; e < 3; ++e) {
                    int const idx = s * 3 + e;
                    vec2 const p14 = attach(s, e);
                    auto const& egg = g.eggs[s][e];
                    vec2 const p15 = rw::lerp(egg.last_pos, egg.pos, t);
                    bool const gone = x.eggs_left <= idx;
                    if (gone) {
                        bool const stabbing = fs.stab_spine[s][e] != 0;
                        float const ext = fs.spine_extensions[s][e];
                        put(back_spear(s, e), sprite{"FireBugSpear", p14, custom::aim(p14, vector), vec2{1.f, ext}, vec2{0.5f, 0.5f}, stabbing ? vec3{1.f, 1.f, 1.f} : black});
                        put(front_spear(s, e),
                            sprite{"FireBugSpearColor", p14, custom::aim(p14, vector) + 180.f, vec2{1.f, ext}, vec2{0.5f, 0.5f}, stabbing ? vec3{1.f, 1.f, 1.f} : spear_color});
                        // (Futile_White with the WaterNut shader: a pale bubble here)
                        stub(s, e, "Circle20", 0.3f);
                        continue;
                    }
                    float const num9 = std::sin(g.draw_ticker * 0.15f + float(idx) * mathf::pi);
                    float const rot = custom::aim(p15, p14);
                    bool const fv = front_visible(s);
                    put(back_egg(s, e, 0), sprite{"LegsAPole", p15, rot, vec2{1.f + num9 * 0.2f, 1.f + num9 * 0.2f}, vec2{0.5f, 0.3f}, egg_cols[1]});
                    put(back_egg(s, e, 1), sprite{"JetFishEyeB", p15, rot, vec2{1.f + num9 * 0.2f, 1.f + num9 * 0.2f}, vec2{0.5f, 0.3f}, egg_cols[1]});
                    if (fv) {
                        put(front_egg(s, e, 0), sprite{"LegsAPole", p15, rot, vec2{1.f, 1.f}, vec2{0.5f, 0.3f}, egg_cols[1]});
                        put(front_egg(s, e, 1), sprite{"JetFishEyeB", p15, rot, vec2{1.f, 1.f}, vec2{0.5f, 0.3f}, egg_cols[1]});
                    }
                    // the bubble over the egg (WaterNut), translucent
                    float const sc = (0.85f + num9 * 0.15f) * 0.8f;
                    vec4 const bc = rgba(egg_cols[1], 0.45f);
                    vec4 const corners[4] = {bc, bc, bc, bc};
                    std::vector<sprites::draw_cmd> tmp;
                    sprites::add(tmp, sprite{"Circle20", p15, rot, vec2{sc, sc}, vec2{0.5f, 0.3f}, egg_cols[1]}, corners);
                    slot[back_egg(s, e, 2)] = tmp[0];
                    visible[back_egg(s, e, 2)] = true;
                    if (fv) {
                        tmp.clear();
                        sprites::add(tmp, sprite{"Circle20", p15, rot, vec2{0.8f, 0.8f}, vec2{0.5f, 0.3f}, egg_cols[1]}, corners);
                        slot[front_egg(s, e, 2)] = tmp[0];
                        visible[front_egg(s, e, 2)] = true;
                    }
                }
        } else if (show) {
            static constexpr std::string_view back_elements[3] = {"DangleFruit0A", "DangleFruit0B", "JetFishEyeA"};
            static constexpr std::string_view front_elements[3] = {"EggBugEggColor", "EggBugEggColor", "JetFishEyeA"};
            for (int s = 0; s < 2; ++s)
                for (int e = 0; e < 3; ++e) {
                    vec2 const p = attach(s, e);
                    auto const& egg = g.eggs[s][e];
                    vec2 const p16 = rw::lerp(egg.last_pos, egg.pos, t);
                    float const rot = custom::aim(p16, p);
                    bool const fv = front_visible(s);
                    for (int part = 0; part < 3; ++part) {
                        vec2 const sc = part < 2 ? vec2{0.7f, 0.75f} : vec2{0.45f, 0.45f};
                        float const ay = part < 2 ? 0.3f : 0.7f;
                        put(back_egg(s, e, part), sprite{back_elements[part], p16, rot, sc, vec2{0.5f, ay}, egg_cols[part]});
                        if (fv)
                            put(front_egg(s, e, part), sprite{front_elements[part], p16, rot, sc, vec2{0.5f, ay}, egg_cols[part]});
                    }
                }
        } else {
            for (int s = 0; s < 2; ++s)
                for (int e = 0; e < 3; ++e)
                    stub(s, e, "JetFishEyeA", 0.7f);
        }

        for (int i = 0; i < total; ++i)
            if (visible[i])
                out.push_back(slot[i]);
    }
}
