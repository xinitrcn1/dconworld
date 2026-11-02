// Spider.cs: the coalescipede, its Flock and its Centipede. See spider.hpp.

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "spider.hpp"
#include "dens.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "rain.hpp"
#include "rooms.hpp"
#include "spider_graphics.hpp"

namespace spider {
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    using world::terrain;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // CreatureTemplate (StaticWorld: Spider): baseDamageResistance,
        // baseStunResistance, instantDeathDamageLimit, visualRadius
        constexpr float base_damage_resistance = 0.1f;
        constexpr float base_stun_resistance = 0.1f;
        constexpr float instant_death_damage_limit = 1.f;
        constexpr float visual_radius = 600.f;
        // float.MinValue
        constexpr float min_value = -3.4028235e38f;
        // ModManager.MMF (Remix) is off: its extra rolls (lightExp every
        // tick, the chain's separation every tick, 20 ticks to cut a part off)
        constexpr uint8_t separated_limit = 20;

        payload& pl(sys::game_state& state, dcon::creature_id c) { return state.world.creature_get_payload(c).spider; }
        payload const& pl(sys::game_state const& state, dcon::creature_id c) { return state.world.creature_get_payload(c).spider; }
        dcon::spider_data_id data(sys::game_state const& state, dcon::creature_id c) {
            return dcon::spider_data_id{dcon::spider_data_id::value_base_t(state.world.creature_get_payload(c).spider.data)};
        }
        dcon::body_chunk_id main(sys::game_state const& state, dcon::creature_id c) { return chunk(state, c, 0); }
        vec2& pos(sys::game_state& state, dcon::creature_id c) { return state.world.body_chunk_get_pos(main(state, c)); }
        vec2& vel(sys::game_state& state, dcon::creature_id c) { return state.world.body_chunk_get_vel(main(state, c)); }
        vec2 pos(sys::game_state const& state, dcon::creature_id c) { return state.world.body_chunk_get_pos(main(state, c)); }
        ivec2 tile_of(vec2 p) { return room_view::get_tile_position(p); }
        bool has(payload const& x, uint8_t f) { return (x.flags & f) != 0; }
        void set(payload& x, uint8_t f, bool on) { x.flags = on ? uint8_t(x.flags | f) : uint8_t(x.flags & ~f); }
        // connectDistance
        float connect_distance(payload const& x) { return mathf::lerp(6.f, 12.f, x.size); }
        ivec2 follow_tile(payload const& x) { return ivec2{x.follow_x, x.follow_y}; }
        void set_follow(payload& x, ivec2 t, move m) {
            x.follow_x = int16_t(t.x);
            x.follow_y = int16_t(t.y);
            x.follow_type = m;
        }

        // AImap.TileAccessibleToCreature (the Spider's accessibilities:
        // Floor, Corridor, Climb, Wall, Ceiling)
        bool accessible(room_view const& room, ivec2 t) {
            return room.inside(t.x, t.y) && pathing::accessible(room, t, pathing::climber);
        }

        // Room.LightSourceExposure over the room's placed LightSources (the
        // creatures' and items' lights aren't in the simulation): Lightness
        // is the light's alpha (strength, fading with the sun) times its
        // colour's brightness (white 1, the environment's and the effect
        // colours taken as 0.5; night lights off, blinking ignored)
        float light_exposure(sys::game_state const& state, dcon::room_id room, vec2 at) {
            auto const& w = state.world;
            float sun = 1.f;
            if (auto const* rc = rain::of_room(state, room))
                sun = mathf::pow(mathf::inverse_lerp(-0.5f, 0.f, rain::shader_light(*rc)), 1.2f);
            float num = 0.f;
            w.room_for_each_light_location(room, [&](dcon::light_location_id ll) {
                auto const l = w.light_location_get_light(ll);
                vec2 const lp = w.light_placed_get_pos(l);
                float const rad = w.light_placed_get_rad(l);
                if (!custom::dist_less(at, lp, rad) || w.light_placed_get_night_light(l))
                    return;
                float alpha = w.light_placed_get_strength(l);
                if (w.light_placed_get_fade_with_sun(l))
                    alpha *= sun;
                float const lightness = (w.light_placed_get_color_type(l) == 1 ? 1.f : 0.5f) * alpha;
                num += custom::s_curve(mathf::inverse_lerp(rad, 0.f, rw::distance(at, lp)), 0.5f) * lightness;
            });
            return mathf::clamp01(num);
        }

        //
        // The room's creatures and spiders (abstractRoom.creatures, the
        // Flock), gathered once per room and tick on the thread updating it
        //
        struct room_lists {
            sys::game_state const* state = nullptr;
            dcon::room_id room;
            uint32_t tick = 0;
            bool built = false;
            // every creature out in the room (not in a pipe or a den), the
            // dead included
            std::vector<dcon::creature_id> creatures;
            // the room's spiders out in it, the dead included
            std::vector<dcon::creature_id> spiders;
            // the Flock: the live ones (a dead one leaves it)
            std::vector<dcon::creature_id> flock;
        };
        thread_local room_lists lists;

        room_lists const& lists_of(sys::game_state const& state, dcon::room_id room) {
            auto& l = lists;
            if (l.built && l.state == &state && l.room == room && l.tick == state.tick)
                return l;
            l.state = &state;
            l.room = room;
            l.tick = state.tick;
            l.built = true;
            l.creatures.clear();
            l.spiders.clear();
            l.flock.clear();
            auto const& w = state.world;
            rooms::for_each_creature(state, room, [&](dcon::creature_id c) {
                if (w.creature_get_away(c) == 0)
                    l.creatures.push_back(c);
            });
            rooms::for_each_creature(state, room, creature::type::spider, [&](dcon::creature_id c) {
                if (w.creature_get_away(c) != 0)
                    return;
                l.spiders.push_back(c);
                if (!w.creature_get_dead(c))
                    l.flock.push_back(c);
            });
            return l;
        }
        // a listed spider still is one of the room's, out in it
        bool listed_ok(sys::game_state const& state, dcon::creature_id s, dcon::room_id room) {
            auto const& w = state.world;
            return w.creature_is_valid(s) && w.creature_get_type(s) == creature::type::spider && w.creature_get_away(s) == 0
                && creature::room_of(state, s) == room;
        }
        // the room's spiders right now, walked straight (the serial phases)
        void gather_spiders(sys::game_state const& state, dcon::room_id room, std::vector<dcon::creature_id>& out) {
            out.clear();
            rooms::for_each_creature(state, room, creature::type::spider, [&](dcon::creature_id s) {
                if (state.world.creature_get_away(s) == 0)
                    out.push_back(s);
            });
        }

        // a list for the accessors the graphics call (never nested)
        std::vector<dcon::creature_id>& scratch() {
            thread_local std::vector<dcon::creature_id> v;
            return v;
        }

