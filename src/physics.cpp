#include <cmath>
#include <vector>

#include "physics.hpp"

namespace physics {
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    using world::terrain;
    namespace mathf = rw::mathf;

    // BodyChunk.MaxRepeats, emergency breakout of the terrain checks
    static constexpr int32_t max_repeats = 100000;

    dcon::physical_object_id create_object(sys::game_state& state, dcon::room_id room, object_desc const& d) {
        auto const o = state.world.create_physical_object();
        state.world.physical_object_set_g(o, d.gravity);
        state.world.physical_object_set_air_friction(o, d.air_friction);
        state.world.physical_object_set_water_friction(o, d.water_friction);
        state.world.physical_object_set_buoyancy(o, d.buoyancy);
        state.world.physical_object_set_surface_friction(o, d.surface_friction);
        state.world.physical_object_set_bounce(o, d.bounce);
        state.world.physical_object_set_impact_threshold(o, d.impact_threshold);
        state.world.physical_object_set_water_retardation_immunity(o, 0.f);
        state.world.physical_object_set_grabbed_by_count(o, 0);
        if (room)
            state.world.physical_object_set_room_from_object_location(o, room);
        return o;
    }

    dcon::body_chunk_id add_chunk(sys::game_state& state, dcon::physical_object_id object, vec2 p, float rad, float mass) {
        auto const range = state.world.physical_object_range_of_chunk_ownership(object);
        uint8_t const index = uint8_t(range.second - range.first);
        auto const c = state.world.create_body_chunk();
        state.world.body_chunk_set_pos(c, p);
        state.world.body_chunk_set_last_pos(c, p);
        state.world.body_chunk_set_last_last_pos(c, p);
        state.world.body_chunk_set_vel(c, vec2{0.f, 0.f});
        state.world.body_chunk_set_rad(c, rad);
        state.world.body_chunk_set_mass(c, mass);
        state.world.body_chunk_set_terrain_squeeze(c, 1.f);
        state.world.body_chunk_set_slope_rad(c, rad);
        state.world.body_chunk_set_restrict_in_room_range(c, 1200.f);
        state.world.body_chunk_set_contact_point(c, ivec2{0, 0});
        state.world.body_chunk_set_last_contact_point(c, ivec2{0, 0});
        state.world.body_chunk_set_on_slope(c, 0);
        state.world.body_chunk_set_collide_with_terrain(c, true);
        state.world.body_chunk_set_collide_with_slopes(c, true);
        state.world.body_chunk_set_go_through_floors(c, false);
        state.world.body_chunk_set_index(c, index);
        state.world.try_create_chunk_ownership(c, object);
        return c;
    }

    dcon::chunk_connection_id add_connection(sys::game_state& state, dcon::physical_object_id object, dcon::body_chunk_id chunk1,
        dcon::body_chunk_id chunk2, float distance, connection_type type, float elasticity, float weight_symmetry) {
        auto const k = state.world.try_create_chunk_connection(chunk1, chunk2, object);
        state.world.chunk_connection_set_distance(k, distance);
        state.world.chunk_connection_set_elasticity(k, elasticity);
        // -1 means "derive from the masses"
        float const m1 = state.world.body_chunk_get_mass(chunk1);
        float const m2 = state.world.body_chunk_get_mass(chunk2);
        state.world.chunk_connection_set_weight_symmetry(k, weight_symmetry == -1.f ? m2 / (m1 + m2) : weight_symmetry);
        state.world.chunk_connection_set_type(k, type);
        state.world.chunk_connection_set_active(k, true);
        return k;
    }

    void delete_object(sys::game_state& state, dcon::physical_object_id object) {
        std::vector<dcon::body_chunk_id> chunks;
        state.world.physical_object_for_each_chunk_ownership(object, [&](dcon::chunk_ownership_id co) {
            chunks.push_back(state.world.chunk_ownership_get_chunk(co));
        });
        // deleting the chunks also deletes their ownership and connections
        for (auto const c : chunks)
            state.world.delete_body_chunk(c);
        state.world.delete_physical_object(object);
    }

