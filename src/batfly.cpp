// Fly.cs, FlyAI.cs, FliesRoomAI.cs and FliesWorldAI.cs: the batflies. See
// batfly.hpp.

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "batfly.hpp"
#include "batfly_graphics.hpp"
#include "batfly_types.hpp"
#include "dens.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "rain.hpp"
#include "rooms.hpp"

namespace batfly {
    using rw::ivec2;
    using rw::vec2;
    using sound::sound_id;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // CreatureTemplate (StaticWorld: Fly)
        constexpr float base_damage_resistance = 0.15f;
        constexpr float base_stun_resistance = 0.2f;
        constexpr float instant_death_damage_limit = 1.f;
        constexpr float float_max = std::numeric_limits<float>::max();

        body_state& pl(sys::game_state& state, dcon::creature_id c) { return state.world.creature_get_payload(c).batfly; }
        body_state const& pl(sys::game_state const& state, dcon::creature_id c) { return state.world.creature_get_payload(c).batfly; }
        dcon::batfly_data_id data(sys::game_state const& state, dcon::creature_id c) {
            return dcon::batfly_data_id{dcon::batfly_data_id::value_base_t(state.world.creature_get_payload(c).batfly.data)};
        }
        ai_state& ai(sys::game_state& state, dcon::creature_id c) { return state.world.batfly_data_get_ai(data(state, c)); }
        ai_state const& ai(sys::game_state const& state, dcon::creature_id c) { return state.world.batfly_data_get_ai(data(state, c)); }
        ivec2 tile_of(vec2 p) { return room_view::get_tile_position(p); }
        bool is_fly(dcon::data_container const& w, dcon::creature_id c) { return creature::is(w, c, creature::type::batfly); }
        vec2 fly_pos(sys::game_state const& state, dcon::creature_id c) { return state.world.body_chunk_get_pos(chunk(state, c, 0)); }

        // Room.Tile.AnyWater
        bool any_water(room_view const& room, ivec2 t) {
            return room.get_tile(t).water != 0 || room_view::middle_of_tile(t).y < room.float_water_level;
        }
        // Room.PointSubmerged(pos, yDisplacement) (water not inverted)
        bool point_submerged(room_view const& room, vec2 pos, float y) { return pos.y < room.float_water_level - y; }

        // RainCycle.RainApproaching of the room's cycle (1 where it never rains)
        float rain_approaching(sys::game_state const& state, dcon::room_id room) {
            auto const* rc = rain::of_room(state, room);
            return rc ? rain::rain_approaching(*rc) : 1.f;
        }
        bool rain_game_over(sys::game_state const& state, dcon::room_id room) {
            auto const* rc = rain::of_room(state, room);
            return rc && rain::deadly(*rc);
        }
        int32_t time_until_rain(sys::game_state const& state, dcon::room_id room) {
            auto const* rc = rain::of_room(state, room);
            return rc ? rain::time_until_rain(*rc) : std::numeric_limits<int32_t>::max();
        }
        // FlyAI.RoomNotACycleHazard
        bool room_not_a_cycle_hazard(sys::game_state const& state, dcon::room_id room) {
            auto const& w = state.world;
            auto const d = w.room_get_danger_type(room);
            return d == world::danger_type::none || d == world::danger_type::blizzard || w.room_get_rain_intensity(room) < 0.2f;
        }

        //
        // The room's nodes, flies and creatures this tick
        //
        // AbstractRoom.nodes as far as the flies go: the room exits (by
        // exitAndDenIndex, the ones with a pipe) and the hives (their middle
        // tile). A fly's followingDijkstraMap is an index into these
        struct node {
            ivec2 tile;
            bool hive = false;
            // an exit's room on the other side (none for a hive)
            dcon::room_id target;
        };
        // FliesRoomAI.flies (the room's flies out in it), abstractRoom
        // .creatures (everything out in it) and the nodes: worked out once a
        // tick per room and thread (a cache of what the room holds: rooms
        // don't change their objects while they update, see rooms.hpp)
        struct room_info {
            sys::game_state const* state = nullptr;
            uint32_t tick = 0;
            uint32_t room_count = 0;
            dcon::room_id room;
            std::vector<dcon::creature_id> flies;
            std::vector<dcon::creature_id> creatures;
            std::vector<node> nodes;
            uint32_t exits = 0;
        };

