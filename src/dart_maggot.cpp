// DartMaggot.cs and its Umbilical (dart_maggot.hpp)

#include <algorithm>
#include <cmath>

#include "dart_maggot.hpp"
#include "big_spider.hpp"
#include "big_spider_types.hpp"
#include "body_parts.hpp"
#include "creature.hpp"
#include "items.hpp"
#include "particles.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "rooms.hpp"
#include "vulture_internal.hpp"

namespace dart_maggot {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using world::room_view;
    using big_spider::maggot_mode;
    using big_spider::maggot_state;
    using big_spider::maggot_mesh_segs;
    using big_spider::until_sleep_delay;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;
    namespace sp = vulture::detail;

    namespace {
        dcon::dart_maggot_data_id data(sys::game_state const& state, dcon::item_id it) {
            return state.world.item_get_data_from_dart_maggot_of(it);
        }

        float life(maggot_state const& s) { return mathf::inverse_lerp(0.65f, 0.55f, s.age); }
        float rot(maggot_state const& s) { return mathf::max(mathf::inverse_lerp(0.65f, 0.75f, s.age), s.squeeze); }
        float dissapear(maggot_state const& s) { return mathf::inverse_lerp(0.75f, 1.f, s.age); }
        bool stuck(maggot_state const& s) { return s.mode == maggot_mode::stuck_in_terrain || s.mode == maggot_mode::stuck_in_chunk; }

        vec3 mix(vec3 a, vec3 b, float t) { return a + (b - a) * mathf::clamp01(t); }

        // ResetBody
        void reset_body(maggot_state& s, vec2 at, vec2 dr) {
            for (uint32_t i = 0; i < 2; ++i) {
                s.body_pos[i] = at - dr * float(i);
                s.body_last[i] = s.body_pos[i];
                s.body_vel[i] = vec2{0.f, 0.f};
            }
        }

        // BodyChunk.Rotation of a creature's chunk: away from the chunk
        // before it (the next one for the first), up for a single chunk
        vec2 chunk_rotation(sys::game_state const& state, dcon::creature_id t, uint32_t k) {
            uint32_t const n = creature::chunk_count_of(state, t);
            if (n < 2 || k >= n)
                return vec2{0.f, 1.f};
            uint32_t const o = k == 0 ? 1 : k - 1;
            return custom::dir_vec(state.world.body_chunk_get_pos(creature::chunk_of(state, t, o)), state.world.body_chunk_get_pos(creature::chunk_of(state, t, k)));
        }

        // ChangeMode (StuckInChunk: the spitter that shot it learns of it,
        // when it is in the room)
        void change_mode(sys::game_state& state, dcon::item_id it, maggot_state& s, maggot_mode m, dcon::room_id room) {
            state.world.body_chunk_set_collide_with_terrain(items::chunk(state, it), m == maggot_mode::free);
            if (m == maggot_mode::stuck_in_chunk && s.stuck_in && s.shot_by && creature::here(state, s.shot_by, room)
            && state.world.creature_get_type(s.shot_by) == creature::type::big_spider && big_spider::spitter(state, s.shot_by))
                big_spider::creature_hit_by_dart(state, s.shot_by, s.stuck_in);
            s.mode = m;
        }

        // Unstuck
        void unstick(sys::game_state& state, dcon::item_id it, maggot_state& s, rw::rng& rng, dcon::room_id room) {
            vec2& vel = state.world.body_chunk_get_vel(items::chunk(state, it));
            if (s.stuck_in) {
                vec2 v{0.f, 0.f};
                if (creature::live(state, s.stuck_in) && s.stuck_chunk < creature::chunk_count_of(state, s.stuck_in))
                    v = state.world.body_chunk_get_vel(creature::chunk_of(state, s.stuck_in, s.stuck_chunk));
                vel = v + custom::rnv(rng) * rng.value() * 2.f;
                s.stuck_in = dcon::creature_id{};
            } else {
                vel = custom::rnv(rng) * rng.value() * 2.f;
            }
            change_mode(state, it, s, maggot_mode::free, room);
        }