        //
        // Spider.Centipede: its keeper, its members in body order
        //
        dcon::creature_id keeper(sys::game_state const& state, dcon::creature_id c) {
            auto const& w = state.world;
            uint16_t const k = pl(state, c).chain;
            if (k == no_chain)
                return dcon::creature_id{};
            dcon::creature_id const kid{dcon::creature_id::value_base_t(k)};
            // (a keeper gone to another room's update is out of reach: the
            // chain is lost to the ones left behind)
            if (!w.creature_is_valid(kid) || w.creature_get_type(kid) != creature::type::spider || !rooms::reachable(state, kid) || pl(state, kid).chain != k)
                return dcon::creature_id{};
            return kid;
        }
        chain_state& cs(sys::game_state& state, dcon::creature_id k) { return state.world.spider_data_get_chain(data(state, k)); }
        chain_state const& cs(sys::game_state const& state, dcon::creature_id k) { return state.world.spider_data_get_chain(data(state, k)); }

        struct member_list {
            dcon::creature_id ids[max_chain];
            uint32_t n = 0;
            int32_t index_of(dcon::creature_id s) const {
                for (uint32_t i = 0; i < n; ++i)
                    if (ids[i] == s)
                        return int32_t(i);
                return -1;
            }
        };
        // the room's spiders of the chain kept by `k`, in body order
        member_list members(sys::game_state const& state, std::vector<dcon::creature_id> const& spiders, dcon::room_id room, dcon::creature_id k) {
            member_list l;
            if (!k)
                return l;
            uint16_t const key = uint16_t(k.index());
            for (auto const s : spiders) {
                if (l.n >= max_chain || !listed_ok(state, s, room) || pl(state, s).chain != key)
                    continue;
                uint32_t i = l.n++;
                for (; i > 0 && pl(state, l.ids[i - 1]).chain_order > pl(state, s).chain_order; --i)
                    l.ids[i] = l.ids[i - 1];
                l.ids[i] = s;
            }
            return l;
        }
        member_list members(sys::game_state const& state, dcon::room_id room, dcon::creature_id k) {
            return members(state, lists_of(state, room).spiders, room, k);
        }

        void join(sys::game_state& state, dcon::creature_id k, dcon::creature_id s) {
            auto& x = pl(state, s);
            x.chain = uint16_t(k.index());
            x.chain_order = cs(state, k).next_order++;
            x.separated_counter = 0;
            set(x, flag_in_right_place, false);
        }
        void unlink(sys::game_state& state, dcon::creature_id s) {
            auto& x = pl(state, s);
            x.chain = no_chain;
            x.separated_counter = 0;
            set(x, flag_in_right_place, false);
        }
        // the keeper `k` leaves its chain to `to` (and the other members)
        void hand_over(sys::game_state& state, dcon::creature_id k, member_list const& rest, dcon::creature_id to) {
            auto& w = state.world;
            w.spider_data_set_chain(data(state, to), cs(state, k));
            for (uint32_t i = 0; i < rest.n; ++i)
                if (rest.ids[i] != k)
                    pl(state, rest.ids[i]).chain = uint16_t(to.index());
        }
        // SpiderMass.AbandonSpider (from `spiders`, the room's spiders): a
        // keeper leaving hands the chain over to the first of the others
        void leave(sys::game_state& state, std::vector<dcon::creature_id> const& spiders, dcon::room_id room, dcon::creature_id s) {
            auto const k = keeper(state, s);
            if (k == s) {
                auto const l = members(state, spiders, room, k);
                for (uint32_t i = 0; i < l.n; ++i)
                    if (l.ids[i] != s) {
                        hand_over(state, s, l, l.ids[i]);
                        break;
                    }
            }
            unlink(state, s);
        }
        void leave(sys::game_state& state, dcon::room_id room, dcon::creature_id s) {
            leave(state, lists_of(state, room).spiders, room, s);
        }

        // VisualContact: within the template's visualRadius, in sight
        bool visual_contact(room_view const& room, vec2 from, vec2 to) {
            return custom::dist_less(from, to, visual_radius) && room.visual_contact(from, to);
        }
        // Spider.ConsiderPrey
        bool consider_prey(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) {
            return creature::live(state, t) && creature::mass_of(state, t) <= 3.3f
                && creature::static_relationship(state, c, t).type == creature::rel::eats && pl(state, c).leeched_out != t;
        }
        // the chain's prey, if it is still about
        dcon::creature_id chain_prey(sys::game_state const& state, chain_state const& s, dcon::room_id room) {
            return s.prey && creature::here(state, s.prey, room) ? s.prey : dcon::creature_id{};
        }

        // Centipede.SeePrey
        void see_prey(sys::game_state& state, dcon::creature_id k, dcon::creature_id head, dcon::creature_id t, dcon::room_id room) {
            auto& s = cs(state, k);
            vec2 const hp = pos(state, head);
            vec2 const tp = creature::main_pos(state, t);
            bool const none = !chain_prey(state, s, room);
            if (s.total_mass > creature::mass_of(state, t) && (none || custom::dist_less(hp, tp, rw::distance(hp, s.prey_pos))))
                s.prey = t;
            if (t == s.prey) {
                s.prey_visual_counter = 0;
                s.prey_pos = tp;
            }
        }

        // Centipede.ConsiderCreature
        void chain_consider(sys::game_state& state, dcon::creature_id k, dcon::creature_id head, room_view const& room, rw::rng& rng) {
            auto const& l = lists_of(state, room.id);
            if (l.creatures.empty() || !head)
                return;
            auto const t = l.creatures[size_t(rng.range(0, int32_t(l.creatures.size())))];
            if (!creature::here(state, t, room.id))
                return;
            auto& s = cs(state, k);
            vec2 const hp = pos(state, head);
            vec2 const tp = creature::main_pos(state, t);
            bool const none = !chain_prey(state, s, room.id);
            if (consider_prey(state, head, t) && s.total_mass > creature::mass_of(state, t)
                && (none || (custom::dist_less(hp, tp, rw::distance(hp, s.prey_pos)) && visual_contact(room, hp, tp)))) {
                s.prey = t;
                s.prey_visual_counter = 0;
            }
        }

        // Centipede.Tighten: the parts in place pulled to the spider ahead
        void tighten(sys::game_state& state, member_list const& l) {
            for (uint32_t i = 1; i < l.n; ++i) {
                if (!has(pl(state, l.ids[i]), flag_in_right_place))
                    continue;
                auto const a = l.ids[i], b = l.ids[i - 1];
                vec2& pa = pos(state, a);
                vec2& pb = pos(state, b);
                vec2 const d = custom::dir_vec(pa, pb);
                float const dist = rw::distance(pa, pb);
                float const cd = connect_distance(pl(state, b));
                vec2 const push = d * (dist - cd) * 0.5f;
                pa += push;
                vel(state, a) += push;
                pb -= push;
                vel(state, b) -= push;
            }
        }

