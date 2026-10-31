// PuffBall.cs (with the SporeCloud and SporePuffVisionObscurer it makes
// when it bursts, simplified)

#include <algorithm>
#include <cmath>

#include "puff_ball.hpp"
#include "puff_ball_types.hpp"
#include "body_parts.hpp"
#include "creature.hpp"
#include "deer.hpp"
#include "items.hpp"
#include "particles.hpp"
#include "physics.hpp"
#include "rooms.hpp"

namespace puff_ball {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        dcon::puff_ball_data_id data(sys::game_state const& state, dcon::item_id it) {
            return state.world.item_get_data_from_puff_ball_of(it);
        }

        vec3 mix(vec3 a, vec3 b, float t) { return a + (b - a) * mathf::clamp01(t); }

        // PuffBall.ApplyPalette's colour, its sporeColor, the dots' colour
        vec3 color_of(vec3 texel) { return mix(vec3{0.9f, 1.f, 0.8f}, texel, 0.5f); }
        vec3 spore_color_of(vec3 color) { return mix(color, vec3{0.02f, 0.1f, 0.08f}, 0.85f); }
        // the spores' colour where no palette is at hand (the simulation):
        // the palette's texel taken as the base colour itself
        constexpr vec3 sim_spore_color{0.152f, 0.235f, 0.188f};

        // PuffBall.ConnectSegment
        void connect_segment(state& s, vec2 pos, uint32_t i) {
            if (i == 0) {
                vec2 const a = custom::dir_vec(s.seg_pos[0], pos);
                float const num = rw::distance(s.seg_pos[0], pos);
                s.seg_pos[0] -= a * (5.f - num);
                s.seg_vel[0] -= a * (5.f - num);
                return;
            }
            vec2 const a2 = custom::dir_vec(s.seg_pos[i], s.seg_pos[i - 1]);
            float const num2 = rw::distance(s.seg_pos[i], s.seg_pos[i - 1]);
            constexpr float num3 = 0.52f;
            s.seg_pos[i] -= a2 * (5.f - num2) * num3;
            s.seg_vel[i] -= a2 * (5.f - num2) * num3;
            s.seg_pos[i - 1] += a2 * (5.f - num2) * (1.f - num3);
            s.seg_vel[i - 1] += a2 * (5.f - num2) * (1.f - num3);
        }