    dcon::body_chunk_id chunk(sys::game_state const& state, dcon::physical_object_id object, uint32_t local) noexcept {
        auto const range = state.world.physical_object_range_of_chunk_ownership(object);
        for (auto it = range.first; it != range.second; ++it) {
            auto const c = state.world.chunk_ownership_get_chunk(*it);
            if (state.world.body_chunk_get_index(c) == local)
                return c;
        }
        return dcon::body_chunk_id{};
    }

    float terrain_rad(sys::game_state const& state, dcon::body_chunk_id c) noexcept {
        return mathf::max(state.world.body_chunk_get_rad(c) * state.world.body_chunk_get_terrain_squeeze(c), 1.f);
    }

    float submersion(sys::game_state const& state, room_view const& room, dcon::body_chunk_id c) noexcept {
        vec2 const p = state.world.body_chunk_get_pos(c);
        float const r = state.world.body_chunk_get_rad(c);
        return mathf::inverse_lerp(p.y - r, p.y + r, room.get_float_water_level(p));
    }

    float object_submersion(sys::game_state const& state, room_view const& room, dcon::physical_object_id object) noexcept {
        float sum = 0.f;
        uint32_t n = 0;
        state.world.physical_object_for_each_chunk_ownership(object, [&](dcon::chunk_ownership_id co) {
            sum += submersion(state, room, state.world.chunk_ownership_get_chunk(co));
            ++n;
        });
        return n > 0 ? sum / float(n) : 0.f;
    }

    bool is_tile_solid(sys::game_state const& state, room_view const& room, dcon::body_chunk_id c, int32_t rx, int32_t ry) noexcept {
        ivec2 const p = room_view::get_tile_position(state.world.body_chunk_get_pos(c)) + ivec2{rx, ry};
        terrain const t = room.get_terrain(p.x, p.y);
        if (t == terrain::solid)
            return true;
        if (t == terrain::floor && ry < 0 && !state.world.body_chunk_get_go_through_floors(c))
            return true;
        return false;
    }

    void set_go_through_floors(sys::game_state& state, dcon::physical_object_id object, bool v) noexcept {
        state.world.physical_object_for_each_chunk_ownership(object, [&](dcon::chunk_ownership_id co) {
            state.world.body_chunk_set_go_through_floors(state.world.chunk_ownership_get_chunk(co), v);
        });
    }

    void hard_set_position(sys::game_state& state, dcon::body_chunk_id c, vec2 p) noexcept {
        state.world.body_chunk_set_pos(c, p);
        state.world.body_chunk_set_last_pos(c, p);
        state.world.body_chunk_set_last_last_pos(c, p);
    }

    // BodyChunk.SolidFloor
    static bool solid_floor(sys::game_state const& state, room_view const& room, dcon::body_chunk_id c, int32_t x, int32_t y) noexcept {
        terrain const t = room.get_terrain(x, y);
        if (t == terrain::solid)
            return true;
        if (t == terrain::floor && !state.world.body_chunk_get_go_through_floors(c)) {
            // floors only hold if the whole body was above them last frame
            float lowest = room.pixel_height();
            state.world.physical_object_for_each_chunk_ownership(state.world.body_chunk_get_owner_from_chunk_ownership(c), [&](dcon::chunk_ownership_id co) {
                lowest = mathf::min(lowest, state.world.body_chunk_get_last_pos(state.world.chunk_ownership_get_chunk(co)).y);
            });
            return room_view::get_tile_position(vec2{0.f, lowest}).y > y;
        }
        return false;
    }

    namespace {
        struct impact_sink {
            terrain_impact_fn fn;
            void* user;
            void operator()(sys::game_state const& state, dcon::body_chunk_id c, ivec2 dir, float speed, bool first) const {
                if (fn != nullptr)
                    fn(user, state.world.body_chunk_get_owner_from_chunk_ownership(c), state.world.body_chunk_get_index(c), dir, speed, first);
            }
        };