        // ShotUpdate
        void shot_update(sys::game_state& state, dcon::item_id it, maggot_state& s, room_view const& room) {
            auto& w = state.world;
            auto const c = items::chunk(state, it);
            vec2& pos = w.body_chunk_get_pos(c);
            vec2& vel = w.body_chunk_get_vel(c);
            vec2 const last_last = w.body_chunk_get_last_pos(c);
            w.body_chunk_set_last_pos(c, pos);
            vec2 const last = pos;
            for (uint32_t i = 0; i < 2; ++i) {
                s.body_last[i] = s.body_pos[i];
                s.body_vel[i] = vel * 0.5f;
            }
            vec2 const m0 = (last + pos) / 2.f, m1 = (last_last + last) / 2.f;
            s.body_pos[0] = rw::lerp(s.body_pos[0], m0, mathf::inverse_lerp(0.f, 50.f, rw::distance(m0, s.body_pos[0])));
            s.body_pos[1] = rw::lerp(s.body_pos[1], m1, mathf::inverse_lerp(0.f, 50.f, rw::distance(m1, s.body_pos[1])));
            vec2 const p = pos;
            vec2 const v = pos + vel;
            s.needle_dir = custom::dir_vec(p, v);
            vec2 wall_at{0.f, 0.f}, into{0.f, 0.f};
            bool wall = sp::exact_terrain_ray_trace(room, p, v, wall_at, into);
            dcon::creature_id hit;
            creature::hit_part part;
            bool crit = creature::trace(state, room.id, p, v, 1.f, s.shot_by, hit, part) && part.chunk >= 0 && hit != s.shot_by
                && uint32_t(part.chunk) < creature::chunk_count_of(state, hit);
            // where the segment meets the chunk
            vec2 cp{0.f, 0.f};
            if (crit) {
                auto const k = creature::chunk_of(state, hit, uint32_t(part.chunk));
                vec2 const kp = w.body_chunk_get_pos(k);
                float const rad = w.body_chunk_get_rad(k) + 1.f;
                vec2 const near = custom::closest_point_on_line(p, v, kp);
                float const d = rw::distance(near, kp);
                cp = near - s.needle_dir * std::sqrt(mathf::max(0.f, rad * rad - d * d));
            }
            if (wall && crit) {
                if (rw::distance(p, wall_at) < rw::distance(p, cp))
                    crit = false;
                else
                    wall = false;
            }
            auto rng = rw::random(state.seed, state.tick, uint32_t(it.value));
            if (wall) {
                if (rw::dot(custom::dir_vec(p, v), into) > 0.5f) {
                    s.stuck_pos = wall_at + custom::dir_vec(v, p) * 15.f;
                    s.stuck_dir = custom::dir_vec(p, v);
                    pos = s.stuck_pos;
                    // (no Dart_Maggot_Stick_In_Wall sound)
                    change_mode(state, it, s, maggot_mode::stuck_in_terrain, room.id);
                } else {
                    if (into.x != 0.f)
                        vel.x = std::fabs(vel.x) * mathf::sign(into.x) * -1.f;
                    if (into.y != 0.f)
                        vel.y = std::fabs(vel.y) * mathf::sign(into.y) * -1.f;
                    pos = wall_at + custom::dir_vec(v, p) * 15.f;
                    // (no Dart_Maggot_Bounce_Off_Wall sound)
                    change_mode(state, it, s, maggot_mode::free, room.id);
                }
            } else if (crit) {
                uint32_t const k = uint32_t(part.chunk);
                vec2 const kp = w.body_chunk_get_pos(creature::chunk_of(state, hit, k));
                float const deg = custom::vec_to_deg(chunk_rotation(state, hit, k));
                pos = cp + custom::dir_vec(v, p) * 11.f;
                s.stuck_pos = body_parts::rotate_around_origo(pos - kp, -deg);
                s.stuck_dir = body_parts::rotate_around_origo(custom::dir_vec(p, v), -deg);
                s.stuck_in = hit;
                s.stuck_chunk = uint8_t(k);
                creature::violence(state, hit, part, custom::dir_vec(p, v) * 3.f, 0.07f, 3.f, creature::damage_type::stab);
                // (no Dart_Maggot_Stick_In_Creature sound)
                change_mode(state, it, s, maggot_mode::stuck_in_chunk, room.id);
            } else {
                pos += vel;
            }
            (void)rng;
            vel.y -= 0.9f;
            if (s.mode == maggot_mode::shot && rw::magnitude(vel) < 30.f)
                change_mode(state, it, s, maggot_mode::free, room.id);
        }

