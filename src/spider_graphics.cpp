// SpiderGraphics.cs. See spider_graphics.hpp.

#include <cmath>

#include "spider_graphics.hpp"
#include "body_parts.hpp"
#include "creature.hpp"
#include "spider.hpp"
#include "spider_types.hpp"

namespace spider_graphics {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using sprites::sprite;
    using spider::graphics_state;
    using spider::limb;
    using world::room_view;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        // SpiderGraphics.legSpriteSizes, limbLengths
        constexpr float leg_sprite_sizes[4][2] = {{19.f, 20.f}, {26.f, 20.f}, {21.f, 23.f}, {26.f, 17.f}};
        constexpr float limb_lengths[4][2] = {{0.85f, 0.5f}, {1.f, 0.6f}, {0.95f, 0.5f}, {0.9f, 0.65f}};
        constexpr std::string_view leg_a[4] = {"SpiderLeg0A", "SpiderLeg1A", "SpiderLeg2A", "SpiderLeg3A"};
        constexpr std::string_view leg_b[4] = {"SpiderLeg0B", "SpiderLeg1B", "SpiderLeg2B", "SpiderLeg3B"};
        // the Limbs' defaults: hunt speed 15, quickness 0.95, air friction 0.98
        constexpr float default_hunt_speed = 15.f;
        constexpr float default_quickness = 0.95f;
        constexpr float air_friction = 0.98f;

        dcon::spider_data_id data(sys::game_state const& state, dcon::creature_id c) {
            return dcon::spider_data_id{dcon::spider_data_id::value_base_t(state.world.creature_get_payload(c).spider.data)};
        }
        graphics_state& gs(sys::game_state& state, dcon::creature_id c) { return state.world.spider_data_get_graphics(data(state, c)); }
        graphics_state const& gs(sys::game_state const& state, dcon::creature_id c) { return state.world.spider_data_get_graphics(data(state, c)); }
        spider::payload const& pl(sys::game_state const& state, dcon::creature_id c) { return state.world.creature_get_payload(c).spider; }
        dcon::body_chunk_id main(sys::game_state const& state, dcon::creature_id c) { return creature::chunk(state, c, 0); }

        // another spider of the room whose graphics module runs
        bool other_spider(sys::game_state const& state, dcon::creature_id o, dcon::room_id room) {
            auto const& w = state.world;
            return o && w.creature_is_valid(o) && w.creature_get_type(o) == creature::type::spider && creature::here(state, o, room);
        }

        // Limb.Update (pushOutOfTerrain off)
        void limb_update(limb& l) {
            l.last_pos = l.pos;
            if (!l.dangle) {
                if (custom::dist_less(l.absolute_hunt_pos, l.pos, l.hunt_speed))
                    l.vel = l.absolute_hunt_pos - l.pos;
                else
                    l.vel = rw::lerp(l.vel, custom::dir_vec(l.pos, l.absolute_hunt_pos) * l.hunt_speed, l.quickness);
            }
            l.quickness = default_quickness;
            l.hunt_speed = default_hunt_speed;
            l.pos += l.vel;
            l.vel *= air_friction;
        }
        // BodyPart.ConnectToPoint on a limb (rad 1)
        void connect(limb& l, vec2 pnt, float rad, vec2 host_vel) {
            player::body_part bp;
            bp.pos = l.pos;
            bp.last_pos = l.last_pos;
            bp.vel = l.vel;
            bp.rad = 1.f;
            body_parts::connect_to_point(bp, pnt, rad, false, 0.f, host_vel, 1.f, 0.5f);
            l.pos = bp.pos;
            l.vel = bp.vel;
        }

