#include <algorithm>
#include <cmath>
#include <vector>

#include "terrain.hpp"
#include "ve_utils.hpp"
#include "particles.hpp"
#include "physics.hpp"
#include "rooms.hpp"
#include "creature.hpp"

namespace terrain {
    using rw::vec2;
    using world::room_view;
    using world::thing_kind;
    namespace mathf = rw::mathf;
    using sand_t = sys::game_state::sand_field;

    namespace {
        // sand holds slopes up to about 40 degrees
        constexpr float repose = 0.85f;
        // how fast bodies press into it, and how deep a mark gets next to its surroundings
        constexpr float press_rate = 0.02f;
        constexpr float max_mark = 7.f;
        // the sandstorm's dunes and how gently it reshapes them
        constexpr float dune_length = 520.f;
        constexpr float storm_rate = 0.0007f;
        // the per tile layer: a tile turns solid once the sand is this far
        // over its top across its whole width, and back to air once its top
        // is uncovered (the hysteresis keeps footprints from flipping tiles)
        constexpr float pack_depth = 8.f;
        constexpr float unpack_depth = 0.f;
        // how often (ticks) a room checks its sand tiles against the surface
        constexpr uint32_t retile_period = 10;

        sand_t* find(sys::game_state& state, dcon::room_id room) {
            if (!state.has_runtime(room))
                return nullptr;
            auto& s = state.runtime(room).sand;
            return s ? &*s : nullptr;
        }

        float bezier(float a, float b, float c, float d, float t) {
            float const u = 1.f - t;
            return u * u * u * a + 3.f * u * u * t * b + 3.f * u * t * t * c + t * t * t * d;
        }

        // the curve through the handles, sampled every `spacing` pixels
        bool build(sys::game_state& state, dcon::room_id room, sand_t& s) {
            auto const& w = state.world;
            struct handle {
                vec2 pos, left, right;
                float back;
            };
            std::vector<handle> hs;
            w.room_for_each_thing_location(room, [&](dcon::thing_location_id l) {
                auto const t = w.thing_location_get_thing(l);
                if (w.placed_thing_get_kind(t) == thing_kind::terrain_handle)
                    hs.push_back({w.placed_thing_get_pos(t), w.placed_thing_get_handle(t), w.placed_thing_get_handle2(t), w.placed_thing_get_extra(t)});
            });
            if (hs.empty())
                return false;
            std::sort(hs.begin(), hs.end(), [](auto const& a, auto const& b) { return a.pos.x < b.pos.x; });
            // the curve as a polyline
            std::vector<vec2> line;
            std::vector<float> back_at;
            line.push_back(hs.front().pos);
            back_at.push_back(hs.front().back);
            for (size_t i = 0; i + 1 < hs.size(); ++i) {
                auto const& a = hs[i];
                auto const& b = hs[i + 1];
                for (int k = 1; k <= 48; ++k) {
                    float const t = float(k) / 48.f;
                    line.push_back(vec2{bezier(a.pos.x, a.pos.x + a.right.x, b.pos.x + b.left.x, b.pos.x, t),
                        bezier(a.pos.y, a.pos.y + a.right.y, b.pos.y + b.left.y, b.pos.y, t)});
                    back_at.push_back(mathf::lerp(a.back, b.back, t));
                }
            }
            float const width = float(w.room_get_width(room)) * 20.f;
            s = sand_t{};
            s.room = room;
            s.x0 = -200.f;
            uint32_t const n = uint32_t((width + 400.f) / s.spacing) + 1;
            s.h.resize(n);
            s.back.resize(n);
            size_t j = 0;
            for (uint32_t i = 0; i < n; ++i) {
                float const x = s.x0 + float(i) * s.spacing;
                if (x <= line.front().x) {
                    s.h[i] = line.front().y;
                    s.back[i] = back_at.front();
                    continue;
                }
                if (x >= line.back().x) {
                    s.h[i] = line.back().y;
                    s.back[i] = back_at.back();
                    continue;
                }
                while (j + 1 < line.size() && line[j + 1].x < x)
                    ++j;
                size_t const k = std::min(j + 1, line.size() - 1);
                float const t = line[k].x > line[j].x ? (x - line[j].x) / (line[k].x - line[j].x) : 0.f;
                s.h[i] = mathf::lerp(line[j].y, line[k].y, t);
                s.back[i] = mathf::lerp(back_at[j], back_at[k], t);
            }
            s.left = line.front().x;
            s.right = line.back().x;
            // the sandstorm's dunes keep the sand's average level and roughly its relief
            double sum = 0.0, sq = 0.0;
            for (float const v : s.h)
                sum += v;
            s.mean = float(sum / double(n));
            for (float const v : s.h)
                sq += double((v - s.mean) * (v - s.mean));
            s.amplitude = mathf::clamp(float(std::sqrt(sq / double(n))) * 1.2f, 6.f, 80.f);
            s.phase = 0.f;
            return true;
        }

