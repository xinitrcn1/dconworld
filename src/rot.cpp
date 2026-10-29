#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <vector>

#include "rot.hpp"
#include "centipede.hpp"
#include "items.hpp"
#include "physics.hpp"
#include "player.hpp"

namespace rot {
    using rw::ivec2;
    using rw::vec2;
    using world::prey_kind;
    using world::room_view;
    using world::rot_prey;
    using world::rot_tube_kind;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        constexpr ivec2 four_directions_and_zero[5] = {{0, 0}, {-1, 0}, {0, -1}, {1, 0}, {0, 1}};
        // Mathf.Sin(x * pi) of ClimbTubeGraphic and friends
        constexpr float pi = mathf::pi;

        //
        // RWCustom.Custom helpers the rot needs
        //
        vec2 rnv(rw::rng& r) { return custom::deg_to_vec(r.value() * 360.f); }
        float range(rw::rng& r, float lo, float hi) { return lo + (hi - lo) * r.value(); }
        float distance_to_line(vec2 v, vec2 l2, vec2 l1) {
            return ((l2.y - l1.y) * v.x - (l2.x - l1.x) * v.y + l2.x * l1.y - l2.y * l1.x)
                / std::sqrt((l2.y - l1.y) * (l2.y - l1.y) + (l2.x - l1.x) * (l2.x - l1.x));
        }
        vec2 closest_point_on_line(vec2 a, vec2 b, vec2 p) {
            vec2 const v = p - a;
            vec2 const d = b - a;
            float const t = (v.x * d.x + v.y * d.y) / (d.x * d.x + d.y * d.y);
            return a + d * t;
        }
        vec2 horizontal_cross_point(vec2 a, vec2 b, float y) {
            if (a.x == b.x)
                return vec2{a.x, y};
            float const k = (a.y - b.y) / (a.x - b.x);
            return vec2{(y - (a.y - a.x * k)) / k, y};
        }
        vec2 vertical_cross_point(vec2 a, vec2 b, float x) {
            if (a.y == b.y)
                return vec2{x, a.y};
            float const k = (a.y - b.y) / (a.x - b.x);
            return vec2{x, a.y - a.x * k + k * x};
        }
        bool point_in_triangle(vec2 pt, vec2 v1, vec2 v2, vec2 v3) {
            bool const a = distance_to_line(pt, v1, v2) <= 0.f;
            bool const b = distance_to_line(pt, v2, v3) <= 0.f;
            bool const c = distance_to_line(pt, v3, v1) <= 0.f;
            return a == b && b == c;
        }
        vec2 bezier(vec2 a, vec2 ca, vec2 b, vec2 cb, float f) {
            vec2 const m = rw::lerp(ca, cb, f);
            ca = rw::lerp(a, ca, f);
            cb = rw::lerp(cb, b, f);
            ca = rw::lerp(ca, m, f);
            cb = rw::lerp(m, cb, f);
            return rw::lerp(ca, cb, f);
        }

        // AImap.getTerrainProximity(pos) < limit: the BFS distance through
        // air to the closest solid tile, -1 outside the room. Rot tubes only
        // ask about small distances, so it is looked up around the tile
        bool terrain_proximity_below(room_view const& room, vec2 pos, int32_t limit) {
            ivec2 const t = room_view::get_tile_position(pos);
            if (!room.inside(t.x, t.y))
                return true;
            for (int32_t dx = -(limit - 1); dx <= limit - 1; ++dx)
                for (int32_t dy = -(limit - 1); dy <= limit - 1; ++dy)
                    if (std::abs(dx) + std::abs(dy) <= limit - 1 && room.inside(t.x + dx, t.y + dy) && room.get_tile(t.x + dx, t.y + dy).solid())
                        return true;
            return false;
        }

        // SharedPhysics.TerrainCollisionData with VerticalCollision and
        // HorizontalCollision (goThroughFloors: only solid tiles)
        struct terrain_collision {
            vec2 pos;
            vec2 last_pos;
            vec2 vel;
            float rad;
            ivec2 contact{0, 0};
        };
        void vertical_collision(room_view const& room, terrain_collision& cd) {
            ivec2 const tp = room_view::get_tile_position(cd.last_pos);
            bool const last_solid = room.get_tile(cd.last_pos).solid();
            if (cd.vel.y > 0.f) {
                int32_t const y = room_view::get_tile_position(vec2{0.f, cd.pos.y + cd.rad + 0.01f}).y;
                int32_t const y2 = room_view::get_tile_position(vec2{0.f, cd.last_pos.y + cd.rad}).y;
                int32_t const x = room_view::get_tile_position(vec2{cd.pos.x - cd.rad + 1.f, 0.f}).x;
                int32_t const x2 = room_view::get_tile_position(vec2{cd.pos.x + cd.rad - 1.f, 0.f}).x;
                for (int32_t i = y2; i <= y; ++i)
                    for (int32_t j = x; j <= x2; ++j)
                        if (room.get_tile(j, i).solid() && !room.get_tile(j, i - 1).solid() && (tp.y < i || last_solid)) {
                            cd.pos.y = float(i) * 20.f - cd.rad;
                            cd.vel.y = 0.f;
                            cd.contact.y = 1;
                            return;
                        }
            } else if (cd.vel.y < 0.f) {
                int32_t const y = room_view::get_tile_position(vec2{0.f, cd.pos.y - cd.rad - 0.01f}).y;
                int32_t const y2 = room_view::get_tile_position(vec2{0.f, cd.last_pos.y - cd.rad}).y;
                int32_t const x = room_view::get_tile_position(vec2{cd.pos.x - cd.rad + 1.f, 0.f}).x;
                int32_t const x2 = room_view::get_tile_position(vec2{cd.pos.x + cd.rad - 1.f, 0.f}).x;
                for (int32_t i = y2; i >= y; --i)
                    for (int32_t j = x; j <= x2; ++j)
                        if (room.get_tile(j, i).solid() && !room.get_tile(j, i + 1).solid() && (tp.y > i || last_solid)) {
                            cd.pos.y = (float(i) + 1.f) * 20.f + cd.rad;
                            cd.vel.y = 0.f;
                            cd.contact.y = -1;
                            return;
                        }
            }
        }
        void horizontal_collision(room_view const& room, terrain_collision& cd) {
            ivec2 const tp = room_view::get_tile_position(cd.last_pos);
            bool const last_solid = room.get_tile(cd.last_pos).solid();
            if (cd.vel.x > 0.f) {
                int32_t const x = room_view::get_tile_position(vec2{cd.pos.x + cd.rad + 0.01f, 0.f}).x;
                int32_t const x2 = room_view::get_tile_position(vec2{cd.last_pos.x + cd.rad, 0.f}).x;
                int32_t const y = room_view::get_tile_position(vec2{0.f, cd.pos.y + cd.rad - 1.f}).y;
                int32_t const y2 = room_view::get_tile_position(vec2{0.f, cd.pos.y - cd.rad + 1.f}).y;
                for (int32_t i = x2; i <= x; ++i)
                    for (int32_t j = y2; j <= y; ++j)
                        if (room.get_tile(i, j).solid() && !room.get_tile(i - 1, j).solid() && (tp.x < i || last_solid)) {
                            cd.pos.x = float(i) * 20.f - cd.rad;
                            cd.vel.x = 0.f;
                            cd.contact.x = 1;
                            return;
                        }
            } else if (cd.vel.x < 0.f) {
                int32_t const x = room_view::get_tile_position(vec2{cd.pos.x - cd.rad - 0.01f, 0.f}).x;
                int32_t const x2 = room_view::get_tile_position(vec2{cd.last_pos.x - cd.rad, 0.f}).x;
                int32_t const y = room_view::get_tile_position(vec2{0.f, cd.pos.y + cd.rad - 1.f}).y;
                int32_t const y2 = room_view::get_tile_position(vec2{0.f, cd.pos.y - cd.rad + 1.f}).y;
                for (int32_t i = x2; i >= x; --i)
                    for (int32_t j = y2; j <= y; ++j)
                        if (room.get_tile(i, j).solid() && !room.get_tile(i + 1, j).solid() && (tp.x > i || last_solid)) {
                            cd.pos.x = (float(i) + 1.f) * 20.f + cd.rad;
                            cd.vel.x = 0.f;
                            cd.contact.x = -1;
                            return;
                        }
            }
        }

        //
        // Rope: a line between two points bending around the solid tile
        // corners it is pulled against
        //
        vec2 a_connect(world::rope_state const& r) { return r.bend_count == 0 ? r.b : r.bends[0].pos; }
        vec2 b_connect(world::rope_state const& r) { return r.bend_count == 0 ? r.a : r.bends[r.bend_count - 1].pos; }

        bool line_overlaps_corner(vec2 l1, vec2 l2, world::rope_corner const& c) {
            int32_t const dx = (c.dir == 0 || c.dir == 3) ? -1 : 1;
            int32_t const dy = (c.dir == 0 || c.dir == 1) ? 1 : -1;
            return (l1.y == l2.y || ((dx >= 0 || horizontal_cross_point(l1, l2, c.pos.y).x >= c.pos.x) && (dx <= 0 || horizontal_cross_point(l1, l2, c.pos.y).x <= c.pos.x)))
                && (l1.x == l2.x || ((dy >= 0 || vertical_cross_point(l1, l2, c.pos.x).y >= c.pos.y) && (dy <= 0 || vertical_cross_point(l1, l2, c.pos.x).y <= c.pos.y)));
        }

        void rope_insert(world::rope_state& r, uint32_t at, world::rope_corner c) {
            if (r.bend_count >= world::rope_state::max_bends)
                return;
            for (uint32_t i = r.bend_count; i > at; --i)
                r.bends[i] = r.bends[i - 1];
            r.bends[at] = c;
            ++r.bend_count;
        }

