#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "offscreen.hpp"
#include "borders.hpp"
#include "dens.hpp"
#include "migrate.hpp"
#include "miros.hpp"
#include "physics.hpp"
#include "rooms.hpp"
#include "scavenger.hpp"
#include "spawns.hpp"
#include "text.hpp"
#include "vulture.hpp"

namespace offscreen {
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    using world::shortcut_type;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // how each kind lives off screen
        enum class role : uint8_t { none, vulture, miros, scavenger };

        role role_of(creature::type t) noexcept {
            switch (t) {
            case creature::type::vulture: return role::vulture;
            case creature::type::miros: return role::miros;
            case creature::type::scavenger: return role::scavenger;
            default: return role::none;
            }
        }
        role role_of(sys::game_state const& state, dcon::creature_id c) noexcept { return role_of(state.world.creature_get_type(c)); }

        state& st(sys::game_state& s, dcon::creature_id c) { return s.world.creature_get_offscreen(c); }
        state const& st(sys::game_state const& s, dcon::creature_id c) { return s.world.creature_get_offscreen(c); }

        // CreatureTemplate.offScreenSpeed
        float speed(role r) noexcept {
            switch (r) {
            case role::vulture: return 0.8f;
            case role::miros: return 1.5f;
            default: return 0.75f;
            }
        }
        // the highway between the den and a room: a stand-in distance (no
        // world map here) at the template's speed
        int32_t travel_ticks(role r, rw::rng& rng) { return int32_t(float(rng.range(100, 500)) / speed(r)); }

        //
        // The tables kept in state.routes (dens.hpp's room to room routes,
        // keyed by a home room's index; these by region and table, with the
        // top bit set): per room of the world, the exit node towards the
        // closest room of the region with a way off screen for the kind
        // (-2: this room has one, -1: none); and the miros birds' allowed
        // side exits (pairs of room index and border exit number)
        //
        enum table : uint32_t { sky_rooms, side_rooms, transport_rooms, miros_nodes };
        uint32_t key(dcon::region_id region, table t) noexcept { return 0x80000000u | (uint32_t(t) << 24) | uint32_t(region.index()); }

        table table_of(role r) noexcept {
            switch (r) {
            case role::vulture: return sky_rooms;
            case role::miros: return side_rooms;
            default: return transport_rooms;
            }
        }

        bool has_transport(sys::game_state const& state, dcon::room_id room) {
            bool found = false;
            state.world.room_for_each_shortcut_location(room, [&](dcon::shortcut_location_id sl) {
                found = found || state.world.shortcut_get_type(state.world.shortcut_location_get_shortcut(sl)) == shortcut_type::region_transportation;
            });
            return found;
        }

        template<typename F>
        void for_each_region_room(sys::game_state const& state, dcon::region_id region, F&& f) {
            state.world.region_for_each_region_membership(region, [&](dcon::region_membership_id m) {
                auto const r = state.world.region_membership_get_room(m);
                if (r && !state.world.room_get_is_shelter(r) && !state.world.room_get_is_gate(r))
                    f(r);
            });
        }

        // the routes towards the rooms `exit_room` marks (a breadth first
        // search backwards over the room exits, like dens::build_route's)
        template<typename P>
        std::vector<int16_t> build_table(sys::game_state const& state, dcon::region_id region, P&& exit_room) {
            auto const& w = state.world;
            std::vector<int16_t> next(size_t(w.room_size()), int16_t(-1));
            std::vector<dcon::room_id> queue;
            for_each_region_room(state, region, [&](dcon::room_id r) {
                if (exit_room(r)) {
                    next[size_t(r.index())] = -2;
                    queue.push_back(r);
                }
            });
            std::vector<std::vector<std::pair<dcon::room_id, uint8_t>>> into(size_t(w.room_size()));
            w.for_each_room_exit([&](dcon::room_exit_id e) {
                auto const from = w.room_exit_get_room_from_exit_location(e);
                if (!from || w.room_get_is_shelter(from))
                    return;
                if (auto const to = dens::exit_target(state, e, from, false); to && dens::exit_entrance(state, from, w.room_exit_get_index(e)))
                    into[size_t(to.index())].emplace_back(from, w.room_exit_get_index(e));
            });
            for (size_t qi = 0; qi < queue.size(); ++qi) {
                auto const r = queue[qi];
                for (auto const& [from, node] : into[size_t(r.index())]) {
                    if (next[size_t(from.index())] != -1)
                        continue;
                    next[size_t(from.index())] = int16_t(node);
                    queue.push_back(from);
                }
            }
            return next;
        }

