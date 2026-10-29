#include <algorithm>
#include <atomic>
#include <vector>

#include "pathing.hpp"

namespace pathing {
    using rw::ivec2;
    using world::room_view;
    using world::terrain;

    namespace {
        std::atomic<uint32_t> build_count = 0;
        // fields kept per room, and how long one is kept unused
        constexpr size_t max_fields = 32;
        constexpr uint32_t keep_ticks = 400;

        field_id make_id(dcon::room_id room, size_t slot) noexcept {
            return field_id(uint32_t(room.index()) << 8 | uint32_t(slot));
        }
        sys::path_field const& field_of(sys::game_state const& state, field_id f) noexcept {
            return state.rooms[f >> 8].path_fields[f & 0xFFu];
        }
        dcon::room_id room_of_field(field_id f) noexcept {
            return dcon::room_id{dcon::room_id::value_base_t(f >> 8)};
        }

        bool solid(room_view const& room, ivec2 t) noexcept {
            return room.get_tile(t).solid();
        }

        // TerrainProximityMapper: a breadth first fill out from the tiles
        // beside solid ones
        void fill_terrain_proximity(room_view const& room, std::vector<uint16_t>& out) {
            int32_t const w = room.width, h = room.height;
            constexpr uint16_t unset = 0xFFFF;
            out.assign(size_t(w) * size_t(h), unset);
            std::vector<ivec2> next;
            for (int32_t x = 0; x < w; ++x)
                for (int32_t y = 0; y < h; ++y) {
                    size_t const i = size_t(y) * size_t(w) + size_t(x);
                    if (solid(room, ivec2{x, y})) {
                        out[i] = 0;
                        continue;
                    }
                    for (auto const d : rw::custom::four_directions)
                        if (solid(room, ivec2{x, y} + d)) {
                            out[i] = 1;
                            next.push_back(ivec2{x, y});
                            break;
                        }
                }
            for (size_t k = 0; k < next.size(); ++k) {
                ivec2 const t = next[k];
                uint16_t const p = out[size_t(t.y) * size_t(w) + size_t(t.x)];
                for (auto const d : rw::custom::four_directions) {
                    ivec2 const n = t + d;
                    if (!room.inside(n.x, n.y) || out[size_t(n.y) * size_t(w) + size_t(n.x)] != unset)
                        continue;
                    out[size_t(n.y) * size_t(w) + size_t(n.x)] = uint16_t(p + 1);
                    next.push_back(n);
                }
            }
        }

        std::vector<uint16_t> const& proximity_map(sys::game_state& state, dcon::room_id room) {
            auto& rt = state.runtime(room);
            uint16_t const version = state.world.room_get_geometry_version(room);
            if (rt.proximity.empty() || rt.proximity_version != version) {
                rt.proximity_version = version;
                fill_terrain_proximity(world::get_room_view(state, room), rt.proximity);
            }
            return rt.proximity;
        }

        // AImap's accessibility of every tile of the room, worked out once per
        // geometry version (every field of the room starts from it)
        std::vector<uint8_t> const& access_map(sys::game_state& state, room_view const& room) {
            auto& rt = state.runtime(room.id);
            uint16_t const version = state.world.room_get_geometry_version(room.id);
            if (rt.access.empty() || rt.access_version != version) {
                rt.access_version = version;
                rt.access.resize(size_t(room.width) * size_t(room.height));
                for (int32_t y = 0; y < room.height; ++y)
                    for (int32_t x = 0; x < room.width; ++x)
                        rt.access[size_t(y) * size_t(room.width) + size_t(x)] = uint8_t(tile_access(room, ivec2{x, y}));
            }
            return rt.access;
        }

