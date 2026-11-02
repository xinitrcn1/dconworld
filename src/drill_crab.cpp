// Watcher/DrillCrab.cs, DrillCrabAI.cs, DrillCrabPather.cs, DrillCrabState.cs:
// the drill crab (and the tower crab). See drill_crab.hpp.

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <source_location>
#include <string>
#include <vector>

#include "drill_crab.hpp"
#include "barnacle.hpp"
#include "dens.hpp"
#include "drill_crab_graphics.hpp"
#include "drill_crab_types.hpp"
#include "particles.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "rooms.hpp"
#include "scavenger.hpp"
#include "tracker.hpp"

namespace drill_crab {
    using creature::relationship;
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // CreatureTemplate (StaticWorld.InitDrillCrab, InitTowerCrab):
        // baseDamageResistance (the tower crab's 3.5), baseStunResistance,
        // visualRadius (instantDeathDamageLimit is left at its default: none)
        constexpr float base_stun_resistance = 1.5f;
        constexpr float visual_radius = 600.f;
        float base_damage_resistance(variant v) { return v == variant::tower ? 3.5f : 2.5f; }

        // SmallLeg / TowerLeg: segmentLengths and the base length
        constexpr float small_lengths[small_leg_segments] = {45.f, 45.f, 50.f, 58.f, 35.f};
        constexpr float tower_lengths[tower_leg_segments] = {55.f, 120.f, 120.f, 120.f, 70.f, 35.f};

        creature::drill_crab_payload& pl(sys::game_state& state, dcon::creature_id c) { return state.world.creature_get_payload(c).drill_crab; }
        creature::drill_crab_payload const& pl(sys::game_state const& state, dcon::creature_id c) { return state.world.creature_get_payload(c).drill_crab; }
        dcon::drill_crab_data_id data(dcon::data_container const& w, dcon::creature_id c) {
            return dcon::drill_crab_data_id{dcon::drill_crab_data_id::value_base_t(w.creature_get_payload(c).drill_crab.data)};
        }
        ivec2 tile_of(vec2 p) { return room_view::get_tile_position(p); }

        // Random.Range(float, float)
        float frange(rw::rng& r, float lo, float hi) { return lo + (hi - lo) * r.value(); }

        // Custom.RotateAroundOrigo (clockwise degrees), RotateAroundVector
        vec2 rotate_around_origo(vec2 v, float deg) {
            float const a = deg * -0.017453292f;
            float const c = std::cos(a), s = std::sin(a);
            return vec2{c * v.x - s * v.y, s * v.x + c * v.y};
        }
        vec2 rotate_around_vector(vec2 v, vec2 pivot, float deg) { return rotate_around_origo(v - pivot, deg) + pivot; }
        // Vector2.SignedAngle
        float signed_angle(vec2 from, vec2 to) { return rw::angle(from, to) * mathf::sign(from.x * to.y - from.y * to.x); }
        // Mathf.DeltaAngle
        float delta_angle(float current, float target) {
            float t = target - current;
            t = mathf::clamp(t - std::floor(t / 360.f) * 360.f, 0.f, 360.f);
            if (t > 180.f)
                t -= 360.f;
            return t;
        }
        // Mathf.SmoothStep
        float smooth_step(float from, float to, float t) {
            t = mathf::clamp01(t);
            t = -2.f * t * t * t + 3.f * t * t;
            return to * t + from * (1.f - t);
        }
        // Mathf.MoveTowards
        float move_towards(float cur, float target, float delta) {
            if (std::fabs(target - cur) <= delta)
                return target;
            return cur + mathf::sign(target - cur) * delta;
        }
        // Custom.PushFromHalf
        float push_from_half(float v, float e) {
            if (v == 0.5f)
                return 0.5f;
            if (v < 0.5f)
                return custom::lerp_map(v, 0.f, 0.5f, 0.f, 0.5f, e);
            return custom::lerp_map(v, 1.f, 0.5f, 1.f, 0.5f, e);
        }

        // Room.RayTraceTilesForTerrain: no solid tile on the way
        bool ray_trace_tiles_for_terrain(room_view const& room, ivec2 a, ivec2 b) {
            int32_t num = std::abs(b.x - a.x);
            int32_t num2 = std::abs(b.y - a.y);
            int32_t x = a.x, y = a.y;
            int32_t i = std::min(1 + num + num2, 4096);
            int32_t const sx = b.x > a.x ? 1 : -1;
            int32_t const sy = b.y > a.y ? 1 : -1;
            int32_t e = num - num2;
            num *= 2;
            num2 *= 2;
            while (i > 0) {
                if (room.get_tile(x, y).solid())
                    return false;
                if (e > 0) {
                    x += sx;
                    e -= num2;
                } else {
                    y += sy;
                    e += num;
                }
                --i;
            }
            return true;
        }

