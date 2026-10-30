#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include <tbb/parallel_for.h>

#include "npc.hpp"
#include "creature.hpp"
#include "dens.hpp"
#include "items.hpp"
#include "migrate.hpp"
#include "pathing.hpp"
#include "player.hpp"
#include "player_graphics.hpp"
#include "rain.hpp"
#include "rooms.hpp"
#include "tracker.hpp"
#include "warps.hpp"

namespace npc {
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    using routes = sys::slugcat_room_routes;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    counters& stats() noexcept {
        static counters c;
        return c;
    }

    debug_options& debug() noexcept {
        static debug_options d;
        return d;
    }

    namespace {
        // a room's goals: its exits by node, its warps after them
        constexpr uint32_t warp_bit = routes::max_exits;
        // a search state: a room (its index) and how the slugcat got there,
        // through one of its exits (the exit's node) or a warp (arrival_slot +
        // the warp's slot)
        constexpr uint32_t slots = 64;
        constexpr uint32_t arrival_slot = 56;
        // the rain is close: ticks before it, when an NPC makes for the
        // nearest shelter
        constexpr int32_t hurry_ticks = 1600;
        // how long it keeps running from a threat it no longer sees
        constexpr int32_t forget_threat = 100;
        // a threat this close (and seen) sends it running
        constexpr float threat_range = 360.f;
        // running from threats this many ticks (lately) it has had enough:
        // it ignores them this long and goes on its way
        constexpr int32_t flee_patience = 400;
        constexpr int32_t brave_ticks = 600;
        // standing in a warp this long without it setting off (a tear out of
        // reach): carried through anyway
        constexpr int32_t warp_patience = 400;
        // a guessed way on steps off an edge only this far above something
        // to stand on
        constexpr int32_t safe_drop = 6;
        // Spearmaster fighting a threat it can't kill this long: on its way
        constexpr int32_t fight_patience = 1200;
        // a weapon lying this close to its body it picks up on the way; a
        // threat this close and level with it gets one thrown at it
        constexpr float pick_range = 20.f;
        constexpr float throw_range = 400.f;
        // a rest in a shelter where it never rains (no cycle to wait out)
        constexpr int32_t dry_rest_ticks = 4800;

        dcon::room_id room_at(uint32_t index) noexcept { return dcon::room_id{dcon::room_id::value_base_t(index)}; }

        dcon::room_exit_id exit_by_node(sys::game_state const& state, dcon::room_id room, uint32_t node) {
            auto const& w = state.world;
            dcon::room_exit_id found;
            w.room_for_each_exit_location(room, [&](dcon::exit_location_id el) {
                auto const e = w.exit_location_get_exit(el);
                if (!found && uint32_t(w.room_exit_get_index(e)) == node)
                    found = e;
            });
            return found;
        }

        // the room an exit leads to, shelters and gates included, if its
        // pipe can be travelled back
        dcon::room_id leads_to(sys::game_state const& state, dcon::room_exit_id e, dcon::room_id from) {
            auto const& w = state.world;
            auto const target = w.room_exit_get_target_from_exit_connection(e);
            if (!target)
                return {};
            bool back = false;
            w.room_for_each_exit_location(target, [&](dcon::exit_location_id el) {
                back = back || w.room_exit_get_target_from_exit_connection(w.exit_location_get_exit(el)) == from;
            });
            return back ? target : dcon::room_id{};
        }

        // the node of `room`'s exit leading back to `from` (where a slugcat
        // coming from there comes out), -1 none
        int32_t back_node(sys::game_state const& state, dcon::room_id room, dcon::room_id from) {
            auto const& w = state.world;
            int32_t node = -1;
            w.room_for_each_exit_location(room, [&](dcon::exit_location_id el) {
                auto const e = w.exit_location_get_exit(el);
                if (node < 0 && w.room_exit_get_target_from_exit_connection(e) == from)
                    node = w.room_exit_get_index(e);
            });
            return node;
        }

        // where a slugcat stands under a warp (migrate::travel_tile heads
        // there too) or comes down from where one puts it
        ivec2 ground_under(room_view const& room, vec2 at) {
            ivec2 tile = room_view::get_tile_position(at);
            for (int32_t i = 0; i < 20 && tile.y > 0 && !room.get_tile(tile + ivec2{0, -1}).solid(); ++i)
                --tile.y;
            return tile;
        }

        // A room's goals for a slugcat: its exits' pipe entrances and the
        // ground under its warps, with their bits
        struct goal_tile {
            ivec2 tile;
            uint32_t bit;
        };
        // ShelterDoor's playerSpawnPos: from the shelter's exit pipe out
        // until the next tile is solid (going up: to the wall on the left
        // too), down to the floor; a ShelterSpawnPoint overrides it (not
        // parsed yet). Where the slugcats wake up and lie down to sleep
        std::optional<ivec2> spawn_spot(sys::game_state const& state, room_view const& room) {
            // (Custom.fourDirections' order; the last entrance in vanilla's
            // x then y scan wins)
            constexpr ivec2 four[4] = {{-1, 0}, {0, 1}, {1, 0}, {0, -1}};
            auto const solid = [&](ivec2 t) { return !room.inside(t.x, t.y) || room.get_tile(t).t == world::terrain::solid; };
            std::optional<ivec2> entrance;
            state.world.room_for_each_exit_location(room.id, [&](dcon::exit_location_id el) {
                auto const node = state.world.room_exit_get_index(state.world.exit_location_get_exit(el));
                if (auto const t = dens::exit_entrance(state, room.id, uint8_t(node)))
                    if (!entrance || t->x > entrance->x || (t->x == entrance->x && t->y > entrance->y))
                        entrance = *t;
            });
            if (!entrance)
                return std::nullopt;
            ivec2 at = *entrance;
            for (auto const d : four)
                if (!solid(*entrance + d)) {
                    for (int32_t k = 0; k < room.width + room.height && !solid(at + d); ++k)
                        at = at + d;
                    if (d.y == 1)
                        for (int32_t k = 0; k < room.width && !solid(at + ivec2{-1, 0}); ++k)
                            at.x -= 1;
                    for (int32_t k = 0; k < room.height && !solid(at + ivec2{0, -1}); ++k)
                        at.y -= 1;
                    break;
                }
            return at;
        }

        std::vector<goal_tile> goals_of(sys::game_state const& state, room_view const& room) {
            auto const& w = state.world;
            auto const& rr = state.slugcat_routes[size_t(room.id.index())];
            std::vector<goal_tile> out;
            w.room_for_each_exit_location(room.id, [&](dcon::exit_location_id el) {
                auto const e = w.exit_location_get_exit(el);
                uint32_t const node = w.room_exit_get_index(e);
                if (node >= routes::max_exits || !leads_to(state, e, room.id))
                    return;
                if (auto const t = dens::exit_entrance(state, room.id, uint8_t(node)))
                    out.push_back(goal_tile{*t, node});
            });
            for (uint32_t k = 0; k < routes::max_warps; ++k)
                if (rr.warp_to[k] != 0)
                    if (auto const wp = warps::point(state, room.id, k))
                        out.push_back(goal_tile{ground_under(room, warps::position(state, wp)), warp_bit + k});
            return out;
        }

        // the field reaches a slugcat at `t` (or just out of the pipe there)
        bool reaches(sys::game_state const& state, room_view const& room, pathing::field_id f, ivec2 t) {
            if (pathing::distance(state, f, t) != pathing::unreachable)
                return true;
            auto const& tile = room.get_tile(t);
            return tile.t == world::terrain::shortcut_entrance && pathing::distance(state, f, t + room.shortcut_entrance_hole_direction(t)) != pathing::unreachable;
        }

        // the goals a slugcat at `at` can get to
        uint64_t goals_from(sys::game_state& state, room_view const& room, ivec2 at) {
            uint64_t mask = 0;
            for (auto const& g : goals_of(state, room)) {
                auto const f = pathing::field_to(state, room.id, g.tile, pathing::slugcat);
                if (reaches(state, room, f, at))
                    mask |= uint64_t(1) << g.bit;
            }
            return mask;
        }

        //
        // The world level search over the rooms' exits and warps
        //

        struct search {
            // per state: the state it was reached from + 1 (0 not yet,
            // `from_start` the start), the hop (an exit's node, a warp's
            // migrate::warp_hop + k)
            static constexpr uint32_t from_start = 0xFFFFFFFFu;
            std::vector<uint32_t> prev;
            std::vector<uint8_t> via;
            // the states reached, in breadth first order
            std::vector<uint32_t> order;
        };

        // every exit and warp of the room that leads somewhere, whether a
        // slugcat can walk between them or not
        uint64_t all_exits(sys::game_state const& state, dcon::room_id r) {
            auto const& w = state.world;
            uint64_t mask = 0;
            w.room_for_each_exit_location(r, [&](dcon::exit_location_id el) {
                auto const e = w.exit_location_get_exit(el);
                uint32_t const node = w.room_exit_get_index(e);
                if (node < routes::max_exits && leads_to(state, e, r))
                    mask |= uint64_t(1) << node;
            });
            auto const& rr = state.slugcat_routes[size_t(r.index())];
            for (uint32_t k = 0; k < routes::max_warps; ++k)
                if (rr.warp_to[k] != 0)
                    mask |= uint64_t(1) << (warp_bit + k);
            return mask;
        }

        // (`any`: a guess for when the walkable way finds no shelter. Where
        // the walkable way goes on from how it got into a room, that; where
        // it doesn't, the room's connections alone, as if every exit could be
        // walked to from every other. A gap no jump closes (WRFA_E04) isn't
        // guessed across when the room's walkable way leads somewhere else,
        // a warp close by)
        // (`wide`: when even that finds none, every room taken as open but
        // the ones a guess already failed in: from WRFA_S08 the walkable way
        // out of WRFA_C11 leads only to WRFA_E04's gap, the one to WRFA_A21
        // and its warp is a move the path graph lacks)
        search& explore(sys::game_state const& state, dcon::room_id room, uint64_t start, bool any = false, nav_state const* known = nullptr, bool wide = false) {
            static thread_local search s;
            auto const& w = state.world;
            size_t const n = size_t(w.room_size()) * slots;
            if (s.prev.size() != n) {
                s.prev.assign(n, 0);
                s.via.assign(n, 0);
            }
            for (auto const st : s.order)
                s.prev[st] = 0;
            s.order.clear();
            auto const& all = state.slugcat_routes;
            auto const hop = [&](dcon::room_id r, uint32_t bit, uint32_t parent) {
                uint32_t next;
                uint8_t via;
                if (bit < warp_bit) {
                    auto const e = exit_by_node(state, r, bit);
                    auto const to = e ? leads_to(state, e, r) : dcon::room_id{};
                    if (!to)
                        return;
                    int32_t const node = back_node(state, to, r);
                    if (node < 0 || uint32_t(node) >= routes::max_exits)
                        return;
                    next = uint32_t(to.index()) * slots + uint32_t(node);
                    via = uint8_t(bit);
                } else {
                    uint32_t const k = bit - warp_bit;
                    uint32_t const wt = all[size_t(r.index())].warp_to[k];
                    if (wt == 0)
                        return;
                    next = ((wt >> 3) - 1u) * slots + arrival_slot + (wt & 7u);
                    via = uint8_t(migrate::warp_hop + k);
                }
                if (s.prev[next] != 0)
                    return;
                s.prev[next] = parent;
                s.via[next] = via;
                s.order.push_back(next);
            };
            for (uint32_t b = 0; b < 64; ++b)
                if (start & (uint64_t(1) << b))
                    hop(room, b, search::from_start);
            for (size_t qi = 0; qi < s.order.size(); ++qi) {
                uint32_t const st = s.order[qi];
                auto const r = room_at(st / slots);
                uint32_t const slot = st % slots;
                auto const& rr = all[size_t(r.index())];
                uint64_t const walkable = slot < routes::max_exits ? rr.from_exit[slot] : rr.from_arrival[slot - arrival_slot];
                // (nor the rooms a guess already failed in: `known`'s)
                bool tried = false;
                if (known)
                    for (auto const k : known->no_guess)
                        tried = tried || k == uint16_t(r.index() + 1);
                uint64_t const mask = any && (walkable == 0 || wide) && !tried ? all_exits(state, r) : walkable;
                for (uint32_t b = 0; b < 64; ++b)
                    if (mask & (uint64_t(1) << b))
                        hop(r, b, st + 1u);
            }
            return s;
        }

