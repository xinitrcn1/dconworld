#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>

#include "barnacle.hpp"
#include "barnacle_graphics.hpp"
#include "dens.hpp"
#include "items.hpp"
#include "migrate.hpp"
#include "particles.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "rooms.hpp"
#include "tracker.hpp"

namespace barnacle {
    using creature::relationship;
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using world::room_view;
    using world::terrain;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // CreatureTemplate (Barnacle): baseDamageResistance,
        // baseStunResistance, instantDeathDamageLimit, visualRadius,
        // throughSurfaceVision (waterVision 1 takes nothing off)
        constexpr float base_damage_resistance = 0.6f;
        constexpr float base_stun_resistance = 0.5f;
        constexpr float instant_death_damage_limit = 1.f;
        constexpr float visual_radius = 300.f;
        constexpr float through_surface_vision = 0.5f;
        constexpr float float_max = std::numeric_limits<float>::max();

        creature::barnacle_payload& pl(sys::game_state& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).barnacle;
        }
        creature::barnacle_payload const& pl(sys::game_state const& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).barnacle;
        }
        dcon::barnacle_data_id data(sys::game_state const& state, dcon::creature_id c) {
            return creature::barnacle_data(state.world, c);
        }
        creature_params const& prm(sys::game_state const& state, dcon::creature_id c) {
            return state.world.barnacle_data_get_params(data(state, c));
        }
        body_state& bs(sys::game_state& state, dcon::creature_id c) {
            return state.world.barnacle_data_get_body(data(state, c));
        }
        ai_state& ai(sys::game_state& state, dcon::creature_id c) {
            return state.world.barnacle_data_get_ai(data(state, c));
        }
        vec2& pos(sys::game_state& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_pos(chunk(state, c, i));
        }
        vec2 pos(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_pos(chunk(state, c, i));
        }
        vec2& vel(sys::game_state& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_vel(chunk(state, c, i));
        }
        ivec2 tile_of(vec2 p) {
            return room_view::get_tile_position(p);
        }
        // abstractCreature.pos: the main chunk's (the shell's) tile
        ivec2 here(sys::game_state const& state, dcon::creature_id c) {
            return tile_of(pos(state, c, 0));
        }
        // AImap.TileAccessibleToCreature
        bool accessible(room_view const& room, ivec2 t) {
            return room.inside(t.x, t.y) && pathing::accessible(room, t, walk);
        }
        bool deep_water(room_view const& room, vec2 p) {
            return room.get_tile(p).water == 1;
        }
        // PhysicalObject.Submersion, Barnacle.Swimming, Footing
        float submersion(sys::game_state const& state, room_view const& room, dcon::creature_id c) {
            return physics::object_submersion(state, room, body(state, c));
        }
        bool swimming(sys::game_state const& state, room_view const& room, dcon::creature_id c) {
            return submersion(state, room, c) > 0.3f;
        }
        bool footing(sys::game_state const& state, dcon::creature_id c) {
            return pl(state, c).footing_counter > 20;
        }
        behavior behavior_of(sys::game_state const& state, dcon::creature_id c) {
            return behavior(state.world.creature_get_behavior(c));
        }
        // Color.Lerp
        vec3 lerp3(vec3 a, vec3 b, float t) {
            t = mathf::clamp01(t);
            return a + (b - a) * t;
        }
        int16_t tick_down(int16_t v) {
            return int16_t(std::max(int32_t(v) - 1, -1000));
        }

        // Custom.PushFromHalf
        float push_from_half(float v, float e) {
            if (v == 0.5f)
                return 0.5f;
            if (v < 0.5f)
                return custom::lerp_map(v, 0.f, 0.5f, 0.f, 0.5f, e);
            return custom::lerp_map(v, 1.f, 0.5f, 1.f, 0.5f, e);
        }

        // what a barnacle's update asks about its room, with the field to
        // where it is (PathFinder.CoordinateReachableAndGetbackable: walking
        // is taken to be the same both ways), built on the first question
        struct ctx {
            sys::game_state& state;
            dcon::creature_id c;
            room_view const& room;
            rw::rng& rng;
            std::optional<pathing::field_id> from_here;
        };
        bool reachable(ctx& x, ivec2 t) {
            if (!accessible(x.room, t))
                return false;
            if (!x.from_here)
                x.from_here = pathing::field_to(x.state, x.room.id, here(x.state, x.c), walk);
            return pathing::distance(x.state, *x.from_here, t) != pathing::unreachable;
        }
        ivec2 random_tile(ctx& x) {
            int32_t const tx = x.rng.range(0, x.room.width);
            return ivec2{tx, x.rng.range(0, x.room.height)};
        }

        //
        // The herd (BarnacleAI.leader / leading): a follower keeps its
        // leader's id; the leader's `leading` list is every barnacle of the
        // room naming it
        //
        bool is_barnacle(sys::game_state const& state, dcon::creature_id t) {
            return creature::live(state, t) && creature::is(state.world, t, creature::type::barnacle);
        }
        template<typename F>
        void for_each_follower(sys::game_state const& state, dcon::creature_id c, dcon::room_id room, F&& f) {
            rooms::for_each_creature(state, room, creature::type::barnacle, [&](dcon::creature_id o) {
                if (o != c && pl(state, o).leader == c && creature::here(state, o, room))
                    f(o);
            });
        }
        uint32_t follower_count(sys::game_state const& state, dcon::creature_id c, dcon::room_id room) {
            uint32_t n = 0;
            for_each_follower(state, c, room, [&](dcon::creature_id) { ++n; });
            return n;
        }
        // BarnacleAI.CanBeALeader
        bool can_be_a_leader(sys::game_state const& state, dcon::creature_id b) {
            return is_barnacle(state, b) && !state.world.creature_get_dead(b) && pl(state, b).has_shell && pl(state, b).temporary_panic <= 0;
        }
        // BarnacleAI.AbandonLeader
        void abandon_leader(sys::game_state& state, dcon::creature_id c) {
            if (pl(state, c).leader) {
                pl(state, c).leader = dcon::creature_id{};
                ai(state, c).invalid_leader_time = 0;
            }
        }
        // BarnacleAI.SetGroupDiscomfortTick: the leader's when it has one
        void set_group_discomfort_tick(sys::game_state& state, dcon::creature_id c, float v) {
            auto const leader = pl(state, c).leader;
            if (leader && is_barnacle(state, leader)) {
                float& t = pl(state, leader).next_group_discomfort_tick;
                t = mathf::max(t, v);
                return;
            }
            float& t = pl(state, c).next_group_discomfort_tick;
            t = mathf::max(t, v);
        }

        //
        // BarnacleAI's spots
        //
        // floatGoalPos's setter: the destination goes with it
        void set_float_goal(ctx& x, std::optional<vec2> g) {
            auto& a = ai(x.state, x.c);
            a.has_float_goal = g ? 1 : 0;
            if (g) {
                a.float_goal = *g;
                x.state.world.creature_set_destination(x.c, tile_of(*g));
            }
        }

        // ShufflePanicSpot
        void shuffle_panic_spot(ctx& x, bool scattering) {
            auto& p = pl(x.state, x.c);
            auto& a = ai(x.state, x.c);
            p.scattering = scattering ? 1 : 0;
            a.next_panic_spot_shuffle = scattering ? x.rng.range(40, 200) : x.rng.range(200, 1200);
            ivec2 t = random_tile(x);
            for (int i = 0; i < 20; ++i) {
                if (reachable(x, t)) {
                    a.has_panic_spot = 1;
                    a.panic_spot = room_view::middle_of_tile(t);
                    return;
                }
                a.has_panic_spot = 0;
                t = random_tile(x);
            }
        }

        // UpdateNarrowIdleSpot: a random tile, scored by how narrow it is
        // (lower is better: a gap of one tile between walls the best), away
        // from the blacklisted spot and the room's exits
        void update_narrow_idle_spot(ctx& x) {
            auto const& w = x.state.world;
            auto& a = ai(x.state, x.c);
            auto const& room = x.room;
            ivec2 t = random_tile(x);
            for (int i = 0; i < 20; ++i) {
                if (!reachable(x, t))
                    t = random_tile(x);
                else if (!a.has_blacklist || rw::distance(room_view::middle_of_tile(t), a.blacklist_idle_pos) >= 400.f)
                    break;
            }
            if (!reachable(x, t))
                return;
            bool left = true, right = true, up = true, down = true;
            int32_t num = 0, num2 = 0;
            if (pathing::narrow_space(room, t)) {
                if (room.get_tile(t.x - 1, t.y).solid() && room.get_tile(t.x + 1, t.y).solid()) {
                    num2 = 99;
                    num = 99;
                }
            } else {
                for (int32_t j = 0; j < 5; ++j) {
                    if (left) {
                        if (room.get_tile(t.x - j, t.y).solid())
                            left = false;
                        else
                            ++num2;
                    }
                    if (right) {
                        if (room.get_tile(t.x + j, t.y).solid())
                            right = false;
                        else
                            ++num2;
                    }
                    // (vanilla looks one tile up and down every time)
                    if (up) {
                        if (room.get_tile(t.x, t.y + 1).solid())
                            up = false;
                        else
                            ++num;
                    }
                    if (down) {
                        if (room.get_tile(t.x, t.y - 1).solid())
                            down = false;
                        else
                            ++num;
                    }
                }
            }
            int32_t score = std::min(num2, num);
            if (num2 < num) {
                ++score;
                score *= 3;
            }
            if (a.has_blacklist && rw::distance(room_view::middle_of_tile(t), a.blacklist_idle_pos) < 400.f) {
                ++score;
                score *= 25;
            }
            // (ExitDistanceForCreature <= 5: here five tiles from an exit's
            // pipe entrance, as the crow flies)
            bool near_exit = false;
            w.room_for_each_exit_location(room.id, [&](dcon::exit_location_id el) {
                auto const e = w.exit_location_get_exit(el);
                if (!near_exit && w.room_exit_get_target_from_exit_connection(e) && custom::manhattan_distance(w.room_exit_get_tile(e), t) <= 5)
                    near_exit = true;
            });
            if (near_exit) {
                ++score;
                score *= 10;
            }
            if (!a.has_best_narrow || score < a.best_narrow_score) {
                a.best_narrow_score = score;
                a.has_best_narrow = 1;
                a.best_narrow_pos = room_view::middle_of_tile(t);
            }
        }

        // WaterIdleSpot: a reachable tile under the surface, in a random column
        std::optional<vec2> water_idle_spot(ctx& x) {
            auto const& room = x.room;
            if (!room.water)
                return std::nullopt;
            float const fx = x.rng.value() * room.pixel_width();
            ivec2 const top = tile_of(vec2{fx, room.float_water_level});
            int32_t n = 0;
            for (int32_t j = 0; j <= top.y; ++j)
                if (reachable(x, ivec2{top.x, j}))
                    ++n;
            if (n == 0)
                return std::nullopt;
            int32_t pick = x.rng.range(0, n);
            for (int32_t j = 0; j <= top.y; ++j)
                if (reachable(x, ivec2{top.x, j}) && pick-- == 0)
                    return room_view::middle_of_tile(ivec2{top.x, j});
            return std::nullopt;
        }

        // IdleSpotNearLeader
        std::optional<vec2> idle_spot_near_leader(ctx& x, dcon::creature_id leader, float min_rad, float max_rad) {
            vec2 const lp = pos(x.state, leader, 0);
            for (int i = 0; i < 20; ++i) {
                vec2 const r = custom::rnv(x.rng);
                vec2 const v = lp + r * mathf::lerp(min_rad, max_rad, x.rng.value());
                if (reachable(x, tile_of(v)))
                    return v;
            }
            return std::nullopt;
        }

        // SwitchSubBehavior (no Barnacle_Assemble_Call, Barnacle_Travel_Call)
        void switch_sub_behavior(ctx& x, sub_behavior sb) {
            auto& a = ai(x.state, x.c);
            a.sub = sb;
            a.has_best_narrow = 0;
            a.has_water_idle = 0;
            a.has_blacklist = 0;
            a.in_sub_behavior_time = 0;
            a.in_water_time = 0;
            a.next_sub_behavior_time = 0;
            if (sb == sub_behavior::travel_to_idle) {
                a.next_sub_behavior_time = x.rng.range(3600, 7200);
            } else if (sb == sub_behavior::travel_to_water) {
                a.next_sub_behavior_time = x.rng.range(2800, 4000);
            } else if (sb == sub_behavior::temporary_panic) {
                a.next_sub_behavior_time = pl(x.state, x.c).temporary_panic;
                shuffle_panic_spot(x, pl(x.state, x.c).scattering != 0);
            }
        }

        // UpdateSubBehavior: the leader alternates between the water and a
        // narrow idle spot
        void update_sub_behavior(ctx& x) {
            auto& a = ai(x.state, x.c);
            if (pl(x.state, x.c).leader) {
                switch_sub_behavior(x, sub_behavior::none);
                a.in_sub_behavior_time = 0;
                a.in_water_time = 0;
                return;
            }
            if (a.sub == sub_behavior::none)
                switch_sub_behavior(x, x.rng.value() < 0.5f ? sub_behavior::travel_to_water : sub_behavior::travel_to_idle);
            else
                ++a.in_sub_behavior_time;
            if (a.sub == sub_behavior::travel_to_water) {
                if (submersion(x.state, x.room, x.c) > 0.f)
                    ++a.in_water_time;
                if (!a.has_water_idle) {
                    if (auto const s = water_idle_spot(x)) {
                        a.has_water_idle = 1;
                        a.water_idle_spot = *s;
                    }
                }
                if (!a.has_water_idle && a.in_sub_behavior_time >= 40) {
                    switch_sub_behavior(x, sub_behavior::travel_to_idle);
                    return;
                }
                if (a.has_water_idle && (a.in_water_time >= 200 || a.in_sub_behavior_time > a.next_sub_behavior_time)) {
                    switch_sub_behavior(x, sub_behavior::travel_to_idle);
                    return;
                }
            } else if (a.sub == sub_behavior::travel_to_idle) {
                if (!a.has_best_narrow || (a.in_sub_behavior_time < 80 && a.best_narrow_score > 5))
                    update_narrow_idle_spot(x);
                if (a.in_sub_behavior_time > a.next_sub_behavior_time)
                    switch_sub_behavior(x, sub_behavior::travel_to_water);
            }
        }

        // Barnacle.TryJump
        void try_jump(sys::game_state& state, dcon::creature_id c, vec2 towards) {
            auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
            pl(state, c).delay_before_jumping = 0;
            if (!consious(state, c))
                return;
            vec2 dir = custom::dir_vec((pos(state, c, 0) + pos(state, c, 1)) / 2.f, towards);
            dir += custom::rnv(rng) * 0.3f;
            dir = rw::normalized(dir);
            dir = rw::slerp(dir, vec2{0.f, 1.f}, custom::lerp_map(dir.y, -0.5f, 0.5f, 0.7f, 0.3f));
            for (uint32_t k = 0; k < 2; ++k) {
                vel(state, c, k) *= 0.5f;
                vec2 const r = custom::rnv(rng);
                vel(state, c, k) += dir * 14.f + r * 3.f * rng.value();
            }
            pl(state, c).footing_counter = 0;
            vec2 const side = custom::perpendicular_vector(dir) * (rng.value() < 0.5f ? -1.f : 1.f);
            vel(state, c, 0) += side * 9.f;
            vel(state, c, 1) -= side * 9.f;
        }

        // BarnacleAI.LeadingUpdate: following a leader, or leading (the
        // group's discomfort: at 1 the whole herd scatters), or looking for
        // a herd
        void leading_update(ctx& x) {
            auto& state = x.state;
            auto const c = x.c;
            auto& p = pl(state, c);
            auto& a = ai(state, c);
            vec2 const m = pos(state, c, 0);
            if (p.leader) {
                auto const leader = p.leader;
                // (a leader out of reach, in another room's update, is left)
                if (!is_barnacle(state, leader)) {
                    abandon_leader(state, c);
                } else {
                    p.group_discomfort = pl(state, leader).group_discomfort;
                    bool const valid = can_be_a_leader(state, leader);
                    if (!valid || !creature::here(state, leader, x.room.id) || rw::distance(pos(state, leader, 0), m) > 800.f)
                        ++a.invalid_leader_time;
                    else
                        a.invalid_leader_time = 0;
                    if (!valid || a.invalid_leader_time >= 400)
                        abandon_leader(state, c);
                }
            } else {
                if (p.next_group_discomfort_tick > 0.f) {
                    p.group_discomfort += p.next_group_discomfort_tick;
                    p.next_group_discomfort_tick = 0.f;
                } else {
                    p.group_discomfort = mathf::max(0.f, p.group_discomfort - 0.0125f);
                }
                if (p.group_discomfort >= 1.f) {
                    for_each_follower(state, c, x.room.id, [&](dcon::creature_id f) {
                        pl(state, f).temporary_panic = int16_t(std::max<int32_t>(pl(state, f).temporary_panic, 400));
                        pl(state, f).run_away_time = 400;
                    });
                    if (a.sub != sub_behavior::travel_to_idle) {
                        switch_sub_behavior(x, sub_behavior::travel_to_idle);
                        p.run_away_time = 400;
                        a.has_blacklist = 1;
                        a.blacklist_idle_pos = m;
                        update_narrow_idle_spot(x);
                        for (int n = 0; n < 50 && (!a.has_best_narrow || a.best_narrow_score >= 10); ++n)
                            update_narrow_idle_spot(x);
                        if (a.has_best_narrow)
                            try_jump(state, c, a.best_narrow_pos);
                        else
                            try_jump(state, c, m + custom::rnv(x.rng) * 50.f);
                    }
                    for_each_follower(state, c, x.room.id, [&](dcon::creature_id f) {
                        pl(state, f).delay_before_jumping = int16_t(x.rng.range(1, 120));
                        bs(state, f).jump_target = a.has_best_narrow ? a.best_narrow_pos : pos(state, f, 0) + custom::rnv(x.rng) * 50.f;
                    });
                    p.group_discomfort = 0.f;
                }
            }
            if (p.leader || follower_count(state, c, x.room.id) > 0)
                return;
            // a herd to join, else the loners around follow it
            dcon::creature_id best;
            float best_dist = 0.f;
            dcon::creature_id loners[32];
            uint32_t loner_count = 0;
            rooms::for_each_creature(state, x.room.id, creature::type::barnacle, [&](dcon::creature_id o) {
                if (o == c || !creature::here(state, o, x.room.id) || state.world.creature_get_dead(o))
                    return;
                float const d = rw::distance(pos(state, o, 0), m);
                if (d > 800.f)
                    return;
                uint32_t const followers = follower_count(state, o, x.room.id);
                if (followers > 0 && can_be_a_leader(state, o) && (!best || d < best_dist)) {
                    best = o;
                    best_dist = d;
                } else if (followers == 0 && !pl(state, o).leader && loner_count < 32) {
                    loners[loner_count++] = o;
                }
            });
            if (best) {
                p.leader = best;
                return;
            }
            if (loner_count > 0 && can_be_a_leader(state, c))
                for (uint32_t i = 0; i < loner_count; ++i)
                    pl(state, loners[i]).leader = c;
        }

        // BarnacleAI.FollowLeader: close behind it, or at an idle spot
        // around it (close by when it is near its narrow spot)
        void follow_leader(ctx& x) {
            auto& state = x.state;
            auto const c = x.c;
            auto& p = pl(state, c);
            auto& a = ai(state, c);
            auto const leader = p.leader;
            vec2 const m = pos(state, c, 0);
            vec2 const lp = pos(state, leader, 0);
            float const num = rw::distance(lp, m);
            float const num2 = rw::distance(m, room_view::middle_of_tile(state.world.creature_get_destination(c)));
            ivec2 wc = tile_of(lp);
            if (num > 100.f && num2 > 40.f) {
                p.run_speed = mathf::lerp(p.run_speed, mathf::inverse_lerp(40.f, 180.f, num), 0.8f);
                a.leader_idle_spot_expiry = 0;
            } else {
                p.idle_splash = 1;
                p.run_speed = mathf::lerp(p.run_speed, 0.f, 0.8f);
                --a.leader_idle_spot_expiry;
                bool const leader_near = pl(state, leader).near_narrow_pos != 0;
                if (a.leader_idle_spot_expiry <= 0 || (a.leader_idle_spot_was_distant && leader_near)) {
                    auto const v = leader_near ? idle_spot_near_leader(x, leader, 50.f, 100.f) : idle_spot_near_leader(x, leader, 300.f, 600.f);
                    if (v) {
                        a.leader_idle_spot = *v;
                        a.leader_idle_spot_expiry = leader_near ? x.rng.range(200, 2000) : x.rng.range(400, 2000);
                        a.leader_idle_spot_was_distant = leader_near ? 0 : 1;
                    } else {
                        a.leader_idle_spot = leader_near ? lp : m;
                    }
                }
                wc = tile_of(a.leader_idle_spot);
            }
            a.has_explore = 1;
            a.explore = wc;
            set_float_goal(x, room_view::middle_of_tile(wc));
            state.world.creature_set_destination(c, wc);
        }

        //
        // The trackers
        //
        tracker::params const& tracker_params() {
            // Tracker(10, 5, -1, 0.5, 5, 5, 10), ThreatTracker(3)
            static tracker::params const p = [] {
                tracker::params q;
                q.see_around_corners = 10;
                q.frames_to_remember = -1;
                q.ghost_dismissal_range = 10;
                q.threat_max = 3;
                return q;
            }();
            return p;
        }
        // ArtificialIntelligence.VisualScore: within its visual radius, less
        // well through the surface
        bool visual_contact(sys::game_state const& state, dcon::creature_id c, room_view const& room, vec2 look) {
            vec2 const eye = pos(state, c, 0);
            if (!custom::dist_less(eye, look, visual_radius))
                return false;
            float score = mathf::inverse_lerp(visual_radius, 0.f, rw::distance(eye, look));
            if (room.water && deep_water(room, eye) != deep_water(room, look))
                score -= 1.f - through_surface_vision;
            return score > 0.f && room.visual_contact(eye, look);
        }
        bool hook_sees(void* u, dcon::body_chunk_id k) {
            auto& x = *static_cast<ctx*>(u);
            return visual_contact(x.state, x.c, x.room, x.state.world.body_chunk_get_pos(k));
        }
        bool hook_sees_point(void* u, vec2 p) {
            auto& x = *static_cast<ctx*>(u);
            return visual_contact(x.state, x.c, x.room, p);
        }
        bool hook_reach(void* u, ivec2 t) {
            return reachable(*static_cast<ctx*>(u), t);
        }
        // ModuleToTrackRelationship: no ThreatTracker until the shell is gone
        // (OnLoseShell adds it), no PreyTracker ever
        creature::track_module module_shelled(relationship) {
            return creature::track_module::none;
        }
        creature::track_module module_bare(relationship r) {
            return r.type == creature::rel::afraid ? creature::track_module::threat : creature::track_module::none;
        }
        tracker::hooks make_hooks(ctx& x) {
            tracker::hooks h;
            h.user = &x;
            h.sees = hook_sees;
            h.sees_point = hook_sees_point;
            // (UpdateDynamicRelationship: the static one)
            h.relate = nullptr;
            h.module = pl(x.state, x.c).has_shell ? module_shelled : module_bare;
            h.reach = hook_reach;
            return h;
        }

        // TravelUtility (a trip ahead; CoordinateReachable of its
        // destination taken for granted)
        float travel_utility(ctx& x) {
            if (!migrate::want_to(x.state, x.c))
                return 0.f;
            float const num = migrate::attraction(x.state, x.c, x.room.id);
            float const num2 = migrate::attraction(x.state, x.c, migrate::destination(x.state, x.c));
            float const num3 = mathf::lerp(0.99f, 0.1f, mathf::pow(num, 0.25f));
            if (num > num2)
                return num3 * 0.25f;
            if (num == num2)
                return num3 * 0.5f;
            return num3;
        }

        // DetermineBehavior: the UtilityComparer over the RainTracker (1,
        // exponent 1.1) and, bare, the ThreatTracker (0.4, 1.1); a temporary
        // panic is a flight, a trip beats a weaker urge
        behavior determine_behavior(ctx& x) {
            auto& w = x.state.world;
            auto const& p = pl(x.state, x.c);
            auto const& a = ai(x.state, x.c);
            float const rain = mathf::pow(mathf::max(a.rain_fear, 0.f), 1.1f);
            float const threat = p.has_shell ? 0.f : mathf::pow(mathf::max(a.fear, 0.f), 1.1f) * 0.4f;
            behavior b = behavior::idle;
            float utility = 0.f;
            if (rain > 0.f || threat > 0.f) {
                b = threat > rain ? behavior::flee : behavior::escape_rain;
                utility = mathf::max(rain, threat);
            }
            if (utility < 0.05f) {
                utility = 0.05f;
                b = behavior::idle;
            }
            if (p.temporary_panic > 0)
                b = behavior::flee;
            if (b != behavior::escape_rain && b != behavior::flee) {
                float const travel = travel_utility(x);
                if (utility < travel) {
                    utility = travel;
                    b = behavior::travelling;
                }
            }
            w.creature_set_current_utility(x.c, utility);
            return b;
        }

        // BarnacleAI.Act
        void ai_act(ctx& x, tracker::hooks const& h) {
            auto& state = x.state;
            auto& w = state.world;
            auto const c = x.c;
            auto& p = pl(state, c);
            auto& a = ai(state, c);
            behavior const b = behavior_of(state, c);
            ivec2 const at = here(state, c);
            vec2 const m = pos(state, c, 0);
            p.idle_splash = 0;
            if (p.leader && b != behavior::escape_rain && b != behavior::flee && b != behavior::get_unstuck) {
                follow_leader(x);
                return;
            }
            a.leader_idle_spot_expiry = 0;
            bool const idle = b == behavior::idle || b == behavior::travelling;
            if (idle)
                update_sub_behavior(x);
            p.near_narrow_pos = follower_count(state, c, x.room.id) > 0 && a.sub == sub_behavior::travel_to_idle && a.has_best_narrow
                && rw::distance(m, a.best_narrow_pos) < 150.f;
            ivec2& dest = w.creature_get_destination(c);
            if (!swimming(state, x.room, c) || !idle) {
                set_float_goal(x, std::nullopt);
                if (idle) {
                    bool flag = true;
                    bool on_trip = false;
                    // Travelling: the trip's next pipe (migrate.hpp)
                    if (b == behavior::travelling)
                        if (auto const e = migrate::travel_tile(state, c, x.room)) {
                            dest = *e;
                            on_trip = true;
                        }
                    if (!on_trip) {
                        if (a.sub == sub_behavior::travel_to_idle && a.has_best_narrow && a.in_sub_behavior_time > 80)
                            dest = tile_of(a.best_narrow_pos);
                        else if (a.sub == sub_behavior::travel_to_water && a.has_water_idle)
                            dest = tile_of(a.water_idle_spot);
                        if (!a.has_best_narrow && !a.has_water_idle) {
                            dest = at;
                            flag = false;
                        } else if (custom::manhattan_distance(at, dest) < 3) {
                            p.idle_splash = 1;
                            flag = false;
                        }
                    }
                    p.run_speed = flag ? mathf::lerp(p.run_speed, 0.75f, 0.5f) : mathf::lerp(p.run_speed, 0.f, 0.8f);
                } else if (b == behavior::flee) {
                    if (p.temporary_panic == 0 && !p.has_shell) {
                        dest = tracker::flee_to(state, c, x.room, at, 5, 20, tracker_params(), h);
                        float const combined = mathf::max(a.fear, mathf::pow(a.rain_fear, 3.f));
                        p.run_speed = mathf::lerp(p.run_speed, mathf::pow(combined, 0.1f), 0.5f);
                    } else {
                        if (a.has_panic_spot)
                            dest = tile_of(a.panic_spot);
                        p.run_speed = mathf::lerp(p.run_speed, 1.f, 0.5f);
                    }
                } else if (b == behavior::escape_rain) {
                    // DenFinder: the den (dens.hpp)
                    if (auto const e = dens::home_tile(state, c, x.room))
                        dest = *e;
                    p.run_speed = mathf::lerp(p.run_speed, 1.f, 0.1f);
                }
                return;
            }
            // swimming about, idle: deep water spots in sight
            if (a.sub == sub_behavior::travel_to_idle && a.has_best_narrow && a.in_sub_behavior_time > 80) {
                dest = tile_of(a.best_narrow_pos);
                a.has_explore = 0;
            } else if (a.has_explore) {
                dest = a.explore;
                if (custom::manhattan_distance(at, a.explore) < 5 || (x.rng.value() < 0.0125f && accessible(x.room, at) && !reachable(x, a.explore)))
                    a.has_explore = 0;
            } else if (custom::manhattan_distance(at, dest) < 5 || !reachable(x, dest)) {
                vec2 const inside{mathf::clamp(m.x, 0.f, x.room.pixel_width()), mathf::clamp(m.y, 0.f, x.room.pixel_height())};
                ivec2 const t = tile_of(inside + custom::rnv(x.rng) * 200.f);
                if (x.room.inside(t.x, t.y) && reachable(x, t) && x.room.get_tile(t).water == 1
                && x.room.visual_contact(room_view::middle_of_tile(at), room_view::middle_of_tile(t)))
                    dest = t;
            }
            if (x.rng.value() < 1.f / (a.has_explore ? 1600.f : 80.f)) {
                ivec2 const t = random_tile(x);
                if (accessible(x.room, t) && reachable(x, t) && x.room.get_tile(t).water == 1) {
                    a.has_explore = 1;
                    a.explore = t;
                }
            }
            ivec2 const d = dest;
            if (rw::magnitude(rw::to_vec2(at - d)) < 20.f && x.room.visual_contact(room_view::middle_of_tile(at), room_view::middle_of_tile(d)))
                set_float_goal(x, room_view::middle_of_tile(d));
            else
                set_float_goal(x, std::nullopt);
        }

        // BarnacleAI.Update
        void ai_update(ctx& x) {
            auto& state = x.state;
            auto& w = state.world;
            auto const c = x.c;
            auto& p = pl(state, c);
            auto& a = ai(state, c);
            ivec2 const at = here(state, c);
            tracker::hooks const h = make_hooks(x);
            tracker::update(state, c, x.room, at, tracker_params(), h);
            leading_update(x);
            if (p.run_away_time > 0)
                --p.run_away_time;
            if (p.temporary_panic > 0) {
                if (a.sub != sub_behavior::temporary_panic)
                    switch_sub_behavior(x, sub_behavior::temporary_panic);
                --p.temporary_panic;
                --a.next_panic_spot_shuffle;
                a.next_sub_behavior_time = p.temporary_panic;
                if (!a.has_panic_spot || a.next_panic_spot_shuffle <= 0)
                    shuffle_panic_spot(x, p.scattering != 0);
                if (p.temporary_panic <= 0)
                    switch_sub_behavior(x, sub_behavior::travel_to_idle);
            }
            // the ThreatTracker's utility, smoothed (FloatTweenUpAndDown: a
            // lerp of 0.5 up, a tick of 0.0025 down); none with the shell on
            float const u = p.has_shell ? 0.f : tracker::threat_utility(tracker::of(state, c));
            a.fear = u > a.fear ? mathf::lerp(a.fear, u, 0.5f) : mathf::max(u, a.fear - 0.0025f);
            a.rain_fear = dens::rain_utility(state, c, x.room.id);
            behavior const b = determine_behavior(x);
            w.creature_set_behavior(c, uint8_t(b));
            // (AbstractBehavior: now and then a trip to another room)
            if (b == behavior::idle)
                migrate::idle_roll(state, c, x.room);
            ai_act(x, h);
            if (p.run_away_time > 0)
                p.run_speed = 1.f;
        }

        //
        // Barnacle.cs
        //
        // Barnacle.MoveTowards
        void move_towards(ctx& x, vec2 move_to) {
            auto& state = x.state;
            auto const c = x.c;
            auto const& p = pl(state, c);
            vec2 const m = pos(state, c, 0);
            vec2 dir = custom::dir_vec(m, move_to);
            auto const acc = pathing::tile_access(x.room, tile_of(pos(state, c, 1)));
            if (acc >= pathing::access::climb)
                dir *= 0.5f;
            if (!footing(state, c))
                dir *= 0.3f;
            if (physics::is_tile_solid(state, x.room, chunk(state, c, 1), 0, -1)
            && ((dir.x < -0.5f && m.x > pos(state, c, 1).x + 5.f) || (dir.x > 0.5f && m.x < pos(state, c, 1).x - 5.f))) {
                // turning around on the floor: the shell goes over
                float const s = dir.x < 0.f ? -1.f : 1.f;
                vel(state, c, 0).x -= s * 1.3f;
                vel(state, c, 1).x += s * 0.5f;
                if (!physics::is_tile_solid(state, x.room, chunk(state, c, 0), 0, 1))
                    vel(state, c, 0).y += 3.2f;
            }
            auto const& cp = prm(state, c);
            behavior const b = behavior_of(state, c);
            float factor = cp.idle_speed_factor;
            if ((b == behavior::flee && !p.scattering) || b == behavior::escape_rain)
                factor = cp.flee_speed_factor;
            else if (b == behavior::travelling || b == behavior::investigate_sound)
                factor = cp.travel_speed_factor;
            if (p.shake_time > 0)
                factor = 0.f;
            vel(state, c, 0) += dir * 4.2f * p.run_speed * factor;
            vel(state, c, 1) -= dir * 1.f * p.run_speed * factor;
            physics::set_go_through_floors(state, body(state, c), move_to.y < m.y - 5.f);
            if (p.run_speed > 0.f && factor > 0.f)
                ++bs(state, c).move_counter;
            // (no Barnacle_Legs_Skitter, Barnacle_Step)
        }

        // StandardPather.FollowPath from tile `from`
        connection follow_path(sys::game_state const& state, pathing::field_id f, ivec2 from) {
            auto const step = pathing::next_step(state, f, from);
            if (!step)
                return connection{};
            return connection{from, *step, 1};
        }

        // Barnacle.Swim (its AI's Update with it)
        void swim(ctx& x) {
            auto& state = x.state;
            auto& w = state.world;
            auto const c = x.c;
            auto const o = body(state, c);
            auto& p = pl(state, c);
            auto& b = bs(state, c);
            auto const& cp = prm(state, c);
            auto const& room = x.room;
            float const sub = submersion(state, room, c);
            float num = 0.f;
            if (sub > 0.f && sub < 1.f)
                num = std::fabs(rw::normalized(pos(state, c, 0) - pos(state, c, 1)).y);
            if (b.surf_speed < num)
                b.surf_speed = mathf::min(b.surf_speed + 0.05f, num);
            else
                b.surf_speed = mathf::max(b.surf_speed - 0.0125f, num);
            w.physical_object_set_water_friction(o, 0.995f);
            w.physical_object_set_water_retardation_immunity(o, 0.8f);
            b.surf_speed *= 0.9f;
            ai_update(x);
            if (!w.creature_is_valid(c))
                return;
            // (no GetUnstuck: the StuckTracker isn't ported; in-room
            // shortcuts aren't in the path fields)
            vec2 const m = pos(state, c, 0), c1 = pos(state, c, 1);
            auto const f = pathing::field_to(state, room.id, w.creature_get_destination(c), walk);
            auto const conn = follow_path(state, f, tile_of(m));
            auto const& a = ai(state, c);
            if (a.has_float_goal && room.visual_contact(m, a.float_goal)) {
                b.swim_dir = custom::dir_vec(m, a.float_goal);
            } else if (conn.valid) {
                b.swim_dir = custom::dir_vec(m, room_view::middle_of_tile(conn.dest));
                ivec2 step = conn.dest;
                for (int k = 0; k < 4; ++k) {
                    auto const n2 = follow_path(state, f, step);
                    if (!n2.valid || !room.visual_contact(room_view::middle_of_tile(conn.dest), room_view::middle_of_tile(n2.dest)))
                        continue;
                    b.swim_dir += custom::dir_vec(m, room_view::middle_of_tile(n2.dest));
                    step = n2.dest;
                    if (pathing::narrow_space(room, n2.dest)) {
                        b.slow_down_for_precision += 0.3f;
                        break;
                    }
                }
                b.swim_dir = rw::normalized(b.swim_dir);
            }
            b.slow_down_for_precision = mathf::clamp(b.slow_down_for_precision - 0.1f, 0.f, 1.f);
            // squeezing through a corridor: along the path
            ivec2 const mt = tile_of(m);
            if ((pathing::narrow_space(room, mt) || room.get_tile(mt).t == terrain::shortcut_entrance)
            && room.get_tile(m + custom::dir_vec(c1, m) * 20.f).t != terrain::air) {
                bool from_tail = false;
                auto conn2 = follow_path(state, f, mt);
                if (!conn2.valid) {
                    from_tail = true;
                    conn2 = follow_path(state, f, tile_of(c1));
                }
                if (conn2.valid) {
                    vel(state, c, 0) += custom::dir_vec(from_tail ? c1 : m, room_view::middle_of_tile(conn2.dest)) * 1.8f;
                    vec2 const r = custom::rnv(x.rng);
                    float const k = x.rng.value();
                    vel(state, c, 1) += r * 2.f * k + custom::dir_vec(m, c1) * x.rng.value();
                }
                w.body_chunk_set_terrain_squeeze(chunk(state, c, 0), 0.1f);
                w.body_chunk_set_terrain_squeeze(chunk(state, c, 1), 0.1f);
                return;
            }
            behavior const bh = behavior_of(state, c);
            float d = cp.idle_swim_factor;
            if ((bh == behavior::flee && !p.scattering) || bh == behavior::escape_rain)
                d = cp.flee_swim_factor;
            else if (bh == behavior::travelling || bh == behavior::investigate_sound)
                d = cp.travel_swim_factor;
            b.swim_speed = mathf::lerp(2.2f * (1.f + b.surf_speed * 0.5f), 1.4f, b.slow_down_for_precision) * 0.85f;
            if (p.shake_time > 0)
                d = 0.f;
            if (rw::magnitude(b.swim_dir) > 0.f)
                b.swim_dir = rw::slerp(rw::normalized(b.swim_dir), custom::rnv(x.rng), 0.2f);
            else
                b.swim_dir = rw::slerp(custom::dir_vec(c1, m), custom::rnv(x.rng), 0.3f);
            float const s0 = physics::submersion(state, room, chunk(state, c, 0));
            float const s1 = physics::submersion(state, room, chunk(state, c, 1));
            if (cp.jump_out_of_water_force <= 0.f && s0 < s1 && b.swim_dir.y > 0.f) {
                b.swim_dir.y *= -1.f;
                vel(state, c, 0).y -= 4.f * s1;
            }
            vec2 const v{std::fabs(b.swim_dir.x) > std::fabs(b.swim_dir.y) ? mathf::sign(b.swim_dir.x) : 0.f,
                std::fabs(b.swim_dir.y) >= std::fabs(b.swim_dir.x) ? mathf::sign(b.swim_dir.y) : 0.f};
            float const tail = mathf::pow(s1, 0.2f);
            vel(state, c, 0) += v * b.swim_speed * tail * d;
            vel(state, c, 1) *= mathf::lerp(1.f, 0.8f, tail);
            float const k = (creature::held(state, c) ? 1.f : 0.7f) * 2.5f * d;
            vel(state, c, 0) += v * k;
            vel(state, c, 1) -= v * k;
            if (cp.jump_out_of_water_force > 0.f && s0 < s1 && v.y > 0.f)
                vel(state, c, 0).y += cp.jump_out_of_water_force * s1;
            if (b.swim_speed > 1.5f && x.rng.value() < 0.5f)
                particles::bubbles(state, room.id, c1, 1, 4.f);
            b.swim_dir = vec2{0.f, 0.f};
        }

        // the shell's aura (Barnacle.Act with hasShell): it breathes for the
        // slugcats close by, shoos leeches off, and frets about what lingers
        void shell_aura(ctx& x) {
            auto& state = x.state;
            auto& w = state.world;
            auto const c = x.c;
            vec2 const m = pos(state, c, 0);
            std::optional<vec2> bubble_at;
            rooms::for_each_creature(state, x.room.id, [&](dcon::creature_id o) {
                if (o == c || !creature::here(state, o, x.room.id) || creature::is(w, o, creature::type::barnacle))
                    return;
                // (Player.outOfCamoTime, IsHidden: the Watcher's camouflage
                // isn't ported)
                auto const ok = creature::chunk_of(state, o, creature::main_chunk_of(state, o));
                vec2 const op = w.body_chunk_get_pos(ok);
                float const num = rw::distance(m, op);
                if (num <= 150.f) {
                    if (creature::is_slugcat(w, o)) {
                        if (auto const p = creature::player_of(state, o))
                            w.player_set_air_in_lungs(p, 1.f);
                        bubble_at = op;
                    } else if (creature::is(w, o, creature::type::leech) && !w.creature_get_dead(o) && num <= 100.f) {
                        float const num2 = mathf::inverse_lerp(100.f, 60.f, num) * physics::submersion(state, x.room, ok);
                        if (x.rng.value() < 0.007f * num2)
                            creature::stun(state, o, 16);
                        if (consious(state, o) && !creature::grasped_object(w, o, 0))
                            w.body_chunk_get_vel(ok) += custom::dir_vec(m, op) * num2 * x.rng.value() * 12.f;
                    }
                    // (AirBreatherCreature.lungs: no ported kind but the
                    // slugcat keeps them)
                }
                if (num <= 200.f && pl(state, c).run_away_time <= 0)
                    set_group_discomfort_tick(state, c, 0.0016666667f);
            });
            if (bubble_at) {
                if (x.rng.value() < 0.35f)
                    particles::bubbles(state, x.room.id, rw::lerp(*bubble_at, m, 0.5f), 1, 4.f);
                if (x.rng.value() < 0.35f)
                    particles::bubbles(state, x.room.id, *bubble_at, 1, 4.f);
            }
        }

        // Barnacle.Act
        void act(ctx& x) {
            auto& state = x.state;
            auto& w = state.world;
            auto const c = x.c;
            auto const o = body(state, c);
            auto& p = pl(state, c);
            auto& b = bs(state, c);
            vec2 const m = pos(state, c, 0);
            ++b.act_counter;
            if (b.act_counter % 200 == 0) {
                // not getting anywhere: a short panic to shake it loose
                if (follower_count(state, c, x.room.id) == 0 && rw::distance(m, b.recent_position) <= 20.f && p.run_speed > 0.5f && !p.idle_splash) {
                    p.temporary_panic = 200;
                    shuffle_panic_spot(x, true);
                }
                b.recent_position = m;
            }
            // (no Barnacle_Suck_Water)
            if (submersion(state, x.room, c) > 0.f)
                b.wet_counter = 4800;
            p.shake_time = tick_down(p.shake_time);
            p.shake_cooldown = tick_down(p.shake_cooldown);
            if (p.delay_before_jumping > 0) {
                --p.delay_before_jumping;
                if (p.delay_before_jumping == 0)
                    try_jump(state, c, b.jump_target);
            }
            if (p.has_shell)
                shell_aura(x);
            // (the idle splash's WaterJet and Barnacle_Spit_Water aren't ported)
            if (swimming(state, x.room, c) && prm(state, c).separate_swim_behavior) {
                p.swimming = 1;
                swim(x);
                return;
            }
            p.swimming = 0;
            w.physical_object_set_water_friction(o, 0.95f);
            w.physical_object_set_water_retardation_immunity(o, 0.f);
            ai_update(x);
            if (!w.creature_is_valid(c))
                return;
            if (p.special_move_counter > 0) {
                --p.special_move_counter;
                vec2 const to = room_view::middle_of_tile(b.special_move_destination);
                move_towards(x, to);
                if (custom::dist_less(pos(state, c, 0), to, 5.f))
                    p.special_move_counter = 0;
                return;
            }
            auto const f = pathing::field_to(state, x.room.id, w.creature_get_destination(c), walk);
            connection conn = follow_path(state, f, tile_of(pos(state, c, 0)));
            if (!conn.valid)
                conn = follow_path(state, f, tile_of(pos(state, c, 1)));
            if (b.last_followed.valid && conn.start == b.last_followed.start)
                conn = b.last_followed;
            if (!conn.valid) {
                physics::set_go_through_floors(state, o, false);
                return;
            }
            ivec2 const step = conn.dest - conn.start;
            if (step.x != 0 && step.y != 0) {
                // OpenDiagonal: a special move
                p.special_move_counter = 30;
                b.special_move_destination = conn.dest;
            } else {
                vec2 const target = room_view::middle_of_tile(conn.dest);
                if (footing(state, c)) {
                    for (uint32_t k = 0; k < 2; ++k) {
                        if (conn.start.x == conn.dest.x)
                            vel(state, c, k).x += mathf::min((target.x - pos(state, c, k).x) / 8.f, 1.2f);
                        else if (conn.start.y == conn.dest.y)
                            vel(state, c, k).y += mathf::min((target.y - pos(state, c, k).y) / 8.f, 1.2f);
                    }
                }
                auto const& last = b.last_followed;
                if (last.valid && (footing(state, c) || accessible(x.room, tile_of(pos(state, c, 0))))
                && ((conn.start.x != conn.dest.x && last.start.x == last.dest.x) || (conn.start.y != conn.dest.y && last.start.y == last.dest.y))) {
                    vel(state, c, 0) *= 0.7f;
                    vel(state, c, 1) *= 0.5f;
                }
                // DropToFloor
                if (step.y < -1)
                    p.footing_counter = 0;
                move_towards(x, target);
            }
            b.last_followed = conn;
        }

        // Creature.Violence (HealthState)
        void base_violence(sys::game_state& state, dcon::creature_id c, uint32_t k, vec2 momentum, float damage, float stun_bonus) {
            auto& w = state.world;
            auto rng = rw::random(state.seed, state.tick, rw::ids(uint32_t(c.value), k));
            auto const hit = chunk(state, c, k < 2 ? k : 0);
            w.body_chunk_get_vel(hit) += rw::clamp_magnitude(momentum / w.body_chunk_get_mass(hit), 10.f);
            float const dmg = damage / base_damage_resistance;
            float st = (damage * 30.f + stun_bonus) / base_stun_resistance;
            float& health = w.creature_get_health(c);
            st *= 1.5f + mathf::inverse_lerp(0.5f, 0.f, health) * rng.value();
            creature::stun(state, c, int32_t(st));
            health -= dmg;
            if (rng.value() < -health || health < -1.f || (health < 0.f && rng.value() < 0.33f) || dmg >= instant_death_damage_limit)
                creature::die(state, c);
        }

        // Barnacle.Collide of `a` with the barnacle `o`: the less dominant
        // one gets on top (no Barnacle_Shell_Clink)
        void collide_barnacle(sys::game_state& state, dcon::creature_id a, dcon::creature_id o) {
            if (!consious(state, a))
                return;
            if (pl(state, o).run_speed > 0.f || pl(state, a).run_speed > 0.f)
                for (uint32_t i = 0; i < 2; ++i) {
                    vec2& v = vel(state, a, i);
                    if (pl(state, o).dominance > pl(state, a).dominance) {
                        if (v.y > -3.f)
                            v.y -= 0.5f;
                    } else if (v.y < 5.f) {
                        v.y += 2.5f;
                    }
                }
        }

        // keyed by the room and the barnacle they fly off
        void sparks(sys::game_state& state, dcon::room_id room, dcon::creature_id c, vec2 at, vec2 base_vel, int32_t n, float spread) {
            auto rng = rw::random(state.seed, state.tick, rw::ids(uint32_t(room.value), uint32_t(c.value)));
            for (int32_t i = 0; i < n; ++i) {
                vec2 const off = custom::deg_to_vec(rng.value() * 360.f) * spread * rng.value();
                vec2 const v = base_vel * -0.1f + custom::deg_to_vec(rng.value() * 360.f) * mathf::lerp(0.2f, 0.4f, rng.value()) * rw::magnitude(base_vel);
                particles::spawn(state, room, particles::kind::spark, at + off, v, 2.f, 0.4f, vec3{1.f, 1.f, 1.f});
            }
        }

        // Barnacle.CreatureParams and AbstractCreature.Personality.dominance,
        // both rolled from the ID's seed
        float roll_dominance(int32_t seed) {
            rw::unity_random r{seed};
            float const sympathy = push_from_half(r.value(), 1.5f);
            float const energy = push_from_half(r.value(), 1.5f);
            float const bravery = push_from_half(r.value(), 1.5f);
            float const n1 = r.value();
            float const n2 = r.value();
            (void)mathf::lerp(n1, mathf::lerp(energy, 1.f - bravery, 0.5f), mathf::pow(n2, 0.25f));
            float const a1 = r.value();
            float const a2 = r.value();
            float const aggression = mathf::lerp(a1, (energy + bravery) / 2.f * (1.f - sympathy), mathf::pow(a2, 0.25f));
            float const d1 = r.value();
            float const d2 = r.value();
            return mathf::lerp(d1, (energy + bravery + aggression) / 3.f, mathf::pow(d2, 0.25f));
        }
        creature_params roll_params(int32_t seed, float dominance) {
            rw::unity_random r{seed};
            creature_params p;
            p.total_mass = 3.6f;
            float const v1 = r.value();
            float const v2 = r.value();
            p.size_multiplier = mathf::lerp(dominance, v1, v2) * 0.4f + 0.6f;
            p.travel_speed_factor = 0.5f;
            p.flee_speed_factor = 1.25f;
            p.idle_speed_factor = 0.5f;
            p.travel_swim_factor = 0.3f;
            p.flee_swim_factor = 0.6f;
            p.idle_swim_factor = 0.3f;
            p.separate_swim_behavior = 1;
            p.jump_out_of_water_force = 1.f;
            p.eye_size_multiplier = r.range(0.7f, 1.f);
            p.cones = r.range(13, 20);
            p.cone_rad_variance = r.value();
            p.cone_length_variance = push_from_half(r.value(), 1.5f);
            p.cone_color_variance = mathf::pow(r.value(), 0.5f);
            p.leg_thickness = mathf::lerp(0.7f, 1.3f, push_from_half(r.value(), 0.7f));
            p.tip_rad_factor = 0.46153846f;
            float const hue = custom::wrapped_random_variation(0.04f, 0.1f, 0.1f, r);
            float const sat = custom::clamped_random_variation(0.5f, 0.2f, 0.3f, r);
            float const light = custom::clamped_random_variation(0.75f, 0.2f, 1.5f, r);
            float const dark_l = mathf::clamp01(light - r.range(0.1f, 0.2f));
            float const light_l = mathf::clamp01(light + r.range(0.f, 0.1f));
            p.dark_shell_color = custom::hsl2rgb(hue, sat, dark_l);
            p.light_shell_color = custom::hsl2rgb(hue, sat, light_l);
            return p;
        }
    }

    bool has_shell(sys::game_state const& state, dcon::creature_id c) noexcept {
        return pl(state, c).has_shell != 0;
    }

    void lose_shell(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        auto& p = pl(state, c);
        if (!p.has_shell)
            return;
        p.has_shell = 0;
        p.temporary_damage_immunity = 20;
        for (uint32_t i = 0; i < 2; ++i) {
            auto const k = chunk(state, c, i);
            w.body_chunk_set_mass(k, w.body_chunk_get_mass(k) / 8.f);
            w.body_chunk_set_rad(k, w.body_chunk_get_rad(k) * 0.75f);
        }
        w.physical_object_for_each_chunk_connection_as_owner(body(state, c), [&](dcon::chunk_connection_id k) {
            w.chunk_connection_set_distance(k, w.chunk_connection_get_distance(k) * 0.75f);
        });
        // (OnLoseShell: the ThreatTracker, here the hooks' threat module)
        auto const room = creature::room_of(state, c);
        if (!room || w.creature_get_away(c) != 0)
            return;
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        auto const& cp = prm(state, c);
        vec2 const m = pos(state, c, 0);
        float const rad = w.body_chunk_get_rad(chunk(state, c, 0));
        // the shell's rocks (made after the room's update)
        struct rock {
            vec2 at;
            vec2 v;
        };
        rock rocks[3];
        uint32_t n = 0;
        float const rock_count = mathf::lerp(1.f, 3.f, cp.size_multiplier);
        for (int32_t i = 0; float(i) < rock_count && n < 3; ++i) {
            vec2 const r1 = custom::rnv(rng);
            vec2 const r2 = custom::rnv(rng);
            float const s = mathf::lerp(8.f, 16.f, rng.value());
            rocks[n++] = rock{m + r1 * rad, r2 * s};
        }
        state.defer(room, [room, rocks, n](sys::game_state& s) {
            for (uint32_t i = 0; i < n; ++i) {
                auto const it = items::create(s, room, rocks[i].at, world::item_type::rock);
                if (it)
                    s.world.body_chunk_set_vel(items::chunk(s, it), rocks[i].v);
            }
        });
        // the shell's shards (CentipedeShell; vanilla's RootBall1,
        // KrakenShield0, Cicada5body, Cicada1body elements aren't kept: the
        // centipede's shell sprite stands in)
        if (state.viewed(room)) {
            float const shards = mathf::lerp(4.f, 7.f, cp.size_multiplier);
            for (int32_t i = 0; float(i) < shards; ++i) {
                vec2 v = custom::rnv(rng) * 4.f;
                v.y = std::fabs(v.y);
                vec3 const col = lerp3(cp.dark_shell_color, cp.light_shell_color, rng.value());
                vec3 const hsl = custom::rgb2hsl(col);
                particles::centipede_shell(state, room, m + custom::rnv(rng) * rad, v, hsl.x, hsl.y, cp.size_multiplier * 1.5f, cp.size_multiplier * 1.5f);
            }
        }
        // (no Barnacle_Shell_Explode, Barnacle_Shell_Crack)
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::barnacle);
        auto const d = data(state, c);
        auto rng = rw::random(state.seed, 0, uint32_t(c.value));
        int32_t seed = int32_t(rng.next() & 0x7FFFFFFFu);
        if (state.spawn_seed >= 0)
            seed = state.spawn_seed;
        float const dominance = roll_dominance(seed);
        creature_params const cp = roll_params(seed, dominance);
        physics::object_desc desc;
        desc.gravity = 0.9f;
        desc.air_friction = 0.999f;
        desc.bounce = 0.1f;
        desc.surface_friction = 0.4f;
        desc.water_friction = 0.95f;
        desc.buoyancy = 0.99f;
        auto const o = physics::create_object(state, room, desc);
        // never inside terrain
        auto const view = world::get_room_view(state, room);
        ivec2 const t0 = tile_of(at);
        if (view.get_tile(t0).solid()) {
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
        // the shell (24 * size) and the underside (10 * size), half the
        // mass each, 12 * size apart
        float const sm = cp.size_multiplier;
        auto const c0 = physics::add_chunk(state, o, at, 24.f * sm, cp.total_mass / 2.f);
        auto const c1 = physics::add_chunk(state, o, at + vec2{0.f, -1.f}, 10.f * sm, cp.total_mass / 2.f);
        physics::add_connection(state, o, c0, c1, 12.f * sm, physics::connection_type::normal, 1.f, 0.5f);
        w.try_create_creature_body(c, o);
        auto& p = pl(state, c);
        {
            uint16_t const keep = p.data;
            p = creature::barnacle_payload{};
            p.data = keep;
        }
        p.dominance = dominance;
        p.has_shell = 1;
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_set_grasps(c, creature::grasp_pair{});
        w.creature_set_behavior(c, uint8_t(behavior::idle));
        w.creature_set_current_utility(c, 0.f);
        w.creature_set_destination(c, tile_of(at));
        w.barnacle_data_set_seed(d, seed);
        w.barnacle_data_set_params(d, cp);
        body_state b;
        b.recent_position = at;
        w.barnacle_data_set_body(d, b);
        w.barnacle_data_set_ai(d, ai_state{});
        w.barnacle_data_set_graphics(d, graphics_state{});
        tracker::clear(state, c);
        barnacle_graphics::reset_positions(state, c);
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        // its followers are left (release_target: release())
        creature::release_target(state, c);
        creature::erase(state, c);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
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
        // Creature.Update + PhysicalObject.Update (no terrain impact sounds)
        creature::stun_countdown(state, c);
        physics::update_object(state, room, o, nullptr, nullptr);
        auto& p = pl(state, c);
        auto& b = bs(state, c);
        if (w.creature_get_dead(c))
            abandon_leader(state, c);
        for (uint32_t i = 0; i < 2; ++i) {
            auto const k = chunk(state, c, i);
            w.body_chunk_set_terrain_squeeze(k, 8.f / w.body_chunk_get_rad(k));
        }
        p.temporary_damage_immunity = tick_down(p.temporary_damage_immunity);
        b.clink_sound_cooldown = std::max(b.clink_sound_cooldown - 1, -1000);
        ctx x{state, c, room, rng, std::nullopt};
        if (consious(state, c)) {
            if (accessible(room, tile_of(pos(state, c, 0))) || accessible(room, tile_of(pos(state, c, 1))))
                p.footing_counter = int16_t(std::min<int32_t>(p.footing_counter + 1, 1000));
            else
                p.footing_counter = 0;
            act(x);
            if (!w.creature_is_valid(c))
                return;
        } else {
            p.footing_counter = 0;
            w.physical_object_set_water_friction(o, 0.95f);
            w.physical_object_set_water_retardation_immunity(o, 0.f);
        }
        // clinging on: gravity cancelled (PhysicalObject.gravity)
        float const sub = submersion(state, room, c);
        bool const swim_now = sub > 0.3f;
        if (footing(state, c)) {
            float const g = w.physical_object_get_g(o);
            for (uint32_t j = 0; j < 2; ++j) {
                if (!swim_now)
                    vel(state, c, j) *= 0.8f;
                vel(state, c, j).y += g * (1.f - sub);
            }
        }
        // fleeing in the air: it flails
        if (consious(state, c) && !footing(state, c) && behavior_of(state, c) == behavior::flee && !p.scattering)
            for (uint32_t k = 0; k < 2; ++k)
                if (accessible(room, tile_of(pos(state, c, k)))) {
                    vec2 const r = custom::deg_to_vec(rng.value() * 360.f);
                    vel(state, c, k) += r * rng.value() * 5.f;
                }
    }

    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) {
        // what holds it is kept apart by the carrying
        return !creature::holds(state.world, t, c);
    }

    void collide(sys::game_state& state, dcon::creature_id c, room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j) {
        auto& w = state.world;
        if (!consious(state, c) || !creature::live(state, t) || j >= creature::chunk_count_of(state, t))
            return;
        if (creature::is(w, t, creature::type::barnacle)) {
            collide_barnacle(state, c, t);
            return;
        }
        auto& p = pl(state, c);
        behavior const b = behavior_of(state, c);
        if (submersion(state, room, c) != 0.f || !p.has_shell || !consious(state, t) || p.shake_cooldown > 0
            || (b != behavior::idle && b != behavior::travelling))
            return;
        // it shakes the bothersome creature off (no
        // Barnacle_Push_Away_Nearby_Creatures)
        auto rng = rw::random(state.seed, state.tick, rw::ids(uint32_t(c.value), uint32_t(t.value)));
        p.shake_time = int16_t(rng.range(10, 20));
        p.shake_cooldown = 160;
        auto const theirs = creature::chunk_of(state, t, j);
        vec2 const a = rw::lerp(w.body_chunk_get_pos(theirs), w.body_chunk_get_pos(chunk(state, c, i)), 0.5f);
        creature::stun(state, t, 30);
        vec2 const m = pos(state, c, 0);
        uint32_t const n = creature::chunk_count_of(state, t);
        for (uint32_t k = 0; k < n; ++k) {
            auto const tk = creature::chunk_of(state, t, k);
            w.body_chunk_set_vel(tk, custom::dir_vec(m, w.body_chunk_get_pos(tk)) * mathf::lerp(3.f, 8.f, rng.value()));
        }
        sparks(state, room.id, c, a, w.body_chunk_get_vel(theirs), 5, 10.f);
        set_group_discomfort_tick(state, c, 0.25f);
    }

    void collide_kin(sys::game_state& state, dcon::creature_id c, uint32_t, dcon::creature_id other, uint32_t) {
        collide_barnacle(state, c, other);
        collide_barnacle(state, other, c);
    }

    void violence(sys::game_state& state, dcon::creature_id c, uint32_t k, vec2 momentum, float damage, float stun_bonus, creature::damage_type type) {
        auto& w = state.world;
        auto& p = pl(state, c);
        // (RippleViolenceCheck: the Watcher's ripple layers aren't ported)
        if (type == creature::damage_type::explosion && damage >= 0.05f)
            lose_shell(state, c);
        if (p.has_shell || p.temporary_damage_immunity > 0) {
            // the shell takes it: sparks, no harm, and the herd startles
            float const num = damage / base_damage_resistance;
            int32_t const n = int32_t(std::min(std::max((damage * 30.f + stun_bonus) / base_stun_resistance / 2.f, float(int32_t(num * 30.f))), 25.f));
            auto const hit = chunk(state, c, k < 2 ? k : 0);
            auto const room = creature::room_of(state, c);
            if (room)
                sparks(state, room, c, w.body_chunk_get_pos(hit), w.body_chunk_get_vel(hit), n, 5.f);
            damage = 0.f;
            stun_bonus = 0.f;
            set_group_discomfort_tick(state, c, 0.65f);
            auto rng = rw::random(state.seed, state.tick, rw::ids(uint32_t(c.value), k));
            p.temporary_panic = int16_t(rng.range(400, 1800));
            if (room && w.creature_get_away(c) == 0) {
                auto const view = world::get_room_view(state, room);
                ctx x{state, c, view, rng, std::nullopt};
                shuffle_panic_spot(x, false);
            }
            // (SpearStick: a spear doesn't stick in the shell; no
            // Barnacle_Shell_Deflect_Projectile)
        }
        base_violence(state, c, k, momentum, damage, stun_bonus);
    }

    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t) {
        if (pl(state, c).leader == t)
            abandon_leader(state, c);
        auto& g = state.world.barnacle_data_get_graphics(data(state, c));
        if (g.look == t)
            g.look = dcon::creature_id{};
    }

    void died(sys::game_state& state, dcon::creature_id c) {
        abandon_leader(state, c);
        pl(state, c).run_speed = 0.f;
    }

    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const view = world::get_room_view(state, room);
        ivec2 const t = tile_of(at);
        vec2 const a = rw::to_vec2(view.shortcut_entrance_hole_direction(t));
        vec2 const mid = room_view::middle_of_tile(t);
        for (uint32_t i = 0; i < 2; ++i) {
            auto const k = chunk(state, c, i);
            physics::hard_set_position(state, k, mid - a * (-1.5f + float(i)) * 15.f);
            w.body_chunk_set_last_pos(k, mid);
            w.body_chunk_set_vel(k, a * 2.f);
        }
        // BarnacleAI.NewRoom: the spots were the other room's (its herd
        // stayed behind: it finds a new one)
        abandon_leader(state, c);
        auto& ai_s = ai(state, c);
        ai_s.has_float_goal = 0;
        ai_s.has_explore = 0;
        ai_s.has_best_narrow = 0;
        ai_s.has_water_idle = 0;
        ai_s.has_panic_spot = 0;
        ai_s.has_blacklist = 0;
        auto& b = bs(state, c);
        b.last_followed = connection{};
        b.recent_position = mid;
        pl(state, c).special_move_counter = 0;
        w.creature_set_destination(c, t);
    }

    std::string describe(sys::game_state const& state, dcon::creature_id c) {
        static constexpr char const* names[] = {"idle", "travelling", "flee", "investigate sound", "escape rain", "get unstuck"};
        static constexpr char const* subs[] = {"none", "to water", "to idle spot", "panic"};
        auto const& w = state.world;
        auto const& p = pl(state, c);
        vec2 const at = pos(state, c, 0);
        uint32_t const b = w.creature_get_behavior(c);
        uint32_t const s = uint32_t(w.barnacle_data_get_ai(data(state, c)).sub);
        auto const room = creature::room_of(state, c);
        uint32_t const followers = room ? follower_count(state, c, room) : 0;
        return "barnacle at " + std::to_string(int(at.x)) + "," + std::to_string(int(at.y)) + " behavior " + (b < 6 ? names[b] : "?") + " (" + (s < 4 ? subs[s] : "?")
            + ")" + (p.has_shell ? " shelled" : " bare") + (p.leader ? " following" : "") + (followers ? " leading " + std::to_string(followers) : std::string{})
            + " discomfort " + std::to_string(p.group_discomfort) + (creature::held(state, c) ? " held" : "");
    }

    bool spawn_spot(room_view const& room, ivec2 tile) {
        return room.inside(tile.x, tile.y) && !room.get_tile(tile).solid() && pathing::accessible(room, tile, walk);
    }

    //
    // The slugcat side
    //
    dcon::creature_id held_in_hand(sys::game_state const& state, dcon::player_id p, uint32_t hand) noexcept {
        if (!p)
            return dcon::creature_id{};
        auto const c = creature::grasped(state.world, player::creature_of(state, p), hand);
        return c && creature::type_of(state.world, c) == creature::type::barnacle ? c : dcon::creature_id{};
    }

    dcon::creature_id held_by(sys::game_state const& state, dcon::player_id p) noexcept {
        if (auto const c = held_in_hand(state, p, 0))
            return c;
        return held_in_hand(state, p, 1);
    }

    dcon::creature_id pickup_candidate(sys::game_state const& state, dcon::player_id p, room_view const& room, float& score) {
        auto const& w = state.world;
        score = float_max;
        vec2 const p0 = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
        int32_t const flip = w.player_get_flip_direction(p);
        dcon::creature_id result;
        rooms::for_each_creature(state, room.id, creature::type::barnacle, [&](dcon::creature_id c) {
            // Player.Grabability: OneHand without the shell, CantGrab with it
            if (pl(state, c).has_shell || creature::held(state, c) || w.creature_get_away(c) != 0)
                return;
            for (uint32_t i = 0; i < 2; ++i) {
                auto const k = chunk(state, c, i);
                vec2 const cp = w.body_chunk_get_pos(k);
                float const rad = w.body_chunk_get_rad(k);
                if (!custom::dist_less(p0, cp, rad + 40.f))
                    continue;
                if (!custom::dist_less(p0, cp, rad + 20.f) && !room.visual_contact(p0, cp))
                    continue;
                float dist = rw::distance(p0, cp);
                if ((cp.x < p0.x) == (flip < 0))
                    dist -= 10.f;
                if (dist < score) {
                    result = c;
                    score = dist;
                }
            }
        });
        return result;
    }

    void grab(sys::game_state& state, dcon::player_id p, dcon::creature_id c, uint32_t hand) {
        // (IsCreatureLegalToHoldWithoutStun: a bare barnacle is held awake)
        abandon_leader(state, c);
        creature::grab(state, player::creature_of(state, p), hand, c, 0);
    }

    void release(sys::game_state& state, dcon::creature_id c) {
        uint32_t g = 0;
        if (auto const h = creature::holder(state, c, &g))
            creature::release_grasp(state, h, g);
    }

    void release_player(sys::game_state& state, dcon::player_id p) {
        for (uint32_t hand = 0; hand < 2; ++hand)
            if (held_in_hand(state, p, hand))
                creature::release_grasp(state, player::creature_of(state, p), hand);
    }

    void carry_update(sys::game_state& state, dcon::player_id p, uint32_t hand, vec2 hand_pos) {
        auto& w = state.world;
        auto const c = held_in_hand(state, p, hand);
        if (!c)
            return;
        auto const k = chunk(state, c, 0);
        w.body_chunk_set_last_pos(k, w.body_chunk_get_pos(k));
        w.body_chunk_set_pos(k, hand_pos);
        w.body_chunk_set_vel(k, w.body_chunk_get_vel(player::body_chunk(state, p, 0)));
    }

    void follow_player(sys::game_state& state, dcon::player_id p, dcon::room_id room, bool spit_out) {
        auto& w = state.world;
        for (uint32_t hand = 0; hand < 2; ++hand) {
            auto const c = held_in_hand(state, p, hand);
            if (!c)
                continue;
            w.physical_object_set_room_from_object_location(body(state, c), room);
            if (!spit_out)
                continue;
            vec2 const at = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
            for (uint32_t i = 0; i < 2; ++i) {
                physics::hard_set_position(state, chunk(state, c, i), at + vec2{0.f, float(i) * -5.f});
                w.body_chunk_set_vel(chunk(state, c, i), w.body_chunk_get_vel(player::body_chunk(state, p, 0)));
            }
            abandon_leader(state, c);
            auto& a = ai(state, c);
            a.has_float_goal = 0;
            a.has_explore = 0;
            a.has_best_narrow = 0;
            a.has_water_idle = 0;
            a.has_panic_spot = 0;
            bs(state, c).last_followed = connection{};
            w.creature_set_destination(c, tile_of(at));
            tracker::clear(state, c);
            barnacle_graphics::reset_positions(state, c);
        }
    }

    bool bit_by_player(sys::game_state& state, dcon::creature_id c, dcon::player_id p) {
        auto& w = state.world;
        auto& b = bs(state, c);
        --b.bites;
        creature::die(state, c);
        // MoveFromOutsideMyUpdate: to the slugcat's mouth
        vec2 const mouth = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
        auto const k = chunk(state, c, 0);
        w.body_chunk_set_pos(k, mouth);
        w.body_chunk_set_last_pos(k, mouth);
        return b.bites < 1;
    }
}