        // the lowest surface over a tile column's 20 px
        float lowest_over(sand_t const& s, int32_t tx) {
            float const a = float(tx) * 20.f, b = a + 20.f;
            int32_t const n = int32_t(s.h.size());
            int32_t const i0 = rw::custom::int_clamp(int32_t(std::floor((a - s.x0) / s.spacing)), 0, n - 1);
            int32_t const i1 = rw::custom::int_clamp(int32_t(std::ceil((b - s.x0) / s.spacing)), 0, n - 1);
            float m = s.h[size_t(i0)];
            for (int32_t i = i0 + 1; i <= i1; ++i)
                m = mathf::min(m, s.h[size_t(i)]);
            return m;
        }

        // The per tile layer against the surface: the buried tiles solid in
        // the room's tiles, the uncovered ones back to air, the partly
        // covered ones marked volatile. Pipes opening into the sand stay
        // open (their tiles and the ones around them never fill), and only
        // tiles that are air in the level ever change. Bumps the room's
        // geometry_version when a tile changed (the path fields rebuild)
        void retile(sys::game_state& state, dcon::room_id room, sand_t& s, bool first) {
            auto& w = state.world;
            int32_t const width = w.room_get_width(room);
            int32_t const height = w.room_get_height(room);
            auto const base = w.room_get_first_tile(room).id.index();
            auto tile_at = [&](int32_t x, int32_t y) -> world::packed_tile& {
                return w.tile_get_bits(dcon::tile_id{dcon::tile_id::value_base_t(base + y * width + x)});
            };
            if (first) {
                s.tiles_w = width;
                s.tiles.assign(size_t(width) * size_t(height), 0);
                for (int32_t y = 0; y < height; ++y)
                    for (int32_t x = 0; x < width; ++x) {
                        auto const& t = tile_at(x, y);
                        if (t.t() != world::terrain::air || t.shortcut() != 0)
                            continue;
                        bool near_pipe = false;
                        for (int32_t dx = -1; dx <= 1 && !near_pipe; ++dx)
                            for (int32_t dy = -1; dy <= 1 && !near_pipe; ++dy) {
                                int32_t const nx = x + dx, ny = y + dy;
                                if (nx < 0 || ny < 0 || nx >= width || ny >= height)
                                    continue;
                                auto const& n = tile_at(nx, ny);
                                near_pipe = n.t() == world::terrain::shortcut_entrance || n.shortcut() != 0;
                            }
                        if (!near_pipe)
                            s.tiles[size_t(y * width + x)] = sys::sand_tile_convertible;
                    }
            }
            bool changed = false;
            for (int32_t x = 0; x < width; ++x) {
                // (only columns the handles' span covers whole)
                bool const covered = float(x) * 20.f >= s.left && float(x + 1) * 20.f <= s.right;
                float const low = covered ? lowest_over(s, x) : -1e9f;
                float const high = covered ? mathf::max(sand_height(s, float(x) * 20.f), mathf::max(sand_height(s, float(x) * 20.f + 10.f), sand_height(s, float(x + 1) * 20.f))) : -1e9f;
                for (int32_t y = 0; y < height; ++y) {
                    uint8_t& st = s.tiles[size_t(y * width + x)];
                    if ((st & sys::sand_tile_convertible) == 0)
                        continue;
                    float const top = float(y + 1) * 20.f;
                    bool solid = (st & sys::sand_tile_solid) != 0;
                    if (!solid && low >= top + pack_depth)
                        solid = true;
                    else if (solid && low < top + unpack_depth)
                        solid = false;
                    bool const loose = !solid && high > float(y) * 20.f;
                    uint8_t const want = uint8_t(sys::sand_tile_convertible | (solid ? sys::sand_tile_solid : 0) | (loose ? sys::sand_tile_volatile : 0));
                    if (want == st)
                        continue;
                    if (((want ^ st) & sys::sand_tile_solid) != 0) {
                        tile_at(x, y).set_terrain(solid ? world::terrain::solid : world::terrain::air);
                        changed = true;
                    }
                    st = want;
                }
            }
            if (changed)
                ++w.room_get_geometry_version(room);
        }