        // reverse Dijkstra from the seeds over the forward connections;
        // `proximity` is the room's terrain proximity map for open_air movers.
        // The costs are small integers, so the open list is a ring of buckets
        // (Dial's algorithm): the same distances as a binary heap, faster
        void build(room_view const& room, std::vector<uint8_t> const& acc, movement const& m, ivec2 goal, std::vector<uint16_t>& dist, std::vector<uint16_t> const* proximity) {
            int32_t const w = room.width, h = room.height;
            dist.assign(size_t(w) * size_t(h), unreachable);
            // a step costs at most 50 * 25.5 + 10 * 25.5 (drop at the highest
            // resistance, open_air at its most; mid_air adds at most 25.5):
            // below the ring's size
            constexpr uint32_t ring = 4096;
            static thread_local std::vector<std::vector<uint32_t>> buckets(ring);
            size_t pending = 0;
            auto const push = [&](uint32_t c, uint32_t i) {
                buckets[c & (ring - 1u)].push_back(i);
                ++pending;
            };
            auto const index = [w](ivec2 t) { return uint32_t(t.y * w + t.x); };
            auto const is_solid = [&](ivec2 t) { return acc[index(t)] == uint8_t(access::solid); };
            // every tile's accessibility and resistance for this movement,
            // worked out once rather than per connection
            static thread_local std::vector<uint8_t> walkable, resistance;
            // for the drops: how far up the open column above a tile the first
            // walkable tile is (0: a solid tile or the top of the room first)
            static thread_local std::vector<uint16_t> drop_from;
            bool const drops = !m.allows(access::air);
            walkable.resize(size_t(w) * size_t(h));
            resistance.resize(size_t(w) * size_t(h));
            drop_from.resize(size_t(w) * size_t(h));
            for (int32_t y = h - 1; y >= 0; --y)
                for (int32_t x = 0; x < w; ++x) {
                    size_t const i = size_t(y) * size_t(w) + size_t(x);
                    access const a = access(acc[i]);
                    bool const wet = m.swim && room.get_tile(x, y).water != 0;
                    walkable[i] = a != access::solid && (m.allows(a) || wet);
                    // a swimmer's waterPathingResistance under the surface
                    resistance[i] = wet ? m.swim : m.resistance[uint32_t(a)];
                    if (drops) {
                        size_t const up = i + size_t(w);
                        uint16_t k = 0;
                        if (y + 1 < h && acc[up] != uint8_t(access::solid))
                            k = walkable[up] ? 1 : (drop_from[up] == 0 ? 0 : uint16_t(drop_from[up] + 1));
                        drop_from[i] = k;
                    }
                }
            auto const walkable_at = [&](ivec2 t) { return walkable[index(t)] != 0; };
            auto const seed = [&](ivec2 t, uint32_t c) {
                if (!room.inside(t.x, t.y) || !walkable_at(t) || c >= dist[index(t)])
                    return;
                dist[index(t)] = uint16_t(c);
                push(c, index(t));
            };
            if (room.inside(goal.x, goal.y) && walkable_at(goal)) {
                seed(goal, 0);
            } else {
                // approach a goal off the walkable tiles from up to 3 tiles away
                for (int32_t dy = -3; dy <= 3; ++dy)
                    for (int32_t dx = -3; dx <= 3; ++dx)
                        seed(goal + ivec2{dx, dy}, uint32_t(std::max(std::abs(dx), std::abs(dy))) * step_cost);
            }
            // the resistance of the tile a step ends on (+ CicadaPather.CheckConnectionCost
            // more than 6 tiles from the goal)
            auto const enter = [&](ivec2 v, uint32_t base) {
                uint32_t const r = resistance[index(v)];
                uint32_t cost = base * (r == 0 ? 10u : r) / 10u;
                if (proximity != nullptr && m.open_air != 0 && std::abs(v.x - goal.x) + std::abs(v.y - goal.y) > 6)
                    cost += uint32_t(10 - std::min<int32_t>((*proximity)[index(v)], 10)) * m.open_air;
                // the flying centiwing's TravelPreference
                if (proximity != nullptr && m.mid_air != 0 && (*proximity)[index(v)] >= 2)
                    cost += uint32_t(rw::custom::lerp_map(float((*proximity)[index(v)]), 1.f, 6.f, float(m.mid_air), 0.f));
                return cost;
            };
            auto const relax = [&](ivec2 u, uint32_t c) {
                // long open_air paths saturate rather than turn unreachable
                c = std::min<uint32_t>(c, unreachable - 1u);
                if (c < dist[index(u)]) {
                    dist[index(u)] = uint16_t(c);
                    push(c, index(u));
                }
            };
            for (uint32_t c = 0; pending > 0; ++c) {
                auto& bucket = buckets[c & (ring - 1u)];
                for (size_t k = 0; k < bucket.size(); ++k) {
                    uint32_t const i = bucket[k];
                    --pending;
                    if (c != dist[i])
                        continue;
                    ivec2 const v{int32_t(i) % w, int32_t(i) / w};
                    // predecessors u with a connection u -> v
                    for (auto const d : rw::custom::eight_directions) {
                        ivec2 const u = v + d;
                        if (!room.inside(u.x, u.y) || !walkable_at(u))
                            continue;
                        if (d.x != 0 && d.y != 0) {
                            // OpenDiagonal: both orthogonal neighbours open
                            if (!is_solid(u + ivec2{-d.x, 0}) && !is_solid(u + ivec2{0, -d.y}))
                                relax(u, c + enter(v, diagonal_cost));
                        } else {
                            relax(u, c + enter(v, step_cost));
                        }
                    }
                    // DropToFloor/DropToClimb: from the first walkable tile above an
                    // open column down to v
                    if (drops) {
                        int32_t const k2 = drop_from[i];
                        if (k2 >= 2)
                            relax(v + ivec2{0, k2}, c + drop_cost);
                    }
                }
                bucket.clear();
            }
        }
    }

