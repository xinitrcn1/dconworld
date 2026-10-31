#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sky_whale.hpp"
#include "dens.hpp"
#include "particles.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "rain.hpp"
#include "rooms.hpp"
#include "sky_whale_graphics.hpp"
#include "spawns.hpp"
#include "tracker.hpp"

namespace sky_whale {
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // CreatureTemplate (SkyWhale): baseDamageResistance, baseStunResistance,
        // visualRadius
        constexpr float base_damage_resistance = 50.f;
        constexpr float base_stun_resistance = 8.f;
        // SkyWhale.turnDegreesPerTick
        constexpr float turn_degrees_per_tick = 2.125f;
        // CreatureParams.totalMass
        constexpr float total_mass = 100.f;
        // SkyWhaleWhisker.connectionRad
        constexpr float whisker_connection_rad = 12.f;
        // SkyWhaleAbstractAI.BLACKLISTEDSKYS: no roaming into these
        constexpr std::string_view blacklisted_skies[] = {"wpta_d04"};
        // Bodychunk.rotationChunk: the other end of each chunk's last
        // BodyChunkConnection (the push connections are made last)
        constexpr uint32_t rotation_chunk[chunk_count] = {2, 3, 4, 5, 2, 3};
        // where it waits while off screen: above its room (no physics runs)
        constexpr float park_height = 2000.f;

