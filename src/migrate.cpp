#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "migrate.hpp"
#include "dens.hpp"
#include "gates.hpp"
#include "warps.hpp"
#include "pathing.hpp"
#include "rooms.hpp"

namespace migrate {
    using attr = creature::attraction;
    using creature::tmpl;
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // CreatureTemplate.abstractedLaziness, roamBetweenRoomsChance, canFly
        // (StaticWorld, copied from the ancestor where a template sets none)
        int32_t laziness(tmpl t) noexcept {
            if (t >= tmpl::pink_lizard && t <= tmpl::peach_lizard)
                return 50;
            switch (t) {
            case tmpl::small_centipede: return 1000;
            case tmpl::centipede:
            case tmpl::centiwing:
            case tmpl::red_centipede:
            case tmpl::aqua_centi: return 150;
            case tmpl::leech:
            case tmpl::sea_leech:
            case tmpl::jungle_leech:
            case tmpl::cicada_a:
            case tmpl::cicada_b: return 100;
            case tmpl::daddy_long_legs:
            case tmpl::brother_long_legs:
            case tmpl::terror_long_legs:
            case tmpl::hunter_daddy: return 400;
            case tmpl::drop_bug: return 200;
            default: return 1;
            }
        }
        float roam_chance(tmpl t) noexcept {
            switch (t) {
            case tmpl::small_centipede: return -1.f;
            case tmpl::daddy_long_legs:
            case tmpl::brother_long_legs:
            case tmpl::terror_long_legs:
            case tmpl::hunter_daddy: return 0.05f;
            case tmpl::drop_bug: return 0.025f;
            default: return 0.1f;
            }
        }
        bool can_fly(tmpl t) noexcept {
            return t == tmpl::cicada_a || t == tmpl::cicada_b || t == tmpl::centiwing;
        }

        attr parse_attraction(std::string_view s) {
            if (s == "Forbidden")
                return attr::forbidden;
            if (s == "Avoid")
                return attr::avoid;
            if (s == "Like")
                return attr::like;
            if (s == "Stay")
                return attr::stay;
            // BackwardsCompatibilityRemix.ParseRoomAttraction's numbers
            if (s == "0")
                return attr::neutral;
            if (s == "1")
                return attr::forbidden;
            if (s == "2")
                return attr::avoid;
            if (s == "3")
                return attr::like;
            if (s == "4")
                return attr::stay;
            return attr::neutral;
        }

        std::string_view trim(std::string_view s) {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
                s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
                s.remove_suffix(1);
            return s;
        }

        // a template by its CreatureTemplate.Type name
        bool parse_template(std::string_view s, tmpl& out) {
            for (uint32_t i = 0; i < creature::tmpl_count; ++i)
                if (s == creature::to_string(tmpl(i))) {
                    out = tmpl(i);
                    return true;
                }
            return false;
        }

        // the exits of `room` a trip can take: the target room and the exit
        // node; with `cross`, through gates and Watcher warps too (a warp:
        // `warp_hop` + its number in the room)
        template<typename F>
        void for_each_exit(sys::game_state const& state, dcon::room_id room, bool cross, F&& f) {
            auto const& w = state.world;
            w.room_for_each_exit_location(room, [&](dcon::exit_location_id el) {
                auto const e = w.exit_location_get_exit(el);
                auto const to = dens::exit_target(state, e, room, cross);
                if (to && dens::exit_entrance(state, room, w.room_exit_get_index(e)))
                    f(to, uint8_t(w.room_exit_get_index(e)));
            });
            if (!cross)
                return;
            uint8_t k = 0;
            w.room_for_each_warp_point_location(room, [&](dcon::warp_point_location_id l) {
                auto const wp = w.warp_point_location_get_point(l);
                if (warps::is_dynamic(state, wp))
                    return;
                auto const to = warps::leads_away(state, wp) ? warps::where_to(state, wp).room : dcon::room_id{};
                if (to && to != room && k < 0x0F)
                    f(to, uint8_t(warp_hop + k));
                ++k;
            });
        }

