// EggBugEgg.cs and MoreSlugcats/FireEgg.cs (egg_bug_egg.hpp)

#include <cmath>

#include "egg_bug_egg.hpp"
#include "creature.hpp"
#include "egg_bug.hpp"
#include "egg_bug_graphics.hpp"
#include "egg_bug_types.hpp"
#include "items.hpp"
#include "particles.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "rooms.hpp"

namespace egg_bug_egg {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using world::room_view;
    using egg_bug::egg_state;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // FireEgg.explodeDuration
        constexpr int32_t explode_duration = 180;

        dcon::egg_bug_egg_data_id data(sys::game_state const& state, dcon::item_id it) {
            return state.world.item_get_data_from_egg_bug_egg_of(it);
        }

        // SwellFac
        float swell_fac(egg_state const& s) {
            return 1.f + 0.15f * s.swell;
        }

        // ResetSegments
        void reset_segments(egg_state& s, vec2 pos, vec2 rotation) {
            for (uint32_t i = 0; i < s.segment_count; ++i) {
                s.seg_pos[i] = pos + rotation * float(i);
                s.seg_last[i] = s.seg_pos[i];
                s.seg_vel[i] = vec2{0.f, 0.f};
            }
        }

        // ConnectSegment
        void connect_segment(egg_state& s, vec2 body, vec2 rotation, uint32_t i) {
            if (i == 0) {
                vec2 const v = body + rotation * 7.f * swell_fac(s);
                vec2 const a = custom::dir_vec(s.seg_pos[0], v);
                float const num = rw::distance(s.seg_pos[0], v);
                s.seg_pos[0] -= a * (2.f - num);
                s.seg_vel[0] -= a * (2.f - num);
                return;
            }
            vec2 const a2 = custom::dir_vec(s.seg_pos[i], s.seg_pos[i - 1]);
            float const num2 = rw::distance(s.seg_pos[i], s.seg_pos[i - 1]);
            s.seg_pos[i] -= a2 * (2.f - num2) * 0.5f;
            s.seg_vel[i] -= a2 * (2.f - num2) * 0.5f;
            s.seg_pos[i - 1] += a2 * (2.f - num2) * 0.5f;
            s.seg_vel[i - 1] += a2 * (2.f - num2) * 0.5f;
        }

        // Weapon.Mode Free for a FireEgg (ChangeMode: let go of what it stuck in)
        void unstick(egg_state& s) {
            s.stuck_in_wall = 0;
            s.stuck_in = dcon::creature_id{};
        }