        // NormalUpdate: the body, the wiggling, ageing. True when it is old
        // enough to go
        bool normal_update(sys::game_state& state, dcon::item_id it, maggot_state& s, room_view const& room, rw::rng& rng) {
            auto& w = state.world;
            auto const c = items::chunk(state, it);
            vec2& pos = w.body_chunk_get_pos(c);
            vec2& vel = w.body_chunk_get_vel(c);
            for (uint32_t i = 0; i < 2; ++i) {
                s.body_last[i] = s.body_pos[i];
                s.body_pos[i] += s.body_vel[i];
                s.body_vel[i] *= 0.999f;
                s.body_vel[i].y -= 0.9f;
            }
            for (uint32_t j = 0; j < 2; ++j) {
                sp::terrain_collision cd{s.body_pos[j], s.body_last[j], s.body_vel[j], (2.5f - float(j) * 0.5f) * s.size_fac};
                sp::vertical_collision(room, cd);
                sp::horizontal_collision(room, cd);
                sp::slopes_vertically(room, cd);
                s.body_pos[j] = cd.pos;
                s.body_vel[j] = cd.vel;
                if (cd.contact.y < 0)
                    s.body_vel[j].x *= 0.4f;
                if (j == 0) {
                    vec2 const a = custom::dir_vec(s.body_pos[0], pos) * (rw::distance(s.body_pos[0], pos) - 5.f * s.size_fac);
                    s.body_pos[0] += a * 0.5f;
                    s.body_vel[0] += a * 0.5f;
                    pos -= a * 0.5f;
                    vel -= a * 0.5f;
                } else {
                    vec2 const a = custom::dir_vec(s.body_pos[1], s.body_pos[0]) * (rw::distance(s.body_pos[1], s.body_pos[0]) - 5.f * s.size_fac);
                    s.body_pos[1] += a * 0.5f;
                    s.body_vel[1] += a * 0.5f;
                    s.body_pos[0] -= a * 0.5f;
                    s.body_vel[0] -= a * 0.5f;
                }
            }
            float const d = mathf::pow(mathf::inverse_lerp(0.25f, -0.75f, rw::dot(rw::normalized(pos - s.body_pos[0]), rw::normalized(s.body_pos[0] - s.body_pos[1]))), 2.f);
            vec2 const st = custom::dir_vec(s.body_pos[1], pos) * d * 1.5f * s.size_fac;
            vel += st;
            pos += st;
            s.body_vel[1] -= st;
            s.body_pos[1] -= st;
            float const l = life(s);
            if (rng.value() < l / 20.f)
                s.wiggle_get_to_dir = custom::rnv(rng) * rng.value();
            s.wiggle_get_to_dir += custom::rnv(rng) * rng.value() * 0.05f;
            s.wiggle_dir = custom::move_towards(s.wiggle_dir, s.wiggle_get_to_dir, 0.02f);
            if (stuck(s)) {
                s.body_vel[0] -= (s.wiggle_dir * 2.3f * l + s.needle_dir * 1.2f) * s.size_fac;
                s.body_vel[1] += s.wiggle_dir * 4.6f * l * s.size_fac;
            } else {
                vel += s.wiggle_dir * 2.3f * l * s.size_fac;
                s.body_vel[0] -= s.wiggle_dir * 4.6f * l * s.size_fac;
                s.body_vel[1] += s.wiggle_dir * 2.3f * l * s.size_fac;
                s.needle_dir = rw::normalized(s.needle_dir + custom::dir_vec(s.body_pos[0], pos) * 0.2f);
            }
            if (l > 0.f) {
                s.squirm_offset += s.squirm_add * 0.2f * l;
                if (rng.value() < l / 50.f)
                    s.squirm_add_get_to = mathf::lerp(-1.f, 1.f, rng.value());
                s.squirm_add = custom::lerp_and_tick(s.squirm_add, s.squirm_add_get_to, 0.03f, 0.0125f);
                if (rng.value() < l / 50.f)
                    s.squirm_width_get_to = rng.value();
                s.squirm_width = custom::lerp_and_tick(s.squirm_width, s.squirm_width_get_to, 0.03f, 0.0125f);
                if (rng.value() < l / 50.f)
                    s.squirm_amp_get_to = rng.value();
                s.squirm_amp = custom::lerp_and_tick(s.squirm_amp, s.squirm_amp_get_to, 0.03f, 0.0125f);
            }
            for (uint32_t k = 0; k < maggot_mesh_segs; ++k) {
                s.squirm[k][1] = s.squirm[k][0];
                s.squirm[k][0] = std::sin(s.squirm_offset + float(k) * mathf::lerp(0.5f, 2.f, s.squirm_width)) * s.squirm_amp * l * (1.f - s.bloat);
            }
            if (s.age < 0.65f)
                s.age += 1.f / s.life_time;
            else
                s.age += 0.0014285714f;
            if (stuck(s) && !s.stuck_in && rng.value() < 1.f / mathf::lerp(240.f, 80.f, 0.5f * rot(s) + 0.5f * dissapear(s)) && rng.value() < rot(s))
                unstick(state, it, s, rng, room.id);
            return s.age > 1.f;
        }

