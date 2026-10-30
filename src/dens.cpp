#include <algorithm>
#include <deque>
#include <limits>

#include "dens.hpp"
#include "centipede.hpp"
#include "cicada.hpp"
#include "creature.hpp"
#include "daddy.hpp"
#include "items.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "rain.hpp"
#include "rooms.hpp"
#include "shortcuts.hpp"

namespace dens {
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    using world::shortcut_type;
    namespace custom = rw::custom;

    namespace {
        // the room's pipe of `type` leading to its node `node`
        dcon::shortcut_id shortcut_to_node(sys::game_state const& state, dcon::room_id room, shortcut_type type, uint8_t node) {
            auto const& w = state.world;
            dcon::shortcut_id found;
            w.room_for_each_shortcut_location(room, [&](dcon::shortcut_location_id sl) {
                auto const s = w.shortcut_location_get_shortcut(sl);
                if (found || w.shortcut_get_type(s) != type)
                    return;
                auto const e = w.shortcut_get_exit_from_shortcut_destination(s);
                if (e && w.room_exit_get_index(e) == node)
                    found = s;
            });
            return found;
        }

        dcon::shortcut_id shortcut_at(sys::game_state const& state, dcon::room_id room, ivec2 tile) {
            auto const& w = state.world;
            dcon::shortcut_id found;
            w.room_for_each_shortcut_location(room, [&](dcon::shortcut_location_id sl) {
                auto const s = w.shortcut_location_get_shortcut(sl);
                if (!found && w.shortcut_get_start(s) == tile)
                    found = s;
            });
            return found;
        }

        // a slugcat holds it (a squidcada, a baby centipede)
        bool held(sys::game_state const& state, dcon::creature_id c, dcon::room_id room) {
            bool h = false;
            rooms::for_each_player(state, room, [&](dcon::player_id p) {
                h = h || cicada::held_by(state, p) == c || centipede::held_in_hand(state, p, 0) == c || centipede::held_in_hand(state, p, 1) == c;
            });
            return h;
        }

        // the slugcat it carries into a pipe is done for (IsEnteringDen: the
        // prey dies; it is carried off, eaten)
        void take_prey(sys::game_state& state, dcon::creature_id c, dcon::room_id room) {
            auto& w = state.world;
            for (uint32_t g = 0; g < 2; ++g) {
                auto const p = creature::player_of(state, w.creature_get_grasp_player(c)[g]);
                if (!p || rooms::room_of(state, p) != room)
                    continue;
                items::release_all(state, p);
                rooms::for_each_creature(state, room, [&](dcon::creature_id o) { creature::release_player(state, o, p); });
                if (!w.player_get_dead(p))
                    player::die(state, p);
                w.player_set_eaten(p, 1);
                ++stats().prey_taken;
            }
        }

        // Creature.SpitOutOfShortCut: out of the pipe at `tile` of `room`
        void spit_out(sys::game_state& state, dcon::creature_id c, dcon::room_id room, ivec2 tile) {
            auto& w = state.world;
            auto const view = world::get_room_view(state, room);
            auto const o = creature::body(state, c);
            w.physical_object_set_room_from_object_location(o, room);
            vec2 const dir = rw::to_vec2(view.shortcut_entrance_hole_direction(tile));
            vec2 const at = room_view::middle_of_tile(tile);
            auto& rng = w.creature_get_random(c);
            uint32_t const n = creature::chunk_count(state, c);
            for (uint32_t i = 0; i < n; ++i) {
                auto const k = creature::chunk(state, c, i);
                physics::hard_set_position(state, k, at + custom::rnv(rng));
                w.body_chunk_set_vel(k, dir * 2.f);
            }
            w.creature_set_away(c, uint8_t(where::room));
            w.creature_set_away_room(c, dcon::room_id{});
            w.creature_set_entering_shortcut(c, ivec2{-1, -1});
            w.creature_set_shortcut_delay(c, 20);
            w.creature_set_vessel_shortcut(c, dcon::shortcut_id{});
            w.creature_set_destination(c, tile);
            creature::placed_in_room(state, c, room, at);
        }