        // the shelters the search reached (not `here`), closest first
        std::vector<dcon::room_id> shelters_of(sys::game_state const& state, search const& s, dcon::room_id here) {
            std::vector<dcon::room_id> out;
            for (auto const st : s.order) {
                auto const r = room_at(st / slots);
                if (r != here && state.world.room_get_is_shelter(r) && std::find(out.begin(), out.end(), r) == out.end())
                    out.push_back(r);
            }
            return out;
        }

        // the creature's trip to `target` along the search (its first
        // hops: a longer way is planned again on the way)
        bool set_trip(sys::game_state& state, dcon::creature_id c, search const& s, dcon::room_id target) {
            uint32_t found = search::from_start;
            for (auto const st : s.order)
                if (room_at(st / slots) == target) {
                    found = st;
                    break;
                }
            if (found == search::from_start)
                return false;
            std::vector<std::pair<uint32_t, uint8_t>> hops;
            for (uint32_t st = found;;) {
                hops.emplace_back(st / slots, s.via[st]);
                uint32_t const p = s.prev[st];
                if (p == search::from_start)
                    break;
                st = p - 1u;
            }
            std::reverse(hops.begin(), hops.end());
            creature::trip t;
            for (auto const& [r, via] : hops) {
                if (t.hops >= creature::trip::max_hops)
                    break;
                t.room[t.hops] = uint16_t(r);
                t.exit[t.hops] = via;
                ++t.hops;
            }
            state.world.creature_set_trip(c, t);
            ++stats().trips;
            return true;
        }

        // how many rooms the search's way to `target` goes through (0 none)
        uint32_t hops_to(search const& s, dcon::room_id target) {
            for (auto const st : s.order)
                if (room_at(st / slots) == target) {
                    uint32_t n = 1;
                    for (uint32_t at = st; s.prev[at] != search::from_start; at = s.prev[at] - 1u)
                        ++n;
                    return n;
                }
            return 0;
        }

        //
        // The slugcat's own state
        //

        vec2 pos(sys::game_state const& state, dcon::player_id p, uint32_t i) {
            return state.world.body_chunk_get_pos(player::body_chunk(state, p, i));
        }
        ivec2 contact(sys::game_state const& state, dcon::player_id p, uint32_t i) {
            return state.world.body_chunk_get_contact_point(player::body_chunk(state, p, i));
        }

        // the tile it is at for its path: its feet's, else its head's, else
        // a walkable one next to its feet
        ivec2 tile_of(sys::game_state& state, dcon::player_id p, room_view const& room) {
            // holding a pole: where its hands are (at the top of the pole the
            // head stays a little under the top tile)
            if (state.world.player_get_body_mode(p) == player::body_mode_index::climbing_on_beam) {
                ivec2 const head = room_view::get_tile_position(pos(state, p, 0));
                if (room.get_tile(head).vertical_beam())
                    return head;
                ivec2 const hands = room_view::get_tile_position(pos(state, p, 0) + vec2{0.f, 8.f});
                if (room.get_tile(hands).vertical_beam())
                    return hands;
            }
            ivec2 const feet = room_view::get_tile_position(pos(state, p, 1));
            if (pathing::slugcat_walkable(state, room, feet))
                return feet;
            ivec2 const head = room_view::get_tile_position(pos(state, p, 0));
            if (pathing::slugcat_walkable(state, room, head))
                return head;
            for (auto const d : custom::eight_directions)
                if (pathing::slugcat_walkable(state, room, feet + d))
                    return feet + d;
            return feet;
        }

        bool airborne(sys::game_state const& state, dcon::player_id p) {
            auto const& w = state.world;
            auto const mode = w.player_get_body_mode(p);
            return mode == player::body_mode_index::default_ && contact(state, p, 0).y == 0 && contact(state, p, 1).y == 0
                && w.player_get_animation(p) == player::animation_index::none;
        }

        player::input_package input(int32_t x, int32_t y, bool jmp = false) {
            return player::make_input(float(x), float(y), jmp, false, false);
        }

        // What hunts it and is in sight, closest first (none: nothing)
        dcon::creature_id spot_threat(sys::game_state const& state, dcon::creature_id c, room_view const& room, vec2 head) {
            dcon::creature_id best;
            float best_d = threat_range;
            rooms::for_each_creature(state, room.id, [&](dcon::creature_id t) {
                if (t == c || creature::is_slugcat(state.world, t) || !creature::live(state, t) || state.world.creature_get_dead(t))
                    return;
                // (squidcadas and leeches aren't worth running from; a stunned
                // one can be run past)
                auto const kind = creature::type_of(state.world, t);
                if (kind == creature::type::cicada || kind == creature::type::leech || state.world.creature_get_stun(t) > 0)
                    return;
                // it would eat or kill a slugcat (its own dynamic relationship)
                auto const r = tracker::dynamic_relationship(state, t, c);
                if (!r.go_for_kill() || r.intensity < 0.05f)
                    return;
                vec2 const at = creature::main_pos(state, t);
                float const d = rw::distance(at, head);
                if (d < best_d && room.visual_contact(head, at)) {
                    best = t;
                    best_d = d;
                }
            });
            return best;
        }

        // The pipe to run into from a threat at `from`: one it is closer to
        // than the threat, away from it
        std::optional<ivec2> flee_pipe(sys::game_state& state, room_view const& room, vec2 me, ivec2 at, vec2 from) {
            auto const& w = state.world;
            struct option {
                ivec2 tile;
                float score;
            };
            std::vector<option> options;
            w.room_for_each_shortcut_location(room.id, [&](dcon::shortcut_location_id sl) {
                auto const sc = w.shortcut_location_get_shortcut(sl);
                auto const type = w.shortcut_get_type(sc);
                if (type != world::shortcut_type::room_exit && type != world::shortcut_type::normal)
                    return;
                ivec2 const t = w.shortcut_get_start(sc);
                vec2 const m = room_view::middle_of_tile(t);
                float const mine = rw::distance(m, me);
                float const theirs = rw::distance(m, from);
                if (mine * 1.2f + 20.f < theirs)
                    options.push_back(option{t, theirs - mine});
            });
            std::sort(options.begin(), options.end(), [](option const& a, option const& b) { return a.score > b.score; });
            for (size_t i = 0; i < options.size() && i < 3; ++i) {
                auto const f = pathing::field_to(state, room.id, options[i].tile, pathing::slugcat);
                if (pathing::distance(state, f, at) != pathing::unreachable)
                    return options[i].tile;
            }
            return std::nullopt;
        }

        // a thing to throw at a threat
        bool weapon(sys::game_state const& state, dcon::item_id it) {
            return it && state.world.item_get_type(it) <= world::item_type::lilly_puck;
        }

        struct brain {
            sys::game_state& state;
            dcon::player_id p;
            dcon::slugcat_npc_id n;
            dcon::creature_id c;
            room_view const& room;
            nav_state& nav;
            rw::rng& rng;
            ivec2 at;

            auto& w() { return state.world; }

            void begin(move m, ivec2 aim, int32_t dir) {
                nav.how = m;
                nav.move_timer = 0;
                nav.aim = aim;
                nav.from = at;
                nav.dir = int8_t(dir);
                nav.hold_jump = 0;
                nav.cool = 0;
                if (m == move::rise)
                    ++stats().rises;
                if (m == move::crouch)
                    ++stats().pounces;
                if (m == move::back_up) {
                    ++stats().leaps;
                    // already running that way: no need to back up
                    if (w().body_chunk_get_vel(player::body_chunk(state, p, 1)).x * float(dir) > 3.f)
                        nav.how = move::run;
                }
            }

            // the move didn't get there: no progress for a while, and
            // that move from there is off the menu for now
            void failed(int32_t penalty) {
                nav.stuck += penalty;
                nav.blocked[nav.blocked_next][0] = nav.from;
                nav.blocked[nav.blocked_next][1] = nav.aim;
                nav.blocked_next = uint8_t((nav.blocked_next + 1) % max_blocked);
            }

            void to(move m) {
                nav.how = m;
                nav.move_timer = 0;
            }

            // a move just begun: its first tick's input (it may be over at
            // once, failed: then nothing this tick)
            player::input_package started() {
                return keep_moving().value_or(input(0, 0));
            }