        // the exit node of `room` leading to `to`, -1 none
        int32_t exit_to(sys::game_state const& state, dcon::room_id room, dcon::room_id to) {
            int32_t node = -1;
            auto const& w = state.world;
            w.room_for_each_exit_location(room, [&](dcon::exit_location_id el) {
                auto const e = w.exit_location_get_exit(el);
                if (node < 0 && w.room_exit_get_target_from_exit_connection(e) == to)
                    node = w.room_exit_get_index(e);
            });
            return node;
        }

        // AbstractRoomNode.ConnectionLength between two exits of a room, as
        // the tiles between their pipe entrances
        int32_t connection_length(sys::game_state const& state, dcon::room_id room, int32_t from_node, uint8_t to_node) {
            if (from_node < 0 || from_node == to_node)
                return 0;
            auto const a = dens::exit_entrance(state, room, uint8_t(from_node));
            auto const b = dens::exit_entrance(state, room, to_node);
            if (!a || !b)
                return 0;
            return std::abs(a->x - b->x) + std::abs(a->y - b->y);
        }

        bool can_roam_through(sys::game_state const& state, dcon::creature_id c, dcon::room_id room) {
            auto const t = creature::template_of(state, c);
            return state.world.room_get_attraction(room).v[size_t(t)] != attr::forbidden && !state.world.room_get_is_shelter(room)
                && !state.world.room_get_is_gate(room);
        }

        void set_trip(sys::game_state& state, dcon::creature_id c, std::vector<std::pair<dcon::room_id, uint8_t>> const& rooms) {
            creature::trip t;
            for (auto const& [r, node] : rooms) {
                if (t.hops >= creature::trip::max_hops)
                    break;
                t.room[t.hops] = uint16_t(r.index());
                t.exit[t.hops] = node;
                ++t.hops;
            }
            state.world.creature_set_trip(c, t);
            ++dens::stats().trips;
            for (uint32_t i = 0; i < t.hops; ++i)
                if (t.exit[i] >= warp_hop) {
                    ++dens::stats().warp_trips;
                    break;
                }
            if (t.hops > 0) {
                auto const& w = state.world;
                auto const last = dcon::room_id{dcon::room_id::value_base_t(t.room[t.hops - 1])};
                auto const here = creature::room_of(state, c);
                if (here && w.room_get_region_from_region_membership(last) != w.room_get_region_from_region_membership(here))
                    ++dens::stats().cross_trips;
            }
        }