        // Umbilical.LifeOfSegment
        float life_of_segment(maggot_state const& s, uint32_t i) {
            if (i > 0)
                return mathf::min(s.u_life[i].x, s.u_life[i - 1].x);
            return s.u_life[i].x;
        }

        // Umbilical.Update (cosmetic, but kept every tick: it is short lived)
        void umbilical_update(sys::game_state& state, maggot_state& s, room_view const& room, bool gone) {
            uint32_t const n = s.umbilical_count;
            if (n == 0)
                return;
            bool all_dead = true;
            for (uint32_t i = 0; i < n; ++i) {
                s.u_last[i] = s.u_pos[i];
                s.u_pos[i] += s.u_vel[i];
                s.u_vel[i] *= custom::lerp_map(rw::magnitude(s.u_vel[i]), 1.f, 30.f, 0.99f, 0.8f);
                s.u_vel[i].y -= mathf::lerp(0.1f, 0.6f, life_of_segment(s, i));
                if (life_of_segment(s, i) > 0.f) {
                    if (room.get_tile(s.u_pos[i]).solid()) {
                        sp::terrain_collision cd{s.u_pos[i], s.u_last[i], s.u_vel[i], 1.f};
                        sp::horizontal_collision(room, cd);
                        sp::vertical_collision(room, cd);
                        s.u_pos[i] = cd.pos;
                        s.u_vel[i] = cd.vel;
                    }
                    if (i > 0) {
                        if (!custom::dist_less(s.u_pos[i], s.u_pos[i - 1], 6.f)) {
                            vec2 const a = custom::dir_vec(s.u_pos[i], s.u_pos[i - 1]) * (rw::distance(s.u_pos[i], s.u_pos[i - 1]) - 6.f);
                            s.u_pos[i] += a * 0.15f;
                            s.u_vel[i] += a * 0.25f;
                            s.u_pos[i - 1] -= a * 0.15f;
                            s.u_vel[i - 1] -= a * 0.25f;
                        }
                        if (!room.visual_contact(s.u_pos[i], s.u_pos[i - 1]))
                            s.u_life[i].x -= 0.2f;
                    }
                    if (i > 1 && life_of_segment(s, i - 1) > 0.f) {
                        vec2 const d = custom::dir_vec(s.u_pos[i - 2], s.u_pos[i]) * 0.6f;
                        s.u_vel[i] += d;
                        s.u_vel[i - 2] -= d;
                    }
                    s.u_life[i].x -= s.u_life[i].y > 0.f ? 1.f / s.u_life[i].y : 1.f;
                    if (s.u_life[i].x > 0.f)
                        all_dead = false;
                }
            }
            // the spitter's end
            if (life_of_segment(s, 0) > 0.f) {
                auto const sp_c = s.shot_by;
                bool const there = sp_c && creature::here(state, sp_c, room.id) && state.world.creature_get_entering_shortcut(sp_c).x < 0
                    && creature::chunk_count_of(state, sp_c) >= 2;
                if (there) {
                    vec2 const c0 = state.world.body_chunk_get_pos(creature::chunk_of(state, sp_c, 0));
                    vec2 const c1 = state.world.body_chunk_get_pos(creature::chunk_of(state, sp_c, 1));
                    s.u_pos[0] = c0;
                    s.u_vel[0] = vec2{0.f, 0.f};
                    if (n > 1)
                        s.u_vel[1] += custom::dir_vec(c1, c0) * 3.f;
                    if (n > 3)
                        s.u_vel[3] += custom::dir_vec(c1, c0) * 1.5f;
                }
                if (!there || life_of_segment(s, 1) <= 0.f)
                    s.u_life[0].x -= 1.f;
            }
            // the maggot's end
            if (life_of_segment(s, n - 1) > 0.f) {
                s.u_pos[n - 1] = s.body_pos[1];
                s.u_vel[n - 1] = vec2{0.f, 0.f};
                vec2 const d = custom::dir_vec(s.body_pos[0], s.body_pos[1]);
                if (n > 1)
                    s.u_vel[n - 2] += d * 3.f;
                if (n > 2)
                    s.u_vel[n - 3] += d * 1.5f;
                if (gone)
                    s.u_life[n - 1].x -= 1.f;
            }
            if (all_dead)
                s.umbilical_count = 0;
        }
    }