        // Everything the collision passes need about one chunk. The references
        // point straight into the data container's property arrays.
        struct chunk_ref {
            sys::game_state& state;
            dcon::body_chunk_id c;
            dcon::physical_object_id o;
            vec2& pos;
            vec2& last_pos;
            vec2& vel;
            ivec2& contact;
            ivec2& last_contact;
            chunk_ref(sys::game_state& state, dcon::body_chunk_id c)
                : state{state}, c{c}, o{state.world.body_chunk_get_owner_from_chunk_ownership(c)}, pos{state.world.body_chunk_get_pos(c)},
                  last_pos{state.world.body_chunk_get_last_pos(c)}, vel{state.world.body_chunk_get_vel(c)},
                  contact{state.world.body_chunk_get_contact_point(c)}, last_contact{state.world.body_chunk_get_last_contact_point(c)} {}
            float bounce() const { return state.world.physical_object_get_bounce(o); }
            float friction() const { return mathf::clamp(state.world.physical_object_get_surface_friction(o) * 2.f, 0.f, 1.f); }
            float threshold() const { return state.world.physical_object_get_impact_threshold(o); }
        };
    }

    // BodyChunk.CheckVerticalCollision (TerrainManager/sand terrain not ported)
    static void check_vertical_collision(chunk_ref& ch, room_view const& room, impact_sink const& impact) {
        auto& state = ch.state;
        float const tr = terrain_rad(state, ch.c);
        float const bounce = ch.bounce();
        float const friction = ch.friction();
        float const threshold = ch.threshold();
        ch.contact.y = 0;
        ivec2 const tile_pos = room_view::get_tile_position(ch.last_pos);
        bool const last_in_solid = room.get_tile(ch.last_pos).t == terrain::solid;
        int32_t num = 0;
        if (ch.pos.y > ch.last_pos.y) {
            int32_t const y_to = room_view::get_tile_position(vec2{0.f, ch.pos.y + tr + 0.01f}).y;
            int32_t const y_from = room_view::get_tile_position(vec2{0.f, ch.last_pos.y + tr}).y;
            int32_t const x_from = room_view::get_tile_position(vec2{ch.pos.x - tr + 1.f, 0.f}).x;
            int32_t const x_to = room_view::get_tile_position(vec2{ch.pos.x + tr - 1.f, 0.f}).x;
            bool hit = false;
            for (int32_t i = y_from; i <= y_to && !hit; ++i) {
                for (int32_t x = x_from; x <= x_to && !hit; ++x) {
                    if (room.get_terrain(x, i) == terrain::solid && room.get_terrain(x, i - 1) != terrain::solid
                    && (tile_pos.y < i || last_in_solid)) {
                        ch.pos.y = float(i) * 20.f - tr;
                        if (ch.vel.y > threshold)
                            impact(state, ch.c, ivec2{0, 1}, std::fabs(ch.vel.y), ch.last_contact.y < 1);
                        ch.contact.y = 1;
                        ch.vel.y = -std::fabs(ch.vel.y) * bounce;
                        if (std::fabs(ch.vel.y) < 1.f + 9.f * (1.f - bounce))
                            ch.vel.y = 0.f;
                        ch.vel.x *= friction;
                        hit = true;
                    }
                    if (++num > max_repeats)
                        hit = true;
                }
            }
        } else if (ch.pos.y < ch.last_pos.y) {
            int32_t const y_to = room_view::get_tile_position(vec2{0.f, ch.pos.y - tr - 0.01f}).y;
            int32_t const y_from = room_view::get_tile_position(vec2{0.f, ch.last_pos.y - tr}).y;
            int32_t const x_from = room_view::get_tile_position(vec2{ch.pos.x - tr + 1.f, 0.f}).x;
            int32_t const x_to = room_view::get_tile_position(vec2{ch.pos.x + tr - 1.f, 0.f}).x;
            bool hit = false;
            for (int32_t i = y_from; i >= y_to && !hit; --i) {
                for (int32_t x = x_from; x <= x_to && !hit; ++x) {
                    if (solid_floor(state, room, ch.c, x, i) && !solid_floor(state, room, ch.c, x, i + 1)
                    && (tile_pos.y > i || last_in_solid)) {
                        ch.pos.y = (float(i) + 1.f) * 20.f + tr;
                        if (ch.vel.y < -threshold)
                            impact(state, ch.c, ivec2{0, -1}, std::fabs(ch.vel.y), ch.last_contact.y > -1);
                        ch.contact.y = -1;
                        ch.vel.y = std::fabs(ch.vel.y) * bounce;
                        if (ch.vel.y < gravity(state, ch.o, room) || ch.vel.y < 1.f + 9.f * (1.f - bounce))
                            ch.vel.y = 0.f;
                        ch.vel.x *= friction;
                        hit = true;
                    }
                    if (++num > max_repeats)
                        hit = true;
                }
            }
        }
    }