        void collide_with_corners(world::rope_state& r, room_view const& room, vec2 la, vec2 a, vec2 lb, vec2 b, uint32_t bend, int32_t recursion) {
            if (recursion > 10)
                return;
            ivec2 lo = room_view::get_tile_position(la), hi = lo;
            for (vec2 const p : {a, lb, b}) {
                ivec2 const t = room_view::get_tile_position(p);
                lo = ivec2{std::min(lo.x, t.x), std::min(lo.y, t.y)};
                hi = ivec2{std::max(hi.x, t.x), std::max(hi.y, t.y)};
            }
            lo -= ivec2{1, 1};
            hi += ivec2{1, 1};
            float const th = r.thickness;
            bool found = false;
            world::rope_corner best;
            float f = std::numeric_limits<float>::max();
            auto const consider = [&](uint8_t dir, vec2 pos) {
                world::rope_corner const c{pos, dir};
                if (line_overlaps_corner(a, b, c) && pos != la && pos != a && pos != lb && pos != b
                && (point_in_triangle(pos, a, la, b) || point_in_triangle(pos, a, lb, b) || point_in_triangle(pos, a, la, lb) || point_in_triangle(pos, la, lb, b))
                && std::fabs(distance_to_line(pos, la, lb)) < std::fabs(f)) {
                    best = c;
                    found = true;
                    f = distance_to_line(pos, r.last_a, r.last_b);
                }
            };
            for (int32_t i = lo.x; i <= hi.x; ++i) {
                for (int32_t j = lo.y; j <= hi.y; ++j) {
                    if (!room.get_tile(i, j).solid())
                        continue;
                    vec2 const m = room_view::middle_of_tile(i, j);
                    if (!room.get_tile(i - 1, j).solid()) {
                        if (!room.get_tile(i, j - 1).solid() && !room.get_tile(i - 1, j - 1).solid())
                            consider(3, m + vec2{-10.f - th, -10.f - th});
                        if (!room.get_tile(i, j + 1).solid() && !room.get_tile(i - 1, j + 1).solid())
                            consider(0, m + vec2{-10.f - th, 10.f + th});
                    }
                    if (!room.get_tile(i + 1, j).solid()) {
                        if (!room.get_tile(i, j - 1).solid() && !room.get_tile(i + 1, j - 1).solid())
                            consider(2, m + vec2{10.f + th, -10.f - th});
                        if (!room.get_tile(i, j + 1).solid() && !room.get_tile(i + 1, j + 1).solid())
                            consider(1, m + vec2{10.f + th, 10.f + th});
                    }
                }
            }
            if (!found)
                return;
            rope_insert(r, bend, best);
            vec2 const v = closest_point_on_line(la, lb, best.pos);
            collide_with_corners(r, room, v, best.pos, lb, b, bend + 1, recursion + 1);
            collide_with_corners(r, room, la, a, v, best.pos, bend, recursion + 1);
        }

        void rope_update(world::rope_state& r, room_view const& room, vec2 new_a, vec2 new_b) {
            r.last_a = r.a;
            r.last_b = r.b;
            r.a = new_a;
            r.b = new_b;
            if (r.bend_count == 0) {
                collide_with_corners(r, room, r.last_a, r.a, r.last_b, r.b, 0, 0);
            } else {
                collide_with_corners(r, room, b_connect(r), b_connect(r), r.last_b, r.b, r.bend_count, 0);
                collide_with_corners(r, room, r.last_a, r.a, a_connect(r), a_connect(r), 0, 0);
            }
            if (r.bend_count > 0) {
                // drop the corners the line no longer wraps around
                bool keep[world::rope_state::max_bends];
                for (uint32_t i = 0; i < r.bend_count; ++i) {
                    vec2 const l = i > 0 ? r.bends[i - 1].pos : r.a;
                    vec2 const l2 = i + 1 < r.bend_count ? r.bends[i + 1].pos : r.b;
                    keep[i] = line_overlaps_corner(l, l2, r.bends[i]);
                }
                uint32_t n = 0;
                for (uint32_t i = 0; i < r.bend_count; ++i)
                    if (keep[i])
                        r.bends[n++] = r.bends[i];
                r.bend_count = uint8_t(n);
            }
            if (r.bend_count == 0) {
                r.total_length = rw::distance(r.a, r.b);
            } else {
                r.total_length = rw::distance(r.a, a_connect(r)) + rw::distance(b_connect(r), r.b);
                for (uint32_t k = 1; k < r.bend_count; ++k)
                    r.total_length += rw::distance(r.bends[k - 1].pos, r.bends[k].pos);
            }
        }

        //
        // Placed objects and creatures
        //
        struct place {
            dcon::rot_placed_id id;
            vec2 pos;
            float rad;
        };
        template<typename F>
        void for_each_place(sys::game_state const& state, dcon::room_id room, F&& f) {
            auto const& w = state.world;
            w.room_for_each_rot_placed_location(room, [&](dcon::rot_placed_location_id l) {
                auto const o = w.rot_placed_location_get_placed(l);
                auto const k = w.rot_placed_get_kind(o);
                if (k == world::rot_kind::corruption || k == world::rot_kind::passive_corruption)
                    f(place{o, w.rot_placed_get_pos(o), rw::magnitude(w.rot_placed_get_handle(o))});
            });
        }

        // DaddyCorruption.CorruptionLevel
        float corruption_level(sys::game_state const& state, dcon::room_id room, vec2 p) {
            float best = 0.f;
            for_each_place(state, room, [&](place const& pl) {
                if (!custom::dist_less(p, pl.pos, pl.rad))
                    return;
                float const d = rw::distance(p, pl.pos);
                float const v = mathf::lerp(mathf::inverse_lerp(pl.rad, 0.f, d), mathf::inverse_lerp(0.f, 200.f, pl.rad - d), 0.5f);
                if (v > best)
                    best = v;
            });
            return best;
        }
        float corruption_level(sys::game_state const& state, dcon::room_id room, ivec2 t) {
            return corruption_level(state, room, room_view::middle_of_tile(t));
        }

        // DaddyCorruption.GetCustomRotData: the PassiveCorruption (if any) the
        // tile is most into
        dcon::rot_placed_id custom_rot_data(sys::game_state const& state, dcon::room_id room, ivec2 t) {
            vec2 const p = room_view::middle_of_tile(t);
            dcon::rot_placed_id best;
            float num = 0.f;
            for_each_place(state, room, [&](place const& pl) {
                if (!custom::dist_less(p, pl.pos, pl.rad))
                    return;
                float const d = rw::distance(p, pl.pos);
                float const v = mathf::lerp(mathf::inverse_lerp(pl.rad, 0.f, d), mathf::inverse_lerp(0.f, 200.f, pl.rad - d), 0.5f);
                if (v > num) {
                    best = pl.id;
                    num = v;
                }
            });
            if (best && state.world.rot_placed_get_kind(best) != world::rot_kind::passive_corruption)
                return dcon::rot_placed_id{};
            return best;
        }

        dcon::player_id prey_player(rot_prey p) { return dcon::player_id{dcon::player_id::value_base_t(p.id)}; }
        dcon::centipede_id prey_centipede(rot_prey p) { return dcon::centipede_id{dcon::centipede_id::value_base_t(p.id)}; }

        dcon::physical_object_id prey_body(sys::game_state const& state, rot_prey p) {
            auto const& w = state.world;
            if (p.kind == prey_kind::player && w.player_is_valid(prey_player(p)))
                return player::body(state, prey_player(p));
            if (p.kind == prey_kind::centipede && w.centipede_is_valid(prey_centipede(p)))
                return centipede::body(state, prey_centipede(p));
            return dcon::physical_object_id{};
        }
        dcon::body_chunk_id prey_chunk(sys::game_state const& state, rot_prey p) {
            auto const o = prey_body(state, p);
            return o ? physics::chunk(state, o, p.chunk) : dcon::body_chunk_id{};
        }
        // the creature still exists and is out in `room` (not travelling a pipe, not eaten)
        bool prey_in_room(sys::game_state const& state, rot_prey p, dcon::room_id room) {
            auto const& w = state.world;
            auto const o = prey_body(state, p);
            if (!o || w.physical_object_get_room_from_object_location(o) != room)
                return false;
            if (p.kind == prey_kind::player)
                return !w.player_get_in_shortcut(prey_player(p)) && !w.player_get_eaten(prey_player(p));
            return true;
        }

        // every chunk of every creature in the room (Room.abstractRoom.creatures)
        template<typename F>
        void for_each_creature_chunk(sys::game_state const& state, dcon::room_id room, F&& f) {
            auto const& w = state.world;
            bool stop = false;
            w.for_each_player([&](dcon::player_id p) {
                if (stop)
                    return;
                rot_prey const base{prey_kind::player, 0, uint16_t(p.index())};
                if (!prey_in_room(state, base, room))
                    return;
                for (uint8_t i = 0; i < 2 && !stop; ++i)
                    stop = f(rot_prey{prey_kind::player, i, base.id}, player::body_chunk(state, p, i));
            });
            w.for_each_centipede([&](dcon::centipede_id c) {
                if (stop)
                    return;
                rot_prey const base{prey_kind::centipede, 0, uint16_t(c.index())};
                if (!prey_in_room(state, base, room))
                    return;
                uint32_t const n = centipede::chunk_count(state, c);
                for (uint32_t i = 0; i < n && !stop; ++i)
                    stop = f(rot_prey{prey_kind::centipede, uint8_t(i), base.id}, centipede::chunk(state, c, i));
            });
        }

        //
        // The system and its bulb grid
        //
        struct grid {
            ivec2 bl;
            ivec2 tr;
            int32_t width() const { return tr.x - bl.x + 1; }
            bool inside(ivec2 t) const { return t.x >= bl.x && t.y >= bl.y && t.x <= tr.x && t.y <= tr.y; }
            uint32_t index(ivec2 t) const { return uint32_t((t.y - bl.y) * width() + (t.x - bl.x)); }
        };
        grid grid_of(sys::game_state const& state, dcon::rot_system_id s) {
            return grid{state.world.rot_system_get_bottom_left(s), state.world.rot_system_get_top_right(s)};
        }