        // UGLYHARDCODEDALLOWEDROOMS (MirosBirdAbstractAI without MMF)
        constexpr char const* hardcoded_miros_rooms[] = {"SH_E01", "SH_E03", "SH_D03", "SH_H01", "SH_E04", "SB_G03", "SB_J01"};

        // the region's tables (serial: world load)
        void prepare(sys::game_state& state, dcon::region_id region) {
            auto& w = state.world;
            if (state.routes.contains(key(region, sky_rooms)))
                return;
            // VultureAbstractAI.RoomViableRoamDestination: a SkyExit wider than 4
            state.routes.emplace(key(region, sky_rooms), build_table(state, region, [&](dcon::room_id r) {
                return borders::any(state, world::get_room_view(state, r), borders::kind::sky, 5);
            }));
            state.routes.emplace(key(region, side_rooms), build_table(state, region, [&](dcon::room_id r) {
                return borders::any(state, world::get_room_view(state, r), borders::kind::side, 1);
            }));
            state.routes.emplace(key(region, transport_rooms), build_table(state, region, [&](dcon::room_id r) { return has_transport(state, r); }));
            // MirosBirdAbstractAI.PopulateAllowedNodes: the SideExits at least
            // 5 wide of the rooms where it likes to Stay (MMF), else of the
            // hardcoded rooms
            std::vector<int16_t> nodes;
            auto const add_room = [&](dcon::room_id r) {
                auto const ex = borders::map(state, world::get_room_view(state, r));
                for (size_t i = 0; i < ex.size(); ++i)
                    if (ex[i].type == borders::kind::side && ex[i].width() >= 5) {
                        nodes.push_back(int16_t(r.index()));
                        nodes.push_back(int16_t(i));
                    }
            };
            for_each_region_room(state, region, [&](dcon::room_id r) {
                if (w.room_get_attraction(r).v[size_t(creature::tmpl::miros_bird)] == creature::attraction::stay)
                    add_room(r);
            });
            if (nodes.empty())
                for (auto const* name : hardcoded_miros_rooms)
                    if (auto const r = state.find_room(text::to_lowercase_native(std::string_view{name})); r && w.room_get_region_from_region_membership(r) == region)
                        add_room(r);
            state.routes.emplace(key(region, miros_nodes), std::move(nodes));
        }

        std::vector<int16_t> const* table_for(sys::game_state const& state, dcon::region_id region, table t) {
            auto const it = state.routes.find(key(region, t));
            return it == state.routes.end() ? nullptr : &it->second;
        }

        dcon::region_id region_of(sys::game_state const& state, dcon::room_id room) {
            return room ? state.world.room_get_region_from_region_membership(room) : dcon::region_id{};
        }
        // the region it lives in: its anchor room's
        dcon::region_id home_region(sys::game_state const& state, dcon::creature_id c) { return region_of(state, state.world.creature_get_home_room(c)); }

        bool is_exit_room(sys::game_state const& state, dcon::region_id region, table t, dcon::room_id room) {
            auto const* v = table_for(state, region, t);
            return v && room && size_t(room.index()) < v->size() && (*v)[size_t(room.index())] == -2;
        }

        // the anchor room of a region: its first room (not a shelter or a
        // gate: its rain is the region's)
        dcon::room_id anchor_of(sys::game_state const& state, dcon::region_id region) {
            dcon::room_id first;
            for_each_region_room(state, region, [&](dcon::room_id r) {
                if (!first)
                    first = r;
            });
            return first;
        }

        // the creatures off screen in the anchor room's bucket (id order)
        template<typename F>
        void for_each_offscreen(sys::game_state const& state, dcon::room_id anchor, F&& f) {
            auto const& w = state.world;
            auto const off = uint8_t(dens::where::offscreen);
            if (state.index.valid) {
                for (auto const c : state.index.away.in(anchor))
                    if (w.creature_get_away(c) == off)
                        f(c);
                return;
            }
            w.for_each_creature([&](dcon::creature_id c) {
                if (w.creature_get_away(c) == off && w.creature_get_away_room(c) == anchor)
                    f(c);
            });
        }

        // Personality(ID).dominance
        float personality_dominance(int32_t seed) {
            rw::unity_random r{seed};
            auto const push = [](float v, float e) {
                if (v == 0.5f)
                    return 0.5f;
                return v < 0.5f ? custom::lerp_map(v, 0.f, 0.5f, 0.f, 0.5f, e) : custom::lerp_map(v, 1.f, 0.5f, 1.f, 0.5f, e);
            };
            float const sympathy = push(r.value(), 1.5f);
            float const energy = push(r.value(), 1.5f);
            float const bravery = push(r.value(), 1.5f);
            r.value();
            r.value();
            float const ar = r.value();
            float const ap = mathf::pow(r.value(), 0.25f);
            float const aggression = mathf::lerp(ar, (energy + bravery) / 2.f * (1.f - sympathy), ap);
            float const dr = r.value();
            float const dp = mathf::pow(r.value(), 0.25f);
            return mathf::lerp(dr, (energy + bravery + aggression) / 3.f, dp);
        }

