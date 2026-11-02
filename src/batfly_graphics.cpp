// FlyGraphics.cs: the batfly's graphics module. See batfly_graphics.hpp.

#include <cmath>

#include "batfly_graphics.hpp"
#include "batfly.hpp"
#include "batfly_types.hpp"
#include "body_parts.hpp"
#include "creature.hpp"
#include "physics.hpp"

namespace batfly_graphics {
    using rw::vec2;
    using rw::vec3;
    using sprites::sprite;
    using batfly::behavior;
    using batfly::body_state;
    using batfly::graphics_state;
    using batfly::mov_mode;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        dcon::batfly_data_id data(sys::game_state const& state, dcon::creature_id c) {
            return dcon::batfly_data_id{dcon::batfly_data_id::value_base_t(state.world.creature_get_payload(c).batfly.data)};
        }
        graphics_state& gs(sys::game_state& state, dcon::creature_id c) { return state.world.batfly_data_get_graphics(data(state, c)); }
        graphics_state const& gs(sys::game_state const& state, dcon::creature_id c) { return state.world.batfly_data_get_graphics(data(state, c)); }
        body_state const& pl(sys::game_state const& state, dcon::creature_id c) { return state.world.creature_get_payload(c).batfly; }
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        auto const& w = state.world;
        auto const k = creature::chunk(state, c, 0);
        auto& g = gs(state, c);
        g.lower_pos = w.body_chunk_get_pos(k);
        g.lower_last_pos = w.body_chunk_get_last_pos(k);
        g.lower_vel = w.body_chunk_get_vel(k);
    }

    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room) {
        auto const& w = state.world;
        if (w.creature_get_away(c) != 0)
            return;
        auto& g = gs(state, c);
        auto const& p = pl(state, c);
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        auto const k = creature::chunk(state, c, 0);
        vec2 const pos = w.body_chunk_get_pos(k);
        vec2 const vel = w.body_chunk_get_vel(k);
        behavior const beh = behavior(w.creature_get_behavior(c));
        // lowerBody: GenericBodyPart(rad 5, surface friction 0.5, air friction 1)
        player::body_part lower;
        lower.pos = g.lower_pos;
        lower.last_pos = g.lower_last_pos;
        lower.vel = g.lower_vel;
        lower.rad = 5.f;
        body_parts::generic_update(lower, room, 1.f, 0.5f);
        if (physics::submersion(state, room, k) > 0.f)
            lower.vel *= 0.9f;
        else if (beh == behavior::drop || beh == behavior::burrow)
            lower.vel *= 0.9f;
        else
            lower.vel.y -= physics::gravity(state, creature::body(state, c), room) * 2.f;
        body_parts::push_out_of_terrain(lower, room, 0.5f);
        body_parts::connect_to_point(lower, pos, 10.f, true, 0.f, vel, 0.f, 0.f);
        uint32_t hg = 0;
        auto const holder = creature::holder(state, c, &hg);
        if (beh == behavior::chain) {
            lower.vel *= 0.f;
            if (auto const gr = creature::grasped(w, c, 0))
                lower.pos = w.body_chunk_get_pos(creature::chunk(state, gr, 0));
            else if (p.has_spot)
                lower.pos = p.spot;
        } else if (holder) {
            vec2 v = custom::perpendicular_vector(rw::normalized(creature::main_pos(state, holder) - pos)) * 4.f;
            v.y = -std::fabs(v.y);
            lower.vel += v;
        }
        if (lower.pos.x == pos.x)
            lower.vel.x += mathf::lerp(-0.1f, 0.1f, rng.value());
        g.lower_pos = lower.pos;
        g.lower_last_pos = lower.last_pos;
        g.lower_vel = lower.vel;
        // in a hand (not a chain's): the wings struggle at random
        if (!w.creature_get_dead(c) && holder && !creature::is(w, holder, creature::type::batfly)) {
            g.wings[0][1] = g.wings[0][0];
            g.wings[1][1] = g.wings[1][0];
            g.wings[0][0] = rng.value();
            g.wings[1][0] = rng.value();
            return;
        }
        if (creature::consious(state, c)) {
            g.last_steering = g.steering;
            g.steering = custom::dir_vec(g.lower_pos, pos).x - p.dir.x;
            for (uint32_t i = 0; i < 2; ++i) {
                if (p.mode == mov_mode::swarm_flight) {
                    g.wings[i][0] = 0.f;
                    g.wings[i][1] = 1.f;
                } else if (beh == behavior::drop || beh == behavior::burrow) {
                    g.wings[i][1] = g.wings[i][0];
                    g.wings[i][0] = 1.f;
                    g.steering = 0.f;
                } else if (beh == behavior::chain) {
                    uint8_t const v = p.chain_variation;
                    if (v == 1 || v == 2) {
                        g.wings[i][1] = g.wings[i][0];
                        g.wings[i][0] = p.flap;
                    } else if (v == 3) {
                        g.wings[i][1] = 0.f;
                        g.wings[i][0] = 1.f;
                    } else {
                        g.wings[i][1] = g.wings[i][0];
                        g.wings[i][0] = 1.f;
                        g.steering = 0.f;
                    }
                } else {
                    g.wings[i][1] = g.wings[i][0];
                    g.wings[i][0] = p.flap;
                }
            }
            g.wing_sync += rng.value() / 40.f;
            if (g.wing_sync > 1.f)
                g.wing_sync -= 1.f;
        } else {
            g.last_steering = g.steering;
            g.steering *= 0.999f;
            float const y = body_parts::rotate_around_origo(pos - w.body_chunk_get_last_pos(k), custom::aim(pos, g.lower_pos)).y;
            for (uint32_t j = 0; j < 2; ++j) {
                g.wings[j][1] = g.wings[j][0];
                g.wings[j][0] = mathf::clamp(mathf::lerp(g.wings[j][0], g.death_wing[j] - y * 0.1f, 0.3f), 0.f, 1.f);
            }
        }
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>&) {
        auto const& w = state.world;
        auto const& g = gs(state, c);
        auto const& p = pl(state, c);
        float const t = alpha;
        auto const k = creature::chunk(state, c, 0);
        vec2 const v = rw::lerp(w.body_chunk_get_last_pos(k), w.body_chunk_get_pos(k), t);
        float const num = custom::aim(rw::lerp(g.lower_last_pos, g.lower_pos, t), v);
        // ApplyPalette: the body and the wings black, the eyes as they are
        vec3 const black = pal.black();
        sprites::add(out, sprite{"FlyBody", v, num, vec2{1.f, 1.f}, vec2{0.5f, 0.5f}, black});
        for (uint32_t i = 0; i < 2; ++i) {
            // sprites[1] while all three bites are left, sprites[2] while two
            if (i == 0 ? p.bites != 3 : p.bites <= 1)
                continue;
            float const wl = mathf::lerp(g.wings[i][1], g.wings[i][0], t);
            float num2 = mathf::lerp(wl, 0.5f, mathf::lerp(0.3f, 0.f, mathf::lerp(p.last_flap_depth, p.flap_depth, t)));
            float num3 = mathf::lerp(g.last_steering, g.steering, t);
            num3 = (num3 < 0.f) == (i == 0) ? 0.f : mathf::clamp(std::fabs(num3 * 0.85f) - 0.1f, 0.f, 1.f);
            num2 = mathf::lerp(num2, 0.5f, num3);
            num2 = mathf::inverse_lerp(0.01f, 0.99f, mathf::pow(num2, 2.f));
            float const side = i == 0 ? -1.f : 1.f;
            float const rot = side * (40.f + 150.f * num2) + num;
            float const sx = side * (p.flap_speed < 0.f ? 1.f : 1.f - 0.6f * (std::sin(wl * 3.1415927f) * (1.f - num3)));
            sprites::add(out, sprite{"FlyWing", v, rot, vec2{sx, 1.f}, vec2{0.5f, 0.f}, black});
        }
        sprites::add(out, sprite{"FlyEyes", v, num, vec2{1.f, 1.f}, vec2{0.5f, 0.5f}, vec3{1.f, 1.f, 1.f}});
    }
}