        // DaddyCorruption.Dir
        vec2 dir_at(sys::game_state const& state, dcon::rot_system_id s, ivec2 t) {
            grid const g = grid_of(state, s);
            if (!g.inside(t))
                return vec2{0.f, 0.f};
            return state.world.rot_system_get_directions(s)[g.index(t)];
        }
        // DaddyCorruption.Occupied
        bool occupied(sys::game_state const& state, dcon::rot_system_id s, ivec2 t) {
            return rw::magnitude(dir_at(state, s, t)) > 0.f;
        }
        world::rot_cell cell_at(sys::game_state const& state, dcon::rot_system_id s, ivec2 t) {
            grid const g = grid_of(state, s);
            if (!g.inside(t))
                return world::rot_cell{};
            return state.world.rot_system_get_cells(s)[g.index(t)];
        }
        dcon::rot_bulb_id bulb_at(uint32_t i) { return dcon::rot_bulb_id{dcon::rot_bulb_id::value_base_t(i)}; }

        dcon::room_id room_of(sys::game_state const& state, dcon::rot_system_id s) {
            return state.world.rot_system_get_room_from_rot_system_location(s);
        }
        dcon::rot_system_id system_of_bulb(sys::game_state const& state, dcon::rot_bulb_id b) {
            return state.world.rot_bulb_get_system_from_rot_bulb_membership(b);
        }
        bool passive(sys::game_state const& state, dcon::rot_bulb_id b) {
            auto const c = state.world.rot_bulb_get_custom(b);
            return c && !state.world.rot_placed_get_custom(c).hostile;
        }

        //
        // CorruptionTube
        //
        float gravity_affected(sys::game_state const& state, dcon::rot_tube_id t, int32_t seg) {
            switch (state.world.rot_tube_get_kind(t)) {
            case rot_tube_kind::leg:
                return mathf::inverse_lerp(2.f, 5.f, float(seg));
            case rot_tube_kind::tendril:
                return 1.f;
            case rot_tube_kind::climb: {
                int32_t const n = int32_t(state.world.rot_tube_get_segments(t).size());
                return mathf::min(mathf::inverse_lerp(2.f, 5.f, float(seg)), mathf::inverse_lerp(float(n - 3), float(n - 6), float(seg)));
            }
            }
            return 1.f;
        }

        dcon::rot_tube_id create_tube(sys::game_state& state, dcon::rot_system_id s, rot_tube_kind kind, float length, vec2 a, vec2 b, bool stuck_a, bool stuck_b, rw::rng& r) {
            auto& w = state.world;
            auto const t = w.create_rot_tube();
            w.try_create_rot_tube_membership(t, s);
            w.rot_tube_set_kind(t, kind);
            w.rot_tube_set_con_rad(t, 10.f);
            w.rot_tube_set_push_apart(t, 0.15f);
            if (stuck_a)
                w.rot_tube_set_stuck_a(t, a);
            if (stuck_b)
                w.rot_tube_set_stuck_b(t, b);
            int32_t const n = int32_t(mathf::clamp(length / 10.f, 1.f, 200.f));
            auto segs = w.rot_tube_get_segments(t);
            for (int32_t i = 0; i < n; ++i) {
                float const f = n > 1 ? float(i) / float(n - 1) : 0.f;
                world::rot_segment sg;
                sg.pos = rw::lerp(a, b, f) + rnv(r) * r.value();
                sg.last_pos = sg.pos;
                sg.vel = rnv(r) * r.value();
                segs.push_back(sg);
            }
            return t;
        }

        // LegGraphic / TendrilGraphic bumps (`deadness` < 0: a tendril, no eyes)
        void add_leg_bumps(sys::game_state& state, dcon::rot_tube_id t, int32_t parts, float deadness, rw::rng& r) {
            auto bumps = state.world.rot_tube_get_bumps(t);
            int32_t const n = parts / 2 + r.range(5, 8);
            for (int32_t i = 0; i < n; ++i) {
                float const num = i == 0 ? 1.f : mathf::pow(r.value(), 0.5f);
                world::rot_bump b;
                b.pos.x = mathf::lerp(-1.f, 1.f, r.value()) * 3.f * num;
                b.pos.y = mathf::lerp(mathf::inverse_lerp(0.f, float(parts), float(parts - 20)), 1.f, num);
                float const s1 = r.value();
                b.size = mathf::lerp(s1, num, r.value());
                if (deadness >= 0.f && r.value() * (1.f + deadness) < mathf::lerp(0.f, 0.6f, num))
                    b.eye_size = mathf::lerp(0.2f, 0.8f, mathf::pow(r.value(), mathf::lerp(1.5f, 0.5f, num)));
                bumps.push_back(b);
            }
        }
        void resize_graphic(sys::game_state& state, dcon::rot_tube_id t, int32_t parts) {
            auto g = state.world.rot_tube_get_graphic(t);
            g.resize(uint32_t(parts));
        }

        // DaddyCorruption.LittleLeg
        dcon::rot_tube_id create_leg(sys::game_state& state, dcon::rot_system_id s, dcon::rot_bulb_id bulb, vec2 spawn, vec2 tip, rw::rng& r) {
            float const len = rw::distance(spawn, tip);
            auto const t = create_tube(state, s, rot_tube_kind::leg, len, spawn, tip, true, false, r);
            auto& w = state.world;
            w.rot_tube_set_bulb(t, bulb);
            int32_t const parts = custom::int_clamp(int32_t(len / 10.f), 1, 200);
            resize_graphic(state, t, parts);
            w.rot_tube_set_deadness(t, r.value());
            add_leg_bumps(state, t, parts, w.rot_tube_get_deadness(t), r);
            w.rot_tube_set_mount_a(t, rw::normalized(tip - spawn));
            return t;
        }
        // DaddyCorruption.Tendril
        dcon::rot_tube_id create_tendril(sys::game_state& state, dcon::rot_system_id s, dcon::rot_bulb_id bulb, vec2 spawn, vec2 tip, rw::rng& r) {
            float const len = rw::distance(spawn, tip);
            auto const t = create_tube(state, s, rot_tube_kind::tendril, len, spawn, tip, true, false, r);
            state.world.rot_tube_set_bulb(t, bulb);
            int32_t const parts = custom::int_clamp(int32_t(len / 10.f), 1, 200);
            resize_graphic(state, t, parts);
            add_leg_bumps(state, t, parts, -1.f, r);
            return t;
        }
        // DaddyCorruption.ClimbableCorruptionTube
        dcon::rot_tube_id create_climb_tube(sys::game_state& state, dcon::rot_system_id s, vec2 pos, vec2 handle, rw::rng& r) {
            auto& w = state.world;
            float const span = rw::magnitude(handle) * 1.1f + 50.f;
            auto const t = create_tube(state, s, rot_tube_kind::climb, span / 3.f, pos, pos + handle, true, true, r);
            int32_t const parts = custom::int_clamp(int32_t(span / 10.f), 1, 200);
            resize_graphic(state, t, parts);
            auto bumps = w.rot_tube_get_bumps(t);
            int32_t const nb = std::max(0, parts + r.range(-5, 6));
            for (int32_t i = 0; i < nb; ++i) {
                world::rot_bump b;
                b.pos = vec2{mathf::lerp(-1.f, 1.f, r.value()) * 2.f, r.value()};
                b.size = mathf::pow(r.value(), 1.4f);
                b.eye_size = r.value() < 0.2f ? mathf::lerp(0.2f, 0.8f, mathf::pow(r.value(), 0.5f)) : 0.f;
                bumps.push_back(b);
            }
            w.rot_tube_set_mount_a(t, dir_at(state, s, room_view::get_tile_position(pos)));
            w.rot_tube_set_mount_b(t, dir_at(state, s, room_view::get_tile_position(pos + handle)));
            auto const segs = w.rot_tube_get_segments(t);
            auto ropes = w.rot_tube_get_ropes(t);
            for (uint32_t i = 0; i + 1 < segs.size(); ++i) {
                world::rope_state rp;
                rp.a = rp.last_a = segs[i].pos;
                rp.b = rp.last_b = segs[i + 1].pos;
                rp.total_length = rw::distance(rp.a, rp.b);
                rp.thickness = 2.f;
                ropes.push_back(rp);
            }
            w.rot_tube_set_con_rad(t, 30.f);
            w.rot_tube_set_push_apart(t, 0.05f);
            return t;
        }

        // ClimbableCorruptionTube.OnTubePos: a bezier through the segments
        vec2 climb_on_tube_pos(sys::game_state const& state, dcon::rot_tube_id t, float ps) {
            auto const segs = state.world.rot_tube_get_segments(t);
            int32_t const n = int32_t(segs.size());
            int32_t const a = custom::int_clamp(int32_t(std::floor(ps * float(n - 1))), 0, n - 1);
            int32_t const b = custom::int_clamp(a + 1, 0, n - 1);
            float const f = mathf::inverse_lerp(float(a), float(b), ps * float(n - 1));
            vec2 const pa = segs[uint32_t(a)].pos;
            vec2 const pb = segs[uint32_t(b)].pos;
            float const d = rw::distance(pa, pb) * 0.25f;
            vec2 const ca = pa - rw::normalized(segs[uint32_t(custom::int_clamp(a - 1, 0, n - 1))].pos - pa) * d;
            vec2 const cb = pb - rw::normalized(segs[uint32_t(custom::int_clamp(b + 1, 0, n - 1))].pos - pb) * d;
            return bezier(pa, ca, pb, cb, f);
        }