        // the exit node the creature is at or closest to (DefineNodeOfLocalCoordinate)
        int32_t node_of(sys::game_state const& state, dcon::creature_id c, dcon::room_id room) {
            auto const& w = state.world;
            vec2 const at = creature::main_pos(state, c);
            int32_t best = -1;
            float best_d = 1e30f;
            w.room_for_each_exit_location(room, [&](dcon::exit_location_id el) {
                auto const e = w.exit_location_get_exit(el);
                if (auto const t = dens::exit_entrance(state, room, w.room_exit_get_index(e))) {
                    float const d = rw::distance(room_view::middle_of_tile(*t), at);
                    if (d < best_d) {
                        best_d = d;
                        best = w.room_exit_get_index(e);
                    }
                }
            });
            return best;
        }
    }

    void load_attractions(sys::game_state& state) {
        auto& w = state.world;
        w.for_each_room([&](dcon::room_id r) {
            creature::attractions a;
            if (w.room_get_is_shelter(r) || w.room_get_is_gate(r))
                for (auto& v : a.v)
                    v = attr::forbidden;
            w.room_set_attraction(r, a);
        });
        auto const world_dir = state.fs.get_root().get_single_subdir("world");
        w.for_each_region([&](dcon::region_id region) {
            std::string const name{state.to_string_view(w.region_get_name(region))};
            auto const of = world_dir.get_single_subdir(name).get_single_file("properties.txt").open_file(state.fs);
            if (!of)
                return;
            std::string_view text = of->to_string_view();
            std::vector<std::pair<tmpl, attr>> defaults;
            // Room_Attr: <room or Default>: Type-Attraction,Type-Attraction,
            auto const apply = [&](std::string_view list, auto&& set) {
                while (!list.empty()) {
                    size_t const comma = list.find(',');
                    std::string_view item = trim(list.substr(0, comma));
                    list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
                    size_t const dash = item.find('-');
                    if (dash == std::string_view::npos)
                        continue;
                    tmpl t;
                    if (parse_template(trim(item.substr(0, dash)), t))
                        set(t, parse_attraction(trim(item.substr(dash + 1))));
                }
            };
            std::vector<std::pair<std::string, std::string_view>> per_room;
            while (!text.empty()) {
                size_t const nl = text.find('\n');
                std::string_view line = trim(text.substr(0, nl));
                text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
                if (!line.starts_with("Room_Attr:"))
                    continue;
                line = trim(line.substr(10));
                size_t const colon = line.find(':');
                if (colon == std::string_view::npos)
                    continue;
                std::string_view const room_name = trim(line.substr(0, colon));
                std::string_view const list = trim(line.substr(colon + 1));
                if (room_name == "Default")
                    apply(list, [&](tmpl t, attr v) { defaults.emplace_back(t, v); });
                else
                    per_room.emplace_back(std::string{room_name}, list);
            }
            // the region's rooms: the default, then their own
            w.region_for_each_region_membership(region, [&](dcon::region_membership_id m) {
                auto const r = w.region_membership_get_room(m);
                if (w.room_get_is_shelter(r) || w.room_get_is_gate(r))
                    return;
                auto& a = w.room_get_attraction(r);
                for (auto const& [t, v] : defaults)
                    a.v[size_t(t)] = v;
            });
            for (auto const& [room_name, list] : per_room) {
                auto const r = state.find_room(room_name);
                if (!r || w.room_get_is_shelter(r) || w.room_get_is_gate(r))
                    continue;
                auto& a = w.room_get_attraction(r);
                apply(list, [&](tmpl t, attr v) { a.v[size_t(t)] = v; });
            }
        });
    }

    dcon::room_id destination(sys::game_state const& state, dcon::creature_id c) noexcept {
        auto const& t = state.world.creature_get_trip(c);
        if (t.next < t.hops)
            return dcon::room_id{dcon::room_id::value_base_t(t.room[t.hops - 1])};
        return rooms::room_of(state, c);
    }

    dcon::room_id next_room(sys::game_state const& state, dcon::creature_id c) noexcept {
        auto const& t = state.world.creature_get_trip(c);
        if (t.next < t.hops)
            return dcon::room_id{dcon::room_id::value_base_t(t.room[t.next])};
        return dcon::room_id{};
    }

    float attraction(sys::game_state const& state, dcon::creature_id c, dcon::room_id room) noexcept {
        if (!room)
            return 0.f;
        return creature::attraction_value(state.world.room_get_attraction(room).v[size_t(creature::template_of(state, c))]);
    }

    float size_dependent_attraction(sys::game_state const& state, dcon::creature_id c, dcon::room_id room) noexcept {
        auto const& w = state.world;
        float const a = attraction(state, c, room);
        float const x = float(w.room_get_width(room)), y = float(w.room_get_height(room));
        if (can_fly(creature::template_of(state, c)))
            return a * mathf::lerp(x * y / 2000.f, 1.f, 0.985f);
        return a * mathf::lerp(x * mathf::lerp(x, 1.f, 0.5f) * y / 50000.f, 1.f, 0.985f);
    }

    void cancel(sys::game_state& state, dcon::creature_id c) noexcept {
        state.world.creature_set_trip(c, creature::trip{});
    }

    void random_move(sys::game_state& state, dcon::creature_id c, room_view const& room, int32_t max_roam) {
        auto& rng = state.world.creature_get_random(c);
        // the walk, newest room first like vanilla's list (a room and the
        // exit of the room before it)
        std::vector<std::pair<dcon::room_id, uint8_t>> list;
        dcon::room_id cur = room.id;
        int32_t node = node_of(state, c, room.id);
        int32_t dist = 0;
        for (int32_t i = 0; i < 100; ++i) {
            std::vector<std::pair<dcon::room_id, uint8_t>> next;
            for_each_exit(state, cur, false, [&](dcon::room_id to, uint8_t exit) {
                if (!can_roam_through(state, c, to) || to == room.id)
                    return;
                for (auto const& [r, n] : list)
                    if (r == to)
                        return;
                next.emplace_back(to, exit);
            });
            if (next.empty())
                break;
            auto pick = next[size_t(rng.range(0, int32_t(next.size())))];
            float sum = 0.f;
            for (auto const& [r, n] : next)
                sum += size_dependent_attraction(state, c, r);
            float roll = rng.value() * sum;
            for (auto const& e : next) {
                float const v = size_dependent_attraction(state, c, e.first);
                if (roll < v) {
                    pick = e;
                    break;
                }
                roll -= v;
            }
            list.insert(list.begin(), pick);
            dist += connection_length(state, cur, node, pick.second);
            if (dist > max_roam)
                break;
            node = exit_to(state, pick.first, cur);
            cur = pick.first;
        }
        // a destination less attractive than here: back up to a better one
        if (!list.empty() && size_dependent_attraction(state, c, list.front().first) < size_dependent_attraction(state, c, room.id))
            while (list.size() > 1 && size_dependent_attraction(state, c, list[0].first) < size_dependent_attraction(state, c, list[1].first))
                list.erase(list.begin());
        if (list.empty())
            return;
        std::reverse(list.begin(), list.end());
        set_trip(state, c, list);
    }

    void move_to_more_attractive_neighbor(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& rng = state.world.creature_get_random(c);
        auto const t = creature::template_of(state, c);
        if (state.world.room_get_attraction(room.id).v[size_t(t)] == attr::stay)
            return;
        float const here_size = size_dependent_attraction(state, c, room.id);
        float const here = attraction(state, c, room.id);
        std::vector<std::pair<dcon::room_id, uint8_t>> list;
        std::vector<float> weights;
        float sum = 0.f;
        for_each_exit(state, room.id, false, [&](dcon::room_id to, uint8_t exit) {
            if (!can_roam_through(state, c, to) || !(rng.value() < 0.025f || attraction(state, c, to) > here))
                return;
            float v = size_dependent_attraction(state, c, to);
            if (v < here_size)
                v /= 10.f;
            sum += v;
            list.emplace_back(to, exit);
            weights.push_back(v);
        });
        float roll = sum * rng.value();
        for (size_t j = 0; j < list.size(); ++j) {
            if (roll < weights[j]) {
                set_trip(state, c, {list[j]});
                return;
            }
            roll -= weights[j];
        }
    }

    bool plan_trip(sys::game_state& state, dcon::creature_id c, dcon::room_id from, dcon::room_id to) {
        // a breadth first search over the exits, gates and warps included
        auto const& w = state.world;
        size_t const n = size_t(w.room_size());
        std::vector<int32_t> prev(n, -1);
        std::vector<uint8_t> via(n, 0);
        std::vector<dcon::room_id> queue{from};
        prev[size_t(from.index())] = int32_t(from.index());
        for (size_t qi = 0; qi < queue.size() && prev[size_t(to.index())] < 0; ++qi) {
            auto const r = queue[qi];
            for_each_exit(state, r, true, [&](dcon::room_id next, uint8_t exit) {
                if (prev[size_t(next.index())] >= 0 || w.room_get_is_shelter(next))
                    return;
                prev[size_t(next.index())] = int32_t(r.index());
                via[size_t(next.index())] = exit;
                queue.push_back(next);
            });
        }
        if (to == from || prev[size_t(to.index())] < 0)
            return false;
        std::vector<std::pair<dcon::room_id, uint8_t>> list;
        for (auto r = to; r != from; r = dcon::room_id{dcon::room_id::value_base_t(prev[size_t(r.index())])})
            list.emplace_back(r, via[size_t(r.index())]);
        if (list.size() > creature::trip::max_hops)
            return false;
        std::reverse(list.begin(), list.end());
        set_trip(state, c, list);
        return true;
    }

    bool cross_region(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        // the rooms of other regions a few hops away, through a gate or a
        // warp: one of them at random
        auto& w = state.world;
        auto& rng = w.creature_get_random(c);
        auto const here = w.room_get_region_from_region_membership(room.id);
        std::vector<dcon::room_id> seen{room.id}, options;
        for (size_t qi = 0; qi < seen.size() && seen.size() < 64; ++qi)
            for_each_exit(state, seen[qi], true, [&](dcon::room_id next, uint8_t) {
                if (std::find(seen.begin(), seen.end(), next) != seen.end() || w.room_get_is_shelter(next))
                    return;
                seen.push_back(next);
                if (!w.room_get_is_gate(next) && w.room_get_region_from_region_membership(next) != here && can_roam_through(state, c, next))
                    options.push_back(next);
            });
        if (options.empty())
            return false;
        return plan_trip(state, c, room.id, options[size_t(rng.range(0, int32_t(options.size())))]);
    }

    void abstract_behavior(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        if (want_to(state, c))
            return;
        auto& rng = w.creature_get_random(c);
        auto const t = creature::template_of(state, c);
        float const roam = roam_chance(t);
        bool const forbidden = w.room_get_attraction(room.id).v[size_t(t)] == attr::forbidden;
        // MigrationBehavior: TimeInfluencedRandomRoll(chance * lerp(2, 0.1, attraction), 1)
        if (!forbidden) {
            if (!(w.creature_get_time_in_room(c) > rng.range(0, laziness(t))))
                return;
            if (!(rng.value() < roam * mathf::lerp(2.f, 0.1f, attraction(state, c, room.id))))
                return;
        }
        // now and then a trip to another region, through a gate or a warp
        if (rng.value() < cross_region_chance && cross_region(state, c, room))
            return;
        if (rng.value() < 0.5f)
            random_move(state, c, room, int32_t(250.f * mathf::pow(rng.value(), 0.25f)));
        else
            move_to_more_attractive_neighbor(state, c, room);
    }

    void idle_roll(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        // LizardAI.RoomLike without the other lizards: the size dependent attraction
        float const like = size_dependent_attraction(state, c, room.id);
        if (state.world.creature_get_random(c).value() < 1.f / rw::custom::lerp_map(like, 0.35f, 1.f, 10.f, 1000.f))
            abstract_behavior(state, c, room);
    }

    std::optional<ivec2> travel_tile(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto& t = w.creature_get_trip(c);
        if (t.next >= t.hops)
            return std::nullopt;
        uint8_t const exit = t.exit[t.next];
        if (exit >= warp_hop) {
            // a warp: into its middle (warps.hpp carries it once there)
            auto const wp = warps::point(state, room.id, exit - warp_hop);
            if (!wp) {
                cancel(state, c);
                return std::nullopt;
            }
            w.creature_set_going_home(c, 0);
            // the tear hangs in the air: what walks heads for the ground
            // below it, and is pulled in once close and in sight of it (the
            // pull reaches further than that: WarpPoint's PullRadius)
            vec2 const at = warps::position(state, wp);
            ivec2 tile = room_view::get_tile_position(at);
            for (int32_t i = 0; i < 20 && tile.y > 0 && !room.get_tile(tile + ivec2{0, -1}).solid(); ++i)
                --tile.y;
            vec2 const me = creature::main_pos(state, c);
            // (a slugcat goes through the warp's own sequence: it stands in
            // it until the warp sets off, npc.cpp)
            if (!creature::is_slugcat(w, c)
                && ((custom::dist_less(me, at, warps::pull_radius) && room.visual_contact(me, at)) || custom::dist_less(me, room_view::middle_of_tile(tile), 40.f)))
                warps::carry(state, c, wp);
            return tile;
        }
        auto const tile = dens::exit_entrance(state, room.id, exit);
        if (!tile) {
            cancel(state, c);
            return std::nullopt;
        }
        // a gate shut between it and the exit: wait at the middle door
        if (auto const g = gates::wait_tile(state, room.id, creature::main_pos(state, c), *tile))
            return *g;
        w.creature_set_going_home(c, 1);
        w.creature_set_travel_tile(c, *tile);
        return *tile + room.shortcut_entrance_hole_direction(*tile);
    }

    void arrived(sys::game_state& state, dcon::creature_id c, dcon::room_id room) {
        auto& w = state.world;
        auto& t = w.creature_get_trip(c);
        w.creature_set_time_in_room(c, 0);
        ++w.creature_get_rooms_changed(c);
        if (t.next >= t.hops)
            return;
        // AbstractCreatureAI.Moved: past the rooms of the trip it is in now
        for (uint32_t i = t.hops; i-- > t.next;)
            if (t.room[i] == uint16_t(room.index())) {
                t.next = uint8_t(i + 1);
                if (t.next >= t.hops)
                    t = creature::trip{};
                return;
            }
        // off the planned way (fled, carried, went home): the trip is over
        t = creature::trip{};
    }
}