        creature::sky_whale_payload& pl(sys::game_state& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).sky_whale;
        }
        creature::sky_whale_payload const& pl(sys::game_state const& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).sky_whale;
        }
        dcon::sky_whale_data_id data(sys::game_state const& state, dcon::creature_id c) {
            return creature::sky_whale_data(state.world, c);
        }
        vec2& pos(sys::game_state& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_pos(chunk(state, c, i));
        }
        vec2& vel(sys::game_state& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_vel(chunk(state, c, i));
        }
        vec2 pos(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_pos(chunk(state, c, i));
        }
        ivec2 tile_of(vec2 p) {
            return room_view::get_tile_position(p);
        }

        //
        // Unity and RWCustom helpers the port needs
        //
        // Mathf.SmoothDamp
        float smooth_damp(float current, float target, float& current_velocity, float smooth_time, float max_speed, float delta_time) {
            smooth_time = mathf::max(0.0001f, smooth_time);
            float const num = 2.f / smooth_time;
            float const num2 = num * delta_time;
            float const num3 = 1.f / (1.f + num2 + 0.48f * num2 * num2 + 0.235f * num2 * num2 * num2);
            float num4 = current - target;
            float const num5 = target;
            float const num6 = max_speed * smooth_time;
            num4 = mathf::clamp(num4, -num6, num6);
            target = current - num4;
            float const num7 = (current_velocity + num * num4) * delta_time;
            current_velocity = (current_velocity - num * num7) * num3;
            float num8 = target + (num4 + num7) * num3;
            if ((num5 - current > 0.f) == (num8 > num5)) {
                num8 = num5;
                current_velocity = (num8 - num5) / delta_time;
            }
            return num8;
        }
        // Vector2.SignedAngle
        float signed_angle(vec2 from, vec2 to) {
            float const a = rw::angle(from, to);
            return a * mathf::sign(from.x * to.y - from.y * to.x);
        }
        // Vector3.RotateTowards (2D): the direction turned by at most
        // `max_radians`, the length moved by at most `max_magnitude`
        vec2 rotate_towards(vec2 current, vec2 target, float max_radians, float max_magnitude) {
            float const lc = rw::magnitude(current), lt = rw::magnitude(target);
            if (lc < 1e-5f || lt < 1e-5f)
                return custom::move_towards(current, target, max_magnitude);
            float const len = lc + mathf::clamp(lt - lc, -max_magnitude, max_magnitude);
            float const ac = std::atan2(current.y, current.x);
            float da = std::atan2(target.y, target.x) - ac;
            while (da > mathf::pi)
                da -= 2.f * mathf::pi;
            while (da < -mathf::pi)
                da += 2.f * mathf::pi;
            float const ang = ac + mathf::clamp(da, -max_radians, max_radians);
            return vec2{std::cos(ang), std::sin(ang)} * len;
        }
        // Custom.ClosestPointOnLineSegment
        vec2 closest_point_on_segment(vec2 a, vec2 b, vec2 p) {
            vec2 const ab = b - a;
            float const l2 = rw::sqr_magnitude(ab);
            if (l2 <= 0.f)
                return a;
            return a + ab * mathf::clamp01(rw::dot(p - a, ab) / l2);
        }

        //
        // The room's SkyWhalePathfinding nodes
        //
        struct node {
            vec2 pos;
            vec2 handle;
        };
        struct node_list {
            node n[max_nodes];
            uint32_t count = 0;
        };
        void room_nodes(sys::game_state const& state, dcon::room_id room, node_list& out) {
            auto const& w = state.world;
            out.count = 0;
            w.room_for_each_thing_location(room, [&](dcon::thing_location_id l) {
                auto const t = w.thing_location_get_thing(l);
                if (w.placed_thing_get_kind(t) == world::thing_kind::sky_whale_node && out.count < max_nodes)
                    out.n[out.count++] = node{w.placed_thing_get_pos(t), w.placed_thing_get_handle(t)};
            });
        }
        bool has_nodes(sys::game_state const& state, dcon::room_id room) {
            bool any = false;
            state.world.room_for_each_thing_location(room, [&](dcon::thing_location_id l) {
                any = any || state.world.placed_thing_get_kind(state.world.thing_location_get_thing(l)) == world::thing_kind::sky_whale_node;
            });
            return any;
        }
        bool blacklisted(sys::game_state const& state, dcon::room_id room) {
            auto const name = state.to_string_view(state.world.room_get_name(room));
            for (auto const b : blacklisted_skies) {
                bool same = name.size() == b.size();
                for (size_t i = 0; same && i < b.size(); ++i)
                    same = char(std::tolower(static_cast<unsigned char>(name[i]))) == b[i];
                if (same)
                    return true;
            }
            return false;
        }
        // the rooms of a region it roams: the ones with nodes
        std::vector<dcon::room_id> roam_rooms(sys::game_state const& state, dcon::region_id region) {
            std::vector<dcon::room_id> out;
            if (!region)
                return out;
            state.world.region_for_each_region_membership(region, [&](dcon::region_membership_id m) {
                auto const r = state.world.region_membership_get_room(m);
                if (!blacklisted(state, r) && has_nodes(state, r))
                    out.push_back(r);
            });
            return out;
        }

        // SkyWhaleAI.FindClosestNode: the closest node not yet used (-1 none)
        int32_t closest_node(node_list const& nodes, uint32_t used, vec2 head) {
            int32_t best = -1;
            float best_d = std::numeric_limits<float>::max();
            for (uint32_t i = 0; i < nodes.count; ++i) {
                if (used & (1u << i))
                    continue;
                float const d = rw::distance(head, nodes.n[i].pos);
                if (d < best_d) {
                    best_d = d;
                    best = int32_t(i);
                }
            }
            return best;
        }

        //
        // Pathing (SkyWhalePather, simplified): the open air, 4 tiles off the
        // terrain
        //
        int32_t prox(sys::game_state& state, room_view const& room, ivec2 t) {
            return pathing::ai_terrain_proximity(state, room.id, t);
        }
        // a tile the pather allows (CheckConnectionCost: within 4 of the
        // terrain is illegal; stands in for CoordinateReachableAndGetbackable)
        bool open_tile(sys::game_state& state, room_view const& room, ivec2 t) {
            return room.inside(t.x, t.y) && !room.get_tile(t).solid() && prox(state, room, t) >= 4;
        }
        // a straight way with room for the body
        bool clear_line(sys::game_state& state, room_view const& room, vec2 a, vec2 b) {
            if (!room.visual_contact(a, b))
                return false;
            float const len = rw::distance(a, b);
            int32_t const n = int32_t(len / 40.f);
            for (int32_t k = 1; k <= n; ++k) {
                ivec2 const t = tile_of(rw::lerp(a, b, float(k) / float(n + 1)));
                if (room.inside(t.x, t.y) && prox(state, room, t) < 3)
                    return false;
            }
            return true;
        }

        // SkyWhale.Act's FollowPath and look-ahead: the point to fly at on
        // the way to `target` (none: no way on)
        std::optional<vec2> steer_point(sys::game_state& state, dcon::creature_id c, room_view const& room, vec2 target) {
            auto& w = state.world;
            auto const& x = pl(state, c);
            vec2 const head = pos(state, c, 0);
            ivec2 const tt = tile_of(target);
            if (!room.inside(tt.x, tt.y))
                return target;
            if (custom::dist_less(head, target, 40.f))
                return rw::magnitude(x.fly_dir) > 0.f ? head + rw::normalized(x.fly_dir) * 100.f : target;
            if (clear_line(state, room, head, target))
                return target;
            auto& ai = w.sky_whale_data_get_ai(data(state, c));
            if (ai.field_goal.x < 0 || custom::manhattan_distance(ai.field_goal, tt) > 3)
                ai.field_goal = tt;
            auto const f = pathing::field_to(state, room.id, ai.field_goal, fly);
            ivec2 from = tile_of(head);
            from = ivec2{custom::int_clamp(from.x, 0, room.width - 1), custom::int_clamp(from.y, 0, room.height - 1)};
            auto step = pathing::next_step(state, f, from);
            // the tiles around it (Act's 8 directions out to 6 * 20 px)
            for (int32_t i = 1; i < 7 && !step; ++i)
                for (auto const d : custom::eight_directions) {
                    ivec2 const t = tile_of(head + rw::to_vec2(d) * 20.f * float(i));
                    if (!room.inside(t.x, t.y))
                        continue;
                    step = pathing::next_step(state, f, t);
                    if (step)
                        break;
                }
            if (!step)
                return std::nullopt;
            vec2 sum = room_view::middle_of_tile(*step);
            float n = 1.f;
            ivec2 cur = *step;
            for (int k = 0; k < 8; ++k) {
                auto const nx = pathing::next_step(state, f, cur);
                if (!nx || !room.visual_contact(head, room_view::middle_of_tile(*nx)))
                    break;
                sum += room_view::middle_of_tile(*nx);
                n += 1.f;
                cur = *nx;
            }
            return sum / n;
        }

        //
        // SkyWhaleAI
        //
        // SkyWhaleAI.IdleScore (lower is better)
        float idle_score(sys::game_state& state, dcon::creature_id c, room_view const& room, ivec2 t) {
            if (t.x < 0 || !open_tile(state, room, t))
                return std::numeric_limits<float>::max();
            float num = 1.f;
            int32_t const p = prox(state, room, t);
            bool const alt = pl(state, c).alt != 0;
            if (alt)
                num += float(std::max(10 - p, 0) * 50);
            else
                num += float(std::min(p, 10) - 10) * 40.f;
            if (alt) {
                // RayTraceTilesForTerrain up to the top
                for (int32_t y = t.y; y < room.height; ++y)
                    if (room.get_tile(t.x, y).solid()) {
                        num += 5000.f;
                        break;
                    }
            }
            if (open_tile(state, room, t + ivec2{0, -1}))
                num += 10.f;
            return num;
        }

        ivec2 random_tile(room_view const& room, rw::rng& rng) {
            int32_t const tx = rng.range(0, room.width);
            return ivec2{tx, rng.range(0, room.height)};
        }

        // SkyWhaleAI.NewRoom (and the constructor's)
        void new_room_ai(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            auto& x = pl(state, c);
            auto& ai = w.sky_whale_data_get_ai(data(state, c));
            ai = ai_state{};
            ai.stuck_pos = pos(state, c, 0);
            ai.stuck_check = 40;
            node_list nodes;
            room_nodes(state, room.id, nodes);
            x.current_node = int8_t(closest_node(nodes, 0, pos(state, c, 0)));
            x.test_node_index = 0;
            x.time_to_tire = rng.range(100, 1400);
            x.time_in_room = 0;
            x.idle_pos_counter = 0;
            x.leaving = 0;
            x.leave_side = 0;
            w.creature_set_destination(c, random_tile(room, rng));
            w.creature_set_behavior(c, uint8_t(behavior::idle));
        }

        // the way out over the room's side (the offscreen den)
        vec2 leave_target(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& x = pl(state, c);
            vec2 const head = pos(state, c, 0);
            if (x.leave_side == 0)
                x.leave_side = head.x < room.pixel_width() * 0.5f ? -1 : 1;
            float const side = float(x.leave_side);
            if ((side < 0.f && head.x < 20.f) || (side > 0.f && head.x > room.pixel_width() - 20.f))
                return head + vec2{side * 300.f, 0.f};
            int32_t const col = side < 0.f ? 0 : room.width - 1;
            int32_t const y0 = custom::int_clamp(tile_of(head).y, 1, std::max(1, room.height - 2));
            // the open tile of the border column closest to its height
            for (int32_t dy = 0; dy < room.height; ++dy)
                for (int32_t s = -1; s <= 1; s += 2) {
                    int32_t const y = y0 + dy * s;
                    if (y < 0 || y >= room.height)
                        continue;
                    bool open = true;
                    for (int32_t k = 0; k < 4 && open; ++k)
                        open = !room.get_tile(col - x.leave_side * k, y).solid();
                    if (open)
                        return room_view::middle_of_tile(col, y);
                }
            return head + vec2{side * 300.f, 0.f};
        }

        // SkyWhaleAI.Update (DetermineBehavior and Act)
        void ai_update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            auto& x = pl(state, c);
            auto const d = data(state, c);
            auto& ai = w.sky_whale_data_get_ai(d);
            auto const& par = w.sky_whale_data_get_params(d);
            vec2 const head = pos(state, c, 0);
            ++x.time_in_room;
            ++ai.abstract_time_in_room;
            // StuckTracker (simplified): its head hardly moving
            if (--ai.stuck_check <= 0) {
                ai.stuck_check = 40;
                bool const still = custom::dist_less(head, ai.stuck_pos, 20.f);
                ai.stuck = still ? mathf::min(1.f, ai.stuck + 0.34f) : mathf::max(0.f, ai.stuck - 0.25f);
                ai.stuck_pos = head;
            }
            node_list nodes;
            room_nodes(state, room.id, nodes);
            // DetermineBehavior: the UtilityComparer over the rain (weight 1
            // in a room that rains, else 0) and the stuck tracker, each to
            // the power 1.1
            float const rain_u = rain::rains_in(state, room.id) ? mathf::pow(mathf::max(dens::rain_utility(state, c, room.id), 0.f), 1.1f) : 0.f;
            float const stuck_u = mathf::pow(ai.stuck, 1.1f);
            behavior b = rain_u >= stuck_u ? behavior::escape_rain : behavior::get_unstuck;
            float const utility = mathf::max(rain_u, stuck_u);
            if (utility < 0.05f)
                b = nodes.count > 0 ? behavior::follow_path : behavior::idle;
            if (x.leaving && (b == behavior::idle || b == behavior::follow_path))
                b = behavior::leave;
            w.creature_set_behavior(c, uint8_t(b));
            w.creature_set_current_utility(c, mathf::max(utility, 0.05f));

            ivec2& dest = w.creature_get_destination(c);
            switch (b) {
            case behavior::idle: {
                ivec2 const coord = random_tile(room, rng);
                if (idle_score(state, c, room, coord) < idle_score(state, c, room, ai.temp_idle_pos))
                    ai.temp_idle_pos = coord;
                if (x.time_in_room > x.time_to_tire) {
                    // tired of the room: off to the den (tryingToLeaveRoom)
                    if (x.idle_pos_counter <= 0) {
                        x.time_in_room = 0;
                    } else {
                        x.idle_pos_counter = rng.range(500, 1000);
                        x.time_to_tire = rng.range(400, 1000);
                        x.time_in_room = std::max(x.time_to_tire + 1, x.time_in_room);
                        x.leaving = 1;
                    }
                } else if (idle_score(state, c, room, ai.temp_idle_pos)
                    < idle_score(state, c, room, dest) + custom::lerp_map(float(x.idle_pos_counter), 0.f, 300.f, 100.f, -300.f)) {
                    dest = ai.temp_idle_pos;
                    x.idle_pos_counter = rng.range(200, 500);
                    ai.temp_idle_pos = random_tile(room, rng);
                }
                --x.idle_pos_counter;
                break;
            }
            case behavior::escape_rain:
            case behavior::leave:
                // (DenFinder: its den is off screen, over the room's side)
                x.leaving = 1;
                dest = tile_of(leave_target(state, c, room));
                break;
            case behavior::get_unstuck: {
                // GetUnstuckPosCalculator (simplified): an open spot nearby
                if (ai.unstuck_goal.x < 0 || custom::dist_less(head, room_view::middle_of_tile(ai.unstuck_goal), 60.f)) {
                    ai.unstuck_goal = ivec2{-1, -1};
                    for (int k = 0; k < 10; ++k) {
                        ivec2 const t = tile_of(head) + ivec2{rng.range(-12, 13), rng.range(-12, 13)};
                        if (open_tile(state, room, t)) {
                            ai.unstuck_goal = t;
                            break;
                        }
                    }
                }
                if (ai.unstuck_goal.x >= 0)
                    dest = ai.unstuck_goal;
                break;
            }
            case behavior::follow_path: {
                // (not vanilla: the abstract AI's time in the room, then on to
                // another room)
                if (ai.abstract_time_in_room > par.abstract_time_to_tire)
                    x.leaving = 1;
                if (x.current_node < 0 || uint32_t(x.current_node) >= nodes.count || (ai.used_nodes & (1u << uint32_t(x.current_node))))
                    x.current_node = int8_t(closest_node(nodes, ai.used_nodes, head));
                if (x.current_node >= 0) {
                    // the nodes not used yet, in order (pathfindingNodes)
                    uint32_t avail[max_nodes];
                    uint32_t na = 0;
                    for (uint32_t i = 0; i < nodes.count; ++i)
                        if (!(ai.used_nodes & (1u << i)))
                            avail[na++] = i;
                    if (x.test_node_index >= na)
                        x.test_node_index = 0;
                    auto const& cn = nodes.n[uint32_t(x.current_node)];
                    vec2 vector = closest_point_on_segment(cn.pos, cn.pos + cn.handle, head);
                    float num = rw::distance(head, vector);
                    if (na > 0) {
                        uint32_t const ti = avail[x.test_node_index];
                        auto const& tn = nodes.n[ti];
                        vec2 const vector2 = closest_point_on_segment(tn.pos, tn.pos + tn.handle, head);
                        float const num2 = rw::distance(head, vector2);
                        if (uint32_t(x.current_node) != ti && num2 < num) {
                            x.current_node = int8_t(ti);
                            num = num2;
                            vector = vector2;
                        }
                    }
                    ++x.test_node_index;
                    ai.current_node_pos = vector;
                    if (custom::manhattan_distance(tile_of(vector), dest) > 1)
                        dest = tile_of(vector);
                    if (num < 100.f) {
                        ai.used_nodes |= 1u << uint32_t(x.current_node);
                        x.current_node = int8_t(closest_node(nodes, ai.used_nodes, head));
                    }
                }
                // all of them done: around again (sooner with a slugcat on
                // its whiskers)
                bool any_left = false;
                for (uint32_t i = 0; i < nodes.count; ++i)
                    any_left = any_left || !(ai.used_nodes & (1u << i));
                if (!any_left) {
                    bool climbed = false;
                    auto const& ws = w.sky_whale_data_get_whiskers(d);
                    for (uint32_t i = 0; i < ws.count; ++i)
                        climbed = climbed || ws.w[i].time_since_climbed <= 1;
                    if (climbed || rng.value() < 0.4f)
                        ai.used_nodes = 0;
                }
                break;
            }
            }
            if (b == behavior::leave || b == behavior::escape_rain)
                ++ai.leave_counter;
        }

        // SkyWhale.FlyTowardsTarget
        void fly_towards_target(sys::game_state& state, dcon::creature_id c, float power, float angle) {
            auto& x = pl(state, c);
            vel(state, c, 0) += x.fly_dir * 0.4f * power;
            vec2 const v = custom::dir_vec(pos(state, c, 1), pos(state, c, 0));
            vel(state, c, 0) += v * 0.5f * mathf::lerp(0.5f, 1.f, power) * 3.25f * mathf::lerp(1.f, 0.75f, angle / 30.f);
            float const d = mathf::clamp(signed_angle(v, x.fly_dir) / 90.f, -1.5f, 1.5f);
            vec2 const perp = custom::perpendicular_vector(v);
            vel(state, c, 0) += perp * d;
            vel(state, c, 1) -= perp * d;
        }

        // SkyWhale.Act
        void act(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            ai_update(state, c, room);
            auto& x = pl(state, c);
            auto const& ai = w.sky_whale_data_get_ai(data(state, c));
            behavior const b = behavior(w.creature_get_behavior(c));
            // idle/flee/travel speed factors are all 1
            float const power = x.flying_power;
            vec2 const head = pos(state, c, 0);
            vec2 target;
            switch (b) {
            case behavior::follow_path: target = ai.current_node_pos; break;
            case behavior::escape_rain:
            case behavior::leave: target = leave_target(state, c, room); break;
            default: target = room_view::middle_of_tile(w.creature_get_destination(c)); break;
            }
            auto vector = steer_point(state, c, room, target);
            x.flying_power = mathf::lerp(x.flying_power, 1.f, 0.1f);
            if (vector) {
                vec2 v = custom::dir_vec(head, *vector);
                // over the border: straight out (AllowMovementBetweenRooms)
                if (x.leaving && x.leave_side < 0 && head.x <= 0.f)
                    v = vec2{-1.f, 0.f};
                else if (x.leaving && x.leave_side > 0 && head.x >= room.pixel_width())
                    v = vec2{1.f, 0.f};
                if (b == behavior::follow_path) {
                    ivec2 const ahead = tile_of(head + custom::dir_vec(pos(state, c, 1), head) * 100.f);
                    ivec2 const nt = tile_of(ai.current_node_pos);
                    if (!room.inside(ahead.x, ahead.y) && !room.inside(nt.x, nt.y)) {
                        *vector = ai.current_node_pos;
                        v = custom::dir_vec(head, *vector);
                    }
                }
                x.fly_dir = rotate_towards(x.fly_dir, v, turn_degrees_per_tick * mathf::deg2rad * (b == behavior::follow_path ? 0.5f : 1.f), 1.f);
                float const angle = rw::angle(custom::dir_vec(pos(state, c, 1), pos(state, c, 0)), v);
                if (b == behavior::get_unstuck) {
                    vec2 const to = *vector;
                    vec2 const d0 = custom::dir_vec(pos(state, c, 0), to);
                    vel(state, c, 0) += d0 * 2.f;
                    pos(state, c, 0) += d0 * 2.f;
                    vel(state, c, 1) += custom::dir_vec(pos(state, c, 1), to) * 1.2f;
                } else {
                    fly_towards_target(state, c, power, angle);
                }
            } else {
                // no way on: backing up
                vec2 const a = custom::dir_vec(pos(state, c, 0), pos(state, c, 1));
                vel(state, c, 0) += a * 3.f;
                pos(state, c, 0) += a * 3.f;
                vel(state, c, 1) += a * 2.f;
            }
        }

        //
        // SkyWhaleWhisker
        //
        vec2 rotation_of(sys::game_state const& state, dcon::creature_id c, uint32_t k) {
            return rw::normalized(pos(state, c, k) - pos(state, c, rotation_chunk[k]));
        }
        // GlobalAttachmentPos
        vec2 attachment_pos(sys::game_state const& state, dcon::creature_id c, whisker const& wh) {
            vec2 const r = rotation_of(state, c, 0);
            vec2 const a{-r.y, r.x};
            float const rad = state.world.body_chunk_get_rad(chunk(state, c, 0));
            return pos(state, c, 0) + a * (rad - 5.f) * std::sin(pl(state, c).roll * mathf::pi / 2.f + wh.side * mathf::pi / 2.f);
        }
        void reset_whiskers(sys::game_state& state, dcon::creature_id c) {
            auto& ws = state.world.sky_whale_data_get_whiskers(data(state, c));
            for (uint32_t i = 0; i < ws.count; ++i) {
                auto& wh = ws.w[i];
                vec2 const at = attachment_pos(state, c, wh);
                for (uint32_t k = 0; k < wh.segments; ++k) {
                    wh.pos[k] = wh.last_pos[k] = at;
                    wh.vel[k] = vec2{0.f, 0.f};
                }
            }
        }
        // SharedPhysics.VerticalCollision / HorizontalCollision of a segment
        // (rad 2), simplified to the tiles: it is moved back out of a solid
        // tile along the axis that frees it; the contact damps the other
        // axis' velocity
        void whisker_terrain(room_view const& room, vec2& p, vec2 last, vec2& v) {
            auto const solid = [&](vec2 q) { return room.get_tile(q).solid(); };
            if (!solid(p))
                return;
            if (!solid(vec2{last.x, p.y})) {
                p.x = last.x;
                v.x = 0.f;
                v.y *= 0.6f;
            } else if (!solid(vec2{p.x, last.y})) {
                p.y = last.y;
                v.y = 0.f;
                v.x *= 0.6f;
            } else {
                p = last;
                v = vec2{0.f, 0.f};
            }
        }
        // ConnectSegments
        void connect_segments(whisker& wh, uint32_t a, uint32_t b) {
            vec2 const n = rw::normalized(wh.pos[a] - wh.pos[b]);
            float const num = rw::distance(wh.pos[a], wh.pos[b]);
            float const d = mathf::inverse_lerp(0.f, whisker_connection_rad, num);
            if (num > whisker_connection_rad) {
                vec2 const m = n * (whisker_connection_rad - num) * 0.5f * d;
                wh.pos[a] += m;
                wh.vel[a] += m;
                wh.pos[b] -= m;
                wh.vel[b] -= m;
            }
        }
        // SkyWhaleWhisker.Update
        void whiskers_update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& ws = w.sky_whale_data_get_whiskers(data(state, c));
            if (ws.count == 0)
                return;
            vec2 const rotation = rotation_of(state, c, 0);
            vec2 const v0 = w.body_chunk_get_vel(chunk(state, c, 0));
            float const roll = pl(state, c).roll;
            for (uint32_t i = 0; i < ws.count; ++i) {
                auto& wh = ws.w[i];
                if (wh.time_since_climbed < 1000000)
                    ++wh.time_since_climbed;
                vec2 const a = vec2{-rotation.y, rotation.x} * std::sin(roll * mathf::pi / 2.f + mathf::pi / 2.f * wh.side);
                uint32_t const n = wh.segments;
                for (uint32_t k = 0; k < n; ++k) {
                    wh.vel[k].y -= 0.9f * room.gravity;
                    float const t = n > 1 ? float(k) / float(n - 1) : 0.f;
                    wh.vel[k] = rw::lerp(wh.vel[k], v0, t * 0.1f);
                    wh.vel[k] += (a - rotation * (1.f + wh.angle)) * t;
                    wh.last_pos[k] = wh.pos[k];
                    wh.pos[k] += wh.vel[k];
                    wh.vel[k] *= 0.95f;
                    if (k > 0 && pathing::terrain_proximity(room, tile_of(wh.pos[k]), 4) < 3)
                        whisker_terrain(room, wh.pos[k], wh.last_pos[k], wh.vel[k]);
                }
                vec2 const at = attachment_pos(state, c, wh);
                auto const connect_ends = [&] {
                    wh.pos[0] = at;
                    wh.vel[0] = v0;
                };
                connect_ends();
                for (uint32_t j = n - 1; j > 0; --j)
                    connect_segments(wh, j, j - 1);
                connect_ends();
                for (uint32_t k = 1; k < n; ++k)
                    connect_segments(wh, k, k - 1);
                connect_ends();
            }
        }

        // AddWhiskers (in the constructor's Random state, seed + 1)
        void add_whiskers(whisker_set& ws, creature_params const& p, rw::unity_random& r) {
            ws.count = whisker_count;
            for (uint32_t i = 0; i < whisker_count; ++i) {
                auto& wh = ws.w[i];
                wh = whisker{};
                bool const flag = r.value() < p.ruggedness * 0.15f;
                wh.side = float(i % 2) * 2.f - 1.f;
                float const num = float(i / 2);
                wh.angle = num + r.range(-0.3f, 0.3f) * p.ruggedness;
                int32_t num2 = num == 1.f ? 17 : 14;
                // Mathf.RoundToInt: to even
                num2 = int32_t(std::nearbyint(float(num2) * (1.f + r.range(-0.3f, 0.3f) * p.ruggedness)));
                if (flag)
                    num2 /= 2;
                wh.segments = uint8_t(custom::int_clamp(num2, 1, int32_t(max_whisker_segments)));
                if (flag)
                    wh.end_radius = r.range(0.3f, 1.f);
            }
        }

        //
        // GasJet
        //
        void jets_update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& js = w.sky_whale_data_get_jets(data(state, c));
            bool const viewed = state.viewed(room.id);
            auto& rng = state.room_random(room.id);
            for (auto& j : js.j) {
                if (j.life <= 0)
                    continue;
                vec2 const rotation = rotation_of(state, c, j.chunk);
                vec2 const a = custom::perpendicular_vector(rotation);
                vec2 const at = rotation * j.rel_pos.x + a * j.rel_pos.y + pos(state, c, j.chunk);
                vec2 const dir = rotation * j.rel_dir.x + a * j.rel_dir.y;
                float const num = float(j.life) / float(j.max_life);
                // (NewVultureSmoke, or Bubbles under water: puffs of dust;
                // no Vulture_Jet_Start/Stop sounds)
                if (viewed && (j.life & 1) == 0) {
                    if (room.point_submerged(at))
                        particles::bubbles(state, room.id, at, 1, 6.f);
                    else
                        particles::spawn(state, room.id, particles::kind::dust, at, dir * 8.f * num + custom::rnv(rng) * 0.5f, mathf::lerp(6.f, 14.f, num), 1.2f,
                            rw::vec3{0.75f, 0.75f, 0.78f}, 0.5f * num);
                }
                --j.life;
            }
        }

        //
        // Off screen
        //
        void park(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            for (uint32_t i = 0; i < chunk_count; ++i) {
                auto const k = chunk(state, c, i);
                physics::hard_set_position(state, k, vec2{room.pixel_width() * 0.5f + float(i) * 20.f, room.pixel_height() + park_height});
                state.world.body_chunk_set_vel(k, vec2{0.f, 0.f});
            }
        }
        void go_hidden(sys::game_state& state, dcon::creature_id c, room_view const& room, int32_t wait) {
            auto& x = pl(state, c);
            x.hidden = 1;
            x.leaving = 0;
            x.leave_side = 0;
            x.hidden_timer = int16_t(std::min(wait, 30000));
            park(state, c, room);
        }

        // a height to fly in at over the room's `side` border (-1 left):
        // open tiles 8 deep into the room (the AltVariant's high up, like
        // SkyWhaleAbstractAI.PlaceInRealizedRoom)
        int32_t entry_height(room_view const& room, int32_t side, bool alt, rw::rng& rng) {
            int32_t const lo = alt ? std::max(1, room.height - 10) : std::max(1, int32_t(float(room.height) * 0.25f));
            int32_t const hi = std::max(lo + 1, room.height - 2);
            auto const open = [&](int32_t y) {
                for (int32_t dx = 0; dx < 8; ++dx) {
                    int32_t const tx = side < 0 ? dx : room.width - 1 - dx;
                    for (int32_t dy = -2; dy <= 2; ++dy)
                        if (room.get_tile(tx, y + dy).solid())
                            return false;
                }
                return true;
            };
            for (int k = 0; k < 40; ++k) {
                int32_t const y = rng.range(lo, hi);
                if (open(y))
                    return y;
            }
            for (int32_t y = room.height - 3; y > 1; --y)
                if (open(y))
                    return y;
            return std::max(1, room.height - 3);
        }

        // back in over a side of `room` (serial: the deferred room change)
        void enter_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            auto& x = pl(state, c);
            auto const from = creature::room_of(state, c);
            auto const view = world::get_room_view(state, room);
            w.physical_object_set_room_from_object_location(body(state, c), room);
            int32_t const side = rng.value() < 0.5f ? -1 : 1;
            int32_t const y = entry_height(view, side, x.alt != 0, rng);
            float const fs = float(side);
            vec2 const head{side < 0 ? -60.f : view.pixel_width() + 60.f, room_view::middle_of_tile(0, y).y};
            float const spacing = 15.f * x.size;
            for (uint32_t i = 0; i < chunk_count; ++i) {
                auto const k = chunk(state, c, i);
                physics::hard_set_position(state, k, head + vec2{fs * float(i) * spacing, 0.f});
                w.body_chunk_set_vel(k, vec2{-fs * 3.f, 0.f});
            }
            x.fly_dir = vec2{-fs, 0.f};
            x.hidden = 0;
            x.hidden_timer = 0;
            new_room_ai(state, c, view);
            reset_whiskers(state, c);
            if (from && from != room)
                dens::changed_rooms(state, c, from, room, false);
            creature::reset_graphics(state, c);
        }

        // the SkyWhaleAbstractAI's next room: one of the region's rooms with
        // nodes (its own if there is none)
        void fly_in(sys::game_state& state, dcon::creature_id c) {
            auto& w = state.world;
            auto const from = creature::room_of(state, c);
            auto const rooms = roam_rooms(state, from ? w.room_get_region_from_region_membership(from) : dcon::region_id{});
            dcon::room_id target = from;
            if (!rooms.empty())
                target = rooms[size_t(w.creature_get_random(c).range(0, int32_t(rooms.size())))];
            if (target)
                enter_room(state, c, target);
        }

        void offscreen_update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& x = pl(state, c);
            park(state, c, room);
            // out of the rain and of everything else
            w.creature_set_rain_death(c, 0.f);
            w.creature_set_stun(c, 0);
            if (x.hidden_timer > 0) {
                --x.hidden_timer;
                return;
            }
            // WantToStayInDenUntilEndOfCycle
            if (w.creature_get_dead(c) || dens::rain_utility(state, c, room.id) > 0.01f) {
                x.hidden_timer = 200;
                return;
            }
            x.hidden_timer = 1000;
            state.defer(room.id, [c](sys::game_state& st) {
                if (st.world.creature_is_valid(c) && st.world.creature_get_type(c) == creature::type::sky_whale && pl(st, c).hidden)
                    fly_in(st, c);
            });
        }

        // GenerateIVars
        creature_params generate_ivars(int32_t seed, bool alt) {
            creature_params p;
            rw::unity_random r{seed};
            float const value = r.value();
            p.size_multiplier = 3.75f + 0.5f * value;
            float const b = r.value();
            float const t = r.value();
            p.ruggedness = mathf::pow(mathf::lerp(value, b, t), 1.5f);
            p.fin_sharpness = r.value();
            if (alt) {
                p.size_multiplier *= 1.5f;
                p.fin_sharpness *= 0.2f;
            }
            return p;
        }
    }

    creature_params const& params(sys::game_state const& state, dcon::creature_id c) noexcept {
        return state.world.sky_whale_data_get_params(data(state, c));
    }
    bool alt_variant(sys::game_state const& state, dcon::creature_id c) noexcept {
        return pl(state, c).alt != 0;
    }
    bool hidden(sys::game_state const& state, dcon::creature_id c) noexcept {
        return pl(state, c).hidden != 0;
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 at, bool alt) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::sky_whale);
        auto const d = data(state, c);
        rw::rng rng{state.random.next() | 1u};
        int32_t seed = int32_t(rng.next() & 0x7FFFFFFFu);
        if (state.spawn_seed >= 0)
            seed = state.spawn_seed;
        creature_params par = generate_ivars(seed, alt);
        // (SkyWhaleAbstractAI.timeToTire: the personality's energy rolled here)
        par.abstract_time_to_tire = int32_t(std::lround(1700.f + 4000.f * (1.f - rng.value())));
        float const size = par.size_multiplier;
        physics::object_desc desc;
        desc.gravity = 0.f;
        desc.air_friction = 0.9f;
        desc.bounce = 0.1f;
        desc.surface_friction = 0.85f;
        desc.water_friction = 0.96f;
        desc.buoyancy = 0.95f;
        auto const o = physics::create_object(state, room, desc);
        // never inside terrain
        auto const view = world::get_room_view(state, room);
        if (view.get_tile(at).solid()) {
            ivec2 const t0 = tile_of(at);
            int best = 1 << 30;
            for (int dy = -8; dy <= 8; ++dy)
                for (int dx = -8; dx <= 8; ++dx) {
                    ivec2 const t = t0 + ivec2{dx, dy};
                    if (view.inside(t.x, t.y) && !view.get_tile(t).solid() && dx * dx + dy * dy < best) {
                        best = dx * dx + dy * dy;
                        at = room_view::middle_of_tile(t);
                    }
                }
        }
        // six chunks (8, 10, 10.5, 10, 9, 8 times the size), a sixth of the
        // mass each, laid out head first along x
        constexpr float rads[chunk_count] = {8.f, 10.f, 10.5f, 10.f, 9.f, 8.f};
        dcon::body_chunk_id ks[chunk_count];
        for (uint32_t i = 0; i < chunk_count; ++i) {
            ks[i] = physics::add_chunk(state, o, at + vec2{-float(i) * 15.f * size, 0.f}, rads[i] * size, total_mass / float(chunk_count));
            // restrictInRoomRange 2400
            w.body_chunk_set_restrict_in_room_range(ks[i], 2400.f);
        }
        for (uint32_t i = 0; i + 1 < chunk_count; ++i)
            physics::add_connection(state, o, ks[i], ks[i + 1], 15.f * size, physics::connection_type::normal, 0.7f, -1.f);
        for (uint32_t i = 0; i + 2 < chunk_count; ++i)
            physics::add_connection(state, o, ks[i], ks[i + 2], 23.f * size, physics::connection_type::push, 0.7f, -1.f);
        physics::set_go_through_floors(state, o, true);
        w.try_create_creature_body(c, o);
        auto& x = pl(state, c);
        x.fly_dir = vec2{0.f, 0.f};
        x.roll = x.last_roll = 0.f;
        x.roll_offset = x.target_roll_offset = x.roll_offset_vel = 0.f;
        x.flying_power = 0.f;
        x.size = size;
        x.alt = alt ? 1 : 0;
        x.hidden = 0;
        x.hidden_timer = 0;
        x.data = uint16_t(d.index());
        w.creature_set_random(c, rng);
        w.creature_set_graphics_random(c, rw::rng{(rng.state ^ 0x5C7A11E5u) | 1u});
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_set_grasps(c, creature::grasp_pair{});
        w.creature_set_current_utility(c, 0.f);
        w.sky_whale_data_set_seed(d, seed);
        w.sky_whale_data_set_params(d, par);
        whisker_set ws;
        if (!alt) {
            rw::unity_random r{seed + 1};
            add_whiskers(ws, par, r);
        }
        w.sky_whale_data_set_whiskers(d, ws);
        w.sky_whale_data_set_jets(d, gas_jets{});
        w.sky_whale_data_set_layout(d, sky_whale_graphics::make_layout(par, alt, seed));
        w.sky_whale_data_set_graphics(d, graphics_state{});
        new_room_ai(state, c, view);
        reset_whiskers(state, c);
        tracker::clear(state, c);
        sky_whale_graphics::reset_positions(state, c);
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        creature::release_target(state, c);
        creature::erase(state, c);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto& x = pl(state, c);
        if (x.hidden) {
            offscreen_update(state, c, room);
            return;
        }
        auto& rng = w.creature_get_random(c);
        auto const o = body(state, c);
        // Creature.Update's HealthState: hurt, it may die or faint, else heals
        if (!w.creature_get_dead(c)) {
            float& health = w.creature_get_health(c);
            if (health < 0.f && rng.value() < -health && rng.value() < 1.f / float(consious(state, c) ? 80 : 800))
                creature::die(state, c);
            if (rng.value() < 0.033333335f && (rng.value() * 0.2f > health || rng.value() < -health))
                creature::stun(state, c, rng.range(1, rng.range(1, 27 - custom::int_clamp(int32_t(20.f * health), 0, 10))));
            if (health > 0.f && health < 1.f && rng.value() < 0.01f)
                health = mathf::min(1.f, health + 1.f / mathf::lerp(550.f, 70.f, health));
        }
        creature::stun_countdown(state, c);
        // terrainSqueeze: squeezing through while getting unstuck
        float squeeze = behavior(w.creature_get_behavior(c)) == behavior::get_unstuck ? 0.5f : 1.f;
        if (x.alt)
            squeeze /= 1.5f;
        for (uint32_t i = 0; i < chunk_count; ++i)
            w.body_chunk_set_terrain_squeeze(chunk(state, c, i), squeeze);
        // PhysicalObject.Update (no terrain impact sounds)
        physics::update_object(state, room, o, nullptr, nullptr);
        // the roll
        x.last_roll = x.roll;
        x.roll_offset = smooth_damp(x.roll_offset, x.target_roll_offset, x.roll_offset_vel, 0.5f, 0.5f, 0.025f);
        if (rng.value() < 0.035f) {
            if (rng.value() < 0.3f)
                x.target_roll_offset = 0.f;
            else
                x.target_roll_offset = rng.value() * 0.75f + (x.target_roll_offset > 0.5f ? 0.f : 0.25f);
        }
        float const to = mathf::lerp(rw::normalized(pos(state, c, 1) - pos(state, c, 2)).x, 0.f, x.roll_offset * 0.35f);
        x.roll = custom::lerp_and_tick(x.roll, to, 0.1f, 0.05f);
        bool const awake = consious(state, c);
        w.physical_object_set_air_friction(o, awake ? 0.9f : 0.98f);
        // (Sky_Whale_Cry and Sky_Whale_Click have no sound ids)
        if (awake)
            act(state, c, room);
        else
            x.fly_dir = rw::normalized(pos(state, c, 0) - pos(state, c, 1));
        whiskers_update(state, c, room);
        jets_update(state, c, room);
        // out over its side: off screen until it flies back in
        if (x.leaving && awake) {
            auto const& ai = w.sky_whale_data_get_ai(data(state, c));
            bool out = true;
            for (uint32_t i = 0; i < chunk_count && out; ++i) {
                float const r = w.body_chunk_get_rad(chunk(state, c, i));
                float const px = pos(state, c, i).x;
                out = x.leave_side < 0 ? px + r < -20.f : x.leave_side > 0 && px - r > room.pixel_width() + 20.f;
            }
            // (not vanilla: one that can't find its way out goes all the same)
            if (out || ai.leave_counter > 2400)
                go_hidden(state, c, room, rng.range(400, 1600));
        }
    }

    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id) {
        return !hidden(state, c);
    }

    void collide(sys::game_state&, dcon::creature_id, room_view const&, uint32_t, dcon::creature_id, uint32_t) {
        // SkyWhale.Collide: CreatureParams.stunOnCollideVelocity is 0, the
        // Violence on what it bumps never happens; the push apart is the
        // room's
    }

    void violence(sys::game_state& state, dcon::creature_id c, uint32_t k, vec2 momentum, float damage, float stun_bonus, creature::damage_type type) {
        auto& w = state.world;
        if (hidden(state, c))
            return;
        auto& rng = w.creature_get_random(c);
        k = k < chunk_count ? k : 1u;
        auto const hit = chunk(state, c, k);
        vec2 const hp = w.body_chunk_get_pos(hit);
        float const rad = w.body_chunk_get_rad(hit);
        // a stab lets out gas where it went in (the source is where the
        // momentum came from)
        if (type == creature::damage_type::stab && rw::magnitude(momentum) > 0.f) {
            vec2 const dir = -rw::normalized(momentum);
            vec2 const at = hp + dir * rad * 0.95f;
            vec2 const rotation = rotation_of(state, c, k);
            vec2 const perp = custom::perpendicular_vector(rotation);
            auto& js = w.sky_whale_data_get_jets(data(state, c));
            gas_jet* slot = &js.j[0];
            for (auto& j : js.j)
                if (j.life < slot->life)
                    slot = &j;
            slot->chunk = uint8_t(k);
            slot->max_life = slot->life = int16_t(rng.range(80, 120));
            slot->rel_pos = vec2{rw::dot(at - hp, rotation), rw::dot(at - hp, perp)};
            slot->rel_dir = vec2{rw::dot(dir, rotation), rw::dot(dir, perp)};
        }
        // (no Sky_Whale_Groan sound id)
        // Creature.Violence (HealthState)
        w.body_chunk_get_vel(hit) += rw::clamp_magnitude(momentum / w.body_chunk_get_mass(hit), 10.f);
        float const dmg = damage / base_damage_resistance;
        float st = (damage * 30.f + stun_bonus) / base_stun_resistance;
        float& health = w.creature_get_health(c);
        st *= 1.5f + mathf::inverse_lerp(0.5f, 0.f, health) * rng.value();
        creature::stun(state, c, int32_t(st));
        health -= dmg;
        if (rng.value() < -health || health < -1.f || (health < 0.f && rng.value() < 0.33f))
            creature::die(state, c);
    }

    void release(sys::game_state&, dcon::creature_id, dcon::creature_id) {
        // it holds nothing (grasps 0); the slugcats let go of its whiskers
        // through rot::vines::valid
    }

    void died(sys::game_state&, dcon::creature_id) {
        // (Creature.Die: it drifts, airFriction 0.98 from the next update)
    }

    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const view = world::get_room_view(state, room);
        ivec2 const t = tile_of(at);
        vec2 const a = rw::to_vec2(view.shortcut_entrance_hole_direction(t));
        vec2 const mid = room_view::middle_of_tile(t);
        for (uint32_t i = 0; i < chunk_count; ++i) {
            auto const k = chunk(state, c, i);
            physics::hard_set_position(state, k, mid - a * (-1.5f + float(i)) * 15.f);
            w.body_chunk_set_last_pos(k, mid);
            w.body_chunk_set_vel(k, a * 2.f);
        }
        pl(state, c).hidden = 0;
        new_room_ai(state, c, view);
        reset_whiskers(state, c);
    }

    std::string describe(sys::game_state const& state, dcon::creature_id c) {
        static constexpr char const* names[] = {"idle", "follow path", "escape rain", "get unstuck", "leave"};
        auto const& w = state.world;
        auto const& x = pl(state, c);
        std::string const kind = x.alt ? "sky whale (alt)" : "sky whale";
        if (x.hidden)
            return kind + " off screen (back in " + std::to_string(int(x.hidden_timer)) + " ticks)";
        vec2 const p = pos(state, c, 1);
        uint32_t const b = w.creature_get_behavior(c);
        bool climbed = false;
        auto const& ws = w.sky_whale_data_get_whiskers(data(state, c));
        for (uint32_t i = 0; i < ws.count; ++i)
            climbed = climbed || ws.w[i].time_since_climbed <= 1;
        return kind + " at " + std::to_string(int(p.x)) + "," + std::to_string(int(p.y)) + " behavior " + (b < 5 ? names[b] : "?")
            + (x.current_node >= 0 ? " node " + std::to_string(int(x.current_node)) : std::string{}) + (climbed ? " climbed on" : "");
    }

    bool spawn_spot(room_view const& room, ivec2 tile) {
        if (!room.s || !room.inside(tile.x, tile.y) || room.get_tile(tile).solid())
            return false;
        if (!has_nodes(*room.s, room.id))
            return false;
        return pathing::terrain_proximity(room, tile, 6) >= 6;
    }

    uint32_t spawn_offscreen(sys::game_state& state, dcon::region_id region, spawns::spawn const& s, spawns::report& r) {
        auto& w = state.world;
        auto const rooms = roam_rooms(state, region);
        if (rooms.empty()) {
            ++r.no_room;
            if (r.missing.size() < 8)
                r.missing.push_back(s.room + ":" + std::to_string(s.den) + " " + s.type);
            return 0;
        }
        auto const sd = spawns::parse_data(s.data);
        uint8_t flags = sd.flags;
        if (s.night)
            flags = uint8_t((flags | dens::flag_night) & ~dens::flag_ignore_cycle);
        bool const alt = (flags & dens::flag_alternate_form) != 0;
        uint32_t made = 0;
        int32_t const n = std::max(1, s.count);
        for (int32_t i = 0; i < n; ++i) {
            // RainWorldGame.GetNewID: the next number, the seed unless {Seed:N}
            int32_t const number = ++state.next_issued_id;
            int32_t const seed = sd.seed >= 0 ? sd.seed : number;
            auto const room = rooms[size_t(uint32_t(seed) * 2654435761u % uint32_t(rooms.size()))];
            auto const saved = state.random;
            state.random = rw::rng{uint32_t(seed) * 0x9E3779B1u ^ 0x5BD1E995u};
            if (state.random.state == 0)
                state.random.state = 0x2545F491u;
            state.spawn_seed = seed;
            auto const view = world::get_room_view(state, room);
            auto const c = create(state, room, vec2{view.pixel_width() * 0.5f, view.pixel_height() * 0.5f}, alt);
            state.spawn_seed = -1;
            state.random = saved;
            if (!c)
                continue;
            w.creature_set_spawn_flags(c, flags);
            // AbstractCreature's constructor: out of the den in 100 to 1000 ticks
            go_hidden(state, c, view, w.creature_get_random(c).range(100, 1000));
            ++made;
            ++r.made[alt ? "sky whale (AlternateForm)" : "sky whale"];
            ++r.creatures;
            r.night += (flags & dens::flag_night) ? 1 : 0;
            r.pre_cycle += (flags & dens::flag_pre_cycle) ? 1 : 0;
            r.ignore_cycle += (flags & dens::flag_ignore_cycle) ? 1 : 0;
        }
        return made;
    }

    //
    // The whiskers (IClimbableVine)
    //
    uint32_t whiskers(sys::game_state const& state, dcon::creature_id c) noexcept {
        return state.world.sky_whale_data_get_whiskers(data(state, c)).count;
    }

    uint32_t vine_positions(sys::game_state const& state, dcon::creature_id c, uint32_t i) noexcept {
        auto const& ws = state.world.sky_whale_data_get_whiskers(data(state, c));
        if (i >= ws.count || ws.w[i].segments <= forbidden_segments)
            return 0;
        return ws.w[i].segments - forbidden_segments;
    }

    bool vine_climbable(sys::game_state const& state, dcon::creature_id c, uint32_t i) noexcept {
        // (CurrentlyClimbable: in the active ripple layer, always here)
        return !hidden(state, c) && state.world.creature_get_away(c) == 0 && vine_positions(state, c, i) >= 2;
    }

    vec2 vine_pos(sys::game_state const& state, dcon::creature_id c, uint32_t i, uint32_t k) noexcept {
        auto const& ws = state.world.sky_whale_data_get_whiskers(data(state, c));
        uint32_t const s = std::min(k + forbidden_segments, uint32_t(ws.w[i].segments) - 1u);
        return ws.w[i].pos[s];
    }

    void vine_push(sys::game_state& state, dcon::creature_id c, uint32_t i, uint32_t k, vec2 movement) {
        auto& ws = state.world.sky_whale_data_get_whiskers(data(state, c));
        if (i >= ws.count || k + forbidden_segments >= ws.w[i].segments)
            return;
        ws.w[i].vel[k + forbidden_segments] += movement;
    }

    void being_climbed_on(sys::game_state& state, dcon::creature_id c, uint32_t i) {
        auto& ws = state.world.sky_whale_data_get_whiskers(data(state, c));
        if (i < ws.count)
            ws.w[i].time_since_climbed = 0;
    }
}
