#include <cmath>
#include <initializer_list>

#include "snail_graphics.hpp"
#include "body_parts.hpp"
#include "creature.hpp"
#include "physics.hpp"
#include "snail.hpp"
#include "tracker.hpp"

namespace snail_graphics {
    using rw::vec2;
    using rw::vec3;
    using world::room_view;
    using sprites::sprite;
    using snail::graphics_state;
    using player::limb_mode;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        // Limb(chunk 0, rad 0.5, surface friction 0.5, air friction 0.98,
        // huntSpeed 5, quickness 0.5)
        constexpr body_parts::limb_params limb_params{0.5f, 0.5f, 0.98f, 5.f, 0.5f};
        // GenericBodyPart: the head (surface friction 0.5, air friction
        // 0.98), the tail (0.5, 0.88)
        constexpr float head_surface_friction = 0.5f, head_air_friction = 0.98f;
        constexpr float tail_surface_friction = 0.5f, tail_air_friction = 0.88f;
        // the CreatureLooker's tracker (seeAroundCorners)
        constexpr int32_t see_around_corners = 10;

        graphics_state& gs(sys::game_state& state, dcon::creature_id c) {
            return state.world.snail_data_get_graphics(creature::snail_data(state.world, c));
        }
        graphics_state const& gs(sys::game_state const& state, dcon::creature_id c) {
            return state.world.snail_data_get_graphics(creature::snail_data(state.world, c));
        }
        dcon::body_chunk_id ch(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return creature::chunk(state, c, i);
        }
        creature::snail_payload const& pl(sys::game_state const& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).snail;
        }

        // CreatureLooker (simplified): the tracked creature most worth
        // looking at (CreatureInterestBonus: the unease), kept a while
        void look_update(sys::game_state& state, dcon::creature_id c, graphics_state& g) {
            auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
            auto const& ts = tracker::of(state, c);
            bool const valid = g.look && creature::live(state, g.look) && tracker::find(ts, g.look) != nullptr;
            if (valid && rng.value() > 1.f / 30.f)
                return;
            dcon::creature_id best;
            float best_score = 0.f;
            for (uint32_t i = 0; i < ts.count; ++i) {
                auto const& r = ts.reps[i];
                if (!creature::live(state, r.who))
                    continue;
                float const score = tracker::chance_of_finding(r, see_around_corners) * (1.f + snail::creature_unease(state, c, r.who));
                if (score > best_score) {
                    best_score = score;
                    best = r.who;
                }
            }
            g.look = best;
        }
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        auto& g = gs(state, c);
        float const size = pl(state, c).size;
        g = graphics_state{};
        vec2 const at = w.body_chunk_get_pos(ch(state, c, 0));
        for (auto& l : g.limbs) {
            l.pos = l.last_pos = at;
            l.vel = vec2{0.f, 0.f};
            l.absolute_hunt_pos = at;
            l.mode = limb_mode::hunt_relative_position;
            l.hunt_speed = limb_params.hunt_speed;
            l.quickness = limb_params.quickness;
        }
        g.head.rad = mathf::lerp(size, 1.f, 0.7f) * 5.f;
        g.tail.rad = 0.1f;
        for (auto* bp : {&g.head, &g.tail}) {
            bp->pos = bp->last_pos = at;
            bp->vel = vec2{0.f, 0.f};
        }
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto& g = gs(state, c);
        auto const& x = pl(state, c);
        auto const k0 = ch(state, c, 0), k1 = ch(state, c, 1);
        bool const awake = creature::consious(state, c);
        float const out = snail::out_of_shell(state, c);
        float const size = x.size;
        vec2 const main = w.body_chunk_get_pos(k0);
        vec2 const main_vel = w.body_chunk_get_vel(k0);
        vec2 const b1 = w.body_chunk_get_pos(k1);
        float const main_sub = physics::submersion(state, room, k0);
        look_update(state, c, g);
        // (no Snail_Charging_LOOP / Snail_Walking_LOOP sounds)
        ++g.hand_change_counter;
        for (uint32_t i = 0; i < 2; ++i) {
            auto& l = g.limbs[i];
            float const num = 14.f * size;
            if (x.has_suck_point) {
                l.mode = limb_mode::hunt_absolute_position;
                if (x.move) {
                    if (i == g.moving_hand) {
                        vec2 v = custom::dir_vec(w.body_chunk_get_last_last_pos(k0), x.suck_point);
                        v = rw::normalized(v + (custom::dir_vec(b1, main) + custom::perpendicular_vector(rw::normalized(b1 - main)) * 0.5f * (i == 0 ? -1.f : 1.f)))
                            * num * 0.9f
                          + x.suck_point;
                        l.hunt_speed = 2.f + float(g.hand_change_counter);
                        if (g.hand_change_counter < 10)
                            l.absolute_hunt_pos = v;
                        else if (g.hand_change_counter == 10)
                            body_parts::find_grip(l, room, main, l.pos, num * 0.9f, v, 2, 2, true);
                    } else if (!custom::dist_less(l.absolute_hunt_pos, main, num * 0.9f) && g.hand_change_counter > 10) {
                        g.moving_hand = uint8_t(1 - g.moving_hand);
                        g.hand_change_counter = 0;
                    }
                }
            } else {
                l.mode = limb_mode::dangle;
            }
            if (!awake)
                l.mode = limb_mode::dangle;
            if (l.mode == limb_mode::dangle)
                l.vel.y -= 0.9f * (1.f - main_sub);
            body_parts::limb_update(l, limb_params, main, main_vel, b1, room);
            body_parts::as_body_part(l, limb_params.rad,
                [&](player::body_part& bp) { body_parts::connect_to_point(bp, main, num, false, 0.f, main_vel, 0.5f, 0.f); });
        }
        // the tail: dragged along while it crawls, else hanging
        if (x.has_suck_point && x.move)
            g.tail.vel = rw::lerp(g.tail.vel,
                (g.tail.pos - w.body_chunk_get_last_last_pos(k0)) * 0.2f * mathf::inverse_lerp(0.5f, 3.f, rw::magnitude(main_vel)), 0.5f);
        else
            g.tail.vel.y -= 0.5f * (1.f - main_sub);
        g.tail.vel -= x.shell_direction * 1.2f;
        body_parts::generic_update(g.tail, room, tail_air_friction, tail_surface_friction);
        if (out > 0.f)
            body_parts::push_out_of_terrain(g.tail, room, tail_surface_friction);
        body_parts::connect_to_point(g.tail, main, 7.f * size * mathf::lerp(0.5f, 1.f, out), false, 0.f, main_vel, 0.f, 0.f);
        g.body_dir = custom::dir_vec(b1, main) + custom::dir_vec(g.tail.pos, main);
        // the head, out of the shell: towards what it looks at
        if (out > 0.f) {
            body_parts::push_out_of_terrain(g.head, room, head_surface_friction);
            if (awake) {
                vec2 a = g.body_dir * 0.2f;
                if (g.look && creature::live(state, g.look)) {
                    if (auto const* r = tracker::find(tracker::of(state, c), g.look)) {
                        if (r->visual && creature::here(state, g.look, room.id))
                            a += custom::dir_vec(main, creature::main_pos(state, g.look));
                        else if (tracker::chance_of_finding(*r, see_around_corners) * snail::creature_unease(state, c, g.look) > 0.2f)
                            a += custom::dir_vec(main, room_view::middle_of_tile(tracker::best_guess(*r)));
                    }
                }
                g.head.vel += rw::normalized(a) * 2.2f;
            } else {
                g.head.vel.y -= 0.5f * (1.f - main_sub);
            }
            g.head.vel += (rw::lerp(g.limbs[g.moving_hand].pos, g.limbs[0].pos, 0.6f) - g.head.pos) * 0.1f;
        }
        body_parts::generic_update(g.head, room, head_air_friction, head_surface_friction);
        body_parts::connect_to_point(g.head, main, 7.5f * size * mathf::lerp(0.2f, 1.f, out), false, 0.f, main_vel, 0.2f, 0.f);
        // (shadowExtensionFac drives the Shadows container's sprite, not ported)
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>&) {
        auto const& w = state.world;
        auto const& g = gs(state, c);
        auto const& x = pl(state, c);
        auto const& lk = snail::looks_of(state, c);
        auto const k0 = ch(state, c, 0), k1 = ch(state, c, 1);
        float const t = alpha;
        // ApplyPalette: the tail, limbs and head in the palette's black (the
        // eyes stay white)
        vec3 const black = pal.black();
        vec3 const white{1.f, 1.f, 1.f};
        vec2 const p0 = rw::lerp(w.body_chunk_get_last_pos(k0), w.body_chunk_get_pos(k0), t);
        vec2 const p1 = rw::lerp(w.body_chunk_get_last_pos(k1), w.body_chunk_get_pos(k1), t);
        float const rad0 = w.body_chunk_get_rad(k0);
        float const rad1 = w.body_chunk_get_rad(k1);
        // DrawSprites' UnityEngine.Random: the shell's shake (a draw time roll)
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));

        // 0 SnailTail
        vec2 const tail = rw::lerp(g.tail.last_pos, g.tail.pos, t);
        float const tail_sx = rad0 * 4.5f / 20.f;
        float const tail_sy = mathf::max(rw::distance(p0, tail) / 23.f, rad0 * 3.5f / 20.f);
        sprites::add(out, sprite{"SnailTail", p0, custom::aim(p0, tail), vec2{tail_sx, tail_sy}, vec2{0.5f, 0.2f}, black});
        // 1, 2 SnailLimb (the first one mirrored)
        for (uint32_t i = 0; i < 2; ++i) {
            vec2 const lp = rw::lerp(g.limbs[i].last_pos, g.limbs[i].pos, t);
            sprites::add(out, sprite{"SnailLimb", lp, custom::aim(lp, p0), vec2{i == 0 ? -1.f : 1.f, rw::distance(lp, p0) / 15.f}, vec2{0.5f, 0.1f}, black});
        }
        // 3 SnailHead
        vec2 const head = rw::lerp(g.head.last_pos, g.head.pos, t);
        float const head_scale = g.head.rad / 6.f;
        sprites::add(out, sprite{"SnailHead", head, custom::aim(head, p0), vec2{head_scale, head_scale}, vec2{0.5f, 0.3f}, black});
        // 4, 5 the eyes (pixel)
        vec2 const hd = rw::normalized(head - p0);
        for (uint32_t j = 0; j < 2; ++j) {
            vec2 v = custom::perpendicular_vector(hd) * 2.2f * (j == 1 ? -1.f : 1.f);
            v *= 1.f - std::fabs(rw::dot(g.body_dir, hd));
            v += hd * 2.f;
            v += head;
            sprites::add(out, sprite{"pixel", v, 0.f, vec2{1.f, 1.f}, vec2{0.5f, 0.5f}, white});
        }
        // 6, 7 SnailShellA, SnailShellB: shaking while triggered or charging,
        // swollen the tick it clicks
        vec2 shell = p1;
        if (!w.creature_get_dead(c) && x.triggered) {
            shell += custom::deg_to_vec(rng.value() * 360.f) * rng.value() * 5.f;
        } else if (creature::consious(state, c) && rng.value() < mathf::inverse_lerp(0.4f, 0.6f, x.click_counter)) {
            shell += custom::deg_to_vec(rng.value() * 360.f) * mathf::inverse_lerp(0.4f, 1.f, x.click_counter) * rng.value() * 4.f;
        }
        float const rot = custom::aim(p0, p1);
        float const scale = mathf::lerp(rad1 / 9.f, x.size, 0.2f) * (x.bloated ? mathf::lerp(1.5f, 1.f, t) : 1.f);
        sprites::add(out, sprite{"SnailShellA", shell, rot, vec2{scale, scale}, vec2{0.5f, 0.5f}, lk.shell_color[0]});
        sprites::add(out, sprite{"SnailShellB", shell, rot, vec2{scale, scale}, vec2{0.5f, 0.5f}, lk.shell_color[1]});
        // (8, the Circle20 shadow on the wall behind, is in the Shadows
        // container: not drawn here)
    }
}