    access tile_access(room_view const& room, ivec2 t) noexcept {
        auto const at = [&](int32_t dx, int32_t dy) { return room.get_tile(t + ivec2{dx, dy}); };
        auto const sol = [&](int32_t dx, int32_t dy) { return at(dx, dy).t == terrain::solid; };
        auto const tile = at(0, 0);
        if (tile.t == terrain::solid)
            return access::solid;
        if (sol(0, -1) || at(0, -1).t == terrain::floor || (sol(-1, -1) && !sol(-1, 0)) || (sol(1, -1) && !sol(1, 0)))
            return access::floor;
        if (sol(-1, 0) && sol(1, 0))
            return access::corridor;
        if (tile.any_beam())
            return access::climb;
        if ((tile.flags & world::tile_wall_behind) || sol(-1, 0) || sol(1, 0) || (sol(-1, 1) && !sol(0, 1)) || (sol(1, 1) && !sol(0, 1)))
            return access::wall;
        if (sol(0, 1))
            return access::ceiling;
        return access::air;
    }

    bool accessible(room_view const& room, ivec2 t, movement const& m) noexcept {
        auto const a = tile_access(room, t);
        if (a == access::solid)
            return false;
        if (m.allows(a))
            return true;
        // swimmers go anywhere under the surface
        return m.swim && room.get_tile(t).water != 0;
    }

    int32_t terrain_proximity(room_view const& room, ivec2 t, int32_t cap) noexcept {
        for (int32_t r = 0; r < cap; ++r)
            for (int32_t dy = -r; dy <= r; ++dy)
                for (int32_t dx = -r; dx <= r; ++dx)
                    if ((std::abs(dx) == r || std::abs(dy) == r) && solid(room, t + ivec2{dx, dy}))
                        return r;
        return cap;
    }

    bool narrow_space(room_view const& room, ivec2 t) noexcept {
        for (int i = 0; i < 4; ++i)
            if (solid(room, t + rw::custom::eight_directions[i]) && solid(room, t - rw::custom::eight_directions[i]))
                return true;
        return false;
    }