        // TubeGraphic.Update / ClimbTubeGraphic.Update
        void graphic_update(sys::game_state& state, dcon::rot_tube_id t) {
            auto& w = state.world;
            auto g = w.rot_tube_get_graphic(t);
            auto const segs = w.rot_tube_get_segments(t);
            uint32_t const n = g.size();
            bool const climb = w.rot_tube_get_kind(t) == rot_tube_kind::climb;
            for (uint32_t i = 0; i < n; ++i) {
                auto gs = g[i];
                gs.last_pos = gs.pos;
                if (climb)
                    gs.pos = climb_on_tube_pos(state, t, n > 1 ? float(i) / float(n - 1) : 0.f);
                else
                    gs.pos = segs[std::min(i, segs.size() - 1)].pos;
                g[i] = gs;
            }
        }

        void connect_to_walls(sys::game_state& state, dcon::rot_tube_id t) {
            auto& w = state.world;
            auto segs = w.rot_tube_get_segments(t);
            if (auto const a = w.rot_tube_get_stuck_a(t); a.has_value()) {
                auto s = segs[0];
                s.pos = *a;
                s.vel = vec2{0.f, 0.f};
                segs[0] = s;
            }
            if (auto const b = w.rot_tube_get_stuck_b(t); b.has_value()) {
                auto s = segs[segs.size() - 1];
                s.pos = *b;
                s.vel = vec2{0.f, 0.f};
                segs[segs.size() - 1] = s;
            }
        }

        // CorruptionTube.Connect
        void connect(sys::game_state& state, dcon::rot_tube_id t, uint32_t ia, uint32_t ib) {
            auto& w = state.world;
            auto segs = w.rot_tube_get_segments(t);
            float const con_rad = w.rot_tube_get_con_rad(t);
            auto a = segs[ia];
            auto b = segs[ib];
            vec2 const n = rw::normalized(a.pos - b.pos);
            float const d = rw::distance(a.pos, b.pos);
            vec2 const push = n * ((con_rad - d) * 0.5f * mathf::inverse_lerp(0.f, con_rad, d));
            a.pos += push;
            a.vel += push;
            b.pos -= push;
            b.vel -= push;
            segs[ia] = a;
            segs[ib] = b;
        }

        // CorruptionTube.Update (the zero gravity wall attraction isn't ported)
        void tube_base_update(sys::game_state& state, room_view const& room, dcon::rot_tube_id t) {
            auto& w = state.world;
            auto segs = w.rot_tube_get_segments(t);
            uint32_t const n = segs.size();
            float const push_apart = w.rot_tube_get_push_apart(t);
            for (uint32_t i = 2; i < n; ++i) {
                vec2 const a = custom::dir_vec(segs[i - 2].pos, segs[i].pos);
                auto s0 = segs[i - 2];
                s0.vel -= a * push_apart;
                segs[i - 2] = s0;
                auto s2 = segs[i];
                s2.vel += a * push_apart;
                segs[i] = s2;
            }
            for (uint32_t j = 0; j < n; ++j) {
                auto s = segs[j];
                s.vel.y -= 0.9f * room.gravity * gravity_affected(state, t, int32_t(j));
                s.last_pos = s.pos;
                s.pos += s.vel;
                s.vel *= 0.999f;
                if (j > 2 && terrain_proximity_below(room, s.pos, 3)) {
                    terrain_collision cd{s.pos, s.last_pos, s.vel, 2.f};
                    vertical_collision(room, cd);
                    horizontal_collision(room, cd);
                    s.pos = cd.pos;
                    s.vel = cd.vel;
                    if (cd.contact.x != 0)
                        s.vel.y *= 0.6f;
                    if (cd.contact.y != 0)
                        s.vel.x *= 0.6f;
                }
                segs[j] = s;
            }
            connect_to_walls(state, t);
            for (uint32_t m = n - 1; m > 0; --m)
                connect(state, t, m, m - 1);
            connect_to_walls(state, t);
            for (uint32_t m = 1; m < n; ++m)
                connect(state, t, m, m - 1);
            connect_to_walls(state, t);
            graphic_update(state, t);
        }

        void feel(sys::game_state& state, dcon::rot_bulb_id b, float intensity, vec2 at);

        // LittleLeg.Update
        void leg_update(sys::game_state& state, room_view const& room, dcon::rot_tube_id t, rw::rng& r) {
            auto& w = state.world;
            tube_base_update(state, room, t);
            auto segs = w.rot_tube_get_segments(t);
            uint32_t const n = segs.size();
            auto const bulb = w.rot_tube_get_bulb(t);
            vec2 const mounted = w.rot_tube_get_mount_a(t);
            rot_prey grab = w.rot_tube_get_grab_chunk(t);
            if (grab.valid() && !prey_in_room(state, grab, room.id))
                grab = rot_prey{};
            auto const gc = grab.valid() ? prey_chunk(state, grab) : dcon::body_chunk_id{};
            auto const reach = w.rot_bulb_get_leg_reach_pos(bulb);
            for (uint32_t i = 0; i < n; ++i) {
                float const d = n > 1 ? float(i) / float(n - 1) : 0.f;
                auto s = segs[i];
                s.vel += mounted * mathf::inverse_lerp(5.f, 1.f, float(i));
                if (gc)
                    s.vel += custom::dir_vec(w.body_chunk_get_pos(gc), s.pos) * 0.2f * r.value();
                else if (reach.has_value())
                    s.vel += custom::dir_vec(s.pos, *reach) * 0.2f * r.value();
                else if (w.rot_tube_get_move_counter(t) < 0)
                    s.vel += rnv(r) * 2.f * r.value() * d;
                segs[i] = s;
            }
            if (gc) {
                if (!w.rot_bulb_get_has_heard_sound(bulb)) {
                    w.rot_bulb_set_light(bulb, mathf::max(r.value(), w.rot_bulb_get_light(bulb)));
                    w.rot_bulb_set_next_look_dir(bulb, custom::dir_vec(w.rot_bulb_get_pos(bulb), w.body_chunk_get_pos(gc)));
                }
                vec2& cpos = w.body_chunk_get_pos(gc);
                vec2& cvel = w.body_chunk_get_vel(gc);
                float const crad = w.body_chunk_get_rad(gc);
                float const mass = w.body_chunk_get_mass(gc);
                auto tip = segs[n - 1];
                vec2 const a = custom::dir_vec(cpos, tip.pos);
                float const num = rw::distance(cpos, tip.pos);
                float const num2 = mass / (0.1f + mass);
                cpos -= a * (crad - num) * (1.f - num2);
                cvel -= a * (crad - num) * (1.f - num2);
                tip.pos += a * (crad - num) * num2;
                tip.vel += a * (crad - num) * num2;
                segs[n - 1] = tip;
                float const pull = w.rot_tube_get_pull_in_prey(t);
                cvel += custom::dir_vec(cpos, w.rot_bulb_get_stuck_pos(bulb)) * mathf::inverse_lerp(0.5f, 1.f, pull) * 3.f / mass;
                if (!custom::dist_less(cpos, *w.rot_tube_get_stuck_a(t), float(n) * 10.f + crad + 50.f))
                    grab = rot_prey{};
                w.rot_tube_set_pull_in_prey(t, mathf::min(1.f, pull + 1.f / 90.f));
            } else {
                grab = rot_prey{};
                w.rot_tube_set_pull_in_prey(t, mathf::max(0.f, w.rot_tube_get_pull_in_prey(t) - 0.025f));
                vec2 const tip = segs[n - 1].pos;
                for_each_creature_chunk(state, room.id, [&](rot_prey p, dcon::body_chunk_id c) {
                    if (!custom::dist_less(w.body_chunk_get_pos(c), tip, w.body_chunk_get_rad(c)))
                        return false;
                    grab = p;
                    feel(state, bulb, 1.f, w.body_chunk_get_pos(c));
                    return true;
                });
                int32_t mc = w.rot_tube_get_move_counter(t) - 1;
                if (mc < 0 && r.value() < 0.025f)
                    mc = r.range(80, 300);
                w.rot_tube_set_move_counter(t, mc);
            }
            w.rot_tube_set_grab_chunk(t, grab);
        }

        // Tendril.Update
        void tendril_update(sys::game_state& state, room_view const& room, dcon::rot_tube_id t, rw::rng& r) {
            tube_base_update(state, room, t);
            if (r.value() < 0.0025f) {
                auto segs = state.world.rot_tube_get_segments(t);
                uint32_t const i = uint32_t(r.range(0, int32_t(segs.size())));
                auto s = segs[i];
                s.vel += rnv(r) * r.value();
                segs[i] = s;
            }
        }

        // ClimbableCorruptionTube.Update
        void climb_update(sys::game_state& state, room_view const& room, dcon::rot_tube_id t, rw::rng& r) {
            auto& w = state.world;
            tube_base_update(state, room, t);
            auto segs = w.rot_tube_get_segments(t);
            int32_t const n = int32_t(segs.size());
            float const g = room.gravity;
            vec2 const ma = w.rot_tube_get_mount_a(t);
            vec2 const mb = w.rot_tube_get_mount_b(t);
            for (int32_t i = 0; i < n; ++i) {
                auto s = segs[uint32_t(i)];
                s.vel += ma * mathf::inverse_lerp(3.f, 1.f, float(i)) * mathf::lerp(0.5f, 0.1f, g);
                s.vel += mb * mathf::inverse_lerp(float(n - 4), float(n - 2), float(i)) * mathf::lerp(0.5f, 0.1f, g);
                s.vel += rnv(r) * 0.15f * r.value() * std::sin(float(i) / float(std::max(n - 1, 1)) * pi) * (1.f - g);
                segs[uint32_t(i)] = s;
            }
            auto ropes = w.rot_tube_get_ropes(t);
            float const con_rad = w.rot_tube_get_con_rad(t);
            for (uint32_t j = 0; j < ropes.size(); ++j) {
                auto rp = ropes[j];
                if (rp.bend_count > 3)
                    rp.bend_count = 0;
                auto s0 = segs[j];
                auto s1 = segs[j + 1];
                rope_update(rp, room, s0.pos, s1.pos);
                if (rp.total_length > con_rad) {
                    float const over = (rp.total_length - con_rad) * 0.5f;
                    vec2 a = custom::dir_vec(s0.pos, a_connect(rp));
                    s0.pos += a * over;
                    s0.vel += a * over;
                    a = custom::dir_vec(s1.pos, b_connect(rp));
                    s1.pos += a * over;
                    s1.vel += a * over;
                }
                segs[j] = s0;
                segs[j + 1] = s1;
                ropes[j] = rp;
            }
        }