        // SpiderGraphics.FindGrip
        void find_grip(graphics_state& g, room_view const& room, vec2 mp, uint32_t i, uint32_t j, vec2 ideal, float rad, float speed) {
            limb& l = g.limbs[i][j];
            if (room.get_tile(ideal).flags & world::tile_wall_behind) {
                l.absolute_hunt_pos = ideal;
            } else {
                player::hand_state h;
                h.pos = l.pos;
                h.last_pos = l.last_pos;
                h.vel = l.vel;
                h.mode = player::limb_mode::hunt_absolute_position;
                h.absolute_hunt_pos = l.absolute_hunt_pos;
                body_parts::find_grip(h, room, mp, ideal, rad, ideal + g.body_dir * mathf::lerp(speed * 2.f, rad / 2.f, 0.5f), 2, 2, true);
                l.absolute_hunt_pos = h.absolute_hunt_pos;
            }
            g.limb_goal_distances[i][j] = rw::distance(l.pos, l.absolute_hunt_pos);
        }
    }

    void reset(sys::game_state& state, dcon::creature_id c) {
        auto& g = gs(state, c);
        g = graphics_state{};
        auto rng = rw::random(state.seed, 0, uint32_t(c.value));
        g.body_dir = custom::deg_to_vec(rng.value() * 360.f);
        g.last_body_dir = g.body_dir;
        g.limb_length = mathf::lerp(10.f, 40.f, pl(state, c).size);
        vec2 const at = state.world.body_chunk_get_pos(main(state, c));
        for (uint32_t i = 0; i < 4; ++i)
            for (uint32_t j = 0; j < 2; ++j) {
                g.death_leg_positions[i][j] = custom::deg_to_vec(rng.value() * 360.f);
                limb& l = g.limbs[i][j];
                l = limb{};
                l.pos = at;
                l.last_pos = at;
                l.absolute_hunt_pos = at;
                l.dangle = 1;
            }
        g.legs_position = rng.value() < 0.5f ? 1 : 0;
        g.last_legs_position = g.legs_position;
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        auto& g = gs(state, c);
        vec2 const at = state.world.body_chunk_get_pos(main(state, c));
        for (uint32_t i = 0; i < 4; ++i)
            for (uint32_t j = 0; j < 2; ++j) {
                limb& l = g.limbs[i][j];
                l.pos = at;
                l.last_pos = at;
                l.vel = vec2{0.f, 0.f};
            }
        g.last_body_dir = g.body_dir;
        g.behind_me = dcon::creature_id{};
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto& g = gs(state, c);
        auto const& x = pl(state, c);
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        auto const m = main(state, c);
        vec2 const mp = w.body_chunk_get_pos(m);
        vec2 const mv = w.body_chunk_get_vel(m);
        bool const awake = creature::consious(state, c);
        bool const dead = w.creature_get_dead(c) != 0;
        g.last_body_dir = g.body_dir;
        auto const att = spider::attached_chunk(state, c);
        bool const att_spider = other_spider(state, att.spider, room.id);
        if (att.chunk) {
            g.body_dir = custom::dir_vec(mp, w.body_chunk_get_pos(att.chunk));
        } else {
            g.body_dir -= custom::dir_vec(mp, x.drag_pos);
            g.body_dir += mv * 0.2f;
            if (!awake)
                g.body_dir += custom::deg_to_vec(rng.value() * 360.f) * x.death_spasms;
            g.body_dir = rw::normalized(g.body_dir);
        }
        float const mag = rw::magnitude(mv);
        if (mag > 1.f) {
            g.walk_cycle += mathf::max(0.f, (mag - 1.f) / 30.f);
            if (g.walk_cycle > 1.f)
                g.walk_cycle -= 1.f;
        }
        g.last_legs_position = g.legs_position;
        g.legs_position = g.walk_cycle > 0.5f ? 1 : 0;
        vec2 const a = custom::perpendicular_vector(g.body_dir);
        auto const chain = spider::chain_of(state, c);
        bool const behind = other_spider(state, g.behind_me, room.id);
        bool const drop = x.follow_type == spider::move::drop;
        bool const in_acc = (x.flags & spider::flag_in_accessible) != 0;
        bool const idle = (x.flags & spider::flag_idle) != 0;
        float const speed_t = mathf::inverse_lerp(0.5f, 5.f, mag);
        for (uint32_t i = 0; i < 4; ++i)
            for (uint32_t j = 0; j < 2; ++j) {
                limb& l = g.limbs[i][j];
                float const side = -1.f + 2.f * float(j);
                vec2 vector = g.body_dir;
                if (behind)
                    vector = rw::slerp(vector, gs(state, g.behind_me).body_dir, 0.2f);
                if (att_spider)
                    vector = rw::slerp(vector, gs(state, att.spider).body_dir, 0.2f);
                bool const flag2 = ((i % 2) == j) == (g.legs_position != 0);
                float const spread = mathf::lerp(30.f, 140.f, float(i) * 0.33333334f) + 20.f * x.legs_position + (i == 3 && !chain.in_chain ? 20.f : 0.f)
                    + 15.f * (flag2 ? -1.f : 1.f) * speed_t;
                vector = custom::deg_to_vec(
                    custom::vec_to_deg(vector) + mathf::lerp(spread, 180.f * (0.5f + x.legs_position / 2.f), std::fabs(x.legs_position) * 0.3f) * side);
                float const num = limb_lengths[i][0] * g.limb_length;
                vec2 const ideal = mp + vector * num * 0.85f + rw::normalized(mv) * num * 0.4f * speed_t;
                if (i == 0 && !dead && (!idle || x.light_exp <= 0.f)) {
                    vec2 const r = custom::deg_to_vec(rng.value() * 360.f);
                    l.pos += r * rng.value();
                }
                bool flag3 = false;
                if (awake) {
                    l.dangle = 0;
                    if (drop || !in_acc) {
                        flag3 = true;
                        l.dangle = 1;
                        vec2 const r = custom::deg_to_vec(rng.value() * 360.f);
                        l.vel += r * rng.value() * 3.f;
                    } else if (i == 0 && att.chunk) {
                        flag3 = true;
                        l.absolute_hunt_pos = w.body_chunk_get_pos(att.chunk) + a * side * w.body_chunk_get_rad(att.chunk) * 0.5f;
                        l.pos = l.absolute_hunt_pos;
                    } else if (i == 3 && behind) {
                        flag3 = true;
                        auto const bm = main(state, g.behind_me);
                        l.absolute_hunt_pos = w.body_chunk_get_pos(bm) + a * side * w.body_chunk_get_rad(bm) * -0.5f;
                        l.pos = l.absolute_hunt_pos;
                    } else if (chain.in_chain && chain.head && i < (chain.count > 2 ? 2u : 1u)) {
                        l.dangle = 1;
                        l.vel += vector * 5.5f;
                        if (i == 1 && chain.has_prey)
                            l.vel += custom::dir_vec(l.pos, chain.prey_pos) * chain.hunt * 4.f;
                    } else if (chain.in_chain && att.chunk && !behind && i > (chain.count > 3 ? 1u : 2u)) {
                        l.dangle = 1;
                        l.vel += vector * 3.5f;
                    }
                } else {
                    l.dangle = 1;
                }
                if (!l.dangle) {
                    if (!flag3) {
                        if (mag < 1.f) {
                            if (rng.value() < 0.05f && !custom::dist_less(l.pos, ideal, num / 6.f))
                                find_grip(g, room, mp, i, j, ideal, num, mag);
                        } else if (flag2 && (g.last_legs_position != g.legs_position || i == 3) && !custom::dist_less(l.pos, ideal, num * 0.5f)) {
                            find_grip(g, room, mp, i, j, ideal, num, mag);
                        }
                    }
                } else {
                    l.vel += body_parts::rotate_around_origo(g.death_leg_positions[i][j], custom::aim(-g.body_dir, g.body_dir)) * 0.65f;
                    vec2 const r = custom::deg_to_vec(rng.value() * 360.f);
                    l.vel += r * x.death_spasms * 5.f;
                    l.vel += vector * 0.7f;
                    l.vel.y -= 0.8f;
                    g.limb_goal_distances[i][j] = 0.f;
                }
                l.hunt_speed = 15.f * mathf::inverse_lerp(-0.05f, 2.f, mag);
                limb_update(l);
                connect(l, mp, num, mv);
            }
        // the spider it holds on to has it behind (the front one reads it
        // on its next update when it already ran this tick)
        if (att_spider)
            gs(state, att.spider).behind_me = c;
        g.behind_me = dcon::creature_id{};
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>&) {
        auto const& w = state.world;
        auto const& g = gs(state, c);
        auto const& x = pl(state, c);
        vec3 const black = pal.black();
        auto const m = main(state, c);
        vec2 const at = rw::lerp(w.body_chunk_get_last_pos(m), w.body_chunk_get_pos(m), alpha);
        vec2 const dir = rw::slerp(g.last_body_dir, g.body_dir, alpha);
        vec2 const a = -custom::perpendicular_vector(dir);
        float const size = x.size;
        float const body_scale = mathf::lerp(0.2f, 1.f, size);
        sprites::add(out, sprite{"SpiderBody", at, custom::aim(-dir, dir), vec2{body_scale, body_scale}, vec2{0.5f, 0.5f}, black});
        // the legs: hip (vector3), knee (vector5), foot (vector4)
        vec2 hip[4][2], knee[4][2], foot[4][2];
        for (uint32_t l = 0; l < 4; ++l)
            for (uint32_t s = 0; s < 2; ++s) {
                float const side = -1.f + 2.f * float(s);
                vec2 v3 = at;
                v3 += dir * (7.f - float(l) * 0.5f - (l == 3 ? 1.5f : 0.f)) * size;
                v3 += a * (3.f + float(l) * 0.5f - (l == 3 ? 5.5f : 0.f)) * side * size;
                limb const& lm = g.limbs[l][s];
                vec2 v4 = rw::lerp(lm.last_pos, lm.pos, alpha);
                v4 = rw::lerp(v4, v3 + dir * g.limb_length * 0.1f,
                    std::sin(mathf::inverse_lerp(0.f, g.limb_goal_distances[l][s], rw::distance(v4, lm.absolute_hunt_pos)) * mathf::pi) * 0.4f);
                float const num = limb_lengths[l][0] * limb_lengths[l][1] * g.limb_length;
                float const num2 = limb_lengths[l][0] * (1.f - limb_lengths[l][1]) * g.limb_length;
                float const num3 = rw::distance(v3, v4);
                float num4 = l < 3 ? 1.f : -1.f;
                if (l == 2)
                    num4 *= 0.7f;
                if (x.legs_position != 0.f)
                    num4 = 1.f - 2.f * mathf::pow(0.5f + 0.5f * x.legs_position, 0.65f);
                num4 *= side;
                float const cosine = num3 > 0.f && num > 0.f ? (num3 * num3 + num * num - num2 * num2) / (2.f * num3 * num) : 0.98f;
                float const num5 = std::acos(mathf::clamp(cosine, 0.2f, 0.98f)) * 57.295776f * num4;
                hip[l][s] = v3;
                foot[l][s] = v4;
                knee[l][s] = v3 + custom::deg_to_vec(custom::aim(v3, v4) + num5) * num;
            }
        // LimbSprite(limb, side, segment) = 1 + limb + segment * 4 + side * 8
        for (uint32_t s = 0; s < 2; ++s) {
            float const sx = (s == 0 ? 1.f : -1.f) * mathf::lerp(0.45f, 0.65f, size);
            for (uint32_t l = 0; l < 4; ++l) {
                float const sy = limb_lengths[l][0] * limb_lengths[l][1] * g.limb_length / leg_sprite_sizes[l][0];
                sprites::add(out, sprite{leg_a[l], hip[l][s], custom::aim(hip[l][s], knee[l][s]), vec2{sx, sy}, vec2{0.5f, 1.f / leg_sprite_sizes[l][0]}, black});
            }
            for (uint32_t l = 0; l < 4; ++l) {
                float const sy = limb_lengths[l][0] * limb_lengths[l][1] * g.limb_length / leg_sprite_sizes[l][1];
                sprites::add(out, sprite{leg_b[l], knee[l][s], custom::aim(knee[l][s], foot[l][s]), vec2{sx, sy}, vec2{0.5f, 1.f / leg_sprite_sizes[l][1]}, black});
            }
        }
    }
}