    int32_t floor_altitude(room_view const& room, ivec2 t) noexcept {
        if (solid(room, t))
            return 100000;
        for (int32_t y = t.y - 1; y >= 0; --y) {
            auto const tt = room.get_terrain(t.x, y);
            if (tt == terrain::floor || tt == terrain::solid)
                return t.y - y;
        }
        return 100000;
    }

    int32_t ai_terrain_proximity(sys::game_state& state, dcon::room_id room, ivec2 t) {
        int32_t const width = state.world.room_get_width(room);
        int32_t const height = state.world.room_get_height(room);
        if (t.x < 0 || t.y < 0 || t.x >= width || t.y >= height)
            return -1;
        return proximity_map(state, room)[size_t(t.y) * size_t(width) + size_t(t.x)];
    }

    std::vector<uint16_t> const& ai_terrain_proximity_map(sys::game_state& state, dcon::room_id room) {
        return proximity_map(state, room);
    }

    field_id field_to(sys::game_state& state, dcon::room_id room, ivec2 goal, movement const& m) {
        auto& fields = state.runtime(room).path_fields;
        uint16_t const version = state.world.room_get_geometry_version(room);
        size_t oldest = 0;
        for (size_t f = 0; f < fields.size(); ++f) {
            auto& pf = fields[f];
            if (pf.goal == goal && pf.movement == m && pf.geometry_version == version) {
                pf.last_used = state.tick;
                return make_id(room, f);
            }
            if (pf.last_used < fields[oldest].last_used)
                oldest = f;
        }
        size_t const f = fields.size() < max_fields ? fields.size() : oldest;
        if (f == fields.size())
            fields.emplace_back();
        auto const* proximity = m.open_air != 0 || m.mid_air != 0 ? &proximity_map(state, room) : nullptr;
        auto const view = world::get_room_view(state, room);
        auto const& acc = access_map(state, view);
        auto& pf = fields[f];
        build(view, acc, m, goal, pf.distance, proximity);
        build_count.fetch_add(1, std::memory_order_relaxed);
        pf.goal = goal;
        pf.movement = m;
        pf.geometry_version = version;
        pf.last_used = state.tick;
        return make_id(room, f);
    }

    void trim(sys::game_state& state, dcon::room_id room) {
        auto& fields = state.runtime(room).path_fields;
        uint32_t const now = state.tick;
        std::erase_if(fields, [now](auto const& pf) { return now - pf.last_used > keep_ticks; });
    }

    uint16_t distance(sys::game_state const& state, field_id f, ivec2 t) noexcept {
        auto const& pf = field_of(state, f);
        auto const room = room_of_field(f);
        int32_t const width = state.world.room_get_width(room);
        int32_t const height = state.world.room_get_height(room);
        if (t.x < 0 || t.y < 0 || t.x >= width || t.y >= height)
            return unreachable;
        uint32_t const i = uint32_t(t.y * width + t.x);
        return i < pf.distance.size() ? pf.distance[i] : unreachable;
    }

    std::optional<ivec2> next_step(sys::game_state const& state, field_id f, ivec2 from) noexcept {
        uint16_t const here = distance(state, f, from);
        std::optional<ivec2> best;
        uint16_t best_d = here;
        for (auto const d : rw::custom::eight_directions) {
            uint16_t const dd = distance(state, f, from + d);
            if (dd < best_d) {
                best_d = dd;
                best = from + d;
            }
        }
        if (best)
            return best;
        // a drop: the field is lower somewhere below an open column
        auto const room = world::get_room_view(state, room_of_field(f));
        for (int32_t k = 1; k < room.height; ++k) {
            ivec2 const t = from - ivec2{0, k};
            if (room.get_tile(t).solid())
                break;
            uint16_t const dd = distance(state, f, t);
            if (dd < here && dd != unreachable)
                return t;
        }
        return std::nullopt;
    }

    uint32_t builds() noexcept {
        return build_count.load(std::memory_order_relaxed);
    }
}