            // a move under way: this tick's input, none when it is over
            std::optional<player::input_package> keep_moving() {
                int32_t const t = nav.move_timer++;
                int32_t const dir = nav.dir;
                auto const mode = w().player_get_body_mode(p);
                switch (nav.how) {
                case move::walk:
                    return std::nullopt;
                case move::rise: {
                    // up there (or hanging on the pole it jumped for): done
                    // (for a pole or a beam: once it holds on)
                    bool const to_beam = room.get_tile(nav.aim).any_beam();
                    if (t > 4 && !to_beam && (at == nav.aim || (at.y >= nav.aim.y && pathing::standable(room, at))))
                        break;
                    if (t > 8 && room.get_tile(nav.aim).any_beam() && mode == player::body_mode_index::climbing_on_beam && at.y >= nav.aim.y - 1)
                        break;
                    if (t > 80) {
                        failed(60);
                        break;
                    }
                    // where the jump leaves from first: under the target going
                    // straight up, else the column it rises in (the next one may be
                    // under a ceiling)
                    bool const on_beam_top = w().player_get_animation(p) == player::animation_index::stand_on_beam;
                    // (from up on a beam: not holding up on the way, it would
                    // catch the beam again; before lining up, which holds the
                    // move's clock at its start)
                    if (t == 0 || (t < 8 && on_beam_top))
                        nav.hold_jump = on_beam_top ? 1 : nav.hold_jump;
                    if (t < 8 && (on_beam_top || (mode != player::body_mode_index::climbing_on_beam && mode != player::body_mode_index::corridor_climb))) {
                        int32_t const feet_x = room_view::get_tile_position(pos(state, p, 1)).x;
                        int32_t const column = dir == 0 ? nav.aim.x : nav.from.x;
                        // (two or more up beside it: the far side of the column,
                        // clear of the ledge's corner on the way up; straight up,
                        // the middle of it: a one tile shaft takes no less)
                        float const off = dir != 0 && nav.aim.y - nav.from.y >= 2
                            ? pos(state, p, 1).x - (room_view::middle_of_tile(nav.from).x - 4.f * float(dir))
                            : dir == 0 ? pos(state, p, 1).x - room_view::middle_of_tile(nav.aim).x : 0.f;
                        float const slack = dir == 0 ? 2.f : 3.f;
                        if ((feet_x != column || std::abs(off) > slack) && t < 6) {
                            if (feet_x == column) {
                                nav.move_timer = 1;
                                if (++nav.cool > 40)
                                    nav.move_timer = 6;
                                // (in nudges: held, it slides past the spot)
                                return input(nav.cool % 2 == 0 ? 0 : off > 0.f ? -1 : 1, 0);
                            }
                            nav.move_timer = 1;
                            if (++nav.cool > 40) {
                                failed(30);
                                break;
                            }
                            return input(rw::math_sign(column - feet_x), 0);
                        }
                    }
                    // off the wall (jumping from it would push it away), stand up,
                    // then jump towards the ledge and hold on
                    if (t < 8) {
                        bool const at_wall = mode == player::body_mode_index::wall_climb || contact(state, p, 0).x == dir || contact(state, p, 1).x == dir;
                        if (dir != 0 && at_wall)
                            return input(-dir, 0);
                        // (only up when not standing yet: by a pole it would grab it)
                        return input(0, nav.hold_jump || mode == player::body_mode_index::stand ? 0 : 1);
                    }
                    // back on the ground and walking off along it: the jump fell short
                    if (t > 16 && at.y <= nav.from.y && std::abs(at.x - nav.from.x) >= 3 && mode == player::body_mode_index::stand) {
                        failed(60);
                        break;
                    }
                    // (like a player's CC_B08 ledge: a tick towards it, then jump
                    // without up, up later to climb on)
                    // (turned that way first: a jump in the middle of the turn
                    // hardly leaves the ground)
                    if (t == 8 && dir != 0) {
                        if (w().player_get_flip_direction(p) != dir && ++nav.cool < 20)
                            nav.move_timer = 8;
                        return input(dir, 0);
                    }
                    // (two or more up: straight up its column first, then into
                    // the ledge's corner for the grab; leaning over at once it
                    // bumps the ceiling beside the column)
                    bool const high = nav.aim.y - nav.from.y >= 2;
                    // (for a pole: up once the hands are by it, to catch it)
                    bool const by_pole = room.get_tile(nav.aim).vertical_beam() && std::abs(pos(state, p, 0).x - room_view::middle_of_tile(nav.aim).x) < 20.f;
                    return input(high && t < 15 && !by_pole ? 0 : dir, t < 22 && !by_pole ? 0 : 1, t < 24 && t > 8);
                }
                case move::crouch: {
                    if (t > 60) {
                        failed(60);
                        break;
                    }
                    // (still coming up out of a pipe or a corridor below: up
                    // onto the tile first, down there it would go back in)
                    ivec2 const feet = room_view::get_tile_position(pos(state, p, 1));
                    bool const on_pole = mode == player::body_mode_index::climbing_on_beam;
                    if ((mode == player::body_mode_index::corridor_climb || on_pole) && feet.y < nav.from.y)
                        return input(0, 1);
                    // (on a pole, or standing where one goes through: down there
                    // climbs it; off it towards the gap, crouching from beside it)
                    if (on_pole)
                        return input(dir, 0);
                    if (mode != player::body_mode_index::crawl && room.get_tile(feet).vertical_beam() && feet.x == nav.from.x)
                        return input(dir, 0);
                    if (mode != player::body_mode_index::crawl)
                        return input(0, -1);
                    // facing the gap, both ends on the ground
                    float const facing = (pos(state, p, 0).x - pos(state, p, 1).x) * float(dir);
                    if (facing < 4.f)
                        return input(dir, 0);
                    if (contact(state, p, 0).y < 0 && contact(state, p, 1).y < 0) {
                        nav.how = move::charge;
                        nav.move_timer = 0;
                    }
                    return input(0, 0);
                }
                case move::charge: {
                    if (t > 40 || mode != player::body_mode_index::crawl)
                        break;
                    if (w().player_get_super_launch_jump(p) >= 20) {
                        to(move::flight);
                        return input(dir, 0);
                    }
                    return input(0, 0, true);
                }
                case move::back_up: {
                    // two tiles back from the edge, then the run up
                    if (t > 60 || (at.x - (nav.from.x - 2 * dir)) * dir <= 0) {
                        to(move::run);
                        return input(dir, 0);
                    }
                    return input(-dir, mode == player::body_mode_index::crawl ? 1 : 0);
                }
                case move::run: {
                    if (t > 80) {
                        failed(60);
                        break;
                    }
                    // at the edge: jump, and hold it
                    float const past = (pos(state, p, 1).x - room_view::middle_of_tile(nav.from).x) * float(dir);
                    if (past > -6.f) {
                        to(move::flight);
                        nav.hold_jump = 1;
                        return input(dir, 0, true);
                    }
                    return input(dir, mode == player::body_mode_index::crawl ? 1 : 0);
                }
                case move::swing: {
                    bool const on_pole = w().player_get_animation(p) == player::animation_index::climb_on_beam;
                    // got hold of the other pole, or of a beam right by it (or
                    // landed somewhere)
                    ivec2 const head = room_view::get_tile_position(pos(state, p, 0));
                    if (t > 8 && on_pole && head.x == nav.aim.x)
                        break;
                    if (t > 8 && nav.cool >= 2 && mode == player::body_mode_index::climbing_on_beam && std::abs(head.x - nav.aim.x) <= 1
                        && std::abs(head.y - nav.aim.y) <= 2)
                        break;
                    if (t > 90 || (t > 4 && contact(state, p, 1).y < 0)) {
                        failed(30);
                        break;
                    }
                    // onto the pole first, then off it towards the other one
                    // (until it jumps: holding the pole, else back onto it)
                    if (!on_pole && nav.cool < 2) {
                        nav.cool = 0;
                        nav.move_timer = 1;
                        if (++nav.wall_side > 60) {
                            failed(60);
                            break;
                        }
                        // (out of a corridor, a pipe's mouth: down onto the pole)
                        return input(0, mode == player::body_mode_index::corridor_climb ? -1 : 1);
                    }
                    // facing the way first (it jumps off the side it faces)
                    // (and holding the direction a few ticks before jumping, like a
                    // player: pressed together, the pole may take it for a step
                    // onto a beam)
                    // (at the pole's top that hold lets go of it, where a beam
                    // crosses it hangs from the beam: no hold there)
                    ivec2 const ht = room_view::get_tile_position(pos(state, p, 0));
                    bool const top = !room.get_tile(ht + ivec2{0, 1}).vertical_beam() || room.get_tile(ht).horizontal_beam()
                        || room.get_tile(room_view::get_tile_position(pos(state, p, 1))).horizontal_beam();
                    if (nav.cool < 2 && (w().player_get_flip_direction(p) != dir || (nav.wall_side < 4 && !top)) && t < 12) {
                        nav.cool = 1;
                        // (as a player did it in CC_B08: up and towards it, then
                        // towards it, then jump)
                        return input(dir, nav.wall_side++ < 2 && !top ? 1 : 0);
                    }
                    // off it: (onto the ground: the rest is a flight)
                    if (!room.get_tile(nav.aim).vertical_beam()) {
                        to(move::flight);
                        nav.hold_jump = 1;
                        return input(dir, 0, true);
                    }
                    if (nav.cool < 2) {
                        nav.cool = 2;
                        nav.move_timer = 1;
                        return input(dir, 0, true);
                    }
                    // (up only well off the pole: a jump still buffered with up
                    // held shimmies up the pole instead; a player's CC_B08 catch
                    // holds jump 12 ticks and reaches up about 25 ticks in)
                    bool const off = mode != player::body_mode_index::climbing_on_beam;
                    bool const near = std::abs(room_view::get_tile_position(pos(state, p, 0)).x - nav.aim.x) <= 1;
                    return input(dir, off && (near || t > 18) ? 1 : 0, t < 13);
                }
                case move::tip: {
                    // standing on the tip: jump
                    if (w().player_get_animation(p) == player::animation_index::beam_tip && t > 2) {
                        to(move::flight);
                        nav.hold_jump = 1;
                        return input(dir, 0, true);
                    }
                    if (t > 150) {
                        failed(60);
                        break;
                    }
                    return input(0, 1);
                }
                case move::bounce: {
                    auto const anim = w().player_get_animation(p);
                    bool const holding = mode == player::body_mode_index::climbing_on_beam && anim != player::animation_index::beam_tip;
                    // caught the pole up there
                    if (nav.wall_side >= 3 && holding && pos(state, p, 0).y > room_view::middle_of_tile(nav.from).y + 40.f)
                        break;
                    if (t > 160) {
                        failed(60);
                        break;
                    }
                    switch (nav.wall_side) {
                    case 0:
                        // up onto the tip
                        if (anim == player::animation_index::beam_tip && t > 2) {
                            nav.wall_side = 1;
                            nav.cool = 0;
                            return input(dir, 0, true);
                        }
                        return input(0, 1);
                    case 1:
                        // towards the wall
                        if (++nav.cool > 4 && (mode == player::body_mode_index::wall_climb || contact(state, p, 0).x == dir)) {
                            nav.wall_side = 2;
                            nav.cool = 0;
                        }
                        return input(dir, 0, nav.cool < 20);
                    case 2:
                        // into it with jump let go, then off it the other way
                        if (++nav.cool < 3)
                            return input(dir, 0);
                        nav.wall_side = 3;
                        nav.cool = 0;
                        return input(-dir, 1, true);
                    default:
                        ++nav.cool;
                        return input(-dir, 1, nav.cool < 10);
                    }
                }
                case move::flight: {
                    // a ledge caught: up onto it (and no more jumping)
                    if (w().player_get_animation(p) == player::animation_index::ledge_crawl)
                        nav.hold_jump = 0;
                    if (w().player_get_animation(p) == player::animation_index::ledge_grab) {
                        nav.hold_jump = 0;
                        return input(dir, 1);
                    }
                    // landed (or caught a pole, in the water, in a corridor);
                    // against a wall it keeps reaching for the ledge
                    bool const landed = t > 4
                        && (contact(state, p, 1).y < 0 || mode == player::body_mode_index::climbing_on_beam || mode == player::body_mode_index::swimming
                            || mode == player::body_mode_index::corridor_climb);
                    if (landed || t > 90)
                        break;
                    // reaching for a pole, or for the ledge at the end
                    bool const beam = room.get_tile(nav.aim).any_beam() || room.get_tile(nav.aim - ivec2{0, 1}).horizontal_beam();
                    bool const reach = beam ? t > 3 : t > 15;
                    return input(dir, reach ? 1 : 0, nav.hold_jump != 0 && t < 20);
                }
                case move::wall: {
                    // up there: catch hold
                    int32_t const y = room_view::get_tile_position(pos(state, p, 1)).y;
                    if (t > 240) {
                        failed(100);
                        break;
                    }
                    if (y >= nav.aim.y && t > 4) {
                        if (!airborne(state, p))
                            break;
                        return input(rw::math_sign(nav.aim.x - room_view::get_tile_position(pos(state, p, 1)).x), 1);
                    }
                    // against a wall in the air: jump off it towards the other one
                    // (from under the shaft: straight up, the first wall it meets)
                    int32_t& side = nav.wall_side;
                    if (side == 0 && dir != 0)
                        side = dir;
                    // (under the shaft: straight up into it holding jump, then
                    // against a wall, the first it touches or the left one)
                    if (side == 0) {
                        int32_t const touch = contact(state, p, 0).x != 0 ? contact(state, p, 0).x : contact(state, p, 1).x;
                        if (touch != 0 && t > 4) {
                            side = touch;
                            nav.cool = 0;
                        } else if (t < 18) {
                            return input(0, 0, true);
                        } else {
                            side = -1;
                            nav.cool = 0;
                        }
                    }
                    bool const on_wall = mode == player::body_mode_index::wall_climb || contact(state, p, 0).x == side || contact(state, p, 1).x == side;
                    if (nav.cool > 0)
                        --nav.cool;
                    if (on_wall && nav.cool == 0) {
                        side = -side;
                        nav.cool = 4;
                        return input(side, t > 12 || y >= nav.aim.y - 2 ? 1 : 0, true);
                    }
                    // off a pole (facing the way first: it jumps off the side
                    // it faces), a beam or the ground towards the first wall
                    auto const anim = w().player_get_animation(p);
                    bool const on_pole = mode == player::body_mode_index::climbing_on_beam && anim != player::animation_index::stand_on_beam;
                    if ((on_pole || !airborne(state, p)) && nav.cool == 0) {
                        if (on_pole && nav.hold_jump < 6) {
                            ++nav.hold_jump;
                            return input(side, 0);
                        }
                        nav.hold_jump = 0;
                        nav.cool = 4;
                        return input(side, 0, true);
                    }
                    // (reaching up for a hold, the beams it passes included, but
                    // not the one it just left)
                    return input(side, t > 12 || y >= nav.aim.y - 2 ? 1 : 0, nav.cool > 0);
                }
                }
                nav.how = move::walk;
                nav.move_timer = 0;
                return std::nullopt;
            }