        void room_nodes(sys::game_state const& state, dcon::room_id room, std::vector<node>& out, uint32_t& exits) {
            auto const& w = state.world;
            out.clear();
            std::vector<std::pair<uint8_t, node>> ex;
            w.room_for_each_exit_location(room, [&](dcon::exit_location_id el) {
                auto const e = w.exit_location_get_exit(el);
                uint8_t const idx = w.room_exit_get_index(e);
                if (auto const t = dens::exit_entrance(state, room, idx))
                    ex.emplace_back(idx, node{*t, false, dens::exit_target(state, e, room)});
            });
            std::sort(ex.begin(), ex.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
            for (auto const& e : ex)
                out.push_back(e.second);
            exits = uint32_t(out.size());
            w.room_for_each_bat_hive_location(room, [&](dcon::bat_hive_location_id l) {
                auto const tiles = w.bat_hive_get_tiles(w.bat_hive_location_get_hive(l));
                if (tiles.size() > 0)
                    out.push_back(node{tiles[tiles.size() / 2], true, dcon::room_id{}});
            });
        }

        room_info const& info_of(sys::game_state const& state, dcon::room_id room) {
            thread_local room_info r;
            auto const& w = state.world;
            if (r.state == &state && r.tick == state.tick && r.room == room && r.room_count == w.room_size())
                return r;
            r.state = &state;
            r.tick = state.tick;
            r.room = room;
            r.room_count = w.room_size();
            r.flies.clear();
            r.creatures.clear();
            rooms::for_each_creature(state, room, [&](dcon::creature_id c) {
                if (w.creature_get_away(c) != 0)
                    return;
                r.creatures.push_back(c);
                if (w.creature_get_type(c) == creature::type::batfly)
                    r.flies.push_back(c);
            });
            room_nodes(state, room, r.nodes, r.exits);
            return r;
        }

        bool has_hives(sys::game_state const& state, dcon::room_id room) {
            auto const range = state.world.room_range_of_bat_hive_location(room);
            return range.first != range.second;
        }

        //
        // FlyAI.FlockBehavior: its constructor's rolls from the flock's
        // seed; sinCycle and life by the ticks since it began (vanilla's
        // leader advances them once a tick)
        //
        struct flock_params {
            float sin0 = 0.f;
            float distance_to_others = 25.f;
            float swarm_vertical_displace = 200.f;
            float sin_freq = 0.f;
            float sin_ampl = 0.f;
            float exaggerate = 0.f;
            float collective = 0.f;
            float life_time = 200.f;
            float dom = 0.f;
            bool form_chains = false;
        };
        flock_params flock_of(uint32_t seed) {
            rw::rng r{seed};
            flock_params f;
            f.sin0 = r.value();
            r.value(); // drop
            f.distance_to_others = 25.f + r.value() * 75.f;
            r.value(); // col
            f.swarm_vertical_displace = mathf::lerp(200.f, 320.f, r.value());
            f.sin_freq = 1.f / (30.f + 120.f * r.value());
            f.sin_ampl = r.value() * 80.f;
            r.value(); // flockDir
            r.value();
            r.value();
            f.exaggerate = r.value();
            f.collective = r.value() < 0.5f ? 0.f : mathf::pow(r.value(), 10.f);
            f.form_chains = r.value() < 0.5f;
            f.life_time = mathf::lerp(200.f, 800.f, r.value());
            f.dom = r.value();
            return f;
        }
        float flock_age(sys::game_state const& state, ai_state const& a) { return float(state.tick - a.flock_birth); }
        float flock_sin_cycle(sys::game_state const& state, ai_state const& a, flock_params const& f) { return f.sin0 + f.sin_freq * flock_age(state, a); }
        float flock_life(sys::game_state const& state, ai_state const& a, flock_params const& f) { return 1.f - flock_age(state, a) / f.life_time; }
        // FlockBehavior.Dominance
        float flock_dominance(sys::game_state const& state, ai_state const& a, flock_params const& f) {
            return f.dom * mathf::inverse_lerp(0.f, 0.2f, flock_life(state, a, f));
        }
        void new_flock(sys::game_state const& state, dcon::creature_id c, ai_state& a) {
            a.flock_seed = rw::random(state.seed, state.tick, uint32_t(c.value)).next();
            a.flock_birth = state.tick;
        }

        //
        // Chains (Fly.FirstInChain, LastInChain, NextInChain)
        //
        dcon::creature_id grasped_fly(dcon::data_container const& w, dcon::creature_id c) {
            auto const g = creature::grasped(w, c, 0);
            return g && is_fly(w, g) ? g : dcon::creature_id{};
        }
        dcon::creature_id first_in_chain(dcon::data_container const& w, dcon::creature_id c) {
            auto f = c;
            for (uint32_t n = 0; n < 64; ++n) {
                auto const g = grasped_fly(w, f);
                if (!g)
                    break;
                f = g;
            }
            return f;
        }
        // the fly hanging from `c` (the first fly holding it, if its grasp
        // holds `c`)
        dcon::creature_id next_in_chain(sys::game_state const& state, room_info const& info, dcon::creature_id c) {
            auto const& w = state.world;
            for (auto const f : info.flies) {
                if (f == c || !is_fly(w, f))
                    continue;
                if (creature::holds(w, f, c))
                    return creature::grasped(w, f, 0) == c ? f : dcon::creature_id{};
            }
            return dcon::creature_id{};
        }
        dcon::creature_id last_in_chain(sys::game_state const& state, room_info const& info, dcon::creature_id c) {
            auto f = c;
            for (uint32_t n = 0; n < 64; ++n) {
                auto const g = next_in_chain(state, info, f);
                if (!g)
                    break;
                f = g;
            }
            return f;
        }

        // CreatureTemplate.canFly of the ported templates
        bool can_fly(creature::tmpl t) {
            using creature::tmpl;
            switch (t) {
            case tmpl::centiwing:
            case tmpl::cicada_a:
            case tmpl::cicada_b:
            case tmpl::big_needle_worm:
            case tmpl::small_needle_worm:
            case tmpl::vulture:
            case tmpl::king_vulture:
            case tmpl::miros_vulture:
            case tmpl::overseer:
            case tmpl::sky_whale:
            case tmpl::batfly:
            case tmpl::big_moth:
            case tmpl::small_moth:
                return true;
            default:
                return false;
            }
        }

        // FlyAI.AfraidDistance
        float afraid_distance(creature::relationship r) { return r.type == creature::rel::afraid ? mathf::lerp(50.f, 600.f, r.intensity) : 0.f; }

        //
        // One fly's update
        //
        struct ctx {
            sys::game_state& state;
            dcon::creature_id c;
            room_view const& room;
            room_info const& info;
            rw::rng& rng;
            body_state& p;
            ai_state& a;
            dcon::body_chunk_id k;
            flock_params flock;
            uint32_t flock_seed = 0;

            vec2& pos() { return state.world.body_chunk_get_pos(k); }
            vec2& vel() { return state.world.body_chunk_get_vel(k); }
            ivec2 contact() { return state.world.body_chunk_get_contact_point(k); }
            behavior beh() const { return behavior(state.world.creature_get_behavior(c)); }
            void set_beh(behavior b) { state.world.creature_set_behavior(c, uint8_t(b)); }
            flock_params const& fl() {
                if (flock_seed != a.flock_seed) {
                    flock = flock_of(a.flock_seed);
                    flock_seed = a.flock_seed;
                }
                return flock;
            }
            float sin_cycle() { return flock_sin_cycle(state, a, fl()); }
            int32_t prox(vec2 at) { return pathing::ai_terrain_proximity(state, room.id, tile_of(at)); }
            bool has_grasp() const { return bool(creature::grasped_object(state.world, c, 0)); }
            void play(sound_id id) { state.play_sound(id, room.id, state.world.body_chunk_get_pos(k)); }
        };

        // the creature's template's relationship towards `t`
        creature::relationship rel_to(ctx& x, dcon::creature_id t) { return creature::static_relationship(x.state, x.c, t); }

        // FlyAI.ChangeBehavior
        void change_behavior(ctx& x, behavior nb) {
            behavior const b = x.beh();
            if (b == nb)
                return;
            // (vanilla's `newBehav != Chain || newBehav != Burrow` always holds)
            x.p.has_spot = 0;
            if (b == behavior::chain && x.has_grasp())
                x.play(sound_id::bat_detatch_from_chain);
            else if (nb == behavior::flee)
                x.play(sound_id::bat_startled);
            x.set_beh(nb);
        }

        bool doing_specific_animation(ctx& x) { return x.beh() == behavior::drop || x.beh() == behavior::burrow; }
        bool stuck(ctx& x) { return x.a.stuck_counter >= 40 && x.beh() != behavior::chain && x.beh() != behavior::burrow; }
        bool in_active_swarm_room(ctx& x) { return active_swarm_room(x.state, x.room.id); }
        void lose_all_grasps(ctx& x) {
            if (x.has_grasp())
                creature::release_grasp(x.state, x.c, 0);
        }

        // ArtificialIntelligence-free distance to a node (ExitDistanceForCreature:
        // tiles, -1 unreachable)
        int32_t exit_distance(ctx& x, ivec2 from, int32_t n) {
            if (n < 0 || size_t(n) >= x.info.nodes.size())
                return -1;
            auto const f = pathing::field_to(x.state, x.room.id, x.info.nodes[size_t(n)].tile, movement);
            uint16_t const d = pathing::distance(x.state, f, from);
            return d == pathing::unreachable ? -1 : int32_t(d / pathing::step_cost);
        }
        bool following_hive(ctx& x) {
            return x.a.following >= 0 && size_t(x.a.following) < x.info.nodes.size() && x.info.nodes[size_t(x.a.following)].hive;
        }

        // FlyAI.VisualContact
        bool visual_contact(ctx& x, vec2 point) {
            return (x.beh() != behavior::swarm || custom::dist_less(x.pos(), point, 60.f)) && x.room.visual_contact(x.pos(), point);
        }
        // FlyAI.ValidSwarmPosition (smoothedFloorAltitude as floorAltitude)
        bool valid_swarm_position(ctx& x, vec2 test) {
            int32_t const p = x.prox(test);
            int32_t const fa = pathing::floor_altitude(x.room, tile_of(x.a.local_goal));
            return p > 2 && p < 15 && fa <= 8 && fa <= 12;
        }
        // FlyAI.ChainTile
        bool chain_tile(ctx& x, ivec2 t) {
            auto const& room = x.room;
            if (!room.get_tile(t.x, t.y + 1).solid() && !room.get_tile(t).horizontal_beam()
                && (!room.get_tile(t.x, t.y + 1).vertical_beam() || room.get_tile(t).vertical_beam()))
                return false;
            for (int32_t i = t.y; i > t.y - 5; --i)
                if (room.get_tile(t.x, i).solid() || any_water(room, ivec2{t.x, i}))
                    return false;
            return true;
        }
        // FlyAI.CanIHangFromThisFly
        bool can_hang_from(ctx& x, dcon::creature_id other) {
            auto const& w = x.state.world;
            if (behavior(w.creature_get_behavior(other)) != behavior::chain)
                return false;
            if (next_in_chain(x.state, x.info, other))
                return false;
            if (creature::grasped(w, other, 0) == x.c)
                return false;
            if (last_in_chain(x.state, x.info, other) != other)
                return false;
            auto const first = first_in_chain(w, other);
            return pl(x.state, first).has_spot && behavior(w.creature_get_behavior(first)) == behavior::chain;
        }
        // Fly.CheckChainForLoops
        void check_chain_for_loops(ctx& x) {
            auto f = next_in_chain(x.state, x.info, x.c);
            for (uint32_t n = 0; f && n < 64; ++n) {
                f = next_in_chain(x.state, x.info, f);
                if (f == x.c) {
                    if (auto const nx = next_in_chain(x.state, x.info, x.c))
                        creature::release_grasp(x.state, nx, 0);
                    return;
                }
            }
        }

        // FlyAI.ProgressLocalGoalAlongDijkstraMap
        vec2 progress_local_goal(ctx& x, vec2 goal, int32_t follow) {
            vec2 const fp = x.pos();
            goal.x = mathf::clamp(goal.x, 10.f, x.room.pixel_width() - 10.f);
            goal.y = mathf::clamp(goal.y, 10.f, x.room.pixel_height() - 10.f);
            if (!custom::dist_less(fp, goal, 400.f))
                return x.room.visual_contact(fp, goal) ? goal : fp + custom::dir_vec(fp, goal);
            if (follow < 0 || size_t(follow) >= x.info.nodes.size())
                return fp + custom::dir_vec(fp, goal);
            auto const f = pathing::field_to(x.state, x.room.id, x.info.nodes[size_t(follow)].tile, movement);
            uint16_t const at = pathing::distance(x.state, f, tile_of(x.a.local_goal));
            if (at < pathing::step_cost)
                return x.a.local_goal;
            auto const next = pathing::next_step(x.state, f, tile_of(goal));
            if (!next)
                return fp + custom::dir_vec(fp, goal);
            // (into a pipe: the entrance itself)
            vec2 const v = room_view::middle_of_tile(*next);
            if (x.room.visual_contact(fp, v))
                return v;
            if (x.room.visual_contact(fp, goal))
                return goal;
            return fp + custom::dir_vec(fp, goal);
        }

        // FlyAI.UpdateFollowDijsktra
        void update_follow(ctx& x) {
            auto& a = x.a;
            int32_t const n = int32_t(x.info.nodes.size());
            if (a.leave_room >= 0) {
                a.following = a.leave_room;
                return;
            }
            if (n == 0) {
                a.following = -1;
                return;
            }
            bool repick = a.following < 0 || a.following >= n;
            if (!repick && x.beh() == behavior::idle)
                repick = exit_distance(x, tile_of(x.pos()), a.following) < (following_hive(x) ? 4 : 15);
            if (!repick)
                return;
            if (in_active_swarm_room(x)) {
                // the hives (and vanilla's sky exits)
                int32_t const e = int32_t(x.info.exits);
                int32_t pick = e < n ? x.rng.range(e, n) : x.rng.range(0, n);
                if (pick == a.following)
                    pick = x.rng.range(0, n);
                a.following = int8_t(pick);
            } else {
                a.following = x.info.exits > 0 ? int8_t(x.rng.range(0, int32_t(x.info.exits))) : int8_t(-1);
            }
        }

        // FlyAI.GenericFlightUpdate
        void generic_flight_update(ctx& x) {
            update_follow(x);
            if (x.a.following < 0)
                return;
            x.a.local_goal = progress_local_goal(x, x.a.local_goal, x.a.following);
        }

        // FlyAI.GravitateToFlock
        void gravitate_to_flock(ctx& x, dcon::creature_id other) {
            auto const& f = x.fl();
            if (!other || f.collective == 0.f || x.a.get_back_to_ground > 0)
                return;
            auto const& w = x.state.world;
            auto const& oa = ai(x.state, other);
            vec2 const op = fly_pos(x.state, other);
            vec2 const fp = x.pos();
            vec2 v;
            if (behavior(w.creature_get_behavior(other)) == behavior::flee) {
                v = fp + custom::dir_vec(op, fp) * 60.f;
                v += custom::dir_vec(op, oa.local_goal) * 30.f;
            } else {
                v = rw::lerp(op, oa.local_goal, f.exaggerate);
                if (custom::dist_less(v, op, f.distance_to_others))
                    v = op + custom::dir_vec(op, v) * f.distance_to_others;
                if (custom::dist_less(v, oa.local_goal, f.distance_to_others))
                    v = oa.local_goal + custom::dir_vec(oa.local_goal, v) * f.distance_to_others;
            }
            v = rw::lerp(x.a.local_goal, v, mathf::inverse_lerp(mathf::lerp(50.f, 300.f, f.collective), 40.f, rw::distance(fp, op) * f.collective));
            if (x.prox(v) >= x.prox(fp) && visual_contact(x, v)) {
                x.a.local_goal = v;
                if (oa.following >= 0 && mathf::pow(x.rng.value(), 0.5f) < f.collective && exit_distance(x, tile_of(fp), oa.following) > 10)
                    x.a.following = oa.following;
            }
        }

        // FlyAI.ConsiderOtherCreature
        void consider_other_creature(ctx& x, dcon::creature_id crit) {
            auto& w = x.state.world;
            if (!crit || !creature::here(x.state, crit, x.room.id))
                return;
            auto const r = rel_to(x, crit);
            if (r.type != creature::rel::afraid)
                return;
            for (uint32_t i = 0; i < x.a.threat_count; ++i)
                if (x.a.threats[i].crit == crit)
                    return;
            uint32_t const n = creature::chunk_count(x.state, crit);
            if (n == 0)
                return;
            vec2 const cp = w.body_chunk_get_pos(chunk(x.state, crit, uint32_t(x.rng.range(0, int32_t(n)))));
            float const ad = afraid_distance(r);
            if (!custom::dist_less(x.pos(), cp, ad) || !visual_contact(x, cp))
                return;
            // (vanilla's list is open ended: the first four)
            if (x.a.threat_count < max_threats) {
                threat& t = x.a.threats[x.a.threat_count++];
                t = threat{};
                t.crit = crit;
                t.last_seen_pos = cp;
                t.ticks_since_seen = 0;
                t.delete_me = 0;
                t.visual_contact = 1;
                t.currently_scared_of = 0;
                t.ticks_to_forget = int16_t(mathf::lerp(50.f, 250.f, r.intensity));
            }
            if (x.beh() != behavior::chain) {
                change_behavior(x, behavior::flee);
                x.a.panic_flee += 10 + int32_t(ad - rw::distance(x.pos(), cp));
            }
        }

        // TrackedThreat.Severity
        float severity(ctx& x, threat const& t) {
            if (!creature::live(x.state, t.crit))
                return 0.f;
            return rel_to(x, t.crit).intensity * mathf::lerp(1.f, 0.f, float(t.ticks_since_seen) / float(std::max<int16_t>(t.ticks_to_forget, 1)));
        }

        // TrackedThreat.Update
        void threat_update(ctx& x, threat& t) {
            auto const& w = x.state.world;
            bool const here = creature::here(x.state, t.crit, x.room.id);
            ++t.ticks_since_seen;
            if (t.ticks_since_seen > 10) {
                t.visual_contact = 0;
                if (here) {
                    uint32_t const n = creature::chunk_count(x.state, t.crit);
                    if (n > 0) {
                        vec2 const cp = w.body_chunk_get_pos(chunk(x.state, t.crit, uint32_t(x.rng.range(0, int32_t(n)))));
                        if (visual_contact(x, cp)) {
                            t.visual_contact = 1;
                            t.ticks_since_seen = 0;
                        }
                    }
                }
                if (t.ticks_since_seen > t.ticks_to_forget)
                    t.delete_me = 1;
            } else if (here) {
                t.last_seen_pos = creature::main_pos(x.state, t.crit);
            } else {
                t.visual_contact = 0;
            }
            float const ad = creature::live(x.state, t.crit) ? afraid_distance(rel_to(x, t.crit)) : 0.f;
            t.currently_scared_of = t.visual_contact && custom::dist_less(x.pos(), t.last_seen_pos, ad);
        }

        // FlyAI.UpdateThreats
        void update_threats(ctx& x) {
            auto& a = x.a;
            if (a.panic_flee > 0)
                --a.panic_flee;
            if (a.threat_count > 0) {
                float num = 0.f;
                for (uint32_t i = 0; i < a.threat_count; ++i) {
                    auto const& t = a.threats[i];
                    num += 600.f * severity(x, t) / mathf::max(rw::distance(t.last_seen_pos, x.pos()), 0.001f);
                    if (x.beh() == behavior::chain && t.last_seen_pos.y < x.pos().y - 80.f)
                        num /= 5.f;
                    else if (a.panic_flee < 1 && t.visual_contact && creature::live(x.state, t.crit)
                    && custom::dist_less(x.pos(), t.last_seen_pos, afraid_distance(rel_to(x, t.crit)) / 3.f))
                        a.panic_flee = 1;
                }
                if (num > 1.f)
                    a.afraid += num / 100.f;
                else
                    a.afraid -= 0.0125f;
                a.afraid = mathf::clamp(a.afraid, num > 1.f ? 1.f : 0.f, 2.f);
                if (a.panic_flee > 0)
                    a.afraid = 2.f;
            } else if (!a.flee_from_rain) {
                a.afraid = 0.f;
            }
            if (x.contact().x != 0 || x.contact().y != 0)
                a.panic_flee = 0;
            if (!x.info.creatures.empty())
                consider_other_creature(x, x.info.creatures[size_t(x.rng.range(0, int32_t(x.info.creatures.size())))]);
            // (deleteMeNextFrame ones out, the rest updated)
            uint32_t keep = 0;
            for (uint32_t i = 0; i < a.threat_count; ++i) {
                if (a.threats[i].delete_me)
                    continue;
                threat t = a.threats[i];
                threat_update(x, t);
                a.threats[keep++] = t;
            }
            a.threat_count = uint8_t(keep);
        }

        // FlyAI.ConsiderOtherFly
        void consider_other_fly(ctx& x, dcon::creature_id other) {
            auto const& oa = ai(x.state, other);
            vec2 const op = fly_pos(x.state, other);
            if (op.y < x.a.lowest_other_pos.y && valid_swarm_position(x, op))
                x.a.lowest_other_pos = op;
            float const dto = x.fl().distance_to_others;
            if (custom::dist_less(oa.local_goal, x.a.local_goal, dto)) {
                vec2 const v = oa.local_goal + custom::dir_vec(oa.local_goal, x.a.local_goal) * dto;
                if (x.prox(v) > 1 && visual_contact(x, v))
                    x.a.local_goal = v;
            }
            flock_params const of = flock_of(oa.flock_seed);
            if (flock_dominance(x.state, oa, of) > flock_dominance(x.state, x.a, x.fl())) {
                x.a.flock_seed = oa.flock_seed;
                x.a.flock_birth = oa.flock_birth;
            }
            if (oa.threat_count > 0)
                consider_other_creature(x, oa.threats[x.rng.range(0, int32_t(oa.threat_count))].crit);
        }

        // FlyAI.IdleUpdate
        void idle_update(ctx& x, dcon::creature_id other) {
            auto& a = x.a;
            auto const& w = x.state.world;
            generic_flight_update(x);
            if (other)
                gravitate_to_flock(x, other);
            if (a.no_swarm_counter > 0) {
                --a.no_swarm_counter;
            } else if (in_active_swarm_room(x) && following_hive(x) && exit_distance(x, tile_of(x.pos()), a.following) < 30) {
                vec2 const test = rw::lerp(x.pos(), a.local_goal, 0.5f);
                if (valid_swarm_position(x, test)) {
                    a.local_goal = test;
                    change_behavior(x, behavior::swarm);
                }
            }
            if (!(a.afraid < 0.5f && x.fl().form_chains && !a.flee_from_rain))
                return;
            bool const flag = other && !point_submerged(x.room, fly_pos(x.state, other), -40.f);
            if (other && x.rng.value() < 0.33333334f && can_hang_from(x, other) && !x.room.get_tile(fly_pos(x.state, other) + vec2{0.f, -20.f}).solid() && flag) {
                vec2 const op = fly_pos(x.state, other);
                if (!custom::dist_less(op, x.pos(), 20.f)) {
                    change_behavior(x, behavior::idle);
                    a.local_goal = op;
                    a.following = -1;
                    return;
                }
                change_behavior(x, behavior::chain);
                creature::grab(x.state, x.c, 0, other, 0);
                check_chain_for_loops(x);
                x.play(sound_id::bat_attatch_to_chain);
                (void)w;
            } else if (x.rng.value() < 0.125f && chain_tile(x, tile_of(x.pos()))) {
                change_behavior(x, behavior::chain);
                vec2 const fp = x.pos();
                x.p.has_spot = 1;
                if (x.room.get_tile(fp).horizontal_beam())
                    x.p.spot = vec2{fp.x, room_view::middle_of_tile(fp).y - 4.f};
                else if (!x.room.get_tile(fp).vertical_beam() && x.room.get_tile(fp + vec2{0.f, 20.f}).vertical_beam())
                    x.p.spot = room_view::middle_of_tile(fp) + vec2{0.f, 10.f};
                else
                    x.p.spot = vec2{fp.x, room_view::middle_of_tile(fp).y + 10.f};
            }
        }

        // FlyAI.SwarmUpdate
        void swarm_update(ctx& x, dcon::creature_id other) {
            auto& a = x.a;
            a.local_goal.y += 0.1f;
            if (other)
                gravitate_to_flock(x, other);
            if (x.pos().y < a.lowest_other_pos.y && valid_swarm_position(x, x.pos()))
                a.lowest_other_pos = x.pos();
            float const rx = x.rng.value();
            vec2 const v{rx * x.room.pixel_width(), x.rng.value() * a.lowest_other_pos.y};
            if (valid_swarm_position(x, v) && x.room.visual_contact(x.pos(), v))
                a.lowest_other_pos = v;
            if (!valid_swarm_position(x, a.local_goal) || x.room.get_tile(a.local_goal + vec2{0.f, 15.f}).solid() || a.local_goal.y > x.room.pixel_height()) {
                if (valid_swarm_position(x, a.lowest_other_pos)) {
                    a.local_goal = a.lowest_other_pos + vec2{mathf::lerp(-10.f, 10.f, x.rng.value()), -15.f};
                    a.lowest_other_pos = vec2{a.local_goal.x, x.room.pixel_height()};
                } else {
                    change_behavior(x, behavior::idle);
                }
            }
            if (!custom::dist_less(x.pos(), a.local_goal, 400.f) || !x.room.visual_contact(x.pos(), a.local_goal))
                change_behavior(x, behavior::idle);
        }

        // FlyAI.FleeUpdate
        void flee_update(ctx& x) {
            auto& a = x.a;
            auto& w = x.state.world;
            int32_t tt = -1;
            float best = 0.f;
            for (uint32_t i = 0; i < a.threat_count; ++i) {
                float const s = severity(x, a.threats[i]) / mathf::max(rw::distance(x.pos(), a.threats[i].last_seen_pos), 0.001f);
                if (s > best) {
                    best = s;
                    tt = int32_t(i);
                }
            }
            if (a.panic_flee > 0 && !doing_specific_animation(x) && tt >= 0 && !stuck(x)) {
                x.p.dir = custom::dir_vec(a.threats[tt].last_seen_pos, x.pos());
                x.p.mode = mov_mode::panic;
                ivec2 const t = tile_of(x.pos());
                if (x.contact().x != 0 || x.contact().y != 0 || !x.room.inside(t.x, t.y))
                    a.panic_flee = 0;
            } else {
                // the node the threats are farthest behind (no dens among
                // these; an exit loses 20)
                float best_node = std::numeric_limits<float>::lowest();
                ivec2 const ft = tile_of(x.pos());
                for (int32_t i = 0; i < int32_t(x.info.nodes.size()); ++i) {
                    float s = 0.f;
                    int32_t const df = exit_distance(x, ft, i);
                    for (uint32_t j = 0; j < a.threat_count; ++j) {
                        int32_t const dt = exit_distance(x, tile_of(a.threats[j].last_seen_pos), i);
                        s += float(dt - df) * mathf::lerp(0.5f, 1.f, severity(x, a.threats[j]));
                        if (dt < df)
                            s -= 1000.f;
                    }
                    if (!x.info.nodes[size_t(i)].hive)
                        s -= 20.f;
                    if (s > best_node) {
                        best_node = s;
                        a.following = int8_t(i);
                    }
                }
            }
            generic_flight_update(x);
            if (in_active_swarm_room(x) && !stuck(x) && tt >= 0) {
                auto const& t = a.threats[tt];
                bool const flies = creature::live(x.state, t.crit) && can_fly(creature::template_of(x.state, t.crit));
                if (!flies && t.currently_scared_of && x.p.dir.y > -0.5f && custom::dir_vec(x.pos(), t.last_seen_pos).y < 0.5f) {
                    int32_t num4 = 0;
                    ivec2 const tp = tile_of(x.pos());
                    for (int32_t y = tp.y; y < x.room.height && !x.room.get_tile(tp.x, y).solid(); ++y)
                        ++num4;
                    if (num4 >= 3) {
                        vec2 v = custom::dir_vec(x.pos(), a.local_goal);
                        v = rw::slerp(v, vec2{0.f, 1.f}, 0.2f);
                        a.local_goal = x.pos() + v * rw::magnitude(a.local_goal);
                    }
                }
            }
            if (a.afraid >= 1.f) {
                if (x.room.get_tile(x.pos()).flags & world::tile_hive) {
                    x.vel().y -= 1.f;
                    return;
                }
                ivec2 const at = tile_of(x.pos());
                for (int32_t y = at.y; y > at.y - 10 && !x.room.get_tile(at.x, y).solid(); --y) {
                    if (!(x.room.get_tile(at.x, y).flags & world::tile_hive))
                        continue;
                    bool flag = true;
                    for (uint32_t j = 0; j < a.threat_count; ++j) {
                        ivec2 const t = tile_of(a.threats[j].last_seen_pos);
                        if (t.x >= at.x - 2 && t.x <= at.x + 2 && t.y >= y - 1 && t.y <= at.y + 1) {
                            flag = false;
                            break;
                        }
                    }
                    if (flag)
                        a.drop = drop_status::dropping;
                }
            }
            (void)w;
        }

        // FlyAI.DropUpdate
        void drop_update(ctx& x) {
            vec2 const fp = x.pos();
            if (x.contact().y < 0 || any_water(x.room, tile_of(fp + vec2{0.f, -20.f})) || fp.y < -50.f) {
                x.a.drop = drop_status::has_dropped;
                if (x.room.get_tile(fp).flags & world::tile_hive) {
                    change_behavior(x, behavior::burrow);
                    x.p.has_spot = 1;
                    x.p.spot = fp;
                } else {
                    change_behavior(x, behavior::idle);
                }
            }
            x.vel().x *= 0.6f;
        }

        // FliesWorldAI.MigrationDirection's room, from inside a room (below)
        dcon::room_id migration_direction(sys::game_state const& state, dcon::room_id room, rw::rng& rng);

        // FlyAI.FleeFromRainUpdate
        void flee_from_rain_update(ctx& x) {
            auto& a = x.a;
            a.afraid = 2.f;
            int32_t const n = int32_t(x.info.nodes.size());
            if (n > int32_t(x.info.exits)) {
                // the closest hive
                int32_t num = -1;
                int32_t best = std::numeric_limits<int32_t>::max();
                ivec2 const ft = tile_of(x.pos());
                for (int32_t i = int32_t(x.info.exits); i < n; ++i) {
                    int32_t const d = exit_distance(x, ft, i);
                    if (d < best) {
                        best = d;
                        num = i;
                    }
                }
                if (num != -1) {
                    a.leave_room = -1;
                    a.following = int8_t(num);
                }
            } else if (a.leave_room < 0 || (x.state.tick + uint32_t(x.c.value)) % 40u == 0u) {
                // (worked out every 40 ticks: vanilla's every tick)
                a.leave_room = -1;
                if (auto const to = migration_direction(x.state, x.room.id, x.rng))
                    for (uint32_t i = 0; i < x.info.exits; ++i)
                        if (x.info.nodes[i].target == to) {
                            a.leave_room = int8_t(i);
                            break;
                        }
                a.following = a.leave_room;
            }
            if (a.following < 0)
                return;
            a.local_goal = progress_local_goal(x, a.local_goal, a.following);
        }

        // FlyAI.HangInChainUpdate
        void hang_in_chain_update(ctx& x, dcon::creature_id other) {
            auto& a = x.a;
            auto& w = x.state.world;
            if (!x.has_grasp()) {
                if (!x.p.has_spot) {
                    change_behavior(x, behavior::idle);
                    return;
                }
                float const num = rw::distance(x.pos(), x.p.spot);
                if (num > 8.f) {
                    vec2 const d = custom::dir_vec(x.pos(), x.p.spot);
                    x.pos() -= d * (8.f - num);
                    x.vel() -= d * (8.f - num);
                }
                if ((x.rng.value() < 0.05f && a.afraid > 1.9f) || (x.rng.value() < 0.008333334f && last_in_chain(x.state, x.info, x.c) == x.c))
                    change_behavior(x, behavior::idle);
                if (other && behavior(w.creature_get_behavior(other)) == behavior::chain && !creature::grasped_object(w, other, 0)
                && custom::dist_less(fly_pos(x.state, other), x.pos(), 30.f))
                    change_behavior(x, behavior::idle);
            } else {
                auto const g = grasped_fly(w, x.c);
                if (!g || (x.rng.value() < 0.007142857f && pl(x.state, first_in_chain(w, x.c)).mode != mov_mode::hang)
                || (x.rng.value() < 0.05f && a.panic_flee > 0) || (x.rng.value() < 0.0035714286f && a.afraid == 2.f) || a.leave_room > -1
                || x.rng.value() < 0.0002f) {
                    change_behavior(x, behavior::idle);
                } else if (x.rng.value() < 0.004166667f && last_in_chain(x.state, x.info, x.c) == x.c && first_in_chain(w, x.c) == g) {
                    change_behavior(x, behavior::idle);
                }
            }
            bool const flag = point_submerged(x.room, x.pos(), -10.f);
            if (x.contact().y < 0 || flag || a.flee_from_rain)
                change_behavior(x, behavior::idle);
        }

        // FlyAI.Update
        void ai_update(ctx& x) {
            auto& a = x.a;
            auto& w = x.state.world;
            if (a.get_back_to_ground > 0)
                --a.get_back_to_ground;
            else if (x.prox(x.pos()) > 8 || pathing::floor_altitude(x.room, tile_of(x.pos())) > 8)
                a.get_back_to_ground = int16_t(x.rng.range(80, 200));
            dcon::creature_id other;
            if (doing_specific_animation(x)) {
                lose_all_grasps(x);
            } else {
                // FliesRoomAI.GetRandomFly().LastInChain()
                if (!x.info.flies.empty()) {
                    auto const r = x.info.flies[size_t(x.rng.range(0, int32_t(x.info.flies.size())))];
                    if (is_fly(w, r) && creature::here(x.state, r, x.room.id))
                        other = last_in_chain(x.state, x.info, r);
                }
                if (other && (other == x.c || !x.room.visual_contact(x.pos(), fly_pos(x.state, other))))
                    other = dcon::creature_id{};
                if (other)
                    consider_other_fly(x, other);
                // (Remix's RoomNotACycleHazard exemption is off)
                if (a.flee_from_rain)
                    change_behavior(x, behavior::flee_from_rain);
                else if (a.afraid >= 1.f && a.lured_counter < 1)
                    change_behavior(x, behavior::flee);
                if (x.has_grasp())
                    x.set_beh(behavior::chain);
                if (a.drop == drop_status::dropping)
                    change_behavior(x, behavior::drop);
            }
            if (a.lured_counter > 0)
                --a.lured_counter;
            x.p.mode = mov_mode::bat_flight;
            switch (x.beh()) {
            case behavior::idle:
                idle_update(x, other);
                break;
            case behavior::swarm:
                a.no_swarm_counter = int16_t(x.rng.range(20, 80));
                x.p.mode = mov_mode::swarm_flight;
                swarm_update(x, other);
                break;
            case behavior::flee:
                flee_update(x);
                if (a.afraid <= 0.f)
                    change_behavior(x, behavior::idle);
                break;
            case behavior::drop:
                x.p.mode = mov_mode::passive;
                drop_update(x);
                break;
            case behavior::burrow:
                x.p.mode = mov_mode::burrow;
                if (!x.p.has_spot || !custom::dist_less(x.pos(), x.p.spot, 100.f)) {
                    x.p.has_spot = 0;
                    change_behavior(x, behavior::idle);
                }
                break;
            case behavior::flee_from_rain:
                flee_from_rain_update(x);
                break;
            case behavior::chain:
                if ((x.p.has_spot && custom::dist_less(x.pos(), x.p.spot, 40.f)) || x.has_grasp()) {
                    x.p.mode = mov_mode::hang;
                    hang_in_chain_update(x, other);
                } else {
                    change_behavior(x, behavior::idle);
                }
                break;
            default:
                break;
            }
            // (the flock's sinCycle and life run by themselves; a spent one
            // is replaced)
            if (flock_life(x.state, a, x.fl()) <= 0.f)
                new_flock(x.state, x.c, a);
            if (x.beh() != behavior::swarm && a.lured_counter < 1)
                update_threats(x);
            bool const hive_tile = (x.room.get_tile(x.pos()).flags & world::tile_hive) != 0;
            if (a.afraid >= 1.f && hive_tile)
                x.vel().y -= 1.f;
            if (x.contact().y == -1 && a.afraid >= 0.75f && hive_tile) {
                change_behavior(x, behavior::burrow);
                x.p.has_spot = 1;
                x.p.spot = x.pos();
            }
            if (x.rng.value() < 0.0125f && x.beh() != behavior::flee && rain_approaching(x.state, x.room.id) < 0.5f)
                a.flee_from_rain = 1;
            // not vanilla: the cycle starts over here (no new world), so a
            // fly that fled the last rain calms down once the next is far
            if (a.flee_from_rain && rain_approaching(x.state, x.room.id) >= 0.5f && !rain_game_over(x.state, x.room.id)) {
                a.flee_from_rain = 0;
                a.leave_room = -1;
                if (x.beh() == behavior::flee_from_rain)
                    change_behavior(x, behavior::idle);
            }
            if (x.rng.value() < 0.016666668f && x.beh() != behavior::chain && x.has_grasp())
                lose_all_grasps(x);
            if (x.beh() != behavior::chain && x.beh() != behavior::burrow) {
                if (a.stuck_counter < 40) {
                    if (!custom::dist_less(x.pos(), a.stuck_pos, 40.f)) {
                        a.stuck_counter = 0;
                        a.stuck_pos = x.pos();
                    } else if (++a.stuck_counter == 40) {
                        a.stuck_counter = 0;
                        ivec2 const t = tile_of(x.pos());
                        for (auto const d : custom::eight_directions)
                            if (x.room.get_tile(t + d).solid()) {
                                a.stuck_counter = 120;
                                break;
                            }
                    }
                } else {
                    --a.stuck_counter;
                }
            }
            a.afraid = mathf::clamp(a.afraid - 0.003125f, 0.f, 1.f);
        }

        //
        // Fly's movement
        //
        // FlyAI.NewRoom (+ Fly.NewRoom's collision layer: always 0 here)
        void ai_new_room(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& a = ai(state, c);
            a.local_goal = fly_pos(state, c);
            a.following = -1;
            a.leave_room = -1;
            a.lowest_other_pos = vec2{a.local_goal.x, room.pixel_height()};
        }

        // into the room's hive (Fly.Burrowed: FlyAI.Burrowed, the sound,
        // FliesRoomAI.MoveFlyToHive)
        void burrowed(ctx& x) {
            auto& a = x.a;
            change_behavior(x, behavior::idle);
            a.drop = drop_status::havent_dropped;
            a.afraid = 0.f;
            a.threat_count = 0;
            x.play(sound_id::bat_dive_into_grass);
            if (!has_hives(x.state, x.room.id) || creature::held(x.state, x.c))
                return;
            lose_all_grasps(x);
            // (out of the room: a transaction)
            x.state.defer(x.room.id, [c = x.c, r = x.room.id](sys::game_state& st) {
                auto& w = st.world;
                if (!is_fly(w, c) || w.creature_get_away(c) != 0 || creature::room_of(st, c) != r)
                    return;
                creature::release_all(st, c, r);
                w.creature_set_away(c, uint8_t(dens::where::den));
                w.creature_set_away_room(c, r);
                w.creature_set_remain_in_den(c, -1);
                w.creature_set_entering_shortcut(c, ivec2{-1, -1});
                w.physical_object_set_room_from_object_location(body(st, c), dcon::room_id{});
                pl(st, c).in_hive = 1;
                pl(st, c).has_spot = 0;
            });
        }

        // Fly.BatFlight
        void bat_flight(ctx& x, bool panic) {
            auto& p = x.p;
            auto& a = x.a;
            float const g = physics::gravity(x.state, body(x.state, x.c), x.room);
            if (!panic) {
                p.dir = custom::dir_vec(x.pos(), a.local_goal);
                if (x.room.get_tile(x.pos() + x.vel() * 3.f).solid())
                    p.dir = rw::lerp(p.dir, -rw::normalized(x.vel()), 0.3f);
                bool flag = false;
                if (!stuck(x) && x.prox(a.local_goal) > 1) {
                    int32_t const tp = x.prox(x.pos());
                    vec2 v{0.f, 0.f};
                    if (tp < 5) {
                        ivec2 const t = tile_of(x.pos());
                        for (auto const d : custom::eight_directions)
                            if (pathing::ai_terrain_proximity(x.state, x.room.id, t + d) < tp) {
                                v += custom::dir_vec(x.pos(), room_view::middle_of_tile(t - d));
                                flag = true;
                            }
                    }
                    if (flag)
                        p.dir = rw::slerp(p.dir, rw::normalized(v), p.wall_avoidance * 0.3f);
                }
                p.wall_avoidance += flag ? -0.05f : 0.025f;
                p.wall_avoidance = mathf::clamp(p.wall_avoidance, 0.f, 1.f);
            }
            vec2& vel = x.vel();
            vel *= 0.92f;
            vel.y += g * 0.45f;
            vel.x += p.dir.x * 0.6f;
            if (x.rng.value() < 0.025f)
                x.play(x.rng.value() < a.afraid ? sound_id::bat_afraid_flying_sounds : sound_id::bat_idle_flying_sounds);
            if (panic)
                vel += p.dir * 0.2f;
            p.flap_depth = mathf::inverse_lerp(-1.f, 1.f, p.dir.y);
            p.flap += p.flap_speed * (panic ? 1.2f : 1.f);
            if (p.flap_speed > 0.f) {
                vel.y += mathf::pow(1.f + p.flap_speed, 1.5f) * mathf::lerp(1.1f, 1.7f, p.flap_depth);
                if (p.flap > 1.f) {
                    p.flap = 1.f;
                    p.flap_speed = -0.04f - mathf::inverse_lerp(-1.f, 1.f, p.dir.y) * 0.15f;
                }
                if (x.contact().x != 0)
                    vel.x = float(x.contact().x) * -6.f;
            } else if (p.flap_speed < 0.f && p.flap < 0.f) {
                p.flap = 0.f;
                p.flap_speed = 0.2f + mathf::inverse_lerp(-1.f, 1.f, p.dir.y) * 0.8f;
                x.play(sound_id::fly_wing_flap);
            }
        }

        // Fly.SwarmFlight
        void swarm_flight(ctx& x) {
            auto& p = x.p;
            auto const& f = x.fl();
            float num = x.sin_cycle() + x.a.local_goal.y / f.swarm_vertical_displace;
            num = std::sin(num * 2.f * 3.1415927f) * f.sin_ampl;
            p.dir = custom::dir_vec(x.pos(), x.a.local_goal + vec2{num, 0.f});
            vec2& vel = x.vel();
            vel *= 0.89f;
            vel.y += physics::gravity(x.state, body(x.state, x.c), x.room) * 0.9f;
            vel += p.dir * 0.6f;
            // Bat_Hover_Fly_LOOP, kept up while it hovers
            x.state.play_loop(sound::loop_key(sound::loop_owner::creature, uint32_t(x.c.index())), sound_id::bat_hover_fly_loop, x.room.id, x.pos(), 1.f,
                0.9f + 0.2f * mathf::inverse_lerp(-4.f, 4.f, vel.y));
            p.flap = 1.f;
            p.flap_depth = 0.5f;
            p.flap_speed = 1.f;
            if (x.contact().x != 0)
                vel.x = float(x.contact().x) * -6.f;
        }

        // Fly.Hanging
        void hanging(ctx& x) {
            auto& p = x.p;
            if (!x.has_grasp() && p.has_spot) {
                float const num = rw::distance(x.pos(), p.spot);
                if (num > 8.f) {
                    vec2 const d = custom::dir_vec(x.pos(), p.spot);
                    x.pos() -= d * (8.f - num);
                    x.vel() -= d * (8.f - num);
                }
            }
            p.flap_depth = 1.f;
            if (p.chain_variation == 0 && x.rng.value() < 0.00066666666f)
                p.chain_variation = uint8_t(x.rng.range(1, 5));
            else if (x.rng.value() < 0.00625f)
                p.chain_variation = 0;
            switch (p.chain_variation) {
            case 1:
                x.vel().x += std::sin(x.sin_cycle() * 2.f * 3.1415927f);
                p.flap_depth = 0.1f;
                p.flap = x.rng.value();
                break;
            case 2:
                p.flap_depth = 0.3f;
                p.flap = mathf::clamp(p.flap + x.rng.value() * x.rng.value() * 0.1f, 0.f, 1.f);
                break;
            case 3:
                x.vel() += custom::deg_to_vec(x.rng.value() * 360.f) * 0.1f;
                p.flap_depth = 0.3f;
                p.flap = 1.f;
                break;
            case 4: {
                float const r1 = x.rng.value();
                float const r2 = x.rng.value();
                x.vel() += custom::deg_to_vec(x.rng.value() * 360.f) * r1 * r2 * 0.2f;
                break;
            }
            default:
                break;
            }
        }

        // Fly.Act
        void act(ctx& x) {
            ai_update(x);
            if (stuck(x))
                x.vel() += custom::deg_to_vec(x.rng.value() * 360.f) * 2.f;
            switch (x.p.mode) {
            case mov_mode::bat_flight:
                bat_flight(x, false);
                return;
            case mov_mode::swarm_flight:
                swarm_flight(x);
                return;
            case mov_mode::burrow:
                if (x.p.has_spot) {
                    x.pos() = x.p.spot + vec2{mathf::lerp(-2.f, 2.f, x.rng.value()), 0.f};
                    x.p.spot += vec2{0.f, -0.4f};
                    if (x.room.get_tile(x.p.spot + vec2{0.f, 5.f}).solid())
                        burrowed(x);
                }
                return;
            case mov_mode::panic:
                bat_flight(x, true);
                return;
            case mov_mode::hang:
                hanging(x);
                return;
            default:
                return;
            }
        }

        // Fly.WaterBehavior
        void water_behavior(ctx& x, float submersion) {
            auto& p = x.p;
            if (submersion == 1.f) {
                p.flap = mathf::clamp(p.flap + mathf::lerp(-1.f, 1.f, x.rng.value()) * 0.1f, 0.f, 1.f);
                x.vel() += custom::deg_to_vec(-45.f + 90.f * x.rng.value()) * 0.75f;
                return;
            }
            if (!creature::held(x.state, x.c) || !x.has_grasp())
                x.vel().y += 7.f;
        }

        //
        // FliesWorldAI (the parts a room's update reads: rooms and routes
        // only change serially)
        //
        // the swarm rooms of each region (a cache: the world file's flags
        // never change once loaded)
        std::vector<dcon::room_id> const& swarm_rooms(sys::game_state const& state, dcon::region_id region) {
            thread_local sys::game_state const* for_state = nullptr;
            thread_local uint32_t for_rooms = 0;
            thread_local std::vector<std::vector<dcon::room_id>> lists;
            auto const& w = state.world;
            if (for_state != &state || for_rooms != w.room_size()) {
                for_state = &state;
                for_rooms = w.room_size();
                lists.assign(size_t(w.region_size()), {});
                w.for_each_room([&](dcon::room_id r) {
                    if (!w.room_get_is_swarm_room(r))
                        return;
                    auto const reg = w.room_get_region_from_region_membership(r);
                    if (reg && size_t(reg.index()) < lists.size())
                        lists[size_t(reg.index())].push_back(r);
                });
            }
            static std::vector<dcon::room_id> const none;
            return region && size_t(region.index()) < lists.size() ? lists[size_t(region.index())] : none;
        }

        // the room an exit (by exitAndDenIndex) of `room` leads to
        dcon::room_id exit_leads_to(sys::game_state const& state, dcon::room_id room, int32_t node) {
            auto const& w = state.world;
            dcon::room_id to;
            w.room_for_each_exit_location(room, [&](dcon::exit_location_id el) {
                auto const e = w.exit_location_get_exit(el);
                if (!to && int32_t(w.room_exit_get_index(e)) == node)
                    to = dens::exit_target(state, e, room);
            });
            return to;
        }

        // FliesWorldAI.GetSwarmRoomDistance (SwarmRoomMapper's distances):
        // here the rooms to go through along dens.hpp's route, -1 none
        float swarm_room_distance(sys::game_state const& state, dcon::room_id from, dcon::room_id swarm) {
            if (!from || !swarm)
                return -1.f;
            if (from == swarm)
                return 0.f;
            auto const it = state.routes.find(uint32_t(swarm.index()));
            if (it == state.routes.end())
                return -1.f;
            auto r = from;
            for (int32_t n = 1; n <= 64; ++n) {
                if (size_t(r.index()) >= it->second.size())
                    return -1.f;
                int16_t const node = it->second[size_t(r.index())];
                if (node < 0)
                    return -1.f;
                r = exit_leads_to(state, r, node);
                if (!r)
                    return -1.f;
                if (r == swarm)
                    return float(n);
            }
            return -1.f;
        }

        // FliesWorldAI.Behavior
        enum class swarm_behavior : uint8_t { inactive, spread, gather, migrate };

        // FliesWorldAI.SwarmRoomBehavior
        swarm_behavior swarm_room_behavior(sys::game_state const& state, dcon::room_id swarm) {
            if (!swarm || !active_swarm_room(state, swarm))
                return swarm_behavior::inactive;
            if (rain_approaching(state, swarm) < 0.75f)
                return swarm_behavior::gather;
            return swarm_behavior(state.world.room_get_swarm_behavior(swarm));
        }

        // FliesWorldAI.ClosestSwarmRoom (the same region's)
        dcon::room_id closest_swarm_room(sys::game_state const& state, dcon::room_id room) {
            if (active_swarm_room(state, room))
                return room;
            dcon::room_id result;
            float best = float_max;
            for (auto const s : swarm_rooms(state, state.world.room_get_region_from_region_membership(room))) {
                if (swarm_room_behavior(state, s) == swarm_behavior::inactive)
                    continue;
                float const d = swarm_room_distance(state, room, s);
                if (d > -1.f && d < best) {
                    best = d;
                    result = s;
                }
            }
            return result;
        }

        // The flies out in each room, outside the rooms' update (when the
        // room index isn't there): one pass over the creatures a tick
        struct fly_table {
            sys::game_state const* state = nullptr;
            uint32_t tick = 0;
            uint32_t room_count = 0;
            std::vector<uint32_t> first;
            std::vector<dcon::creature_id> ids;
        };
        fly_table const& flies_by_room(sys::game_state const& state) {
            thread_local fly_table t;
            auto const& w = state.world;
            if (t.state == &state && t.tick == state.tick && t.room_count == w.room_size())
                return t;
            t.state = &state;
            t.tick = state.tick;
            t.room_count = w.room_size();
            t.first.assign(size_t(w.room_size()) + 1, 0u);
            t.ids.clear();
            std::vector<std::pair<uint32_t, dcon::creature_id>> all;
            w.for_each_creature([&](dcon::creature_id c) {
                if (w.creature_get_type(c) != creature::type::batfly || w.creature_get_away(c) != 0)
                    return;
                auto const r = creature::room_of(state, c);
                if (r && size_t(r.index()) < size_t(w.room_size()))
                    all.emplace_back(uint32_t(r.index()), c);
            });
            std::stable_sort(all.begin(), all.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
            for (auto const& [r, c] : all) {
                ++t.first[size_t(r) + 1];
                t.ids.push_back(c);
            }
            for (size_t r = 0; r + 1 < t.first.size(); ++r)
                t.first[r + 1] += t.first[r];
            return t;
        }
        // the flies out in `room`, in id order
        template<typename F>
        void for_each_fly_in(sys::game_state const& state, dcon::room_id room, F&& f) {
            if (!room)
                return;
            if (state.index.valid) {
                rooms::for_each_creature(state, room, creature::type::batfly, f);
                return;
            }
            auto const& t = flies_by_room(state);
            size_t const r = size_t(room.index());
            if (r + 1 >= t.first.size())
                return;
            for (uint32_t i = t.first[r]; i < t.first[r + 1]; ++i)
                f(t.ids[i]);
        }

        // AbstractRoom.NumberOfQuantifiedCreatureInRoom(Fly): the flies out
        // in it (what the room index holds while the rooms update)
        int32_t flies_in(sys::game_state const& state, dcon::room_id room) {
            int32_t n = 0;
            for_each_fly_in(state, room, [&](dcon::creature_id) { ++n; });
            return n;
        }

        // the rooms a room's exits lead to (AbstractRoom.connections)
        template<typename F>
        void for_each_connection(sys::game_state const& state, dcon::room_id room, F&& f) {
            auto const& w = state.world;
            auto const region = w.room_get_region_from_region_membership(room);
            w.room_for_each_exit_location(room, [&](dcon::exit_location_id el) {
                auto const to = dens::exit_target(state, w.exit_location_get_exit(el), room);
                // (a world is a region in vanilla)
                if (to && w.room_get_region_from_region_membership(to) == region)
                    f(to);
            });
        }

        // FliesWorldAI.MigrationDirection
        dcon::room_id migration_direction(sys::game_state const& state, dcon::room_id room, rw::rng& rng) {
            dcon::room_id result;
            auto const closest = closest_swarm_room(state, room);
            if (active_swarm_room(state, room) && flies_in(state, room) < 6)
                return result;
            float best = float_max;
            switch (swarm_room_behavior(state, closest)) {
            case swarm_behavior::gather:
                if (room != closest && closest)
                    for_each_connection(state, room, [&](dcon::room_id to) {
                        float const d = swarm_room_distance(state, to, closest);
                        if (d < best && d > -1.f) {
                            best = d;
                            result = to;
                        }
                    });
                break;
            case swarm_behavior::spread: {
                int32_t const here = flies_in(state, room);
                for_each_connection(state, room, [&](dcon::room_id to) {
                    float const v = float(flies_in(state, to)) * 20.f + rng.value() - swarm_room_distance(state, to, closest);
                    if (v <= float(here) && v < best) {
                        best = v;
                        result = to;
                    }
                });
                break;
            }
            case swarm_behavior::migrate: {
                auto const target = state.world.room_get_swarm_migration_target(closest);
                if (target && room != target)
                    for_each_connection(state, room, [&](dcon::room_id to) {
                        float const d = swarm_room_distance(state, to, target);
                        if (d != -1.f && d < best) {
                            best = d;
                            result = to;
                        }
                    });
                break;
            }
            default:
                break;
            }
            return result;
        }

        // a random hive tile of `room` (FliesRoomAI.RandomHiveNode's tile)
        bool random_hive_tile(sys::game_state const& state, dcon::room_id room, rw::rng& rng, ivec2& out) {
            auto const& w = state.world;
            std::vector<dcon::bat_hive_id> hives;
            w.room_for_each_bat_hive_location(room, [&](dcon::bat_hive_location_id l) { hives.push_back(w.bat_hive_location_get_hive(l)); });
            if (hives.empty())
                return false;
            auto const tiles = w.bat_hive_get_tiles(hives[size_t(rng.range(0, int32_t(hives.size())))]);
            if (tiles.size() == 0)
                return false;
            out = tiles[size_t(rng.range(0, int32_t(tiles.size())))];
            return true;
        }

        // Room.PlaceQuantifiedCreaturesInRoom for a fly, roughly: an open
        // tile clear of the terrain, high over the floor
        bool open_air_tile(sys::game_state& state, dcon::room_id room, rw::rng& rng, ivec2& out) {
            auto const view = world::get_room_view(state, room);
            float best = -1.f;
            for (int32_t i = 0; i < 40; ++i) {
                ivec2 const t{rng.range(0, view.width), rng.range(0, view.height)};
                if (view.get_tile(t).t != world::terrain::air || any_water(view, t))
                    continue;
                float const s = float(pathing::ai_terrain_proximity(state, room, t)) * 0.2f + float(std::min(pathing::floor_altitude(view, t), 8)) * 0.1f
                    + rng.value() * 0.1f;
                if (s > best) {
                    best = s;
                    out = t;
                }
            }
            return best >= 0.f;
        }

        // into its room's hive at once (serial, or within a transaction)
        void put_in_hive(sys::game_state& state, dcon::creature_id c) {
            auto& w = state.world;
            auto const room = creature::room_of(state, c);
            w.creature_set_away(c, uint8_t(dens::where::den));
            w.creature_set_away_room(c, room);
            w.creature_set_remain_in_den(c, -1);
            w.physical_object_set_room_from_object_location(body(state, c), dcon::room_id{});
            pl(state, c).in_hive = 1;
        }

        // FliesWorldAI.AddFlyToSwarmRoom (serial): into a hive of the room
        // (FliesRoomAI.CreateFlyInHive), out in the room without one
        dcon::creature_id add_fly_to_swarm_room(sys::game_state& state, dcon::room_id room, rw::rng& rng) {
            ivec2 t;
            if (random_hive_tile(state, room, rng, t)) {
                auto const c = create(state, room, room_view::middle_of_tile(t));
                put_in_hive(state, c);
                return c;
            }
            if (!open_air_tile(state, room, rng, t))
                return dcon::creature_id{};
            return create(state, room, room_view::middle_of_tile(t));
        }

        // FliesRoomAI.FlyEmergeFromHive: the hive to come out of (-1 none)
        int32_t emerge_hive(sys::game_state& state, room_view const& room, dcon::creature_id fly, rw::rng& rng, std::vector<dcon::bat_hive_id> const& hives) {
            auto const& w = state.world;
            if (!room_not_a_cycle_hazard(state, room.id) && (rain_approaching(state, room.id) < 0.3f || rain_game_over(state, room.id)))
                return -1;
            auto const& info = info_of(state, room.id);
            float best = 0.f;
            int32_t result = -1;
            for (size_t i = 0; i < hives.size(); ++i) {
                float num = rng.value() * 50.f;
                auto const tiles = w.bat_hive_get_tiles(hives[i]);
                if (tiles.size() == 0)
                    continue;
                auto const f = pathing::field_to(state, room.id, tiles[tiles.size() / 2], movement);
                for (auto const c : info.creatures) {
                    if (!w.creature_is_valid(c) || creature::static_relationship(state, fly, c).type != creature::rel::afraid)
                        continue;
                    uint16_t const d = pathing::distance(state, f, tile_of(creature::main_pos(state, c)));
                    int32_t const num4 = d == pathing::unreachable ? -1 : int32_t(d / pathing::step_cost);
                    // (Remix off: the distance itself counts too)
                    num += float(num4);
                    if (num4 >= 1 && num4 < 20)
                        num -= 1000.f;
                }
                if (num > best) {
                    best = num;
                    result = int32_t(i);
                }
            }
            return result;
        }
    }

    bool active_swarm_room(sys::game_state const& state, dcon::room_id room) noexcept {
        return room && state.world.room_get_is_swarm_room(room);
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::batfly);
        auto const d = data(state, c);
        auto rng = rw::random(state.seed, 0, uint32_t(c.value));
        physics::object_desc desc;
        desc.gravity = 0.9f;
        desc.air_friction = 0.98f;
        desc.bounce = 0.1f;
        desc.surface_friction = 0.5f;
        desc.water_friction = 0.9f;
        desc.buoyancy = 0.94f;
        auto const o = physics::create_object(state, room, desc);
        auto const k = physics::add_chunk(state, o, at, 6.f, 0.05f);
        (void)k;
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
        p = body_state{};
        p.data = keep;
        p.dir = vec2{0.f, 1.f};
        p.flap = rng.value();
        p.flap_speed = rng.value() + 0.1f;
        p.bites = start_bites;
        p.mode = mov_mode::bat_flight;
        ai_state a{};
        a.following = -1;
        a.leave_room = -1;
        a.local_goal = at;
        a.stuck_pos = at;
        auto const view = world::get_room_view(state, room);
        a.lowest_other_pos = vec2{at.x, view.pixel_height()};
        // FlyAI's constructor: fleeFromRain if the rain is close
        a.flee_from_rain = rain_approaching(state, room) < 0.3f ? 1 : 0;
        a.drop = drop_status::havent_dropped;
        new_flock(state, c, a);
        w.batfly_data_set_ai(d, a);
        graphics_state g{};
        g.wing_sync = rng.value();
        g.death_wing[0] = rng.value();
        g.death_wing[1] = rng.value();
        w.batfly_data_set_graphics(d, g);
        batfly_graphics::reset_positions(state, c);
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        creature::release_target(state, c);
        creature::erase(state, c);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        auto& p = pl(state, c);
        auto const k = chunk(state, c, 0);
        // Creature.Update + PhysicalObject.Update
        creature::stun_countdown(state, c);
        physics::update_object(state, room, body(state, c), nullptr, nullptr);
        p.last_flap_depth = p.flap_depth;
        uint32_t hg = 0;
        auto const h = creature::holder(state, c, &hg);
        bool const held_by_fly = h && is_fly(w, h);
        bool const conscious = consious(state, c);
        w.body_chunk_set_collide_with_terrain(k, uint8_t((!h || held_by_fly) && (!conscious || p.mode != mov_mode::burrow)));
        float const submersion = physics::submersion(state, room, k);
        if (!w.creature_get_dead(c)) {
            p.drown = mathf::clamp(p.drown + 0.0125f * (submersion == 1.f ? 1.f : -1.f), 0.f, 1.f);
            if (p.drown == 1.f)
                creature::die(state, c);
            // in a slugcat's hand it may die of it (not the Saint's)
            if (rng.value() < 0.00625f && h && creature::is_slugcat(w, h)
            && w.player_get_stats(creature::player_of(state, h)).name != ::player::slugcat_name::saint)
                creature::die(state, c);
        }
        if (consious(state, c)) {
            ctx x{state, c, room, info_of(state, room.id), rng, p, ai(state, c), k, flock_params{}, 0};
            x.fl();
            if (submersion == 0.f)
                act(x);
            else
                water_behavior(x, submersion);
            // into a pipe it flies onto
            ivec2 const t = tile_of(w.body_chunk_get_pos(k));
            if (w.creature_get_shortcut_delay(c) == 0 && room.inside(t.x, t.y) && room.get_tile(t).t == world::terrain::shortcut_entrance
            && room.get_shortcut_data(t).type != world::shortcut_type::dead_end && w.creature_get_away(c) == 0) {
                w.creature_set_entering_shortcut(c, t);
                if (creature::grasped_object(w, c, 0))
                    creature::release_grasp(state, c, 0);
            }
        } else {
            p.flap_depth = 1.f;
        }
        // hanging from another fly: the chain's spring
        if (auto const g = creature::grasped(w, c, 0)) {
            auto const gk = chunk(state, g, creature::grasp_chunk(w, c, 0));
            vec2& pos = w.body_chunk_get_pos(k);
            vec2& gp = w.body_chunk_get_pos(gk);
            float const num = rw::distance(pos, gp);
            if (num > 8.f) {
                vec2 const a = custom::dir_vec(pos, gp);
                pos -= a * (8.f - num) * 0.5f;
                w.body_chunk_get_vel(k) -= a * (8.f - num) * 0.5f;
                w.body_chunk_get_vel(gk) += a * (8.f - num) * 0.25f;
                auto const first = is_fly(w, g) ? first_in_chain(w, c) : c;
                auto const& fp = pl(state, first);
                if (fp.mode != mov_mode::hang || !fp.has_spot)
                    gp += a * (8.f - num) * 0.25f;
            }
        }
        if (p.eaten > 0 && --p.eaten == 0)
            state.defer(room.id, [c](sys::game_state& st) {
                if (is_fly(st.world, c))
                    creature::destroy(st, c);
            });
    }