        int32_t index_of(sand_t const& s, float x) {
            return rw::custom::int_clamp(int32_t(std::lround((x - s.x0) / s.spacing)), 0, int32_t(s.h.size()) - 1);
        }

        // presses the sand under a chunk down a little, piling it at the sides
        void press(sand_t& s, float x, float rad, float weight, float push) {
            int32_t const c = index_of(s, x);
            int32_t const r = std::max(1, int32_t(rad / s.spacing));
            int32_t const n = int32_t(s.h.size());
            float moved = 0.f;
            for (int32_t i = std::max(0, c - r); i <= std::min(n - 1, c + r); ++i) {
                // the mark stays shallow next to the sand around it
                float const around = 0.5f * (s.h[size_t(std::max(0, c - r - 3))] + s.h[size_t(std::min(n - 1, c + r + 3))]);
                if (s.h[size_t(i)] < around - max_mark)
                    continue;
                float const fall = 1.f - std::fabs(float(i - c)) / float(r + 1);
                float const d = weight * press_rate * fall + 0.25f * push;
                s.h[size_t(i)] -= d;
                moved += d;
            }
            // moving bodies shove more of it ahead of them
            float const ahead = mathf::clamp(0.5f + push, 0.f, 1.f);
            int32_t const left = std::max(0, c - r - 1), right = std::min(n - 1, c + r + 1);
            s.h[size_t(right)] += moved * ahead;
            s.h[size_t(left)] += moved * (1.f - ahead);
        }
    }

    sand_t const* sand_if_built(sys::game_state const& state, dcon::room_id room) noexcept {
        if (!state.has_runtime(room))
            return nullptr;
        auto const& s = state.runtime(room).sand;
        return s && !s->h.empty() ? &*s : nullptr;
    }

    sand_t const* sand(sys::game_state& state, dcon::room_id room) {
        if (sand_t* s = find(state, room); s != nullptr)
            return s->h.empty() ? nullptr : s;
        auto& slot = state.runtime(room).sand.emplace();
        if (!build(state, room, slot)) {
            // remembered as "no sand here"
            slot = sand_t{};
            slot.room = room;
        } else {
            retile(state, room, slot, true);
        }
        return slot.h.empty() ? nullptr : &slot;
    }

    float sand_height(sand_t const& s, float x) noexcept {
        float const f = (x - s.x0) / s.spacing;
        int32_t const i = rw::custom::int_clamp(int32_t(std::floor(f)), 0, int32_t(s.h.size()) - 1);
        int32_t const k = std::min(i + 1, int32_t(s.h.size()) - 1);
        return mathf::lerp(s.h[size_t(i)], s.h[size_t(k)], f - float(i));
    }

    float sand_back(sand_t const& s, float x) noexcept {
        return s.back[size_t(index_of(s, x))];
    }

    bool sandstorm(sys::game_state const& state, dcon::room_id room) noexcept {
        return state.storm && sand_if_built(state, room) != nullptr;
    }