            // towards `goal` along the slugcat field; `push`: then into the
            // pipe there
            player::input_package go(ivec2 goal, bool push) {
                if (auto const in = keep_moving())
                    return *in;
                if (nav.goal_tile != goal) {
                    nav.goal_tile = goal;
                    nav.best = pathing::unreachable;
                    nav.stuck = 0;
                }
                auto const& hist = w().player_get_input(p);
                // hanging from a ledge: up onto it; under a pole: up onto it
                if (w().player_get_animation(p) == player::animation_index::ledge_grab)
                    return input(w().player_get_flip_direction(p), 1);
                if (w().player_get_animation(p) == player::animation_index::hang_under_vertical_beam)
                    return input(0, 1);
                // a pipe whose mouth is a beam under the rock (no room to
                // stand there): off the beam it stands on, catch it hanging,
                // and in along it
                // (any pipe whose mouth is a horizontal beam, it goes in along the
                // beam hanging from it: standing on it, it would push at the rock
                // in the corner)
                ivec2 const mouth = push ? goal + room.shortcut_entrance_hole_direction(goal) : ivec2{-1, -1};
                bool const low_mouth = push && room.get_tile(mouth).horizontal_beam() && room.shortcut_entrance_hole_direction(goal).y == 0
                    && custom::dist_less(pos(state, p, 0), room_view::middle_of_tile(mouth), 80.f);
                if (low_mouth) {
                    ivec2 const in_dir = ivec2{0, 0} - room.shortcut_entrance_hole_direction(goal);
                    auto const anim = w().player_get_animation(p);
                    if (anim == player::animation_index::stand_on_beam)
                        return input(0, hist[0].y == -1 ? 0 : -1);
                    if (airborne(state, p))
                        return input(in_dir.x, 1);
                }
                // in the air: keep going the way it was, grabbing a pole on
                // the way
                if (airborne(state, p)) {
                    auto const f = pathing::field_to(state, room.id, goal, pathing::slugcat);
                    bool grab = room.get_tile(at).vertical_beam() && pathing::distance(state, f, at) <= nav.best;
                    // (the way on is up: reaching for whatever is there, a
                    // platform, a beam, a ledge)
                    if (auto const st = pathing::slugcat_step(state, f, at); st && st->kind == pathing::step_kind::walk && st->tile.y > at.y)
                        return input(st->tile.x - at.x, 1);
                    return input(hist[0].x, grab ? 1 : 0);
                }
                if (push) {
                    ivec2 const hole = room.shortcut_entrance_hole_direction(goal);
                    ivec2 const head = room_view::get_tile_position(pos(state, p, 0));
                    for (ivec2 const t : {at, head})
                        if (t == goal || t == goal + hole)
                            return input(-hole.x, -hole.y);
                }
                auto const f = pathing::field_to(state, room.id, goal, pathing::slugcat);
                // the end of its body further along the way (its head
                // against a wall its feet haven't reached)
                ivec2 const head_tile = room_view::get_tile_position(pos(state, p, 0));
                if (head_tile != at && pathing::slugcat_walkable(state, room, head_tile) && pathing::distance(state, f, head_tile) < pathing::distance(state, f, at))
                    at = head_tile;
                uint16_t const d = pathing::distance(state, f, at);
                if (d < nav.best) {
                    nav.best = d;
                    nav.stuck = 0;
                } else {
                    ++nav.stuck;
                    // no way on for a while: the moves that failed get another go
                    if (nav.stuck % 200 == 0)
                        for (auto& e : nav.blocked)
                            e[0] = e[1] = ivec2{-1, -1};
                }
                // stuck: a hop in some direction now and then
                if (nav.stuck > 150 && nav.stuck % 80 < 12)
                    return input(nav.stuck % 160 < 80 ? -1 : 1, 1, nav.stuck % 80 < 6);
                auto const st = pathing::slugcat_step(state, f, at, [&](ivec2 a, ivec2 b) {
                    for (auto const& e : nav.blocked)
                        if (e[0] == a && e[1] == b)
                            return true;
                    return false;
                });
                // no walkable way it knows of on a guessed trip (a move the
                // path graph lacks): make for it, jumping now and then
                // (ledges, walls, poles to catch)
                if (!st) {
                    if (d != pathing::unreachable || !nav.guessed)
                        return input(0, 0);
                    ivec2 const to = goal - at;
                    int32_t const y = to.y > 1 ? 1 : to.y < -1 && std::abs(to.x) < 3 ? -1 : 0;
                    int32_t const x = rw::math_sign(to.x);
                    // (never towards a column without something to stand on a
                    // short drop down: off that edge it falls into the pit;
                    // jumping only at a wall or a ledge in front)
                    ivec2 const ahead = at + ivec2{x, 0};
                    bool const wall = x != 0 && room.get_tile(ahead).solid();
                    bool floor_ahead = x == 0 || wall;
                    for (int32_t k = 0; k <= safe_drop && !floor_ahead; ++k) {
                        auto const t = room.get_tile(ahead - ivec2{0, k});
                        floor_ahead = pathing::standable(room, ahead - ivec2{0, k}) || t.any_beam() || t.water != 0;
                    }
                    if (!floor_ahead)
                        return input(0, y);
                    return input(x, wall ? y : std::min(y, 0), wall && nav.stuck % 40 < 8);
                }
                nav.last_step = st->tile;
                nav.last_kind = uint8_t(st->kind);
                ivec2 const dd = st->tile - at;
                auto const mode = w().player_get_body_mode(p);
                auto const anim = w().player_get_animation(p);
                bool const on_pole = mode == player::body_mode_index::climbing_on_beam;
                bool const swimming = mode == player::body_mode_index::swimming;
                bool const corridor = mode == player::body_mode_index::corridor_climb;
                switch (st->kind) {
                case pathing::step_kind::rise:
                    // (holding a pole: from its tip; up on a horizontal beam
                    // it is a plain jump up)
                    begin(on_pole && room.get_tile(at).vertical_beam() && anim != player::animation_index::stand_on_beam
                                && anim != player::animation_index::hang_from_beam && anim != player::animation_index::get_up_on_beam && st->dir == 0
                            ? move::tip
                            : move::rise,
                        st->tile, st->dir);
                    nav.cool = 0;
                    return started();
                case pathing::step_kind::pounce:
                    begin(move::crouch, st->tile, st->dir);
                    return started();
                case pathing::step_kind::leap:
                    begin(move::back_up, st->tile, st->dir);
                    return started();
                case pathing::step_kind::swing:
                    // holding the pole first (out of a pipe's mouth, onto it)
                    if (!on_pole)
                        return input(0, corridor ? -1 : 1);
                    begin(move::swing, st->tile, st->dir);
                    nav.wall_side = 0;
                    nav.cool = 0;
                    return started();
                case pathing::step_kind::tip:
                    begin(move::tip, st->tile, st->dir);
                    return started();
                case pathing::step_kind::bounce:
                    begin(move::bounce, st->tile, st->dir);
                    nav.wall_side = 0;
                    nav.cool = 0;
                    return started();
                case pathing::step_kind::wall:
                    begin(move::wall, st->tile, st->dir);
                    nav.wall_side = 0;
                    nav.cool = 0;
                    return started();
                case pathing::step_kind::pipe: {
                    // in front of a pipe within the room: into it
                    for (auto const d : custom::four_directions)
                        if (room.get_tile(at + d).t == world::terrain::shortcut_entrance && room.shortcut_entrance_hole_direction(at + d) == ivec2{-d.x, -d.y})
                            return input(d.x, d.y);
                    return input(0, 0);
                }
                case pathing::step_kind::drop:
                    // over the edge, or let go of the pole
                    if (st->dir != 0 && !on_pole)
                        return input(st->dir, 0);
                    return input(0, -1);
                case pathing::step_kind::walk:
                    break;
                }
                // hanging from a horizontal beam, up onto it: only a fresh press
                // of up does it (Player.cs: input[1].y == 0), held from the jump
                // that caught the beam it hangs there for good
                if (anim == player::animation_index::hang_from_beam && dd.y > 0)
                    return input(0, hist[0].y > 0 ? 0 : 1);
                if (swimming || corridor)
                    return input(dd.x, dd.y);
                if (dd.y > 0) {
                    // a pole or a beam above: grab it and climb
                    if (room.get_tile(st->tile).any_beam() || on_pole)
                        return input(on_pole ? 0 : dd.x, 1);
                    begin(move::rise, st->tile, dd.x);
                    return started();
                }
                // down: on a pole only down (sideways too it would wall climb)
                if (dd.y < 0)
                    return input(on_pole ? 0 : dd.x, -1);
                // off a pole onto a horizontal beam beside it: its feet level
                // with the beam first (with its head there it would hang)
                if (on_pole && dd.x != 0 && room.get_tile(st->tile - ivec2{0, 1}).horizontal_beam()) {
                    int32_t const feet = room_view::get_tile_position(pos(state, p, 1)).y;
                    int32_t const beam = st->tile.y - 1;
                    if (feet != beam)
                        return input(0, feet < beam ? 1 : -1);
                    return input(dd.x, 0);
                }
                // along a vertical pole there is no walking off: jump off it
                if (on_pole && anim == player::animation_index::climb_on_beam && dd.x != 0)
                    return input(dd.x, 0, !hist[0].jmp);
                // up on its feet where there is room (it runs faster than it crawls)
                if (mode == player::body_mode_index::crawl && pathing::standable(room, at) && !room.get_tile(at + ivec2{0, 1}).solid()
                    && !room.get_tile(at + ivec2{0, 1}).any_beam() && !room.get_tile(at + ivec2{dd.x, 1}).solid())
                    return input(dd.x, 1);
                return input(dd.x, 0);
            }

            // the wide guess starts from every exit of the room, but in a room
            // a guess already failed in only from those it can walk to
            uint64_t wide_start() {
                uint16_t const here = uint16_t(room.id.index() + 1);
                if (std::find(std::begin(nav.no_guess), std::end(nav.no_guess), here) != std::end(nav.no_guess))
                    return goals_from(state, room, at);
                return all_exits(state, room.id);
            }

            // a guessed search starts from the exits it can walk to, from all
            // of the room's when it can walk to none
            uint64_t guess_start() {
                uint64_t const walk = goals_from(state, room, at);
                return walk != 0 ? walk : all_exits(state, room.id);
            }

            // the trip to the target shelter from where it stands
            bool plan() {
                auto const target = w().slugcat_npc_get_target(n);
                if (!target || target == room.id)
                    return false;
                nav.guessed = 0;
                if (set_trip(state, c, explore(state, room.id, goals_from(state, room, at)), target))
                    return true;
                // (no walkable way: the rooms' connections, to try)
                nav.guessed = 1;
                if (set_trip(state, c, explore(state, room.id, guess_start(), true, &nav), target))
                    return true;
                return set_trip(state, c, explore(state, room.id, wide_start(), true, &nav, true), target);
            }

            // a target shelter it can get to, the closest or any
            // (none it knows a walkable way to: one the rooms' connections
            // lead to, to try anyway rather than stay; the one it gave up on
            // last only when there is no other)
            // (any: not back to the shelter it set off from; with no other it
            // knows a walkable way to, through the closest warp it can walk
            // to, picking again on the other side; then a guessed one; home
            // last)
            bool pick_target(bool closest) {
                auto const home = w().slugcat_npc_get_home(n);
                auto const* sp = &explore(state, room.id, goals_from(state, room, at));
                auto options = shelters_of(state, *sp, room.id);
                auto const home_only = options;
                if (!closest)
                    std::erase_if(options, [&](dcon::room_id r) { return r == home; });
                nav.guessed = options.empty() ? 1 : 0;
                if (options.empty() && !closest)
                    for (auto const st : sp->order)
                        if (sp->via[st] >= migrate::warp_hop) {
                            auto const beyond = room_at(st / slots);
                            w().slugcat_npc_set_target(n, beyond);
                            nav.route_hops = 0;
                            nav.guessed = 0;
                            ++stats().diverted;
                            return set_trip(state, c, *sp, beyond);
                        }
                if (options.empty()) {
                    sp = &explore(state, room.id, guess_start(), true, &nav);
                    options = shelters_of(state, *sp, room.id);
                    if (std::erase_if(options, [&](dcon::room_id r) { return !closest && r == home; }), options.empty()) {
                        sp = &explore(state, room.id, wide_start(), true, &nav, true);
                        options = shelters_of(state, *sp, room.id);
                    }
                    if (!closest && options.size() > 1)
                        std::erase_if(options, [&](dcon::room_id r) { return r == home; });
                }
                if (options.empty() && !home_only.empty()) {
                    sp = &explore(state, room.id, goals_from(state, room, at));
                    options = home_only;
                    nav.guessed = 0;
                }
                auto const& s = *sp;
                if (options.size() > 1 && nav.given_up != 0)
                    std::erase_if(options, [&](dcon::room_id r) { return uint32_t(r.index()) + 1u == nav.given_up; });
                if (options.empty())
                    return false;
                auto const t = closest ? options.front() : options[size_t(rng.range(0, int32_t(options.size())))];
                w().slugcat_npc_set_target(n, t);
                nav.route_hops = uint8_t(std::min(255u, hops_to(s, t)));
                return set_trip(state, c, s, t);
            }

