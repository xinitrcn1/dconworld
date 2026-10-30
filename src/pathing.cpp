#include <algorithm>
#include <atomic>
#include <string>
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

    namespace {
        //
        // The NPC slugcat's walking (movement::slugcat)
        //

        // farthest a slugcat lets itself fall, in tiles (a long fall stuns)
        constexpr int32_t max_fall = 30;
        // a jump up to a ledge or a pole (measured with Player.cs: three
        // tiles with the ledge grab)
        constexpr int32_t max_rise = 3;

        bool slug_walkable(room_view const& room, std::vector<uint8_t> const& acc, ivec2 t, bool swim) noexcept {
            if (!room.inside(t.x, t.y))
                return false;
            auto const tile = room.get_tile(t);
            if (tile.solid())
                return false;
            // (a pipe's entrance only as a goal: stepping into it with the
            // direction held is going into the pipe)
            if (tile.t == terrain::shortcut_entrance)
                return false;
            // a platform it climbs up through and drops through
            if (standable(room, t) || tile.any_beam() || tile.t == terrain::floor)
                return true;
            if (swim && tile.water != 0)
                return true;
            // a one tile hole in the floor it runs over (AImapper's floor)
            if (standable(room, t - ivec2{1, 0}) && standable(room, t + ivec2{1, 0}))
                return true;
            // the mouth of a pipe (it climbs or pushes into it)
            for (auto const d : rw::custom::four_directions)
                if (room.inside(t.x + d.x, t.y + d.y) && room.get_tile(t + d).t == terrain::shortcut_entrance
                    && room.shortcut_entrance_hole_direction(t + d) == ivec2{-d.x, -d.y})
                    return true;
            return acc[size_t(t.y) * size_t(room.width) + size_t(t.x)] == uint8_t(access::corridor);
        }

        // (outside the room as the physics sees it: its edge tile, so a room
        // open at its edge is open beyond it: no wall to push off there)
        bool open(room_view const& room, ivec2 t) noexcept {
            return !room.get_tile(t).solid();
        }

        bool arc_open(room_view const& room, ivec2 g, ivec2 v, int32_t s, int32_t height) {
            int32_t const dx = std::abs(v.x - g.x);
            // room over its head where it leaves to go up (the higher it goes,
            // the more: a hop onto a pole three up and one over goes up first)
            if (v.y >= g.y)
                for (int32_t k = 1; k <= std::min(v.y - g.y + 1, 3); ++k)
                    if (!open(room, g + ivec2{0, k}))
                        return false;
            // (and it sets off sideways: room beside it first)
            if (v.y > g.y && (!open(room, g + ivec2{s, 0}) || !open(room, g + ivec2{s, 1})))
                return false;
            float const dy = float(v.y - g.y);
            float const b = 4.f * float(height);
            for (int32_t i = 1; i <= dx; ++i) {
                float const f = float(i) / float(dx);
                int32_t const y = i == dx ? v.y : int32_t(std::floor(float(g.y) + (dy + b) * f - b * f * f));
                for (int32_t yy = y; yy <= y + 1; ++yy)
                    if (!open(room, ivec2{g.x + s * i, yy}))
                        return false;
            }
            return true;
        }

        // wall jumps up a shaft fail the most: a last resort
        constexpr uint16_t wall_cost(int32_t height) noexcept { return uint16_t(300 + 30 * height); }

        // a jump at the very end of its reach costs more (a start with
        // some margin is surer)
        constexpr uint32_t edge_of_reach(int32_t d, int32_t reach) noexcept { return d >= reach ? 40u : 0u; }

        // How far the jumps carry (measured with Player.cs, landing on the
        // ledge or grabbing it, less a tile to be sure), in tiles from where
        // they leave the ground by how much higher they land: a running jump
        // from the edge, a charge pounce from a tile back from it
        constexpr int32_t max_fall_jump = 6;
        constexpr int32_t leap_reach(int32_t dy) noexcept { return dy >= 3 ? 4 : dy >= 1 ? 5 : 6; }
        // a jump off a pole onto another one `d` tiles across: how much
        // higher it can be (measured: it grabs the other pole on the way)
        constexpr int32_t max_swing = 8;
        constexpr int32_t swing_rise(int32_t d) noexcept { return d <= 4 ? 1 : d == 5 ? 0 : d == 6 ? -2 : d == 7 ? -4 : -7; }
        // off a pole's side onto the ground `dy` over the tile it holds
        // (in CC_A02 eight tiles over and five down, into a corridor)
        // (measured with the start a player used in CC_B08: up and towards
        // it 2 ticks, towards it 2, then jump)
        constexpr int32_t pole_jump_reach(int32_t dy) noexcept { return dy >= 2 ? 4 : dy == 1 ? 5 : dy >= -1 ? 6 : dy >= -4 ? 7 : dy >= -7 ? 8 : 9; }
        // standing on a pole's tip and jumping (measured, and in HI_A10 onto
        // the ledge five tiles over and two up)
        constexpr int32_t tip_reach(int32_t dy) noexcept { return dy >= 4 ? 1 : dy == 3 ? 4 : dy >= 0 ? 5 : 6; }
        // from a pole's tip up to another pole starting `k` tiles over it
        constexpr int32_t tip_pole_reach(int32_t k) noexcept { return k <= 2 ? 5 : k <= 4 ? 4 : 0; }
        constexpr int32_t pounce_reach(int32_t dy) noexcept { return dy >= 2 ? 4 : dy == 1 ? 7 : dy == 0 ? 9 : dy == -1 ? 10 : 11; }

        // A jump from `g` to the ground at `v` on side `s` clears the
        // terrain: the body (two tiles tall) along a parabola rising
        // `height` tiles over a level jump, down to the landing
        bool arc_clear(room_view const& room, ivec2 g, ivec2 v, int32_t s, int32_t height) {
            if (!standable(room, v) || room.get_tile(v).water != 0)
                return false;
            return arc_open(room, g, v, s, height);
        }

        // Every jump of the room (the moves a plain step or a straight drop
        // doesn't cover), by forward search from the tiles they start on
        void find_jumps(sys::game_state const& state, room_view const& room, std::vector<uint8_t> const& acc, std::vector<jump>& out) {
            out.clear();
            int32_t const w = room.width, h = room.height;
            auto const index = [w](ivec2 t) { return uint32_t(t.y * w + t.x); };
            auto const walkable = [&](ivec2 t) { return slug_walkable(room, acc, t, true); };
            auto const add = [&](ivec2 a, ivec2 b, uint32_t cost, step_kind k, int32_t dir) {
                out.push_back(jump{index(a), index(b), uint16_t(cost), k, int8_t(dir)});
            };
            for (int32_t y = 1; y < h - 1; ++y)
                for (int32_t x = 1; x < w - 1; ++x) {
                    ivec2 const g{x, y};
                    if (!walkable(g) || room.get_tile(g).water != 0)
                        continue;
                    // standing on solid ground or a platform (else at a pole's or a
                    // corridor's top, up on a beam: no run up, no pounce from there)
                    bool const on_beam_top = standable(room, g) && room.get_tile(g - ivec2{0, 1}).horizontal_beam();
                    bool const ground = standable(room, g) && !on_beam_top;
                    bool beam_safe = false;
                    for (int32_t k = 2; k <= 4 && on_beam_top && !beam_safe; ++k)
                        beam_safe = walkable(g - ivec2{0, k}) || !open(room, g - ivec2{0, k});
                    for (int32_t const s : {-1, 1}) {
                        ivec2 const side = g + ivec2{s, 0};
                        // off the edge: the tile beside is open air over
                        // a drop, fall down its column to what's below
                        if (ground && open(room, side) && !walkable(side)) {
                            for (int32_t k = 1; k <= max_fall; ++k) {
                                ivec2 const t = side - ivec2{0, k};
                                if (!open(room, t))
                                    break;
                                if (walkable(t)) {
                                    add(g, t, drop_cost + uint32_t(k), step_kind::drop, s);
                                    break;
                                }
                            }
                        }
                        // up the open column above, onto the ledge on the
                        // other side of it (a wall's top beside the column)
                        for (int32_t k = 1; k <= max_rise; ++k) {
                            ivec2 const c = g + ivec2{0, k};
                            if (!open(room, c) || (k > 1 && walkable(c - ivec2{0, 1})) || !open(room, c + ivec2{0, 1}))
                                break;
                            ivec2 const top = c + ivec2{s, 0};
                            // (from up on a beam only over something to land on: off it the
                            // jump may drop through the beam, over a pit for good)
                            if ((!on_beam_top || beam_safe) && standable(room, top) && room.get_tile(top - ivec2{0, 1}).solid() && !walkable(c))
                                add(g, top, 20u + 10u * uint32_t(k), step_kind::rise, s);
                        }
                        // over a gap: a running jump from the edge (with room
                        // to run up to it), a charge pounce from a tile back
                        // from it (the body crouches over two tiles)
                        if (!ground || !open(room, side) || standable(room, side))
                            continue;
                        // past a landing at `v` going `s`, within `left` tiles: a wall
                        // it stops against, else ground all the way to land on
                        auto const stops = [&](ivec2 v, int32_t sd, int32_t left) {
                            for (int32_t k = 1; k <= left - 1; ++k) {
                                ivec2 const t = v + ivec2{sd * k, 0};
                                if (!room.inside(t.x, t.y) || !open(room, t) || !open(room, t + ivec2{0, 1}))
                                    return room.inside(t.x, t.y);
                                if (!standable(room, t))
                                    return false;
                            }
                            return true;
                        };
                        bool const run_up = standable(room, g - ivec2{s, 0}) && standable(room, g - ivec2{2 * s, 0}) && open(room, g + ivec2{0, 1})
                            && open(room, g - ivec2{s, -1}) && open(room, g - ivec2{2 * s, -1});
                        // (onto the ground, or catching a pole on the way)
                        auto const pole = [&](ivec2 v) { return room.inside(v.x, v.y) && room.get_tile(v).any_beam(); };
                        if (run_up)
                            for (int32_t dy = 3; dy >= -max_fall_jump; --dy)
                                for (int32_t dx = 2; dx <= leap_reach(dy); ++dx) {
                                    ivec2 const v = g + ivec2{s * dx, dy};
                                    if (!(arc_clear(room, g, v, s, 2) || (pole(v) && arc_open(room, g, v, s, 2))))
                                        continue;
                                    // (a running jump carries on too)
                                    if (!pole(v) && !stops(v, s, leap_reach(dy) - dx))
                                        continue;
                                    add(g, v, 30u + 10u * uint32_t(dx) + edge_of_reach(dx, leap_reach(dy)), step_kind::leap, s);
                                }
                        ivec2 const back = g - ivec2{s, 0};
                        if (standable(room, back) && open(room, back + ivec2{0, 1}))
                            for (int32_t dy = 2; dy >= -max_fall_jump; --dy)
                                for (int32_t dx = 3; dx <= pounce_reach(dy); ++dx) {
                                    ivec2 const v = back + ivec2{s * dx, dy};
                                    if (!(arc_clear(room, back, v, s, 1) || (pole(v) && arc_open(room, back, v, s, 1))))
                                        continue;
                                    // (a pounce goes its full length: short of that, onto a
                                    // ledge with nothing past it to stop it, it carries on
                                    // over the edge, out of WARG_B35's open side)
                                    if (!pole(v) && !stops(v, s, pounce_reach(dy) - dx))
                                        continue;
                                    add(back, v, 40u + 10u * uint32_t(dx) + edge_of_reach(dx, pounce_reach(dy)), step_kind::pounce, s);
                                }
                    }
                    // a hop onto a pole close by
                    for (int32_t const s : {-1, 1})
                        for (int32_t d = 1; d <= 3 && ground; ++d)
                            for (int32_t dy = 3; dy >= -3; --dy) {
                                ivec2 const v = g + ivec2{s * d, dy};
                                if (room.inside(v.x, v.y) && room.get_tile(v).vertical_beam() && !room.get_tile(g).vertical_beam() && arc_open(room, g, v, s, 1))
                                    add(g, v, 25u + 10u * uint32_t(d + std::abs(dy)), step_kind::rise, s);
                            }
                    // straight up to a pole, a beam, into a corridor (a
                    // shelter's shaft up to its pipe; from up on a beam too,
                    // over something to land on, though not onto another beam:
                    // WRFA_S01's beam under its shaft, not HI_C11's ladder of
                    // beams it wall jumps up)
                    for (int32_t k = 2; k <= max_rise && (ground || (on_beam_top && beam_safe)); ++k) {
                        ivec2 const c = g + ivec2{0, k};
                        if (!open(room, c) || !open(room, c - ivec2{0, 1}) || walkable(c - ivec2{0, 1}))
                            break;
                        if (!ground && room.get_tile(c).horizontal_beam())
                            break;
                        if (walkable(c)) {
                            add(g, c, 20u + 10u * uint32_t(k), step_kind::rise, 0);
                            break;
                        }
                    }
                }
            // from a vertical pole to another one across: a jump off it,
            // grabbing the other one on the way (up a little close by,
            // further on falling)
            for (int32_t y = 1; y < h - 1; ++y)
                for (int32_t x = 1; x < w - 1; ++x) {
                    ivec2 const u{x, y};
                    if (!room.get_tile(u).vertical_beam() || room.get_tile(u).water != 0)
                        continue;
                    // (to another pole not from the pole's top tile: off its side
                    // there it doesn't carry far enough, the tip jump is the move)
                    bool const top = !room.get_tile(u + ivec2{0, 1}).vertical_beam();
                    for (int32_t const s : {-1, 1})
                        for (int32_t d = 2; d <= max_swing && !top; ++d)
                            for (int32_t dy = swing_rise(d); dy >= -max_fall_jump - 2; --dy) {
                                ivec2 const v = u + ivec2{s * d, dy};
                                if (room.inside(v.x, v.y) && room.get_tile(v).vertical_beam() && arc_open(room, u, v, s, 1))
                                    add(u, v, 30u + 10u * uint32_t(d) + (dy == swing_rise(d) ? 30u : 0u) + (d >= 6 ? 60u : 0u), step_kind::swing, s);
                            }
                    // or onto the ground
                    for (int32_t const s : {-1, 1})
                        for (int32_t dy = 2; dy >= -8; --dy)
                            for (int32_t d = 2; d <= pole_jump_reach(dy); ++d)
                                if (ivec2 const v = u + ivec2{s * d, dy}; arc_clear(room, u, v, s, 1))
                                    add(u, v, 35u + 10u * uint32_t(d) + edge_of_reach(d, pole_jump_reach(dy)), step_kind::swing, s);
                }
            // from the tip of a pole (standing on it): a jump onto the
            // ground around it
            for (int32_t y = 1; y < h - 2; ++y)
                for (int32_t x = 1; x < w - 1; ++x) {
                    ivec2 const u{x, y};
                    ivec2 const g = u + ivec2{0, 1};
                    if (!room.get_tile(u).vertical_beam() || room.get_tile(g).vertical_beam() || !open(room, g) || !open(room, g + ivec2{0, 1})
                        || room.get_tile(u).water != 0)
                        continue;
                    for (int32_t const s : {-1, 1})
                        for (int32_t dy = 4; dy >= -max_fall_jump; --dy)
                            for (int32_t d = 1; d <= tip_reach(dy); ++d)
                                if (ivec2 const v = g + ivec2{s * d, dy}; arc_clear(room, g, v, s, 2))
                                    add(u, v, 30u + 10u * uint32_t(d) + edge_of_reach(d, tip_reach(dy)), step_kind::tip, s);
                    // or into a wall beside it and off the wall back up to a
                    // pole over it (CC_B06)
                    for (int32_t const s : {-1, 1}) {
                        int32_t wall = 0;
                        for (int32_t k = 1; k <= 2 && wall == 0; ++k)
                            if (!open(room, u + ivec2{s * k, 2}) && !open(room, u + ivec2{s * k, 3}))
                                wall = k;
                            else if (!open(room, u + ivec2{s * k, 1}))
                                break;
                        if (wall == 0)
                            continue;
                        for (int32_t back = 0; back <= 1; ++back)
                            for (int32_t k = 3; k <= 6; ++k) {
                                ivec2 const v = u + ivec2{-s * back, k};
                                if (!room.inside(v.x, v.y) || !room.get_tile(v).vertical_beam() || room.get_tile(v - ivec2{0, 1}).vertical_beam())
                                    continue;
                                bool clear = true;
                                for (int32_t yy = u.y + 1; yy < v.y && clear; ++yy)
                                    for (int32_t xx = std::min(v.x, u.x + s * (wall - 1)); xx <= std::max(v.x, u.x + s * (wall - 1)) && clear; ++xx)
                                        clear = open(room, ivec2{xx, yy});
                                if (clear)
                                    add(u, v, 60u + 10u * uint32_t(k), step_kind::bounce, s);
                            }
                    }
                    // or up to a pole over it, grabbing it on the way
                    for (int32_t k = 1; k <= 5; ++k)
                        for (int32_t d = -tip_pole_reach(k); d <= tip_pole_reach(k); ++d) {
                            ivec2 const v = u + ivec2{d, k};
                            if (!room.inside(v.x, v.y) || !room.get_tile(v).vertical_beam() || room.get_tile(v - ivec2{0, 1}).vertical_beam())
                                continue;
                            bool clear = true;
                            for (int32_t xx = std::min(u.x, v.x); xx <= std::max(u.x, v.x) && clear; ++xx)
                                for (int32_t yy = u.y + 1; yy < v.y && clear; ++yy)
                                    clear = open(room, ivec2{xx, yy});
                            if (clear)
                                add(u, v, 30u + 10u * uint32_t(k + std::abs(d)), step_kind::tip, rw::math_sign(d));
                        }
                }
            // up a shaft two or three tiles wide, wall jumping from side to
            // side: from what it stands or hangs on at the bottom (or just
            // under it) to what it can hold on to higher up
            for (int32_t y = 1; y < h - 1; ++y)
                for (int32_t x = 1; x < w - 1; ++x) {
                    ivec2 const u{x, y};
                    if (!walkable(u) || room.get_tile(u).water != 0)
                        continue;
                    // the shaft's span around column `cx` on row `yy` (none: not a shaft)
                    auto const span = [&](int32_t cx, int32_t yy, int32_t& lo, int32_t& hi) {
                        if (!open(room, ivec2{cx, yy}))
                            return false;
                        lo = cx;
                        hi = cx;
                        while (lo - 1 >= 0 && open(room, ivec2{lo - 1, yy}) && cx - lo < 3)
                            --lo;
                        while (hi + 1 < w && open(room, ivec2{hi + 1, yy}) && hi - cx < 3)
                            ++hi;
                        int32_t const width = hi - lo + 1;
                        return width >= 2 && width <= 3 && !open(room, ivec2{lo - 1, yy}) && !open(room, ivec2{hi + 1, yy});
                    };
                    // (each shaft once, whichever of the columns by it leads in:
                    // under a three wide shaft's right side its left column is two
                    // over, as in WRFA_C11)
                    int32_t done[3] = {-1, -1, -1};
                    for (int32_t cx = x - 1; cx <= x + 1; ++cx) {
                        // into the shaft within the first two rows over it
                        int32_t start = -1;
                        for (int32_t yy = y + 1; yy <= y + 2 && start < 0; ++yy) {
                            int32_t lo, hi;
                            if (span(cx, yy, lo, hi))
                                start = yy;
                            else if (!open(room, ivec2{cx, yy}))
                                break;
                        }
                        if (start < 0)
                            continue;
                        int32_t lo0 = 0, hi0 = 0;
                        span(cx, start, lo0, hi0);
                        if (std::find(std::begin(done), std::end(done), lo0) != std::end(done))
                            continue; // once per shaft
                        done[cx - (x - 1)] = lo0;
                        for (int32_t yy = start; yy < h - 1 && yy - y <= 40; ++yy) {
                            int32_t lo, hi;
                            if (!span(cx, yy, lo, hi))
                                break;
                            if (yy - y < 3)
                                continue;
                            // (its first jump: towards the shaft from beside it, which
                            // has to start right over it there; straight up from under it)
                            // (under the shaft starting higher up: towards a side whose wall
                            // comes down level with its head, to push off from; without one
                            // it flies out sideways under the shaft. CC_B08's left gap, as
                            // recorded by a player: its outer wall comes down, the right
                            // gap's walls don't)
                            auto const wall_low = [&](int32_t wx) { return !open(room, ivec2{wx, y + 1}); };
                            int32_t first = x < lo0 ? 1 : x > hi0 ? -1 : x - lo0 <= hi0 - x ? -1 : 1;
                            if (x >= lo0 && x <= hi0 && start > y + 1) {
                                bool const left = wall_low(lo0 - 1), right = wall_low(hi0 + 1);
                                if (!left && !right)
                                    break;
                                if (left != right)
                                    first = left ? -1 : 1;
                            }
                            if ((x < lo0 || x > hi0) && start != y + 1)
                                break;
                            for (int32_t xx = lo; xx <= hi; ++xx)
                                if (walkable(ivec2{xx, yy}))
                                    add(u, ivec2{xx, yy}, wall_cost(yy - y), step_kind::wall, first);
                            // and out over the top onto the ground beside it
                            if (!span(cx, yy + 1, lo, hi) && open(room, ivec2{cx, yy + 1}))
                                for (int32_t xx = lo - 1; xx <= hi + 1; ++xx)
                                    if (ivec2 const t{xx, yy + 1}; standable(room, t))
                                        add(u, t, wall_cost(yy + 1 - y), step_kind::wall, first);
                        }
                    }
                }
            // through a pipe within the room: from in front of one end to in
            // front of the other
            auto const& wd = state.world;
            wd.room_for_each_shortcut_location(room.id, [&](dcon::shortcut_location_id sl) {
                auto const sc = wd.shortcut_location_get_shortcut(sl);
                auto const path = wd.shortcut_get_path(sc);
                if (wd.shortcut_get_type(sc) != world::shortcut_type::normal || path.size() < 2)
                    return;
                ivec2 const a = wd.shortcut_get_start(sc);
                ivec2 const b = path[path.size() - 1];
                if (room.get_tile(a).t != terrain::shortcut_entrance || room.get_tile(b).t != terrain::shortcut_entrance)
                    return;
                ivec2 const fa = a + room.shortcut_entrance_hole_direction(a);
                ivec2 const fb = b + room.shortcut_entrance_hole_direction(b);
                if (walkable(fa) && walkable(fb))
                    add(fa, fb, 20u + 4u * uint32_t(path.size()), step_kind::pipe, 0);
            });
            std::sort(out.begin(), out.end(), [](jump const& a, jump const& b) { return a.to != b.to ? a.to < b.to : a.from < b.from; });
        }

        std::vector<jump> const& jumps_of(sys::game_state& state, room_view const& room, std::vector<uint8_t> const& acc) {
            auto& rt = state.runtime(room.id);
            uint16_t const version = state.world.room_get_geometry_version(room.id);
            if (!rt.jumps_version || *rt.jumps_version != version) {
                rt.jumps_version = version;
                find_jumps(state, room, acc, rt.jumps);
            }
            return rt.jumps;
        }

        // the plain steps between walkable neighbours: up and down only
        // where there is something to climb (a pole, a corridor, water),
        // diagonals between open tiles
        bool slug_step_ok(room_view const& room, std::vector<uint8_t> const& acc, ivec2 u, ivec2 v) {
            ivec2 const d = v - u;
            // up on a horizontal beam only from beside it standing, or up from
            // hanging on it (not sideways off a pole)
            // (a platform tile is gone through up or down, not along)
            auto const in_platform = [&](ivec2 t) { return room.get_tile(t).t == terrain::floor && !standable(room, t) && !room.get_tile(t).any_beam(); };
            if (d.x != 0 && (in_platform(u) || in_platform(v)))
                return false;
            auto const on_beam = [&](ivec2 t) { return standable(room, t) && room.get_tile(t - ivec2{0, 1}).horizontal_beam(); };
            if (on_beam(v) && d.y >= 0 && !standable(room, u) && !(d.x == 0 && room.get_tile(u).horizontal_beam()))
                return false;
            if (d.x != 0 && d.y != 0)
                return open(room, u + ivec2{d.x, 0}) && open(room, u + ivec2{0, d.y})
                    && ((standable(room, u) && standable(room, v)) || (room.get_tile(u).water != 0 && room.get_tile(v).water != 0));
            if (d.y == 0)
                return true;
            // up: onto what holds it there
            auto const climbable = [&](ivec2 t) {
                auto const tile = room.get_tile(t);
                bool mouth = false;
                for (auto const d : rw::custom::four_directions)
                    mouth = mouth || (room.get_tile(t + d).t == terrain::shortcut_entrance && room.shortcut_entrance_hole_direction(t + d) == ivec2{-d.x, -d.y});
                return tile.any_beam() || tile.water != 0 || tile.t == terrain::shortcut_entrance || tile.t == terrain::floor || mouth
                    || acc[size_t(t.y) * size_t(room.width) + size_t(t.x)] == uint8_t(access::corridor);
            };
            return climbable(u) || climbable(v) || (d.y > 0 && standable(room, u) && standable(room, v));
        }

        uint32_t slug_enter(room_view const& room, ivec2 v, uint32_t base) {
            auto const tile = room.get_tile(v);
            uint32_t r = 10;
            if (tile.water != 0)
                r = slugcat.swim;
            else if (!standable(room, v) && tile.any_beam())
                r = slugcat.resistance[uint32_t(access::climb)];
            // up on a horizontal beam: only where the ground won't do
            else if (room.get_tile(v - ivec2{0, 1}).horizontal_beam() && !tile.any_beam())
                r = 40;
            return base * r / 10u;
        }

        void build_slugcat(room_view const& room, std::vector<uint8_t> const& acc, std::vector<jump> const& jumps, ivec2 goal, std::vector<uint16_t>& dist) {
            int32_t const w = room.width, h = room.height;
            dist.assign(size_t(w) * size_t(h), unreachable);
            constexpr uint32_t ring = 4096;
            static thread_local std::vector<std::vector<uint32_t>> buckets(ring);
            static thread_local std::vector<uint8_t> walk;
            static thread_local std::vector<uint16_t> drop_from;
            size_t pending = 0;
            auto const index = [w](ivec2 t) { return uint32_t(t.y * w + t.x); };
            walk.resize(size_t(w) * size_t(h));
            drop_from.resize(size_t(w) * size_t(h));
            for (int32_t y = h - 1; y >= 0; --y)
                for (int32_t x = 0; x < w; ++x) {
                    size_t const i = size_t(y) * size_t(w) + size_t(x);
                    walk[i] = slug_walkable(room, acc, ivec2{x, y}, true) ? 1 : 0;
                    // straight drops: how far up the open column the first
                    // walkable tile is (off a pole, out of a corridor)
                    size_t const up = i + size_t(w);
                    uint16_t k = 0;
                    if (y + 1 < h && open(room, ivec2{x, y + 1}))
                        k = walk[up] ? 1 : (drop_from[up] == 0 || drop_from[up] >= max_fall ? 0 : uint16_t(drop_from[up] + 1));
                    drop_from[i] = k;
                }
            auto const push = [&](uint32_t c, uint32_t i) {
                buckets[c & (ring - 1u)].push_back(i);
                ++pending;
            };
            auto const relax = [&](ivec2 u, uint32_t c) {
                c = std::min<uint32_t>(c, unreachable - 1u);
                if (c < dist[index(u)]) {
                    dist[index(u)] = uint16_t(c);
                    push(c, index(u));
                }
            };
            if (room.inside(goal.x, goal.y) && walk[index(goal)]) {
                relax(goal, 0);
            } else if (room.inside(goal.x, goal.y) && room.get_tile(goal).t == terrain::shortcut_entrance) {
                // a pipe: from its mouth (and the entrance itself, where a
                // slugcat coming out of it stands)
                ivec2 const mouth = goal + room.shortcut_entrance_hole_direction(goal);
                if (room.inside(mouth.x, mouth.y) && walk[index(mouth)])
                    relax(mouth, 0);
            } else {
                for (int32_t dy = -3; dy <= 3; ++dy)
                    for (int32_t dx = -3; dx <= 3; ++dx) {
                        ivec2 const t = goal + ivec2{dx, dy};
                        if (room.inside(t.x, t.y) && walk[index(t)])
                            relax(t, uint32_t(std::max(std::abs(dx), std::abs(dy))) * step_cost);
                    }
            }
            for (uint32_t c = 0; pending > 0; ++c) {
                auto& bucket = buckets[c & (ring - 1u)];
                for (size_t k = 0; k < bucket.size(); ++k) {
                    uint32_t const i = bucket[k];
                    --pending;
                    if (c != dist[i])
                        continue;
                    ivec2 const v{int32_t(i) % w, int32_t(i) / w};
                    for (auto const d : rw::custom::eight_directions) {
                        ivec2 const u = v + d;
                        if (!room.inside(u.x, u.y) || !walk[index(u)] || !slug_step_ok(room, acc, u, v))
                            continue;
                        relax(u, c + slug_enter(room, v, d.x != 0 && d.y != 0 ? diagonal_cost : step_cost));
                    }
                    if (int32_t const k2 = drop_from[i]; k2 >= 2)
                        relax(v + ivec2{0, k2}, c + drop_cost);
                    auto it = std::lower_bound(jumps.begin(), jumps.end(), i, [](jump const& j, uint32_t to) { return j.to < to; });
                    for (; it != jumps.end() && it->to == i; ++it)
                        relax(ivec2{int32_t(it->from) % w, int32_t(it->from) / w}, c + it->cost);
                }
                bucket.clear();
            }
        }
    }

    bool standable(room_view const& room, ivec2 t) noexcept {
        if (!room.inside(t.x, t.y))
            return false;
        auto const tile = room.get_tile(t);
        if (tile.solid())
            return false;
        if (tile.t == terrain::slope)
            return true;
        auto const under = room.get_tile(t.x, t.y - 1);
        auto const below = under.t;
        // (on a horizontal beam it stands too, up on it)
        return below == terrain::solid || below == terrain::slope || below == terrain::floor || (under.horizontal_beam() && !tile.horizontal_beam());
    }

    bool slugcat_walkable(sys::game_state& state, room_view const& room, ivec2 t) {
        return slug_walkable(room, access_map(state, room), t, true);
    }

    std::string debug_jumps(sys::game_state& state, room_view const& room, ivec2 from) {
        auto const& acc = access_map(state, room);
        auto const& jumps = jumps_of(state, room, acc);
        std::string out;
        uint32_t const i = uint32_t(from.y * room.width + from.x);
        for (auto const& j : jumps)
            if (j.from == i)
                out += "  jump kind " + std::to_string(int(j.kind)) + " to " + std::to_string(int32_t(j.to) % room.width) + "," + std::to_string(int32_t(j.to) / room.width)
                    + " cost " + std::to_string(j.cost) + " dir " + std::to_string(j.dir) + "\n";
        return out;
    }

    std::vector<uint8_t> slugcat_reach_from(sys::game_state& state, room_view const& room, ivec2 from) {
        auto const& acc = access_map(state, room);
        auto const& jumps = jumps_of(state, room, acc);
        int32_t const w = room.width;
        std::vector<uint8_t> seen(size_t(room.width) * size_t(room.height), 0);
        std::vector<ivec2> queue;
        auto const visit = [&](ivec2 t) {
            if (!room.inside(t.x, t.y) || seen[size_t(t.y * w + t.x)] || !slug_walkable(room, acc, t, true))
                return;
            seen[size_t(t.y * w + t.x)] = 1;
            queue.push_back(t);
        };
        visit(from);
        if (room.get_tile(from).t == terrain::shortcut_entrance)
            visit(from + room.shortcut_entrance_hole_direction(from));
        for (size_t k = 0; k < queue.size(); ++k) {
            ivec2 const u = queue[k];
            for (auto const d : rw::custom::eight_directions)
                if (slug_walkable(room, acc, u + d, true) && slug_step_ok(room, acc, u, u + d))
                    visit(u + d);
            for (int32_t i = 1; i <= max_fall; ++i) {
                ivec2 const t = u - ivec2{0, i};
                if (!open(room, t))
                    break;
                if (slug_walkable(room, acc, t, true)) {
                    if (i >= 2)
                        visit(t);
                    break;
                }
            }
            uint32_t const i = uint32_t(u.y * w + u.x);
            for (auto const& j : jumps)
                if (j.from == i)
                    visit(ivec2{int32_t(j.to) % w, int32_t(j.to) / w});
        }
        return seen;
    }

    std::optional<step> slugcat_step(sys::game_state& state, field_id f, ivec2 from, std::function<bool(ivec2, ivec2)> const& skip) {
        auto const room = world::get_room_view(state, room_of_field(f));
        auto const& acc = access_map(state, room);
        auto const& jumps = jumps_of(state, room, acc);
        uint16_t const here = distance(state, f, from);
        if (here == unreachable || here == 0)
            return std::nullopt;
        std::optional<step> best;
        uint32_t best_c = 0xFFFFFFFFu;
        auto const consider = [&](ivec2 t, uint32_t cost, step_kind k, int32_t dir) {
            uint16_t const d = distance(state, f, t);
            if (d == unreachable || (skip && k != step_kind::walk && skip(from, t)))
                return;
            uint32_t const c = uint32_t(d) + cost;
            if (c < best_c) {
                best_c = c;
                best = step{t, k, int8_t(dir)};
            }
        };
        if (slug_walkable(room, acc, from, true)) {
            for (auto const d : rw::custom::eight_directions) {
                ivec2 const v = from + d;
                if (slug_walkable(room, acc, v, true) && slug_step_ok(room, acc, from, v))
                    consider(v, slug_enter(room, v, d.x != 0 && d.y != 0 ? diagonal_cost : step_cost), step_kind::walk, d.x);
            }
            // straight down an open column
            for (int32_t k = 1; k <= max_fall; ++k) {
                ivec2 const t = from - ivec2{0, k};
                if (!open(room, t))
                    break;
                if (slug_walkable(room, acc, t, true)) {
                    if (k >= 2)
                        consider(t, drop_cost, step_kind::drop, 0);
                    break;
                }
            }
            uint32_t const i = uint32_t(from.y * room.width + from.x);
            for (auto const& j : jumps)
                if (j.from == i)
                    consider(ivec2{int32_t(j.to) % room.width, int32_t(j.to) / room.width}, j.cost, j.kind, j.dir);
        }
        // off the walkable tiles (in mid air, half way up a wall): towards
        // whichever close tile is nearest the goal
        if (!best)
            for (auto const d : rw::custom::eight_directions)
                consider(from + d, step_cost, step_kind::walk, d.x);
        return best;
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
        if (m.slugcat != 0)
            build_slugcat(view, acc, jumps_of(state, view, acc), goal, pf.distance);
        else
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