        // Centipede.Update (once a tick, from the first of its spiders to
        // update; the Tighten counter on every member's call)
        void chain_update(sys::game_state& state, dcon::creature_id c, room_view const& room, rw::rng& rng) {
            auto const& lst = lists_of(state, room.id);
            auto k = keeper(state, c);
            if (!k) {
                unlink(state, c);
                return;
            }
            auto l = members(state, lst.spiders, room.id, k);
            {
                auto& s = cs(state, k);
                ++s.counter;
                if (s.counter >= int32_t(l.n)) {
                    s.counter = 0;
                    tighten(state, l);
                }
                if (s.last_tick == state.tick + 1)
                    return;
                s.last_tick = state.tick + 1;
            }
            // SpiderMass.Update: the dead and the ones gone leave; the parts
            // cut off for too long are dropped
            member_list kept;
            for (uint32_t i = 0; i < l.n; ++i) {
                auto const m = l.ids[i];
                if (state.world.creature_get_dead(m) || !creature::here(state, m, room.id) || pl(state, m).separated_counter > separated_limit) {
                    if (m != k)
                        unlink(state, m);
                    continue;
                }
                kept.ids[kept.n++] = m;
            }
            if (kept.index_of(k) < 0) {
                if (kept.n > 0) {
                    hand_over(state, k, kept, kept.ids[0]);
                    unlink(state, k);
                    k = kept.ids[0];
                } else {
                    unlink(state, k);
                    return;
                }
            }
            auto& s = cs(state, k);
            if (s.light_adaption < s.hunt)
                s.light_adaption = mathf::min(s.hunt, s.light_adaption + 0.00083333335f);
            else if (s.light_adaption > s.hunt)
                s.light_adaption = mathf::max(s.hunt, s.light_adaption - 0.004166667f);
            auto const head = kept.n > 0 ? kept.ids[0] : dcon::creature_id{};
            s.walk_cycle -= head && has(pl(state, head), flag_moving) ? 2 : 1;
            s.total_mass = 0.f;
            uint32_t const n = kept.n;
            for (uint32_t j = 0; j < n; ++j) {
                auto const m = kept.ids[j];
                auto& x = pl(state, m);
                // CentipedePart.Update
                if (j > 0) {
                    vec2 const mp = pos(state, m);
                    vec2 const ap = pos(state, kept.ids[j - 1]);
                    if (custom::manhattan_distance(tile_of(mp), tile_of(ap)) > 1 && !room.visual_contact(mp, ap)) {
                        if (x.separated_counter < 254)
                            ++x.separated_counter;
                    } else {
                        x.separated_counter = 0;
                    }
                    if (!custom::dist_less(mp, ap, has(x, flag_in_right_place) ? 50.f : 100.f))
                        x.separated_counter = 255;
                } else {
                    x.separated_counter = 0;
                }
                s.total_mass += state.world.body_chunk_get_mass(main(state, m));
                x.legs_position = mathf::lerp(-1.f, 1.f, n >= 2 ? float(j) / float(n - 1) : 0.f);
                // the bigger ones go in front, one swap a tick
                if (j > 0 && x.size > pl(state, kept.ids[j - 1]).size) {
                    std::swap(kept.ids[j], kept.ids[j - 1]);
                    set(pl(state, kept.ids[j]), flag_in_right_place, false);
                    set(pl(state, kept.ids[j - 1]), flag_in_right_place, false);
                    break;
                }
                // the body straightened, three in place in a row
                if (j >= 3 && has(pl(state, kept.ids[j - 2]), flag_in_right_place) && has(pl(state, kept.ids[j - 1]), flag_in_right_place)
                    && has(x, flag_in_right_place)) {
                    vec2 const d = custom::dir_vec(pos(state, m), pos(state, kept.ids[j - 2]));
                    vel(state, m) -= d;
                    vel(state, kept.ids[j - 2]) += d;
                }
            }
            for (uint32_t j = 0; j < n; ++j)
                pl(state, kept.ids[j]).chain_order = uint8_t(j);
            s.next_order = uint8_t(n);
            auto const first = kept.n > 0 ? kept.ids[0] : dcon::creature_id{};
            chain_consider(state, k, first, room, rng);
            if (auto const prey = chain_prey(state, s, room.id); prey && first) {
                if (s.total_mass < creature::mass_of(state, prey))
                    s.prey = dcon::creature_id{};
                else if (visual_contact(room, pos(state, first), creature::main_pos(state, prey))) {
                    s.prey_pos = creature::main_pos(state, prey);
                    s.prey_visual_counter = 0;
                }
            } else if (s.prey && !chain_prey(state, s, room.id)) {
                // (gone from the room)
                s.prey = dcon::creature_id{};
            }
            if (s.prey_visual_counter < 100 && s.prey)
                s.hunt = mathf::min(s.hunt + 0.005f, 1.f);
            else
                s.hunt = mathf::max(s.hunt - 0.005f, 0.f);
            ++s.prey_visual_counter;
        }

        // Spider.WantToFormCentipede
        bool want_to_form(sys::game_state const& state, dcon::creature_id s, rw::rng& rng) {
            auto const& x = pl(state, s);
            if (!consious(state, s) || x.no_centipede_counter >= 1)
                return false;
            if (!(mathf::pow(rng.value(), 0.3f) < x.blood_lust))
                return false;
            return rng.value() > x.light_exp && x.den_movement == 0;
        }

        // Spider.FormCentipede
        void form_chain(sys::game_state& state, dcon::creature_id c, dcon::creature_id o, room_view const& room, rw::rng& rng) {
            auto const kc = keeper(state, c);
            auto const ko = keeper(state, o);
            if (!kc) {
                if (!ko) {
                    // a new Centipede kept by `c`: it and the other
                    auto& s = cs(state, c);
                    s = chain_state{};
                    s.max_size = uint8_t(rng.range(12, 22));
                    s.next_order = 0;
                    pl(state, c).chain = uint16_t(c.index());
                    join(state, c, c);
                    join(state, c, o);
                    return;
                }
                if (members(state, room.id, ko).n + 1 < uint32_t(cs(state, ko).max_size))
                    join(state, ko, c);
                return;
            }
            if (!ko) {
                if (members(state, room.id, kc).n + 1 < uint32_t(cs(state, kc).max_size))
                    join(state, kc, o);
                return;
            }
            if (kc == ko)
                return;
            // SpiderMass.Merge: the other chain's spiders join this one, in order
            auto const lc = members(state, room.id, kc);
            auto const lo = members(state, room.id, ko);
            if (lc.n + lo.n >= uint32_t(cs(state, kc).max_size))
                return;
            for (uint32_t i = 0; i < lo.n; ++i)
                join(state, kc, lo.ids[i]);
        }

        // Spider.Move
        void move_to(sys::game_state& state, dcon::creature_id c, vec2 dest, uint32_t chain_count, rw::rng& rng) {
            auto& x = pl(state, c);
            float num = 1.f;
            if (x.chain != no_chain)
                num += float(chain_count) * 0.05f;
            vec2 const p = pos(state, c);
            vec2 const r = custom::deg_to_vec(rng.value() * 360.f);
            vel(state, c) += custom::dir_vec(p, dest) * num * 2.f + r * mathf::lerp(0.8f, 1.2f, x.size) * 2.f;
            set(x, flag_moving, true);
        }

