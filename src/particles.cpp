#include <algorithm>
#include <cmath>

#include "particles.hpp"
#include "water.hpp"
#include "terrain.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "rooms.hpp"

namespace particles {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using world::room_view;
    namespace mathf = rw::mathf;
    using sound::sound_id;

    namespace {
        constexpr size_t max_particles = 4000;
        constexpr vec3 water_color{0.62f, 0.72f, 0.85f};
        constexpr vec3 dust_color{0.55f, 0.53f, 0.5f};
        constexpr vec3 spark_color{1.f, 0.88f, 0.6f};
        constexpr vec3 shock_color{0.7f, 0.8f, 1.f};

        vec2 rnv(rw::rng& r) { return rw::custom::deg_to_vec(r.value() * 360.f); }
        float range(rw::rng& r, float lo, float hi) { return lo + (hi - lo) * r.value(); }

        // the particles keep their own random numbers: cosmetic rolls must not
        // change the gameplay's (item spawns, AI) between runs. Every room
        // has its own, rooms spawn them while updating in parallel
        rw::rng& random(sys::game_state& state, dcon::room_id room) {
            return state.runtime(room).cosmetic;
        }

        // where the water's front surface is at `x` (waves included)
        float surface_y(sys::game_state const& state, room_view const& view, float x) {
            if (auto const* s = water::surface(state, view.id); s != nullptr)
                return s->level + water::height(state, *s, x, 0.f, 1.f);
            return view.float_water_level;
        }

        void dust(sys::game_state& state, dcon::room_id room, vec2 at, int count, float strength) {
            auto& r = random(state, room);
            for (int i = 0; i < count; ++i) {
                vec2 const v{range(r, -1.f, 1.f) * (1.f + strength), range(r, 0.f, 0.6f) * strength};
                spawn(state, room, kind::dust, at + vec2{range(r, -6.f, 6.f), 0.f}, v, range(r, 3.f, 6.f), range(r, 0.35f, 0.7f), dust_color, 0.35f);
            }
        }

        void sparks(sys::game_state& state, dcon::room_id room, vec2 at, int count, float speed, vec3 color) {
            auto& r = random(state, room);
            for (int i = 0; i < count; ++i)
                spawn(state, room, kind::spark, at, rnv(r) * range(r, 0.3f, 1.f) * speed, range(r, 1.f, 2.f), range(r, 0.12f, 0.35f), color);
        }
    }

    void spawn(sys::game_state& state, dcon::room_id room, kind k, vec2 pos, vec2 vel, float size, float seconds, vec3 color, float alpha) {
        // only where a camera looks
        if (!state.viewed(room))
            return;
        // the capacity is reserved once: update() spawns while it walks the vector
        state.particles.reserve(max_particles);
        auto& out = state.parallel ? state.runtime(room).particles : state.particles;
        if (out.size() >= max_particles)
            return;
        sys::game_state::particle p;
        p.room = room;
        p.kind = uint8_t(k);
        p.pos = p.last_pos = pos;
        p.vel = vel;
        p.size = size;
        p.decay = 1.f / mathf::max(1.f, seconds * float(state.ticks_per_second));
        p.color = color;
        p.alpha = alpha;
        out.push_back(p);
    }

    void splash(sys::game_state& state, dcon::room_id room, vec2 pos, float strength) {
        if (!state.viewed(room))
            return;
        auto& r = random(state, room);
        int const n = int(mathf::min(strength * 2.5f, 28.f));
        for (int i = 0; i < n; ++i) {
            float const spread = range(r, -1.f, 1.f);
            vec2 const v{spread * (1.f + strength * 0.25f), range(r, 0.4f, 1.f) * (2.f + strength * 0.6f)};
            spawn(state, room, kind::drop, pos + vec2{spread * 6.f, 1.f}, v, range(r, 0.8f, 1.6f), 2.f, water_color, 0.85f);
        }
    }

    void bubbles(sys::game_state& state, dcon::room_id room, vec2 pos, int count, float spread) {
        if (!state.viewed(room))
            return;
        auto& r = random(state, room);
        for (int i = 0; i < count; ++i)
            spawn(state, room, kind::bubble, pos + rnv(r) * spread * r.value(), rnv(r) * 0.6f, range(r, 1.2f, 3.2f), range(r, 1.5f, 3.f), water_color, 0.8f);
    }