        //
        // SharedPhysics.TerrainCollisionData: VerticalCollision,
        // SlopesVertically, HorizontalCollision (as deer.cpp's)
        //
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
        void slopes_vertically(room_view const& room, terrain_collision& cd) {
            using world::slope_direction;
            ivec2 const tp = room_view::get_tile_position(cd.pos);
            ivec2 b{0, 0};
            slope_direction a = room.identify_slope(cd.pos);
            vec2 const mid = room_view::middle_of_tile(cd.pos);
            if (room.get_tile(cd.pos).t != world::terrain::slope) {
                if (room.identify_slope(tp.x - 1, tp.y) != slope_direction::broken && cd.pos.x - cd.rad <= mid.x - 10.f) {
                    a = room.identify_slope(tp.x - 1, tp.y);
                    b.x = -1;
                } else if (room.identify_slope(tp.x + 1, tp.y) != slope_direction::broken && cd.pos.x + cd.rad >= mid.x + 10.f) {
                    a = room.identify_slope(tp.x + 1, tp.y);
                    b.x = 1;
                } else if (cd.pos.y - cd.rad < mid.y - 10.f) {
                    if (room.identify_slope(tp.x, tp.y - 1) != slope_direction::broken) {
                        a = room.identify_slope(tp.x, tp.y - 1);
                        b.y = -1;
                    }
                } else if (cd.pos.y + cd.rad > mid.y + 10.f && room.identify_slope(tp.x, tp.y + 1) != slope_direction::broken) {
                    a = room.identify_slope(tp.x, tp.y + 1);
                    b.y = 1;
                }
            }
            if (a == slope_direction::broken)
                return;
            vec2 const v = room_view::middle_of_tile(tp + b);
            float num;
            int32_t num2;
            if (a == slope_direction::up_left) {
                num = cd.pos.x - (v.x - 10.f) + (v.y - 10.f);
                num2 = -1;
            } else if (a == slope_direction::up_right) {
                num = 20.f - (cd.pos.x - (v.x - 10.f)) + (v.y - 10.f);
                num2 = -1;
            } else if (a == slope_direction::down_left) {
                num = 20.f - (cd.pos.x - (v.x - 10.f)) + (v.y - 10.f);
                num2 = 1;
            } else {
                num = cd.pos.x - (v.x - 10.f) + (v.y - 10.f);
                num2 = 1;
            }
            if (num2 == -1 && cd.pos.y <= num + cd.rad + cd.rad)
                cd.pos.y = num + cd.rad + cd.rad;
            else if (num2 == 1 && cd.pos.y >= num - cd.rad - cd.rad)
                cd.pos.y = num - cd.rad - cd.rad;
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

        // the closest distance between the segments a-b and c-d, and where
        // along each (s on a-b, t on c-d) (vulture.cpp's)
        float segment_distance(vec2 a, vec2 b, vec2 c, vec2 d, float& s, float& t) {
            vec2 const d1 = b - a, d2 = d - c, r = a - c;
            float const aa = rw::dot(d1, d1), ee = rw::dot(d2, d2), f = rw::dot(d2, r);
            if (aa <= 1e-6f && ee <= 1e-6f) {
                s = t = 0.f;
                return rw::distance(a, c);
            }
            if (aa <= 1e-6f) {
                s = 0.f;
                t = mathf::clamp01(f / ee);
            } else {
                float const cc = rw::dot(d1, r);
                if (ee <= 1e-6f) {
                    t = 0.f;
                    s = mathf::clamp01(-cc / aa);
                } else {
                    float const bb = rw::dot(d1, d2);
                    float const den = aa * ee - bb * bb;
                    s = den != 0.f ? mathf::clamp01((bb * f - cc * ee) / den) : 0.f;
                    t = (bb * s + f) / ee;
                    if (t < 0.f) {
                        t = 0.f;
                        s = mathf::clamp01(-cc / aa);
                    } else if (t > 1.f) {
                        t = 1.f;
                        s = mathf::clamp01((bb - cc) / aa);
                    }
                }
            }
            return rw::distance(a + d1 * s, c + d2 * t);
        }

        // DrillCrab.GetNearbyTiles: the offsets within `r`, closest first
        std::vector<ivec2> make_nearby(int32_t r) {
            std::vector<ivec2> out;
            for (int32_t i = -r; i <= r; ++i)
                for (int32_t j = -r; j <= r; ++j)
                    if (rw::magnitude(vec2{float(j), float(i)}) <= float(r))
                        out.push_back(ivec2{j, i});
            std::stable_sort(out.begin(), out.end(), [](ivec2 a, ivec2 b) { return rw::magnitude(rw::to_vec2(a)) < rw::magnitude(rw::to_vec2(b)); });
            return out;
        }
        std::vector<ivec2> const& nearby_tiles(bool tower) {
            static std::vector<ivec2> const small = make_nearby(2);
            static std::vector<ivec2> const big = make_nearby(4);
            return tower ? big : small;
        }

        // DrillCrab.Leg's constants by variant
        struct leg_consts {
            float base_length;
            float const* lengths;
            uint32_t segments;
            float accel;
            float max_speed;
            float separation;
        };
        constexpr float leg_drag = 0.95f;
        leg_consts consts_of(variant v) {
            if (v == variant::tower)
                return leg_consts{450.f, tower_lengths, tower_leg_segments, 0.05f, 50.f, 9.f};
            return leg_consts{185.f, small_lengths, small_leg_segments, 0.2f, 25.f, 18.f};
        }
        // DrillCrab.LegLengthFactor
        float leg_length_factor(variant v, individual_variations const& iv) { return mathf::lerp(v == variant::tower ? 0.6f : 0.8f, 1.2f, iv.leg_length); }
        // the leg's side (-1 .. 1) and lengthFac (the constructor's)
        float side_of(uint32_t i) { return float(i) / float(leg_count - 1) * 2.f - 1.f; }
        float length_fac_of(variant v, individual_variations const& iv, uint32_t i) {
            return (1.2f - std::fabs(side_of(i)) * 0.2f) * leg_length_factor(v, iv);
        }

        //
        // The update context
        //
        struct ctx {
            sys::game_state& state;
            dcon::creature_id c;
            room_view const& room;
            creature::drill_crab_payload& p;
            dcon::drill_crab_data_id d;
            body_state& b;
            ai_state& ai;
            individual_variations const& iv;
            rw::rng rng;
            dcon::body_chunk_id ch[2];
            dcon::creature_part_id legs[leg_count];
            leg_consts lc;
            std::optional<pathing::field_id> field;

            ctx(sys::game_state& s, dcon::creature_id cc, room_view const& r, std::source_location const l = std::source_location::current())
                : state(s), c(cc), room(r), p(pl(s, cc)), d(data(s.world, cc)), b(s.world.drill_crab_data_get_body(data(s.world, cc))),
                  ai(s.world.drill_crab_data_get_ai(data(s.world, cc))), iv(s.world.drill_crab_data_get_ivars(data(s.world, cc))),
                  rng(rw::random(s.seed, s.tick, uint32_t(cc.value), l)), lc(consts_of(pl(s, cc).variant)) {
                for (uint32_t i = 0; i < 2; ++i)
                    ch[i] = creature::chunk(s, cc, i);
                uint32_t k = 0;
                for (auto const o : creature::parts(s.world, cc)) {
                    auto const part = s.world.part_ownership_get_part(o);
                    if (s.world.creature_part_get_kind(part) == creature::part_kind::drill_crab_leg && k < leg_count)
                        legs[k++] = part;
                }
            }
            dcon::data_container& w() { return state.world; }
            bool tower() const { return p.variant == variant::tower; }
            vec2& pos(uint32_t i) { return state.world.body_chunk_get_pos(ch[i]); }
            vec2& last_pos(uint32_t i) { return state.world.body_chunk_get_last_pos(ch[i]); }
            vec2& vel(uint32_t i) { return state.world.body_chunk_get_vel(ch[i]); }
            float mass(uint32_t i) { return state.world.body_chunk_get_mass(ch[i]); }
            float rad(uint32_t i) { return state.world.body_chunk_get_rad(ch[i]); }
            dcon::physical_object_id obj() { return creature::body(state, c); }
            // abstractCreature.pos
            ivec2 here() { return tile_of(pos(0)); }
            bool consious() { return creature::consious(state, c); }
            float health() { return state.world.creature_get_health(c); }
            ivec2 destination() { return state.world.creature_get_destination(c); }
            void set_destination(ivec2 t) {
                if (destination() != t) {
                    state.world.creature_set_destination(c, t);
                    field.reset();
                }
            }
            pathing::field_id path() {
                if (!field)
                    field = pathing::field_to(state, room.id, destination(), movement);
                return *field;
            }
            int32_t proximity(ivec2 t) { return pathing::ai_terrain_proximity(state, room.id, t); }
            // AImap.TileAccessibleToCreature for a drill crab: open air (its
            // only accessibility) not too close to the terrain
            // (IsTooCloseToTerrain: 2 tiles, the tower crab's 3) with a floor
            // at most 8 (18) tiles under it or a column or three beside
            bool accessible(ivec2 t) {
                if (!room.inside(t.x, t.y) || room.get_tile(t).solid())
                    return false;
                if (proximity(t) < (tower() ? 3 : 2))
                    return false;
                int32_t const reach = tower() ? 3 : 1;
                int32_t const max_alt = tower() ? 18 : 8;
                for (int32_t i = -reach; i <= reach; ++i)
                    if (t.x + i >= 0 && t.x + i < room.width && pathing::floor_altitude(room, ivec2{t.x + i, t.y}) <= max_alt)
                        return true;
                return false;
            }
            // PathFinder.CoordinateReachableAndGetbackable (every open tile
            // connects both ways: in reach of the destination's field)
            bool reachable(ivec2 t) { return accessible(t) && pathing::distance(state, path(), t) != pathing::unreachable; }
            // StuckTracker.Utility
            float stuck() { return mathf::inverse_lerp(100.f, 200.f, float(ai.stuck_counter)); }
            bool hiding() const { return p.hide_timer > 0; }
            bool spinning_out() const { return p.spin_out_timer > 0 && p.spin_out_timer < 200; }
            bool kneeling() const { return ai.kneel_timer > 0; }

            // the legs
            creature::drill_crab_leg_payload& leg(uint32_t i) { return state.world.creature_part_get_payload(legs[i]).drill_crab_leg; }
            segment* segs(uint32_t i) { return b.segments[i]; }
            segment& tip(uint32_t i) { return b.segments[i][lc.segments - 1]; }
            float leg_health(uint32_t i) { return legs[i] ? state.world.creature_part_get_health(legs[i]) : 0.f; }
            void set_leg_health(uint32_t i, float h) {
                if (legs[i])
                    state.world.creature_part_set_health(legs[i], h);
            }
            bool leg_dead(uint32_t i) { return leg_health(i) <= 0.f; }
            float length_fac(uint32_t i) { return length_fac_of(p.variant, iv, i); }
            float max_length(uint32_t i) { return lc.base_length * length_fac(i); }
            float prev_dist(uint32_t i, uint32_t s) { return lc.lengths[s] * length_fac(i); }
            uint32_t legs_supporting() {
                uint32_t n = 0;
                for (uint32_t i = 0; i < leg_count; ++i)
                    if (legs[i] && leg(i).supporting)
                        ++n;
                return n;
            }
        };

        //
        // DrillCrab.Leg
        //
        // IdealAngle, CurrentAngle, WantsToStep, Limp
        float ideal_angle(ctx& x, uint32_t i) { return -x.lc.separation * side_of(i) - 15.f * x.p.target_flip; }
        float current_angle(ctx& x, uint32_t i) { return -signed_angle(vec2{0.f, -1.f}, x.tip(i).pos - x.pos(0)); }
        bool wants_to_step(ctx& x, uint32_t i) { return std::fabs(delta_angle(ideal_angle(x, i), current_angle(x, i))) > 15.f || !x.leg(i).supporting; }
        bool limp(ctx& x, uint32_t i) { return x.leg(i).mode == leg_mode::retracting || x.leg_dead(i); }

        void switch_mode(ctx& x, uint32_t i, leg_mode m) {
            auto& l = x.leg(i);
            if (m != l.mode && l.mode == leg_mode::retracting)
                l.flatten = 1.f;
            l.mode = m;
            l.supporting = 0;
            l.segments_retracted = 0;
            l.scan_time = 0;
            l.scan_from = vec2{0.f, 0.f};
        }
        void start_step(ctx& x, uint32_t i) {
            if (x.leg_dead(i))
                return;
            switch_mode(x, i, leg_mode::scanning);
            x.leg(i).scan_from = x.b.next_step_pos;
        }
        void set_target(ctx& x, uint32_t i, vec2 at) {
            if (x.leg_dead(i))
                return;
            switch_mode(x, i, leg_mode::seeking);
            x.leg(i).target_pos = at;
        }
        void force_attach(ctx& x, uint32_t i, vec2 at) {
            set_target(x, i, at);
            x.tip(i).pos = at;
            x.leg(i).supporting = 1;
        }
        // Leg.Retract (GetNormal never finds a side: the tip stops)
        void retract(ctx& x, uint32_t i) {
            if (x.leg_dead(i))
                return;
            switch_mode(x, i, leg_mode::retracting);
            x.tip(i).vel = vec2{0.f, 0.f};
            x.leg(i).target_pos = vec2{0.f, 0.f};
        }
        void reset_physics(ctx& x, uint32_t i) {
            vec2 const at = x.pos(0);
            for (uint32_t s = 0; s < x.lc.segments; ++s) {
                x.segs(i)[s].pos = at;
                x.segs(i)[s].last_pos = at;
                x.segs(i)[s].vel = vec2{0.f, 0.f};
            }
            auto& l = x.leg(i);
            l.anchor = at;
            l.last_anchor = at;
            l.supporting = 0;
            start_step(x, i);
        }
        // Leg.ApplyForce on a segment (-1: the body chunk)
        void leg_apply_force(ctx& x, uint32_t i, int32_t s, vec2 momentum) {
            if (s < 0) {
                x.vel(0) += momentum / x.mass(0);
                return;
            }
            if (uint32_t(s) < x.lc.segments)
                x.segs(i)[s].vel += momentum * 2.f;
        }

        // Leg.InverseKinematic
        vec2 inverse_kinematic(vec2 shoulder, vec2 hand, float upper, float lower, float flip) {
            float const num = rw::distance(shoulder, hand);
            float const ratio = num > 1e-5f ? (num * num + upper * upper - lower * lower) / (2.f * num * upper) : 1.f;
            float const f = std::acos(mathf::clamp(ratio, -1.f, 1.f));
            vec2 const n = rw::normalized(hand - shoulder);
            float const c = std::cos(f);
            float const s = std::sin(f) * flip;
            return vec2{n.x * c + n.y * s, n.y * c - n.x * s} * upper + shoulder;
        }
        // SmallLeg / TowerLeg.DoInverseKinematics
        void do_inverse_kinematics(ctx& x, uint32_t i, float flip, float extended) {
            segment* s = x.segs(i);
            vec2 const anchor = x.leg(i).anchor;
            vec2 const tip = x.tip(i).pos;
            float const lf = x.length_fac(i);
            auto const pd = [&](uint32_t k) { return x.prev_dist(i, k); };
            if (x.tower()) {
                s[2].pos = inverse_kinematic(anchor, tip, (100.f + 150.f * extended) * lf, (125.f + 70.f * mathf::pow(extended, 0.2f) + 25.f * extended) * lf, -flip);
                s[1].pos = inverse_kinematic(anchor, s[2].pos, (100.f + 50.f * extended) * lf, pd(2), -flip);
                s[0].pos = inverse_kinematic(anchor, s[1].pos, pd(0), pd(1), -flip);
                s[3].pos = inverse_kinematic(s[2].pos, tip, pd(3), (85.f + 15.f * extended) * lf, -flip);
                s[4].pos = inverse_kinematic(s[3].pos, tip, pd(4), pd(5), flip);
            } else {
                s[2].pos = inverse_kinematic(anchor, tip, lf * 97.f * (extended * 0.8f + 0.2f), lf * 92.f * (extended * 0.5f + 0.5f), -flip);
                s[1].pos = inverse_kinematic(anchor, s[2].pos, lf * 52.f * (extended * 0.5f + 0.5f), pd(2), -flip);
                s[0].pos = inverse_kinematic(anchor, s[1].pos, pd(0), pd(1), -flip);
                s[3].pos = inverse_kinematic(s[2].pos, tip, pd(3), pd(4), flip);
            }
        }
        // Leg.LandOnGround (its step sounds aren't ported)
        void land_on_ground(ctx& x, uint32_t i) {
            x.tip(i).vel = vec2{0.f, 0.f};
            x.leg(i).supporting = 1;
        }
        // Leg.AnimateInverseKinematics (the culled shortcut, a straight line of
        // segments when no camera draws it, is left out: the simulation
        // doesn't know of graphics, the bones are always solved)
        void animate_inverse_kinematics(ctx& x, uint32_t i, vec2 aim) {
            auto& l = x.leg(i);
            segment& tp = x.tip(i);
            float const maxl = x.max_length(i);
            if (rw::distance(tp.pos, l.anchor) > maxl) {
                tp.pos = rw::normalized(tp.pos - l.anchor) * maxl + l.anchor;
                l.supporting = 0;
            }
            if (l.supporting) {
                tp.vel = vec2{0.f, 0.f};
            } else {
                vec2 const n = rw::normalized(tp.pos - x.pos(0));
                vec2 const a = custom::perpendicular_vector(n);
                float const num = rw::distance(tp.pos, x.pos(0));
                float const num2 = rw::distance(aim, x.pos(0));
                float const num3 = signed_angle(n, aim - x.pos(0));
                float const num4 = custom::lerp_map(std::fabs(num3), 45.f, 10.f, 100.f, num2);
                vec2 v = n * (mathf::clamp((num4 - num) / 20.f, -1.f, 1.f) * x.lc.max_speed);
                v += a * (mathf::clamp(num3 / 45.f, -1.f, 1.f) * x.lc.max_speed);
                tp.vel = rw::lerp(tp.vel, v, x.lc.accel);
                tp.pos += tp.vel;
                tp.vel *= leg_drag;
                // (the Watcher's TerrainManager terrain isn't ported: tiles only)
                if (x.room.get_tile(tp.pos).solid() && num4 == num2)
                    land_on_ground(x, i);
            }
            float const num5 = custom::s_curve(l.flatten, 2.f);
            float const flip = std::sin((side_of(i) * 0.4f + 0.75f * x.p.flip) * mathf::pi / 2.f) * (1.f - num5 * 0.8f);
            float const extended = rw::distance(tp.pos, l.anchor) / maxl;
            do_inverse_kinematics(x, i, flip, extended);
            for (uint32_t j = 0; j + 1 < x.lc.segments; ++j)
                x.segs(i)[j].vel = x.segs(i)[j].pos - x.segs(i)[j].last_pos;
        }
        // Leg.AnimatePhysics: a limp leg dangling
        void animate_physics(ctx& x, uint32_t i) {
            auto& l = x.leg(i);
            segment* s = x.segs(i);
            uint32_t const n = x.lc.segments;
            uint32_t const sr = std::min<uint32_t>(l.segments_retracted, n);
            l.supporting = 0;
            vec2 pos = l.anchor;
            for (uint32_t k = sr; k < n; ++k) {
                vec2 a = s[k].pos - pos;
                float const mag = rw::magnitude(a);
                a = mag > 1e-5f ? a / mag : vec2{0.f, 0.f};
                float const prev = x.prev_dist(i, k);
                float dd;
                if (mag > prev)
                    dd = mag - prev;
                else if (mag < prev / 2.f && (l.mode != leg_mode::retracting || k != sr))
                    dd = mag - prev / 2.f;
                else
                    dd = 0.f;
                s[k].pos -= a * dd * 0.25f;
                s[k].vel -= a * dd * 0.25f;
                if (k > sr) {
                    s[k - 1].pos += a * dd * 0.25f;
                    s[k - 1].vel += a * dd * 0.25f;
                }
                pos = s[k].pos;
            }
            pos = l.anchor;
            bool const awake = x.consious();
            for (uint32_t k = 0; k < n; ++k) {
                segment& g = s[k];
                if (k < sr) {
                    g.pos = l.anchor;
                    g.vel = x.vel(0);
                } else {
                    g.vel.y -= x.room.gravity;
                    g.pos += g.vel;
                    if (l.time_spent_retracting < 20 || !awake) {
                        terrain_collision cd{g.pos, g.last_pos, g.vel, 1.f};
                        vertical_collision(x.room, cd);
                        slopes_vertically(x.room, cd);
                        horizontal_collision(x.room, cd);
                        g.pos = cd.pos;
                        g.vel = cd.vel;
                    }
                    g.pos = rw::clamp_magnitude(g.pos - pos, x.prev_dist(i, k) * 1.5f) + pos;
                }
                g.vel *= 0.975f;
                pos = g.pos;
            }
        }
        // Leg.Update
        void leg_update(ctx& x, uint32_t i) {
            auto& l = x.leg(i);
            segment* s = x.segs(i);
            uint32_t const n = x.lc.segments;
            vec2 const vector = custom::dir_vec(x.pos(1), x.pos(0));
            float const side = side_of(i);
            l.last_anchor = l.anchor;
            l.anchor = x.pos(0) + vector * 10.f
                + custom::perpendicular_vector(x.pos(1), x.pos(0)) * ((mathf::pow(std::fabs(side), 0.5f) * 8.f + 4.f) * mathf::sign(side) + x.p.flip * 5.f);
            for (uint32_t k = 0; k < n; ++k)
                s[k].last_pos = s[k].pos;
            std::optional<vec2> aim;
            bool const dead = x.leg_dead(i);
            if (dead)
                l.mode = leg_mode::retracting;
            if (dead && (!x.tower() || l.segments_retracted > 1)) {
                if (!x.tower())
                    l.segments_retracted = 0;
                l.time_spent_retracting = 0;
            } else if (l.mode == leg_mode::seeking) {
                float const maxl = x.max_length(i);
                float const num = rw::distance(l.anchor, l.target_pos);
                if (num <= maxl) {
                    aim = l.target_pos;
                } else {
                    float const t = mathf::inverse_lerp(0.f, 80.f, num - maxl);
                    aim = (l.target_pos - l.anchor) / num * smooth_step(maxl, 80.f, t) + l.anchor;
                }
                if (l.supporting)
                    l.flatten = mathf::max(l.flatten - 0.033333335f, 0.f);
            } else if (l.mode == leg_mode::retracting) {
                if (l.segments_retracted < n && x.consious()) {
                    segment& g = s[l.segments_retracted];
                    g.vel += rw::normalized(l.anchor - g.pos) * 5.f;
                    g.vel = rw::lerp(g.vel, x.vel(0), 0.1f);
                    g.pos = rotate_around_vector(g.pos, l.anchor, signed_angle(vector, g.pos - l.anchor) * 0.25f);
                    if (custom::dist_less(g.pos, l.anchor, x.rad(0)))
                        ++l.segments_retracted;
                }
                if (l.time_spent_retracting < 30000)
                    ++l.time_spent_retracting;
            } else if (l.mode == leg_mode::scanning) {
                if (l.scan_time++ > 40)
                    start_step(x, i);
                aim = l.anchor + rotate_around_origo(vec2{0.f, -100.f}, ideal_angle(x, i));
                float const deg = ideal_angle(x, i) + frange(x.rng, -10.f, 10.f);
                vec2 const from = l.scan_from;
                if (auto const hit = scavenger::exact_terrain_ray_trace_pos(x.room, from, from + rotate_around_origo(vec2{0.f, -1.f} * x.max_length(i), deg)))
                    set_target(x, i, *hit);
            }
            if (limp(x, i)) {
                animate_physics(x, i);
                return;
            }
            if (aim)
                animate_inverse_kinematics(x, i, *aim);
        }

        // DrillCrab.ResetLegs (and the eyes, PlaceInRoom)
        void reset_legs(ctx& x) {
            x.b.next_step_pos = x.pos(0);
            for (uint32_t i = 0; i < leg_count; ++i) {
                if (!x.legs[i])
                    continue;
                reset_physics(x, i);
                force_attach(x, i, x.pos(0) - vec2{0.f, x.b.ideal_height});
            }
            for (auto& e : x.b.eyes) {
                e.pos = x.pos(0);
                e.last_pos = x.pos(0);
                e.vel = vec2{0.f, 0.f};
            }
        }

        //
        // DrillCrabAI
        //
        // Tracker(10, 5, 320, 0.3, 5, 5, 10), ThreatTracker(5)
        tracker::params const& tracker_params() {
            static tracker::params const p = [] {
                tracker::params q;
                q.see_around_corners = 10;
                q.frames_to_remember = 320;
                q.ghost_dismissal_range = 10;
                q.threat_max = 5;
                return q;
            }();
            return p;
        }
        struct hook_ctx {
            ctx& x;
        };
        // ArtificialIntelligence.VisualScore/VisualContact from the body
        bool visual_contact(ctx& x, vec2 look) {
            vec2 const eye = x.pos(0);
            if (!custom::dist_less(eye, look, visual_radius))
                return false;
            float score = mathf::inverse_lerp(visual_radius, 0.f, rw::distance(eye, look));
            if (pathing::narrow_space(x.room, tile_of(look)))
                score -= 0.5f;
            return score > 0.f && x.room.visual_contact(eye, look);
        }
        bool hook_sees(void* u, dcon::body_chunk_id k) {
            auto& h = *static_cast<hook_ctx*>(u);
            return visual_contact(h.x, h.x.state.world.body_chunk_get_pos(k));
        }
        bool hook_sees_point(void* u, vec2 p) { return visual_contact(static_cast<hook_ctx*>(u)->x, p); }
        // ModuleToTrackRelationship: Afraid the ThreatTracker, Uncomfortable
        // the DiscomfortTracker (not ported: the ThreatTracker too)
        creature::track_module hook_module(relationship r) {
            return r.type == creature::rel::afraid || r.type == creature::rel::uncomfortable ? creature::track_module::threat : creature::track_module::none;
        }
        bool hook_reach(void* u, ivec2 t) { return static_cast<hook_ctx*>(u)->x.reachable(t); }
        tracker::hooks make_hooks(hook_ctx& hx) {
            tracker::hooks h;
            h.user = &hx;
            h.sees = hook_sees;
            h.sees_point = hook_sees_point;
            h.module = hook_module;
            h.reach = hook_reach;
            return h;
        }

        // Creature.DangerPos of a tracked creature (where it was seen out of sight)
        vec2 danger_pos(ctx& x, creature::tracked const& r) {
            if (creature::here(x.state, r.who, x.room.id))
                return creature::main_pos(x.state, r.who);
            return r.last_seen;
        }

        // CreatureLooker.ReevaluateLookObject (CreatureInterestBonus: as is;
        // scaryness isn't ported: 1)
        void reevaluate_look(ctx& x) {
            auto& a = x.ai;
            auto const& ts = tracker::of(x.state, x.c);
            a.look_creature = dcon::creature_id{};
            float best = 0.f;
            bool const flag = x.rng.value() < 0.5f;
            ivec2 const at = x.here();
            auto const me = creature::template_of(x.state, x.c);
            for (uint32_t i = 0; i < ts.count; ++i) {
                auto const& r = ts.reps[i];
                if (!creature::live(x.state, r.who))
                    continue;
                auto const tt = creature::template_of(x.state, r.who);
                relationship const rel = creature::static_relationship(me, tt);
                float num2 = 1.f + mathf::lerp(creature::body_size(tt), 1.f, 0.5f) * rel.intensity * tracker::chance_of_finding(r, tracker_params().see_around_corners);
                if (flag)
                    num2 += x.rng.value();
                if (r.visual && creature::here(x.state, r.who, x.room.id))
                    num2 *= 1.f + rw::magnitude(x.w().body_chunk_get_vel(creature::chunk_of(x.state, r.who, creature::main_chunk_of(x.state, r.who))));
                num2 /= mathf::lerp(rw::magnitude(rw::to_vec2(tracker::best_guess(r) - at)), 1.f, 0.6f);
                if (rel.type == creature::rel::ignores)
                    num2 /= 2.f;
                if (num2 > best) {
                    best = num2;
                    a.look_creature = r.who;
                }
            }
        }
        // CreatureLooker.Update (chanceToLookAtNothing 0.1, 20 frames between
        // focus changes)
        void looker_update(ctx& x) {
            auto& a = x.ai;
            if (a.look_creature && !tracker::find(tracker::of(x.state, x.c), a.look_creature))
                a.look_creature = dcon::creature_id{};
            if (a.look_focus_delay > 0)
                --a.look_focus_delay;
            if (a.look_focus_delay < 1 && x.rng.value() < 1.f / 20.f) {
                a.look_focus_delay = 20;
                if (x.rng.value() > 0.1f)
                    reevaluate_look(x);
                else
                    a.look_creature = dcon::creature_id{};
            }
        }

        // StuckTracker.Update (trackPastPositions; the not following the
        // current goal counter never counts here: the effectual destination
        // is the destination)
        void stuck_update(ctx& x) {
            auto& a = x.ai;
            int32_t num = 0;
            ivec2 const here = x.here();
            if (!a.satisfied && custom::manhattan_distance(here, x.destination()) > 3) {
                a.past_head = uint8_t((a.past_head + stuck_positions - 1) % stuck_positions);
                a.past[a.past_head] = here;
                if (a.past_count < stuck_positions)
                    ++a.past_count;
                for (uint32_t j = 40; j < a.past_count; ++j)
                    if (custom::dist_less(rw::to_vec2(here), rw::to_vec2(a.past[(a.past_head + j) % stuck_positions]), 4.f))
                        ++num;
            }
            if (num > 30)
                ++a.stuck_counter;
            else
                a.stuck_counter -= 2;
            a.stuck_counter = custom::int_clamp(a.stuck_counter, 0, 200);
        }

        void hide(ctx& x, bool spin);

        // DrillCrabAI.Update
        void ai_update(ctx& x) {
            auto& a = x.ai;
            auto& w = x.w();
            hook_ctx hx{x};
            tracker::hooks const h = make_hooks(hx);
            ivec2 const here = x.here();
            tracker::update(x.state, x.c, x.room, here, tracker_params(), h);
            stuck_update(x);
            // UtilityComparer: the ThreatTracker (with the DiscomfortTracker's
            // creatures) smoothed, the RainTracker at weight 2
            float const threat = tracker::threat_utility(tracker::of(x.state, x.c));
            if (threat > a.threat_smoothed)
                a.threat_smoothed = mathf::lerp(a.threat_smoothed, threat, 0.05f);
            else
                a.threat_smoothed = move_towards(a.threat_smoothed, threat, 0.025f);
            float const rain = dens::rain_utility(x.state, x.c, x.room.id) * 2.f;
            bool const rain_highest = rain > a.threat_smoothed;
            float const highest = mathf::max(a.threat_smoothed, rain);
            looker_update(x);
            behavior b = behavior::idle;
            if (a.kneel_timer > 0)
                --a.kneel_timer;
            a.flee_from = dcon::creature_id{};
            creature::tracked const* flee = nullptr;
            if (highest > 0.1f) {
                if (rain_highest) {
                    b = behavior::escape_rain;
                } else if (auto const* t = tracker::threat(tracker::of(x.state, x.c)); t && creature::live(x.state, t->who)) {
                    flee = t;
                    a.flee_from = t->who;
                }
                if (flee) {
                    vec2 const dp = danger_pos(x, *flee);
                    bool const hid = x.hiding();
                    if (custom::dist_less(dp, x.pos(0), hid ? 275.f : 225.f) && std::fabs(dp.x - x.pos(0).x) < (hid ? 140.f : 120.f) && flee->visual && flee->age > 20)
                        b = behavior::hide;
                    else
                        b = behavior::flee;
                }
            }
            if (x.p.hide_timer > 0)
                b = behavior::hide;
            // (the squeal when it gets scared isn't ported)
            float to = 1.f;
            if ((b == behavior::hide || b == behavior::flee) && flee && flee->visual)
                to = custom::lerp_map(rw::distance(x.pos(0), danger_pos(x, *flee)), 250.f, 150.f, 0.75f, 0.25f);
            x.p.speed = custom::lerp_and_tick(x.p.speed, to, 0.05f, 0.05f);
            // (its abstract destination is always this room here: the tower
            // crab's walk to its den when idle, for a destination elsewhere,
            // never comes up)
            if (b == behavior::idle) {
                if (a.idle_timer <= 0) {
                    ivec2 iv{x.rng.range(20, 51), x.rng.range(-8, 9)};
                    if (x.rng.value() < 0.5f)
                        iv.x = -iv.x;
                    ivec2 coord;
                    if (x.pos(0).x < 0.f || x.pos(0).x > x.room.pixel_width() || x.rng.value() < 0.1f) {
                        int32_t const rx = x.rng.range(0, x.room.width);
                        coord = ivec2{rx, x.rng.range(0, x.room.height)};
                    } else {
                        coord = here + iv;
                    }
                    if (x.room.inside(coord.x, coord.y) && x.reachable(coord) && x.proximity(coord) >= 3) {
                        x.set_destination(coord);
                        a.idle_timer = x.rng.range(120, 480);
                    }
                } else {
                    --a.idle_timer;
                }
            } else if (b == behavior::flee) {
                x.set_destination(tracker::flee_to(x.state, x.c, x.room, here, 6, 30, tracker_params(), h));
            } else if (b == behavior::hide) {
                // Hide(spin) for the ThreatTracker's creature (what it fears,
                // not what it is only uncomfortable with)
                if (flee)
                    hide(x, flee->rel.type == creature::rel::afraid && x.p.hide_timer == 0);
            } else if (b == behavior::escape_rain) {
                if (auto const e = dens::home_tile(x.state, x.c, x.room))
                    x.set_destination(*e);
            }
            w.creature_set_behavior(x.c, uint8_t(b));
            w.creature_set_current_utility(x.c, highest);
            // kneeling to a more dominant crab facing it
            if (x.stuck() < 0.5f) {
                auto const me = creature::template_of(x.state, x.c);
                float const my_dom = w.drill_crab_data_get_personality(x.d).dominance;
                auto const& ts = tracker::of(x.state, x.c);
                for (uint32_t i = 0; i < ts.count; ++i) {
                    auto const& r = ts.reps[i];
                    if (!r.visual || !creature::is(w, r.who, creature::type::drill_crab) || !creature::here(x.state, r.who, x.room.id))
                        continue;
                    if (creature::template_of(x.state, r.who) != me)
                        continue;
                    auto const od = data(w, r.who);
                    if (!(w.drill_crab_data_get_personality(od).dominance > my_dom))
                        continue;
                    auto const& op = pl(x.state, r.who);
                    if (op.hide_timer > 0)
                        continue;
                    vec2 const opos = creature::main_pos(x.state, r.who);
                    if (std::fabs(x.pos(0).x + 100.f * x.p.flip - opos.x) < 150.f && mathf::sign(op.flip) != mathf::sign(x.p.flip))
                        a.kneel_timer = std::max(a.kneel_timer, x.rng.range(40, 80));
                }
            }
        }

        //
        // DrillCrab
        //
        // DrillCrab.Hide, Panic
        void hide(ctx& x, bool spin) {
            if (x.p.hide_timer >= 80)
                return;
            // (Drill_Crab_Enter_Shell isn't ported)
            if (spin) {
                x.p.hide_timer = 400;
                x.p.spin_out_timer = 260;
                return;
            }
            int32_t const r1 = x.rng.range(120, 400);
            int32_t const r2 = x.rng.range(120, 400);
            x.p.hide_timer = int16_t(std::min(r1, r2));
        }
        void panic(sys::game_state& state, dcon::creature_id c) {
            auto& p = pl(state, c);
            if (p.spin_out_timer > 0)
                return;
            p.hide_timer = 400;
            p.spin_out_timer = 200;
        }

        // DrillCrab.GetMoveTarget: idealHeight over the floor around `tile`,
        // as far up as it can go
        vec2 get_move_target(ctx& x, ivec2 tile) {
            int32_t const num = x.tower() ? 3 : 1;
            float num2 = 0.f;
            for (int32_t i = -num; i <= num; ++i) {
                ivec2 const t{custom::int_clamp(tile.x + i, 0, x.room.width - 1), custom::int_clamp(tile.y, 0, x.room.height - 1)};
                num2 += float(pathing::floor_altitude(x.room, t)) * 20.f - 10.f;
            }
            num2 /= float(2 * num + 1);
            float const num3 = (x.kneeling() ? 0.5f : 1.f) * custom::lerp_map(x.stuck(), 0.2f, 1.f, 1.f, 0.7f);
            ivec2 const tp = tile_of(room_view::middle_of_tile(tile) + vec2{0.f, x.b.ideal_height * num3 - num2});
            ivec2 const b{0, rw::math_sign(tp.y - tile.y)};
            for (int32_t num4 = 0; num4 < 18 && tile != tp; ++num4) {
                ivec2 const nt{custom::int_clamp(tile.x + b.x, 0, x.room.width - 1), custom::int_clamp(tile.y + b.y, 0, x.room.height - 1)};
                if (!x.accessible(nt))
                    break;
                tile += b;
            }
            return room_view::middle_of_tile(tile);
        }

        // DrillCrab.RefreshMoveTarget over the path field (DrillCrabPather's
        // FollowPath: the field's next step; the pathing cells' generations
        // aren't kept, the closest tile around with a way on is taken)
        void refresh_move_target(ctx& x) {
            x.p.move_target_age = 0;
            ivec2 const tile = x.here();
            ivec2 start = tile, dest = tile;
            auto const f = x.path();
            for (ivec2 const o : nearby_tiles(x.tower())) {
                ivec2 const s = tile + o;
                if (!x.room.inside(s.x, s.y) || x.room.get_tile(s).solid())
                    continue;
                if (auto const n = pathing::next_step(x.state, f, s)) {
                    start = s;
                    dest = *n;
                    break;
                }
            }
            x.b.next_step_pos = room_view::middle_of_tile(dest);
            if (x.proximity(tile) <= 2)
                x.p.squeeze = 1;
            ivec2 const goal = x.destination();
            for (int32_t i = 0; i < 16; ++i) {
                if (i < 4 && x.proximity(dest) <= 2)
                    x.p.squeeze = 1;
                auto const n = pathing::next_step(x.state, f, dest);
                if (!n || *n == goal)
                    break;
                dest = *n;
                if (std::fabs(x.pos(0).x - float(dest.x) * 20.f) > 120.f || !ray_trace_tiles_for_terrain(x.room, tile, dest))
                    break;
                // (vanilla compares the pixel position with the tile index)
                if (std::fabs(x.pos(0).x - float(dest.x)) < (x.tower() ? 80.f : 30.f))
                    x.b.next_step_pos = get_move_target(x, dest);
            }
            x.b.next_step_pos = x.pos(0) + rw::clamp_magnitude(x.b.next_step_pos - x.pos(0), rw::magnitude(x.vel(0)) * 20.f);
            x.b.move_target = dest.x == start.x ? room_view::middle_of_tile(dest) : get_move_target(x, dest);
        }

        // DrillCrab.FollowPath
        void follow_path(ctx& x) {
            int32_t const max_age = x.tower() ? 20 : 5;
            if (++x.p.move_target_age > max_age)
                refresh_move_target(x);
            vec2 const mt = x.b.move_target;
            if (mt == vec2{0.f, 0.f})
                return;
            if (x.here() != x.destination() || !custom::dist_less(x.pos(0), room_view::middle_of_tile(mt), 10.f)) {
                vec2 const v = custom::dir_vec(x.pos(0), mt) * (0.4f * x.p.speed * (1.f + x.stuck()));
                x.vel(0) += v;
                if (mt.y > x.pos(0).y && x.vel(0).y < -1.f)
                    x.vel(0).y *= 0.5f;
                x.ai.satisfied = 0;
            } else {
                x.ai.satisfied = 1;
            }
            if (std::fabs(mt.x - x.pos(0).x) > 35.f)
                x.p.target_flip = mt.x > x.pos(0).x ? 1.f : -1.f;
        }

        // DrillCrab.Act
        void act(ctx& x) {
            auto& p = x.p;
            if (x.ai.flee_from) {
                if (auto const* r = tracker::find(tracker::of(x.state, x.c), x.ai.flee_from))
                    p.target_flip = mathf::sign(room_view::middle_of_tile(tracker::best_guess(*r)).x - x.pos(0).x);
            }
            p.walk_anim += p.speed / 80.f;
            p.walk_anim = std::fmod(p.walk_anim, 1.f);
            ++p.step_counter;
            if (float(p.step_counter) * p.speed > 5.f) {
                p.step_counter = 0;
                p.leg_to_move = uint8_t((p.leg_to_move + 1) % (leg_count / 2));
                uint32_t const num = p.leg_to_move;
                uint32_t const num2 = leg_count - p.leg_to_move - 1;
                int32_t num3 = int32_t(x.legs_supporting());
                if (x.leg(num).supporting)
                    --num3;
                if (x.leg(num2).supporting)
                    --num3;
                if ((wants_to_step(x, num) || wants_to_step(x, num2)) && (!x.leg(num).supporting || !x.leg(num2).supporting || num3 >= 2)) {
                    start_step(x, num);
                    start_step(x, num2);
                }
            }
            x.vel(0).y += 0.5f / x.mass(0);
            x.vel(1).y -= 0.5f / x.mass(1);
            float const num4 = mathf::inverse_lerp(1.f, 5.f, rw::distance(x.pos(0), x.last_pos(0)));
            float const num5 = 0.35f + std::sin(p.walk_anim * mathf::pi * 2.f) * 0.15f * p.speed * num4;
            if (x.pos(1).y < x.pos(0).y - 3.f) {
                x.vel(0).x += num5 * p.flip / x.mass(0);
                x.vel(1).x -= num5 * p.flip / x.mass(1);
            }
            float const health = x.health();
            if (mathf::inverse_lerp(x.tower() ? 0.25f : 0.75f, 0.f, health) > 0.f && x.rng.value() < (1.f - health) * (x.tower() ? 0.002f : 0.01f))
                creature::stun(x.state, x.c, x.rng.range(40, 120));
        }

        // DrillCrab.DoSmoke: ExhaustSmoke puffs (drawn as dust)
        void do_smoke(ctx& x) {
            vec2 const v = custom::dir_vec(x.pos(1), x.pos(0));
            vec2 const a = custom::perpendicular_vector(v);
            vec2 const at = x.pos(0) + v * 21.f - a * 28.f;
            if (x.p.spin_up > 0.2f) {
                vec2 const vel = v * (x.p.spin_up * frange(x.rng, 10.f, 15.f)) + custom::rnv(x.rng) * (x.rng.value() * 6.f) + x.vel(0) * 0.75f;
                particles::spawn(x.state, x.room.id, particles::kind::dust, at, vel, 6.f, 0.6f, rw::vec3{0.2f, 0.2f, 0.2f}, 0.6f);
            }
        }

        // DrillCrab.BounceOffTerrain: the spinning drill against the terrain
        bool bounce_off_terrain(ctx& x) {
            vec2 const v = -rw::normalized(rw::to_vec2(x.w().body_chunk_get_contact_point(x.ch[1])));
            if (v == vec2{0.f, 0.f})
                return false;
            vec2 v2 = custom::perpendicular_vector(v);
            if (rw::dot(v2, x.pos(1) - x.pos(0)) < 0.f)
                v2 = -v2;
            vec2 const a = rotate_around_origo(v, frange(x.rng, -45.f, 45.f));
            float const num = 25.f * x.p.spin_up * frange(x.rng, 0.4f, 1.f);
            x.vel(0) += a * num;
            x.vel(1) += a * (x.rng.value() * num);
            for (uint32_t i = 0; i < 2; ++i)
                x.vel(i) = rw::clamp_magnitude(x.vel(i), 60.f);
            x.p.target_flip = frange(x.rng, -1.f, 1.f);
            auto const hit = scavenger::exact_terrain_ray_trace_pos(x.room, x.pos(1), x.pos(1) - v * x.rad(1) * 1.25f);
            vec2 const a2 = hit ? *hit : x.pos(1) - v * x.rad(1);
            if (x.p.spin_up > 0.5f && x.p.spark_delay == 0) {
                x.p.spark_delay = 4;
                int32_t const n = int32_t(float(x.rng.range(4, 9)) * (x.p.spin_up * 2.f - 1.f));
                for (int32_t j = 0; j < n; ++j) {
                    vec2 const sp = a2 + v2 * (x.rng.value() * 5.f) + v;
                    vec2 const sv = custom::rnv(x.rng) * 6.f + v2 * (x.rng.value() * 10.f);
                    particles::spawn(x.state, x.room.id, particles::kind::spark, sp, sv, 2.f, 0.25f, rw::vec3{1.f, 1.f, 0.8f});
                }
            }
            // (Drill_Crab_Collide_Terrain and the screen shake aren't ported)
            return hit.has_value();
        }

        // DrillCrab.DrillHitChunk on a creature's chunk
        void drill_hit_chunk(ctx& x, dcon::creature_id t, uint32_t j) {
            auto& w = x.w();
            auto const ok = creature::chunk_of(x.state, t, j);
            if (!ok)
                return;
            dcon::body_chunk_id const mk = x.ch[1];
            float const mag = rw::magnitude(w.body_chunk_get_vel(mk) - w.body_chunk_get_vel(ok));
            float num = x.p.spin_up * custom::lerp_map(mag, 0.f, 20.f, 1.f, 2.f);
            vec2 const vector = rw::normalized(w.body_chunk_get_pos(ok) - w.body_chunk_get_pos(mk)) * num * 10.f;
            if (creature::is(w, t, creature::type::drill_crab))
                num *= 0.25f;
            if (x.p.spin_up > 0.1f && x.p.object_hit_delay == 0) {
                x.p.object_hit_delay = 4;
                vec2 const a = rotate_around_origo(rw::normalized(w.body_chunk_get_pos(mk) - w.body_chunk_get_pos(ok)), frange(x.rng, -45.f, 45.f));
                float const num2 = 15.f * x.p.spin_up * frange(x.rng, 0.4f, 1.f);
                float const r = x.rng.value();
                bool const barnacle = creature::is(w, t, creature::type::barnacle);
                creature::violence_on(x.state, t, j, vector, num, 20.f, creature::damage_type::blunt, creature::template_of(x.state, x.c));
                x.vel(0) += a * num2;
                x.vel(1) += a * (r * num2);
                // Barnacle.LoseShell
                if (barnacle && creature::is(w, t, creature::type::barnacle) && barnacle::has_shell(x.state, t))
                    barnacle::lose_shell(x.state, t);
            }
        }

        // DrillCrab.IsDamageBlocked: only the body, from the drill's side
        bool damage_blocked(sys::game_state const& state, dcon::creature_id c, creature::damage_type type, std::optional<vec2> dir, std::optional<uint32_t> chunk) {
            if (!chunk || type == creature::damage_type::electric || type == creature::damage_type::explosion)
                return false;
            if (!dir)
                return *chunk != 0;
            auto const& w = state.world;
            float const ang = rw::angle(*dir, w.body_chunk_get_pos(creature::chunk(state, c, 1)) - w.body_chunk_get_pos(creature::chunk(state, c, 0)));
            return *chunk != 0 || ang > 70.f;
        }

        // the eyes on their stalks (DrillCrab.Update)
        void eyes_update(ctx& x) {
            vec2 const vector = custom::dir_vec(x.pos(1), x.pos(0));
            bool const awake = x.consious();
            for (uint32_t k = 0; k < 2; ++k) {
                eye& e = x.b.eyes[k];
                float const side = k == 0 ? 1.f : -1.f;
                e.last_pos = e.pos;
                if (awake) {
                    vec2 const v2 = rw::slerp(vector, vec2{0.f, 1.f}, 0.65f);
                    vec2 const a2 = custom::perpendicular_vector(v2);
                    float const dd = mathf::lerp(50.f, 90.f, x.iv.eyestalk_length) + 5.f * (x.iv.eyestalk_asymmetry * 2.f - 1.f) * side;
                    bool const up = x.tower() && !x.hiding();
                    vec2 v3 = x.pos(0) + v2 * (dd * (up ? -0.5f : 1.f)) - a2 * (3.f * side + x.p.flip * 6.f);
                    if (up)
                        v3.x += 50.f * x.p.flip;
                    if (x.ai.look_creature) {
                        if (auto const* r = tracker::find(tracker::of(x.state, x.c), x.ai.look_creature)) {
                            vec2 const a3 = r->visual && creature::here(x.state, r->who, x.room.id) ? creature::main_pos(x.state, r->who)
                                                                                                  : room_view::middle_of_tile(tracker::best_guess(*r));
                            vec2 const m = rw::clamp_magnitude((a3 - v3) / 20.f, 20.f);
                            v3 += vec2{m.x * 0.65f, m.y};
                        }
                    }
                    if (auto const hit = scavenger::exact_terrain_ray_trace_pos(x.room, x.pos(0), v3))
                        v3 = *hit;
                    e.vel += (v3 - e.pos) * 0.075f;
                    e.pos += (v3 - e.pos) * 0.1f;
                    e.vel *= 0.935f;
                    e.pos += e.vel;
                } else {
                    vec2 const v4 = x.pos(0) + vector * 25.f - custom::perpendicular_vector(vector) * side * 5.f;
                    float const num2 = rw::distance(v4, e.pos);
                    constexpr float num3 = 50.f;
                    if (num2 > num3) {
                        vec2 const dv = custom::dir_vec(v4, e.pos) * ((num3 - num2) * 0.5f);
                        e.vel += dv;
                        e.pos += dv;
                    }
                    e.vel *= 0.975f;
                    e.vel.y -= x.room.gravity;
                    e.pos += e.vel;
                    terrain_collision cd{e.pos, e.last_pos, e.vel, 2.f};
                    horizontal_collision(x.room, cd);
                    vertical_collision(x.room, cd);
                    slopes_vertically(x.room, cd);
                    e.pos = cd.pos;
                    e.vel = cd.vel;
                    e.pos = rw::clamp_magnitude(e.pos - v4, num3 * 1.5f) + v4;
                }
            }
        }

        // HealthState's part of Creature.Update: hurt, it may die or faint,
        // else heals
        void health_update(sys::game_state& state, dcon::creature_id c) {
            auto& w = state.world;
            if (w.creature_get_dead(c))
                return;
            auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
            float& health = w.creature_get_health(c);
            if (health < 0.f && rng.value() < -health && rng.value() < 1.f / float(creature::consious(state, c) ? 80 : 800))
                creature::die(state, c);
            if (rng.value() < 0.033333335f && (rng.value() * 0.2f > health || rng.value() < -health)) {
                // (DrillCrab.Stun: nothing under 31 takes)
                int32_t const st = rng.range(1, rng.range(1, 27 - custom::int_clamp(int32_t(20.f * health), 0, 10)));
                if (st > 30)
                    creature::stun(state, c, st);
            }
            if (health > 0.f && health < 1.f && rng.value() < 0.01f)
                health = mathf::min(1.f, health + 1.f / mathf::lerp(550.f, 70.f, health));
        }
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 at, variant v) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::drill_crab);
        auto const d = data(w, c);
        auto rng = rw::random(state.seed, 0, uint32_t(c.value));
        int32_t seed = int32_t(rng.next() & 0x7FFFFFFFu);
        if (state.spawn_seed >= 0)
            seed = state.spawn_seed;
        // AbstractCreature.personality (its own InitState(seed), as yeek.cpp's)
        personality pe;
        {
            rw::unity_random r{seed};
            pe.sympathy = push_from_half(r.value(), 1.5f);
            pe.energy = push_from_half(r.value(), 1.5f);
            pe.bravery = push_from_half(r.value(), 1.5f);
            float const n0 = r.value();
            float const n1 = mathf::pow(r.value(), 0.25f);
            pe.nervous = mathf::lerp(n0, mathf::lerp(pe.energy, 1.f - pe.bravery, 0.5f), n1);
            float const a0 = r.value();
            float const a1 = mathf::pow(r.value(), 0.25f);
            pe.aggression = mathf::lerp(a0, (pe.energy + pe.bravery) / 2.f * (1.f - pe.sympathy), a1);
            float const d0 = r.value();
            float const d1 = mathf::pow(r.value(), 0.25f);
            pe.dominance = mathf::lerp(d0, (pe.energy + pe.bravery + pe.aggression) / 3.f, d1);
            pe.nervous = push_from_half(pe.nervous, 2.5f);
            pe.aggression = push_from_half(pe.aggression, 2.5f);
        }
        // DrillCrab.IndividualVariations (InitState(ID.RandomSeed))
        individual_variations iv;
        {
            rw::unity_random r{seed};
            float const value = r.value();
            if (value < 0.08f)
                iv.extra_teeth = 2;
            else if (value < 0.3f)
                iv.extra_teeth = 1;
            if (r.value() > pe.aggression)
                iv.extra_teeth = -iv.extra_teeth;
            auto const lerp_rr = [&](float a) {
                float const b = r.value();
                float const t = r.value();
                return mathf::lerp(a, b, t);
            };
            iv.leg_thickness = lerp_rr(pe.dominance);
            iv.leg_uniformity = lerp_rr(1.f - pe.energy);
            iv.leg_length = lerp_rr(pe.dominance);
            iv.body_size = lerp_rr(mathf::max(1.f - pe.energy, pe.dominance));
            iv.drill_size = lerp_rr(mathf::max(pe.aggression, pe.dominance));
            iv.eyestalk_length = r.value();
            if (r.value() < 0.25f) {
                float const l = lerp_rr(pe.nervous);
                iv.eyestalk_asymmetry = 0.5f + 0.5f * l * (r.value() < 0.5f ? 1.f : -1.f);
                iv.eyestalk_asymmetry = push_from_half(iv.eyestalk_asymmetry, 0.5f);
            }
            iv.eye_lightness = r.value();
            if (v == variant::tower) {
                iv.body_size += 1.f;
                iv.drill_size += 1.f;
                iv.leg_thickness += 1.f;
                iv.eyestalk_length *= 2.f;
            }
        }
        // never inside terrain
        auto const view = world::get_room_view(state, room);
        if (view.get_tile(at).solid()) {
            ivec2 const t0 = tile_of(at);
            int best = 1 << 30;
            for (int dy = -6; dy <= 6; ++dy)
                for (int dx = -6; dx <= 6; ++dx) {
                    ivec2 const t = t0 + ivec2{dx, dy};
                    if (view.inside(t.x, t.y) && !view.get_tile(t).solid() && dx * dx + dy * dy < best) {
                        best = dx * dx + dy * dy;
                        at = room_view::middle_of_tile(t);
                    }
                }
        }
        physics::object_desc desc;
        desc.gravity = 0.9f;
        desc.air_friction = 0.999f;
        desc.bounce = 0.1f;
        desc.surface_friction = 0.1f;
        desc.water_friction = 0.96f;
        desc.buoyancy = 0.f;
        auto const o = physics::create_object(state, room, desc);
        auto const lerp_u = [](float a, float b, float t) { return a + (b - a) * t; };
        // the body (20, 1) and the drill under it (16, 0.5), 27 apart
        auto const k0 = physics::add_chunk(state, o, at, 20.f * lerp_u(0.9f, 1.1f, iv.body_size), 1.f);
        auto const k1 = physics::add_chunk(state, o, at - vec2{0.f, 20.f}, 16.f * lerp_u(0.9f, 1.1f, iv.drill_size), 0.5f);
        physics::add_connection(state, o, k0, k1, 27.f, physics::connection_type::normal, 1.f, -1.f);
        for (auto const k : {k0, k1})
            w.body_chunk_set_collide_with_slopes(k, 0);
        physics::set_go_through_floors(state, o, true);
        w.try_create_creature_body(c, o);
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_set_grasps(c, creature::grasp_pair{});
        w.creature_set_behavior(c, uint8_t(behavior::idle));
        w.creature_set_current_utility(c, 0.f);
        w.creature_set_destination(c, tile_of(at));
        auto& p = pl(state, c);
        uint16_t const keep = p.data;
        p = creature::drill_crab_payload{};
        p.data = keep;
        p.variant = v;
        p.speed = 1.f;
        p.move_target_age = uint8_t(rng.range(0, v == variant::tower ? 20 : 5));
        w.drill_crab_data_set_seed(d, seed);
        w.drill_crab_data_set_personality(d, pe);
        w.drill_crab_data_set_ivars(d, iv);
        ai_state a;
        a.idle_timer = rng.range(40, 120);
        w.drill_crab_data_set_ai(d, a);
        body_state b;
        b.ideal_height = (v == variant::tower ? 400.f : 150.f) * leg_length_factor(v, iv);
        w.drill_crab_data_set_body(d, b);
        w.drill_crab_data_set_graphics(d, graphics_state{});
        // the six legs (DrillCrabState.legHealths: the parts' health)
        for (uint32_t i = 0; i < leg_count; ++i) {
            auto const part = creature::add_part(state, c, creature::part_kind::drill_crab_leg, uint8_t(i));
            creature::drill_crab_leg_payload lp{};
            lp.mode = leg_mode::scanning;
            w.creature_part_get_payload(part).drill_crab_leg = lp;
        }
        tracker::clear(state, c);
        {
            ctx x(state, c, view);
            reset_legs(x);
        }
        drill_crab_graphics::reset_positions(state, c);
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        creature::release_target(state, c);
        creature::erase(state, c);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        // Creature.Update (HealthState) + PhysicalObject.Update
        health_update(state, c);
        if (!w.creature_is_valid(c))
            return;
        creature::stun_countdown(state, c);
        physics::update_object(state, room, creature::body(state, c), nullptr, nullptr);
        ctx x(state, c, room);
        auto& p = x.p;
        p.squeeze = 0;
        if (x.legs_supporting() < 2)
            x.b.next_step_pos = x.pos(0);
        else
            follow_path(x);
        // (the chatter and drill sound loops aren't ported)
        p.last_flip = p.flip;
        p.last_eyes_hidden = p.eyes_hidden;
        bool const awake = x.consious();
        if (awake) {
            float target = 0.f;
            if (x.hiding() && p.hide_timer < 80)
                target = 0.2f;
            else if (x.hiding())
                target = 1.f;
            p.eyes_hidden = move_towards(p.eyes_hidden, target, 0.033333335f);
        }
        p.spin_up = move_towards(p.spin_up, x.spinning_out() ? 1.f : 0.f, 0.016666668f);
        if (p.spin_out_timer > 0)
            --p.spin_out_timer;
        if (p.spark_delay > 0)
            --p.spark_delay;
        if (p.object_hit_delay > 0)
            --p.object_hit_delay;
        if (p.hide_timer > 0)
            --p.hide_timer;
        if (p.spin_up > 0.f) {
            bounce_off_terrain(x);
            do_smoke(x);
            vec2 const a = custom::rnv(x.rng) * (x.rng.value() * 0.5f * p.spin_up);
            x.vel(0) += a / x.mass(0);
            x.vel(1) -= a / x.mass(0);
        }
        p.squeeze_factor = move_towards(p.squeeze_factor, p.squeeze ? 1.f : 0.f, 0.033333335f);
        for (uint32_t i = 0; i < 2; ++i)
            w.body_chunk_set_terrain_squeeze(x.ch[i], 1.f - 0.4f * p.squeeze_factor);
        for (uint32_t i = 0; i < leg_count; ++i)
            if (x.legs[i])
                leg_update(x, i);
        if (awake)
            ai_update(x);
        if (!x.hiding() && awake) {
            act(x);
        } else {
            if (!awake)
                p.hide_timer = int16_t(std::max<int32_t>(p.hide_timer, 90));
            for (uint32_t i = 0; i < leg_count; ++i)
                if (x.legs[i] && x.leg(i).mode != leg_mode::retracting)
                    retract(x, i);
        }
        // (noPathCounter is never counted up in vanilla: its term is 0)
        float const sup = float(x.legs_supporting());
        float const gb = mathf::lerp(0.9f, 0.f, sup / (x.tower() ? 1.f : 2.f));
        auto const o = x.obj();
        w.physical_object_set_g(o, mathf::max(w.physical_object_get_g(o) - 0.033333335f, gb));
        p.flip = custom::lerp_and_tick(p.flip, p.target_flip, 0.035f, 0.015f);
        w.physical_object_set_air_friction(o, mathf::lerp(1.f, 0.925f, sup / 2.f));
        eyes_update(x);
    }

    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) {
        return !creature::holds(state.world, t, c) && !creature::holds(state.world, c, t);
    }

    void collide(sys::game_state& state, dcon::creature_id c, room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j) {
        if (i != 1 || pl(state, c).spin_up < 0.25f || !creature::live(state, t))
            return;
        ctx x(state, c, room);
        drill_hit_chunk(x, t, j);
    }

    void violence(sys::game_state& state, dcon::creature_id c, creature::hit_part const& part, vec2 momentum, float damage, float stun_bonus,
        creature::damage_type type) {
        auto& w = state.world;
        auto rng = rw::random(state.seed, state.tick, rw::ids(uint32_t(c.value), uint32_t(part.chunk + 7 * (part.tentacle + 2))));
        int32_t const app = part.tentacle >= 0 && part.tentacle < int32_t(leg_count) ? part.tentacle : -1;
        std::optional<uint32_t> hit_chunk;
        if (app < 0)
            hit_chunk = part.chunk >= 0 && part.chunk < 2 ? uint32_t(part.chunk) : 0u;
        std::optional<vec2> dir;
        if (rw::sqr_magnitude(momentum) > 0.f)
            dir = momentum;
        if (damage_blocked(state, c, type, dir, hit_chunk)) {
            // Lizard_Head_Shield_Deflect
            state.play_sound(sound::sound_id::lizard_head_shield_deflect, creature::room_of(state, c), w.body_chunk_get_pos(creature::chunk(state, c, 0)));
            return;
        }
        auto const room = world::get_room_view(state, creature::room_of(state, c));
        ctx x(state, c, room);
        float damage2 = damage;
        if (type == creature::damage_type::stab && damage >= 0.25f && (app < 0 || !x.leg_dead(uint32_t(app))))
            panic(state, c);
        if (app >= 0) {
            uint32_t const li = uint32_t(app);
            if (type != creature::damage_type::electric)
                stun_bonus = 0.f;
            float const num = x.tower() ? 2.f : 0.6666667f;
            float const lh = x.leg_health(li);
            damage2 = mathf::min(lh, damage / num) * 0.5f;
            x.set_leg_health(li, mathf::max(lh - damage / num, 0.f));
        }
        // Creature.Violence (HealthState, quickDeath)
        if (dir) {
            if (hit_chunk) {
                auto const k = x.ch[*hit_chunk];
                w.body_chunk_get_vel(k) += rw::clamp_magnitude(momentum / w.body_chunk_get_mass(k), 10.f);
            } else if (app >= 0) {
                // ApplyForceOnAppendage: the segment before the hit and the next
                float const dn = mathf::clamp01(part.distance_to_next);
                int32_t const num = part.prev_segment - 1;
                leg_apply_force(x, uint32_t(app), num, momentum * (1.f - dn));
                leg_apply_force(x, uint32_t(app), num + 1, momentum * dn);
            }
        }
        float const dmg = damage2 / base_damage_resistance(x.p.variant);
        float st = (damage2 * 30.f + stun_bonus) / base_stun_resistance;
        float& health = w.creature_get_health(c);
        st *= 1.5f + mathf::inverse_lerp(0.5f, 0.f, health) * rng.value();
        // DrillCrab.Stun: nothing under 31 takes
        if (int32_t(st) > 30)
            creature::stun(state, c, int32_t(st));
        health -= dmg;
        if (rng.value() < -health || health < -1.f || (health < 0.f && rng.value() < 0.33f))
            creature::die(state, c);
    }

    void release(sys::game_state&, dcon::creature_id, dcon::creature_id) {}

    void died(sys::game_state&, dcon::creature_id) {}

    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const view = world::get_room_view(state, room);
        ivec2 const t = tile_of(at);
        vec2 const dir = rw::to_vec2(view.shortcut_entrance_hole_direction(t));
        vec2 const mid = room_view::middle_of_tile(t);
        // Creature.SpitOutOfShortCut: the body first, out of the hole
        for (uint32_t i = 0; i < 2; ++i) {
            auto const k = creature::chunk(state, c, i);
            physics::hard_set_position(state, k, mid + dir * (i == 0 ? 5.f : -5.f));
            w.body_chunk_set_last_pos(k, mid);
            w.body_chunk_set_vel(k, dir * 2.f);
        }
        ctx x(state, c, view);
        x.ai.look_creature = dcon::creature_id{};
        x.ai.flee_from = dcon::creature_id{};
        x.ai.past_count = 0;
        x.ai.stuck_counter = 0;
        x.b.move_target = vec2{0.f, 0.f};
        reset_legs(x);
    }

    std::string describe(sys::game_state const& state, dcon::creature_id c) {
        static constexpr char const* names[] = {"idle", "hide", "flee", "escape rain"};
        auto const& w = state.world;
        vec2 const h = w.body_chunk_get_pos(creature::chunk(state, c, 0));
        uint32_t const b = w.creature_get_behavior(c);
        auto const& p = pl(state, c);
        uint32_t sup = 0, dead_legs = 0;
        for (uint32_t i = 0; i < leg_count; ++i) {
            auto const part = creature::part(w, c, i);
            if (w.creature_part_get_payload(part).drill_crab_leg.supporting)
                ++sup;
            if (w.creature_part_get_health(part) <= 0.f)
                ++dead_legs;
        }
        return std::string(p.variant == variant::tower ? "tower crab" : "drill crab") + " at " + std::to_string(int(h.x)) + "," + std::to_string(int(h.y)) + " behavior "
            + (b < 4 ? names[b] : "?") + " legs " + std::to_string(sup) + "/6 down" + (dead_legs ? ", " + std::to_string(dead_legs) + " dead" : "")
            + (p.hide_timer > 0 ? " hiding" : "") + (p.spin_up > 0.f ? " drilling" : "");
    }

    bool spawn_spot(room_view const& room, ivec2 tile) {
        return room.inside(tile.x, tile.y) && !room.get_tile(tile).solid() && room.get_tile(tile).water == 0 && room.get_tile(tile + ivec2{0, -1}).solid()
            && !room.get_tile(tile + ivec2{0, 1}).solid() && !room.get_tile(tile + ivec2{0, 2}).solid();
    }

    bool trace_legs(sys::game_state const& state, dcon::room_id room, vec2 from, vec2 to, dcon::creature_id thrown_by, dcon::creature_id& hit,
        creature::hit_part& part, float& at) {
        auto const& w = state.world;
        float best = 2.f;
        rooms::for_each_creature(state, room, creature::type::drill_crab, [&](dcon::creature_id c) {
            if (c == thrown_by || w.creature_get_away(c) != 0)
                return;
            auto const& b = w.drill_crab_data_get_body(data(w, c));
            uint32_t const n = leg_segments(pl(state, c).variant);
            vec2 const body = w.body_chunk_get_pos(creature::chunk(state, c, 0));
            for (uint32_t leg = 0; leg < leg_count; ++leg) {
                // IHaveAppendages.AppendagePosition: the body, then the segments
                vec2 prev = body;
                for (uint32_t s = 0; s < n; ++s) {
                    vec2 const next = b.segments[leg][s].pos;
                    float sa = 0.f, sb = 0.f;
                    if (segment_distance(from, to, prev, next, sa, sb) < 5.f && sa < best) {
                        best = sa;
                        hit = c;
                        part = creature::hit_part{-1, int32_t(leg), int32_t(s), sb};
                    }
                    prev = next;
                }
            }
        });
        at = best;
        return best <= 1.f;
    }

    variant variant_of(sys::game_state const& state, dcon::creature_id c) noexcept { return pl(state, c).variant; }

    uint32_t leg_segments(variant v) noexcept { return v == variant::tower ? tower_leg_segments : small_leg_segments; }

    float segment_length(sys::game_state const& state, dcon::creature_id c, uint32_t leg, uint32_t seg) noexcept {
        auto const v = pl(state, c).variant;
        auto const& iv = state.world.drill_crab_data_get_ivars(data(state.world, c));
        float const* lengths = v == variant::tower ? tower_lengths : small_lengths;
        return lengths[seg] * length_fac_of(v, iv, leg);
    }

    bool hiding(sys::game_state const& state, dcon::creature_id c) noexcept { return pl(state, c).hide_timer > 0; }
}