            // The failsafe against the rain: by its pack's share of the cycle,
            // less than half way to the target, back to the shelter it
            // started in (else the closest one). Once a cycle, the leader's
            void turn_back() {
                auto const target = w().slugcat_npc_get_target(n);
                auto const home = w().slugcat_npc_get_home(n);
                auto const* rc = rain::of_room(state, room.id);
                // (where its region rains, a shelter included)
                auto const region = w().room_get_rain_region(room.id);
                if (!target || !home || target == home || nav.route_hops == 0 || !rc || !region || w().region_get_never_rains(region) || rc->cycle_length <= 0)
                    return;
                if (nav.turn_cycle == rc->cycles + 1 || rc->timer * 100 < int32_t(nav.turn_back) * rc->cycle_length)
                    return;
                nav.turn_cycle = rc->cycles + 1;
                auto const& s = explore(state, room.id, goals_from(state, room, at));
                uint32_t const left = hops_to(s, target);
                // (no way on from here counts as none of the way done)
                if (left != 0 && 2 * (nav.route_hops - std::min<uint32_t>(left, nav.route_hops)) >= nav.route_hops)
                    return;
                ++stats().turned_back;
                if (room.id == home || hops_to(s, home) != 0) {
                    w().slugcat_npc_set_target(n, home);
                    nav.route_hops = uint8_t(std::min(255u, hops_to(s, home)));
                    migrate::cancel(state, c);
                    if (room.id != home)
                        set_trip(state, c, s, home);
                } else {
                    pick_target(true);
                }
            }

            bool leads() {
                auto const l = w().slugcat_npc_get_leader(n);
                return !l || l == n;
            }

            // the pack members in this room (not itself)
            template<typename F>
            void for_each_mate(F&& f) {
                auto const l = w().slugcat_npc_get_leader(n);
                if (!l)
                    return;
                rooms::for_each_player(state, room.id, [&](dcon::player_id o) {
                    auto const on = w().player_get_npc(o);
                    if (o != p && on && w().slugcat_npc_get_leader(on) == l && !player::dead(state, o))
                        f(o, on);
                });
            }

            // the leader waits at the pipe for the rest of the pack in the room
            bool wait_for_pack(ivec2 entrance) {
                if (!leads() || !custom::dist_less(pos(state, p, 1), room_view::middle_of_tile(entrance), 70.f))
                    return false;
                bool behind = false;
                for_each_mate([&](dcon::player_id o, dcon::slugcat_npc_id) {
                    behind = behind || (!player::in_shortcut(state, o) && !custom::dist_less(pos(state, o, 1), room_view::middle_of_tile(entrance), 140.f));
                });
                if (!behind || nav.wait > 400)
                    return false;
                ++nav.wait;
                return true;
            }

            void see_threats() {
                vec2 const head = pos(state, p, 0);
                if (auto const t = spot_threat(state, c, room, head)) {
                    if (nav.threat != t || nav.threat_timer > forget_threat)
                        nav.flee_tile = ivec2{-1, -1};
                    nav.threat = t;
                    nav.threat_pos = creature::main_pos(state, t);
                    nav.threat_timer = 0;
                } else if (spearmaster() && nav.threat && creature::here(state, nav.threat, room.id) && !creature::dead(state, nav.threat)
                    && room.visual_contact(head, creature::main_pos(state, nav.threat))) {
                    // (Spearmaster keeps after one it stunned, till it's dead)
                    nav.threat_pos = creature::main_pos(state, nav.threat);
                    nav.threat_timer = 0;
                } else {
                    ++nav.threat_timer;
                }
                // the pack tells each other what it sees (YellowAI)
                if (nav.threat && nav.threat_timer == 0)
                    return;
                for_each_mate([&](dcon::player_id, dcon::slugcat_npc_id on) {
                    auto const& o = w().slugcat_npc_get_nav(on);
                    if (o.threat && o.threat_timer == 0 && creature::here(state, o.threat, room.id)
                        && (!nav.threat || nav.threat_timer > 0)) {
                        nav.threat = o.threat;
                        nav.threat_pos = o.threat_pos;
                        nav.threat_timer = 0;
                    }
                });
                if (nav.threat && (nav.threat_timer > forget_threat || !creature::here(state, nav.threat, room.id) || creature::dead(state, nav.threat))) {
                    nav.threat = {};
                    nav.flee_tile = ivec2{-1, -1};
                }
            }

            bool spearmaster() { return w().player_get_stats(p).name == player::slugcat_name::spear; }

            // Spearmaster goes for the threat instead of running: up to where
            // it is level with it and in range, then a needle thrown (arm()
            // throws what is in hand) or, hands empty, pick up held for a new
            // one out of its tail, again and again until the thing is dead
            player::input_package attack() {
                ++nav.fight_ticks;
                vec2 const head = pos(state, p, 0);
                vec2 const at = creature::main_pos(state, nav.threat);
                bool const in_range = std::fabs(at.y - head.y) < 60.f && std::fabs(at.x - head.x) < throw_range * 0.8f && room.visual_contact(head, at);
                bool const armed = weapon(state, items::held(state, p, 0)) || weapon(state, items::held(state, p, 1));
                if (!in_range) {
                    if (auto const in = keep_moving())
                        return *in;
                    return go(ground_under(room, at), false);
                }
                if (auto const in = keep_moving())
                    return *in;
                int32_t const towards = at.x > head.x ? 1 : -1;
                if (armed)
                    return input(towards, 0);
                // reloading: backing off while the needle comes out (pulling
                // works on the run, as long as it isn't holding up or down),
                // standing its ground once it has room
                bool const close = std::fabs(at.x - head.x) < 200.f;
                return player::make_input(close ? float(-towards) : 0.f, 0.f, false, false, true);
            }

            player::input_package flee() {
                vec2 const me = pos(state, p, 1);
                if (nav.flee_tile.x < 0 || nav.goal_timer % 40 == 0) {
                    nav.flee_tile = flee_pipe(state, room, me, at, nav.threat_pos).value_or(ivec2{-1, -1});
                    if (nav.what != goal::flee)
                        ++stats().flights;
                }
                nav.what = goal::flee;
                if (nav.flee_tile.x >= 0)
                    return go(nav.flee_tile, true);
                // nowhere to hide: away from it
                if (auto const in = keep_moving())
                    return *in;
                return input(nav.threat_pos.x > me.x ? -1 : 1, 0);
            }

            player::input_package travel() {
                nav.what = goal::travel;
                // chased back in here: hiding a while before going out again
                if (nav.hide > 0) {
                    --nav.hide;
                    nav.stuck = 0;
                    return input(0, 0);
                }
                auto const target = w().slugcat_npc_get_target(n);
                // (through a warp it made for: a shelter from here)
                if (target == room.id && !w().room_get_is_shelter(room.id) && leads()) {
                    w().slugcat_npc_set_target(n, dcon::room_id{});
                    migrate::cancel(state, c);
                    if (!pick_target(false))
                        return input(0, 0);
                    return travel();
                }
                if (target == room.id) {
                    nav.what = goal::rest;
                    nav.goal_timer = 0;
                    nav.goal_tile = ivec2{-1, -1};
                    if (auto const* rc = rain::of_room(state, room.id))
                        nav.cycles = rc->cycles;
                    migrate::cancel(state, c);
                    ++stats().arrived;
                    return input(0, 0);
                }
                // on a guessed trip, somewhere new (through a warp, say): a
                // shelter it knows the walkable way to from here, if there is
                // one now, the closest
                if (leads() && nav.rethink) {
                    nav.rethink = 0;
                    auto const& s = explore(state, room.id, goals_from(state, room, at));
                    auto options = shelters_of(state, s, room.id);
                    // (not back to where it set off from)
                    std::erase_if(options, [&](dcon::room_id r) { return r == w().slugcat_npc_get_home(n); });
                    if (!options.empty() && options.front() != target) {
                        w().slugcat_npc_set_target(n, options.front());
                        nav.route_hops = uint8_t(std::min(255u, hops_to(s, options.front())));
                        nav.guessed = 0;
                        migrate::cancel(state, c);
                        set_trip(state, c, s, options.front());
                        ++stats().retargeted;
                        return input(0, 0);
                    }
                }
                if (leads() && target) {
                    turn_back();
                    // (back already: in the shelter it started in)
                    if (w().slugcat_npc_get_target(n) == room.id)
                        return travel();
                }
                // the rain is close: the nearest shelter will do
                if (leads() && !nav.hurried && !w().room_get_is_shelter(room.id))
                    if (auto const* rc = rain::of_room(state, room.id); rc && rain::rains_in(state, room.id) && rain::time_until_rain(*rc) < hurry_ticks
                        && !rain::deadly(*rc)) {
                        nav.hurried = 1;
                        if (pick_target(true))
                            ++stats().hurried;
                    }
                if (!target) {
                    // the pack's leader picks (a member gets it from the leader)
                    if (!leads() || !pick_target(false)) {
                        if (leads())
                            ++stats().no_way;
                        return input(0, 0);
                    }
                }
                if (!migrate::want_to(state, c) && !plan()) {
                    ++stats().no_way;
                    nav.fails = uint8_t(std::min(255, nav.fails + 1));
                    if (leads())
                        w().slugcat_npc_set_target(n, dcon::room_id{});
                    return input(0, 0);
                }
                w().creature_set_going_home(c, 0);
                auto const tile = migrate::travel_tile(state, c, room);
                if (!tile)
                    return input(0, 0);
                bool const pipe = w().creature_get_going_home(c) != 0;
                ivec2 const goal = pipe ? w().creature_get_travel_tile(c) : *tile;
                // a warp: in its grab, still, until it sets off and carries it
                // through (the warp's own sequence, as for the player); a tear
                // out of its reach from the ground carries it after a while
                if (auto const& trip = w().creature_get_trip(c); !pipe && trip.next < trip.hops && trip.exit[trip.next] >= migrate::warp_hop)
                    if (auto const wp = warps::point(state, room.id, trip.exit[trip.next] - migrate::warp_hop)) {
                        vec2 const tear = warps::position(state, wp);
                        // (still: any input holds the warp back)
                        if (custom::dist_less(pos(state, p, 0), tear, warps::pull_radius - 5.f) || at == goal) {
                            nav.stuck = 0;
                            if (++nav.warp_wait > warp_patience) {
                                nav.warp_wait = 0;
                                warps::carry(state, c, wp);
                            }
                            return input(0, 0);
                        }
                    }
                nav.warp_wait = 0;
                if (pipe && wait_for_pack(goal))
                    return input(0, 0);
                // at a gate's closed middle door: wait for it to open
                if (!pipe && w().room_get_is_gate(room.id) && std::abs(at.x - goal.x) <= 1 && std::abs(at.y - goal.y) <= 2) {
                    nav.stuck = 0;
                    return input(0, 0);
                }
                // no way on along this trip: plan again, then try another shelter
                // (a guessed way through here that doesn't go: not guessed
                // through this room again, the next plan goes round it, to a
                // warp say: WRFA_E04's gap from WRFA_C11's side)
                if (nav.guessed && nav.stuck > 300) {
                    uint16_t const here = uint16_t(room.id.index() + 1);
                    if (std::find(std::begin(nav.no_guess), std::end(nav.no_guess), here) == std::end(nav.no_guess)) {
                        nav.no_guess[nav.no_guess_next] = here;
                        nav.no_guess_next = uint8_t((nav.no_guess_next + 1) % max_no_guess);
                        ++stats().diverted;
                    }
                    nav.stuck = 601;
                }
                if (nav.stuck > 600) {
                    ++stats().stuck;
                    nav.stuck = 0;
                    nav.best = pathing::unreachable;
                    migrate::cancel(state, c);
                    nav.fails = uint8_t(std::min(255, nav.fails + 1));
                    if (nav.fails >= 3 && leads()) {
                        nav.fails = 0;
                        if (target)
                            nav.given_up = uint16_t(target.id.index() + 1);
                        w().slugcat_npc_set_target(n, dcon::room_id{});
                    }
                    return input(0, 0);
                }
                return go(goal, pipe);
            }