        // SporeCloud, as the dust puffs of particles.hpp (cosmetic, where a
        // camera looks)
        void spore_puff(sys::game_state& state, dcon::room_id room, rw::rng& r, vec2 pos, vec2 vel, float size) {
            if (!state.viewed(room))
                return;
            float const rad = mathf::lerp(0.6f, 1.5f, r.value()) * size;
            vec2 const at = pos + rw::normalized(vel) * 60.f * r.value();
            // (Futile_White is 16 pixels, the cloud 7 * rad of it, half of it here)
            particles::spawn(state, room, particles::kind::dust, at, vel * 0.1f, 28.f * rad, mathf::lerp(170.f, 400.f, r.value()) / 40.f, sim_spore_color, 0.55f);
        }
    }

    void init(sys::game_state& state, dcon::item_id it) {
        auto& w = state.world;
        auto const o = items::body(state, it);
        // the PuffBall constructor's body
        w.physical_object_set_air_friction(o, 0.98f);
        w.physical_object_set_g(o, 0.86f);
        w.physical_object_set_bounce(o, 0.2f);
        w.physical_object_set_surface_friction(o, 0.3f);
        w.physical_object_set_water_friction(o, 0.98f);
        w.physical_object_set_buoyancy(o, 1.8f);
        auto const c = items::chunk(state, it);
        w.body_chunk_set_rad(c, 7.f);
        w.body_chunk_set_mass(c, 0.11f);
        auto const d = w.create_puff_ball_data();
        w.try_create_puff_ball_of(d, it);
        // Random.InitState(ID.RandomSeed): the stalk's length, the dots
        puff_ball::state s;
        rw::unity_random r{w.item_get_seed(it)};
        s.segment_count = uint8_t(std::clamp(int32_t(mathf::lerp(3.f, 15.f, r.value())), 3, int32_t(max_segments)));
        s.dot_count = uint8_t(std::clamp(r.range(6, 11), 1, int32_t(max_dots)));
        uint32_t const n = s.dot_count;
        for (uint32_t i = 0; i < n; ++i) {
            float const v = r.value();
            s.dots[i] = custom::deg_to_vec(float(i) / float(n) * 360.f) * v + custom::rnv(r) * 0.2f;
        }
        for (int32_t j = 0; j < 3; ++j)
            for (uint32_t k = 0; k < n; ++k)
                for (uint32_t l = 0; l < n; ++l)
                    if (custom::dist_less(s.dots[k], s.dots[l], 1.4f)) {
                        vec2 const a = custom::dir_vec(s.dots[k], s.dots[l]) * (rw::distance(s.dots[k], s.dots[l]) - 1.4f);
                        float const num = (k + l) == 0 ? 0.f : float(k) / (float(k) + float(l));
                        s.dots[k] += a * num;
                        s.dots[l] -= a * (1.f - num);
                    }
        float a2 = 1.f, num2 = -1.f, a3 = 1.f, num3 = -1.f;
        for (uint32_t m = 0; m < n; ++m) {
            a2 = mathf::min(a2, s.dots[m].x);
            num2 = mathf::max(num2, s.dots[m].x);
            a3 = mathf::min(a3, s.dots[m].y);
            num3 = mathf::max(num3, s.dots[m].y);
        }
        for (uint32_t m = 0; m < n; ++m) {
            s.dots[m].x = -1.f + 2.f * mathf::inverse_lerp(a2, num2, s.dots[m].x);
            s.dots[m].y = -1.f + 2.f * mathf::inverse_lerp(a3, num3, s.dots[m].y);
        }
        float num4 = 0.f;
        for (uint32_t m = 0; m < n; ++m)
            num4 = mathf::max(num4, rw::magnitude(s.dots[m]));
        if (num4 > 0.f)
            for (uint32_t m = 0; m < n; ++m)
                s.dots[m] /= num4;
        w.puff_ball_data_set_s(d, s);
    }

    void place(sys::game_state& state, dcon::item_id it) {
        auto& w = state.world;
        auto const d = data(state, it);
        if (!d)
            return;
        auto const c = items::chunk(state, it);
        auto const room = w.physical_object_get_room_from_object_location(items::body(state, it));
        auto const view = world::get_room_view(state, room);
        // PuffBall.PlaceInRoom: down to the floor, then Weapon.PlaceInRoom
        ivec2 t = room_view::get_tile_position(w.body_chunk_get_pos(c));
        while (t.y >= 0 && !view.get_tile(t + ivec2{0, -1}).solid())
            --t.y;
        t.y = std::max(t.y, 0);
        physics::hard_set_position(state, c, room_view::middle_of_tile(t));
        // game.SeededRandom(ID.RandomSeed)
        rw::unity_random r{w.item_get_seed(it)};
        w.item_set_rotation(it, custom::deg_to_vec(mathf::lerp(-45.f, 45.f, r.value())));
        w.item_set_last_rotation(it, w.item_get_rotation(it));
        // NewRoom: the stalk straight up
        auto& s = w.puff_ball_data_get_s(d);
        vec2 const pos = w.body_chunk_get_pos(c);
        for (uint32_t i = 0; i < s.segment_count; ++i) {
            s.seg_pos[i] = pos + vec2{0.f, 5.f * float(i)};
            s.seg_last[i] = s.seg_pos[i];
            s.seg_vel[i] = vec2{0.f, 0.f};
        }
    }

    void destroyed(sys::game_state& state, dcon::item_id it) {
        if (auto const d = data(state, it))
            state.world.delete_puff_ball_data(d);
    }

    bool update(sys::game_state& state, dcon::item_id it, room_view const& room) {
        auto& w = state.world;
        auto const d = data(state, it);
        if (!d)
            return false;
        auto& s = w.puff_ball_data_get_s(d);
        auto const c = items::chunk(state, it);
        vec2 const pos = w.body_chunk_get_pos(c);
        if (s.being_eaten > 0.f) {
            s.being_eaten += 0.1f;
            for (uint32_t i = 0; i < s.segment_count; ++i)
                s.seg_pos[i] = rw::lerp(s.seg_pos[i], pos, s.being_eaten);
            if (s.being_eaten > 1.f && s.being_eaten < 1.1f)
                state.defer(room.id, [it](sys::game_state& st) {
                    if (st.world.item_is_valid(it))
                        items::destroy(st, it);
                });
        }
        ivec2 const contact = w.body_chunk_get_contact_point(c);
        bool const burst = s.last_mode_thrown && (contact.x != 0 || contact.y != 0);
        s.last_mode_thrown = w.item_get_mode(it) == world::weapon_mode::thrown ? 1 : 0;
        if (contact.y != 0)
            w.item_set_rotation_speed(it, (w.item_get_rotation_speed(it) * 2.f + w.body_chunk_get_vel(c).x * 5.f) / 3.f);
        vec2 const rot = w.item_get_rotation(it);
        uint32_t const n = s.segment_count;
        for (uint32_t j = 0; j < n; ++j) {
            float const num = n > 1 ? float(j) / float(n - 1) : 0.f;
            s.seg_last[j] = s.seg_pos[j];
            s.seg_pos[j] += s.seg_vel[j];
            s.seg_vel[j] *= mathf::lerp(1.f, 0.85f, num);
            s.seg_vel[j] += rw::lerp(rot * 5.f, vec2{mathf::clamp(pos.x - s.seg_pos[j].x, -2.f, 2.f) * 0.0025f, 0.25f} * (1.f - num), mathf::pow(num, 0.01f));
            s.seg_vel[j].y += 0.01f;
            connect_segment(s, pos, j);
        }
        for (int32_t k = int32_t(n) - 1; k >= 0; --k)
            connect_segment(s, pos, uint32_t(k));
        // (SporesSmoke from the stalk's tip isn't ported; the slugcat's
        // swallowing it isn't either: swallowed stays 0)
        return burst;
    }

    void thrown(sys::game_state& state, dcon::item_id it, ivec2 throw_dir) {
        auto& w = state.world;
        auto const room = w.physical_object_get_room_from_object_location(items::body(state, it));
        auto& r = w.item_get_random(it);
        // a SporeCloud of size 0.5 (Slugcat_Throw_Puffball has no sample here)
        spore_puff(state, room, r, w.body_chunk_get_pos(items::chunk(state, it)), custom::rnv(r) * r.value() + rw::to_vec2(throw_dir) * 10.f, 0.5f);
    }

    void explode(sys::game_state& state, room_view const& room, dcon::item_id it, vec2 at) {
        auto& w = state.world;
        auto& r = w.item_get_random(it);
        // 70 SporeClouds
        for (int32_t j = 0; j < 70; ++j)
            spore_puff(state, room.id, r, at, custom::rnv(r) * r.value() * 10.f, 1.f);
        // the clouds' toxicity, at once: the insects in them stunned
        // (InsectoidCreature.poison simplified), Stun(20 * value * life / mass)
        rooms::for_each_creature(state, room.id, [&](dcon::creature_id c) {
            auto const t = w.creature_get_type(c);
            if (t != creature::type::centipede && t != creature::type::dropbug && t != creature::type::cicada && t != creature::type::needle_worm)
                return;
            if (!creature::here(state, c, room.id) || w.creature_get_dead(c))
                return;
            uint32_t const n = creature::chunk_count(state, c);
            bool hit = false;
            for (uint32_t k = 0; k < n && !hit; ++k) {
                auto const ch = creature::chunk(state, c, k);
                hit = custom::dist_less(at, w.body_chunk_get_pos(ch), 90.f + w.body_chunk_get_rad(ch));
            }
            if (!hit)
                return;
            float const m = mathf::lerp(creature::total_mass(state, c), 1.f, 0.15f);
            // a few of the clouds' checks' worth
            creature::stun(state, c, int32_t(std::nearbyint(60.f * (0.5f + 0.5f * r.value()) / mathf::max(m, 0.05f))));
        });
        // the SporePuffVisionObscurer calls a deer (PuffBallSkin's shreds
        // and Puffball_Eplode aren't ported)
        deer::attract_to_spores(state, room.id, at);
    }

    void start_being_eaten(sys::game_state& state, dcon::item_id it) {
        if (auto const d = data(state, it)) {
            auto& s = state.world.puff_ball_data_get_s(d);
            s.being_eaten = mathf::max(s.being_eaten, 0.01f);
        }
    }

    void build(sys::game_state const& state, dcon::item_id it, float alpha, vec3 black, vec3 texel_11_4, std::vector<sprites::draw_cmd>& out) {
        auto const& w = state.world;
        auto const d = data(state, it);
        if (!d)
            return;
        auto const& s = w.puff_ball_data_get_s(d);
        auto const c = items::chunk(state, it);
        vec2 const pos = rw::lerp(w.body_chunk_get_last_pos(c), w.body_chunk_get_pos(c), alpha);
        float const num = custom::vec_to_deg(rw::slerp(w.item_get_last_rotation(it), w.item_get_rotation(it), alpha));
        // ApplyPalette
        vec3 const color = color_of(texel_11_4);
        vec3 const spore = spore_color_of(color);
        vec3 const dot_color = mix(mix(vec3{0.8f, 1.f, 0.5f}, texel_11_4, 0.2f), black, 0.5f);
        float num2 = 1.f;
        if (s.being_eaten > 0.f || s.swallowed > 0.f)
            num2 = 1.f - mathf::max(s.being_eaten, s.swallowed * 0.5f);
        // the Items container's order: the ball, the stalk, the dots, the
        // highlight, the spore pixels (but the first: AddToContainer skips it)
        sprites::add(out, sprites::sprite{"BodyA", pos, num, vec2{num2, 0.9f * num2}, vec2{0.5f, 0.5f}, color});
        {
            std::vector<vec2> v;
            v.reserve(size_t(s.segment_count) * 4);
            vec2 v3 = pos;
            for (uint32_t j = 0; j < s.segment_count; ++j) {
                vec2 const v4 = rw::lerp(s.seg_last[j], s.seg_pos[j], alpha);
                vec2 const n = rw::normalized(v4 - v3);
                vec2 const a = custom::perpendicular_vector(n);
                float const dd = rw::distance(v4, v3) / 5.f;
                if (j == 0) {
                    v.push_back(v3 - a * 0.5f);
                    v.push_back(v3 + a * 0.5f);
                } else {
                    v.push_back(v3 - a * 0.5f + n * dd);
                    v.push_back(v3 + a * 0.5f + n * dd);
                }
                v.push_back(v4 - a * 0.5f - n * dd);
                v.push_back(v4 + a * 0.5f - n * dd);
                v3 = v4;
            }
            sprites::long_mesh(out, v.data(), v.size(), color);
        }
        vec2 dot_pos[max_dots];
        for (uint32_t i = 0; i < s.dot_count; ++i) {
            vec2 const dt = s.dots[i];
            dot_pos[i] = pos + body_parts::rotate_around_origo(vec2{dt.x * 7.f, dt.y * 8.5f} * num2, num);
            sprites::add(out, sprites::sprite{"JetFishEyeB", dot_pos[i], custom::vec_to_deg(rw::normalized(body_parts::rotate_around_origo(dt, num))),
                vec2{num2, custom::lerp_map(rw::magnitude(dt), 0.f, 1.f, 1.f, 0.25f, 4.f)}, vec2{0.5f, 0.5f}, dot_color});
        }
        {
            // sprite 2: BodyA at half alpha (its colour is never set: white)
            vec4 const col{1.f, 1.f, 1.f, 0.5f};
            vec4 const corners[4] = {col, col, col, col};
            sprites::add(out, sprites::sprite{"BodyA", pos + vec2{-2.5f, 2.5f}, num, vec2{0.5f * num2, 0.45f * num2}, vec2{0.5f, 0.5f}, vec3{1.f, 1.f, 1.f}}, corners);
        }
        for (uint32_t i = 1; i < s.dot_count; ++i)
            sprites::add(out, sprites::sprite{"pixel", dot_pos[i], 0.f, vec2{1.f, 1.f}, vec2{0.5f, 0.5f}, spore});
    }
}