    void collide_chunk(sys::game_state& state, room_view const& room, dcon::body_chunk_id c) {
        sand_t* s = find(state, room.id);
        if (s == nullptr || s->h.empty())
            return;
        auto& w = state.world;
        vec2& pos = w.body_chunk_get_pos(c);
        vec2& vel = w.body_chunk_get_vel(c);
        float const rad = w.body_chunk_get_rad(c);
        if (pos.x < s->left || pos.x > s->right)
            return;
        float const ground = sand_height(*s, pos.x);
        if (pos.y - rad >= ground)
            return;
        // pipes open into the sand: they stay reachable
        rw::ivec2 const t = room_view::get_tile_position(pos);
        for (int32_t dx = -1; dx <= 1; ++dx)
            for (int32_t dy = -1; dy <= 1; ++dy) {
                auto const tile = room.get_tile(t.x + dx, t.y + dy);
                if (tile.t == world::terrain::shortcut_entrance || tile.shortcut != 0)
                    return;
            }
        // stands on the sand: out of it, falling stopped, the slope's grain slowing it
        pos.y = ground + rad;
        if (vel.y < 0.f)
            vel.y = 0.f;
        float const slope = (sand_height(*s, pos.x + 4.f) - sand_height(*s, pos.x - 4.f)) / 8.f;
        vel.x *= 1.f - mathf::min(0.3f, std::fabs(slope) * 0.1f);
        w.body_chunk_set_contact_point(c, rw::ivec2{w.body_chunk_get_contact_point(c).x, -1});
        // and slowly marks it
        press(*s, pos.x, rad, w.body_chunk_get_mass(c), vel.x);
    }