        // it stays in: dead, hurt, the rain near (WantToStayInDenUntilEndOfCycle:
        // the rain tracker's; a night creature by day)
        bool stays_in(sys::game_state const& state, dcon::creature_id c, dcon::room_id anchor) {
            auto const& w = state.world;
            if (w.creature_get_dead(c) || w.creature_get_health(c) < 0.5f)
                return true;
            uint8_t const f = w.creature_get_spawn_flags(c);
            if (f & dens::flag_pre_cycle)
                return true;
            if ((f & dens::flag_night) != 0 && (f & dens::flag_ignore_cycle) == 0) {
                auto const r = anchor ? w.room_get_rain_region(anchor) : dcon::region_id{};
                if (r && w.region_get_rain(r).day_night_counter < 600)
                    return true;
            }
            return dens::rain_utility(state, c, anchor) > 0.01f;
        }

        bool ready(sys::game_state const& state, dcon::creature_id c, dcon::room_id anchor) {
            auto const& o = st(state, c);
            return o.timer <= 0 && !o.target && !stays_in(state, c, anchor);
        }

        // the vulture's next check room (AddRandomCheckRoom /
        // AddRoomClusterToCheckList, one room at a time): now and then a
        // neighbour of the room it left, else a room of the region with a
        // wide SkyExit rolled by its size dependent attraction squared
        dcon::room_id pick_check_room(sys::game_state const& state, dcon::creature_id c, dcon::room_id left, rw::rng& rng) {
            auto const& w = state.world;
            auto const region = home_region(state, c);
            auto const viable = [&](dcon::room_id r) {
                return r && r != left && is_exit_room(state, region, sky_rooms, r)
                    && w.room_get_attraction(r).v[size_t(creature::template_of(state, c))] != creature::attraction::forbidden;
            };
            if (left && rng.value() > 0.5f && rng.value() < migrate::attraction(state, c, left)) {
                std::vector<dcon::room_id> near;
                w.room_for_each_exit_location(left, [&](dcon::exit_location_id el) {
                    auto const to = dens::exit_target(state, w.exit_location_get_exit(el), left, false);
                    if (viable(to) && rng.value() < migrate::attraction(state, c, to) * 1.5f)
                        near.push_back(to);
                });
                if (!near.empty())
                    return near[size_t(rng.range(0, int32_t(near.size())))];
            }
            std::vector<std::pair<dcon::room_id, float>> list;
            float sum = 0.f;
            for_each_region_room(state, region, [&](dcon::room_id r) {
                if (!viable(r))
                    return;
                float const v = mathf::pow(migrate::size_dependent_attraction(state, c, r), 2.f);
                list.emplace_back(r, v);
                sum += v;
            });
            float roll = rng.value() * sum;
            for (auto const& [r, v] : list) {
                if (roll < v)
                    return r;
                roll -= v;
            }
            return list.empty() ? dcon::room_id{} : list.back().first;
        }

        // ScavengerAbstractAI.RandomDestinationRoom: by the size dependent
        // attraction, of the rooms with a way to the den
        dcon::room_id pick_destination(sys::game_state const& state, dcon::creature_id c, rw::rng& rng) {
            auto const& w = state.world;
            auto const region = home_region(state, c);
            auto const* t = table_for(state, region, transport_rooms);
            if (!t)
                return dcon::room_id{};
            std::vector<std::pair<dcon::room_id, float>> list;
            float sum = 0.f;
            for_each_region_room(state, region, [&](dcon::room_id r) {
                if ((*t)[size_t(r.index())] == -1 || w.room_get_attraction(r).v[size_t(creature::template_of(state, c))] == creature::attraction::forbidden)
                    return;
                float const v = migrate::size_dependent_attraction(state, c, r);
                if (v <= 0.f)
                    return;
                list.emplace_back(r, v);
                sum += v;
            });
            float roll = rng.value() * sum;
            for (auto const& [r, v] : list) {
                if (roll < v)
                    return r;
                roll -= v;
            }
            return dcon::room_id{};
        }