        // the creature's way out of the den: its den pipe from the den end
        void leave_den(sys::game_state& state, dcon::creature_id c, dcon::room_id room) {
            auto& w = state.world;
            auto const s = shortcut_to_node(state, room, shortcut_type::creature_hole, w.creature_get_home_den(c));
            if (!s)
                return;
            w.creature_set_away(c, uint8_t(where::shortcut));
            w.creature_set_vessel_shortcut(c, s);
            w.creature_set_vessel_reverse(c, 1);
            w.creature_set_vessel_progress(c, 0.f);
            w.creature_set_remain_in_den(c, -1);
            ++stats().out_of_dens;
        }

        // the closest den of the room to `from` (DenFinder for a creature
        // without a home), none if the room has no den
        dcon::shortcut_id closest_den(sys::game_state const& state, dcon::room_id room, vec2 from) {
            auto const& w = state.world;
            dcon::shortcut_id best;
            float best_d = std::numeric_limits<float>::max();
            w.room_for_each_shortcut_location(room, [&](dcon::shortcut_location_id sl) {
                auto const s = w.shortcut_location_get_shortcut(sl);
                if (w.shortcut_get_type(s) != shortcut_type::creature_hole || !w.shortcut_get_exit_from_shortcut_destination(s))
                    return;
                float const d = rw::distance(room_view::middle_of_tile(w.shortcut_get_start(s)), from);
                if (d < best_d) {
                    best_d = d;
                    best = s;
                }
            });
            return best;
        }

        // the room an exit of `room` leads to, if the pipe can be travelled
        // both ways (the target's exit back, its pipe) and isn't a gate or
        // a shelter (creatures keep out of those)
        dcon::room_id exit_target(sys::game_state const& state, dcon::room_exit_id e, dcon::room_id from) {
            auto const& w = state.world;
            auto const target = w.room_exit_get_target_from_exit_connection(e);
            if (!target || w.room_get_is_gate(target) || w.room_get_is_shelter(target))
                return dcon::room_id{};
            bool back = false;
            w.room_for_each_exit_location(target, [&](dcon::exit_location_id el) {
                back = back || w.room_exit_get_target_from_exit_connection(w.exit_location_get_exit(el)) == from;
            });
            return back ? target : dcon::room_id{};
        }

        // the away creatures of a room
        template<typename F>
        void for_each_away(sys::game_state const& state, dcon::room_id room, F&& f) {
            if (state.index.valid) {
                for (auto const c : state.index.away.in(room))
                    f(c);
                return;
            }
            state.world.for_each_creature([&](dcon::creature_id c) {
                if (state.world.creature_get_away(c) != 0 && state.world.creature_get_away_room(c) == room)
                    f(c);
            });
        }
    }

    counters& stats() noexcept {
        static counters c;
        return c;
    }

    void build_route(sys::game_state& state, dcon::room_id home) {
        auto& w = state.world;
        uint32_t const key = uint32_t(home.index());
        if (state.routes.contains(key))
            return;
        // breadth first from home over the exits backwards: a room's next
        // step is the exit into a room one step closer
        std::vector<int16_t> next(size_t(w.room_size()), int16_t(-1));
        std::vector<uint8_t> seen(size_t(w.room_size()), 0);
        // the rooms leading into each room
        std::vector<std::vector<std::pair<dcon::room_id, uint8_t>>> into(size_t(w.room_size()));
        w.for_each_room_exit([&](dcon::room_exit_id e) {
            auto const from = w.room_exit_get_room_from_exit_location(e);
            if (!from || w.room_get_is_gate(from) || w.room_get_is_shelter(from))
                return;
            if (auto const to = exit_target(state, e, from); to && shortcut_to_node(state, from, shortcut_type::room_exit, w.room_exit_get_index(e)))
                into[size_t(to.index())].emplace_back(from, w.room_exit_get_index(e));
        });
        std::deque<dcon::room_id> queue{home};
        seen[size_t(home.index())] = 1;
        while (!queue.empty()) {
            auto const r = queue.front();
            queue.pop_front();
            for (auto const& [from, node] : into[size_t(r.index())]) {
                if (seen[size_t(from.index())])
                    continue;
                seen[size_t(from.index())] = 1;
                next[size_t(from.index())] = int16_t(node);
                queue.push_back(from);
            }
        }
        state.routes.emplace(key, std::move(next));
    }