        vec3 mix(vec3 a, vec3 b, float t) {
            return a + (b - a) * mathf::clamp01(t);
        }
    }

    bool is_egg(world::item_type t) noexcept {
        return t == world::item_type::egg_bug_egg || t == world::item_type::fire_egg;
    }

    void init(sys::game_state& state, dcon::item_id it) {
        auto& w = state.world;
        auto const o = items::body(state, it);
        bool const fire = w.item_get_type(it) == world::item_type::fire_egg;
        // the constructor's body: one chunk of 4 * SwellFac, 0.2
        w.physical_object_set_air_friction(o, 0.999f);
        w.physical_object_set_g(o, 0.9f);
        w.physical_object_set_bounce(o, 0.2f);
        w.physical_object_set_surface_friction(o, 0.7f);
        w.physical_object_set_water_friction(o, 0.95f);
        w.physical_object_set_buoyancy(o, 1.1f);
        auto const c = items::chunk(state, it);
        egg_state s;
        s.fire = fire ? 1 : 0;
        s.segment_count = uint8_t(fire ? 3 : 5);
        s.swell = 1.f;
        s.bites = 2;
        auto rng = rw::random(state.seed, state.tick, uint32_t(it.value));
        s.liquid_deplete = mathf::lerp(40.f, 80.f, rng.value());
        w.body_chunk_set_rad(c, 4.f * swell_fac(s));
        w.body_chunk_set_mass(c, 0.2f);
        w.item_set_rotation_speed(it, 0.f);
        reset_segments(s, w.body_chunk_get_pos(c), w.item_get_rotation(it));
        auto const d = w.create_egg_bug_egg_data();
        w.try_create_egg_bug_egg_of(d, it);
        w.egg_bug_egg_data_set_s(d, s);
    }

    void place(sys::game_state& state, dcon::item_id it) {
        auto& w = state.world;
        auto const d = data(state, it);
        if (!d)
            return;
        auto rng = rw::random(state.seed, state.tick, uint32_t(it.value));
        vec2 const rot = custom::rnv(rng);
        w.item_set_rotation(it, rot);
        w.item_set_last_rotation(it, rot);
        w.item_set_rotation_speed(it, 0.f);
        reset_segments(w.egg_bug_egg_data_get_s(d), w.body_chunk_get_pos(items::chunk(state, it)), rot);
    }

    void destroyed(sys::game_state& state, dcon::item_id it) {
        if (auto const d = data(state, it))
            state.world.delete_egg_bug_egg_data(d);
    }

    void dropped(sys::game_state& state, dcon::item_id it, float hue, vec2 vel, vec2 rotation, float rot_vel) {
        auto& w = state.world;
        auto const d = data(state, it);
        if (!d)
            return;
        auto& s = w.egg_bug_egg_data_get_s(d);
        auto const c = items::chunk(state, it);
        s.hue = hue;
        s.swell = 0.f;
        s.rot_vel = rot_vel;
        s.liquid = 1.f;
        w.body_chunk_set_vel(c, vel);
        w.body_chunk_set_rad(c, 4.f * swell_fac(s));
        w.item_set_rotation(it, rotation);
        w.item_set_last_rotation(it, rotation);
        w.item_set_rotation_speed(it, 0.f);
        reset_segments(s, w.body_chunk_get_pos(c), rotation);
    }

    bool update(sys::game_state& state, dcon::item_id it, room_view const& room) {
        auto& w = state.world;
        auto const d = data(state, it);
        if (!d)
            return false;
        auto& s = w.egg_bug_egg_data_get_s(d);
        auto rng = rw::random(state.seed, state.tick, uint32_t(it.value));
        auto const o = items::body(state, it);
        auto const c = items::chunk(state, it);
        bool const fire = s.fire != 0;
        bool const grabbed = w.physical_object_get_grabbed_by_count(o) > 0;
        vec2& pos = w.body_chunk_get_pos(c);
        vec2& vel = w.body_chunk_get_vel(c);
        // FireEgg: the fuse
        if (fire && s.active_counter > 0) {
            ++s.active_counter;
            if (s.active_counter == explode_duration)
                return true;
        }
        w.body_chunk_set_rad(c, 4.f * swell_fac(s));
        // (the items' own spin stays off: the egg turns by rotVel)
        w.item_set_rotation_speed(it, 0.f);
        vec2 rotation = w.item_get_rotation(it);
        if (!fire || s.active_counter > 0)
            s.swell = custom::lerp_and_tick(s.swell, 1.f, 0.01f, rng.value() / 80.f);
        if (grabbed) {
            // held: hanging off the hand (FireEgg.Grabbed: let go of what it stuck in)
            if (auto const h = w.item_get_grabbed_by(it); h && w.creature_is_valid(h)) {
                rotation = custom::perpendicular_vector(custom::dir_vec(pos, creature::main_pos(state, h)));
                rotation.y = std::fabs(rotation.y);
            }
            if (fire)
                unstick(s);
        }
        bool const stuck = s.stuck_in_wall || s.stuck_in;
        if (w.body_chunk_get_contact_point(c).y < 0 && (!fire || !stuck)) {
            s.rot_vel = mathf::lerp(s.rot_vel, 0.12f * vel.x, 0.8f);
            vel.x *= 0.8f;
        }
        if (s.rot_vel > 0.f)
            rotation = rw::normalized(rotation - custom::perpendicular_vector(rotation) * s.rot_vel);

        if (fire) {
            // flying with its fuse lit: it sticks to the first creature (not
            // its thrower) or wall it meets
            if (!stuck && !grabbed && s.active_counter > 0) {
                dcon::creature_id hit;
                creature::hit_part part;
                if (creature::trace(state, room.id, pos, pos + vel, 1.f, s.thrown_by, hit, part) && part.chunk >= 0 && hit != s.thrown_by) {
                    s.stuck_in = hit;
                    s.stuck_chunk = uint8_t(part.chunk);
                } else if (w.body_chunk_get_contact_point(c) != ivec2{0, 0}) {
                    s.stuck_in_wall = 1;
                    s.stuck_pos = pos;
                }
            }
            if (s.stuck_in) {
                s.rot_vel = 0.f;
                auto const t = s.stuck_in;
                if (!creature::live(state, t) || w.creature_get_away(t) != 0 || w.creature_get_entering_shortcut(t).x >= 0
                || s.stuck_chunk >= creature::chunk_count_of(state, t)) {
                    unstick(s);
                } else {
                    auto const k = creature::chunk_of(state, t, s.stuck_chunk);
                    vel = w.body_chunk_get_vel(k);
                    pos = w.body_chunk_get_pos(k);
                }
            } else if (s.stuck_in_wall) {
                pos = s.stuck_pos;
                vel = vec2{0.f, 0.f};
                s.rot_vel = 0.f;
            }
        }
        w.item_set_rotation(it, rotation);

        // the tail of goo
        uint32_t const n = s.segment_count;
        for (uint32_t i = 0; i < n; ++i) {
            float const value = float(i) / float(n - 1);
            s.seg_last[i] = s.seg_pos[i];
            s.seg_pos[i] += s.seg_vel[i] * (fire ? 0.5f : 1.f);
            s.seg_vel[i] *= 0.995f;
            s.seg_vel[i].y -= 0.9f * mathf::inverse_lerp(0.5f, 1.f, value);
            s.seg_vel[i] += rotation * (fire ? 1.f : 5.f) * mathf::inverse_lerp(0.5f, 0.f, value);
            if (i > 1) {
                vec2 const dv = custom::dir_vec(s.seg_pos[i - 2], s.seg_pos[i]);
                s.seg_vel[i] += dv;
                s.seg_vel[i - 2] -= dv;
            }
            connect_segment(s, pos, rotation, i);
        }
        for (int32_t j = int32_t(n) - 1; j >= 0; --j)
            connect_segment(s, pos, rotation, uint32_t(j));
        for (uint32_t k = 0; k < n; ++k)
            connect_segment(s, pos, rotation, k);

        // dripping (EggBugEgg.LiquidDrip, cosmetic)
        if (s.liquid > 0.f) {
            s.liquid -= 1.f / s.liquid_deplete;
            if (state.viewed(room.id) && rng.value() < 0.25f && rng.value() < mathf::inverse_lerp(0.f, 0.5f, s.liquid)) {
                vec3 const col = custom::hsl2rgb(custom::decimal(s.hue + 1.5f), 1.f, 0.5f) * 0.6f;
                if (s.bites < 2) {
                    particles::spawn(state, room.id, particles::kind::drop, pos, rotation * 3.f + custom::rnv(rng) * rng.value() * 3.f, 1.5f, 0.75f, col);
                } else {
                    vec2 dv = custom::dir_vec(s.seg_pos[n - 2], s.seg_pos[n - 1]) * mathf::lerp(8.f, 1.f, s.liquid);
                    dv += s.seg_vel[n - 1];
                    dv += custom::rnv(rng) * 4.f * rng.value();
                    particles::spawn(state, room.id, particles::kind::drop, rw::lerp(s.seg_last[n - 1], s.seg_pos[n - 1], rng.value()), dv, 1.5f, 0.75f, col);
                }
            }
        }
        return false;
    }

    void tossed(sys::game_state& state, dcon::item_id it, dcon::creature_id by) {
        auto const d = data(state, it);
        if (!d)
            return;
        auto& s = state.world.egg_bug_egg_data_get_s(d);
        if (!s.fire)
            return;
        s.thrown_by = by;
        if (s.active_counter == 0)
            s.active_counter = 1;
    }

    void explode(sys::game_state& state, room_view const& room, dcon::item_id it, vec2) {
        auto& w = state.world;
        auto const d = data(state, it);
        if (!d)
            return;
        auto const& s = w.egg_bug_egg_data_get_s(d);
        auto const c = items::chunk(state, it);
        vec2 const at = rw::lerp(w.body_chunk_get_pos(c), w.body_chunk_get_last_pos(c), 0.35f);
        egg_bug::explosion e;
        e.life_time = 7;
        e.rad = 225.f;
        e.force = 4.2f;
        e.damage = 50.f;
        e.stun = 280.f;
        e.min_stun = 160.f;
        e.color = custom::hsl2rgb(custom::decimal(s.hue), 1.f, 0.75f);
        e.kill_tag = s.thrown_by;
        e.kill_tag_factor = 0.7f;
        egg_bug::explode(state, room, at, e, items::body(state, it));
        // (no Bomb_Explode sound id)
        state.add_noise(room.id, at, 9000.f, items::body(state, it));
    }

    bool bite(sys::game_state& state, dcon::item_id it, dcon::player_id p) {
        auto& w = state.world;
        auto const d = data(state, it);
        if (!d)
            return true;
        auto& s = w.egg_bug_egg_data_get_s(d);
        if (s.bites > 0)
            --s.bites;
        // (no Slugcat_Bite_Dangle_Fruit / Slugcat_Eat_Dangle_Fruit sounds)
        w.body_chunk_set_pos(items::chunk(state, it), w.body_chunk_get_pos(player::body_chunk(state, p, 0)));
        s.liquid = 1.f;
        return s.bites < 1;
    }

    int32_t bites_left(sys::game_state const& state, dcon::item_id it) noexcept {
        auto const d = data(state, it);
        return d ? int32_t(state.world.egg_bug_egg_data_get_s(d).bites) : 0;
    }

    int32_t nourishment(world::item_type t, player::slugcat_name name) noexcept {
        using player::slugcat_name;
        if (name == slugcat_name::spear)
            return 0;
        if (name == slugcat_name::red || name == slugcat_name::artificer)
            return t == world::item_type::egg_bug_egg ? 4 : 1;
        return 4;
    }

    void build(sys::game_state const& state, dcon::item_id it, float alpha, vec3 black, vec3 fog, float darkness, std::vector<sprites::draw_cmd>& out) {
        auto const& w = state.world;
        auto const d = data(state, it);
        if (!d)
            return;
        auto const& s = w.egg_bug_egg_data_get_s(d);
        bool const fire = s.fire != 0;
        auto const c = items::chunk(state, it);
        float const sf = swell_fac(s);
        vec2 const rot = rw::slerp(w.item_get_last_rotation(it), w.item_get_rotation(it), alpha);
        vec2 const pos = rw::lerp(w.body_chunk_get_last_pos(c), w.body_chunk_get_pos(c), alpha) - rot * 3.f * sf;
        float const deg = custom::vec_to_deg(rot);
        vec3 cols[3];
        egg_bug_graphics::egg_colors(black, fog, s.hue, darkness, fire, cols);

        // the tail mesh (MakeLongMesh(segments))
        if (s.bites >= 2) {
            vec2 v[egg_bug::max_egg_segments * 4];
            vec2 v3 = pos + rot * 5.f * sf;
            float num = 1.f;
            uint32_t const n = s.segment_count;
            for (uint32_t j = 0; j < n; ++j) {
                float const f = float(j) / float(n - 1);
                vec2 const v4 = rw::lerp(s.seg_last[j], s.seg_pos[j], alpha);
                vec2 const nd = rw::normalized(v4 - v3);
                vec2 const a = custom::perpendicular_vector(nd);
                float const num2 = mathf::lerp(1.f, 0.5f, mathf::pow(f, 0.25f));
                float const dd = rw::distance(v4, v3) / 5.f;
                if (j == 0) {
                    v[0] = v3 - a * num2;
                    v[1] = v3 + a * num2;
                } else {
                    v[j * 4] = v3 - a * (num2 + num) * 0.5f + nd * dd;
                    v[j * 4 + 1] = v3 + a * (num2 + num) * 0.5f + nd * dd;
                }
                v[j * 4 + 2] = v4 - a * num2 - nd * dd;
                v[j * 4 + 3] = v4 + a * num2 - nd * dd;
                v3 = v4;
                num = num2;
            }
            sprites::long_mesh(out, v, size_t(n) * 4, cols[0]);
        }
        if (!fire) {
            // DangleFruit0A (bitten: 1A), EggBugEggColor (Eaten), JetFishEyeA
            bool const eaten = s.bites < 2;
            sprites::add(out, sprites::sprite{eaten ? "DangleFruit1A" : "DangleFruit0A", pos, deg, vec2{0.7f * sf, 0.75f * sf}, vec2{0.5f, 0.3f}, cols[0]});
            sprites::add(out, sprites::sprite{eaten ? "EggBugEggColorEaten" : "EggBugEggColor", pos, deg, vec2{0.7f * sf, 0.75f * sf}, vec2{0.5f, 0.3f}, cols[1]});
            sprites::add(out, sprites::sprite{"JetFishEyeA", pos, deg, vec2{0.45f * sf, 0.45f * sf}, vec2{0.5f, eaten ? 0.4f : 0.7f}, cols[2]});
            return;
        }
        // FireEgg: LegsAPole (shaking and flickering white as the fuse
        // burns), JetFishEyeB, the WaterNut bubble (a pale Circle20 here)
        float const k = float(s.active_counter) / float(explode_duration);
        float const num = k * k;
        float const a = float(s.active_counter);
        float const shake = std::sin(a / 27.f * mathf::pi) * num * 80.f;
        vec3 col = cols[1];
        if (s.active_counter > 0) {
            float const num3 = std::fabs(std::sin(a / mathf::max(1.f, 80.f * (1.f - num * 0.8f)) * mathf::pi)) * 0.45f;
            col = mix(col, vec3{1.f, 1.f, 1.f}, num3);
        }
        sprites::add(out, sprites::sprite{"LegsAPole", pos, deg + shake,
                              vec2{sf + std::fabs(std::sin(a / 40.f * mathf::pi) * num * 0.95f), sf + std::fabs(std::cos(a / 40.f * mathf::pi) * num * 0.95f)}, vec2{0.5f, 0.5f}, col});
        sprites::add(out, sprites::sprite{"JetFishEyeB", pos + rot * 5.f, deg + shake, vec2{sf, sf}, vec2{0.5f, 0.5f}, vec3{1.f, 1.f, 1.f}});
        float const bs = (0.85f * sf + 0.6f * num) * 0.8f;
        vec4 const bc{1.f, 1.f, 1.f, 0.3f};
        vec4 const corners[4] = {bc, bc, bc, bc};
        sprites::add(out, sprites::sprite{"Circle20", pos, deg + shake, vec2{bs, bs}, vec2{0.5f, 0.5f}, vec3{1.f, 1.f, 1.f}}, corners);
    }
}
