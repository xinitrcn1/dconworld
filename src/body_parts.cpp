#include <cmath>

#include "body_parts.hpp"

namespace body_parts {
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    using world::terrain;
    using player::limb_mode;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        constexpr ivec2 eight_directions_and_zero[9] = {
            {0, 0}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}
        };
    }

    // BodyPart.ConnectToPoint
    void connect_to_point(player::body_part& bp, vec2 pnt, float connection_rad, bool push, float elastic, vec2 host_vel, float adapt, float exaggerate) {
        if (elastic > 0.f)
            bp.vel += custom::dir_vec(bp.pos, pnt) * rw::distance(bp.pos, pnt) * elastic;
        bp.vel += host_vel * exaggerate;
        if (push || !custom::dist_less(bp.pos, pnt, connection_rad)) {
            float const d = rw::distance(bp.pos, pnt);
            vec2 const a = custom::dir_vec(bp.pos, pnt);
            bp.pos -= a * (connection_rad - d);
            bp.vel -= a * (connection_rad - d);
        }
        bp.vel -= host_vel;
        bp.vel *= 1.f - adapt;
        bp.vel += host_vel;
    }

    // BodyPart.PushOutOfTerrain (tile terrain only)
    bool push_out_of_terrain(player::body_part& bp, room_view const& room, float surface_friction) {
        bool contact = false;
        for (auto const d : eight_directions_and_zero) {
            ivec2 const t = room_view::get_tile_position(bp.pos) + d;
            auto const tt = room.get_terrain(t.x, t.y);
            if (tt == terrain::solid) {
                vec2 const m = room_view::middle_of_tile(t);
                float nx = 0.f, ny = 0.f;
                if (bp.pos.y >= m.y - 10.f && bp.pos.y <= m.y + 10.f) {
                    if (bp.last_pos.x < m.x) {
                        if (bp.pos.x > m.x - 10.f - bp.rad && room.get_terrain(t.x - 1, t.y) != terrain::solid)
                            nx = m.x - 10.f - bp.rad;
                    } else if (bp.pos.x < m.x + 10.f + bp.rad && room.get_terrain(t.x + 1, t.y) != terrain::solid) {
                        nx = m.x + 10.f + bp.rad;
                    }
                }
                if (bp.pos.x >= m.x - 10.f && bp.pos.x <= m.x + 10.f) {
                    if (bp.last_pos.y < m.y) {
                        if (bp.pos.y > m.y - 10.f - bp.rad && room.get_terrain(t.x, t.y - 1) != terrain::solid)
                            ny = m.y - 10.f - bp.rad;
                    } else if (bp.pos.y < m.y + 10.f + bp.rad && room.get_terrain(t.x, t.y + 1) != terrain::solid) {
                        ny = m.y + 10.f + bp.rad;
                    }
                }
                if (std::fabs(bp.pos.x - nx) < std::fabs(bp.pos.y - ny) && nx != 0.f) {
                    bp.pos.x = nx;
                    bp.vel.x = nx - bp.pos.x;
                    bp.vel.y *= surface_friction;
                    contact = true;
                } else if (ny != 0.f) {
                    bp.pos.y = ny;
                    bp.vel.y = ny - bp.pos.y;
                    bp.vel.x *= surface_friction;
                    contact = true;
                } else {
                    vec2 const c{mathf::clamp(bp.pos.x, m.x - 10.f, m.x + 10.f), mathf::clamp(bp.pos.y, m.y - 10.f, m.y + 10.f)};
                    if (custom::dist_less(bp.pos, c, bp.rad)) {
                        float const dist = rw::distance(bp.pos, c);
                        vec2 const a = custom::dir_vec(bp.pos, c);
                        bp.vel *= surface_friction;
                        bp.pos -= a * (bp.rad - dist);
                        bp.vel -= a * (bp.rad - dist);
                        contact = true;
                    }
                }
            } else if (d.x == 0 && tt == terrain::slope) {
                vec2 const m = room_view::middle_of_tile(t);
                auto const slope = room.identify_slope(t);
                using world::slope_direction;
                float target = 0.f;
                bool hit = false;
                if (slope == slope_direction::up_left) {
                    target = m.y - (m.x - bp.pos.x) + bp.rad;
                    hit = bp.pos.y < target;
                } else if (slope == slope_direction::up_right) {
                    target = m.y + (m.x - bp.pos.x) + bp.rad;
                    hit = bp.pos.y < target;
                } else if (slope == slope_direction::down_left) {
                    target = m.y + (m.x - bp.pos.x) - bp.rad;
                    hit = bp.pos.y > target;
                } else if (slope == slope_direction::down_right) {
                    target = m.y - (m.x - bp.pos.x) - bp.rad;
                    hit = bp.pos.y > target;
                }
                if (hit) {
                    bp.pos.y = target;
                    bp.vel.y = 0.f;
                    bp.vel.x *= surface_friction;
                    contact = true;
                }
            }
        }
        return contact;
    }

    // GenericBodyPart.Update
    void generic_update(player::body_part& bp, room_view const& room, float air_friction, float surface_friction) {
        bp.last_pos = bp.pos;
        bp.pos += bp.vel;
        bp.vel *= air_friction;
        push_out_of_terrain(bp, room, surface_friction);
    }

    // Custom.RotateAroundOrigo (clockwise degrees)
    vec2 rotate_around_origo(vec2 v, float deg) {
        float const a = deg * -0.017453292f;
        float const c = std::cos(a), sn = std::sin(a);
        return vec2{c * v.x - sn * v.y, sn * v.x + c * v.y};
    }

    // Limb.FindGrip: the closest point on terrain or a beam to `goal`
    void find_grip(player::hand_state& h, room_view const& room, vec2 attached, vec2 search_from, float max_rad, vec2 goal, int forbidden_x, int forbidden_y, bool behind_walls) {
        if (!custom::dist_less(attached, search_from, max_rad))
            search_from = attached + custom::dir_vec(attached, search_from) * (max_rad - 1.f);
        if (!custom::dist_less(attached, goal, max_rad))
            goal = attached + custom::dir_vec(attached, goal) * max_rad;
        ivec2 const tp = room_view::get_tile_position(search_from);
        vec2 best{-100000.f, -100000.f};
        auto const consider = [&](vec2 v) {
            if (rw::sqr_magnitude(goal - v) < rw::sqr_magnitude(goal - best) && custom::dist_less(attached, v, max_rad))
                best = v;
        };
        for (auto const d : eight_directions_and_zero) {
            if (d.x == forbidden_x || d.y == forbidden_y)
                continue;
            ivec2 const t = tp + d;
            vec2 const m = room_view::middle_of_tile(t);
            vec2 v{mathf::clamp(goal.x, m.x - 10.f, m.x + 10.f), mathf::clamp(goal.y, m.y - 10.f, m.y + 10.f)};
            auto const tile = room.get_tile(t);
            if (tile.t == terrain::solid) {
                if (d.x != 0 && room.get_terrain(t.x - d.x, t.y) != terrain::solid)
                    v.x = m.x - float(d.x) * 10.f;
                if (d.y != 0 && room.get_terrain(t.x, t.y - d.y) != terrain::solid)
                    v.y = m.y - float(d.y) * 10.f;
                consider(v);
            } else if (tile.t == terrain::floor) {
                v.y = m.y + 10.f;
                consider(v);
            } else if (tile.t == terrain::slope) {
                auto const sl = room.identify_slope(t);
                if (sl == world::slope_direction::down_left || sl == world::slope_direction::up_right)
                    v.y = m.y + 10.f - (v.x - (m.x - 10.f));
                else if (sl == world::slope_direction::down_right || sl == world::slope_direction::up_left)
                    v.y = m.y - 10.f + (v.x - (m.x - 10.f));
                consider(v);
            } else if (tile.t == terrain::air && behind_walls && (tile.flags & world::tile_wall_behind) && best == vec2{-100000.f, -100000.f}
                && custom::dist_less(attached, v, max_rad)) {
                best = v;
            }
            if (tile.horizontal_beam())
                consider(vec2{mathf::clamp(goal.x, m.x - 10.f, m.x + 10.f), m.y});
            if (tile.vertical_beam())
                consider(vec2{m.x, mathf::clamp(goal.y, m.y - 10.f, m.y + 10.f)});
        }
        if (best.x != -100000.f && best.y != -100000.f) {
            h.mode = limb_mode::hunt_absolute_position;
            h.absolute_hunt_pos = best;
        }
    }

    // Limb.Update
    void limb_update(player::hand_state& h, limb_params const& lp, vec2 conn, vec2 conn_vel, vec2 rotation_chunk, room_view const& room) {
        h.last_pos = h.pos;
        if (h.mode == limb_mode::hunt_relative_position)
            h.absolute_hunt_pos = conn + rotate_around_origo(h.relative_hunt_pos, custom::aim(rotation_chunk, conn));
        if (h.mode == limb_mode::hunt_relative_position || h.mode == limb_mode::hunt_absolute_position) {
            if (custom::dist_less(h.absolute_hunt_pos, h.pos, h.hunt_speed)) {
                h.vel = h.absolute_hunt_pos - h.pos;
                h.reached_snap_position = 1;
            } else {
                h.vel = rw::lerp(h.vel, custom::dir_vec(h.pos, h.absolute_hunt_pos) * h.hunt_speed, h.quickness);
                h.reached_snap_position = 0;
            }
        } else if (h.mode == limb_mode::retracted) {
            h.vel = conn_vel;
            h.pos = conn;
            h.reached_snap_position = 1;
        } else {
            h.reached_snap_position = 0;
        }
        h.quickness = lp.quickness;
        h.hunt_speed = lp.hunt_speed;
        if (h.mode != limb_mode::retracted) {
            h.pos += h.vel;
            if (h.mode == limb_mode::hunt_relative_position)
                h.pos += conn_vel;
            h.vel *= lp.air_friction;
            as_body_part(h, lp.rad, [&](player::body_part& bp) { push_out_of_terrain(bp, room, lp.surface_friction); });
        }
    }
}