    bool is_maggot(world::item_type t) noexcept {
        return t == world::item_type::dart_maggot;
    }

    void init(sys::game_state& state, dcon::item_id it) {
        auto& w = state.world;
        auto const o = items::body(state, it);
        auto const c = items::chunk(state, it);
        maggot_state s;
        {
            rw::unity_random r{w.item_get_seed(it)};
            s.size_fac = custom::clamped_random_variation(0.8f, 0.2f, 0.5f, r);
        }
        w.physical_object_set_air_friction(o, 0.999f);
        w.physical_object_set_g(o, 0.9f);
        w.physical_object_set_bounce(o, 0.1f);
        w.physical_object_set_surface_friction(o, 0.4f);
        w.physical_object_set_water_friction(o, 0.96f);
        w.physical_object_set_buoyancy(o, 0.95f);
        w.body_chunk_set_rad(c, 2.6f * s.size_fac);
        w.body_chunk_set_mass(c, 0.1f);
        w.item_set_rotation_speed(it, 0.f);
        auto rng = rw::random(state.seed, state.tick, uint32_t(it.value));
        reset_body(s, w.body_chunk_get_pos(c), custom::rnv(rng));
        s.wiggle_get_to_dir = custom::rnv(rng) * rng.value();
        s.wiggle_dir = s.wiggle_get_to_dir;
        s.mode = maggot_mode::free;
        s.needle_dir = custom::rnv(rng);
        s.last_needle_dir = s.needle_dir;
        s.life_time = mathf::lerp(1100.f, 2100.f, rng.value());
        auto const d = w.create_dart_maggot_data();
        w.try_create_dart_maggot_of(d, it);
        w.dart_maggot_data_set_s(d, s);
    }

    void place(sys::game_state& state, dcon::item_id it) {
        auto& w = state.world;
        auto const d = data(state, it);
        if (!d)
            return;
        auto rng = rw::random(state.seed, state.tick, uint32_t(it.value));
        reset_body(w.dart_maggot_data_get_s(d), w.body_chunk_get_pos(items::chunk(state, it)), custom::rnv(rng));
    }

    void destroyed(sys::game_state& state, dcon::item_id it) {
        if (auto const d = data(state, it))
            state.world.delete_dart_maggot_data(d);
    }

    void shoot(sys::game_state& state, dcon::item_id it, vec2 at, vec2 dir, dcon::creature_id by) {
        auto& w = state.world;
        auto const d = data(state, it);
        if (!d)
            return;
        auto& s = w.dart_maggot_data_get_s(d);
        auto const c = items::chunk(state, it);
        auto rng = rw::random(state.seed, state.tick, uint32_t(it.value));
        physics::hard_set_position(state, c, at);
        w.body_chunk_set_vel(c, dir * 70.f);
        reset_body(s, at, dir);
        auto const room = items::body(state, it) ? w.physical_object_get_room_from_object_location(items::body(state, it)) : dcon::room_id{};
        s.mode = maggot_mode::shot;
        w.body_chunk_set_collide_with_terrain(c, false);
        s.shot_by = by;
        s.needle_dir = dir;
        s.last_needle_dir = dir;
        s.sleep_counter = until_sleep_delay;
        s.new_and_pink = 1.f;
        // (no Dart_Maggot_Whizz_By sound)
        // the Umbilical back to the spitter
        s.umbilical_count = 0;
        if (by && w.creature_is_valid(by) && w.creature_get_type(by) == creature::type::big_spider && w.creature_get_away(by) == 0) {
            vec2 const sp_pos = w.body_chunk_get_pos(creature::chunk(state, by, 0));
            uint32_t const n = uint32_t(rng.range(10, 20));
            s.umbilical_count = uint8_t(n);
            for (uint32_t i = 0; i < n; ++i) {
                s.u_pos[i] = sp_pos + custom::rnv(rng);
                s.u_last[i] = sp_pos;
                s.u_vel[i] = dir * 70.f * 0.3f * rng.value() + custom::rnv(rng) * rng.value() * 1.5f;
                s.u_life[i] = vec2{1.f, rng.value() < 0.016666668f ? 0.f : mathf::lerp(50.f, 200.f, mathf::pow(rng.value(), 0.3f))};
            }
            if (rng.value() < 0.5f)
                s.u_life[uint32_t(rng.range(0, int32_t(n)))].x = 0.f;
        }
        // WaterDrips (cosmetic)
        if (room && state.viewed(room))
            for (int32_t i = rng.range(0, 5); i >= 0; --i)
                particles::spawn(state, room, particles::kind::drop, at, dir * rng.value() * 15.f + custom::rnv(rng) * rng.value() * 5.f, 1.5f, 0.75f,
                    vec3{0.95f, 0.8f, 0.55f});
    }