        //
        // Paths: MovementConnections made of tile steps
        //
        // the connection out of `s` a step code makes (none: there is none)
        std::optional<ivec2> step_dest(room_view const& room, ivec2 s, uint32_t code) {
            if (code < 8) {
                ivec2 const d = custom::eight_directions[code];
                ivec2 const t = s + d;
                if (!accessible(room, t))
                    return std::nullopt;
                // OpenDiagonal: past the two corners
                if (d.x != 0 && d.y != 0 && (room.get_tile(s + ivec2{d.x, 0}).solid() || room.get_tile(s + ivec2{0, d.y}).solid()))
                    return std::nullopt;
                return t;
            }
            if (code != step_drop)
                return std::nullopt;
            // DropToFloor: off an edge over open air, to the first tile below
            // it can hold on to
            ivec2 t = s + ivec2{0, -1};
            if (!room.inside(t.x, t.y) || room.get_tile(t).solid() || accessible(room, t))
                return std::nullopt;
            for (t.y -= 1; t.y >= 0; --t.y) {
                if (room.get_tile(t).solid())
                    return std::nullopt;
                if (accessible(room, t))
                    return t;
            }
            return std::nullopt;
        }
        move move_of(uint32_t code) {
            if (code == step_drop)
                return move::drop;
            ivec2 const d = custom::eight_directions[code];
            return d.x != 0 && d.y != 0 ? move::diagonal : move::standard;
        }
        // AItile.outgoingPaths: the step codes out of `s`
        uint32_t outgoing(room_view const& room, ivec2 s, uint32_t (&codes)[9], ivec2 (&dests)[9]) {
            uint32_t n = 0;
            for (uint32_t code = 0; code <= step_drop; ++code)
                if (auto const t = step_dest(room, s, code)) {
                    codes[n] = code;
                    dests[n] = *t;
                    ++n;
                }
            return n;
        }
        // a path laid out: each step's start tile, destination and kind
        struct path_steps {
            ivec2 start[max_path];
            ivec2 dest[max_path];
            move kind[max_path];
            uint32_t n = 0;
        };
        path_steps decode(room_view const& room, path_state const& p) {
            path_steps out;
            ivec2 s{p.x, p.y};
            for (uint32_t i = 0; i < p.count && i < max_path; ++i) {
                uint32_t const code = uint32_t((p.steps >> (4u * i)) & 0xFu);
                auto const t = step_dest(room, s, code);
                if (!t)
                    break;
                out.start[out.n] = s;
                out.dest[out.n] = *t;
                out.kind[out.n] = move_of(code);
                ++out.n;
                s = *t;
            }
            return out;
        }

        // Spider.CreateRandomPath
        path_state create_random_path(room_view const& room, vec2 at, rw::rng& rng) {
            path_state p;
            ivec2 const here = tile_of(at);
            ivec2 wc = here;
            if (!accessible(room, wc)) {
                for (auto const d : custom::four_directions)
                    if (accessible(room, wc + d) && room.get_tile(wc + d).t != terrain::slope) {
                        wc += d;
                        break;
                    }
            }
            if (!accessible(room, wc))
                return p;
            p.x = int16_t(wc.x);
            p.y = int16_t(wc.y);
            ivec2 a = here;
            ivec2 starts[max_path], dests[max_path];
            uint32_t num = 0;
            for (int32_t j = 0; j < rng.range(5, 16) && num < max_path; ++j) {
                uint32_t codes[9];
                ivec2 ds[9];
                uint32_t const n = outgoing(room, wc, codes, ds);
                if (n == 0)
                    continue;
                uint32_t const i = uint32_t(rng.range(0, int32_t(n)));
                if (a == ds[i])
                    continue;
                bool fresh = true;
                for (uint32_t k = 0; k < num && fresh; ++k)
                    fresh = starts[k] != ds[i] && dests[k] != ds[i];
                if (!fresh)
                    continue;
                a = wc;
                starts[num] = wc;
                dests[num] = ds[i];
                p.steps |= uint64_t(codes[i]) << (4u * num);
                ++num;
                wc = ds[i];
            }
            p.count = uint8_t(num);
            return p;
        }

        // what Spider.TileScore needs besides the spider
        struct score_ctx {
            room_view const& room;
            // the den's distance field (denMovement), if any
            std::optional<pathing::field_id> den_field = std::nullopt;
            // the chain: kept by, its hunt and light adaption, the head, the prey
            dcon::creature_id keeper{};
            float hunt = 0.f;
            float light_adaption = 0.f;
            bool head = false;
            bool prey = false;
            vec2 prey_pos{0.f, 0.f};
            ivec2 last_shortcut{-1, -1};
        };

        // Spider.TileScore
        float tile_score(sys::game_state& state, dcon::creature_id c, score_ctx const& k, ivec2 tile, rw::rng& rng) {
            auto const& x = pl(state, c);
            room_view const& room = k.room;
            vec2 const mid = room_view::middle_of_tile(tile);
            float num = 0.f;
            if (has(x, flag_move_away))
                num += rw::distance(mid, x.move_away_from);
            if (x.den_movement != 0 && k.den_field) {
                uint16_t const d = pathing::distance(state, *k.den_field, tile);
                if (d == pathing::unreachable)
                    num -= 100.f;
                else
                    num -= float(d) / float(pathing::step_cost) * float(x.den_movement);
            }
            float const num2 = light_exposure(state, room.id, mid);
            bool const ls = k.last_shortcut.x >= 0;
            if (x.light_exp == 0.f && num2 == 0.f) {
                if (room.get_tile(tile).t == terrain::shortcut_entrance)
                    return min_value;
                for (auto const d : custom::four_directions)
                    if (room.get_tile(tile + d).t == terrain::shortcut_entrance)
                        return min_value;
                auto const& flock = lists_of(state, room.id).flock;
                for (auto const o : flock)
                    if (o != c && listed_ok(state, o, room.id) && pl(state, o).size > x.size && tile_of(pos(state, o)) == tile)
                        num -= 1.f;
                // (AItile.visibility isn't mapped: its visibility / 800 left out)
                if (pathing::narrow_space(room, tile))
                    num -= 0.01f;
                num -= float(pathing::ai_terrain_proximity(state, room.id, tile)) * 0.01f;
                if (ls)
                    num -= 10.f / rw::distance(rw::to_vec2(k.last_shortcut), rw::to_vec2(tile));
                if ((!k.keeper || k.hunt == 0.f) && x.blood_lust > 0.f && !flock.empty()) {
                    vec2 const p = pos(state, c);
                    for (int32_t n3 = 0; float(n3) < 10.f * x.blood_lust; ++n3) {
                        auto const o = flock[size_t(rng.range(0, int32_t(flock.size())))];
                        if (o != c && listed_ok(state, o, room.id) && custom::dist_less(p, pos(state, o), 200.f))
                            num -= rw::distance(p, pos(state, o)) * x.blood_lust;
                        else
                            num -= 200.f * x.blood_lust;
                    }
                }
            } else {
                float const num4 = k.keeper ? k.light_adaption : 0.f;
                num -= mathf::max(num2 - num4, 0.f) * 10000.f;
                if (ls && custom::manhattan_distance(tile, k.last_shortcut) < 2)
                    num -= 10000.f;
            }
            if (k.keeper && k.head && k.prey)
                num -= rw::distance(mid, k.prey_pos) * k.hunt;
            return num;
        }
        // Spider.ScoreOfPath (its lastFollowingConnection penalty counts the
        // spider's own path for both paths alike: it never tells them apart)
        float path_score(sys::game_state& state, dcon::creature_id c, score_ctx const& k, path_steps const& p, rw::rng& rng) {
            if (p.n == 0)
                return min_value;
            return tile_score(state, c, k, p.dest[p.n - 1], rng);
        }