    void set_home(sys::game_state& state, dcon::creature_id c, dcon::room_id room, uint8_t den) {
        auto& w = state.world;
        w.creature_set_home_room(c, room);
        w.creature_set_home_den(c, den);
        if (room)
            build_route(state, room);
    }

    std::optional<ivec2> den_entrance(sys::game_state const& state, dcon::room_id room, uint8_t den) {
        auto const s = shortcut_to_node(state, room, shortcut_type::creature_hole, den);
        return s ? std::optional<ivec2>{state.world.shortcut_get_start(s)} : std::nullopt;
    }

    void put_in_den(sys::game_state& state, dcon::creature_id c, int32_t remain) {
        auto& w = state.world;
        auto const room = creature::room_of(state, c);
        w.creature_set_away(c, uint8_t(where::den));
        w.creature_set_away_room(c, room);
        w.creature_set_remain_in_den(c, remain);
        w.physical_object_set_room_from_object_location(creature::body(state, c), dcon::room_id{});
    }

    float rain_utility(sys::game_state const& state, dcon::creature_id c, dcon::room_id room) noexcept {
        if (state.world.creature_get_spawn_flags(c) & (flag_night | flag_ignore_cycle))
            return 0.f;
        return rain::tracker_utility(state, room);
    }

    bool want_to_stay(sys::game_state const& state, dcon::creature_id c, dcon::room_id room) {
        auto const& w = state.world;
        uint8_t const f = w.creature_get_spawn_flags(c);
        if (w.creature_get_dead(c) || w.creature_get_health(c) < 0.6f)
            return true;
        if ((f & (flag_night | flag_ignore_cycle)) == 0 && rain::stay_in_den(state, room))
            return true;
        // (no precycles: a PreCycle creature never comes out)
        return (f & flag_pre_cycle) != 0;
    }

    std::optional<ivec2> home_tile(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        // (a leech's FleeFromRainGoal only knows the dens of the room it is in)
        auto const home = w.creature_get_type(c) == creature::type::leech ? dcon::room_id{} : w.creature_get_home_room(c);
        std::optional<ivec2> tile;
        if (!home || home == room.id) {
            dcon::shortcut_id s = home ? shortcut_to_node(state, room.id, shortcut_type::creature_hole, w.creature_get_home_den(c)) : dcon::shortcut_id{};
            if (!s)
                s = closest_den(state, room.id, w.body_chunk_get_pos(creature::chunk(state, c, creature::main_chunk(state, c))));
            if (s)
                tile = w.shortcut_get_start(s);
        } else if (auto const it = state.routes.find(uint32_t(home.index())); it != state.routes.end()) {
            int16_t const node = it->second[size_t(room.id.index())];
            if (node >= 0)
                if (auto const s = shortcut_to_node(state, room.id, shortcut_type::room_exit, uint8_t(node)))
                    tile = w.shortcut_get_start(s);
        }
        if (!tile)
            return std::nullopt;
        w.creature_set_going_home(c, 1);
        w.creature_set_travel_tile(c, *tile);
        // the AI heads for the open tile in front of the entrance (the
        // entrance itself is in the rock)
        return *tile + room.shortcut_entrance_hole_direction(*tile);
    }