    void centipede_shell(sys::game_state& state, dcon::room_id room, vec2 pos, vec2 vel, float hue, float saturation, float scale_x, float scale_y) {
        if (!state.viewed(room))
            return;
        size_t const before = (state.parallel ? state.runtime(room).particles : state.particles).size();
        spawn(state, room, kind::centipede_shell, pos + vel, vel, 1.f, 1.f, vec3{1.f, 1.f, 1.f});
        auto& out = state.parallel ? state.runtime(room).particles : state.particles;
        if (out.size() == before)
            return;
        auto& r = random(state, room);
        auto& p = out.back();
        // CentipedeShell's constructor; it goes when its counters say so
        p.last_pos = pos;
        p.decay = 0.f;
        p.hue = hue;
        p.saturation = saturation;
        p.scale = vec2{scale_x, scale_y};
        p.rotation = r.value() * 360.f;
        p.last_rotation = p.rotation;
        p.rot_vel = mathf::lerp(-1.f, 1.f, r.value()) * rw::custom::lerp_map(rw::magnitude(vel), 0.f, 18.f, 5.f, 26.f);
        p.z_rotation = r.value() * 360.f;
        p.last_z_rotation = p.rotation;
        p.z_rot_vel = mathf::lerp(-1.f, 1.f, r.value()) * rw::custom::lerp_map(rw::magnitude(vel), 0.f, 18.f, 2.f, 16.f);
    }

    void from_sounds(sys::game_state& state, size_t first_sound) {
        for (size_t i = first_sound; i < state.sounds.size(); ++i) {
            auto const& snd = state.sounds[i];
            if (snd.room == 0xFFFFFFFFu)
                continue;
            dcon::room_id const room{dcon::room_id::value_base_t(snd.room)};
            if (!state.viewed(room))
                continue;
            auto& r = random(state, room);
            vec2 const at = snd.pos;
            vec2 const feet = at - vec2{0.f, 14.f};
            switch (snd.id) {
            // pushing off and landing hard kick up dust
            case sound_id::slugcat_terrain_impact_hard:
            case sound_id::slugcat_terrain_impact_death:
                dust(state, room, feet, 9, 2.f);
                break;
            case sound_id::slugcat_terrain_impact_medium:
                dust(state, room, feet, 5, 1.2f);
                break;
            case sound_id::slugcat_floor_impact_standard:
                dust(state, room, feet, 2, 0.6f);
                break;
            case sound_id::slugcat_rocket_jump:
            case sound_id::slugcat_super_jump:
            case sound_id::slugcat_belly_slide_init:
                dust(state, room, feet, 6, 1.5f);
                break;
            case sound_id::slugcat_crouch_jump:
            case sound_id::slugcat_wall_jump:
            case sound_id::slugcat_roll_init:
                dust(state, room, feet, 3, 1.f);
                break;
            // weapons hitting things throw sparks
            case sound_id::rock_hit_wall:
            case sound_id::spear_bounce_off_wall:
                sparks(state, room, at, 6, 4.f, spark_color);
                dust(state, room, at, 2, 0.5f);
                break;
            case sound_id::spear_stick_in_wall:
            case sound_id::spear_stick_in_ground:
                sparks(state, room, at, 4, 3.f, spark_color);
                dust(state, room, at, 3, 0.7f);
                break;
            case sound_id::weapon_skid:
                if (r.value() < 0.5f)
                    sparks(state, room, at, 1, 2.f, spark_color);
                break;
            case sound_id::rock_hit_creature:
                spawn(state, room, kind::flash, at, vec2{0.f, 0.f}, 6.f, 0.18f, vec3{1.f, 1.f, 1.f}, 0.9f);
                sparks(state, room, at, 5, 3.f, vec3{1.f, 1.f, 1.f});
                break;
            case sound_id::centipede_shock:
                spawn(state, room, kind::flash, at, vec2{0.f, 0.f}, 14.f, 0.3f, shock_color, 1.f);
                sparks(state, room, at, 18, 7.f, shock_color);
                break;
            default:
                break;
            }
        }
    }