    // BodyChunk.checkAgainstSlopesVertically
    static void check_against_slopes_vertically(chunk_ref& ch, room_view const& room) {
        using world::slope_direction;
        auto& state = ch.state;
        float const surface_friction = state.world.physical_object_get_surface_friction(ch.o);
        vec2& pos = ch.pos;
        vec2& vel = ch.vel;
        float& slope_rad = state.world.body_chunk_get_slope_rad(ch.c);
        ivec2 const tp = room_view::get_tile_position(pos);
        ivec2 b{0, 0};
        slope_direction a = room.identify_slope(pos);
        if (room.get_tile(pos).t != terrain::slope) {
            vec2 const mid = room_view::middle_of_tile(pos);
            if (room.identify_slope(tp.x - 1, tp.y) != slope_direction::broken && pos.x - slope_rad <= mid.x - 10.f) {
                a = room.identify_slope(tp.x - 1, tp.y);
                b.x = -1;
            } else if (room.identify_slope(tp.x + 1, tp.y) != slope_direction::broken && pos.x + slope_rad >= mid.x + 10.f) {
                a = room.identify_slope(tp.x + 1, tp.y);
                b.x = 1;
            } else if (pos.y - slope_rad < mid.y - 10.f) {
                if (room.identify_slope(tp.x, tp.y - 1) != slope_direction::broken) {
                    a = room.identify_slope(tp.x, tp.y - 1);
                    b.y = -1;
                }
            } else if (pos.y + slope_rad > mid.y + 10.f && room.identify_slope(tp.x, tp.y + 1) != slope_direction::broken) {
                a = room.identify_slope(tp.x, tp.y + 1);
                b.y = 1;
            }
        }
        if (a == slope_direction::broken)
            return;
        vec2 const v = room_view::middle_of_tile(room_view::get_tile_position(pos) + b);
        int32_t dir = 0;
        float surface_y;
        int32_t side;
        if (a == slope_direction::up_left) {
            dir = -1;
            surface_y = pos.x - (v.x - 10.f) + (v.y - 10.f);
            side = -1;
        } else if (a == slope_direction::up_right) {
            dir = 1;
            surface_y = 20.f - (pos.x - (v.x - 10.f)) + (v.y - 10.f);
            side = -1;
        } else if (a == slope_direction::down_left) {
            surface_y = 20.f - (pos.x - (v.x - 10.f)) + (v.y - 10.f);
            side = 1;
        } else {
            surface_y = pos.x - (v.x - 10.f) + (v.y - 10.f);
            side = 1;
        }
        if (side == -1 && pos.y <= surface_y + slope_rad + slope_rad) {
            pos.y = surface_y + slope_rad + slope_rad;
            ch.contact.y = -1;
            vel.x *= 1.f - surface_friction;
            vel.x += std::fabs(vel.y) * mathf::clamp(0.5f - surface_friction, 0.f, 0.5f) * float(dir) * 0.2f;
            vel.y = 0.f;
            state.world.body_chunk_set_on_slope(ch.c, int8_t(dir));
            slope_rad = terrain_rad(state, ch.c) - 1.f;
        } else if (side == 1 && pos.y >= surface_y - slope_rad - slope_rad) {
            pos.y = surface_y - slope_rad - slope_rad;
            ch.contact.y = 1;
            vel.y = 0.f;
            vel.x *= 1.f - surface_friction;
            slope_rad = terrain_rad(state, ch.c) - 1.f;
        }
    }

