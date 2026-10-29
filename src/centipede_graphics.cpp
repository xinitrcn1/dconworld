#include <algorithm>
#include <cmath>

#include "centipede_graphics.hpp"
#include "body_parts.hpp"
#include "centipede.hpp"
#include "pathing.hpp"
#include "physics.hpp"

namespace centipede_graphics {
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using world::room_view;
    using player::limb_mode;
    using sprites::sprite;
    using centipede::variant;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        // Limb(rad 2, surface friction 0.5, air friction 0.9, hunt speed 7, quickness 0.8)
        constexpr body_parts::limb_params leg{2.f, 0.5f, 0.9f, 7.f, 0.8f};
        // GenericBodyPart(rad 1, surface friction 0.5, air friction 0.9)
        constexpr float whisker_rad = 1.f, whisker_surface_friction = 0.5f, whisker_air_friction = 0.9f;

        bool consious(dcon::data_container const& w, dcon::creature_id c) {
            return w.creature_get_stun(c) <= 0 && !w.creature_get_dead(c);
        }

        vec2 chunk_pos(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_pos(centipede::chunk(state, c, i));
        }
        // ChunkDrawPos
        vec2 chunk_draw_pos(sys::game_state const& state, dcon::creature_id c, uint32_t i, float t) {
            auto const k = centipede::chunk(state, c, i);
            return rw::lerp(state.world.body_chunk_get_last_pos(k), state.world.body_chunk_get_pos(k), t);
        }

        // BodyPart.Reset
        template<typename P>
        void reset_part(P& bp, vec2 at, rw::rng& rng) {
            bp.pos = at + custom::deg_to_vec(rng.value() * 360.f);
            bp.last_pos = bp.pos;
            bp.vel = vec2{0.f, 0.f};
        }

        // WhiskerLength
        float whisker_length(variant v, float size, int part) {
            if (v == variant::aqua)
                return 72.f;
            if (v == variant::red)
                return part == 0 ? 44.f : 48.f;
            return (part == 0 ? 17.f : 43.f) * mathf::lerp(0.5f, 1.5f, size) * (v == variant::small ? 0.75f : 1.f);
        }

        // RotatAtChunk
        vec2 rotat_at_chunk(centipede::graphics_state const& g, uint32_t n, uint32_t chunk, float t) {
            auto const rot = [&](int i) { return rw::slerp(g.body_rotations[i][1], g.body_rotations[i][0], t); };
            uint32_t const half = n / 2;
            if (chunk <= half)
                return rw::slerp(rot(0), rot(1), mathf::inverse_lerp(0.f, float(half), float(chunk)));
            return rw::slerp(rot(1), rot(2), mathf::inverse_lerp(float(half), float(n - 1), float(chunk)));
        }

        // WhiskerDir
        vec2 whisker_dir(sys::game_state const& state, dcon::creature_id c, centipede::graphics_state const& g, uint32_t n, int end, int side, int part, float t) {
            vec2 dir, rot;
            if (end == 0) {
                dir = custom::dir_vec(chunk_draw_pos(state, c, 1, t), chunk_draw_pos(state, c, 0, t));
                rot = rotat_at_chunk(g, n, 0, t);
            } else {
                dir = custom::dir_vec(chunk_draw_pos(state, c, n - 2, t), chunk_draw_pos(state, c, n - 1, t));
                rot = rotat_at_chunk(g, n, n - 1, t);
            }
            vec2 const a = custom::perpendicular_vector(dir) * (end == 0 ? -1.f : 1.f);
            return rw::normalized(dir + rw::slerp(a * (side == 0 ? -1.f : 1.f) * rot.y * (part == 0 ? 0.4f : 1.4f),
                a * rot.x * (part == 0 ? 0.25f : -0.5f), std::fabs(rot.x)));
        }

        // BestBodyRotatAtChunk: the side of the body facing away from the
        // surface it is on
        vec2 best_body_rotat_at_chunk(sys::game_state const& state, dcon::creature_id c, centipede::graphics_state const& g, uint32_t n, uint32_t chunk, room_view const& room) {
            vec2 v;
            if (chunk == 0)
                v = custom::dir_vec(chunk_pos(state, c, 0), chunk_pos(state, c, 1));
            else if (chunk == n - 1)
                v = custom::dir_vec(chunk_pos(state, c, n - 2), chunk_pos(state, c, n - 1));
            else
                v = custom::dir_vec(chunk_pos(state, c, n / 2 - 1), chunk_pos(state, c, n / 2 + 1));
            v = custom::perpendicular_vector(v);
            vec2 const p = chunk_pos(state, c, chunk);
            float num = 0.f;
            if (room.get_tile(p + v * 20.f).solid())
                num += 1.f;
            if (room.get_tile(p - v * 20.f).solid())
                num -= 1.f;
            if (num == 0.f && (room.get_tile(p).vertical_beam() || room.get_tile(p).horizontal_beam()))
                return rw::normalized(vec2{g.default_rotat * 0.01f, -1.f});
            return rw::slerp(rw::normalized(vec2{-1.f, 0.15f}), rw::normalized(vec2{1.f, 0.15f}), mathf::inverse_lerp(-1.f, 1.f, num + g.default_rotat * 0.01f));
        }