    bool collides(sys::game_state const&, dcon::creature_id, dcon::creature_id) { return false; }

    void collide(sys::game_state&, dcon::creature_id, room_view const&, uint32_t, dcon::creature_id, uint32_t) {}

    void violence(sys::game_state& state, dcon::creature_id c, uint32_t, vec2 momentum, float damage, float stun_bonus, creature::damage_type) {
        auto& w = state.world;
        // Creature.Violence (a plain CreatureState)
        auto const k = chunk(state, c, 0);
        w.body_chunk_get_vel(k) += rw::clamp_magnitude(momentum / w.body_chunk_get_mass(k), 10.f);
        float const dmg = damage / base_damage_resistance;
        float const st = (damage * 30.f + stun_bonus) / base_stun_resistance;
        creature::stun(state, c, int32_t(st));
        if (dmg >= instant_death_damage_limit)
            creature::die(state, c);
    }

    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t) {
        if (creature::grasped(state.world, c, 0) == t)
            creature::release_grasp(state, c, 0);
    }

    void died(sys::game_state& state, dcon::creature_id c) {
        state.world.physical_object_set_surface_friction(body(state, c), 0.3f);
    }

    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const view = world::get_room_view(state, room);
        ivec2 const t = tile_of(at);
        auto const k = chunk(state, c, 0);
        pl(state, c).in_hive = 0;
        if (view.inside(t.x, t.y) && view.get_tile(t).t == world::terrain::shortcut_entrance) {
            // Fly.SpitOutOfShortCut
            w.creature_set_shortcut_delay(c, 40);
            vec2 const d = rw::to_vec2(view.shortcut_entrance_hole_direction(t));
            vec2 const p = room_view::middle_of_tile(t) - d * 5.f;
            physics::hard_set_position(state, k, p);
            w.body_chunk_set_last_pos(k, p);
            w.body_chunk_set_vel(k, d * 15.f);
        } else {
            // PlaceInRoom (out of the hive grass)
            vec2 const p = room_view::middle_of_tile(t);
            physics::hard_set_position(state, k, p);
            w.body_chunk_set_last_pos(k, p);
            w.body_chunk_set_vel(k, vec2{0.f, 0.f});
        }
        ai_new_room(state, c, view);
        batfly_graphics::reset_positions(state, c);
    }

    std::string describe(sys::game_state const& state, dcon::creature_id c) {
        static constexpr char const* names[] = {"idle", "swarm", "drop", "flee", "flee rain", "leave room", "burrow", "chain", "lure"};
        auto const& w = state.world;
        auto const& p = pl(state, c);
        uint32_t const b = w.creature_get_behavior(c);
        if (p.in_hive)
            return "batfly in the hive";
        vec2 const at = fly_pos(state, c);
        return "batfly at " + std::to_string(int(at.x)) + "," + std::to_string(int(at.y)) + " behavior " + (b < 9 ? names[b] : "?")
            + " afraid " + std::to_string(ai(state, c).afraid).substr(0, 4) + (creature::held(state, c) ? " held" : "");
    }

    bool spawn_spot(room_view const& room, ivec2 tile) {
        return room.inside(tile.x, tile.y) && room.get_tile(tile).t == world::terrain::air && room.get_tile(tile).water == 0;
    }

    //
    // Rooms and world
    //
    void spawn_room(sys::game_state& state, dcon::room_id room) {
        auto& w = state.world;
        if (has_hives(state, room))
            return;
        room_view const view = world::get_room_view(state, room);
        // Room.LoadFromDataString's order (columns left to right, each top
        // to bottom) and AddHiveTile's grouping: a tile joins the hive of
        // the first earlier tile it touches
        std::vector<ivec2> tiles;
        std::vector<int32_t> index;
        int32_t count = 0;
        for (int32_t x = 0; x < view.width; ++x)
            for (int32_t y = view.height - 1; y >= 0; --y) {
                if (!(view.get_tile(x, y).flags & world::tile_hive))
                    continue;
                ivec2 const t{x, y};
                int32_t found = -1;
                for (size_t i = 0; i < tiles.size() && found == -1; ++i)
                    for (auto const d : custom::eight_directions)
                        if (t + d == tiles[i]) {
                            found = index[i];
                            break;
                        }
                if (found == -1)
                    found = count++;
                tiles.push_back(t);
                index.push_back(found);
            }
        for (int32_t h = 0; h < count; ++h) {
            std::vector<ivec2> mine;
            for (size_t i = 0; i < tiles.size(); ++i)
                if (index[i] == h)
                    mine.push_back(tiles[i]);
            auto const hive = w.create_bat_hive();
            w.try_create_bat_hive_location(hive, room);
            w.bat_hive_get_tiles(hive).load_range(mine.data(), mine.data() + mine.size());
        }
    }

    uint32_t spawn_world(sys::game_state& state) {
        auto& w = state.world;
        uint32_t made = 0;
        std::vector<dcon::room_id> swarms;
        w.for_each_room([&](dcon::room_id r) {
            if (w.room_get_is_swarm_room(r))
                swarms.push_back(r);
        });
        for (auto const r : swarms) {
            dens::build_route(state, r);
            w.room_set_swarm_behavior(r, uint8_t(swarm_behavior::spread));
            auto rng = rw::random(state.seed, 0, uint32_t(r.value));
            // FliesWorldAI's migrationTargets: a random swarm room of the region
            auto const& list = swarm_rooms(state, w.room_get_region_from_region_membership(r));
            w.room_set_swarm_migration_target(r, list.empty() ? r : list[size_t(rng.range(0, int32_t(list.size())))]);
            auto const region = w.room_get_region_from_region_membership(r);
            uint32_t const n = region ? w.region_get_bats_per_active_swarm_room(region) : 10u;
            for (uint32_t i = 0; i < n; ++i)
                if (add_fly_to_swarm_room(state, r, rng))
                    ++made;
        }
        return made;
    }

    void update_room(sys::game_state& state, room_view const& room) {
        auto& w = state.world;
        // the room's flies in the hive grass
        std::vector<dcon::creature_id> in_hive;
        auto const take = [&](dcon::creature_id c) {
            if (w.creature_get_type(c) == creature::type::batfly && w.creature_get_away(c) == uint8_t(dens::where::den) && w.creature_get_away_room(c) == room.id
            && pl(state, c).in_hive)
                in_hive.push_back(c);
        };
        if (state.index.valid) {
            for (auto const c : state.index.away.in(room.id))
                take(c);
        } else {
            if (!has_hives(state, room.id))
                return;
            w.for_each_creature(take);
        }
        if (in_hive.empty())
            return;
        std::vector<dcon::bat_hive_id> hives;
        w.room_for_each_bat_hive_location(room.id, [&](dcon::bat_hive_location_id l) { hives.push_back(w.bat_hive_location_get_hive(l)); });
        for (auto const c : in_hive) {
            auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
            // (dead in the hive: it stays there)
            if (w.creature_get_dead(c) || rng.value() >= 0.025f || hives.empty())
                continue;
            int32_t const hv = emerge_hive(state, room, c, rng, hives);
            if (hv < 0)
                continue;
            auto const tiles = w.bat_hive_get_tiles(hives[size_t(hv)]);
            if (tiles.size() == 0)
                continue;
            ivec2 const t = tiles[size_t(rng.range(0, int32_t(tiles.size())))];
            state.play_sound(sound_id::bat_emerge_from_grass, room.id, room_view::middle_of_tile(t));
            state.defer(room.id, [c, r = room.id, t](sys::game_state& st) {
                auto& ww = st.world;
                if (!is_fly(ww, c) || ww.creature_get_away(c) != uint8_t(dens::where::den) || ww.creature_get_away_room(c) != r)
                    return;
                ww.physical_object_set_room_from_object_location(body(st, c), r);
                ww.creature_set_away(c, uint8_t(dens::where::room));
                ww.creature_set_away_room(c, dcon::room_id{});
                ww.creature_set_remain_in_den(c, -1);
                ww.creature_set_entering_shortcut(c, ivec2{-1, -1});
                ww.creature_set_shortcut_delay(c, 0);
                creature::placed_in_room(st, c, r, room_view::middle_of_tile(t));
            });
        }
    }

    void update_world(sys::game_state& state) {
        auto& w = state.world;
        w.for_each_region([&](dcon::region_id region) {
            auto const& swarms = swarm_rooms(state, region);
            if (swarms.empty())
                return;
            auto rng = rw::random(state.seed, state.tick, uint32_t(region.value));
            // MoveFly: a fly of a random room of the region sent on its way
            auto const range = w.region_range_of_region_membership(region);
            int32_t const nrooms = int32_t(range.second - range.first);
            if (nrooms > 0) {
                auto const from = w.region_membership_get_room(*(range.first + rng.range(0, nrooms)));
                if (auto const to = migration_direction(state, from, rng)) {
                    std::vector<dcon::creature_id> flies;
                    for_each_fly_in(state, from, [&](dcon::creature_id c) {
                        if (is_fly(w, c) && w.creature_get_away(c) == 0 && creature::room_of(state, c) == from)
                            flies.push_back(c);
                    });
                    if (!flies.empty()) {
                        auto const c = flies[size_t(rng.range(0, int32_t(flies.size())))];
                        // FlyAI.LeaveRoom: the exit leading there
                        std::vector<node> nodes;
                        uint32_t exits = 0;
                        room_nodes(state, from, nodes, exits);
                        for (uint32_t i = 0; i < exits; ++i)
                            if (nodes[i].target == to) {
                                ai(state, c).leave_room = int8_t(i);
                                break;
                            }
                    }
                }
            }
            // the swarm rooms' behaviours: the most crowded spreads, the
            // second migrates, the rest gather
            if (rng.value() < 0.0025f) {
                for (auto const s : swarms)
                    w.room_set_swarm_behavior(s, uint8_t(swarm_behavior::gather));
                dcon::room_id skip;
                for (int32_t j = 0; j < 2; ++j) {
                    int32_t most = 0;
                    dcon::room_id pick;
                    for (auto const s : swarms) {
                        int32_t const n = flies_in(state, s);
                        if (s != skip && n > most) {
                            most = n;
                            pick = s;
                        }
                    }
                    skip = pick;
                    if (!pick)
                        continue;
                    if (j == 0) {
                        w.room_set_swarm_behavior(pick, uint8_t(swarm_behavior::spread));
                    } else {
                        w.room_set_swarm_behavior(pick, uint8_t(swarm_behavior::migrate));
                        w.room_set_swarm_migration_target(pick, swarms[size_t(rng.range(0, int32_t(swarms.size())))]);
                    }
                }
            }
            // not vanilla (its world is made anew every cycle, with its
            // flies): now and then a swarm room short of its flies gets one
            // in its hive, the region's flies capped at what it started with
            if (rng.value() < 1.f / 800.f) {
                auto const s = swarms[size_t(rng.range(0, int32_t(swarms.size())))];
                if (time_until_rain(state, s) <= 800)
                    return;
                int32_t const per = int32_t(w.region_get_bats_per_active_swarm_room(region));
                int32_t in_room = 0, in_region = 0;
                w.for_each_creature([&](dcon::creature_id c) {
                    if (w.creature_get_type(c) != creature::type::batfly || w.creature_get_dead(c))
                        return;
                    auto const r = w.creature_get_away(c) != 0 ? w.creature_get_away_room(c) : creature::room_of(state, c);
                    if (!r)
                        return;
                    if (r == s)
                        ++in_room;
                    if (w.room_get_region_from_region_membership(r) == region)
                        ++in_region;
                });
                if (in_room < per && in_region < per * int32_t(swarms.size()))
                    add_fly_to_swarm_room(state, s, rng);
            }
        });
    }

    //
    // The slugcat side
    //
    dcon::creature_id held_in_hand(sys::game_state const& state, dcon::player_id p, uint32_t hand) noexcept {
        if (!p || hand > 1)
            return dcon::creature_id{};
        auto const c = creature::grasped(state.world, player::creature_of(state, p), hand);
        return c && is_fly(state.world, c) ? c : dcon::creature_id{};
    }

    dcon::creature_id held_by(sys::game_state const& state, dcon::player_id p) noexcept {
        if (auto const c = held_in_hand(state, p, 0))
            return c;
        return held_in_hand(state, p, 1);
    }

    namespace {
        // Fly.PlayerAutoGrabable
        bool player_auto_grabable(sys::game_state const& state, dcon::creature_id c) {
            auto const& w = state.world;
            if (!consious(state, c) || w.creature_get_shortcut_delay(c) >= 1)
                return false;
            auto const h = creature::holder(state, c);
            return !h || is_fly(w, h);
        }
    }

    dcon::creature_id auto_grab_candidate(sys::game_state const& state, dcon::player_id p, room_view const& room) {
        auto const& w = state.world;
        vec2 const p0 = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
        float const num9 = active_swarm_room(state, room.id) ? 1.f : 0.5f;
        dcon::creature_id result;
        rooms::for_each_creature(state, room.id, creature::type::batfly, [&](dcon::creature_id c) {
            if (result || w.creature_get_away(c) != 0 || !player_auto_grabable(state, c) || creature::held(state, c))
                return;
            vec2 const fp = fly_pos(state, c);
            if (custom::dist_less(p0, fp, (pathing::narrow_space(room, tile_of(fp)) ? 10.f : 20.f) * num9))
                result = c;
        });
        return result;
    }

    dcon::creature_id pickup_candidate(sys::game_state const& state, dcon::player_id p, room_view const& room, float& score) {
        auto const& w = state.world;
        score = float_max;
        vec2 const p0 = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
        int32_t const flip = w.player_get_flip_direction(p);
        dcon::creature_id result;
        auto const pc = player::creature_of(state, p);
        rooms::for_each_creature(state, room.id, creature::type::batfly, [&](dcon::creature_id c) {
            // Player.Grabability: OneHand, CantGrab while its shortcutDelay runs
            if (w.creature_get_away(c) != 0 || creature::holds(w, pc, c) || w.creature_get_shortcut_delay(c) != 0)
                return;
            auto const k = chunk(state, c, 0);
            vec2 const cp = w.body_chunk_get_pos(k);
            float const rad = w.body_chunk_get_rad(k);
            if (!custom::dist_less(p0, cp, rad + 40.f))
                return;
            if (!custom::dist_less(p0, cp, rad + 20.f) && !room.visual_contact(p0, cp))
                return;
            float dist = rw::distance(p0, cp);
            if ((cp.x < p0.x) == (flip < 0))
                dist -= 10.f;
            if (dist < score) {
                result = c;
                score = dist;
            }
        });
        return result;
    }

    void grab(sys::game_state& state, dcon::player_id p, dcon::creature_id c, uint32_t hand) {
        auto const& w = state.world;
        state.play_sound(w.creature_get_dead(c) ? sound_id::fly_caught_dead : sound_id::fly_caught, creature::room_of(state, c), fly_pos(state, c));
        creature::grab(state, player::creature_of(state, p), hand, c, 0);
    }

    void release(sys::game_state& state, dcon::creature_id c) {
        auto const room = creature::room_of(state, c);
        dcon::creature_id holder;
        uint32_t grasp = 0;
        creature::for_each_holder(state, room, c, [&](dcon::creature_id h, uint32_t g) {
            if (!holder && creature::is_slugcat(state.world, h)) {
                holder = h;
                grasp = g;
            }
        });
        if (holder)
            creature::release_grasp(state, holder, grasp);
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
            // (a chain hanging from it lets go: the others stay behind)
            if (creature::grasped_object(w, c, 0))
                creature::release_grasp(state, c, 0);
            w.physical_object_set_room_from_object_location(body(state, c), room);
            if (!spit_out)
                continue;
            vec2 const at = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
            auto const k = chunk(state, c, 0);
            physics::hard_set_position(state, k, at);
            w.body_chunk_set_vel(k, w.body_chunk_get_vel(player::body_chunk(state, p, 0)));
            ai_new_room(state, c, world::get_room_view(state, room));
            ai(state, c).threat_count = 0;
            batfly_graphics::reset_positions(state, c);
        }
    }

    bool bit_by_player(sys::game_state& state, dcon::creature_id c, dcon::player_id p) {
        auto& w = state.world;
        auto& b = pl(state, c);
        if (b.bites > 0)
            --b.bites;
        if (!w.creature_get_dead(c))
            creature::die(state, c);
        state.play_sound(b.bites == 0 ? sound_id::slugcat_final_bite_fly : sound_id::slugcat_bite_fly, creature::room_of(state, c), fly_pos(state, c));
        // MoveFromOutsideMyUpdate: to the slugcat's mouth
        vec2 const mouth = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
        auto const k = chunk(state, c, 0);
        w.body_chunk_set_pos(k, mouth);
        w.body_chunk_set_last_pos(k, mouth);
        if (b.bites < 1 && b.eaten == 0) {
            b.eaten = 3;
            return true;
        }
        return false;
    }

    int32_t nourishment(::player::slugcat_name name) noexcept {
        using ::player::slugcat_name;
        switch (name) {
        case slugcat_name::saint: return -1;
        case slugcat_name::red:
        case slugcat_name::artificer: return food_points;
        case slugcat_name::spear: return 0;
        default: return 4 * food_points;
        }
    }
}
