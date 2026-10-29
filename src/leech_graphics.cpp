#include <cmath>

#include "leech_graphics.hpp"
#include "body_parts.hpp"
#include "leech.hpp"
#include "physics.hpp"
#include "player.hpp"

namespace leech_graphics {
    using rw::vec2;
    using rw::vec3;
    using world::room_view;
    using leech::variant;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        // GenericBodyPart(rad 1, surface friction 0.7, air friction 1)
        constexpr float part_surface_friction = 0.7f, part_air_friction = 1.f;

        uint32_t body_count(variant v) noexcept { return v == variant::sea ? 11u : 5u; }
        // LeechGraphics.Radius
        float radius(variant v, float at) noexcept {
            if (v == variant::sea)
                return 1.5f + std::sin(at * mathf::pi) * 1.5f;
            return 2.f + std::sin(at * mathf::pi);
        }

        creature::leech_payload const& pl(sys::game_state const& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).leech;
        }
        dcon::body_chunk_id main(sys::game_state const& state, dcon::creature_id c) {
            return leech::chunk(state, c, 0);
        }
    }

    void reset(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        auto& g = w.leech_data_get_graphics(creature::leech_data(w, c));
        variant const v = pl(state, c).variant;
        uint32_t const n = body_count(v);
        g = leech::graphics_state{};
        w.creature_get_graphics_random(c) = rw::rng{w.creature_get_random(c).state ^ 0x1EEC4u};
        auto& rng = w.creature_get_graphics_random(c);
        for (uint32_t j = 0; j < n; ++j)
            g.radiuses[j] = radius(v, float(j) / float(n));
        g.sin_counter = rng.value();
        // Reset: the body bunched up on the chunk
        vec2 const at = w.body_chunk_get_pos(main(state, c));
        vec2 const vel = w.body_chunk_get_vel(main(state, c));
        for (uint32_t j = 0; j < n; ++j) {
            auto& b = g.body[j];
            b.rad = 1.f;
            b.pos = at + custom::deg_to_vec(rng.value() * 360.f);
            b.last_pos = b.pos;
            b.vel = vel;
        }
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        auto const keep_random = w.creature_get_graphics_random(c);
        float const keep_sin = w.leech_data_get_graphics(creature::leech_data(w, c)).sin_counter;
        reset(state, c);
        w.creature_set_graphics_random(c, keep_random);
        w.leech_data_get_graphics(creature::leech_data(w, c)).sin_counter = keep_sin;
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto& g = w.leech_data_get_graphics(creature::leech_data(w, c));
        auto& rng = w.creature_get_graphics_random(c);
        auto const& x = pl(state, c);
        variant const v = x.variant;
        bool const sea = v == variant::sea;
        uint32_t const n = body_count(v);
        auto const m = main(state, c);
        vec2 const mp = w.body_chunk_get_pos(m);
        vec2 const mv = w.body_chunk_get_vel(m);
        float const sub = physics::submersion(state, room, m);
        bool const on_ground = sub == 0.f && w.body_chunk_get_contact_point(m).y == -1;
        bool const dead = w.creature_get_dead(c) != 0;
        bool const awake = leech::consious(state, c);
        bool const attached = w.creature_get_grasp_player(c)[0] != 0;
        bool const attacking = x.charge_counter > 15;
        bool const charging = x.charge_counter > 0 && x.charge_counter < 15;
        constexpr float seg = 4.f;
        if (!dead && !charging && !attacking)
            g.sin_counter -= 0.06666667f;
        if (g.vibrate > 0)
            --g.vibrate;
        vec2 const side = custom::perpendicular_vector(x.swim_direction);
        float const damp = mathf::lerp(0.99f, 0.8f, sub);
        float const fall = 0.9f * (1.f - sub);
        for (uint32_t j = n; j-- > 0;)
            body_parts::generic_update(g.body[j], room, part_air_friction, part_surface_friction);
        for (uint32_t j = n; j-- > 0;) {
            auto& b = g.body[j];
            b.vel *= damp;
            b.vel.y -= fall;
            if (on_ground) {
                b.vel.x -= float(x.land_walk_dir) * x.land_walk_cycle;
                if (j == 2 || j == 1)
                    b.vel.y += fall * (1.f + std::sin(x.land_walk_cycle * mathf::pi * 2.f));
            }
            if (!custom::dist_less(b.pos, mp, 7.f * float(j + 1)))
                b.pos = mp + custom::dir_vec(mp, b.pos) * 7.f * float(j + 1);
            vec2 const before = j > 0 ? g.body[j - 1].pos : mp;
            b.vel += custom::dir_vec(before, b.pos) * 0.1f - x.swim_direction * (sea ? 0.005f : 0.05f);
            // latched on or drowning: writhing
            if (!dead && (attached || x.air_drown > 0.7f) && rng.value() < 0.1f) {
                vec2 const r = custom::deg_to_vec(rng.value() * 360.f);
                float const a = rng.value();
                b.vel += r * a * rng.value();
            }
            if (j == 0) {
                b.pos = mp + x.swim_direction * 4.f;
                if (awake) {
                    if (!attached && sub > 0.5f)
                        b.pos += side * std::sin(g.sin_counter * mathf::pi * 2.f) * 2.f;
                    else if (on_ground)
                        b.pos += vec2{1.5f * std::sin(x.land_walk_cycle * mathf::pi * 2.f) * -float(x.land_walk_dir), -3.f};
                }
                b.vel = mv;
                continue;
            }
            auto& f = g.body[j - 1];
            float const dist = rw::distance(b.pos, f.pos);
            vec2 const a = custom::dir_vec(b.pos, f.pos);
            b.vel -= a * (seg - dist) * 0.5f;
            b.pos -= a * (seg - dist) * (sea ? 0.5f : 0.15f);
            f.vel += a * (seg - dist) * 0.5f;
            f.pos += a * (seg - dist) * (sea ? 0.5f : 0.15f);
            g.radiuses[j] = radius(v, float(j) / float(n));
            if (!attacking) {
                if (sub == 1.f && !attached)
                    b.vel += side * std::sin((g.sin_counter + float(j) / (sea ? 5.f : 3.f)) * mathf::pi * 2.f) * (sea ? 1.2f : 0.2f);
                g.radiuses[j] *= 1.f + std::sin((g.sin_counter + float(j) / 4.f) * mathf::pi * 2.f) * 0.2f;
            }
            if (dist > seg)
                g.radiuses[j] /= 1.f + mathf::pow((dist - seg) * (sea ? 0.06f : 0.1f), 1.5f);
            else if (!sea)
                g.radiuses[j] *= mathf::clamp(mathf::pow(1.f + (seg - dist) * 1.5f, 0.5f), 1.f, 3.f);
        }
        if (sea) {
            for (uint32_t k = 1; k < n; ++k) {
                auto& b = g.body[k];
                auto& f = g.body[k - 1];
                float const dist = rw::distance(b.pos, f.pos);
                vec2 const a = custom::dir_vec(b.pos, f.pos);
                b.vel -= a * (seg - dist) * 0.5f;
                b.pos -= a * (seg - dist) * 0.5f;
                f.vel += a * (seg - dist) * 0.5f;
                f.pos += a * (seg - dist) * 0.5f;
            }
        }
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, vec3 black, std::vector<sprites::draw_cmd>& out) {
        auto const& w = state.world;
        auto const& g = w.leech_data_get_graphics(creature::leech_data(w, c));
        auto const& x = pl(state, c);
        variant const v = x.variant;
        uint32_t const n = body_count(v);
        vec2 at = rw::lerp(g.body[0].last_pos, g.body[0].pos, alpha) + x.swim_direction * 2.f;
        float prev_rad = g.radiuses[0];
        // latched on: the mouth on the slugcat's skin
        if (auto const p = leech::attached_to(state, c)) {
            auto const k = player::body_chunk(state, p, w.creature_get_grasp_chunk(c)[0]);
            vec2 const kp = w.body_chunk_get_pos(k);
            at = kp + custom::dir_vec(kp, w.body_chunk_get_pos(main(state, c))) * w.body_chunk_get_rad(k) * 0.6f;
            prev_rad = 0.f;
        }
        auto const black_to = [&](vec3 col, float t) { return col + (black - col) * mathf::clamp01(t); };
        vec3 col;
        if (v == variant::sea)
            col = black_to(vec3{0.f, 0.4f, 0.8f}, 0.85f);
        else if (v == variant::jungle)
            col = black_to(vec3{0.1f, 0.8f, 0.2f}, 0.85f);
        else
            col = black_to(vec3{0.8f, 0.f, 0.1f}, 0.5f + 0.5f * mathf::inverse_lerp(0.05f, 0.2f, x.air_drown));
        vec2 vs[leech::max_body * 4];
        for (uint32_t i = 0; i < n; ++i) {
            // (Vibrate's shake comes from snail clicks: no snails here)
            vec2 const p = rw::lerp(g.body[i].last_pos, g.body[i].pos, alpha);
            vec2 const d = rw::normalized(p - at);
            vec2 const a = custom::perpendicular_vector(d);
            float const along = rw::distance(p, at) / 5.f;
            vs[i * 4] = at - a * (prev_rad + g.radiuses[i]) * 0.5f + d * along;
            vs[i * 4 + 1] = at + a * (prev_rad + g.radiuses[i]) * 0.5f + d * along;
            if (i < n - 1) {
                vs[i * 4 + 2] = p - a * g.radiuses[i] - d * along;
                vs[i * 4 + 3] = p + a * g.radiuses[i] - d * along;
            } else {
                vs[i * 4 + 2] = p;
            }
            prev_rad = g.radiuses[i];
            at = p;
        }
        sprites::long_mesh(out, vs, size_t(n - 1) * 4 + 3, col);
    }
}