        // VerticalWingFlapAtChunk / HorizontalWingFlapAtChunk
        float vertical_wing_flap(centipede::graphics_state const& g, uint32_t chunk, float t) {
            return std::sin((mathf::lerp(g.last_wing_flap_cycle, g.wing_flap_cycle, t) + float(chunk) * 1.8f) * mathf::pi * 0.3f)
                * (1.f - mathf::lerp(g.last_wings_folded, g.wings_folded, t));
        }
        float horizontal_wing_flap(centipede::graphics_state const& g, uint32_t chunk, vec2 rot, float t) {
            return std::cos((mathf::lerp(g.last_wing_flap_cycle, g.wing_flap_cycle, t) + float(chunk) * custom::lerp_map(rot.y, -1.f, 1.f, 1.8f, 0.6f)) * mathf::pi * 0.3f)
                * (1.f - mathf::lerp(g.last_wings_folded, g.wings_folded, t));
        }

        // WingPos: where a wing's tip is, flapping or folded back along the body
        vec2 wing_pos(sys::game_state const& state, dcon::creature_id c, centipede::graphics_state const& g, uint32_t n, int side, uint32_t wing, vec2 dr, vec2 prp,
            vec2 rot, float t) {
            float const along = float(wing) / float(n - 1);
            float const f = vertical_wing_flap(g, wing, t);
            float const h = horizontal_wing_flap(g, wing, rot, t);
            vec2 const b = dr * mathf::lerp(mathf::lerp(-1.f, 1.f, along), h * rot.y, 0.5f * std::fabs(rot.y)) * std::fabs(rot.y);
            vec2 v = rw::lerp(prp * (side == 0 ? -1.f : 1.f) * rot.y, prp * mathf::pow(std::fabs(f), 0.5f) * mathf::sign(f) * std::fabs(rot.x), std::fabs(rot.x)) + b;
            v = rw::lerp(v, rw::normalized(v), mathf::pow(std::fabs(rot.y), 2.f));
            vec2 const folded = (dr * mathf::lerp(1.f, -1.f, along) + rw::lerp(prp * (side == 0 ? -1.f : 1.f) * rot.y, prp * -rot.x, std::fabs(rot.x))) * 0.5f;
            v = rw::lerp(v, folded, mathf::lerp(g.last_wings_folded, g.wings_folded, t));
            vec2 const at = chunk_draw_pos(state, c, wing, t);
            if (centipede::variant_of(state.world, c) == variant::aqua) {
                vec2 const sway = custom::perpendicular_vector(at, at + v)
                    * std::cos((mathf::lerp(g.last_wing_swim_cycle, g.wing_swim_cycle, t) + float(wing) * custom::lerp_map(rot.y, -1.f, 1.f, 1.8f, 0.6f)) * mathf::pi * 0.3f)
                    * (g.wing_lengths[wing] / 4.f);
                return at + v * g.wing_lengths[wing] + sway;
            }
            return at + v * g.wing_lengths[wing];
        }
    }

    void reset(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        auto const d = creature::centipede_data(w, c);
        auto& g = w.centipede_data_get_graphics(d);
        uint32_t const n = std::min(centipede::chunk_count(state, c), centipede::max_segments);
        variant const v = centipede::variant_of(w, c);
        float const size = w.creature_get_payload(c).centipede.size;
        g = centipede::graphics_state{};
        w.creature_get_graphics_random(c) = rw::rng{w.creature_get_random(c).state ^ 0x5bd1e995u};
        auto& rng = w.creature_get_graphics_random(c);
        // the constructor's layout, from Random.InitState(ID.RandomSeed)
        rw::unity_random r{w.centipede_data_get_seed(d)};
        g.default_rotat = mathf::lerp(-5.f, 5.f, r.value());
        for (auto& rot : g.body_rotations)
            rot[0] = rot[1] = custom::deg_to_vec(g.default_rotat);
        float hue = 0.f;
        switch (v) {
        case variant::wing:
            hue = mathf::lerp(0.28f, 0.38f, r.value());
            g.saturation = 0.5f;
            for (uint32_t j = 0; j < n; ++j) {
                float const t = float(j) / float(n - 1);
                float a = std::sin(mathf::pow(mathf::inverse_lerp(0.5f, 0.f, t), 0.75f) * mathf::pi) * (1.f - t);
                float b = std::sin(mathf::pow(mathf::inverse_lerp(1.f, 0.5f, t), 0.75f) * mathf::pi) * t;
                a = 0.5f + 0.5f * a;
                b = 0.5f + 0.5f * b;
                g.wing_lengths[j] = mathf::lerp(3.f, custom::lerp_map(size, 0.5f, 1.f, 60.f, 80.f), mathf::max(a, b) - std::sin(t * mathf::pi) * 0.25f);
            }
            break;
        case variant::aqua:
            hue = mathf::lerp(0.5f, 0.6f, r.value());
            g.saturation = mathf::lerp(0.8f, 1.f, r.value());
            for (uint32_t j = 0; j < n; ++j) {
                float const t = float(j) / float(n - 1);
                float a = std::sin(mathf::pow(mathf::inverse_lerp(0.4f, 0.f, t), 0.75f) * mathf::pi) * (1.f - t);
                float b = std::cos(mathf::pow(mathf::inverse_lerp(0.6f, 0.4f, t), 0.75f) * mathf::pi) * t;
                a = 0.5f + 0.5f * a;
                b = 0.5f + 0.5f * b;
                g.wing_lengths[j] = mathf::clamp(mathf::lerp(3.f, custom::lerp_map(size, 0.5f, 1.f, 100.f, 130.f), mathf::max(a, b)) * 0.75f, 60.f, 80.f);
            }
            break;
        case variant::red:
            hue = mathf::lerp(-0.02f, 0.01f, r.value());
            g.saturation = 0.9f + 0.1f * r.value();
            break;
        default:
            hue = mathf::lerp(0.04f, 0.1f, r.value());
            g.saturation = 0.9f;
            break;
        }
        w.centipede_data_set_hue(d, hue);
        // InitiateSprites' rolls (UnityEngine.Random there, here still the
        // seeded layout's): the aquapede's AquapedeBody alphas and wing elements
        if (v == variant::aqua) {
            for (uint32_t i = 0; i < n; ++i)
                g.aqua_body_alpha[i] = r.value();
            for (auto& side : g.aqua_wing)
                for (uint32_t i = 0; i < n; ++i)
                    side[i] = uint8_t(r.range(0, 4));
        }
        for (uint32_t i = 0; i < n; ++i) {
            float const t = float(i) / float(n - 1);
            g.leg_lengths[i] = mathf::lerp(10.f, 25.f, std::sin(t * mathf::pi)) * mathf::lerp(0.5f, 1.5f, size) * (v == variant::wing ? 0.65f : 1.f);
            for (auto& l : g.legs[i]) {
                l = player::hand_state{};
                reset_part(l, chunk_pos(state, c, i), rng);
                l.mode = limb_mode::hunt_relative_position;
                l.hunt_speed = leg.hunt_speed;
                l.quickness = leg.quickness;
                l.absolute_hunt_pos = l.pos;
            }
        }
        for (int end = 0; end < 2; ++end)
            for (auto& part : g.whiskers[end])
                for (auto& wh : part) {
                    wh.rad = whisker_rad;
                    reset_part(wh, chunk_pos(state, c, end == 0 ? 0 : n - 1), rng);
                }
        g.wings_folded = g.last_wings_folded = 1.f - w.centipede_data_get_wings_started_up(d);
    }

    void reset_positions(sys::game_state& state, dcon::creature_id c) {
        // the layout comes from the seed again; the graphics' generator goes on
        auto& w = state.world;
        auto const keep_random = w.creature_get_graphics_random(c);
        reset(state, c);
        w.creature_set_graphics_random(c, keep_random);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto const d = creature::centipede_data(w, c);
        auto& g = w.centipede_data_get_graphics(d);
        uint32_t const n = std::min(centipede::chunk_count(state, c), centipede::max_segments);
        if (n < 2)
            return;
        auto const& pl = w.creature_get_payload(c).centipede;
        bool const awake = consious(w, c);
        bool const moving = pl.moving != 0;
        bool const flying = pl.flying != 0;
        bool const body_direction = pl.body_direction != 0;
        float const size = pl.size;
        variant const v = pl.variant;
        auto& rng = w.creature_get_graphics_random(c);

        // soundLoop (ChunkDynamicSoundLoop on the main chunk): the charge's
        // crackle, the centiwing's buzz while it flies, else the legs' crawl
        {
            auto const main = centipede::chunk(state, c, n / 2);
            float const charge = pl.shock_charge;
            auto const key = sound::loop_key(sound::loop_owner::creature, uint32_t(c.index()));
            vec2 const at = w.body_chunk_get_pos(main);
            if (charge > 0.f) {
                state.play_loop(key, sound::sound_id::centipede_electric_charge_loop, room.id, at,
                    mathf::inverse_lerp(0.f, 0.1f, charge), mathf::lerp(0.5f, 1.5f, charge));
            } else if (w.creature_get_dead(c)) {
            } else if (flying) {
                float const up = rw::dot(rw::normalized(pl.move_to_pos - at), vec2{0.f, 1.f});
                state.play_loop(key, sound::sound_id::centiwing_fly_loop, room.id, at, mathf::lerp(0.75f, 1.f, size),
                    mathf::inverse_lerp(1.2f, 0.8f, size) * custom::lerp_map(up, 0.f, 1.f, 1.f, 1.4f, 1.5f));
            } else if (moving) {
                // InverseLerp(distance, 0.5, 2) as vanilla has it (the
                // arguments swapped: loud when crawling slowly or fast)
                state.play_loop(key, sound::sound_id::centipede_crawl_loop, room.id, at,
                    mathf::inverse_lerp(rw::distance(w.body_chunk_get_last_pos(main), at), 0.5f, 2.f) * mathf::lerp(0.5f, 1.f, size),
                    mathf::lerp(1.2f, 0.8f, size));
            }
        }
        if (moving && awake)
            g.walk_cycle += (body_direction ? -1.f : 1.f) / 10.f;
        g.last_wing_flap_cycle = g.wing_flap_cycle;
        g.last_wing_swim_cycle = g.wing_swim_cycle;
        if (awake) {
            g.wing_flap_cycle += mathf::pow(w.centipede_data_get_wings_started_up(d), 3.f);
            if (physics::object_submersion(state, room, centipede::body(state, c)) > 0.f)
                g.wing_swim_cycle = 0.f;
            else
                g.wing_swim_cycle = mathf::lerp(g.wing_swim_cycle, 0.f, 0.05f);
        } else {
            g.wing_swim_cycle = mathf::lerp(g.wing_swim_cycle, 0.f, 0.05f);
        }
        g.last_wings_folded = g.wings_folded;
        g.wings_folded = 1.f - w.centipede_data_get_wings_started_up(d);
        g.last_body_dir = g.body_dir;
        g.body_dir = mathf::lerp(g.body_dir, body_direction ? -1.f : 1.f, 0.1f);
        uint32_t const head = centipede::head_index(state, c);
        for (int i = 0; i < 3; ++i) {
            g.body_rotations[i][1] = g.body_rotations[i][0];
            uint32_t const chunk = i == 0 ? 0 : (i == 1 ? n / 2 : n - 1);
            // (vanilla tests the terrain at chunk i, not at `chunk`)
            if (!flying || pathing::ai_terrain_proximity(state, room.id, room_view::get_tile_position(chunk_pos(state, c, uint32_t(i)))) < 2) {
                g.body_rotations[i][0] = rw::slerp(g.body_rotations[i][0], best_body_rotat_at_chunk(state, c, g, n, chunk, room), moving ? 0.4f : 0.01f);
            } else {
                // flying: the back up, the head up most
                vec2 a{0.f, 0.f};
                if (chunk > 0)
                    a += custom::dir_vec(chunk_pos(state, c, chunk), chunk_pos(state, c, chunk - 1));
                if (chunk < 2)
                    a -= custom::dir_vec(chunk_pos(state, c, chunk), chunk_pos(state, c, chunk + 1));
                a += vec2{0.f, chunk == head ? 0.8f : 0.1f};
                g.body_rotations[i][0] = rw::slerp(g.body_rotations[i][0], rw::normalized(a), chunk == head ? 0.5f : 0.1f);
            }
        }

        // the legs reach for a grip, else dangle out to the sides (always
        // while flying)
        vec2 const p = chunk_pos(state, c, 0) + custom::dir_vec(chunk_pos(state, c, 1), chunk_pos(state, c, 0));
        for (uint32_t j = 0; j < n; ++j) {
            auto const k = centipede::chunk(state, c, j);
            float const t = float(j) / float(n - 1);
            vec2 const pos = w.body_chunk_get_pos(k);
            vec2 const vel = w.body_chunk_get_vel(k);
            float const rad = w.body_chunk_get_rad(k);
            vec2 const dir = custom::dir_vec(p, pos);
            vec2 const perp = custom::perpendicular_vector(dir);
            vec2 const rot = rotat_at_chunk(g, n, j, 1.f);
            float const step = 0.5f + 0.5f * std::sin((g.walk_cycle + float(j) / 10.f) * mathf::pi * 2.f);
            float const len = g.leg_lengths[j];
            for (int s = 0; s < 2; ++s) {
                auto& l = g.legs[j][s];
                float const side = s == 0 ? -1.f : 1.f;
                body_parts::limb_update(l, leg, pos, vel, pos, room);
                vec2 const hip = pos + perp * side * rot.y * rad;
                float f = mathf::lerp(-1.f, 1.f, t);
                f = mathf::lerp(f, body_direction ? -1.f : 1.f, std::fabs(step - 0.5f));
                vec2 const reach = rw::normalized(rw::slerp(dir * f, rw::slerp(perp * side * rot.y, perp * rot.x, std::fabs(rot.x)),
                    mathf::lerp(0.5f + 0.5f * std::sin(t * mathf::pi), 0.f, std::fabs(step - 0.5f) * 2.f)));
                vec2 const goal = hip + reach * len;
                body_parts::as_body_part(l, leg.rad, [&](player::body_part& bp) { body_parts::connect_to_point(bp, hip, len, false, 0.f, vel, 0.1f, 0.f); });
                if (flying) {
                    l.mode = limb_mode::dangle;
                    l.vel += reach * 6.f;
                    l.vel = rw::lerp(l.vel, goal - l.pos, 0.5f);
                    continue;
                }
                if (awake && !l.reached_snap_position)
                    body_parts::find_grip(l, room, hip, hip, len * 1.5f, goal, -2, -2, true);
                if (!awake || !custom::dist_less(l.pos, l.absolute_hunt_pos, len * 1.5f)) {
                    l.mode = limb_mode::dangle;
                    l.vel += reach * 13.f;
                    l.vel = rw::lerp(l.vel, goal - l.pos, 0.5f);
                } else {
                    l.vel += reach * 5.f;
                }
            }
        }
        // the feelers, twitching while it stands still
        for (int end = 0; end < 2; ++end) {
            vec2 const root = chunk_pos(state, c, end == 0 ? 0 : n - 1);
            for (int side = 0; side < 2; ++side)
                for (int part = 0; part < 2; ++part) {
                    auto& wh = g.whiskers[end][part][side];
                    float const len = whisker_length(v, size, part);
                    vec2 const dd = whisker_dir(state, c, g, n, end, side, part, 1.f);
                    body_parts::generic_update(wh, room, whisker_air_friction, whisker_surface_friction);
                    body_parts::connect_to_point(wh, root, len, false, 0.f, vec2{0.f, 0.f}, 0.f, 0.f);
                    wh.vel += (root + dd * len - wh.pos) / 30.f;
                    wh.vel += dd;
                    wh.vel.y -= 0.3f;
                    if (awake && !moving)
                        wh.pos += custom::deg_to_vec(rng.value() * 360.f) * mathf::lerp(0.5f, 1.5f, size) * (body_direction == (end == 0) ? 2.f : 0.8f);
                }
        }
    }

    void build(sys::game_state const& state, dcon::creature_id c, float alpha, vec3 black, float darkness, std::vector<sprites::draw_cmd>& out) {
        auto const& w = state.world;
        auto const d = creature::centipede_data(w, c);
        auto const& g = w.centipede_data_get_graphics(d);
        uint32_t const n = std::min(centipede::chunk_count(state, c), centipede::max_segments);
        if (n < 2)
            return;
        float const size = w.creature_get_payload(c).centipede.size;
        variant const v = centipede::variant_of(w, c);
        bool const small = v == variant::small, red = v == variant::red, aqua = v == variant::aqua;
        bool const winged = v == variant::wing || aqua;
        uint32_t const bites = w.centipede_data_get_bites(d);
        float const hue = w.centipede_data_get_hue(d);
        auto const rad_of = [&](uint32_t i) { return w.body_chunk_get_rad(centipede::chunk(state, c, i)); };
        auto const black_to = [&](vec3 col, float t) { return col + (black - col) * mathf::clamp01(t); };
        vec3 const secondary_shell = black_to(custom::hsl2rgb(hue, g.saturation, 0.3f), 0.3f + 0.7f * darkness);
        auto const visible = [&](uint32_t i) { return small ? bites > i : centipede::has_shell(state, c, i); };
        // the sprites, gathered per kind and added in vanilla sprite order
        constexpr uint32_t m = centipede::max_segments;
        sprite secondary[m], segments[m], shells[m][2], legs[m][2][2], wings_back[m], wings_front[m];
        bool secondary_on[m] = {}, segment_on[m] = {}, shell_on[m] = {}, back_on[m] = {}, front_on[m] = {};
        vec2 tube[m * 4];

        vec2 prev_dir{0.f, 0.f};
        vec2 prev = chunk_draw_pos(state, c, 0, alpha);
        prev += custom::dir_vec(chunk_draw_pos(state, c, 1, alpha), prev) * 10.f;
        for (uint32_t i = 0; i < n; ++i) {
            float const t = float(i) / float(n - 1);
            float const rad = rad_of(i);
            vec2 const rot = rw::normalized(rotat_at_chunk(g, n, i, alpha));
            vec2 const pos = chunk_draw_pos(state, c, i, alpha);
            vec2 const next = i < n - 1 ? chunk_draw_pos(state, c, i + 1, alpha) : pos + custom::dir_vec(prev, pos) * 10.f;
            vec2 const dir = rw::normalized(prev - next);
            vec2 const perp = custom::perpendicular_vector(dir);
            float const d_prev = rw::distance(pos, prev) / 4.f;
            float const d_next = rw::distance(pos, next) / 4.f;
            float w_prev = 3.f, w_next = 3.f;
            if (i == 0 || small)
                w_prev = 1.f;
            else if (i == n - 1)
                w_next = 1.f;
            tube[i * 4] = pos - perp * w_prev + dir * d_prev;
            tube[i * 4 + 1] = pos + perp * w_prev + dir * d_prev;
            tube[i * 4 + 2] = pos - perp * w_next - dir * d_next;
            tube[i * 4 + 3] = pos + perp * w_next - dir * d_next;
            float flat = mathf::clamp(std::sin(t * mathf::pi), 0.f, 1.f) * mathf::lerp(1.f, 0.5f, size);
            if (v == variant::wing)
                flat = mathf::lerp(0.6f, 0.3f, mathf::pow(mathf::clamp01(std::sin(t * mathf::pi)), 2.f));
            else if (aqua)
                flat = mathf::lerp(0.8f, 0.2f, mathf::pow(mathf::clamp01(std::sin(t * mathf::pi)), 2.f));
            float const deg = custom::vec_to_deg(dir);
            // a baby centipede shrinks bite by bite
            segment_on[i] = !small || bites > i;
            shell_on[i] = visible(i);

            segments[i] = sprite{"CentipedeSegment", pos, deg,
                vec2{rad * mathf::lerp(1.f, mathf::lerp(1.5f, 0.9f, std::fabs(rot.x)), flat) * 2.f * 0.0625f, rad * 1.8f * 0.083333336f}, vec2{0.5f, 0.5f}, black};
            // the back shell while the back faces up, else the belly (the
            // aquapede's second shell sprite through the AquapedeBody shader)
            for (int k = 0; k < (aqua ? 2 : 1); ++k) {
                sprite& sh = shells[i][k];
                sh.rotation = deg;
                if (rot.y > 0.f) {
                    float shine = mathf::inverse_lerp(-0.5f, 0.5f, rw::dot(dir, custom::deg_to_vec(30.f) * rot.x));
                    shine *= mathf::max(mathf::inverse_lerp(0.3f, 0.05f, std::fabs(-0.5f - rot.x)), mathf::inverse_lerp(0.3f, 0.05f, std::fabs(0.5f - rot.x)));
                    shine *= mathf::pow(1.f - darkness, 2.f);
                    sh.element = "CentipedeBackShell";
                    sh.scale = vec2{rad * mathf::lerp(1.f, mathf::lerp(1.5f, 0.9f, std::fabs(rot.x)), flat) * 1.8f * rot.y * 0.071428575f, rad * (red ? 1.7f : 1.5f) * 0.09090909f};
                    if (k == 0) {
                        sh.color = black_to(custom::hsl2rgb(hue, g.saturation, 0.5f + 0.25f * shine), darkness);
                    } else {
                        sh.color = black_to(custom::hsl2rgb(hue, g.saturation, 0.5f + 0.25f * shine), 0.5f);
                        sh.color = sh.color + (vec3{0.4392157f, 0.07450981f, 0.f} - sh.color) * 0.25f;
                    }
                    sh.pos = pos + perp * rot.x * rad * 1.1f;
                } else {
                    sh.element = "CentipedeBellyShell";
                    sh.scale = vec2{rad * -1.8f * rot.y * 0.071428575f, rad * 1.3f * 0.09090909f};
                    sh.color = k == 0 ? secondary_shell : black;
                    sh.pos = pos - perp * rot.x * rad;
                }
            }
            if (i > 0) {
                // a 1x1 pixel stretched over the joint
                secondary_on[i - 1] = !small || bites > i;
                secondary[i - 1] = sprite{"pixel", rw::lerp(prev, pos, 0.5f), custom::vec_to_deg(rw::slerp(prev_dir, dir, 0.5f)),
                    vec2{rad * mathf::lerp(0.9f, mathf::lerp(1.1f, 0.8f, std::fabs(rot.x)), flat) * 2.f, rad_of(i - 1)}, vec2{0.5f, 0.5f},
                    black_to(custom::hsl2rgb(hue, 1.f, 0.2f), mathf::lerp(0.4f, 1.f, darkness))};
            }
            if (red) {
                // the red centipede's armour: plates on the sides over the
                // shells, spikes behind the body
                vec2 const side = custom::deg_to_vec(custom::vec_to_deg(rot) + (rot.x > 0.f ? -90.f : 90.f));
                if (side.y > 0.f && centipede::has_shell(state, c, i)) {
                    float shine = mathf::inverse_lerp(-0.5f, 0.5f, rw::dot(dir, custom::deg_to_vec(30.f) * side.x));
                    shine *= mathf::max(mathf::inverse_lerp(0.3f, 0.05f, std::fabs(-0.5f - side.x)), mathf::inverse_lerp(0.3f, 0.05f, std::fabs(0.5f - side.x)));
                    shine *= mathf::pow(1.f - darkness, 2.f);
                    front_on[i] = true;
                    wings_front[i] = sprite{"CentipedeSegment", pos + perp * side.x * rad * 1.2f, deg,
                        vec2{rad * mathf::lerp(1.f, mathf::lerp(1.5f, 0.9f, std::fabs(side.x)), flat) * 0.7f * side.y * 0.071428575f, rad * 1.2f * 0.09090909f},
                        vec2{0.5f, 0.5f}, black_to(custom::hsl2rgb(hue, g.saturation, 0.5f + 0.25f * shine), 0.3f + 0.7f * darkness * (1.f - shine))};
                }
                if (centipede::has_shell(state, c, i)) {
                    float const lean = mathf::pow(std::fabs(rot.x), 0.5f) * mathf::sign(rot.x);
                    float shine = mathf::inverse_lerp(-0.5f, 0.5f, rw::dot(dir, custom::deg_to_vec(30.f) * rot.x));
                    shine *= mathf::max(mathf::inverse_lerp(0.3f, 0.05f, std::fabs(-0.5f - rot.x)), mathf::inverse_lerp(0.3f, 0.05f, std::fabs(0.5f - rot.x)));
                    shine *= mathf::pow(1.f - darkness, 2.f);
                    back_on[i] = true;
                    wings_back[i] = sprite{"Cicada8body", pos + perp * lean * rad * 1.1f,
                        custom::vec_to_deg(rw::slerp(t < 0.5f ? dir : dir * -1.f, perp * mathf::sign(lean), 0.3f + 0.7f * std::sin(t * mathf::pi))),
                        vec2{mathf::lerp(0.15f, 0.25f, std::sin(t * mathf::pi)), std::fabs(lean) * mathf::lerp(-0.25f, -0.6f, std::sin(t * mathf::pi))},
                        vec2{0.5f, 0.55f}, black_to(custom::hsl2rgb(hue, g.saturation, 0.5f + 0.25f * shine), darkness)};
                }
            }
            prev = pos;
            prev_dir = dir;
            // two piece legs from the hip to the foot (an aquapede has fins instead)
            for (int s = 0; s < 2; ++s) {
                auto const& l = g.legs[i][s];
                vec2 const hip = pos - perp * (s == 0 ? -1.f : 1.f) * rot.y * rad;
                vec2 const foot = rw::lerp(l.last_pos, l.pos, alpha);
                float flip = mathf::lerp(-1.f, 1.f, mathf::clamp(t - g.body_dir * 0.4f, 0.f, 1.f)) * mathf::lerp(s == 0 ? 1.f : -1.f, -rot.x, std::fabs(rot.x));
                flip = mathf::pow(std::fabs(flip), 0.2f) * mathf::sign(flip);
                float const len = g.leg_lengths[i];
                float const thick = red ? 1.3f : 1.f;
                vec2 const knee = custom::inverse_kinematic(hip, foot, len / 2.f, len / 2.f, flip);
                legs[i][s][0] = sprite{"CentipedeLegA", hip, custom::aim(hip, knee), vec2{-mathf::sign(flip) * thick, rw::distance(hip, knee) / 27.f}, vec2{0.5f, 0.1f}, black};
                legs[i][s][1] = sprite{"CentipedeLegB", knee, custom::aim(knee, foot), vec2{-mathf::sign(flip) * thick, rw::distance(knee, foot) / 25.f}, vec2{0.5f, 0.1f}, black};
            }
        }

        // the centiwing's and the aquapede's wings: CustomFSprites from the
        // body out to the tips, iridescent while they spread (the CicadaWing
        // shader isn't ported: the element tinted by the vertex colours)
        vec2 wing_quads[2][m][4];
        vec4 wing_colors[2][m][4];
        if (winged) {
            for (int side = 0; side < 2; ++side)
                for (uint32_t i = 0; i < n; ++i) {
                    vec2 const dr = i == 0 ? custom::dir_vec(chunk_draw_pos(state, c, 0, alpha), chunk_draw_pos(state, c, 1, alpha))
                                           : custom::dir_vec(chunk_draw_pos(state, c, i - 1, alpha), chunk_draw_pos(state, c, i, alpha));
                    vec2 const prp = custom::perpendicular_vector(dr);
                    vec2 const rot = rotat_at_chunk(g, n, i, alpha);
                    vec2 const tip = wing_pos(state, c, g, n, side, i, dr, prp, rot, alpha);
                    vec2 const root = chunk_draw_pos(state, c, i, alpha) + prp * (rad_of(i) * (side == 0 ? -1.f : 1.f) * rot.y);
                    vec2 const out_dir = custom::deg_to_vec(custom::aim(tip, root) + custom::vec_to_deg(rot));
                    float shine = mathf::inverse_lerp(0.85f, 1.f, rw::dot(out_dir, custom::deg_to_vec(45.f)))
                        * std::fabs(rw::dot(custom::deg_to_vec(45.f + custom::vec_to_deg(rot)), dr));
                    vec2 const in_dir = custom::deg_to_vec(custom::aim(root, tip) + custom::vec_to_deg(rot));
                    float const back = mathf::inverse_lerp(0.85f, 1.f, rw::dot(in_dir, custom::deg_to_vec(45.f)))
                        * std::fabs(rw::dot(custom::deg_to_vec(45.f + custom::vec_to_deg(rot)), dr * -1.f));
                    shine = mathf::pow(mathf::max(shine, back), 0.5f);
                    float const wd = aqua ? 5.f : 2.f;
                    auto& q = wing_quads[side][i];
                    q[1] = tip + dr * wd;
                    q[0] = tip - dr * wd;
                    q[2] = root + dr * wd;
                    q[3] = root - dr * wd;
                    // (the shader turns vanilla's vertex colours into a
                    // translucent film with a rainbow sheen: here a pale film
                    // of the body's hue, the sheen's colour where it catches
                    // the light)
                    vec3 const sheen = custom::hsl2rgb(0.99f - 0.4f * mathf::pow(shine, 2.f), 1.f, 0.5f + 0.5f * shine);
                    vec3 const film = custom::hsl2rgb(hue, g.saturation * 0.6f, 0.75f);
                    vec3 const tip_col = black_to(film + (sheen - film) * (0.5f * shine), 0.4f * darkness);
                    vec3 const root_col = black + (vec3{1.f, 1.f, 1.f} - black) * (0.5f * shine);
                    auto& col = wing_colors[side][i];
                    col[0] = col[1] = vec4{tip_col.x, tip_col.y, tip_col.z, 0.4f + 0.4f * shine};
                    col[2] = col[3] = vec4{root_col.x, root_col.y, root_col.z, 0.7f};
                }
        }
        static constexpr std::string_view aqua_wings[4] = {"AquapedeWing1", "AquapedeWing2", "AquapedeWing3", "AquapedeWing4"};
        auto const add_wings = [&](int side) {
            if (winged) {
                for (uint32_t i = 0; i < n; ++i)
                    sprites::add_quad(out, aqua ? aqua_wings[g.aqua_wing[side][i] & 3u] : std::string_view{"CentipedeWing"}, wing_quads[side][i], wing_colors[side][i]);
            } else if (red) {
                for (uint32_t i = 0; i < n; ++i)
                    if (side == 0 ? back_on[i] : front_on[i])
                        sprites::add(out, side == 0 ? wings_back[i] : wings_front[i]);
            }
        };

        // WingSprite(0, ..), TubeSprite, SecondarySegmentSprite, SegmentSprite,
        // WhiskerSprite, LegSprite, ShellSprite, WingSprite(1, ..)
        add_wings(0);
        sprites::long_mesh(out, tube, size_t(n) * 4, black);
        for (uint32_t i = 0; i + 1 < n; ++i)
            if (secondary_on[i])
                sprites::add(out, secondary[i]);
        for (uint32_t i = 0; i < n; ++i)
            if (segment_on[i])
                sprites::add(out, segments[i]);
        float const thin = small ? 0.5f : 1.f;
        for (int end = 0; end < 2; ++end) {
            vec2 const root = chunk_draw_pos(state, c, end == 0 ? 0 : n - 1, alpha);
            vec2 const out_dir = custom::dir_vec(chunk_draw_pos(state, c, end == 0 ? 1 : n - 2, alpha), root);
            for (int side = 0; side < 2; ++side)
                for (int part = 0; part < 2; ++part) {
                    auto const& wh = g.whiskers[end][part][side];
                    vec2 const tip = rw::lerp(wh.last_pos, wh.pos, alpha);
                    vec2 vs[15];
                    vec2 at = root;
                    for (int k = 0; k < 4; ++k) {
                        vec2 const b = custom::bezier(root, root + out_dir * rw::distance(root, tip) * 0.7f, tip, tip, float(k) / 3.f);
                        vec2 const dd = rw::normalized(b - at);
                        vec2 const a = custom::perpendicular_vector(dd) * thin;
                        float const along = rw::distance(b, at) / (k == 0 ? 1.f : 5.f);
                        vs[k * 4] = at - a + dd * along;
                        vs[k * 4 + 1] = at + a + dd * along;
                        if (k < 3) {
                            vs[k * 4 + 2] = b - a - dd * along;
                            vs[k * 4 + 3] = b + a - dd * along;
                        } else {
                            vs[k * 4 + 2] = b + dd * 2.1f;
                        }
                        at = b;
                    }
                    sprites::long_mesh(out, vs, 15, black);
                }
        }
        // CentipedeLegB is a VertexColorSprite: its top corners in the
        // secondary shell colour, the bottom ones black
        if (!aqua) {
            vec4 const top_col{secondary_shell.x, secondary_shell.y, secondary_shell.z, 1.f};
            vec4 const bottom_col{black.x, black.y, black.z, 1.f};
            vec4 const leg_b[4] = {top_col, top_col, bottom_col, bottom_col};
            for (uint32_t i = 0; i < n; ++i)
                for (int s = 0; s < 2; ++s) {
                    sprites::add(out, legs[i][s][0]);
                    sprites::add(out, legs[i][s][1], leg_b);
                }
        }
        for (uint32_t i = 0; i < n; ++i)
            if (shell_on[i])
                sprites::add(out, shells[i][0]);
        if (aqua) {
            // AquapedeBody: a translucent second shell (its alpha rolled per segment)
            for (uint32_t i = 0; i < n; ++i)
                if (shell_on[i]) {
                    vec4 const col{shells[i][1].color.x, shells[i][1].color.y, shells[i][1].color.z, 0.35f + 0.4f * g.aqua_body_alpha[i]};
                    vec4 const corners[4] = {col, col, col, col};
                    sprites::add(out, shells[i][1], corners);
                }
        }
        add_wings(1);
    }

    void build_shell(sys::particle const& p, float alpha, vec3 black, vec3 earth, float darkness, std::vector<sprites::draw_cmd>& out) {
        vec2 at = rw::lerp(p.last_pos, p.pos, alpha);
        float const fade = mathf::inverse_lerp(305.f, 380.f, float(p.counter) + alpha);
        at.y -= 20.f * mathf::pow(fade, 3.f);
        float const shrink = mathf::pow(1.f - fade, 0.25f);
        vec2 const z = custom::deg_to_vec(mathf::lerp(p.last_z_rotation, p.z_rotation, alpha));
        float const rotation = mathf::lerp(p.last_rotation, p.rotation, alpha);
        float const sx = (std::fabs(z.x) < 0.1f ? 0.1f * mathf::sign(z.x) : z.x) * shrink * p.scale.x;
        auto const black_to = [&](vec3 col, float t) { return col + (black - col) * mathf::clamp01(t); };
        vec3 c0 = black_to(custom::hsl2rgb(p.hue, p.saturation, 0.5f), 0.7f + 0.3f * darkness);
        vec3 c1;
        if (z.y > 0.f) {
            float shine = custom::lerp_map(std::fabs(rw::dot(z, custom::deg_to_vec(rotation - 45.f))), 0.5f, 1.f, 0.f, 1.f, 2.f);
            shine *= 1.f - fade;
            c1 = black_to(custom::hsl2rgb(p.hue, p.saturation, 0.5f + 0.25f * shine), darkness);
        } else {
            c1 = black_to(custom::hsl2rgb(p.hue, p.saturation * 0.8f, 0.4f), 0.4f + 0.6f * darkness);
        }
        if (fade > 0.3f) {
            float const e = mathf::pow(mathf::inverse_lerp(0.3f, 1.f, fade), 1.6f);
            c0 = c0 + (earth - c0) * e;
            c1 = c1 + (earth - c1) * e;
        }
        vec2 const up = custom::deg_to_vec(rotation) * 1.5f * shrink;
        sprites::add(out, sprite{"CentipedeBackShell", at + up, rotation, vec2{sx, shrink * p.scale.y}, vec2{0.5f, 0.5f}, c0});
        sprites::add(out, sprite{"CentipedeBackShell", at, rotation, vec2{sx, shrink * p.scale.y}, vec2{0.5f, 0.5f}, c1});
    }
}