            player::input_package rest() {
                // the rain came and went (where it never rains, a while
                // went by): off to another shelter
                auto const* rc = rain::of_room(state, room.id);
                // (a shelter has no rain of its own: its region's)
                auto const region = w().room_get_rain_region(room.id);
                bool const rains = rc && region && !w().region_get_never_rains(region);
                bool const over = rains ? rc->cycles != nav.cycles : nav.goal_timer > dry_rest_ticks;
                if (w().slugcat_npc_get_target(n) == room.id && !over) {
                    // to where slugcats sleep, not just in by the pipe
                    // (giving up after a while: a crowd, a spot it can't reach)
                    if (auto const spot = spawn_spot(state, room); spot && *spot != at && nav.goal_timer < 600) {
                        auto const f = pathing::field_to(state, room.id, *spot, pathing::slugcat);
                        if (pathing::distance(state, f, at) != pathing::unreachable)
                            return go(*spot, false);
                    }
                    if (auto const in = keep_moving())
                        return *in;
                    return input(0, 0);
                }
                if (over && leads())
                    w().slugcat_npc_set_target(n, dcon::room_id{});
                nav.hurried = 0;
                return travel();
            }

            player::input_package think() {
                see_threats();
                if (nav.what == goal::start && nav.goal_timer < 0)
                    return input(0, 0);
                // Rivulet runs from nothing; the others, after running for a
                // while (or chased back into the same pipe again), go anyway
                if (nav.brave > 0)
                    --nav.brave;
                bool const fearless = w().player_get_stats(p).name == player::slugcat_name::rivulet || nav.brave > 0;
                // Spearmaster fights (giving up on one it can't get at: on its
                // way, deaf to threats a while)
                if (spearmaster() && !fearless && nav.what != goal::rest && nav.threat && nav.threat_timer < forget_threat
                    && creature::here(state, nav.threat, room.id) && !creature::dead(state, nav.threat)) {
                    if (nav.fight_ticks <= fight_patience)
                        return attack();
                    nav.fight_ticks = 0;
                    nav.brave = brave_ticks;
                    ++stats().braved;
                } else {
                    nav.fight_ticks = 0;
                }
                if (nav.threat && nav.threat_timer < forget_threat && nav.what != goal::rest && !fearless) {
                    if (++nav.flee_ticks <= flee_patience)
                        return flee();
                    nav.flee_ticks = 0;
                    nav.brave = brave_ticks;
                    ++stats().braved;
                } else if (nav.flee_ticks > 0) {
                    --nav.flee_ticks;
                }
                if (nav.what == goal::flee) {
                    nav.what = goal::travel;
                    nav.flee_tile = ivec2{-1, -1};
                }
                if (nav.what == goal::rest)
                    return rest();
                return travel();
            }
        };

        // Its hands, on top of whatever its legs do this tick: a weapon at its
        // feet picked up (never out of its way for one), one thrown at a
        // threat level with it (a stunned or dead one isn't a threat: on it
        // goes)
        void arm(sys::game_state& state, dcon::player_id p, room_view const& room, nav_state& nav, player::input_package& in) {
            auto& w = state.world;
            if (nav.arm_cool > 0) {
                --nav.arm_cool;
                return;
            }
            auto const& hist = w.player_get_input(p);
            vec2 const head = pos(state, p, 0);
            if (nav.threat && creature::here(state, nav.threat, room.id) && !creature::dead(state, nav.threat)
                && (w.creature_get_stun(nav.threat) <= 0 || w.player_get_stats(p).name == player::slugcat_name::spear) && !hist[0].thrw && (weapon(state, items::held(state, p, 0)) || weapon(state, items::held(state, p, 1)))) {
                vec2 const at = creature::main_pos(state, nav.threat);
                if (std::fabs(at.y - head.y) < 60.f && std::fabs(at.x - head.x) < throw_range && room.visual_contact(head, at)) {
                    in.x = at.x > head.x ? 1 : -1;
                    in.y = 0;
                    in.analogue_dir = vec2{float(in.x), 0.f};
                    in.thrw = true;
                    nav.arm_cool = 20;
                    ++stats().thrown;
                    return;
                }
            }
            if ((items::held(state, p, 0) && items::held(state, p, 1)) || in.y < 0 || hist[0].pckp)
                return;
            vec2 const feet = pos(state, p, 1);
            bool near = false;
            rooms::for_each_item(state, room.id, [&](dcon::item_id it) {
                if (near || !weapon(state, it) || w.item_get_mode(it) != world::weapon_mode::free)
                    return;
                vec2 const ip = w.body_chunk_get_pos(items::chunk(state, it));
                near = custom::dist_less(ip, head, pick_range + 10.f) || custom::dist_less(ip, feet, pick_range + 10.f);
            });
            if (near) {
                in.pckp = true;
                nav.arm_cool = 10;
                ++stats().picked_up;
            }
        }

        rw::vec3 random_color(rw::rng& rng) {
            return custom::hsl2rgb(rng.value(), mathf::lerp(0.25f, 1.f, rng.value()), mathf::lerp(0.3f, 0.85f, rng.value()));
        }
    }

    dcon::player_id create(sys::game_state& state, dcon::room_id room, vec2 feet, rw::rng& rng, dcon::slugcat_npc_id leader, player::slugcat_name name) {
        auto& w = state.world;
        if (state.slugcat_routes.size() != size_t(w.room_size()))
            build_routes(state);
        // (rolled either way: the same colours whatever the slugcat)
        auto const rolled = player::slugcat_name(rng.range(0, int32_t(player::slugcat_name::count)));
        if (name == player::slugcat_name::count)
            name = rolled;
        auto const p = player::create(state, room, feet, name);
        auto& stats_ = w.player_get_stats(p);
        stats_.body_color = random_color(rng);
        // light eyes on a dark body
        if (custom::rgb2hsl(stats_.body_color).z < 0.45f)
            stats_.eye_color = rw::vec3{1.f, 1.f, 1.f};
        if (name == player::slugcat_name::watcher && rng.value() < 0.5f)
            player_graphics::set_weaver(state, p, true);
        player_graphics::reset(state, p);
        auto const c = player::creature_of(state, p);
        w.creature_set_random(c, rw::rng{rng.next() | 1u});
        auto const n = w.create_slugcat_npc();
        w.slugcat_npc_set_player(n, p);
        w.slugcat_npc_set_leader(n, leader);
        nav_state nav;
        // a while in the shelter before setting off
        nav.goal_timer = -rng.range(40, 400);
        nav.turn_back = uint8_t(rng.range(50, 76));
        w.slugcat_npc_set_nav(n, nav);
        if (w.room_get_is_shelter(room))
            w.slugcat_npc_set_home(n, room);
        w.player_set_npc(p, n);
        ++stats().spawned;
        return p;
    }

    void spawn_group(sys::game_state& state, dcon::room_id room, vec2 at, uint32_t count, rw::rng& rng) {
        auto& w = state.world;
        auto const view = world::get_room_view(state, room);
        // the standable tiles near `at`, closest first
        std::vector<ivec2> spots;
        for (int32_t y = 1; y < view.height - 1; ++y)
            for (int32_t x = 1; x < view.width - 1; ++x)
                if (pathing::standable(view, ivec2{x, y}) && !view.get_tile(x, y + 1).solid() && view.get_tile(x, y).t != world::terrain::shortcut_entrance)
                    spots.push_back(ivec2{x, y});
        std::sort(spots.begin(), spots.end(), [&](ivec2 a, ivec2 b) {
            return rw::distance(room_view::middle_of_tile(a), at) < rw::distance(room_view::middle_of_tile(b), at);
        });
        if (spots.empty())
            spots.push_back(room_view::get_tile_position(at));
        // some of them a pack
        std::vector<bool> pack(count);
        uint32_t packed = 0;
        for (uint32_t i = 0; i < count; ++i) {
            pack[i] = rng.value() < 0.5f;
            packed += pack[i] ? 1 : 0;
        }
        dcon::slugcat_npc_id leader;
        for (uint32_t i = 0; i < count; ++i) {
            ivec2 const t = spots[size_t(i) % std::min<size_t>(spots.size(), 6)];
            vec2 const feet = room_view::middle_of_tile(t) + vec2{rng.value() * 8.f - 4.f, -2.f};
            auto const p = create(state, room, feet, rng, packed >= 2 && pack[i] ? leader : dcon::slugcat_npc_id{});
            if (packed >= 2 && pack[i] && !leader) {
                leader = w.player_get_npc(p);
                w.slugcat_npc_set_leader(leader, leader);
            }
        }
    }

    uint32_t spawn_world(sys::game_state& state, float chance) {
        auto& w = state.world;
        build_routes(state);
        uint32_t const before = stats().spawned;
        w.for_each_room([&](dcon::room_id r) {
            if (!w.room_get_is_shelter(r))
                return;
            auto& rng = state.room_random(r);
            if (!(rng.value() < chance))
                return;
            auto const view = world::get_room_view(state, r);
            auto const spot = spawn_spot(state, view);
            spawn_group(state, r, spot ? room_view::middle_of_tile(*spot) : vec2{view.pixel_width() * 0.5f, view.pixel_height() * 0.5f},
                uint32_t(rng.range(1, 9)), rng);
        });
        return stats().spawned - before;
    }

    std::optional<ivec2> shelter_spot(sys::game_state const& state, dcon::room_id shelter) {
        return spawn_spot(state, world::get_room_view(state, shelter));
    }

    void destroy(sys::game_state& state, dcon::player_id p) {
        // (its brain goes with it; a pack that loses its leader gets
        // another one in update_world)
        player::destroy(state, p);
    }

    void destroy_all(sys::game_state& state) {
        std::vector<dcon::player_id> all;
        state.world.for_each_slugcat_npc([&](dcon::slugcat_npc_id n) { all.push_back(state.world.slugcat_npc_get_player(n)); });
        for (auto const p : all)
            destroy(state, p);
    }

    void build_routes(sys::game_state& state) {
        auto& w = state.world;
        size_t const n = size_t(w.room_size());
        auto& all = state.slugcat_routes;
        all.assign(n, routes{});
        if (state.rooms.size() < n)
            state.rooms.resize(n);
        for (auto& rr : all)
            for (auto& a : rr.arrival)
                a = ivec2{-1, -1};
        // the warps: where each one puts a slugcat
        std::vector<uint8_t> arrivals(n, 0);
        w.for_each_room([&](dcon::room_id r) {
            uint32_t k = 0;
            w.room_for_each_warp_point_location(r, [&](dcon::warp_point_location_id l) {
                auto const wp = w.warp_point_location_get_point(l);
                uint32_t const mine = k++;
                if (mine >= routes::max_warps || !warps::leads_away(state, wp))
                    return;
                auto const to = warps::where_to(state, wp);
                if (!to.room || to.room == r)
                    return;
                uint8_t& slot = arrivals[size_t(to.room.index())];
                if (slot >= routes::max_warps)
                    return;
                all[size_t(to.room.index())].arrival[slot] = ground_under(world::get_room_view(state, to.room), to.pos);
                all[size_t(r.index())].warp_to[mine] = (uint32_t(to.room.index()) + 1u) << 3 | slot;
                ++slot;
            });
        });
        // every room's exits and warps, each from every other one
        tbb::parallel_for(size_t(0), n, [&](size_t i) {
            auto const r = room_at(uint32_t(i));
            auto const view = world::get_room_view(state, r);
            auto& rr = all[i];
            auto const goals = goals_of(state, view);
            // where a slugcat comes out of each exit
            std::vector<std::pair<uint32_t, ivec2>> starts;
            w.room_for_each_exit_location(r, [&](dcon::exit_location_id el) {
                uint32_t const node = w.room_exit_get_index(w.exit_location_get_exit(el));
                if (node < routes::max_exits)
                    if (auto const t = dens::exit_entrance(state, r, uint8_t(node)))
                        starts.emplace_back(node, *t);
            });
            if (w.room_get_is_gate(r)) {
                // the airlock lets it through either way
                uint64_t every = 0;
                for (auto const& g : goals)
                    every |= uint64_t(1) << g.bit;
                for (auto const& [node, t] : starts)
                    rr.from_exit[node] = every;
                return;
            }
            for (auto const& g : goals) {
                auto const f = pathing::field_to(state, r, g.tile, pathing::slugcat);
                for (auto const& [node, t] : starts)
                    if (node != g.bit && reaches(state, view, f, t))
                        rr.from_exit[node] |= uint64_t(1) << g.bit;
                for (uint32_t j = 0; j < routes::max_warps; ++j)
                    if (rr.arrival[j].x >= 0 && reaches(state, view, f, rr.arrival[j]))
                        rr.from_arrival[j] |= uint64_t(1) << g.bit;
            }
            // (the fields were for this only)
            state.runtime(r).path_fields.clear();
        });
    }