        // the lowest tile of a side exit (on the floor), its side (-1 left)
        ivec2 floor_of(borders::exit const& e) {
            ivec2 best = e.tiles.front();
            for (auto const t : e.tiles)
                if (t.y < best.y)
                    best = t;
            return best;
        }

        // the room's region transportation pipes
        std::vector<dcon::shortcut_id> transports(sys::game_state const& state, dcon::room_id room) {
            std::vector<dcon::shortcut_id> list;
            state.world.room_for_each_shortcut_location(room, [&](dcon::shortcut_location_id sl) {
                auto const s = state.world.shortcut_location_get_shortcut(sl);
                if (state.world.shortcut_get_type(s) == shortcut_type::region_transportation)
                    list.push_back(s);
            });
            return list;
        }

        // out of the den into its room's body (serial): the fields of a creature out in a room
        void out_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, ivec2 tile) {
            auto& w = state.world;
            w.physical_object_set_room_from_object_location(creature::body(state, c), room);
            w.creature_set_away(c, uint8_t(dens::where::room));
            w.creature_set_away_room(c, dcon::room_id{});
            w.creature_set_entering_shortcut(c, ivec2{-1, -1});
            w.creature_set_vessel_shortcut(c, dcon::shortcut_id{});
            w.creature_set_going_home(c, 0);
            w.creature_set_destination(c, tile);
            creature::placed_in_room(state, c, room, room_view::middle_of_tile(tile));
            // Creature.FlyIntoRoom
            w.creature_set_shortcut_delay(c, 360);
            auto& o = st(state, c);
            o.room_timer = 0;
            o.flags = uint8_t(o.flags & ~(flag_going_home | flag_exit_tile | flag_impatient));
            ++stats().entered;
        }

        // ShortcutHandler.CreatureEnterFromAbstractRoom (serial)
        void enter(sys::game_state& state, dcon::creature_id c) {
            auto& w = state.world;
            auto& o = st(state, c);
            auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
            auto const r = role_of(state, c);
            auto const room = o.target;
            auto const give_up = [&] {
                o.target = dcon::room_id{};
                o.entry = o.leave = 0;
                o.leader = dcon::creature_id{};
                o.timer = 400;
            };
            if (!room) {
                give_up();
                return;
            }
            auto const view = world::get_room_view(state, room);
            if (r == role::scavenger) {
                // out of a region transportation pipe of the room closest
                // to its destination (the highway's way into the region)
                auto const region = home_region(state, c);
                auto const* t = table_for(state, region, transport_rooms);
                dcon::room_id entry;
                if (t) {
                    // (backwards over the exits from the destination)
                    std::vector<dcon::room_id> queue{room};
                    std::vector<uint8_t> seen(size_t(w.room_size()), 0);
                    seen[size_t(room.index())] = 1;
                    for (size_t qi = 0; qi < queue.size() && !entry; ++qi) {
                        auto const q = queue[qi];
                        if ((*t)[size_t(q.index())] == -2) {
                            entry = q;
                            break;
                        }
                        w.room_for_each_exit_location(q, [&](dcon::exit_location_id el) {
                            auto const to = dens::exit_target(state, w.exit_location_get_exit(el), q, false);
                            if (to && !seen[size_t(to.index())]) {
                                seen[size_t(to.index())] = 1;
                                queue.push_back(to);
                            }
                        });
                    }
                }
                auto const pipes = entry ? transports(state, entry) : std::vector<dcon::shortcut_id>{};
                if (pipes.empty()) {
                    give_up();
                    return;
                }
                auto const s = pipes[size_t(rng.range(0, int32_t(pipes.size())))];
                // the vessel comes up the pipe (dens.hpp spits it out at its entrance)
                w.creature_set_away(c, uint8_t(dens::where::shortcut));
                w.creature_set_away_room(c, entry);
                w.creature_set_vessel_shortcut(c, s);
                w.creature_set_vessel_reverse(c, 1);
                w.creature_set_vessel_progress(c, 0.f);
                w.creature_set_vessel_last_progress(c, 0.f);
                o.room_timer = rng.range(400, 4800);
                o.flags = uint8_t(o.flags & ~(flag_going_home | flag_exit_tile | flag_impatient));
                ++stats().entered;
                return;
            }
            auto const exits = borders::map(state, view);
            if (r == role::vulture) {
                // a random wide SkyExit (RandomRelevantNodeAccountingForEntranceWidth)
                std::vector<size_t> ok;
                for (size_t i = 0; i < exits.size(); ++i)
                    if (exits[i].type == borders::kind::sky && exits[i].width() > 4)
                        ok.push_back(i);
                if (ok.empty()) {
                    give_up();
                    return;
                }
                auto const& e = exits[ok[size_t(rng.range(0, int32_t(ok.size())))]];
                ivec2 const tile = e.tiles[size_t(rng.range(0, int32_t(e.tiles.size())))];
                o.target = dcon::room_id{};
                out_in_room(state, c, room, tile);
                // FlyingCreatureArrivedInRealizedRoom: 99 tiles over its tile
                vulture::fly_into_room(state, c, room, room_view::middle_of_tile(tile + ivec2{0, 99}), vec2{0.f, -1.f});
                return;
            }
            // a miros bird: in by the raid's side exit, out by the other one
            auto const side_ok = [&](uint8_t n, int32_t min_w) {
                return n > 0 && size_t(n - 1) < exits.size() && exits[n - 1].type == borders::kind::side && exits[n - 1].width() >= min_w;
            };
            if (!side_ok(o.entry, 5)) {
                o.entry = 0;
                for (size_t i = 0; i < exits.size() && !o.entry; ++i)
                    if (exits[i].type == borders::kind::side && exits[i].width() >= 5)
                        o.entry = uint8_t(i + 1);
            }
            if (!o.entry) {
                give_up();
                return;
            }
            ivec2 const in = floor_of(exits[o.entry - 1]);
            ivec2 const out = side_ok(o.leave, 1) ? floor_of(exits[o.leave - 1]) : in;
            out_in_room(state, c, room, in);
            o.exit_tile = out;
            o.flags = uint8_t(o.flags | flag_exit_tile);
            miros::walk_in(state, c, room, in, in.x == 0 ? -1 : 1);
        }

        void defer_enter(sys::game_state& state, dcon::creature_id c, dcon::room_id anchor) {
            state.defer(anchor, [c](sys::game_state& s) {
                if (s.world.creature_is_valid(c) && s.world.creature_get_away(c) == uint8_t(dens::where::offscreen))
                    enter(s, c);
            });
        }

        // MirosBirdAbstractAI.Raid
        void raid(sys::game_state& state, dcon::creature_id c, dcon::room_id anchor, rw::rng& rng) {
            auto& o = st(state, c);
            int32_t num = 0;
            bool higher = false;
            for_each_offscreen(state, anchor, [&](dcon::creature_id m) {
                if (higher || state.world.creature_get_type(m) != creature::type::miros || !ready(state, m, anchor))
                    return;
                if (st(state, m).dominance > o.dominance) {
                    higher = true;
                    return;
                }
                ++num;
            });
            if (higher)
                return;
            // (not vanilla: a bird whose flock has shrunk goes after waiting long)
            if (num < 3 && num < int32_t(o.flock) && (o.flags & flag_impatient) == 0) {
                if (++o.room_timer > 2400)
                    o.flags = uint8_t(o.flags | flag_impatient);
                return;
            }
            auto const* nodes = table_for(state, home_region(state, c), miros_nodes);
            if (!nodes || nodes->size() < 2)
                return;
            size_t const pick = size_t(rng.range(0, int32_t(nodes->size() / 2))) * 2;
            auto const room = dcon::room_id{dcon::room_id::value_base_t((*nodes)[pick])};
            auto const node = uint8_t((*nodes)[pick + 1]);
            // another wide side exit of the room to leave by
            uint8_t leave = 0;
            auto const exits = borders::map(state, world::get_room_view(state, room));
            for (size_t i = 0; i < exits.size() && !leave; ++i)
                if (i != node && exits[i].type == borders::kind::side && exits[i].width() >= 5)
                    leave = uint8_t(i + 1);
            auto const send = [&](dcon::creature_id m, int32_t wait) {
                auto& x = st(state, m);
                x.target = room;
                x.entry = uint8_t(node + 1);
                x.leave = leave ? leave : uint8_t(node + 1);
                x.leader = c;
                x.timer = wait;
                x.room_timer = 0;
            };
            send(c, 0);
            // the followers (the leader among them, as vanilla's loop has it)
            int32_t n = rng.range(2, 5);
            for_each_offscreen(state, anchor, [&](dcon::creature_id m) {
                if (n <= 0 || state.world.creature_get_type(m) != creature::type::miros || (m != c && !ready(state, m, anchor)))
                    return;
                send(m, rng.range(0, rng.range(0, 50)));
                --n;
            });
            ++stats().raids;
        }

        // ScavengerAbstractAI.TryAssembleSquad
        void assemble_squad(sys::game_state& state, dcon::creature_id c, dcon::room_id anchor, rw::rng& rng) {
            auto& w = state.world;
            auto& o = st(state, c);
            int32_t num = 0;
            bool higher = false;
            std::vector<dcon::creature_id> others;
            for_each_offscreen(state, anchor, [&](dcon::creature_id m) {
                if (higher || m == c || w.creature_get_type(m) != creature::type::scavenger || !ready(state, m, anchor))
                    return;
                if (st(state, m).dominance > o.dominance) {
                    higher = true;
                    return;
                }
                others.push_back(m);
                ++num;
            });
            if (higher || num < 2)
                return;
            auto const dest = pick_destination(state, c, rng);
            if (!dest)
                return;
            int32_t max_exclusive = 7;
            auto const region = home_region(state, c);
            std::string const name = region ? text::to_lowercase_native(state.to_string_view(w.region_get_name(region))) : std::string{};
            if (name == "sh" || name == "cc" || name == "lc")
                max_exclusive = 5;
            else if (name == "su" || name == "uw" || name == "si")
                max_exclusive = 3;
            int32_t n = std::min(num, rng.range(2, max_exclusive));
            o.target = dest;
            o.leader = c;
            o.timer = 0;
            int32_t m = 1;
            for (auto const x : others) {
                if (n <= 0)
                    break;
                auto& xs = st(state, x);
                xs.target = dest;
                xs.leader = c;
                xs.timer = m * 10 + rng.range(0, 10);
                ++m;
                --n;
            }
            ++stats().squads;
        }
    }

    counters& stats() noexcept {
        static counters c;
        return c;
    }

    bool homed(sys::game_state const& state, dcon::creature_id c) noexcept {
        return state.world.creature_get_home_den(c) == den_node && state.world.creature_get_home_room(c);
    }

    bool heading_home(sys::game_state const& state, dcon::creature_id c) noexcept {
        return homed(state, c) && (st(state, c).flags & flag_going_home) != 0;
    }

    bool lives_offscreen(creature::type kind) noexcept { return role_of(kind) != role::none; }

    uint32_t spawn(sys::game_state& state, dcon::region_id region, spawns::spawn const& s, spawns::report& r) {
        auto& w = state.world;
        auto const k = spawns::kind_of(s.type);
        auto const anchor = anchor_of(state, region);
        auto const miss = [&] {
            ++r.no_room;
            if (r.missing.size() < 8)
                r.missing.push_back(s.room + ":" + std::to_string(s.den) + " " + s.type);
            return 0u;
        };
        if (!k || !lives_offscreen(k->kind) || !anchor)
            return miss();
        prepare(state, region);
        auto const data = spawns::parse_data(s.data);
        uint8_t flags = data.flags;
        if (s.night)
            flags = uint8_t((flags | dens::flag_night) & ~dens::flag_ignore_cycle);
        auto const view = world::get_room_view(state, anchor);
        vec2 const middle{view.pixel_width() * 0.5f, view.pixel_height() * 0.5f};
        uint32_t made = 0;
        int32_t const n = std::max(1, s.count);
        for (int32_t i = 0; i < n; ++i) {
            // RainWorldGame.GetNewID: the next number, the seed unless {Seed:N}
            int32_t const number = ++state.next_issued_id;
            int32_t const seed = data.seed >= 0 ? data.seed : number;
            state.spawn_seed = seed;
            auto const c = creature::create(state, anchor, middle, *k);
            state.spawn_seed = -1;
            if (!c)
                continue;
            w.creature_set_spawn_flags(c, flags);
            w.creature_set_home_room(c, anchor);
            w.creature_set_home_den(c, den_node);
            // in the offscreen den: the body in no room
            w.creature_set_away(c, uint8_t(dens::where::offscreen));
            w.creature_set_away_room(c, anchor);
            w.creature_set_remain_in_den(c, -1);
            w.physical_object_set_room_from_object_location(creature::body(state, c), dcon::room_id{});
            offscreen::state o;
            o.dominance = personality_dominance(seed);
            auto rng = rw::random(state.seed, 0, uint32_t(c.value));
            // AbstractCreature's constructor's 100 to 1000 ticks; a scavenger's
            // first InOffscreenDen freeze
            o.timer = k->kind == creature::type::scavenger ? rng.range(300, 500) : rng.range(100, 1000);
            w.creature_set_offscreen(c, o);
            ++made;
            ++stats().spawned;
            std::string key = std::string{creature::to_string(k->kind)};
            if (k->kind == creature::type::vulture)
                key = vulture::to_string(vulture::variant(k->variety));
            else if (k->kind == creature::type::scavenger && k->variety != 0)
                key = k->variety == uint8_t(scavenger::variant::elite) ? "scavenger elite" : "scavenger disciple";
            ++r.made[key + " (offscreen)"];
            ++r.creatures;
            r.night += (flags & dens::flag_night) ? 1 : 0;
            r.pre_cycle += (flags & dens::flag_pre_cycle) ? 1 : 0;
            r.ignore_cycle += (flags & dens::flag_ignore_cycle) ? 1 : 0;
        }
        // the miros birds' flock: all of the region's (Raid's "all waiting")
        if (k->kind == creature::type::miros) {
            uint32_t flock = 0;
            w.for_each_creature([&](dcon::creature_id c) {
                if (w.creature_get_type(c) == creature::type::miros && homed(state, c) && w.creature_get_home_room(c) == anchor)
                    ++flock;
            });
            w.for_each_creature([&](dcon::creature_id c) {
                if (w.creature_get_type(c) == creature::type::miros && homed(state, c) && w.creature_get_home_room(c) == anchor)
                    st(state, c).flock = uint8_t(std::min(flock, 255u));
            });
        }
        return made;
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& anchor) {
        auto& w = state.world;
        auto& o = st(state, c);
        // out of the rain and of everything else
        w.creature_set_rain_death(c, 0.f);
        w.creature_set_stun(c, 0);
        if (w.creature_get_dead(c))
            return;
        // (not vanilla: it heals in the den, a stand-in for the abstract
        // creature's slow recovery)
        float& health = w.creature_get_health(c);
        if (health < 1.f)
            health = std::min(1.f, health + 0.0005f);
        if (o.timer > 0) {
            --o.timer;
            return;
        }
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        if (stays_in(state, c, anchor.id)) {
            // (the raid or the squad it was sent with is off)
            o.target = dcon::room_id{};
            o.leader = dcon::creature_id{};
            o.timer = 100;
            return;
        }
        switch (role_of(state, c)) {
        case role::vulture:
            if (!o.target)
                o.target = pick_check_room(state, c, dcon::room_id{}, rng);
            if (!o.target) {
                o.timer = 400;
                return;
            }
            defer_enter(state, c, anchor.id);
            o.timer = 1;
            return;
        case role::miros:
            if (!o.target)
                raid(state, c, anchor.id, rng);
            if (o.target && o.timer <= 0) {
                defer_enter(state, c, anchor.id);
                o.timer = 1;
            }
            return;
        case role::scavenger:
            if (!o.target)
                assemble_squad(state, c, anchor.id, rng);
            if (o.target && o.timer <= 0) {
                defer_enter(state, c, anchor.id);
                o.timer = 1;
            }
            return;
        case role::none: return;
        }
    }

    std::optional<ivec2> way_out(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto const r = role_of(state, c);
        auto const region = home_region(state, c);
        auto const* t = table_for(state, region, table_of(r));
        if (!t || r == role::none || size_t(room.id.index()) >= t->size())
            return std::nullopt;
        int16_t const m = (*t)[size_t(room.id.index())];
        if (m >= 0)
            return dens::exit_entrance(state, room.id, uint8_t(m));
        if (m != -2)
            return std::nullopt;
        switch (r) {
        case role::vulture:
            // up through the sky (the vulture's own way)
            return std::nullopt;
        case role::miros: {
            auto& o = st(state, c);
            if ((o.flags & flag_exit_tile) && o.target == room.id)
                return o.exit_tile;
            // (out of the raid's room: the closest side exit)
            vec2 const at = creature::main_pos(state, c);
            std::optional<ivec2> best;
            float best_d = std::numeric_limits<float>::max();
            for (auto const& e : borders::map(state, room)) {
                if (e.type != borders::kind::side)
                    continue;
                ivec2 const f = floor_of(e);
                float const d = rw::distance(room_view::middle_of_tile(f), at);
                if (d < best_d) {
                    best_d = d;
                    best = f;
                }
            }
            if (best) {
                o.target = room.id;
                o.exit_tile = *best;
                o.flags = uint8_t(o.flags | flag_exit_tile);
            }
            return best;
        }
        case role::scavenger: {
            vec2 const at = creature::main_pos(state, c);
            std::optional<ivec2> best;
            float best_d = std::numeric_limits<float>::max();
            for (auto const s : transports(state, room.id)) {
                ivec2 const start = w.shortcut_get_start(s);
                float const d = rw::distance(room_view::middle_of_tile(start), at);
                if (d < best_d) {
                    best_d = d;
                    best = start;
                }
            }
            return best;
        }
        case role::none: break;
        }
        return std::nullopt;
    }

    bool near_border(sys::game_state const& state, dcon::creature_id c, room_view const& room) {
        auto const& w = state.world;
        ivec2 const tile = w.creature_get_travel_tile(c);
        if (!room.inside(tile.x, tile.y) || (tile.x != 0 && tile.x != room.width - 1 && tile.y != room.height - 1))
            return false;
        vec2 const p = creature::main_pos(state, c);
        vec2 const m = room_view::middle_of_tile(tile);
        // (walking: at the edge, over the exit's floor tile)
        if (std::abs(p.y - m.y) > 200.f)
            return false;
        if (tile.x == 0 && p.x < 40.f)
            return true;
        if (tile.x == room.width - 1 && p.x > room.pixel_width() - 40.f)
            return true;
        return std::abs(p.x - m.x) < 20.f;
    }

    void arrive(sys::game_state& state, dcon::creature_id c, dcon::room_id from, int32_t meal) {
        auto& w = state.world;
        if (!w.creature_is_valid(c) || !homed(state, c))
            return;
        for (uint32_t g = 0; g < 2; ++g)
            creature::release_grasp(state, c, g);
        auto const anchor = w.creature_get_home_room(c);
        w.creature_set_away(c, uint8_t(dens::where::offscreen));
        w.creature_set_away_room(c, anchor);
        w.creature_set_vessel_shortcut(c, dcon::shortcut_id{});
        w.creature_set_vessel_reverse(c, 0);
        w.creature_set_vessel_progress(c, 0.f);
        w.creature_set_entering_shortcut(c, ivec2{-1, -1});
        w.creature_set_going_home(c, 0);
        w.creature_set_remain_in_den(c, -1);
        w.physical_object_set_room_from_object_location(creature::body(state, c), dcon::room_id{});
        migrate::cancel(state, c);
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        auto& o = st(state, c);
        auto const r = role_of(state, c);
        o.flags = 0;
        o.target = dcon::room_id{};
        o.leader = dcon::creature_id{};
        o.entry = o.leave = 0;
        o.room_timer = 0;
        o.timer = travel_ticks(r, rng) + std::max(0, meal);
        if (r == role::vulture)
            o.target = pick_check_room(state, c, from, rng);
        if (r == role::scavenger)
            o.timer += rng.range(300, 500);
        ++stats().left;
    }

    bool vulture_idle(sys::game_state& state, dcon::creature_id c) {
        auto& o = st(state, c);
        // VultureAbstractAI: 620 in the check room, then on to the next one
        if (++o.room_timer > 620)
            o.flags = uint8_t(o.flags | flag_going_home);
        return (o.flags & flag_going_home) != 0;
    }

    void take_flight(sys::game_state& state, dcon::creature_id c, room_view const& room, int32_t meal) {
        state.defer(room.id, [c, from = room.id, meal](sys::game_state& s) {
            if (s.world.creature_is_valid(c) && s.world.creature_get_away(c) == uint8_t(dens::where::room) && rooms::room_of(s, c) == from)
                arrive(s, c, from, meal);
        });
    }

    void scavenger_tick(sys::game_state& state, dcon::creature_id c, room_view const& room, float roaming) {
        auto& o = st(state, c);
        if (o.flags & flag_going_home)
            return;
        // the squad's destination (GoToRoom: the way there)
        if (o.target) {
            if (o.target == room.id)
                o.target = dcon::room_id{};
            else if (!migrate::want_to(state, c) && !migrate::plan_trip(state, c, room.id, o.target))
                o.target = dcon::room_id{};
            return;
        }
        // a leader gone elsewhere (home, another room): the squad is over
        if (o.leader && o.leader != c && !rooms::reachable(state, o.leader))
            o.leader = dcon::creature_id{};
        if (o.room_timer > 0) {
            --o.room_timer;
            return;
        }
        if (migrate::want_to(state, c))
            return;
        float num = roaming;
        if (o.leader && o.leader != c)
            num /= 3.f;
        if (o.leader == c)
            num = 0.8f;
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        // TimeInfluencedRandomRoll(0.1 / lerp(100, 2, num), 1)
        if (!(rng.value() < 0.1f / mathf::lerp(100.f, 2.f, num)))
            return;
        // Migrate: home one time in five (always on its own), a squad
        // member only with its leader
        bool const home = (rng.value() < 0.2f || !o.leader) && (!o.leader || o.leader == c);
        if (home) {
            o.flags = uint8_t(o.flags | flag_going_home);
            o.leader = dcon::creature_id{};
        } else {
            migrate::random_move(state, c, room, int32_t(mathf::lerp(30.f, 600.f, num)));
        }
        o.room_timer = rng.range(400, 4800);
    }
}