        //
        // DaddyCorruption.Bulb
        //

        // Bulb.FeltSomething
        void feel(sys::game_state& state, dcon::rot_bulb_id b, float intensity, vec2 at) {
            auto& w = state.world;
            if (w.rot_bulb_get_felt_something_delay(b) > 0 || intensity <= 0.f || passive(state, b))
                return;
            auto& r = w.rot_system_get_random(system_of_bulb(state, b));
            w.rot_bulb_set_felt_something_intensity(b, intensity);
            w.rot_bulb_set_felt_something_at(b, at);
            w.rot_bulb_set_look_dir(b, custom::dir_vec(w.rot_bulb_get_stuck_pos(b), at));
            w.rot_bulb_set_felt_something_delay(b, int16_t(r.range(20, 40)));
            if (!w.rot_bulb_get_leg_reach_pos(b).has_value())
                w.rot_bulb_set_leg_reach_pos(b, at);
            if (r.value() < intensity)
                w.rot_bulb_set_has_heard_sound(b, true);
        }

        // Bulb.HeardNoise (DaddyBubbles aren't ported)
        void heard_noise(sys::game_state& state, dcon::rot_bulb_id b, vec2 at) {
            auto& w = state.world;
            if (w.rot_system_get_gw_mode(system_of_bulb(state, b)) || passive(state, b))
                return;
            w.rot_bulb_set_next_look_dir(b, custom::dir_vec(w.rot_bulb_get_stuck_pos(b), at));
            w.rot_bulb_set_has_heard_sound(b, true);
        }

        // Bulb.TransmitFeel: the bulbs around feel it too, a little less
        void transmit_feel(sys::game_state& state, dcon::rot_system_id s, dcon::rot_bulb_id b) {
            auto& w = state.world;
            ivec2 const tile = w.rot_bulb_get_tile(b);
            vec2 const stuck = w.rot_bulb_get_stuck_pos(b);
            float const rad = w.rot_bulb_get_rad(b);
            float const intensity = w.rot_bulb_get_felt_something_intensity(b) - 0.075f;
            vec2 const pos = w.rot_bulb_get_pos(b);
            for (auto const d : four_directions_and_zero) {
                auto const cell = cell_at(state, s, tile + d);
                for (uint32_t i = 0; i < cell.count; ++i) {
                    auto const o = bulb_at(cell.first + i);
                    if (custom::dist_less(stuck, w.rot_bulb_get_stuck_pos(o), 20.f + rad + w.rot_bulb_get_rad(o)))
                        feel(state, o, intensity, pos);
                }
            }
        }

        dcon::rot_meal_id meal_of(sys::game_state const& state, dcon::rot_system_id s, rot_prey p) {
            dcon::rot_meal_id found;
            state.world.rot_system_for_each_rot_meal_membership(s, [&](dcon::rot_meal_membership_id m) {
                auto const meal = state.world.rot_meal_membership_get_meal(m);
                if (state.world.rot_meal_get_prey(meal).same_creature(p))
                    found = meal;
            });
            return found;
        }

        // DaddyCorruption.BulbNibbleAtChunk: every nibble brings the creature
        // closer to being eaten
        void nibble(sys::game_state& state, dcon::rot_system_id s, dcon::rot_bulb_id b, rot_prey p) {
            auto& w = state.world;
            vec2 const goal = w.rot_bulb_get_eye_stalk_pos(b);
            float const rad = w.rot_bulb_get_rad(b);
            if (auto const m = meal_of(state, s, p); m) {
                // EatenCreature.BulbInteraction
                w.rot_meal_set_wait(m, w.rot_meal_get_wait(m) + rad);
                if (rad > w.rot_meal_get_goal_rad(m)) {
                    w.rot_meal_set_goal_pos(m, goal);
                    w.rot_meal_set_goal_rad(m, rad);
                }
                w.rot_meal_set_disable_counter(m, 0);
                return;
            }
            if (!prey_in_room(state, p, room_of(state, s)))
                return;
            auto const m = w.create_rot_meal();
            w.try_create_rot_meal_membership(m, s);
            w.rot_meal_set_prey(m, rot_prey{p.kind, 0, p.id});
            w.rot_meal_set_goal_pos(m, goal);
            w.rot_meal_set_goal_rad(m, rad);
            w.rot_meal_set_progression(m, 0.f);
            w.rot_meal_set_wait(m, 0.f);
            w.rot_meal_set_disable_counter(m, 0);
        }

        // Bulb.Update
        void bulb_update(sys::game_state& state, dcon::rot_system_id s, dcon::rot_bulb_id b, rw::rng& r) {
            auto& w = state.world;
            auto const room = room_of(state, s);
            auto const custom_place = w.rot_bulb_get_custom(b);
            world::rot_custom_data const cd = custom_place ? w.rot_placed_get_custom(custom_place) : world::rot_custom_data{};
            bool const is_passive = custom_place && !cd.hostile;
            vec2& pos = w.rot_bulb_get_pos(b);
            vec2& vel = w.rot_bulb_get_vel(b);
            vec2 const stuck = w.rot_bulb_get_stuck_pos(b);
            float const rad = w.rot_bulb_get_rad(b);
            w.rot_bulb_set_last_pos(b, pos);
            pos += vel;
            vel *= custom_place ? 0.8f : 0.9f;
            vel += w.rot_bulb_get_look_dir(b) * 0.1f;
            vel -= (pos - stuck) / 10.f;
            w.rot_bulb_set_last_closed(b, w.rot_bulb_get_closed(b));
            if (w.rot_bulb_get_bubbles_wait(b) > 0)
                w.rot_bulb_set_bubbles_wait(b, int16_t(w.rot_bulb_get_bubbles_wait(b) - 1));
            if (!custom::dist_less(pos, stuck, rad / 2.f)) {
                vec2 const off = rw::normalized(pos - stuck) * (rw::distance(pos, stuck) - rad / 2.f);
                vel -= off;
                pos -= off;
            }
            rot_prey eat = w.rot_bulb_get_eat_chunk(b);
            if (eat.valid() && !prey_in_room(state, eat, room))
                eat = rot_prey{};
            if (eat.valid()) {
                auto const c = prey_chunk(state, eat);
                vec2& cpos = w.body_chunk_get_pos(c);
                vec2& cvel = w.body_chunk_get_vel(c);
                vel += custom::dir_vec(pos, cpos) * (is_passive ? 0.2f : 1.f);
                if (is_passive) {
                    cvel *= vec2{1.f - 0.4f * cd.slowdown, 1.f - 0.2f * cd.slowdown};
                } else {
                    // pulled towards the thick of the rot
                    ivec2 const tile = w.rot_bulb_get_tile(b);
                    float num = corruption_level(state, room, tile);
                    vec2 a3 = custom::dir_vec(cpos, pos);
                    for (auto const d : custom::eight_directions) {
                        float const num2 = corruption_level(state, room, tile + d);
                        if (num2 > num) {
                            a3 = rw::normalized(rw::to_vec2(d));
                            num = num2;
                        }
                    }
                    cvel *= 0.9f;
                    cvel += a3 * rad * 0.02f / w.body_chunk_get_mass(c);
                    nibble(state, s, b, eat);
                    pos += rnv(r);
                    vel += rnv(r) * 0.2f;
                    w.rot_bulb_set_closed(b, mathf::min(1.f, w.rot_bulb_get_closed(b) + 0.1f));
                    if (w.rot_bulb_get_felt_something_delay(b) < 1 && r.value() < 0.025f)
                        feel(state, b, r.value(), cpos);
                }
                if (!custom::dist_less(cpos, pos, rad + w.body_chunk_get_rad(c) + (is_passive ? 2.f : 10.f)))
                    eat = rot_prey{};
                w.rot_bulb_set_eat_chunk(b, eat);
                return;
            }
            for_each_creature_chunk(state, room, [&](rot_prey p, dcon::body_chunk_id c) {
                if (!custom::dist_less(w.body_chunk_get_pos(c), pos, w.body_chunk_get_rad(c) + rad))
                    return false;
                eat = p;
                feel(state, b, 1.f, w.body_chunk_get_pos(c));
                return true;
            });
            w.rot_bulb_set_eat_chunk(b, eat);
            if (auto const leg = w.rot_bulb_get_leg(b); leg) {
                auto const segs = w.rot_tube_get_segments(leg);
                int32_t const n = int32_t(segs.size());
                vel += custom::dir_vec(pos, segs[uint32_t(custom::int_clamp(n / 2, 0, n - 1))].pos);
            }
            w.rot_bulb_set_closed(b, mathf::max(0.f, w.rot_bulb_get_closed(b) - 0.05f));
            vec2 const look = w.rot_bulb_get_look_dir(b);
            float const light = w.rot_bulb_get_light(b);
            float const num7 = light * mathf::inverse_lerp(0.f, 1.f, rw::distance(w.rot_bulb_get_last_look_dir(b), look));
            w.rot_bulb_set_light(b, mathf::max(0.f, light - 0.05f));
            if (r.value() < num7)
                w.rot_bulb_set_get_to_focus(b, mathf::max(w.rot_bulb_get_get_to_focus(b), r.value()));
            else if (r.value() < 1.f / 70.f)
                w.rot_bulb_set_get_to_focus(b, 0.f);
            float const focus = w.rot_bulb_get_focus(b);
            float const goal = w.rot_bulb_get_get_to_focus(b);
            w.rot_bulb_set_last_focus(b, focus);
            w.rot_bulb_set_focus(b, focus < goal ? mathf::min(focus + 0.05f, goal) : mathf::max(focus - 0.05f, goal));
            if (r.value() < 0.01f)
                w.rot_bulb_set_leg_reach_pos(b, std::nullopt);
            w.rot_bulb_set_last_look_dir(b, look);
            if (w.rot_bulb_get_reaction_delay(b) < 1) {
                vec2 const next = w.rot_bulb_get_next_look_dir(b);
                w.rot_bulb_set_look_dir(b, next);
                if (w.rot_bulb_get_has_heard_sound(b)) {
                    w.rot_bulb_set_light(b, mathf::max(w.rot_bulb_get_light(b), r.value()));
                    w.rot_bulb_set_leg_reach_pos(b, pos + next * mathf::lerp(100.f, 200.f, r.value()));
                }
                w.rot_bulb_set_has_heard_sound(b, false);
                w.rot_bulb_set_reaction_delay(b, int16_t(r.range(10, 20)));
            } else {
                w.rot_bulb_set_reaction_delay(b, int16_t(w.rot_bulb_get_reaction_delay(b) - 1));
            }
            if (r.value() < 0.00125f)
                w.rot_bulb_set_next_look_dir(b, rnv(r));
            if (w.rot_bulb_get_felt_something_delay(b) > 0)
                w.rot_bulb_set_felt_something_delay(b, int16_t(w.rot_bulb_get_felt_something_delay(b) - 1));
            if (w.rot_bulb_get_felt_something_intensity(b) > 0.f) {
                transmit_feel(state, s, b);
                w.rot_bulb_set_felt_something_intensity(b, 0.f);
            }
        }

