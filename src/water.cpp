#include <algorithm>
#include <cmath>

#include "water.hpp"
#include "particles.hpp"
#include "physics.hpp"

namespace water {
    using rw::vec2;
    using world::room_view;
    namespace mathf = rw::mathf;
    using surface_t = sys::game_state::water_surface;

    namespace {
        // wave speed squared (in nodes per tick), damping, the pull back to rest
        constexpr float c2 = 0.32f;
        constexpr float depth_coupling = 0.55f;
        constexpr float damping = 0.991f;
        constexpr float restore = 0.0012f;
        constexpr float max_height = 30.f;
        constexpr uint32_t max_surfaces = 8;

        uint32_t at(surface_t const& s, uint32_t x, uint32_t z) { return z * s.nx + x; }

        surface_t* find(sys::game_state& state, dcon::room_id room) {
            for (auto& s : state.waters)
                if (s.room == room)
                    return &s;
            return nullptr;
        }

        surface_t& make(sys::game_state& state, room_view const& room) {
            auto& waters = state.waters;
            surface_t* slot = nullptr;
            if (waters.size() < max_surfaces) {
                slot = &waters.emplace_back();
            } else {
                slot = &*std::min_element(waters.begin(), waters.end(), [](auto const& a, auto const& b) { return a.last_used < b.last_used; });
            }
            surface_t& s = *slot;
            s = surface_t{};
            s.room = room.id;
            s.level = room.float_water_level;
            s.x0 = -100.f;
            s.nx = uint32_t((room.pixel_width() + 200.f) / s.spacing) + 1;
            s.h.assign(size_t(s.nx) * rows, 0.f);
            s.last_h = s.h;
            s.v = s.h;
            s.wall.assign(s.nx, 0);
            // walls cutting through the surface reflect the waves
            for (uint32_t x = 0; x < s.nx; ++x) {
                float const px = s.x0 + float(x) * s.spacing;
                s.wall[x] = room.get_tile(vec2{px, s.level - 4.f}).solid() && room.get_tile(vec2{px, s.level + 4.f}).solid() ? 1 : 0;
            }
            return s;
        }

        // the swells: a long one, a second one running against it and a short chop
        // across, shaped by the room's wave settings
        float swells(sys::game_state const& state, surface_t const& s, float x, float z, float t) {
            auto const& w = state.world;
            float const length = w.room_get_wave_length(s.room);
            float const amp = w.room_get_wave_amplitude(s.room);
            float const speed = w.room_get_wave_speed(s.room);
            float const length2 = w.room_get_second_wave_length(s.room);
            float const amp2 = w.room_get_second_wave_amplitude(s.room);
            // rooms without settings get a gentle default
            float const a1 = mathf::lerp(1.2f, 7.f, amp > 0.f ? amp : 0.4f);
            float const a2 = mathf::lerp(0.6f, 5.f, amp2 > 0.f ? amp2 : 0.3f);
            float const l1 = mathf::lerp(180.f, 620.f, length > 0.f ? length : 0.5f);
            float const l2 = mathf::lerp(90.f, 360.f, length2 > 0.f ? length2 : 0.5f);
            float const sp = mathf::lerp(0.4f, 2.2f, speed > 0.f ? speed : 0.5f);
            float const depth = z * 40.f;
            constexpr float tau = 6.2831853f;
            float hgt = a1 * std::sin(tau * (x * 0.96f + depth * 0.28f) / l1 - t * sp * tau / l1 * 6.f);
            hgt += a2 * std::sin(tau * (-x * 0.8f + depth * 0.6f) / l2 - t * sp * tau / l2 * 5.f + 1.7f);
            hgt += 0.7f * std::sin(tau * (x * 0.5f - depth * 0.86f) / 55.f - t * 0.13f + 0.4f);
            // a swell's crest is sharper than its trough
            return hgt - std::fabs(hgt) * 0.15f;
        }

        float sim_height(surface_t const& s, float x, float z, float alpha) {
            float const fx = (x - s.x0) / s.spacing;
            int32_t const ix = int32_t(std::floor(fx));
            float const tx = fx - float(ix);
            float const fz = mathf::clamp(z, 0.f, float(rows - 1));
            uint32_t const z0 = uint32_t(fz);
            uint32_t const z1 = std::min(z0 + 1, rows - 1);
            float const tz = fz - float(z0);
            auto const node = [&](int32_t i, uint32_t zz) {
                uint32_t const c = uint32_t(rw::custom::int_clamp(i, 0, int32_t(s.nx) - 1));
                return mathf::lerp(s.last_h[at(s, c, zz)], s.h[at(s, c, zz)], alpha);
            };
            // Catmull-Rom along x keeps the drawn surface smooth between nodes
            auto const row = [&](uint32_t zz) {
                float const p0 = node(ix - 1, zz), p1 = node(ix, zz), p2 = node(ix + 1, zz), p3 = node(ix + 2, zz);
                float const t = tx, t2 = t * t, t3 = t2 * t;
                return 0.5f * ((2.f * p1) + (-p0 + p2) * t + (2.f * p0 - 5.f * p1 + 4.f * p2 - p3) * t2 + (-p0 + 3.f * p1 - 3.f * p2 + p3) * t3);
            };
            return row(z0) + (row(z1) - row(z0)) * tz;
        }