    bool update(sys::game_state& state, dcon::item_id it, room_view const& room) {
        auto& w = state.world;
        auto const d = data(state, it);
        if (!d)
            return true;
        auto& s = w.dart_maggot_data_get_s(d);
        auto rng = rw::random(state.seed, state.tick, uint32_t(it.value));
        auto const o = items::body(state, it);
        auto const c = items::chunk(state, it);
        bool old = false;
        s.last_needle_dir = s.needle_dir;
        if (s.mode != maggot_mode::shot) {
            physics::update_object(state, room, o, nullptr, nullptr);
            old = normal_update(state, it, s, room, rng);
        }
        vec2& pos = w.body_chunk_get_pos(c);
        vec2& vel = w.body_chunk_get_vel(c);
        if (s.mode == maggot_mode::shot) {
            shot_update(state, it, s, room);
        } else if (s.mode == maggot_mode::stuck_in_chunk) {
            auto const t = s.stuck_in;
            if (!t || !creature::here(state, t, room.id) || s.stuck_chunk >= creature::chunk_count_of(state, t)) {
                // its creature left: it drops out and rots
                s.stuck_in = dcon::creature_id{};
                unstick(state, it, s, rng, room.id);
                s.age = mathf::max(s.age, 0.75f);
            } else {
                auto const k = creature::chunk_of(state, t, s.stuck_chunk);
                float const deg = custom::vec_to_deg(chunk_rotation(state, t, s.stuck_chunk));
                s.needle_dir = body_parts::rotate_around_origo(s.stuck_dir, deg);
                // (StuckInChunkPos: the chunk, not the slugcat's drawn position)
                pos = w.body_chunk_get_pos(k) + body_parts::rotate_around_origo(s.stuck_pos, deg);
                vel = vec2{0.f, 0.f};
                if (s.sleep_counter > 0) {
                    if (s.sleep_counter < 260) {
                        float const tt = mathf::inverse_lerp(260.f, 30.f, float(s.sleep_counter));
                        s.bloat = mathf::min(1.f, s.bloat + 0.016666668f);
                        s.squeeze = mathf::min(1.f, s.squeeze + 0.0038461538f);
                        if (rng.value() < 1.f / mathf::lerp(40.f, 20.f, tt)) {
                            int32_t& st = w.creature_get_stun(t);
                            st = std::max(st, int32_t(rng.value() * mathf::lerp(8.f, 22.f, tt)));
                        }
                        if (creature::is_slugcat(w, t) && rng.value() < 1.f / mathf::lerp(60.f, 10.f, tt)) {
                            if (auto const p = creature::player_of(state, t)) {
                                int32_t& sm = w.player_get_slow_movement_stun(p);
                                sm = std::max(sm, int32_t(rng.value() * 20.f));
                            }
                        }
                    }
                    --s.sleep_counter;
                    if (s.sleep_counter == 0) {
                        // the darts in it (DartMaggotSticks)
                        int32_t num = 0;
                        rooms::for_each_item(state, room.id, [&](dcon::item_id other) {
                            if (w.item_get_type(other) != world::item_type::dart_maggot)
                                return;
                            if (auto const od = data(state, other)) {
                                auto const& os = w.dart_maggot_data_get_s(od);
                                if (os.mode == maggot_mode::stuck_in_chunk && os.stuck_in == t)
                                    ++num;
                            }
                        });
                        int32_t& st = w.creature_get_stun(t);
                        st = std::max(st, 40 * (2 + 3 * custom::int_clamp(num, 1, 4)));
                    }
                } else if (rng.value() < 0.033333335f) {
                    unstick(state, it, s, rng, room.id);
                    s.age = mathf::max(s.age, 0.75f);
                }
            }
        } else if (s.mode == maggot_mode::stuck_in_terrain) {
            s.needle_dir = s.stuck_dir;
            pos = s.stuck_pos;
            vel = vec2{0.f, 0.f};
        }
        bool out = !stuck(s) && (pos.x < -100.f || pos.x > room.pixel_width() + 100.f || pos.y < -100.f);
        s.new_and_pink = mathf::max(0.f, s.new_and_pink - 0.025f);
        umbilical_update(state, s, room, old || out);
        return old || out;
    }