        // DaddyCorruption.EatenCreature.Update; true once it is done with
        bool meal_update(sys::game_state& state, dcon::rot_system_id s, dcon::rot_meal_id m) {
            auto& w = state.world;
            rot_prey const p = w.rot_meal_get_prey(m);
            if (!prey_in_room(state, p, room_of(state, s)))
                return true;
            if (w.rot_meal_get_wait(m) <= 12000.f) {
                w.rot_meal_set_disable_counter(m, w.rot_meal_get_disable_counter(m) + 1);
                return w.rot_meal_get_disable_counter(m) > 20;
            }
            auto const o = prey_body(state, p);
            w.physical_object_for_each_chunk_connection_as_owner(o, [&](dcon::chunk_connection_id k) {
                w.chunk_connection_set_type(k, physics::connection_type::pull);
            });
            float const prog = w.rot_meal_get_progression(m) + 0.05f;
            w.rot_meal_set_progression(m, prog);
            vec2 const goal = w.rot_meal_get_goal_pos(m);
            w.physical_object_for_each_chunk_ownership_as_owner(o, [&](dcon::chunk_ownership_id co) {
                auto const c = w.chunk_ownership_get_chunk(co);
                w.body_chunk_set_collide_with_terrain(c, false);
                w.body_chunk_get_vel(c) *= 1.f - prog;
                w.body_chunk_set_pos(c, rw::lerp(w.body_chunk_get_pos(c), goal, prog));
            });
            if (prog < 1.f)
                return false;
            // Creature.Die, then Destroy
            if (p.kind == prey_kind::player) {
                auto const pl = prey_player(p);
                items::release_all(state, pl);
                centipede::release_player(state, pl);
                release_player(state, pl);
                player::die(state, pl);
                w.player_set_eaten(pl, 1);
            } else {
                centipede::destroy(state, prey_centipede(p));
            }
            return true;
        }