    void ambient(sys::game_state& state, room_view const& room) {
        auto& w = state.world;
        if (!state.viewed(room.id))
            return;
        auto& r = random(state, room.id);
        // RoomRain: streaks from the top of the room
        // (a storm over sand is a sandstorm, terrain.cpp blows that)
        float const rain = terrain::sand_if_built(state, room.id) ? 0.f : mathf::max(w.room_get_rain_intensity(room.id) * 0.6f, state.storm ? 1.f : 0.f);
        // (RoomRain's sounds follow the rain cycle's intensity, which isn't
        // ported: once it is, its DisembodiedDynamicSoundLoops go here as
        // disembodied loop requests, owner loop_owner::rain: normal_rain_loop
        // at 0.1 + 0.9 * clamp01(sin(InverseLerp(0.001, 0.7, i) * pi))^1.5,
        // heavy_rain_loop at InverseLerp(0.12, 0.5, i)^0.85, then the death
        // rain, rumble, flood and approaching loops)
        if (rain > 0.f) {
            float const want = rain * room.pixel_width() / 260.f;
            int n = int(want);
            if (r.value() < want - float(n))
                ++n;
            for (int i = 0; i < n; ++i) {
                vec2 const at{range(r, -40.f, room.pixel_width() + 40.f), room.pixel_height() + 20.f};
                spawn(state, room.id, kind::rain, at, vec2{range(r, -1.5f, -0.5f), range(r, -26.f, -20.f)}, range(r, 0.8f, 1.2f), 3.f, water_color, 0.35f + 0.3f * rain);
            }
        }
        // WaterDrip: drops forming under ceilings
        float const drips = w.room_get_ceiling_drips(room.id);
        if (drips > 0.f && r.value() < drips * 0.3f) {
            for (int k = 0; k < 6; ++k) {
                int32_t const x = r.range(0, room.width);
                int32_t const y = r.range(1, room.height);
                if (room.get_tile(x, y).solid() && !room.get_tile(x, y - 1).solid()) {
                    vec2 const at = room_view::middle_of_tile(x, y) + vec2{range(r, -8.f, 8.f), -11.f};
                    spawn(state, room.id, kind::drop, at, vec2{0.f, -0.5f}, 1.f, 4.f, water_color, 0.8f);
                    break;
                }
            }
        }
        // Bubble: what a submerged slugcat breathes out (more when running out of air)
        if (room.water) {
            rooms::for_each_player(state, room.id, [&](dcon::player_id p) {
                if (w.player_get_in_shortcut(p) || w.player_get_eaten(p))
                    return;
                vec2 const head = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
                if (!room.point_submerged(head + vec2{0.f, 8.f}))
                    return;
                float const air = w.player_get_air_in_lungs(p);
                if (r.value() < 0.03f + 0.12f * (1.f - air))
                    bubbles(state, room.id, head + vec2{0.f, 4.f}, 1 + int(r.value() * 2.f), 3.f);
            });
        }
    }