    void build(sys::game_state const& state, dcon::item_id it, float alpha, vec3 black, vec3 fog, float darkness, std::vector<sprites::draw_cmd>& out) {
        auto const& w = state.world;
        auto const d = data(state, it);
        if (!d)
            return;
        auto const& s = w.dart_maggot_data_get_s(d);
        auto const c = items::chunk(state, it);
        float const t = alpha;
        float const rt = rot(s), dis = dissapear(s);

        // the Umbilical first (it is added to the room when shot, before
        // the maggot draws: behind it)
        if (s.umbilical_count > 1) {
            uint32_t const n = s.umbilical_count;
            vec2 v[big_spider::max_umbilical * 4];
            vec4 cols[big_spider::max_umbilical * 4];
            vec3 const thread = mix(vec3{0.95f, 0.8f, 0.55f}, fog, 0.2f);
            vec2 vector = rw::lerp(s.u_last[0], s.u_pos[0], t);
            float num = 0.f, b = 1.f;
            bool const dark = darkness > 0.2f;
            for (uint32_t i = 0; i < n; ++i) {
                float const f = float(i) / float(n - 1);
                float num2 = life_of_segment(s, i);
                if (i < n - 1)
                    num2 = mathf::min(num2, life_of_segment(s, i + 1));
                float const num3 = 0.5f * mathf::inverse_lerp(0.f, 0.3f, num2);
                vec2 v2 = rw::lerp(s.u_last[i], s.u_pos[i], t);
                if (i == n - 1 && life_of_segment(s, i) > 0.f)
                    v2 = rw::lerp(s.body_last[1], s.body_pos[1], t);
                vec2 const a = custom::perpendicular_vector(v2, vector);
                v[i * 4] = (vector + v2) / 2.f - a * (num3 + num) * 0.5f;
                v[i * 4 + 1] = (vector + v2) / 2.f + a * (num3 + num) * 0.5f;
                v[i * 4 + 2] = v2 - a * num3;
                v[i * 4 + 3] = v2 + a * num3;
                vec3 col = mix(fog, mix(vec3{1.f, 0.f, 0.f}, thread, 0.1f + 0.9f * mathf::pow(f, 0.25f + num2)), mathf::min(num2, b));
                if (dark && num2 > 0.f)
                    col = mix(col, black, darkness);
                for (uint32_t j = 0; j < 4; ++j)
                    cols[i * 4 + j] = vec4{col.x, col.y, col.z, 1.f};
                vector = v2;
                num = num3;
                b = num2;
            }
            vec3 const c0 = mix(fog, black, life_of_segment(s, 0));
            for (uint32_t k = 0; k < 4; ++k)
                cols[k] = vec4{c0.x, c0.y, c0.z, 1.f};
            sprites::long_mesh(out, v, cols, size_t(n) * 4);
        }

        // ApplyPalette
        vec3 yellow = mix(vec3{0.95f, 0.8f, 0.55f}, fog, 0.2f);
        vec3 a = vec3{1.f, 0.f, 0.f};
        vec3 color = vec3{1.f, 1.f, 1.f};
        if (rt > 0.f) {
            yellow = mix(yellow, vec3{0.6f, 0.4f, 0.2f}, rt);
            a = mix(a, vec3{0.7f, 0.2f, 0.2f}, rt);
            color = mix(color, vec3{0.9f, 0.5f, 0.5f}, rt * 0.5f);
        }
        if (dis > 0.f) {
            vec3 const g = mix(black, fog, 0.4f * mathf::pow(dis, 3.f));
            yellow = mix(yellow, g, dis);
            a = mix(a, g, dis);
            color = mix(color, g, dis);
        }
        if (darkness > 0.f) {
            yellow = mix(yellow, black, darkness);
            a = mix(a, black, darkness);
            color = mix(color, black, mathf::pow(darkness, 1.5f));
        }

        // DrawSprites
        vec2 vector = rw::lerp(w.body_chunk_get_last_pos(c), w.body_chunk_get_pos(c), t);
        vec2 const vector2 = rw::lerp(s.body_last[0], s.body_pos[0], t);
        vec2 vector3 = rw::lerp(s.body_last[1], s.body_pos[1], t);
        vec2 const an = -rw::slerp(s.last_needle_dir, s.needle_dir, t);
        vec2 const a2 = custom::dir_vec(vector, vector2);
        vec2 const a3 = custom::dir_vec(vector2, vector3);
        vector -= an * 7.f * s.size_fac;
        vector3 += a2 * (3.f * (1.f - s.bloat)) * s.size_fac;
        vec2 vector4 = vector - an * 18.f;
        vec2 vv = vector4;
        float num = 0.f;
        float const num2 = custom::lerp_map(rw::distance(vector, vector2) + rw::distance(vector2, vector3), 20.f, 140.f, 1.f, 0.3f, 2.f);
        float num3 = 1.f;
        if (dis > 0.f)
            num3 *= mathf::pow(1.f - dis, 0.2f);
        vec2 const a4 = custom::deg_to_vec(-45.f);
        constexpr uint32_t segs = maggot_mesh_segs;
        vec2 m0[segs * 4];
        vec2 m1[(segs - 3) * 4];
        for (uint32_t i = 0; i < segs; ++i) {
            float const num4 = mathf::inverse_lerp(1.f, float(segs - 1), float(i));
            float num5 = (i < 2 ? (0.5f + float(i)) : (custom::lerp_map(num4, 0.5f, 1.f, mathf::lerp(3.f, 2.5f, num4), 1.f, 3.f) * num2)) * num3;
            if (s.bloat > 0.f && i > 1)
                num5 = mathf::lerp(num5 * (1.2f + 0.65f * std::sin(mathf::pi * num4) * s.bloat), 1.f,
                    (0.5f + 0.5f * s.squeeze) * mathf::inverse_lerp(1.f - s.squeeze - 0.1f, 1.f - s.squeeze + 0.1f, num4));
            num5 *= s.size_fac;
            vec2 v5;
            if (i == 0)
                v5 = vector - an * 4.f;
            else if (num4 < 0.5f)
                v5 = custom::bezier(vector, vector + an * 2.f, vector2, vector2 - a2 * 4.f, mathf::inverse_lerp(0.f, 0.5f, num4));
            else
                v5 = custom::bezier(vector2, vector2 + a2 * 4.f, vector3, vector3 - a3 * 2.f, mathf::inverse_lerp(0.5f, 1.f, num4));
            vec2 const v6 = v5;
            vec2 const a5 = custom::perpendicular_vector(v6, vv);
            v5 += a5 * mathf::lerp(s.squirm[i][1], s.squirm[i][0], t) * num5 * (num4 * 0.3f + std::sin(num4 * mathf::pi));
            vec2 const a6 = custom::perpendicular_vector(v5, vector4);
            m0[i * 4] = (vector4 + v5) / 2.f - a6 * (num5 + num) * 0.5f;
            m0[i * 4 + 1] = (vector4 + v5) / 2.f + a6 * (num5 + num) * 0.5f;
            m0[i * 4 + 2] = v5 - a6 * num5;
            m0[i * 4 + 3] = v5 + a6 * num5;
            if (i > 1 && i < segs - 1) {
                float const dd = mathf::lerp(0.2f, 0.5f, std::sin(mathf::pi * mathf::pow(mathf::inverse_lerp(2.f, float(segs - 2), float(i)), 0.5f)));
                vec2 const mid = (vector4 + a4 * num * dd + v5 + a4 * num5 * dd) / 2.f;
                m1[(i - 2) * 4] = mid - a6 * (num5 + num) * 0.5f * dd;
                m1[(i - 2) * 4 + 1] = mid + a6 * (num5 + num) * 0.5f * dd;
                m1[(i - 2) * 4 + 2] = v5 + a4 * num5 * dd - a6 * num5 * dd;
                m1[(i - 2) * 4 + 3] = v5 + a4 * num5 * dd + a6 * num5 * dd;
            }
            vector4 = v5;
            vv = v6;
            num = num5;
        }
        vec4 c0[segs * 4];
        for (uint32_t i = 0; i < segs * 4; ++i) {
            float const value = float(i) / float(segs * 4 - 1);
            vec3 const cc = mix(a, yellow, 0.25f + mathf::inverse_lerp(0.f, 0.2f + 0.8f * s.new_and_pink, value) * 0.75f);
            c0[i] = vec4{cc.x, cc.y, cc.z, 1.f};
        }
        c0[0] = vec4{color.x, color.y, color.z, 1.f};
        c0[1] = c0[0];
        vec4 c1[(segs - 3) * 4];
        for (uint32_t j = 0; j < (segs - 3) * 4; ++j) {
            float const value = float(j) / float((segs - 3) * 4 - 1);
            c1[j] = vec4{color.x, color.y, color.z, mathf::inverse_lerp(0.f, 0.2f, value) * (1.f - 0.75f * rt - 0.25f * dis)};
        }
        sprites::long_mesh(out, m0, c0, segs * 4);
        sprites::long_mesh(out, m1, c1, (segs - 3) * 4);
    }
}