        // Spider.ConsiderCreature: one creature of the room a tick
        void consider_creature(sys::game_state& state, dcon::creature_id c, room_view const& room, rw::rng& rng) {
            auto& x = pl(state, c);
            if (x.seen_no_prey < 0xFFFF)
                ++x.seen_no_prey;
            auto const& l = lists_of(state, room.id);
            if (l.creatures.empty())
                return;
            uint32_t const count = uint32_t(l.creatures.size());
            auto const t = l.creatures[size_t(rng.range(0, int32_t(count)))];
            if (!creature::here(state, t, room.id))
                return;
            vec2 const p = pos(state, c);
            vec2 const tp = creature::main_pos(state, t);
            auto const k = keeper(state, c);
            bool const t_spider = state.world.creature_get_type(t) == creature::type::spider;
            if (consider_prey(state, c, t) && visual_contact(room, p, tp)) {
                x.blood_lust = mathf::clamp01(x.blood_lust + creature::static_relationship(state, c, t).intensity / 10.f);
                x.seen_no_prey = 0;
                if (k) {
                    auto const ml = members(state, room.id, k);
                    if (ml.n > 0)
                        see_prey(state, k, ml.ids[0], t, room.id);
                }
            } else {
                if (t_spider && custom::dist_less(p, tp, 300.f) && (custom::dist_less(p, tp, 150.f) || visual_contact(room, p, tp))) {
                    auto const& o = pl(state, t);
                    if (o.blood_lust > x.blood_lust)
                        x.blood_lust = mathf::lerp(x.blood_lust, o.blood_lust, 0.3f);
                    if (o.seen_no_prey < x.seen_no_prey)
                        x.seen_no_prey = o.seen_no_prey;
                    if (auto const ok = keeper(state, t); k && ok) {
                        auto const& os = cs(state, ok);
                        auto& ms = cs(state, k);
                        if (chain_prey(state, os, room.id) && os.prey_visual_counter < ms.prey_visual_counter)
                            ms.prey_pos = os.prey_pos;
                    }
                    if (!has(x, flag_move_away) && has(o, flag_move_away) && custom::dist_less(o.move_away_from, p, 100.f)) {
                        x.move_away_from = o.move_away_from;
                        set(x, flag_move_away, true);
                    }
                }
                if (uint32_t(x.seen_no_prey) > 100u + count)
                    x.blood_lust = mathf::clamp01(x.blood_lust - 0.016666668f);
                else
                    x.blood_lust = mathf::clamp01(x.blood_lust - 1.f / (60.f * float(count)));
            }
            // too close to something big: away from it (its DangerPos: the
            // main chunk)
            if (!t_spider && !(k && cs(state, k).prey == t) && custom::dist_less(p, tp, 30.f + creature::mass_of(state, t) * 8.f)) {
                x.move_away_from = tp;
                set(x, flag_move_away, true);
            }
        }

        // Spider.PushOutOfChunk
        void push_out_of_chunk(sys::game_state& state, dcon::creature_id c, dcon::body_chunk_id k) {
            auto& w = state.world;
            auto const m = main(state, c);
            vec2& mp = w.body_chunk_get_pos(m);
            vec2& kp = w.body_chunk_get_pos(k);
            float const r = w.body_chunk_get_rad(k) + w.body_chunk_get_rad(m);
            if (!custom::dist_less(kp, mp, r))
                return;
            vec2 const a = custom::dir_vec(mp, kp);
            float const dist = rw::distance(mp, kp);
            float const ratio = w.body_chunk_get_mass(m) / (w.body_chunk_get_mass(m) + w.body_chunk_get_mass(k));
            w.body_chunk_get_vel(m) += a * (dist - r) * (1.f - ratio);
            mp += a * (dist - r) * (1.f - ratio);
            w.body_chunk_get_vel(k) -= a * (dist - r) * ratio;
            kp -= a * (dist - r) * ratio;
        }

        // Spider.TryToAttatch: a spider of a chain onto the chain's prey
        bool try_to_attach(sys::game_state& state, dcon::creature_id c, dcon::creature_id k, room_view const& room, rw::rng& rng) {
            auto& w = state.world;
            auto const prey = chain_prey(state, cs(state, k), room.id);
            if (!prey)
                return false;
            auto const m = main(state, c);
            vec2 const p = w.body_chunk_get_pos(m);
            uint32_t const n = creature::chunk_count_of(state, prey);
            for (uint32_t i = 0; i < n; ++i) {
                auto const pc = creature::chunk_of(state, prey, i);
                vec2 const cp = w.body_chunk_get_pos(pc);
                if (rng.value() < 0.033333335f && custom::dist_less(p, cp, w.body_chunk_get_rad(m) + w.body_chunk_get_rad(pc))) {
                    creature::grab(state, c, 0, prey, i);
                    return true;
                }
                if (rng.value() < 0.2f && custom::dist_less(p, cp, 60.f)) {
                    w.body_chunk_get_vel(m) += custom::dir_vec(p, cp) * 4.f;
                    return false;
                }
            }
            return false;
        }

        // Spider.Attached: holding on to its prey, eating it
        void attached(sys::game_state& state, dcon::creature_id c, room_view const& room, rw::rng& rng) {
            auto& w = state.world;
            auto& x = pl(state, c);
            auto const t = creature::grasped(w, c, 0);
            if (!creature::here(state, t, room.id) || creature::grasp_chunk(w, c, 0) >= creature::chunk_count_of(state, t)) {
                creature::release_grasp(state, c, 0);
                return;
            }
            auto const k = creature::chunk_of(state, t, creature::grasp_chunk(w, c, 0));
            set(x, flag_gfx_attached, true);
            float const prey_mass = creature::mass_of(state, t);
            if (!creature::dead(state, t)) {
                // the spiders on it weigh as much as it does: it dies
                float num = 0.f;
                creature::for_each_holder(state, room.id, t, [&](dcon::creature_id h, uint32_t) {
                    if (w.creature_get_type(h) == creature::type::spider)
                        num += creature::total_mass(state, h);
                });
                if (num >= prey_mass)
                    creature::kill(state, t, creature::tmpl::spider);
            } else if (rng.value() < 0.001f) {
                x.leeched_out = t;
            }
            auto const kp = keeper(state, c);
            if (kp)
                cs(state, kp).light_adaption = 1.f;
            auto const m = main(state, c);
            vec2& mp = w.body_chunk_get_pos(m);
            vec2& tp = w.body_chunk_get_pos(k);
            vec2 const a = custom::dir_vec(mp, tp);
            float const dist = rw::distance(mp, tp);
            float const want = w.body_chunk_get_rad(m) + w.body_chunk_get_rad(k);
            float const ratio = w.body_chunk_get_mass(m) / (w.body_chunk_get_mass(m) + w.body_chunk_get_mass(k));
            w.body_chunk_get_vel(m) += a * (dist - want) * (1.f - ratio);
            mp += a * (dist - want) * (1.f - ratio);
            w.body_chunk_get_vel(k) -= a * (dist - want) * ratio;
            tp -= a * (dist - want) * ratio;
            uint32_t const n = creature::chunk_count_of(state, t);
            for (uint32_t j = 0; j < n; ++j)
                push_out_of_chunk(state, c, creature::chunk_of(state, t, j));
            creature::for_each_holder(state, room.id, t, [&](dcon::creature_id h, uint32_t) {
                if (h == c)
                    return;
                uint32_t const hn = creature::chunk_count(state, h);
                for (uint32_t j = 0; j < hn; ++j)
                    push_out_of_chunk(state, c, creature::chunk(state, h, j));
            });
            bool let_go = creature::dead(state, t) && rng.value() < 0.01f;
            if (!let_go)
                let_go = rng.value() < 0.00083333335f;
            if (!let_go)
                let_go = w.creature_get_entering_shortcut(t).x >= 0 || !kp || cs(state, kp).total_mass < creature::mass_of(state, t);
            if (let_go)
                creature::release_grasp(state, c, 0);
        }

