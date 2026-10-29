#include <algorithm>
#include <queue>
#include <vector>

#include "pathing.hpp"

namespace pathing {
    using rw::ivec2;
    using world::room_view;
    using world::terrain;

    namespace {
        uint32_t build_count = 0;

        bool solid(room_view const& room, ivec2 t) noexcept {
            return room.get_tile(t).solid();
        }

        // the four diagonals with both orthogonal neighbours open (OpenDiagonal)
        bool open_diagonal(room_view const& room, ivec2 a, ivec2 d) noexcept {
            return !solid(room, a + ivec2{d.x, 0}) && !solid(room, a + ivec2{0, d.y});
        }

        // reverse Dijkstra from the seeds over the forward connections
        void build(room_view const& room, movement m, ivec2 goal, std::vector<uint16_t>& dist) {
            int32_t const w = room.width, h = room.height;
            dist.assign(size_t(w) * size_t(h), unreachable);
            using node = std::pair<uint32_t, uint32_t>; // cost, tile index
            std::priority_queue<node, std::vector<node>, std::greater<>> open;
            auto const index = [w](ivec2 t) { return uint32_t(t.y * w + t.x); };
            auto const seed = [&](ivec2 t, uint32_t c) {
                if (!room.inside(t.x, t.y) || !accessible(room, t, m) || c >= dist[index(t)])
                    return;
                dist[index(t)] = uint16_t(c);
                open.push({c, index(t)});
            };
            if (room.inside(goal.x, goal.y) && accessible(room, goal, m)) {
                seed(goal, 0);
            } else {
                // approach a goal off the walkable tiles from up to 3 tiles away
                for (int32_t dy = -3; dy <= 3; ++dy)
                    for (int32_t dx = -3; dx <= 3; ++dx)
                        seed(goal + ivec2{dx, dy}, uint32_t(std::max(std::abs(dx), std::abs(dy))) * step_cost);
            }
            auto const relax = [&](ivec2 u, uint32_t c) {
                if (c < dist[index(u)] && c < unreachable) {
                    dist[index(u)] = uint16_t(c);
                    open.push({c, index(u)});
                }
            };
            while (!open.empty()) {
                auto const [c, i] = open.top();
                open.pop();
                if (c != dist[i])
                    continue;
                ivec2 const v{int32_t(i) % w, int32_t(i) / w};
                // predecessors u with a connection u -> v
                for (auto const d : rw::custom::eight_directions) {
                    ivec2 const u = v + d;
                    if (!room.inside(u.x, u.y) || !accessible(room, u, m))
                        continue;
                    if (d.x != 0 && d.y != 0) {
                        if (open_diagonal(room, u, -d))
                            relax(u, c + diagonal_cost);
                    } else {
                        relax(u, c + step_cost);
                    }
                }
                // DropToFloor/DropToClimb: from the first walkable tile above an
                // open column down to v
                for (int32_t k = 1; v.y + k < h; ++k) {
                    ivec2 const u = v + ivec2{0, k};
                    if (solid(room, u))
                        break;
                    if (accessible(room, u, m)) {
                        if (k >= 2)
                            relax(u, c + drop_cost);
                        break;
                    }
                }
            }
        }
    }

    bool accessible(room_view const& room, ivec2 t, movement m) noexcept {
        (void)m;
        auto const tile = room.get_tile(t);
        if (tile.solid())
            return false;
        if (tile.t == terrain::slope || tile.t == terrain::floor || tile.t == terrain::shortcut_entrance || tile.any_beam())
            return true;
        // Floor, Wall, Ceiling and Corridor: next to solid terrain
        for (auto const d : rw::custom::four_directions)
            if (solid(room, t + d))
                return true;
        auto const below = room.get_tile(t + ivec2{0, -1});
        return below.t == terrain::floor || below.t == terrain::slope;
    }

    int32_t terrain_proximity(room_view const& room, ivec2 t, int32_t cap) noexcept {
        for (int32_t r = 0; r < cap; ++r)
            for (int32_t dy = -r; dy <= r; ++dy)
                for (int32_t dx = -r; dx <= r; ++dx)
                    if ((std::abs(dx) == r || std::abs(dy) == r) && solid(room, t + ivec2{dx, dy}))
                        return r;
        return cap;
    }

    field_id field_to(sys::game_state& state, dcon::room_id room, ivec2 goal, movement m) {
        auto& fields = state.path_fields;
        uint16_t const version = state.world.room_get_geometry_version(room);
        field_id oldest = 0;
        for (field_id f = 0; f < fields.size(); ++f) {
            auto& pf = fields[f];
            if (pf.room == room && pf.goal == goal && pf.movement == uint8_t(m) && pf.geometry_version == version) {
                pf.last_used = state.tick;
                return f;
            }
            if (pf.last_used < fields[oldest].last_used)
                oldest = f;
        }
        field_id const f = fields.size() < 32 ? field_id(fields.size()) : oldest;
        if (f == fields.size())
            fields.emplace_back();
        auto& pf = fields[f];
        build(world::get_room_view(state, room), m, goal, pf.distance);
        ++build_count;
        pf.room = room;
        pf.goal = goal;
        pf.movement = uint8_t(m);
        pf.geometry_version = version;
        pf.last_used = state.tick;
        return f;
    }

    uint16_t distance(sys::game_state const& state, field_id f, ivec2 t) noexcept {
        auto const& pf = state.path_fields[f];
        int32_t const width = state.world.room_get_width(pf.room);
        int32_t const height = state.world.room_get_height(pf.room);
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
        auto const room = world::get_room_view(state, state.path_fields[f].room);
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
        return build_count;
    }
}