    void update(sys::game_state& state) {
        auto& ps = state.particles;
        ps.reserve(max_particles);
        // a room no camera looks at any more lets its particles go
        std::erase_if(ps, [&](auto const& p) { return !state.viewed(p.room); });
        // room views of the rooms in use
        dcon::room_id view_room;
        room_view view;
        size_t const count = ps.size();
        for (size_t i = 0; i < count; ++i) {
            auto& p = ps[i];
            if (p.room != view_room) {
                view_room = p.room;
                view = world::get_room_view(state, p.room);
            }
            auto& r = random(state, p.room);
            p.last_pos = p.pos;
            p.life += p.decay;
            auto const k = kind(p.kind);
            bool const wet = view.water && view.point_submerged(p.pos);
            switch (k) {
            case kind::drop:
            case kind::rain: {
                if (k == kind::drop)
                    p.vel.y -= 0.9f * view.gravity;
                p.pos += p.vel;
                if (view.get_tile(p.pos).solid() || p.pos.y < -50.f) {
                    // a drop bursting on the ground
                    if (p.size >= 0.8f && r.value() < (k == kind::rain ? 0.35f : 0.8f)) {
                        vec2 const at = p.last_pos;
                        for (int j = 0; j < 2; ++j)
                            spawn(state, p.room, kind::drop, at, vec2{range(r, -1.5f, 1.5f), range(r, 0.8f, 2.f)}, 0.5f, 0.4f, p.color, p.alpha * 0.8f);
                    }
                    p.life = 1.f;
                } else if (view.water && p.last_pos.y >= surface_y(state, view, p.pos.x) && p.pos.y < surface_y(state, view, p.pos.x)) {
                    // hit the water (it moved through the surface this tick)
                    float const at = surface_y(state, view, p.pos.x);
                    water::disturb(state, p.room, p.pos.x, std::fabs(p.vel.y) * 0.04f * p.size, 10.f);
                    if (p.size >= 0.8f && r.value() < 0.3f)
                        spawn(state, p.room, kind::drop, vec2{p.pos.x, at + 1.f}, vec2{range(r, -1.f, 1.f), range(r, 1.f, 2.5f)}, 0.5f, 0.3f, p.color, 0.7f);
                    p.life = 1.f;
                } else if (view.water && p.pos.y < surface_y(state, view, p.pos.x) - 4.f && k == kind::rain) {
                    // spawned under the water: never falls through it
                    p.life = 1.f;
                }
                break;
            }
            case kind::spark:
                p.vel *= 0.86f;
                p.vel.y -= 0.35f;
                p.pos += p.vel;
                if (view.get_tile(p.pos).solid())
                    p.life = 1.f;
                break;
            case kind::dust:
                p.vel *= 0.88f;
                p.pos += p.vel;
                p.size += 0.25f;
                break;
            case kind::bubble:
                if (!wet) {
                    // popped at the surface
                    if (view.water)
                        water::disturb(state, p.room, p.pos.x, -0.15f * p.size, 6.f);
                    p.life = 1.f;
                    break;
                }
                p.vel.y += 0.12f;
                p.vel *= 0.9f;
                p.vel.x += std::sin(p.life * 25.f + float(i)) * 0.06f;
                p.pos += p.vel;
                if (view.get_tile(p.pos).solid())
                    p.life = 1.f;
                break;
            case kind::flash:
                p.size *= 1.12f;
                break;
            case kind::centipede_shell: {
                // CentipedeShell.Update: tumbling, sinking slowly in water,
                // bouncing off the terrain, then lying still until it fades
                ++p.counter;
                if (wet) {
                    p.vel *= 0.92f;
                    p.vel.y -= view.gravity * 0.1f;
                    p.rot_vel *= 0.965f;
                    p.z_rot_vel *= 0.965f;
                } else {
                    p.vel *= 0.999f;
                    p.vel.y -= view.gravity * 0.9f;
                }
                float const moved = rw::distance(p.last_pos, p.pos);
                p.last_rotation = p.rotation;
                p.rotation += p.rot_vel * moved;
                p.last_z_rotation = p.z_rotation;
                p.z_rotation += p.z_rot_vel * moved;
                vec2 next = p.pos + p.vel;
                // (SharedPhysics' terrain collision as two axis checks)
                ivec2 contact{0, 0};
                if (view.get_tile(vec2{next.x, p.pos.y}).solid()) {
                    contact.x = p.vel.x > 0.f ? 1 : -1;
                    next.x = p.pos.x;
                    p.vel.x = -p.vel.x * 0.15f;
                }
                if (view.get_tile(vec2{next.x, next.y - 3.f}).solid() && p.vel.y <= 0.f) {
                    contact.y = -1;
                    next.y = room_view::middle_of_tile(vec2{next.x, next.y - 3.f}).y + 13.f;
                    p.vel.y = std::fabs(p.vel.y) * 0.15f;
                } else if (view.get_tile(vec2{next.x, next.y + 3.f}).solid() && p.vel.y > 0.f) {
                    contact.y = 1;
                    p.vel.y = -std::fabs(p.vel.y) * 0.15f;
                    next.y = p.pos.y;
                }
                if (contact != ivec2{0, 0}) {
                    p.rot_vel *= 0.8f;
                    p.z_rot_vel *= 0.8f;
                }
                p.pos = next;
                if (contact.x != 0)
                    p.vel.y *= 0.6f;
                if (contact.y != 0)
                    p.vel.x *= 0.6f;
                if (contact.y < 0) {
                    p.rot_vel *= 0.7f;
                    p.z_rot_vel *= 0.7f;
                    if (rw::magnitude(p.vel) < 1.f && ++p.disappear > 30)
                        p.counter = std::max(p.counter, 300);
                }
                if (p.disappear > 390 || p.counter > 380 || p.pos.x < -100.f || p.pos.y < -100.f)
                    p.life = 1.f;
                break;
            }
            }
        }
        ps.erase(std::remove_if(ps.begin(), ps.end(), [](auto const& p) { return p.life >= 1.f; }), ps.end());
    }
}