        // Spider.Assist: a spider of a chain crawling after the one ahead
        void assist(sys::game_state& state, dcon::creature_id c, dcon::creature_id k, room_view const& room, rw::rng& rng) {
            auto const l = members(state, room.id, k);
            int32_t const idx = l.index_of(c);
            if (idx <= 0)
                return;
            auto const master = l.ids[idx - 1];
            auto& x = pl(state, c);
            vec2 const p = pos(state, c);
            vec2 const mp = pos(state, master);
            if (has(x, flag_in_right_place) && !custom::dist_less(p, mp, 60.f))
                set(x, flag_in_right_place, false);
            if (has(x, flag_in_right_place)) {
                auto const& mx = pl(state, master);
                set(x, flag_gfx_attached, true);
                bool const in_acc = has(x, flag_in_accessible) || has(mx, flag_in_accessible);
                set(x, flag_in_accessible, in_acc);
                if (in_acc) {
                    vel(state, c) *= 0.8f;
                    vel(state, c) += rw::clamp_magnitude(mx.drag_pos - p, 10.f) / 10.f;
                }
                if (has(mx, flag_moving)) {
                    vel(state, master) += custom::dir_vec(p, mp) * (in_acc ? 1.5f : 0.75f);
                    set(x, flag_moving, true);
                }
                float num = float(idx) / 6.f;
                num = std::sin((float(cs(state, k).walk_cycle) / 40.f + num) * mathf::pi * 2.f);
                vel(state, c) += custom::perpendicular_vector(custom::dir_vec(p, mp)) * num * 0.9f;
                vec2 const a = custom::dir_vec(p, mp);
                float const d = rw::distance(p, mp);
                vec2 const push = a * (d - connect_distance(mx)) * 0.5f;
                pos(state, c) += push;
                vel(state, c) += push;
                pos(state, master) -= push;
                vel(state, master) -= push;
                return;
            }
            if (has(x, flag_in_accessible)) {
                vel(state, c) *= 0.8f;
                move_to(state, c, mp, l.n, rng);
            }
            if (custom::dist_less(pos(state, c), mp, 20.f))
                set(x, flag_in_right_place, true);
        }

        // Spider.Crawl
        void crawl(sys::game_state& state, dcon::creature_id c, room_view const& room, std::optional<ivec2> den, rw::rng& rng) {
            auto& w = state.world;
            auto& x = pl(state, c);
            auto& cr = w.spider_data_get_crawl(data(state, c));
            vec2 const p = pos(state, c);
            ivec2 const here = tile_of(p);
            if (!room.inside(here.x, here.y)) {
                creature::die(state, c);
                return;
            }
            score_ctx k{room};
            k.keeper = keeper(state, c);
            uint32_t chain_count = 0;
            if (k.keeper) {
                auto const& s = cs(state, k.keeper);
                auto const l = members(state, room.id, k.keeper);
                chain_count = l.n;
                k.hunt = s.hunt;
                k.light_adaption = s.light_adaption;
                k.head = l.n > 0 && l.ids[0] == c;
                k.prey = bool(chain_prey(state, s, room.id));
                k.prey_pos = s.prey_pos;
            }
            k.last_shortcut = cr.last_shortcut;
            bool const busy = (k.keeper && k.hunt > 0.f) || x.den_movement != 0 || has(x, flag_move_away);
            if (x.light_exp == 0.f && !busy) {
                if (x.idle_counter < 255)
                    ++x.idle_counter;
                if (!has(x, flag_idle) && x.idle_counter > 10) {
                    set(x, flag_idle, true);
                    x.light_to_move = mathf::pow(rng.value(), 1.f + x.size * 2.f) < 0.8f ? 0.f : mathf::pow(rng.value(), 0.5f + x.size * 2.f) * 0.95f;
                }
            } else if (x.light_exp > x.light_to_move || busy) {
                x.idle_counter = 0;
                set(x, flag_idle, false);
            }
            if (has(x, flag_idle)) {
                if (x.follow_type != move::none) {
                    move_to(state, c, room_view::middle_of_tile(follow_tile(x)), chain_count, rng);
                    if (tile_of(pos(state, c)) == follow_tile(x))
                        set_follow(x, ivec2{0, 0}, move::none);
                } else if (x.light_exp == 0.f && rng.value() < 0.083333336f) {
                    uint32_t codes[9];
                    ivec2 ds[9];
                    uint32_t const n = outgoing(room, here, codes, ds);
                    if (n > 0) {
                        uint32_t const i = uint32_t(rng.range(0, int32_t(n)));
                        if (move_of(codes[i]) != move::drop && light_exposure(state, room.id, room_view::middle_of_tile(ds[i])) == 0.f)
                            set_follow(x, ds[i], move_of(codes[i]));
                    }
                }
                return;
            }
            if (x.den_movement != 0 && den)
                k.den_field = pathing::field_to(state, room.id, *den, pathing::climber);
            if (x.light_exp > 0.f || busy) {
                path_state const scratch = create_random_path(room, p, rng);
                if (path_score(state, c, k, decode(room, scratch), rng) > path_score(state, c, k, decode(room, cr.path), rng))
                    cr.path = scratch;
            }
            if (x.follow_type != move::none && x.follow_type != move::standard) {
                if (cr.last_follow != follow_tile(x) || cr.last_follow_type != x.follow_type)
                    x.outside_accessible_counter = 20;
                cr.last_follow = follow_tile(x);
                cr.last_follow_type = x.follow_type;
                move_to(state, c, room_view::middle_of_tile(follow_tile(x)), chain_count, rng);
                if (tile_of(pos(state, c)) != follow_tile(x))
                    return;
            } else if (x.follow_type != move::none) {
                cr.last_follow = follow_tile(x);
                cr.last_follow_type = x.follow_type;
            }
            if (cr.path.count > 0) {
                set_follow(x, ivec2{0, 0}, move::none);
                auto const steps = decode(room, cr.path);
                ivec2 const at = tile_of(pos(state, c));
                for (uint32_t i = steps.n; i-- > 0;)
                    if (steps.start[i] == at) {
                        set_follow(x, steps.dest[i], steps.kind[i]);
                        break;
                    }
                if (x.follow_type == move::none)
                    cr.path.count = 0;
            }
            // (no pipes on its paths: a ShortCut connection never comes up)
            if (x.follow_type == move::standard || x.follow_type == move::drop)
                move_to(state, c, room_view::middle_of_tile(follow_tile(x)), chain_count, rng);
        }

        // Spider.Stun's side: the chain falls apart, it lets go
        void break_chain(sys::game_state& state, dcon::creature_id c, dcon::room_id room) {
            auto const k = keeper(state, c);
            if (!k)
                return;
            auto const l = members(state, room, k);
            for (uint32_t i = 0; i < l.n; ++i)
                pl(state, l.ids[i]).separated_counter = 255;
        }