        void step(surface_t& s) {
            uint32_t const nx = s.nx;
            for (uint32_t z = 0; z < rows; ++z) {
                uint32_t const zm = z > 0 ? z - 1 : 0;
                uint32_t const zp = std::min(z + 1, rows - 1);
                for (uint32_t x = 0; x < nx; ++x) {
                    uint32_t const i = at(s, x, z);
                    if (s.wall[x]) {
                        s.v[i] = 0.f;
                        continue;
                    }
                    // a wall reflects: its side counts as this node's own height
                    float const left = x > 0 && !s.wall[x - 1] ? s.h[at(s, x - 1, z)] : s.h[i];
                    float const right = x + 1 < nx && !s.wall[x + 1] ? s.h[at(s, x + 1, z)] : s.h[i];
                    float const lap = left + right - 2.f * s.h[i] + (s.h[at(s, x, zm)] + s.h[at(s, x, zp)] - 2.f * s.h[i]) * depth_coupling;
                    s.v[i] = (s.v[i] + c2 * lap - restore * s.h[i]) * damping;
                }
            }
            for (size_t i = 0; i < s.h.size(); ++i)
                s.h[i] = mathf::clamp(s.h[i] + s.v[i], -max_height, max_height);
        }

        // Water.Update's bodies: entering, leaving and moving along the surface
        void bodies(sys::game_state& state, surface_t& s, room_view const& room) {
            auto& w = state.world;
            w.room_for_each_object_location(room.id, [&](dcon::object_location_id l) {
                auto const o = w.object_location_get_object(l);
                w.physical_object_for_each_chunk_ownership_as_owner(o, [&](dcon::chunk_ownership_id co) {
                    auto const c = w.chunk_ownership_get_chunk(co);
                    vec2 const pos = w.body_chunk_get_pos(c);
                    vec2 const last = w.body_chunk_get_last_pos(c);
                    vec2 const vel = w.body_chunk_get_vel(c);
                    float const rad = w.body_chunk_get_rad(c);
                    float const mass = w.body_chunk_get_mass(c);
                    float const surf = s.level + sim_height(s, pos.x, 0.f, 1.f);
                    if (last.y >= surf && pos.y < surf) {
                        float const hit = mathf::min(-vel.y, 22.f) * mathf::lerp(mass, 1.f, 0.3f);
                        if (hit > 0.3f) {
                            disturb(state, room.id, pos.x, hit * 1.4f, rad * 2.f + 12.f);
                            particles::splash(state, room.id, vec2{pos.x, surf}, hit);
                            particles::bubbles(state, room.id, pos - vec2{0.f, rad}, int(mathf::min(hit * 1.5f, 12.f)), rad);
                        }
                    } else if (last.y < surf && pos.y >= surf) {
                        float const lift = mathf::min(vel.y, 18.f) * mathf::lerp(mass, 1.f, 0.3f);
                        if (lift > 0.3f) {
                            disturb(state, room.id, pos.x, -lift * 0.7f, rad * 2.f + 8.f);
                            particles::splash(state, room.id, vec2{pos.x, surf}, lift * 0.5f);
                        }
                    } else if (std::fabs(pos.y - surf) < rad + 4.f && std::fabs(vel.x) > 0.5f) {
                        // a wake: water piles up ahead and drops behind
                        float const k = mathf::min(std::fabs(vel.x), 8.f) * mathf::lerp(mass, 1.f, 0.3f) * 0.06f;
                        float const ahead = mathf::sign(vel.x) * (rad + 4.f);
                        disturb(state, room.id, pos.x + ahead, -k, rad + 6.f);
                        disturb(state, room.id, pos.x - ahead, k, rad + 6.f);
                    }
                });
            });
        }
    }

    sys::game_state::water_surface const* surface(sys::game_state const& state, dcon::room_id room) noexcept {
        for (auto const& s : state.waters)
            if (s.room == room)
                return &s;
        return nullptr;
    }

    float height(sys::game_state const& state, surface_t const& s, float x, float z, float alpha) noexcept {
        return sim_height(s, x, z, alpha) + swells(state, s, x, z, s.time - 1.f + alpha);
    }

    void disturb(sys::game_state& state, dcon::room_id room, float x, float strength, float width) {
        surface_t* s = find(state, room);
        if (s == nullptr || width <= 0.f)
            return;
        int32_t const c = int32_t((x - s->x0) / s->spacing);
        int32_t const r = int32_t(std::ceil(width / s->spacing));
        for (int32_t i = std::max(0, c - r); i <= std::min(int32_t(s->nx) - 1, c + r); ++i) {
            float const d = (s->x0 + float(i) * s->spacing - x) / width;
            float const fall = std::exp(-d * d * 2.5f);
            for (uint32_t z = 0; z < 3; ++z)
                s->v[at(*s, uint32_t(i), z)] -= strength * fall * (1.f - 0.3f * float(z)) * 0.35f;
        }
    }

    void update_room(sys::game_state& state, room_view const& room) {
        if (!room.water)
            return;
        surface_t* s = find(state, room.id);
        if (s == nullptr)
            s = &make(state, room);
        s->last_used = state.tick;
        s->time += 1.f;
        s->last_h = s->h;
        step(*s);
        step(*s);
        bodies(state, *s, room);
    }
}