    // BodyChunk.CheckHorizontalCollision
    static void check_horizontal_collision(chunk_ref& ch, room_view const& room, impact_sink const& impact) {
        auto& state = ch.state;
        float const tr = terrain_rad(state, ch.c);
        float const bounce = ch.bounce();
        float const friction = ch.friction();
        float const threshold = ch.threshold();
        ch.contact.x = 0;
        ivec2 const tile_pos = room_view::get_tile_position(ch.last_pos);
        bool const last_in_solid = room.get_tile(ch.last_pos).t == terrain::solid;
        int32_t num = 0;
        if (ch.vel.x > 0.f) {
            int32_t const x_to = room_view::get_tile_position(vec2{ch.pos.x + tr + 0.01f, 0.f}).x;
            int32_t const x_from = room_view::get_tile_position(vec2{ch.last_pos.x + tr, 0.f}).x;
            int32_t const y_to = room_view::get_tile_position(vec2{0.f, ch.pos.y + tr - 1.f}).y;
            int32_t const y_from = room_view::get_tile_position(vec2{0.f, ch.pos.y - tr + 1.f}).y;
            bool hit = false;
            for (int32_t i = x_from; i <= x_to && !hit; ++i) {
                for (int32_t y = y_from; y <= y_to && !hit; ++y) {
                    if (room.get_terrain(i, y) == terrain::solid && room.get_terrain(i - 1, y) != terrain::solid
                    && (tile_pos.x < i || last_in_solid)) {
                        ch.pos.x = float(i) * 20.f - tr;
                        if (ch.vel.x > threshold)
                            impact(state, ch.c, ivec2{1, 0}, std::fabs(ch.vel.x), ch.last_contact.x < 1);
                        ch.contact.x = 1;
                        ch.vel.x = -std::fabs(ch.vel.x) * bounce;
                        if (std::fabs(ch.vel.x) < 1.f + 9.f * (1.f - bounce))
                            ch.vel.x = 0.f;
                        ch.vel.y *= friction;
                        hit = true;
                    }
                    if (++num > max_repeats)
                        hit = true;
                }
            }
        } else if (ch.vel.x < 0.f) {
            int32_t const x_to = room_view::get_tile_position(vec2{ch.pos.x - tr - 0.01f, 0.f}).x;
            int32_t const x_from = room_view::get_tile_position(vec2{ch.last_pos.x - tr, 0.f}).x;
            int32_t const y_to = room_view::get_tile_position(vec2{0.f, ch.pos.y + tr - 1.f}).y;
            int32_t const y_from = room_view::get_tile_position(vec2{0.f, ch.pos.y - tr + 1.f}).y;
            bool hit = false;
            for (int32_t i = x_from; i >= x_to && !hit; --i) {
                for (int32_t y = y_from; y <= y_to && !hit; ++y) {
                    if (room.get_terrain(i, y) == terrain::solid && room.get_terrain(i + 1, y) != terrain::solid
                    && (tile_pos.x > i || last_in_solid)) {
                        ch.pos.x = (float(i) + 1.f) * 20.f + tr;
                        if (ch.vel.x < -threshold)
                            impact(state, ch.c, ivec2{-1, 0}, std::fabs(ch.vel.x), ch.last_contact.x > -1);
                        ch.contact.x = -1;
                        ch.vel.x = std::fabs(ch.vel.x) * bounce;
                        if (std::fabs(ch.vel.x) < 1.f + 9.f * (1.f - bounce))
                            ch.vel.x = 0.f;
                        ch.vel.y *= friction;
                        hit = true;
                    }
                    if (++num > max_repeats)
                        hit = true;
                }
            }
        }
    }