        void stun(sys::game_state& state, dcon::creature_id c, int32_t st) {
            break_chain(state, c, creature::room_of(state, c));
            creature::release_grasp(state, c, 0);
            creature::stun(state, c, st);
        }
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::spider);
        auto const d = data(state, c);
        auto rng = rw::random(state.seed, 0, uint32_t(c.value));
        int32_t seed = int32_t(rng.next() & 0x7FFFFFFFu);
        if (state.spawn_seed >= 0)
            seed = state.spawn_seed;
        // GenerateIVars: dominance = size = the ID's first Random.value
        float size = 0.f;
        {
            rw::unity_random r{seed};
            size = r.value();
        }
        physics::object_desc desc;
        desc.gravity = 0.8f;
        desc.air_friction = 0.99f;
        desc.bounce = 0.f;
        desc.surface_friction = 0.87f;
        desc.water_friction = 0.92f;
        desc.buoyancy = 0.95f;
        auto const o = physics::create_object(state, room, desc);
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
        physics::add_chunk(state, o, at, mathf::lerp(2.f, 9.f, size), mathf::lerp(0.035f, 0.15f, size));
        physics::set_go_through_floors(state, o, true);
        w.try_create_creature_body(c, o);
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_set_grasps(c, creature::grasp_pair{});
        auto& x = pl(state, c);
        uint16_t const keep = x.data;
        x = payload{};
        x.data = keep;
        x.size = size;
        x.death_spasms = 1.f;
        x.chain = no_chain;
        x.banned_chain = no_chain;
        x.follow_type = move::none;
        x.drag_pos = at + vec2{0.f, -mathf::lerp(6.f, 12.f, size)};
        // (CycleStartUp is 1: never denMovement -1) the rain is near: home
        if (auto const* rc = rain::of_room(state, room); rc && rain::time_until_rain(*rc) < 40 * 60)
            x.den_movement = 1;
        w.spider_data_set_seed(d, seed);
        w.spider_data_set_crawl(d, crawl_state{});
        w.spider_data_set_chain(d, chain_state{});
        spider_graphics::reset(state, c);
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        creature::release_grasp(state, c, 0);
        if (pl(state, c).chain != no_chain) {
            std::vector<dcon::creature_id> spiders;
            auto const room = creature::room_of(state, c);
            gather_spiders(state, room, spiders);
            leave(state, spiders, room, c);
        }
        creature::release_target(state, c);
        creature::erase(state, c);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        // Creature.Update + PhysicalObject.Update
        creature::stun_countdown(state, c);
        physics::update_object(state, room, body(state, c), nullptr, nullptr);
        auto& x = pl(state, c);
        set(x, flag_gfx_attached, false);
        vec2 const p0 = pos(state, c);
        x.drag_pos = p0 + custom::dir_vec(p0, x.drag_pos) * connect_distance(x);
        if (x.outside_accessible_counter > 0)
            --x.outside_accessible_counter;
        if (x.no_centipede_counter > 0)
            --x.no_centipede_counter;
        if (w.creature_get_dead(c))
            x.death_spasms = mathf::max(0.f, x.death_spasms - 1.f / mathf::lerp(200.f, 400.f, rng.value()));
        ivec2 tile = tile_of(p0);
        tile.x = custom::int_clamp(tile.x, 0, room.width - 1);
        tile.y = custom::int_clamp(tile.y, 0, room.height - 1);
        bool const flag = accessible(room, tile);
        x.light_exp = light_exposure(state, room.id, p0);
        set(x, flag_moving, false);
        // a stun (Spider.Stun) breaks the chain apart and lets go
        if (w.creature_get_stun(c) > 0 && !w.creature_get_dead(c)) {
            break_chain(state, c, room.id);
            if (creature::grasped(w, c, 0))
                creature::release_grasp(state, c, 0);
        }
        if (!consious(state, c))
            return;
        consider_creature(state, c, room, rng);
        // pushed out of a pipe's mouth it isn't heading into
        if ((x.follow_type == move::none || follow_tile(x) != tile) && room.get_tile(p0).t == terrain::shortcut_entrance)
            vel(state, c) += rw::to_vec2(room.shortcut_entrance_hole_direction(tile)) * 8.f;
        // denMovement: less than a minute before the rain, off home
        if (auto const* rc = rain::of_room(state, room.id); rc && rain::time_until_rain(*rc) < 40 * 60 && rng.value() < 0.0125f
            && dens::rain_utility(state, c, room.id) > 0.f)
            x.den_movement = 1;
        std::optional<ivec2> den;
        if (x.den_movement != 0) {
            if (x.den_movement == -1 && rng.value() < 1.f / 400.f)
                x.den_movement = 0;
            // (the den pipe or the way home: dens.hpp pulls it in close to it)
            if (x.den_movement == 1)
                den = dens::home_tile(state, c, room);
            if (!den)
                x.den_movement = 0;
        }
        if (has(x, flag_move_away) && (rng.value() < 0.0125f || !custom::dist_less(pos(state, c), x.move_away_from, 150.f)))
            set(x, flag_move_away, false);
        set(x, flag_in_accessible, x.follow_type != move::drop && (x.outside_accessible_counter > 0 || flag));
        if (x.follow_type == move::none && !flag) {
            for (auto const d : custom::four_directions)
                if (accessible(room, tile + d)) {
                    vel(state, c) += rw::to_vec2(d) * 0.5f;
                    break;
                }
        }
        bool assisting = false;
        if (x.chain != no_chain) {
            x.banned_chain = x.chain;
            chain_update(state, c, room, rng);
            auto k = keeper(state, c);
            if (k && members(state, room.id, k).n < 2) {
                leave(state, room.id, c);
                k = dcon::creature_id{};
            }
            if (k && !creature::grasped(w, c, 0) && !try_to_attach(state, c, k, room, rng)) {
                auto const l = members(state, room.id, k);
                if (l.n > 0 && l.ids[0] != c) {
                    assist(state, c, k, room, rng);
                    assisting = true;
                }
            }
            k = keeper(state, c);
            if (k && (rng.value() < 1.f / mathf::lerp(1.f, 3000.f, custom::s_curve(x.blood_lust, 0.05f)) || x.den_movement != 0)) {
                x.no_centipede_counter = std::max<uint8_t>(x.no_centipede_counter, 20);
                leave(state, room.id, c);
            }
            if (x.chain != no_chain && !keeper(state, c))
                unlink(state, c);
        } else {
            x.legs_position = 0.f;
        }
        if (creature::grasped(w, c, 0)) {
            attached(state, c, room, rng);
            return;
        }
        auto const& flock = lists_of(state, room.id).flock;
        if (!flock.empty() && want_to_form(state, c, rng)) {
            auto const o = flock[size_t(rng.range(0, int32_t(flock.size())))];
            if (o != c && listed_ok(state, o, room.id) && want_to_form(state, o, rng)) {
                auto const kc = keeper(state, c), ko = keeper(state, o);
                bool const banned = x.banned_chain != no_chain && ko && uint16_t(ko.index()) == x.banned_chain;
                vec2 const p = pos(state, c), op = pos(state, o);
                if ((ko != kc || !kc) && !banned && custom::dist_less(p, op, 30.f) && room.visual_contact(p, op))
                    form_chain(state, c, o, room, rng);
            }
        }
        if (assisting)
            return;
        if (has(x, flag_in_accessible)) {
            vel(state, c) *= 0.7f;
            vel(state, c).y += w.physical_object_get_g(body(state, c));
            auto const k = keeper(state, c);
            if (!k || members(state, room.id, k).ids[0] == c)
                crawl(state, c, room, den, rng);
        } else {
            set_follow(x, ivec2{0, 0}, move::none);
            w.spider_data_get_crawl(data(state, c)).path.count = 0;
        }
    }

    void collide(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        if (w.creature_get_away(c) != 0)
            return;
        auto const& spiders = lists_of(state, room.id).spiders;
        auto const a = main(state, c);
        for (auto const o : spiders) {
            if (o.index() <= c.index() || !listed_ok(state, o, room.id))
                continue;
            auto const b = main(state, o);
            vec2& pa = w.body_chunk_get_pos(a);
            vec2& pb = w.body_chunk_get_pos(b);
            float const r = w.body_chunk_get_rad(a) + w.body_chunk_get_rad(b);
            if (!custom::dist_less(pa, pb, r))
                continue;
            float const dist = rw::distance(pa, pb);
            vec2 const dir = custom::dir_vec(pa, pb);
            float const ratio = w.body_chunk_get_mass(b) / (w.body_chunk_get_mass(a) + w.body_chunk_get_mass(b));
            pa -= dir * ((r - dist) * ratio);
            w.body_chunk_get_vel(a) -= dir * ((r - dist) * ratio);
            pb += dir * ((r - dist) * (1.f - ratio));
            w.body_chunk_get_vel(b) += dir * ((r - dist) * (1.f - ratio));
            if (pa.x == pb.x) {
                auto rng = rw::random(state.seed, state.tick, rw::ids(uint32_t(c.value), uint32_t(o.value)));
                w.body_chunk_get_vel(a) += custom::rnv(rng) * 0.0001f;
                w.body_chunk_get_vel(b) += custom::rnv(rng) * 0.0001f;
            }
        }
    }

    void violence(sys::game_state& state, dcon::creature_id c, vec2 momentum, float damage, float stun_bonus) {
        auto& w = state.world;
        auto const m = main(state, c);
        // Creature.Violence: no HealthState, a hit at its
        // instantDeathDamageLimit kills it
        w.body_chunk_get_vel(m) += rw::clamp_magnitude(momentum / w.body_chunk_get_mass(m), 10.f);
        float const dmg = damage / base_damage_resistance;
        float const st = (damage * 30.f + stun_bonus) / base_stun_resistance;
        stun(state, c, int32_t(st));
        if (dmg >= instant_death_damage_limit)
            creature::die(state, c);
    }

    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t) {
        auto& w = state.world;
        if (creature::grasped(w, c, 0) == t)
            creature::release_grasp(state, c, 0);
        auto& x = pl(state, c);
        if (x.leeched_out == t)
            x.leeched_out = dcon::creature_id{};
        if (x.chain == uint16_t(c.index())) {
            auto& s = cs(state, c);
            if (s.prey == t)
                s.prey = dcon::creature_id{};
        }
    }

    void died(sys::game_state& state, dcon::creature_id c) {
        state.world.physical_object_set_surface_friction(body(state, c), 0.4f);
    }

    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const view = world::get_room_view(state, room);
        ivec2 const t = tile_of(at);
        vec2 const dir = rw::to_vec2(view.shortcut_entrance_hole_direction(t));
        vec2 const mid = room_view::middle_of_tile(t);
        auto const m = main(state, c);
        physics::hard_set_position(state, m, mid - dir * 5.f);
        w.body_chunk_set_vel(m, dir * 5.f);
        auto& x = pl(state, c);
        x.no_centipede_counter = std::max<uint8_t>(x.no_centipede_counter, 50);
        // NewRoom: no flock, no chain (the old one's spiders let it go)
        x.chain = no_chain;
        x.separated_counter = 0;
        set(x, flag_in_right_place, false);
        set(x, flag_idle, false);
        set(x, flag_move_away, false);
        x.idle_counter = 0;
        x.den_movement = 0;
        set_follow(x, ivec2{0, 0}, move::none);
        x.drag_pos = mid - dir * (5.f + connect_distance(x));
        auto& cr = w.spider_data_get_crawl(data(state, c));
        cr.path.count = 0;
        cr.last_follow_type = move::none;
        cr.last_shortcut = t;
    }

    std::string describe(sys::game_state const& state, dcon::creature_id c) {
        auto const& x = pl(state, c);
        vec2 const p = pos(state, c);
        std::string s = "spider at " + std::to_string(int(p.x)) + "," + std::to_string(int(p.y)) + " size " + std::to_string(int(x.size * 100.f)) + "%";
        s += " lust " + std::to_string(int(x.blood_lust * 100.f)) + "%";
        if (uint32_t const n = chain_size(state, c))
            s += " in a chain of " + std::to_string(n);
        if (creature::grasped(state.world, c, 0))
            s += " eating";
        if (has(x, flag_idle))
            s += " idle";
        if (x.den_movement == 1)
            s += " going home";
        if (x.light_exp > 0.f)
            s += " in the light";
        return s;
    }

    bool spawn_spot(room_view const& room, ivec2 tile) {
        return accessible(room, tile) && room.get_tile(tile).water == 0;
    }

    attachment attached_chunk(sys::game_state const& state, dcon::creature_id c) {
        attachment a;
        auto const& w = state.world;
        auto const& x = pl(state, c);
        if (!has(x, flag_gfx_attached))
            return a;
        if (auto const t = creature::grasped(w, c, 0); t && creature::live(state, t)) {
            uint32_t const k = creature::grasp_chunk(w, c, 0);
            if (k < creature::chunk_count_of(state, t))
                a.chunk = creature::chunk_of(state, t, k);
            return a;
        }
        auto const room = creature::room_of(state, c);
        auto& spiders = scratch();
        gather_spiders(state, room, spiders);
        auto const l = members(state, spiders, room, keeper(state, c));
        int32_t const i = l.index_of(c);
        if (i > 0) {
            a.spider = l.ids[i - 1];
            a.chunk = main(state, a.spider);
        }
        return a;
    }

    chain_view chain_of(sys::game_state const& state, dcon::creature_id c) {
        chain_view v;
        auto const k = keeper(state, c);
        if (!k)
            return v;
        auto const room = creature::room_of(state, c);
        auto& spiders = scratch();
        gather_spiders(state, room, spiders);
        auto const l = members(state, spiders, room, k);
        if (l.index_of(c) < 0)
            return v;
        auto const& s = cs(state, k);
        v.in_chain = true;
        v.head = l.ids[0] == c;
        v.count = l.n;
        v.has_prey = bool(chain_prey(state, s, room));
        v.prey_pos = s.prey_pos;
        v.hunt = s.hunt;
        return v;
    }

    uint32_t chain_size(sys::game_state const& state, dcon::creature_id c) {
        auto const k = keeper(state, c);
        if (!k)
            return 0;
        auto const room = creature::room_of(state, c);
        auto& spiders = scratch();
        gather_spiders(state, room, spiders);
        return members(state, spiders, room, k).n;
    }
}