    void update_room(sys::game_state& state, room_view const& room) {
        auto& w = state.world;
        // sand: slumping slopes and the storm's reshaping
        if (sand_t const* cs = sand(state, room.id); cs != nullptr) {
            sand_t& s = *find(state, room.id);
            float const max_step = repose * s.spacing;
            for (size_t i = 0; i + 1 < s.h.size(); ++i) {
                float const d = s.h[i] - s.h[i + 1];
                if (std::fabs(d) > max_step) {
                    float const m = (std::fabs(d) - max_step) * 0.25f * mathf::sign(d);
                    s.h[i] -= m;
                    s.h[i + 1] += m;
                }
            }
            // the per tile layer follows the reshaped surface (staggered over the rooms)
            if ((state.tick + uint32_t(room.id.index())) % retile_period == 0)
                retile(state, room.id, s, false);
            if (sandstorm(state, room.id)) {
                // the wind drifts the dunes along, smoothing them towards its own shape
                s.phase += 0.0004f;
                constexpr float tau = 6.2831853f;
                // (the wind's shape for every column at once: ve::sin)
                static thread_local std::vector<float> wave;
                wave.resize(s.h.size());
                for (size_t i = 0; i < s.h.size(); ++i) {
                    float const x = s.x0 + float(i) * s.spacing;
                    wave[i] = tau * x / dune_length + s.phase;
                }
                ve::transform(wave.data(), wave.size(), [](ve::fp_vector v) { return ve::sin(v); });
                for (size_t i = 0; i < s.h.size(); ++i) {
                    float const target = s.mean + s.amplitude * wave[i];
                    s.h[i] += (target - s.h[i]) * storm_rate;
                }
                // and blows sand through the air
                auto r = rw::random(state.seed, state.tick, uint32_t(room.id.value));
                for (int k = 0; k < 4; ++k) {
                    vec2 const at{r.value() * room.pixel_width(), r.value() * room.pixel_height()};
                    particles::spawn(state, room.id, particles::kind::dust, at, vec2{8.f + 6.f * r.value(), -0.5f + r.value()}, 2.f + 3.f * r.value(), 1.5f,
                        rw::vec3{0.82f, 0.7f, 0.5f}, 0.25f);
                }
            }
            // SandPuffSpawner: the slugcats' chunks on the sand kick up puffs,
            // landing hard or (now and then) walking along it
            auto r = rw::random(state.seed, state.tick, uint32_t(room.id.value) ^ 0x5A4D5u);
            rooms::for_each_creature(state, room.id, creature::type::slugcat, [&](dcon::creature_id c) {
                uint32_t const n = creature::chunk_count(state, c);
                for (uint32_t i = 0; i < n; ++i) {
                    auto const k = creature::chunk(state, c, i);
                    vec2 const pos = w.body_chunk_get_pos(k), last = w.body_chunk_get_last_pos(k);
                    float const rad = w.body_chunk_get_rad(k);
                    if (w.body_chunk_get_contact_point(k).y >= 0 || pos.x < s.left || pos.x > s.right)
                        continue;
                    // (on the sand, not on a tile)
                    if (pos.y - rad > sand_height(s, pos.x) + 2.f)
                        continue;
                    float size = -1.f;
                    if (w.body_chunk_get_last_contact_point(k).y >= 0 && last.y - pos.y > 5.f)
                        size = rw::custom::lerp_map(last.y - pos.y, 5.f, 10.f, 0.5f, 1.f);
                    else if (r.value() < 0.1f && std::fabs(last.x - pos.x) > 3.f)
                        size = 0.25f * r.value();
                    if (size < 0.f)
                        continue;
                    // SandPuff: 40..120 ticks by its size, drifting left (its motion's x and y swapped)
                    float const life_time = mathf::lerp(40.f, 120.f, r.value()) * mathf::lerp(0.5f, 1.5f, size);
                    particles::spawn(state, room.id, particles::kind::sand_puff, pos - vec2{0.f, rad}, vec2{-0.25f, 0.f}, size,
                        life_time / float(state.ticks_per_second), rw::vec3{1.f, 1.f, 1.f}, 1.f);
                }
            });
        }
        // mud pits: bodies in them sink slowly and drag
        w.room_for_each_thing_location(room.id, [&](dcon::thing_location_id l) {
            auto const t = w.thing_location_get_thing(l);
            if (w.placed_thing_get_kind(t) != thing_kind::mud_pit)
                return;
            vec2 const p = w.placed_thing_get_pos(t);
            vec2 const size = w.placed_thing_get_handle(t);
            float const top = p.y + size.y;
            float const bottom = p.y - mathf::max(10.f, w.placed_thing_get_extra(t));
            float const x0 = mathf::min(p.x, p.x + size.x), x1 = mathf::max(p.x, p.x + size.x);
            w.room_for_each_object_location(room.id, [&](dcon::object_location_id ol) {
                auto const o = w.object_location_get_object(ol);
                w.physical_object_for_each_chunk_ownership_as_owner(o, [&](dcon::chunk_ownership_id co) {
                    auto const c = w.chunk_ownership_get_chunk(co);
                    vec2& pos = w.body_chunk_get_pos(c);
                    vec2& vel = w.body_chunk_get_vel(c);
                    vec2 const last = w.body_chunk_get_last_pos(c);
                    if (pos.x < x0 || pos.x > x1 || pos.y > top || pos.y < bottom)
                        return;
                    if (last.y > top && vel.y < -3.f) {
                        // (the splash of this chunk in this pit)
                        auto r = rw::random(state.seed, state.tick, rw::ids(uint32_t(t.value), uint32_t(c.value)));
                        for (int k = 0; k < 6; ++k)
                            particles::spawn(state, room.id, particles::kind::drop, vec2{pos.x, top}, vec2{(r.value() - 0.5f) * 3.f, 1.f + 2.f * r.value()},
                                1.4f, 1.f, rw::vec3{0.32f, 0.22f, 0.13f}, 0.9f);
                    }
                    float const depth = mathf::clamp01((top - pos.y) / 30.f);
                    vel.x *= mathf::lerp(0.95f, 0.8f, depth);
                    vel.y *= mathf::lerp(0.95f, 0.75f, depth);
                    // held up, but not quite enough: a slow sink
                    vel.y += 0.9f * room.gravity * mathf::lerp(0.2f, 0.85f, depth);
                });
            });
        });
    }
}