    std::vector<dcon::room_id> reachable_shelters(sys::game_state& state, dcon::player_id p) {
        if (state.slugcat_routes.size() != size_t(state.world.room_size()))
            build_routes(state);
        auto const room = rooms::room_of(state, p);
        if (!room)
            return {};
        auto const view = world::get_room_view(state, room);
        auto const& s = explore(state, room, goals_from(state, view, tile_of(state, p, view)));
        return shelters_of(state, s, room);
    }

    std::string debug_route(sys::game_state& state, std::vector<std::string> const& names) {
        auto& w = state.world;
        if (state.slugcat_routes.size() != size_t(w.room_size()))
            build_routes(state);
        std::string out;
        for (size_t i = 1; i + 1 < names.size(); ++i) {
            auto const prev = state.find_room(names[i - 1]), room = state.find_room(names[i]), next = state.find_room(names[i + 1]);
            if (!prev || !room || !next) {
                out += "  " + names[i] + ": unknown room\n";
                continue;
            }
            int32_t const in = back_node(state, room, prev);
            int32_t const leave = back_node(state, room, next);
            bool const ok = in >= 0 && leave >= 0 && (state.slugcat_routes[size_t(room.index())].from_exit[in] >> leave & 1u);
            auto const a = in >= 0 ? dens::exit_entrance(state, room, uint8_t(in)) : std::nullopt;
            auto const b = leave >= 0 ? dens::exit_entrance(state, room, uint8_t(leave)) : std::nullopt;
            if (!ok && a && b && debug().maps) {
                // f: reachable from where it comes in, o: from where it can
                // get out, B: both
                auto const view = world::get_room_view(state, room);
                auto const fwd = pathing::slugcat_reach_from(state, view, *a);
                auto const f = pathing::field_to(state, room, *b, pathing::slugcat);
                out += names[i] + " poles:";
                for (int32_t x = 0; x < view.width; ++x)
                    for (int32_t y = 0; y < view.height; ++y)
                        if (view.get_tile(x, y).vertical_beam() && !view.get_tile(x, y - 1).vertical_beam()) {
                            int32_t top = y;
                            while (top + 1 < view.height && view.get_tile(x, top + 1).vertical_beam())
                                ++top;
                            out += " x" + std::to_string(x) + ":" + std::to_string(y) + "-" + std::to_string(top);
                        }
                out += "\n    ";
                for (int32_t x = 0; x < view.width; ++x)
                    out += char('0' + x % 10);
                out += "\n";
                for (int32_t y = view.height - 1; y >= 0; --y) {
                    char buf[16];
                    std::snprintf(buf, sizeof buf, "%3d ", y);
                    out += buf;
                    for (int32_t x = 0; x < view.width; ++x) {
                        ivec2 const t{x, y};
                        auto const tile = view.get_tile(t);
                        bool const fr = fwd[size_t(y * view.width + x)] != 0;
                        bool const back = pathing::distance(state, f, t) != pathing::unreachable;
                        char ch = tile.t == world::terrain::solid ? '#' : tile.t == world::terrain::slope ? '/' : tile.t == world::terrain::floor ? '='
                            : tile.t == world::terrain::shortcut_entrance ? 'S' : tile.vertical_beam() ? '|' : tile.horizontal_beam() ? '-' : tile.water ? '~' : ' ';
                        if (fr && back)
                            ch = 'B';
                        else if (fr)
                            ch = 'f';
                        else if (back)
                            ch = 'o';
                        out += ch;
                    }
                    out += "\n";
                }
            }
            out += std::string{"  "} + (ok ? "ok  " : "NO  ") + names[i] + ": in by node " + std::to_string(in) + (a ? " (" + std::to_string(a->x) + "," + std::to_string(a->y) + ")" : "")
                + ", out by node " + std::to_string(leave) + (b ? " (" + std::to_string(b->x) + "," + std::to_string(b->y) + ")" : "") + "\n";
        }
        return out;
    }

    std::string debug_reach(sys::game_state& state, dcon::player_id p) {
        auto const room = rooms::room_of(state, p);
        auto const view = world::get_room_view(state, room);
        auto const& s = explore(state, room, goals_from(state, view, tile_of(state, p, view)));
        std::string out = "reached:";
        std::vector<dcon::room_id> seen;
        for (auto const st : s.order) {
            auto const r = room_at(st / slots);
            if (std::find(seen.begin(), seen.end(), r) != seen.end())
                continue;
            seen.push_back(r);
            out += " " + std::string{state.to_string_view(state.world.room_get_name(r))};
        }
        return out + "\n";
    }

    std::string debug_dead_ends(sys::game_state& state) {
        auto& w = state.world;
        std::string out;
        uint32_t rooms_n = 0, dead = 0, exits_n = 0;
        w.for_each_room([&](dcon::room_id r) {
            auto const view = world::get_room_view(state, r);
            auto const goals = goals_of(state, view);
            if (!debug().room.empty() && state.find_room(debug().room) == r)
                for (auto const& g : goals)
                    out += "goal " + std::to_string(g.bit) + " at " + std::to_string(g.tile.x) + "," + std::to_string(g.tile.y) + "\n";
            if (goals.size() < 2)
                return;
            ++rooms_n;
            auto const& rr = state.slugcat_routes[size_t(r.index())];
            std::string bad;
            for (auto const& g : goals)
                if (g.bit < warp_bit) {
                    ++exits_n;
                    if (rr.from_exit[g.bit] == 0) {
                        ++dead;
                        bad += " " + std::to_string(g.bit) + "@" + std::to_string(g.tile.x) + "," + std::to_string(g.tile.y);
                    }
                }
            if (!bad.empty())
                out += std::string{state.to_string_view(w.room_get_name(r))} + bad + "\n";
        });
        return out + std::to_string(dead) + " of " + std::to_string(exits_n) + " exits in " + std::to_string(rooms_n) + " rooms lead nowhere\n";
    }

    std::string debug_room(sys::game_state& state, dcon::room_id room, ivec2 mark) {
        auto const view = world::get_room_view(state, room);
        std::string out;
        // the poles: each column's runs of beam tiles
        for (int32_t x = 0; x < view.width; ++x)
            for (int32_t y = 0; y < view.height; ++y)
                if (view.get_tile(x, y).vertical_beam() && !view.get_tile(x, y - 1).vertical_beam()) {
                    int32_t top = y;
                    while (top + 1 < view.height && view.get_tile(x, top + 1).vertical_beam())
                        ++top;
                    out += "pole x=" + std::to_string(x) + " y " + std::to_string(y) + ".." + std::to_string(top) + "\n";
                }
        auto& w = state.world;
        w.room_for_each_shortcut_location(room, [&](dcon::shortcut_location_id sl) {
            auto const sc = w.shortcut_location_get_shortcut(sl);
            auto const path = w.shortcut_get_path(sc);
            ivec2 const a = w.shortcut_get_start(sc);
            ivec2 const b = path.size() > 0 ? path[path.size() - 1] : a;
            out += "shortcut type " + std::to_string(int(w.shortcut_get_type(sc))) + " " + std::to_string(a.x) + "," + std::to_string(a.y) + " -> " + std::to_string(b.x)
                + "," + std::to_string(b.y) + " len " + std::to_string(path.size()) + "\n";
        });
        // --npc-goal: the field to that exit or warp (and --npc-box: its
        // distances over the box's walkable tiles)
        auto const& dbg = debug();
        std::optional<pathing::field_id> goal_field;
        for (auto const& gt : goals_of(state, view))
            if (dbg.goal >= 0 && gt.bit == uint32_t(dbg.goal))
                goal_field = pathing::field_to(state, room, gt.tile, pathing::slugcat);
        if (goal_field && dbg.box)
            for (int32_t y = std::max(0, dbg.box_lo.y); y <= std::min(view.height - 1, dbg.box_hi.y); ++y)
                for (int32_t x = std::max(0, dbg.box_lo.x); x <= std::min(view.width - 1, dbg.box_hi.x); ++x)
                    if (pathing::slugcat_walkable(state, view, ivec2{x, y}))
                        out += "tile " + std::to_string(x) + "," + std::to_string(y) + (pathing::standable(view, ivec2{x, y}) ? " stand" : " walk")
                            + (view.get_tile(x, y).vertical_beam() ? " pole" : "") + (view.get_tile(x, y).horizontal_beam() ? " hbeam" : "")
                            + (view.get_tile(x, y).t == world::terrain::shortcut_entrance ? " pipe" : "") + " dist "
                            + std::to_string(pathing::distance(state, *goal_field, ivec2{x, y})) + "\n";
        out += "   ";
        for (int32_t x = 0; x < view.width; ++x)
            out += char('0' + x % 10);
        out += "\n";
        for (int32_t y = view.height - 1; y >= 0; --y) {
            out += (y < 10 ? " " : "") + std::to_string(y) + " ";
            for (int32_t x = 0; x < view.width; ++x) {
                auto const t = view.get_tile(x, y);
                char ch = t.t == world::terrain::solid ? '#' : t.t == world::terrain::slope ? '/' : t.t == world::terrain::floor ? '=' : t.t == world::terrain::shortcut_entrance ? 'S' : t.vertical_beam() ? '|' : t.horizontal_beam() ? '-' : t.water ? '~' : ' ';
                if (ch == ' ' && pathing::slugcat_walkable(state, view, ivec2{x, y}))
                    ch = '.';
                if (goal_field && ch != '#' && pathing::distance(state, *goal_field, ivec2{x, y}) != pathing::unreachable)
                    ch = 'o';
                if (ivec2{x, y} == mark)
                    ch = '@';
                out += ch;
            }
            out += "\n";
        }
        return out;
    }

    std::string debug_routes(sys::game_state& state, dcon::player_id p) {
        auto& w = state.world;
        auto const room = rooms::room_of(state, p);
        auto const view = world::get_room_view(state, room);
        ivec2 const at = tile_of(state, p, view);
        std::string out = std::string{state.to_string_view(w.room_get_name(room))} + " at " + std::to_string(at.x) + "," + std::to_string(at.y)
            + " walkable " + std::to_string(pathing::slugcat_walkable(state, view, at)) + " standable " + std::to_string(pathing::standable(view, at)) + "\n";
        for (int32_t y = view.height - 1; y >= 0; --y) {
            out += (y < 10 ? " " : "") + std::to_string(y) + " ";
            for (int32_t x = 0; x < view.width; ++x) {
                auto const t = view.get_tile(x, y);
                char ch = t.t == world::terrain::solid ? '#' : t.t == world::terrain::slope ? '/' : t.t == world::terrain::floor ? '=' : t.t == world::terrain::shortcut_entrance ? 'S' : t.vertical_beam() ? '|' : t.horizontal_beam() ? '-' : ' ';
                if (ivec2{x, y} == at)
                    ch = '@';
                out += ch;
            }
            out += "\n";
        }
        for (auto const& g : goals_of(state, view)) {
            auto const f = pathing::field_to(state, room, g.tile, pathing::slugcat);
            out += "  goal bit " + std::to_string(g.bit) + " tile " + std::to_string(g.tile.x) + "," + std::to_string(g.tile.y) + " dist here "
                + std::to_string(pathing::distance(state, f, at)) + " at goal " + std::to_string(pathing::distance(state, f, g.tile)) + " walkable goal "
                + std::to_string(pathing::slugcat_walkable(state, view, g.tile)) + "\n";
            auto const e = exit_by_node(state, room, g.bit);
            auto const to = e ? leads_to(state, e, room) : dcon::room_id{};
            if (to) {
                auto const& rr = state.slugcat_routes[size_t(to.index())];
                int32_t const node = back_node(state, to, room);
                out += "    -> " + std::string{state.to_string_view(w.room_get_name(to))} + " node " + std::to_string(node) + " links "
                    + std::to_string(node >= 0 ? rr.from_exit[node] : 0) + "\n";
            }
        }
        return out;
    }