    void update_chunk(sys::game_state& state, room_view const& room, dcon::body_chunk_id c, terrain_impact_fn on_impact, void* user) {
        chunk_ref ch(state, c);
        auto const o = ch.o;
        impact_sink const impact{on_impact, user};

        if (std::isnan(ch.vel.y))
            ch.vel.y = 0.f;
        if (std::isnan(ch.vel.x))
            ch.vel.x = 0.f;
        ch.vel.y -= gravity(state, o, room);

        float const rad = state.world.body_chunk_get_rad(c);
        float const air_friction = state.world.physical_object_get_air_friction(o);
        bool const touches_water = ch.pos.y - rad <= room.get_float_water_level(ch.pos);
        if (room.water && touches_water) {
            float const sub = submersion(state, room, c);
            // skipping across the surface
            if (ch.vel.x > ch.vel.y * 5.f && (std::fabs(ch.vel.x) > 10.f && ch.vel.y < 0.f) && sub < 0.5f) {
                ch.vel.y *= -0.5f;
                ch.vel.x *= 0.75f;
            } else {
                ch.vel.y += state.world.physical_object_get_buoyancy(o) * room.gravity * sub;
                float const wf = state.world.physical_object_get_water_friction(o);
                ch.vel *= mathf::lerp(air_friction,
                    mathf::lerp(wf * state.world.physical_object_get_water_retardation_immunity(o), wf, std::pow(1.f / mathf::max(1.f, rw::magnitude(ch.vel) - 10.f), 0.5f)),
                    sub);
            }
        } else {
            ch.vel *= air_friction;
        }

        state.world.body_chunk_set_last_last_pos(c, ch.last_pos);
        ch.last_pos = ch.pos;
        ch.pos += ch.vel;

        state.world.body_chunk_set_on_slope(c, 0);
        state.world.body_chunk_set_slope_rad(c, terrain_rad(state, c));
        ch.last_contact = ch.contact;
        if (state.world.body_chunk_get_collide_with_terrain(c)) {
            check_vertical_collision(ch, room, impact);
            if (state.world.body_chunk_get_collide_with_slopes(c))
                check_against_slopes_vertically(ch, room);
            check_horizontal_collision(ch, room, impact);
        } else {
            ch.contact = ivec2{0, 0};
        }

        if (state.world.physical_object_get_grabbed_by_count(o) == 0) {
            float const range = state.world.body_chunk_get_restrict_in_room_range(c);
            if (ch.pos.x < -range) {
                ch.vel.x = 0.f;
                ch.pos.x = -range;
            } else if (ch.pos.x > room.pixel_width() + range) {
                ch.vel.x = 0.f;
                ch.pos.x = room.pixel_width() + range;
            }
            if (ch.pos.y < -range) {
                ch.vel.y = 0.f;
                ch.pos.y = -range;
            } else if (ch.pos.y > room.pixel_height() + range) {
                ch.vel.y = 0.f;
                ch.pos.y = room.pixel_height() + range;
            }
        }
    }

    void update_connection(sys::game_state& state, dcon::chunk_connection_id k) {
        if (!state.world.chunk_connection_get_active(k))
            return;
        auto const c1 = state.world.chunk_connection_get_chunk1(k);
        auto const c2 = state.world.chunk_connection_get_chunk2(k);
        vec2& p1 = state.world.body_chunk_get_pos(c1);
        vec2& p2 = state.world.body_chunk_get_pos(c2);
        float const d = state.world.chunk_connection_get_distance(k);
        float const num = rw::distance(p1, p2);
        auto const t = state.world.chunk_connection_get_type(k);
        if (t == connection_type::normal || (t == connection_type::pull && num > d) || (t == connection_type::push && num < d)) {
            vec2 const a = rw::custom::dir_vec(p1, p2);
            float const ws = state.world.chunk_connection_get_weight_symmetry(k);
            float const el = state.world.chunk_connection_get_elasticity(k);
            vec2 const d1 = a * ((d - num) * ws * el);
            vec2 const d2 = a * ((d - num) * (1.f - ws) * el);
            p1 -= d1;
            state.world.body_chunk_get_vel(c1) -= d1;
            p2 += d2;
            state.world.body_chunk_get_vel(c2) += d2;
        }
    }

    void update_object(sys::game_state& state, room_view const& room, dcon::physical_object_id object, terrain_impact_fn on_impact, void* user) {
        // chunks in BodyChunk.index order, like PhysicalObject.bodyChunks
        auto const range = state.world.physical_object_range_of_chunk_ownership(object);
        uint32_t const n = uint32_t(range.second - range.first);
        for (uint32_t i = 0; i < n; ++i)
            update_chunk(state, room, chunk(state, object, i), on_impact, user);
        state.world.physical_object_for_each_chunk_connection_as_owner(object, [&](dcon::chunk_connection_id k) {
            update_connection(state, k);
        });
    }
}