        //
        // DaddyCorruption.AIMapReady
        //
        void create_bulb(sys::game_state& state, dcon::rot_system_id s, ivec2 tile, bool black_goo, rw::rng& r) {
            auto& w = state.world;
            auto const room = room_of(state, s);
            auto const b = w.create_rot_bulb();
            w.try_create_rot_bulb_membership(b, s);
            w.rot_bulb_set_tile(b, tile);
            w.rot_bulb_set_has_black_goo(b, black_goo);
            vec2 stuck = room_view::middle_of_tile(tile);
            for (auto const d : custom::eight_directions)
                if (occupied(state, s, tile + d))
                    stuck = rw::lerp(stuck, room_view::middle_of_tile(tile + d), r.value() * 0.5f);
            w.rot_bulb_set_next_look_dir(b, rnv(r));
            auto const cp = custom_rot_data(state, room, tile);
            w.rot_bulb_set_custom(b, cp);
            world::rot_custom_data const cd = cp ? w.rot_placed_get_custom(cp) : world::rot_custom_data{};
            float const level = corruption_level(state, room, stuck);
            w.rot_bulb_set_corruption_level(b, level);
            float rad;
            if (cp)
                rad = mathf::lerp(cd.min_size, mathf::lerp(cd.min_size, cd.max_size, range(r, 0.5f, 1.f)), level);
            else
                rad = mathf::lerp(4.f, 10.f + 10.f * r.value(), level);
            w.rot_bulb_set_rad(b, rad);
            bool has_eye;
            if (cp)
                has_eye = cd.cross_eyes && r.value() < level * mathf::min(cd.eye_chance * 2.f, 1.f) + mathf::max(cd.eye_chance * 2.f - 1.f, 0.f);
            else
                has_eye = r.value() < level;
            // no two eyes on top of each other
            for (int32_t i = 0; i < 5 && has_eye; ++i) {
                auto const cell = cell_at(state, s, tile + four_directions_and_zero[i]);
                for (uint32_t k = 0; k < cell.count; ++k) {
                    auto const o = bulb_at(cell.first + k);
                    if (w.rot_bulb_get_has_eye(o) && custom::dist_less(stuck, w.rot_bulb_get_stuck_pos(o), rad + w.rot_bulb_get_rad(o))) {
                        has_eye = false;
                        break;
                    }
                }
            }
            stuck -= dir_at(state, s, room_view::get_tile_position(stuck)) * (20.f - rad);
            w.rot_bulb_set_stuck_pos(b, stuck);
            w.rot_bulb_set_pos(b, stuck);
            w.rot_bulb_set_last_pos(b, stuck);
            w.rot_bulb_set_eye_stalk_pos(b, stuck - dir_at(state, s, tile) * mathf::lerp(10.f, 50.f, r.value()));
            w.rot_bulb_set_rotation(b, r.value() * 360.f);
            w.rot_bulb_set_has_eye(b, has_eye);
            if (has_eye)
                w.rot_bulb_set_eye_rad(b, cp ? range(r, cd.min_eye_size, cd.max_eye_size) : r.value());
            auto const own_cell = cell_at(state, s, tile);
            // the bulbs of this tile so far (this one isn't counted yet)
            uint32_t const before = uint32_t(b.index()) - own_cell.first;
            bool const has_dot = before == 0 && cp && !cd.cross_eyes && r.value() < cd.eye_chance;
            w.rot_bulb_set_has_dot(b, has_dot);
            bool midground = !has_eye && !has_dot && r.value() < 0.5f;
            if (cp && !cd.cross_eyes && before > 0 && w.rot_bulb_get_has_dot(bulb_at(own_cell.first)))
                midground = true;
            w.rot_bulb_set_body_in_midground(b, midground);
            // InitiateSprites/ApplyPalette rolls
            w.rot_bulb_set_dot_scale(b, range(r, cp ? cd.min_eye_size : 0.45f, cp ? cd.max_eye_size : 0.65f));
            w.rot_bulb_set_color_fac(b, cp && cd.cross_eyes ? mathf::lerp(cd.min_color, cd.max_color, mathf::pow(r.value(), 0.5f)) : 0.f);
            if (r.value() < 0.2f && r.value() < level && (!cp || cd.hostile)) {
                vec2 const tip = stuck + dir_at(state, s, tile) * mathf::lerp(20.f, 150.f, mathf::pow(r.value(), 0.5f) * level);
                w.rot_bulb_set_leg(b, create_leg(state, s, b, stuck, tip, r));
            }
            room_view const view = world::get_room_view(state, room);
            if (cp && !cd.hostile && r.value() < cd.leg_chance && r.value() < mathf::pow(level, 0.5f)
            && dir_at(state, s, room_view::get_tile_position(stuck)).y < 0.f
            && view.get_tile(room_view::get_tile_position(stuck) - ivec2{0, 1}).t == world::terrain::air) {
                vec2 const tip = stuck + vec2{0.f, -1.f} * mathf::lerp(35.f, 65.f, mathf::pow(r.value(), 0.75f) * level);
                w.rot_bulb_set_tendril(b, create_tendril(state, s, b, stuck, tip, r));
            }
        }
    }

    dcon::rot_system_id system_of(sys::game_state const& state, dcon::room_id room) noexcept {
        return state.world.room_get_system_from_rot_system_location(room);
    }

    void spawn_room(sys::game_state& state, dcon::room_id room) {
        auto& w = state.world;
        if (w.room_get_rot_spawned(room))
            return;
        w.room_set_rot_spawned(room, true);
        std::vector<place> places;
        for_each_place(state, room, [&](place const& p) { places.push_back(p); });
        if (places.empty())
            return;
        room_view const view = world::get_room_view(state, room);
        auto const s = w.create_rot_system();
        w.try_create_rot_system_location(s, room);
        // UnityEngine.Random.InitState(places[0].pos.x + places[0].pos.y)
        rw::rng r{uint32_t(int32_t(places[0].pos.x) + int32_t(places[0].pos.y)) * 0x9E3779B1u ^ 0x2545F491u};
        if (r.state == 0)
            r.state = 0x2545F491u;

        // the air tiles touching a wall within the areas
        std::vector<ivec2> tiles;
        std::unordered_set<uint64_t> seen;
        ivec2 bl{std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::max()};
        ivec2 tr{std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::min()};
        for (auto const& p : places) {
            ivec2 const c = room_view::get_tile_position(p.pos);
            int32_t const reach = int32_t(p.rad / 20.f) + 1;
            for (int32_t j = c.x - reach; j <= c.x + reach; ++j) {
                for (int32_t k = c.y - reach; k <= c.y + reach; ++k) {
                    uint64_t const key = (uint64_t(uint32_t(j)) << 32) | uint32_t(k);
                    if (view.get_tile(j, k).solid() || !custom::dist_less(room_view::middle_of_tile(j, k), p.pos, p.rad) || seen.contains(key))
                        continue;
                    bool wall = false;
                    for (auto const d : custom::eight_directions)
                        wall = wall || view.get_tile(j + d.x, k + d.y).solid();
                    if (!wall)
                        continue;
                    seen.insert(key);
                    tiles.push_back(ivec2{j, k});
                    bl = ivec2{std::min(bl.x, j), std::min(bl.y, k)};
                    tr = ivec2{std::max(tr.x, j), std::max(tr.y, k)};
                }
            }
        }
        if (tiles.empty()) {
            bl = tr = ivec2{0, 0};
        }
        w.rot_system_set_bottom_left(s, bl);
        w.rot_system_set_top_right(s, tr);
        grid const g{bl, tr};
        auto dirs = w.rot_system_get_directions(s);
        auto cells = w.rot_system_get_cells(s);
        uint32_t const cell_count = tiles.empty() ? 0u : uint32_t(g.width() * (tr.y - bl.y + 1));
        dirs.resize(cell_count);
        cells.resize(cell_count);
        for (uint32_t i = 0; i < cell_count; ++i) {
            dirs[i] = vec2{0.f, 0.f};
            cells[i] = world::rot_cell{};
        }
        // pointing away from the walls...
        for (auto const t : tiles) {
            vec2 v{0.f, 0.f};
            for (int32_t m = 1; m < 3; ++m)
                for (auto const d : custom::eight_directions) {
                    vec2 const n = rw::normalized(rw::to_vec2(d)) / float(m);
                    v += view.get_tile(t + d * m).solid() ? -n : n;
                }
            dirs[g.index(t)] = rw::normalized(v);
        }
        // ...and from the rest of the rot
        for (auto const t : tiles) {
            vec2 a{0.f, 0.f};
            for (int32_t m = 1; m < 4; ++m)
                for (auto const d : custom::eight_directions) {
                    if (view.get_tile(t + d * m).solid())
                        continue;
                    vec2 const n = rw::normalized(rw::to_vec2(d)) / float(m);
                    a += occupied(state, s, t + d * m) ? -n : n;
                }
            dirs[g.index(t)] = rw::normalized(dirs[g.index(t)] + a * 0.15f);
        }
        w.rot_system_set_gw_mode(s, false);
        auto const region = w.room_get_region_from_region_membership(room);
        std::string_view const rname = region ? state.to_string_view(w.region_get_name(region)) : std::string_view{};
        auto const is = [&](std::string_view n) { return rname.size() == n.size() && std::equal(n.begin(), n.end(), rname.begin(), [](char a, char b) { return std::tolower(uint8_t(a)) == std::tolower(uint8_t(b)); }); };
        if (is("hr")) {
            // Rubicon: RainWorld.SaturatedGold
            w.rot_system_set_effect_color(s, rw::vec3{1.058f, 0.73f, 0.368f});
            w.rot_system_set_eye_color(s, rw::vec3{1.058f, 0.73f, 0.368f});
        } else {
            // Garbage Wastes rot is in its later, quieter state from Survivor's
            // timeline on (Artificer's comes before it)
            bool artificer = false;
            w.for_each_player([&](dcon::player_id p) {
                artificer = artificer || w.player_get_stats(p).name == player::slugcat_name::artificer;
            });
            w.rot_system_set_gw_mode(s, (is("gw") || is("cl")) && !artificer);
            w.rot_system_set_effect_color(s, region ? w.region_get_corruption_effect_color(region) : rw::vec3{0.f, 0.f, 1.f});
            w.rot_system_set_eye_color(s, region ? w.region_get_corruption_eye_color(region) : rw::vec3{0.f, 0.f, 1.f});
            // the Watcher's sentient rot (DaddyCorruption.SentientRotMode): the
            // Outer Rim's and the rot regions' is purple
            bool watcher = false;
            w.for_each_player([&](dcon::player_id p) { watcher = watcher || w.player_get_stats(p).name == player::slugcat_name::watcher; });
            if (is("wora") || is("wrsa") || is("wsur") || (watcher && rname.size() > 1 && (rname[0] == 'w' || rname[0] == 'W'))) {
                w.rot_system_set_effect_color(s, rw::vec3{0.5f, 0.12f, 0.72f});
                w.rot_system_set_eye_color(s, rw::vec3{0.75f, 0.35f, 1.f});
            }
        }

        // bulbs, a few per tile, more where the rot is thick
        for (auto const t : tiles) {
            auto const cp = custom_rot_data(state, room, t);
            float const density = cp ? w.rot_placed_get_custom(cp).density : 1.f;
            float const darkness = cp ? w.rot_placed_get_custom(cp).darkness_chance : 1.f;
            uint32_t const first = w.rot_bulb_size();
            int32_t const count = r.range(1, 1 + int32_t(density * mathf::lerp(1.f, 3.f, corruption_level(state, room, t))));
            w.rot_system_get_cells(s)[g.index(t)] = world::rot_cell{first, 0};
            for (int32_t i = count; i >= 0; --i) {
                bool goo = i == 0;
                if (goo && cp && r.value() > darkness)
                    goo = false;
                create_bulb(state, s, t, goo, r);
                auto c = w.rot_system_get_cells(s)[g.index(t)];
                ++c.count;
                w.rot_system_get_cells(s)[g.index(t)] = c;
            }
        }
        // the climbable tubes
        w.room_for_each_rot_placed_location(room, [&](dcon::rot_placed_location_id l) {
            auto const o = w.rot_placed_location_get_placed(l);
            if (w.rot_placed_get_kind(o) == world::rot_kind::tube)
                create_climb_tube(state, s, w.rot_placed_get_pos(o), w.rot_placed_get_handle(o), r);
        });
        // settle the graphic segments so the first frame doesn't interpolate from the origin
        w.rot_system_for_each_rot_tube_membership(s, [&](dcon::rot_tube_membership_id m) {
            auto const t = w.rot_tube_membership_get_tube(m);
            graphic_update(state, t);
            graphic_update(state, t);
        });
    }

    void update_room(sys::game_state& state, room_view const& room) {
        auto& w = state.world;
        auto const s = system_of(state, room.id);
        if (!s)
            return;
        auto& r = w.rot_system_get_random(s);
        // DaddyCorruption.NoiseInRoom
        for (auto const& n : state.noises) {
            if (n.room != room.id)
                continue;
            w.rot_system_for_each_rot_bulb_membership(s, [&](dcon::rot_bulb_membership_id m) {
                auto const b = w.rot_bulb_membership_get_bulb(m);
                if (custom::dist_less(n.pos, w.rot_bulb_get_pos(b), n.strength))
                    heard_noise(state, b, n.pos);
            });
        }
        w.rot_system_for_each_rot_bulb_membership(s, [&](dcon::rot_bulb_membership_id m) {
            bulb_update(state, s, w.rot_bulb_membership_get_bulb(m), r);
        });
        std::vector<dcon::rot_meal_id> done;
        w.rot_system_for_each_rot_meal_membership(s, [&](dcon::rot_meal_membership_id m) {
            auto const meal = w.rot_meal_membership_get_meal(m);
            if (meal_update(state, s, meal))
                done.push_back(meal);
        });
        for (auto const m : done)
            w.delete_rot_meal(m);
        w.rot_system_for_each_rot_tube_membership(s, [&](dcon::rot_tube_membership_id m) {
            auto const t = w.rot_tube_membership_get_tube(m);
            switch (w.rot_tube_get_kind(t)) {
            case rot_tube_kind::leg:
                leg_update(state, room, t, r);
                break;
            case rot_tube_kind::tendril:
                tendril_update(state, room, t, r);
                break;
            case rot_tube_kind::climb:
                climb_update(state, room, t, r);
                break;
            }
        });
    }

    void release_player(sys::game_state& state, dcon::player_id p) {
        auto& w = state.world;
        auto const is_p = [&](rot_prey const& q) { return q.kind == prey_kind::player && q.id == uint16_t(p.index()); };
        w.for_each_rot_tube([&](dcon::rot_tube_id t) {
            if (is_p(w.rot_tube_get_grab_chunk(t)))
                w.rot_tube_set_grab_chunk(t, rot_prey{});
        });
        w.for_each_rot_bulb([&](dcon::rot_bulb_id b) {
            if (is_p(w.rot_bulb_get_eat_chunk(b)))
                w.rot_bulb_set_eat_chunk(b, rot_prey{});
        });
        std::vector<dcon::rot_meal_id> gone;
        w.for_each_rot_meal([&](dcon::rot_meal_id m) {
            if (is_p(w.rot_meal_get_prey(m)))
                gone.push_back(m);
        });
        for (auto const m : gone)
            w.delete_rot_meal(m);
    }

    vec2 tube_graphic_pos(sys::game_state const& state, dcon::rot_tube_id t, float float_pos, float alpha) {
        // TubeGraphic.OneDimensionalTubePos
        auto const g = state.world.rot_tube_get_graphic(t);
        int32_t const n = int32_t(g.size());
        int32_t const a = custom::int_clamp(int32_t(std::floor(float_pos * float(n - 1))), 0, n - 1);
        int32_t const b = custom::int_clamp(a + 1, 0, n - 1);
        float const f = mathf::inverse_lerp(float(a), float(b), float_pos * float(n - 1));
        auto const ga = g[uint32_t(a)];
        auto const gb = g[uint32_t(b)];
        return rw::lerp(rw::lerp(ga.last_pos, gb.last_pos, f), rw::lerp(ga.pos, gb.pos, f), alpha);
    }

    namespace vines {
        namespace {
            // IClimbableVine of a ClimbableCorruptionTube
            uint32_t total_positions(sys::game_state const& state, dcon::rot_tube_id v) { return state.world.rot_tube_get_segments(v).size(); }
            vec2 vpos(sys::game_state const& state, dcon::rot_tube_id v, uint32_t i) { return state.world.rot_tube_get_segments(v)[i].pos; }
            constexpr float vrad = 2.f;
            constexpr float vmass = 0.25f;
            void vpush(sys::game_state& state, dcon::rot_tube_id v, uint32_t i, vec2 movement) {
                auto segs = state.world.rot_tube_get_segments(v);
                auto s = segs[i];
                s.pos += movement;
                s.vel += movement;
                segs[i] = s;
            }

            float float_at_segment(sys::game_state const& state, dcon::rot_tube_id v, uint32_t segment) {
                float total = 0.f, upto = 0.f;
                uint32_t const n = total_positions(state, v);
                for (uint32_t i = 0; i + 1 < n; ++i) {
                    float const d = rw::distance(vpos(state, v, i), vpos(state, v, i + 1));
                    if (i < segment)
                        upto += d;
                    total += d;
                }
                return total > 0.f ? upto / total : 0.f;
            }
            uint32_t prev_seg_at_float(sys::game_state const& state, dcon::rot_tube_id v, float f) {
                f *= total_length(state, v);
                float num = 0.f;
                uint32_t const n = total_positions(state, v);
                for (uint32_t i = 0; i + 1 < n; ++i) {
                    num += rw::distance(vpos(state, v, i), vpos(state, v, i + 1));
                    if (num > f)
                        return i;
                }
                return n - 1;
            }
            // the two segments around a position and how far between them
            struct span {
                uint32_t a;
                uint32_t b;
                float t;
            };
            span span_at(sys::game_state const& state, position p) {
                uint32_t const a = prev_seg_at_float(state, p.vine, p.float_pos);
                uint32_t const b = uint32_t(custom::int_clamp(int32_t(a) + 1, 0, int32_t(total_positions(state, p.vine)) - 1));
                return span{a, b, mathf::inverse_lerp(float_at_segment(state, p.vine, a), float_at_segment(state, p.vine, b), p.float_pos)};
            }
            vec2 dir_of_segment(sys::game_state const& state, dcon::rot_tube_id v, uint32_t seg) {
                uint32_t const n = total_positions(state, v);
                if (seg == n - 1)
                    return custom::dir_vec(vpos(state, v, n - 2), vpos(state, v, n - 1));
                return custom::dir_vec(vpos(state, v, seg), vpos(state, v, seg + 1));
            }
            bool overlapping_segment(vec2 a, float arad, vec2 b, float brad, vec2 test, float trad) {
                vec2 const perp = custom::perpendicular_vector(rw::normalized(a - b));
                if (distance_to_line(test, a + perp, a - perp) >= 0.f)
                    return rw::distance(test, a) < arad + trad;
                if (distance_to_line(test, b + perp, b - perp) <= 0.f)
                    return rw::distance(test, b) < brad + trad;
                vec2 const c = closest_point_on_line(a, b, test);
                return rw::distance(c, test) < custom::lerp_map(rw::distance(a, c), 0.f, rw::distance(a, b), arad, brad) + trad;
            }
            vec2 closest_point_on_segment(vec2 a, vec2 b, vec2 test) {
                vec2 const perp = custom::perpendicular_vector(rw::normalized(a - b));
                if (distance_to_line(test, a + perp, a - perp) >= 0.f)
                    return a;
                if (distance_to_line(test, b + perp, b - perp) <= 0.f)
                    return b;
                return closest_point_on_line(a, b, test);
            }
            template<typename F>
            void for_each_vine(sys::game_state const& state, dcon::room_id room, F&& f) {
                auto const s = system_of(state, room);
                if (!s)
                    return;
                state.world.rot_system_for_each_rot_tube_membership(s, [&](dcon::rot_tube_membership_id m) {
                    auto const t = state.world.rot_tube_membership_get_tube(m);
                    if (state.world.rot_tube_get_kind(t) == rot_tube_kind::climb && total_positions(state, t) >= 2)
                        f(t);
                });
            }
        }

        bool valid(sys::game_state const& state, dcon::room_id room, dcon::rot_tube_id vine) noexcept {
            auto const& w = state.world;
            return vine && uint32_t(vine.index()) < w.rot_tube_size() && w.rot_tube_get_kind(vine) == rot_tube_kind::climb
                && room_of(state, w.rot_tube_get_system_from_rot_tube_membership(vine)) == room;
        }

        float total_length(sys::game_state const& state, dcon::rot_tube_id v) {
            float num = 0.f;
            uint32_t const n = total_positions(state, v);
            for (uint32_t i = 0; i + 1 < n; ++i)
                num += rw::distance(vpos(state, v, i), vpos(state, v, i + 1));
            return num;
        }

        std::optional<position> overlap(sys::game_state const& state, dcon::room_id room, vec2 pos, float rad) {
            float best = std::numeric_limits<float>::max();
            std::optional<position> result;
            for_each_vine(state, room, [&](dcon::rot_tube_id v) {
                uint32_t const n = total_positions(state, v);
                for (uint32_t j = 0; j + 1 < n; ++j) {
                    vec2 const a = vpos(state, v, j), b = vpos(state, v, j + 1);
                    if (!overlapping_segment(a, vrad, b, vrad, pos, rad))
                        continue;
                    vec2 const c = closest_point_on_segment(a, b, pos);
                    float const d = rw::distance(pos, c);
                    if (d < best) {
                        best = d;
                        float const t = mathf::inverse_lerp(0.f, rw::distance(a, b), rw::distance(a, c));
                        result = position{v, mathf::lerp(float_at_segment(state, v, j), float_at_segment(state, v, j + 1), t)};
                    }
                }
            });
            return result;
        }

        vec2 on_vine_pos(sys::game_state const& state, position p) {
            span const s = span_at(state, p);
            return rw::lerp(vpos(state, p.vine, s.a), vpos(state, p.vine, s.b), s.t);
        }

        float vine_rad(sys::game_state const&, position) { return vrad; }

        vec2 vine_dir(sys::game_state const& state, position p) {
            span const s = span_at(state, p);
            uint32_t const n = total_positions(state, p.vine);
            if (s.a == s.b)
                return custom::dir_vec(vpos(state, p.vine, n - 2), vpos(state, p.vine, n - 1));
            return rw::normalized(rw::lerp(dir_of_segment(state, p.vine, s.a), dir_of_segment(state, p.vine, s.b), s.t));
        }

        float climb_on_vine_speed(sys::game_state const& state, position p, vec2 goal) {
            if (p.float_pos == 0.f)
                return 1.f;
            if (p.float_pos == 1.f)
                return -1.f;
            span const s = span_at(state, p);
            vec2 const pa = vpos(state, p.vine, s.a), pb = vpos(state, p.vine, s.b);
            vec2 const at = rw::lerp(pa, pb, s.t);
            float const f = rw::dot(rw::normalized(pa - pb), rw::normalized(at - goal));
            return mathf::inverse_lerp(0.3f, 0.5f, std::fabs(f)) * mathf::sign(f);
        }

        std::optional<position> vine_switch(sys::game_state const& state, dcon::room_id room, position p, vec2 goal, float rad) {
            span const s = span_at(state, p);
            vec2 const pa = vpos(state, p.vine, s.a), pb = vpos(state, p.vine, s.b);
            vec2 const at = rw::lerp(pa, pb, s.t);
            // only when reaching away from the vine
            if (std::fabs(rw::dot(rw::normalized(pa - pb), rw::normalized(at - goal))) > 0.5f)
                return std::nullopt;
            float best = std::numeric_limits<float>::max();
            std::optional<position> result;
            float const own_len = total_length(state, p.vine);
            for_each_vine(state, room, [&](dcon::rot_tube_id v) {
                uint32_t const n = total_positions(state, v);
                for (uint32_t j = 0; j + 1 < n; ++j) {
                    vec2 const a = vpos(state, v, j), b = vpos(state, v, j + 1);
                    if (!overlapping_segment(a, vrad, b, vrad, at, rad))
                        continue;
                    vec2 const c = closest_point_on_segment(a, b, at);
                    float d = rw::distance(c, goal);
                    d *= 1.f - 0.25f * std::fabs(rw::dot(rw::normalized(a - b), rw::normalized(at - goal)));
                    float const t = mathf::inverse_lerp(0.f, rw::distance(a, b), rw::distance(a, c));
                    float const fp = mathf::lerp(float_at_segment(state, v, j), float_at_segment(state, v, j + 1), t);
                    // not just further along the same vine
                    if (v == p.vine && std::fabs(p.float_pos * own_len - fp * total_length(state, v)) < 100.f)
                        d = std::numeric_limits<float>::max();
                    if (d < best) {
                        best = d;
                        result = position{v, fp};
                    }
                }
            });
            return result;
        }

        void connect_chunk_to_vine(sys::game_state& state, dcon::body_chunk_id chunk, position p, float con_rad) {
            auto& w = state.world;
            span const s = span_at(state, p);
            vec2 const at = rw::lerp(vpos(state, p.vine, s.a), vpos(state, p.vine, s.b), s.t);
            float const mass = w.body_chunk_get_mass(chunk);
            float const num3 = mass / (mass + vmass);
            vec2& cpos = w.body_chunk_get_pos(chunk);
            float const d = rw::distance(cpos, at);
            vec2 const a = custom::dir_vec(cpos, at);
            if (d > con_rad) {
                cpos += a * (d - con_rad) * (1.f - num3);
                w.body_chunk_get_vel(chunk) += a * (d - con_rad) * (1.f - num3);
                vpush(state, p.vine, s.a, -a * (d - con_rad) * num3);
                vpush(state, p.vine, s.b, -a * (d - con_rad) * num3);
            }
        }

        void push_at_vine(sys::game_state& state, position p, vec2 push) {
            span const s = span_at(state, p);
            float const d = mathf::max(1.f, vmass);
            vpush(state, p.vine, s.a, push / d * (1.f - s.t));
            vpush(state, p.vine, s.b, push / d * s.t);
        }
    }
}