    std::optional<ivec2> planned_exit(sys::game_state& state, dcon::player_id p, dcon::room_id& next) {
        auto& w = state.world;
        auto const n = w.player_get_npc(p);
        auto const c = player::creature_of(state, p);
        auto const room = rooms::room_of(state, p);
        if (!n || !room)
            return std::nullopt;
        auto const target = w.slugcat_npc_get_target(n);
        if (!target || target == room)
            return std::nullopt;
        auto const view = world::get_room_view(state, room);
        // (no walkable way: the way its guessed trip goes, the rooms'
        // connections where the walkable way doesn't reach)
        auto& nav = w.slugcat_npc_get_nav(n);
        if (!set_trip(state, c, explore(state, room, goals_from(state, view, tile_of(state, p, view))), target)) {
            uint64_t start = goals_from(state, view, tile_of(state, p, view));
            if (start == 0)
                start = all_exits(state, room);
            if (!set_trip(state, c, explore(state, room, start, true, &nav), target)
                && !set_trip(state, c, explore(state, room, all_exits(state, room), true, &nav, true), target))
                return std::nullopt;
        }
        auto const& t = w.creature_get_trip(c);
        next = room_at(t.room[t.next]);
        uint8_t const exit = t.exit[t.next];
        if (exit >= migrate::warp_hop) {
            auto const wp = warps::point(state, room, exit - migrate::warp_hop);
            if (!wp)
                return std::nullopt;
            return ground_under(view, warps::position(state, wp));
        }
        return dens::exit_entrance(state, room, exit);
    }

    void take_over(sys::game_state& state, dcon::player_id p) {
        auto& w = state.world;
        if (auto const n = w.player_get_npc(p)) {
            w.player_set_npc(p, dcon::slugcat_npc_id{});
            w.delete_slugcat_npc(n);
        }
    }

    void update_world(sys::game_state& state) {
        auto& w = state.world;
        // a dead (or gone) leader's place goes to the first living member
        std::vector<std::pair<dcon::slugcat_npc_id, dcon::slugcat_npc_id>> handed;
        w.for_each_slugcat_npc([&](dcon::slugcat_npc_id n) {
            auto l = w.slugcat_npc_get_leader(n);
            if (!l)
                return;
            for (auto const& [from, to] : handed)
                if (l == from) {
                    w.slugcat_npc_set_leader(n, to);
                    return;
                }
            if (w.slugcat_npc_is_valid(l) && !player::dead(state, w.slugcat_npc_get_player(l)))
                return;
            if (player::dead(state, w.slugcat_npc_get_player(n))) {
                return;
            }
            // this one leads now (and keeps the target)
            if (w.slugcat_npc_is_valid(l) && !w.slugcat_npc_get_target(n))
                w.slugcat_npc_set_target(n, w.slugcat_npc_get_target(l));
            handed.emplace_back(l, n);
            w.slugcat_npc_set_leader(n, n);
        });
        // the members go where the leader goes, and set off with it
        w.for_each_slugcat_npc([&](dcon::slugcat_npc_id n) {
            auto const l = w.slugcat_npc_get_leader(n);
            if (!l || l == n || !w.slugcat_npc_is_valid(l))
                return;
            auto& nav = w.slugcat_npc_get_nav(n);
            if (nav.what == goal::start)
                nav.goal_timer = w.slugcat_npc_get_nav(l).what == goal::start ? std::min(nav.goal_timer, -1) : std::max(nav.goal_timer, 0);
            auto const t = w.slugcat_npc_get_target(l);
            if (t != w.slugcat_npc_get_target(n)) {
                w.slugcat_npc_set_target(n, t);
                migrate::cancel(state, player::creature_of(state, w.slugcat_npc_get_player(n)));
            }
        });
    }

    player::input_package think(sys::game_state& state, dcon::player_id p, room_view const& room) {
        auto& w = state.world;
        auto const n = w.player_get_npc(p);
        auto const c = player::creature_of(state, p);
        if (!n || player::dead(state, p) || w.player_get_eaten(p))
            return player::input_package{};
        // fell out of the room's bottom: dead (Creature.Update; the port
        // doesn't kill a slugcat for it otherwise, and down there it would
        // wait under the room for good)
        if (room.default_water_level < 0 && !player::in_shortcut(state, p)) {
            float const limit = w.player_get_body_mode(p) == player::body_mode_index::wall_climb ? -250.f : -500.f;
            if (w.body_chunk_get_pos(player::body_chunk(state, p, 0)).y < limit) {
                player::die(state, p);
                return player::input_package{};
            }
        }
        auto& nav = w.slugcat_npc_get_nav(n);
        // a new room: its trip moves on (a warp did that already), what it
        // knew of the last room is gone
        uint16_t const here = uint16_t(room.id.index() + 1);
        if (nav.room != here) {
            if (nav.room != 0) {
                auto const from = room_at(uint32_t(nav.room - 1));
                if (w.creature_get_time_in_room(c) > 0)
                    migrate::arrived(state, c, room.id);
                else
                    player_graphics::reset(state, p);
                if (w.room_get_region_from_region_membership(from) != w.room_get_region_from_region_membership(room.id))
                    ++stats().regions_crossed;
            }
            // (back and forth between two rooms on a guessed trip: neither
            // is guessed through any more)
            nav.bounces = here == nav.room_before ? uint8_t(nav.bounces + 1) : 0;
            // (on a walkable trip: that target is given up, another one picked)
            if (nav.bounces >= 3) {
                if (nav.guessed) {
                    for (uint16_t const r : {here, nav.room})
                        if (r != 0 && std::find(std::begin(nav.no_guess), std::end(nav.no_guess), r) == std::end(nav.no_guess)) {
                            nav.no_guess[nav.no_guess_next] = r;
                            nav.no_guess_next = uint8_t((nav.no_guess_next + 1) % max_no_guess);
                        }
                } else if (auto const l = w.slugcat_npc_get_leader(n); !l || l == n) {
                    if (auto const target = w.slugcat_npc_get_target(n))
                        nav.given_up = uint16_t(target.id.index() + 1);
                    w.slugcat_npc_set_target(n, dcon::room_id{});
                }
                nav.bounces = 0;
                ++stats().diverted;
                migrate::cancel(state, c);
            }
            nav.room_before = nav.room;
            nav.room = here;
            nav.rethink = nav.guessed;
            nav.threat = {};
            nav.flee_tile = ivec2{-1, -1};
            nav.goal_tile = ivec2{-1, -1};
            nav.how = move::walk;
            nav.wait = 0;
            for (auto& e : nav.blocked)
                e[0] = e[1] = ivec2{-1, -1};
            if (nav.what == goal::flee && nav.room_before != 0) {
                nav.chased = uint8_t(std::min(8, nav.chased + 1));
                nav.chased_from = nav.room_before;
                nav.hide = 400 * int32_t(nav.chased) + w.creature_get_random(c).range(0, 200);
                // the second time it waits a little and goes in anyway
                if (nav.chased >= 2) {
                    nav.hide = 100 + w.creature_get_random(c).range(0, 100);
                    nav.brave = nav.hide + brave_ticks;
                    ++stats().braved;
                }
            } else if (here != nav.chased_from) {
                nav.chased = 0;
            }
            if (nav.what == goal::flee)
                nav.what = goal::travel;
        }
        migrate::tick(state, c);
        ++nav.goal_timer;
        // stunned, held, sucked into a pipe: its legs aren't its own
        if (w.creature_get_stun(c) > 0 || w.creature_get_entering_shortcut(c).x >= 0)
            return player::input_package{};
        // Spearmaster in a shelter at the start of a cycle (waking up there,
        // or still resting when a new one starts): pick up held with a free
        // hand until a needle is pulled out of its tail for each hand
        // (items.cpp's spearmaster_update), once a cycle
        if (w.player_get_stats(p).name == player::slugcat_name::spear && w.room_get_is_shelter(room.id)) {
            auto const* rc = rain::of_room(state, room.id);
            uint32_t const cycle = (rc ? rc->cycles : 0u) + 1u;
            bool const new_cycle = nav.what == goal::start || (nav.what == goal::rest && rc && rc->cycles != nav.cycles);
            if (nav.spear_cycle != cycle && new_cycle) {
                bool const free_hand = !items::held(state, p, 0) || !items::held(state, p, 1);
                if (free_hand && ++nav.spear_timer < 400)
                    return player::make_input(0.f, 0.f, false, false, true);
                if (!free_hand || nav.spear_timer >= 400) {
                    nav.spear_cycle = cycle;
                    nav.spear_timer = 0;
                }
            }
        }
        brain b{state, p, n, c, room, nav, w.creature_get_random(c), tile_of(state, p, room)};
        if (creature::held(state, c)) {
            // wriggling
            auto& rng = b.rng;
            return input(rng.range(-1, 2), rng.range(-1, 2), rng.value() < 0.2f);
        }
        auto in = b.think();
        arm(state, p, room, nav, in);
        return in;
    }

    std::string describe(sys::game_state const& state, dcon::player_id p) {
        auto const& w = state.world;
        auto const n = w.player_get_npc(p);
        if (!n)
            return "player";
        auto const& nav = w.slugcat_npc_get_nav(n);
        static char const* const goals[] = {"start", "travel", "flee", "rest"};
        static char const* const moves[] = {"walk", "rise", "back up", "run", "crouch", "charge", "flight", "swing", "tip", "wall", "bounce"};
        auto const target = w.slugcat_npc_get_target(n);
        auto const l = w.slugcat_npc_get_leader(n);
        std::string s = std::string{"NPC "} + player::to_string(w.player_get_stats(p).name) + " " + goals[size_t(nav.what)] + "/" + moves[size_t(nav.how)];
        if (target)
            s += " to " + std::string{state.to_string_view(w.room_get_name(target))};
        if (l)
            s += l == n ? " (leads a pack)" : " (in a pack)";
        if (player::dead(state, p))
            s += " dead";
        return s;
    }

    std::string stats_line(sys::game_state const& state) {
        auto const& s = stats();
        uint32_t alive = 0, total = 0, resting = 0, packs = 0;
        state.world.for_each_slugcat_npc([&](dcon::slugcat_npc_id n) {
            ++total;
            auto const p = state.world.slugcat_npc_get_player(n);
            if (!player::dead(state, p))
                ++alive;
            if (state.world.slugcat_npc_get_nav(n).what == goal::rest)
                ++resting;
            if (state.world.slugcat_npc_get_leader(n) == n)
                ++packs;
        });
        char buf[420];
        std::snprintf(buf, sizeof buf,
            "NPC slugcats: %u (%u alive, %u resting, %u packs); %u shelters reached, %u regions crossed, %u trips, %u rises, %u leaps, %u pounces, "
            "%u flights, %u hurried, %u turned back, %u braved, %u retargeted, %u diverted, %u pick ups, %u throws, %u stuck, %u no way",
            total, alive, resting, packs, s.arrived.load(), s.regions_crossed.load(), s.trips.load(), s.rises.load(), s.leaps.load(), s.pounces.load(), s.flights.load(),
            s.hurried.load(), s.turned_back.load(), s.braved.load(), s.retargeted.load(), s.diverted.load(), s.picked_up.load(), s.thrown.load(), s.stuck.load(), s.no_way.load());
        return buf;
    }
}