    bool before_update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        w.creature_set_going_home(c, 0);
        if (w.creature_get_shortcut_delay(c) > 0)
            w.creature_get_shortcut_delay(c) -= 1;
        ivec2 const entering = w.creature_get_entering_shortcut(c);
        if (entering.x < 0)
            return false;
        // Creature.Update while enteringShortCut: every chunk pulled into the hole
        float& rad_add = w.creature_get_shortcut_rad_add(c);
        rad_add += 0.2f;
        creature::stun_countdown(state, c);
        vec2 const target = room_view::middle_of_tile(entering) + rw::to_vec2(room.shortcut_entrance_hole_direction(entering)) * -5.f;
        uint32_t outside = 0;
        uint32_t const n = creature::chunk_count(state, c);
        for (uint32_t i = 0; i < n; ++i) {
            auto const k = creature::chunk(state, c, i);
            vec2& pos = w.body_chunk_get_pos(k);
            w.body_chunk_get_vel(k) *= 0.06f;
            if (!custom::dist_less(pos, target, std::max(10.f, rad_add))) {
                w.body_chunk_set_last_pos(k, pos);
                pos += custom::dir_vec(pos, target) * 4.5f;
                ++outside;
            } else {
                pos = target;
                w.body_chunk_set_last_pos(k, target);
            }
        }
        if (!creature::consious(state, c)) {
            // stunned: it falls back out
            w.creature_set_entering_shortcut(c, ivec2{-1, -1});
            return true;
        }
        if (outside == 0) {
            // SuckedIntoShortCut: a vessel at the entrance, the body out of the room
            auto const s = shortcut_at(state, room.id, entering);
            w.creature_set_entering_shortcut(c, ivec2{-1, -1});
            rad_add = 0.f;
            if (!s)
                return true;
            dcon::room_id const in = room.id;
            ++stats().into_pipes;
            state.defer(room.id, [c, s, in](sys::game_state& st) {
                auto& ww = st.world;
                if (!ww.creature_is_valid(c))
                    return;
                ww.creature_set_away(c, uint8_t(where::shortcut));
                ww.creature_set_away_room(c, in);
                ww.creature_set_vessel_shortcut(c, s);
                ww.creature_set_vessel_reverse(c, 0);
                ww.creature_set_vessel_progress(c, 0.f);
                ww.physical_object_set_room_from_object_location(creature::body(st, c), dcon::room_id{});
            });
        }
        return true;
    }

    void after_update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        if (!w.creature_is_valid(c) || !w.creature_get_going_home(c) || !creature::consious(state, c) || w.creature_get_shortcut_delay(c) > 0)
            return;
        // (vanilla's path leads into the pipe: here a chunk within a couple
        // of tiles of the entrance or the tile before it, in sight of it,
        // is pulled in: the AIs stop a tile or two short of their goal)
        ivec2 const tile = w.creature_get_travel_tile(c);
        if (room.get_tile(tile).t != world::terrain::shortcut_entrance)
            return;
        vec2 const hole = room_view::middle_of_tile(tile);
        vec2 const front = room_view::middle_of_tile(tile + room.shortcut_entrance_hole_direction(tile));
        bool close = false;
        uint32_t const n = creature::chunk_count(state, c);
        for (uint32_t i = 0; i < n && !close; ++i) {
            vec2 const p = w.body_chunk_get_pos(creature::chunk(state, c, i));
            close = (custom::dist_less(p, hole, 50.f) || custom::dist_less(p, front, 50.f)) && room.visual_contact(p, front);
        }
        if (!close)
            return;
        if (held(state, c, room.id))
            return;
        take_prey(state, c, room.id);
        creature::release_all(state, c, room.id);
        w.creature_set_entering_shortcut(c, tile);
        w.creature_set_shortcut_rad_add(c, 0.f);
    }

    void update_room(sys::game_state& state, room_view const& room) {
        auto& w = state.world;
        for_each_away(state, room.id, [&](dcon::creature_id c) {
            if (w.creature_get_away(c) == uint8_t(where::den)) {
                // AbstractCreature.InDenUpdate
                int32_t& remain = w.creature_get_remain_in_den(c);
                uint8_t const f = w.creature_get_spawn_flags(c);
                auto const* rc = rain::of_room(state, room.id);
                bool const night_ok = (f & flag_night) == 0 || (rc && rc->day_night_counter > 600) || (f & flag_ignore_cycle) != 0;
                if (remain <= -1 || !night_ok || (f & flag_pre_cycle) != 0)
                    return;
                if (want_to_stay(state, c, room.id)) {
                    remain = -1;
                    return;
                }
                if (--remain < 0)
                    leave_den(state, c, room.id);
                return;
            }
            // a vessel along its pipe (ShortcutHandler: a tile every third tick)
            auto const s = w.creature_get_vessel_shortcut(c);
            if (!s) {
                // (lost its pipe: back out where it is)
                state.defer(room.id, [c, r = room.id](sys::game_state& st) {
                    if (st.world.creature_is_valid(c))
                        spit_out(st, c, r, room_view::get_tile_position(st.world.body_chunk_get_pos(creature::chunk(st, c, 0))));
                });
                w.creature_set_away(c, uint8_t(where::room));
                return;
            }
            auto const path = w.shortcut_get_path(s);
            float const len = float(path.size());
            float& progress = w.creature_get_vessel_progress(c);
            progress += shortcuts::vessel_speed;
            if (progress < len - 1.f)
                return;
            progress = len - 1.f;
            ivec2 const start = w.shortcut_get_start(s);
            if (w.creature_get_vessel_reverse(c) || len < 2.f) {
                // back out of the pipe's entrance
                w.creature_set_vessel_reverse(c, 0);
                state.defer(room.id, [c, r = room.id, start](sys::game_state& st) {
                    if (st.world.creature_is_valid(c))
                        spit_out(st, c, r, start);
                });
                return;
            }
            auto const type = w.shortcut_get_type(s);
            auto const bounce = [&] {
                w.creature_set_vessel_reverse(c, 1);
                progress = 0.f;
            };
            if (type == shortcut_type::normal) {
                ivec2 const end = path[uint32_t(len) - 1];
                w.creature_set_vessel_reverse(c, 0);
                state.defer(room.id, [c, r = room.id, end](sys::game_state& st) {
                    if (st.world.creature_is_valid(c))
                        spit_out(st, c, r, end);
                });
                return;
            }
            if (type == shortcut_type::creature_hole) {
                // AbstractCreature.OpportunityToEnterDen
                bool const prey = w.creature_get_grasp_player(c)[0] != 0 || w.creature_get_grasp_player(c)[1] != 0;
                if (want_to_stay(state, c, room.id) || rain::tracker_utility(state, room.id) > 0.01f) {
                    w.creature_set_remain_in_den(c, -1);
                } else if (prey || w.creature_get_remain_in_den(c) > 0) {
                    w.creature_set_remain_in_den(c, std::max(w.creature_get_remain_in_den(c), 200));
                } else {
                    bounce();
                    return;
                }
                w.creature_set_away(c, uint8_t(where::den));
                w.creature_set_grasp_player(c, world::byte_pair{});
                ++stats().into_dens;
                // it lives here now if it had no den
                if (!w.creature_get_home_room(c) || w.creature_get_home_room(c) == room.id) {
                    if (auto const e = w.shortcut_get_exit_from_shortcut_destination(s)) {
                        w.creature_set_home_room(c, room.id);
                        w.creature_set_home_den(c, w.room_exit_get_index(e));
                    }
                }
                return;
            }
            if (type != shortcut_type::room_exit) {
                bounce();
                return;
            }
            // a room exit: the connected room's pipe back here, travelled
            // backwards (moving rooms: a transaction)
            auto const exit = w.shortcut_get_exit_from_shortcut_destination(s);
            auto const target = exit ? exit_target(state, exit, room.id) : dcon::room_id{};
            dcon::shortcut_id arrival;
            if (target) {
                dcon::room_exit_id back;
                w.room_for_each_exit_location(target, [&](dcon::exit_location_id el) {
                    auto const e = w.exit_location_get_exit(el);
                    if (!back && w.room_exit_get_target_from_exit_connection(e) == room.id)
                        back = e;
                });
                if (back)
                    arrival = shortcut_to_node(state, target, shortcut_type::room_exit, w.room_exit_get_index(back));
            }
            if (!arrival) {
                bounce();
                return;
            }
            ++stats().room_changes;
            state.defer(room.id, [c, target, arrival](sys::game_state& st) {
                auto& ww = st.world;
                if (!ww.creature_is_valid(c))
                    return;
                ww.creature_set_away_room(c, target);
                ww.creature_set_vessel_shortcut(c, arrival);
                ww.creature_set_vessel_reverse(c, 1);
                ww.creature_set_vessel_progress(c, 0.f);
            });
        });
    }

    void cycle_restarted(sys::game_state& state, dcon::region_id leader) {
        auto& w = state.world;
        w.for_each_creature([&](dcon::creature_id c) {
            if (w.creature_get_away(c) != uint8_t(where::den) || w.creature_get_remain_in_den(c) > -1)
                return;
            auto const room = w.creature_get_away_room(c);
            auto const r = room ? w.room_get_rain_region(room) : dcon::region_id{};
            if (!r || (w.region_get_rain_leader(r) ? w.region_get_rain_leader(r) : r) != leader)
                return;
            // a new cycle's spawn: out in 100 to 1000 ticks
            if (!w.creature_get_dead(c))
                w.creature_set_remain_in_den(c, w.creature_get_random(c).range(100, 1000));
        });
    }
}
