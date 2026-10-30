#include <cmath>
#include <vector>

#include "leech.hpp"
#include "migrate.hpp"
#include "dens.hpp"
#include "rain.hpp"
#include "items.hpp"
#include "leech_graphics.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "rooms.hpp"

namespace leech {
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    using world::terrain;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    char const* to_string(variant v) noexcept {
        switch (v) {
        case variant::leech: return "leech";
        case variant::sea: return "sea leech";
        case variant::jungle: return "jungle leech";
        }
        return "leech";
    }

    bool parse_variant(std::string_view name, variant& out) noexcept {
        if (name == "leech" || name == "red") out = variant::leech;
        else if (name == "sea") out = variant::sea;
        else if (name == "jungle") out = variant::jungle;
        else return false;
        return true;
    }

    namespace {
        // CreatureTemplate: Leech (damage and stun resistance 0.1), SeaLeech
        // (0.2, 0.2), JungleLeech (0.2, the Leech's 0.1); instantDeathDamageLimit 1,
        // visualRadius 400; the slugcat's bodySize 1
        float damage_resistance(variant v) noexcept { return v == variant::leech ? 0.1f : 0.2f; }
        float stun_resistance(variant v) noexcept { return v == variant::sea ? 0.2f : 0.1f; }
        constexpr float visual_radius = 400.f;

        creature::leech_payload& pl(sys::game_state& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).leech;
        }
        creature::leech_payload const& pl(sys::game_state const& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).leech;
        }
        dcon::leech_data_id data(sys::game_state const& state, dcon::creature_id c) {
            return creature::leech_data(state.world, c);
        }
        dcon::body_chunk_id main(sys::game_state const& state, dcon::creature_id c) {
            return chunk(state, c, 0);
        }
        vec2 main_pos(sys::game_state const& state, dcon::creature_id c) {
            return state.world.body_chunk_get_pos(main(state, c));
        }
        vec2 rnv(rw::rng& rng) {
            return custom::deg_to_vec(rng.value() * 360.f);
        }

        // ChargingAttack, Attacking
        bool charging_attack(creature::leech_payload const& x) { return x.charge_counter > 0 && x.charge_counter < 15; }

        // what it is latched on to (grasps[0]), if it can be reached
        dcon::creature_id grasped(sys::game_state const& state, dcon::creature_id c) {
            auto const t = creature::grasped(state.world, c, 0);
            return creature::live(state, t) ? t : dcon::creature_id{};
        }
        dcon::creature_id hunt_prey(sys::game_state const& state, dcon::creature_id c) {
            auto const t = pl(state, c).hunt_prey;
            return creature::live(state, t) ? t : dcon::creature_id{};
        }

        // ReallyTrulyRealizedInRoom: in the room, not in a pipe
        bool here(sys::game_state const& state, dcon::creature_id p, dcon::room_id room) {
            return creature::here(state, p, room);
        }

        // TargetBodyChunk: SeededRandomRange(ID.RandomSeed, 0, chunks)
        dcon::body_chunk_id target_chunk(sys::game_state const& state, dcon::creature_id c, dcon::creature_id p) {
            rw::unity_random r{state.world.leech_data_get_seed(data(state, c))};
            return creature::chunk_of(state, p, uint32_t(r.range(0, int32_t(creature::chunk_count_of(state, p)))));
        }

        // Room.GetTile(pos).DeepWater
        bool deep_water(room_view const& room, ivec2 t) {
            return room.get_tile(t).water == 1;
        }
        // AImap.TileAccessibleToCreature for a WaterOnly Air creature
        bool accessible(room_view const& room, ivec2 t) {
            return room.inside(t.x, t.y) && !room.get_tile(t).solid() && room.get_tile(t).water != 0;
        }

        void lose_all_grasps(sys::game_state& state, dcon::creature_id c) {
            creature::release_grasp(state, c, 0);
        }

        // Creature.Stun + Leech.Stun
        void stun(sys::game_state& state, dcon::creature_id c, int32_t st) {
            if (st > 10)
                lose_all_grasps(state, c);
            state.world.creature_set_stun(c, std::max(state.world.creature_get_stun(c), st));
        }

        // Leech.Die: it sinks a little
        void die(sys::game_state& state, dcon::creature_id c) {
            lose_all_grasps(state, c);
            state.world.physical_object_set_buoyancy(body(state, c), 0.83f);
            state.world.creature_set_dead(c, 1);
        }

        //
        // LeechSchool
        //
        dcon::creature_id keeper(sys::game_state const& state, dcon::creature_id c) {
            return dcon::creature_id{dcon::creature_id::value_base_t(pl(state, c).school)};
        }
        // LeechSchool.leeches: the room's leeches of the school, in order
        struct school_list {
            static constexpr uint32_t capacity = 64;
            dcon::creature_id ids[capacity];
            uint32_t n = 0;
        };
        // (only the room's leeches: a school never spans rooms, one left over
        // in another room after a warp makes a new one when it updates)
        school_list members(sys::game_state const& state, dcon::room_id room, dcon::creature_id k) {
            school_list l;
            rooms::for_each_creature(state, room, creature::type::leech, [&](dcon::creature_id x) {
                if (pl(state, x).school != k.index() || l.n >= school_list::capacity)
                    return;
                // in place, by their place in the school
                uint32_t i = l.n++;
                for (; i > 0 && pl(state, l.ids[i - 1]).school_order > pl(state, x).school_order; --i)
                    l.ids[i] = l.ids[i - 1];
                l.ids[i] = x;
            });
            return l;
        }

        // LeechSchool.RemoveLeech: the school stays with the others; if this
        // leech kept it, the first of them keeps it now
        void leave_school(sys::game_state& state, dcon::creature_id c) {
            auto& w = state.world;
            auto const k = keeper(state, c);
            if (k != c)
                return;
            auto const l = members(state, creature::room_of(state, c), c);
            dcon::creature_id next;
            for (uint32_t i = 0; i < l.n && !next; ++i)
                if (l.ids[i] != c)
                    next = l.ids[i];
            if (!next)
                return;
            auto const from = data(state, c), to = data(state, next);
            w.leech_data_set_prey(to, w.leech_data_get_prey(from));
            w.leech_data_set_prey_count(to, w.leech_data_get_prey_count(from));
            w.leech_data_set_next_order(to, w.leech_data_get_next_order(from));
            for (uint32_t i = 0; i < l.n; ++i)
                if (l.ids[i] != c)
                    pl(state, l.ids[i]).school = uint16_t(next.index());
        }

        // Leech.ResetSchool: out of its school into a new one of its own
        void reset_school(sys::game_state& state, dcon::creature_id c) {
            auto& w = state.world;
            leave_school(state, c);
            // the LeechSchool constructor's colour
            w.creature_get_random(c).value();
            pl(state, c).school = uint16_t(c.index());
            pl(state, c).school_order = 0;
            w.leech_data_set_prey_count(data(state, c), 0);
            w.leech_data_set_next_order(data(state, c), 1);
        }

        // LeechSchool.AddPrey
        void add_prey(sys::game_state& state, dcon::creature_id k, dcon::creature_id p) {
            auto& w = state.world;
            auto const d = data(state, k);
            auto& prey = w.leech_data_get_prey(d).items;
            uint8_t& n = w.leech_data_get_prey_count(d);
            for (uint32_t i = 0; i < n; ++i)
                if (prey[i].who == p)
                    return;
            if (n < max_prey)
                prey[n++] = school_prey{p, 0, 0, 1.f};
        }

        // LeechSchool.Merge: the other school's leeches join this one, in order
        void merge(sys::game_state& state, dcon::creature_id into, dcon::creature_id from) {
            auto& w = state.world;
            auto const k = keeper(state, into), f = keeper(state, from);
            if (k == f)
                return;
            auto const l = members(state, creature::room_of(state, into), f);
            for (uint32_t i = 0; i < l.n; ++i) {
                pl(state, l.ids[i]).school = uint16_t(k.index());
                pl(state, l.ids[i]).school_order = w.leech_data_get_next_order(data(state, k))++;
            }
            auto const fd = data(state, f);
            for (uint32_t i = 0; i < w.leech_data_get_prey_count(fd); ++i)
                if (auto const p = w.leech_data_get_prey(fd).items[i].who; creature::live(state, p))
                    add_prey(state, k, p);
        }

        // LeechSchool.BitDeadPrey: a dead one is only good for so many bites
        // (by its bodySize)
        void bit_dead_prey(sys::game_state& state, dcon::creature_id c, dcon::creature_id p) {
            auto& w = state.world;
            auto const d = data(state, keeper(state, c));
            auto& prey = w.leech_data_get_prey(d).items;
            for (uint32_t i = 0; i < w.leech_data_get_prey_count(d); ++i)
                if (prey[i].who == p) {
                    prey[i].bites_after_death -= 0.01f / creature::body_size(creature::template_of(state, p));
                    if (prey[i].bites_after_death < 0.f)
                        prey[i].leeched_out = 1;
                    return;
                }
        }

        // Creature.Grab of `p`'s chunk `j`, and the bite (Violence: Bite,
        // 0.02 damage, 3 stun)
        void latch_on(sys::game_state& state, dcon::creature_id c, dcon::creature_id p, uint32_t j) {
            auto& w = state.world;
            creature::grab(state, c, 0, p, j);
            state.play_sound(creature::is_slugcat(w, p) ? sound::sound_id::leech_attatch_player : sound::sound_id::leech_attatch_npc, creature::room_of(state, c), main_pos(state, c));
            auto const k = creature::chunk_of(state, p, j);
            vec2 const momentum = w.body_chunk_get_vel(main(state, c)) * w.body_chunk_get_mass(main(state, c));
            if (creature::is_slugcat(w, p)) {
                w.body_chunk_get_vel(k) += rw::clamp_magnitude(momentum / w.body_chunk_get_mass(k), 10.f);
                creature::stun(state, p, int32_t((0.02f * 30.f + 3.f) / 1.f));
            } else {
                creature::violence_on(state, p, j, momentum, 0.02f, 3.f, creature::damage_type::bite, creature::template_of(state, c));
            }
        }

        // Leech.ConsiderOtherCreature: prey for the school, or another school to join
        void consider(sys::game_state& state, dcon::creature_id c, room_view const& room, dcon::creature_id t) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            vec2 const at = main_pos(state, c);
            if (!here(state, t, room.id))
                return;
            uint32_t const n = creature::chunk_count_of(state, t);
            if (n == 0)
                return;
            vec2 const cp = w.body_chunk_get_pos(creature::chunk_of(state, t, uint32_t(rng.range(0, int32_t(n)))));
            if (!custom::dist_less(at, cp, 150.f) || !room.visual_contact(at, cp))
                return;
            // what it eats is prey for the school (the snails it fears aren't ported)
            dcon::creature_id const other = t;
            if (w.creature_get_type(other) != creature::type::leech) {
                if (creature::static_relationship(state, c, t).type == creature::rel::eats)
                    add_prey(state, keeper(state, c), t);
                return;
            }
            // its school left in another room (it hasn't updated since a warp)
            if (creature::room_of(state, keeper(state, other)) != room.id)
                reset_school(state, other);
            if (school_size(state, c) < school_size(state, other) || !custom::dist_less(at, cp, 50.f) || keeper(state, c) == keeper(state, other))
                return;
            bool const sea = pl(state, c).variant == variant::sea;
            if ((pl(state, other).variant == variant::sea) != sea)
                return;
            if (sea) {
                ivec2& mine = w.leech_data_get_territory(data(state, c));
                ivec2& theirs = w.leech_data_get_territory(data(state, other));
                if (mine.x >= 0 && theirs.x >= 0 && std::abs(theirs.x - mine.x) > 5)
                    return;
                if (mine.x < 0 && theirs.x >= 0)
                    mine = theirs;
                else if (mine.x >= 0 && theirs.x < 0)
                    theirs = mine;
            }
            merge(state, c, other);
        }

        // Leech.SwitchDestination
        void switch_destination(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            auto& x = pl(state, c);
            auto const d = data(state, c);
            vec2 const at = main_pos(state, c);
            ivec2 const tile = room_view::get_tile_position(at);
            auto const jitter = [&](float dx, float dy) {
                float const a = mathf::lerp(-dx, dx, rng.value());
                return vec2{a, mathf::lerp(-dy, dy, rng.value())};
            };
            ivec2& territory = w.leech_data_get_territory(d);
            if (x.variant == variant::sea && territory.x >= 0) {
                ivec2 const t{territory.x, rng.range(0, room.height)};
                bool const pick = rng.value() < 0.1f || !x.has_goal || room_view::get_tile_position(x.off_shoot_goal).x != territory.x;
                if (pick && deep_water(room, t) && room.visual_contact(at, room_view::middle_of_tile(t))) {
                    x.off_shoot_goal = room_view::middle_of_tile(t) + jitter(9.f, 9.f);
                    x.has_goal = 1;
                    w.leech_data_set_far_from_territory(d, 0);
                    return;
                }
                if (!room.visual_contact(at, room_view::middle_of_tile(t)) && ++w.leech_data_get_far_from_territory(d) > 5) {
                    w.leech_data_set_far_from_territory(d, 0);
                    territory = ivec2{-1, -1};
                }
            }
            int32_t const dx = rng.range(-10, 11);
            ivec2 t = tile + ivec2{dx, rng.range(-10, 11)};
            if (x.variant == variant::jungle) {
                physics::set_go_through_floors(state, body(state, c), false);
                if (rng.value() < 0.2f && !room.get_tile(t).solid()) {
                    x.off_shoot_goal = room_view::middle_of_tile(t) + jitter(39.f, 8.f);
                    x.has_goal = 1;
                } else {
                    t = tile + ivec2{rng.range(-10, 11), 0};
                    if (room.get_tile(t).t == terrain::floor && !room.get_tile(t).solid()) {
                        x.off_shoot_goal = room_view::middle_of_tile(t) + jitter(9.f, 2.f);
                        x.has_goal = 1;
                    }
                }
            } else if (rng.value() < 0.1f && !deep_water(room, t) && !room.visual_contact(at, room_view::middle_of_tile(t))) {
                x.off_shoot_goal = room_view::middle_of_tile(t) + jitter(9.f, 9.f);
                x.has_goal = 1;
            } else if (rng.value() < 0.05f) {
                // a pipe entrance a few tiles off (it never goes in: creatures
                // don't travel the pipes here)
                ivec2 const a = custom::four_directions[rng.range(0, 4)];
                for (int k = 1; k < 6 && room.get_tile(tile + a * k).t != terrain::solid; ++k) {
                    ivec2 const s = tile + a * k;
                    if (room.get_tile(s).t == terrain::shortcut_entrance && room.get_shortcut_data(s).type != world::shortcut_type::dead_end
                    && s != w.leech_data_get_avoid_shortcut(d)) {
                        x.off_shoot_goal = room_view::middle_of_tile(s);
                        x.has_goal = 1;
                    }
                }
            }
            if (rng.value() < 0.0055555557f)
                w.leech_data_get_avoid_shortcut(d).x = -1;
        }

        // Leech.Swim
        void swim(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            auto const d = data(state, c);
            auto const m = main(state, c);
            vec2 const at = w.body_chunk_get_pos(m);
            bool const sea = pl(state, c).variant == variant::sea;
            if (!pl(state, c).hunt_prey)
                pl(state, c).charge_counter = 0;
            vec2& mv = w.body_chunk_get_vel(m);
            if (pl(state, c).charge_counter == 0)
                mv += pl(state, c).swim_direction * 0.3f;
            else if (charging_attack(pl(state, c)))
                mv *= 0.9f;
            else
                mv += pl(state, c).swim_direction * 1.2f;
            vec2& sd = pl(state, c).swim_direction;
            sd += rnv(rng) * 0.15f;
            if (at.y < 0.f)
                sd.y += 0.1f;
            ivec2& territory = w.leech_data_get_territory(d);
            if (sea && territory.x < 0 && accessible(room, room_view::get_tile_position(at)))
                territory = room_view::get_tile_position(at);
            pl(state, c).hunt_prey = dcon::creature_id{};
            if (pl(state, c).flee_from_rain) {
                // FleeFromRainGoal: a den's pipe (dens.hpp)
                auto& x = pl(state, c);
                if (auto const t = dens::home_tile(state, c, room)) {
                    x.off_shoot_goal = room_view::middle_of_tile(*t);
                    x.has_goal = 1;
                }
            } else {
                // the school's prey: the closest one it can see, the submerged ones closer
                auto const kd = data(state, keeper(state, c));
                auto& prey = w.leech_data_get_prey(kd).items;
                uint8_t& count = w.leech_data_get_prey_count(kd);
                uint32_t const school = school_size(state, c);
                float best = 300.f;
                for (int32_t i = int32_t(count) - 1; i >= 0; --i) {
                    auto& e = prey[size_t(i)];
                    if (e.leeched_out)
                        continue;
                    auto const p = creature::live(state, e.who) ? e.who : dcon::creature_id{};
                    vec2 const tp = p ? w.body_chunk_get_pos(target_chunk(state, c, p)) : vec2{0.f, 0.f};
                    if (p && custom::dist_less(tp, at, visual_radius) && here(state, p, room.id) && room.visual_contact(tp, at)) {
                        float score = rw::distance(tp, at) / mathf::lerp(1.f, 1.f, 0.7f);
                        if (physics::submersion(state, room, target_chunk(state, c, p)) > 0.f)
                            score /= 2.f;
                        if (sea && territory.x >= 0)
                            score *= mathf::clamp(float(std::abs(territory.x - room_view::get_tile_position(tp).x)) - 3.f, 1.f, 100.f);
                        if (score < best) {
                            best = score;
                            pl(state, c).hunt_prey = p;
                        }
                        e.forget_counter = 0;
                    } else if (++e.forget_counter > int32_t(school) * 400 || !p || !here(state, p, room.id)) {
                        for (uint32_t k = uint32_t(i); k + 1 < count; ++k)
                            prey[k] = prey[k + 1];
                        --count;
                    }
                }
            }
            // (no snails click here: nothing to be afraid of)
            auto const prey = hunt_prey(state, c);
            auto school = members(state, room.id, keeper(state, c));
            // the leech ahead of it in the school after the same prey (a sea
            // leech: any leech ahead with the same prey or none)
            dcon::creature_id ahead;
            if (sea || prey) {
                bool past = false;
                for (uint32_t i = school.n; i-- > 0;) {
                    if (past && pl(state, school.ids[i]).hunt_prey == pl(state, c).hunt_prey) {
                        ahead = school.ids[i];
                        break;
                    }
                    if (school.ids[i] == c)
                        past = true;
                }
            }
            auto& x = pl(state, c);
            if (prey) {
                vec2 const tp = w.body_chunk_get_pos(target_chunk(state, c, prey));
                float const trad = w.body_chunk_get_rad(target_chunk(state, c, prey));
                if (x.charge_counter == 0) {
                    if (!sea && ahead)
                        sd += custom::dir_vec(at, rw::lerp(main_pos(state, ahead), tp, 0.1f)) * 0.5f;
                    else
                        sd += custom::dir_vec(at, tp) * (sea ? 1.5f : 0.5f);
                    for (uint32_t i = 0; i < school.n; ++i)
                        if (school.ids[i] != c && custom::dist_less(at, main_pos(state, school.ids[i]), 25.f))
                            sd += custom::dir_vec(main_pos(state, school.ids[i]), at) * 0.1f;
                    if (creature::live(state, prey) && w.creature_get_entering_shortcut(prey).x >= 0) {
                        x.off_shoot_goal = room_view::middle_of_tile(w.creature_get_entering_shortcut(prey));
                        x.has_goal = 1;
                    }
                    if (custom::dist_less(at, tp, trad + 50.f) && !custom::dist_less(at, tp, trad + 10.f) && rw::dot(rw::normalized(sd), rw::normalized(tp - at)) > 0.6f)
                        x.charge_counter = 1;
                } else {
                    sd += custom::dir_vec(at, tp) * (charging_attack(x) ? 0.25f : 0.02f);
                    if (++x.charge_counter > 30)
                        x.charge_counter = 0;
                }
            } else if (x.has_goal) {
                if (ahead) {
                    vec2 const ap = main_pos(state, ahead);
                    sd += custom::dir_vec(at, ap + custom::dir_vec(ap, w.body_chunk_get_last_pos(main(state, ahead))) * 5.f) * 1.5f;
                } else {
                    ivec2 const gt = room_view::get_tile_position(x.off_shoot_goal);
                    sd += custom::dir_vec(at, x.off_shoot_goal) * (pathing::narrow_space(room, gt) ? 0.8f : 0.2f) * (sea ? 4.f : 1.f);
                    if (room.get_tile(gt).t == terrain::shortcut_entrance) {
                        ivec2 const contact = w.body_chunk_get_contact_point(main(state, c));
                        if (rng.value() < 0.025f && (contact.x != 0 || contact.y != 0 || physics::submersion(state, room, main(state, c)) < 1.f))
                            x.has_goal = 0;
                    } else if (custom::dist_less(at, x.off_shoot_goal, 40.f) || !room.visual_contact(at, x.off_shoot_goal)) {
                        x.has_goal = 0;
                    }
                }
            } else {
                // wandering with the school, off the walls and under the surface
                ivec2 const here_t = room_view::get_tile_position(at);
                int32_t const prox = pathing::ai_terrain_proximity(state, room.id, here_t);
                if (prox < 4 && pathing::ai_terrain_proximity(state, room.id, room_view::get_tile_position(at + sd * 30.f)) < prox)
                    for (auto const dir : custom::eight_directions)
                        if (pathing::ai_terrain_proximity(state, room.id, room_view::get_tile_position(at + rw::to_vec2(dir) * 20.f)) < prox)
                            sd -= rw::to_vec2(dir) * 0.3f;
                if (at.y > room.float_water_level - 50.f)
                    sd.y -= 0.2f;
                bool alone = true;
                for (uint32_t i = 0; i < school.n; ++i) {
                    auto const o = school.ids[i];
                    if (o == c || !custom::dist_less(at, main_pos(state, o), 50.f))
                        continue;
                    alone = false;
                    sd += pl(state, o).swim_direction * 0.05f;
                    if (custom::dist_less(at, main_pos(state, o), 25.f))
                        sd += custom::dir_vec(main_pos(state, o), at) * 0.2f;
                }
                if (alone && school.n > 1)
                    ++x.loner_counter;
                else if (x.loner_counter > 0)
                    --x.loner_counter;
                if (x.loner_counter > 100)
                    reset_school(state, c);
                switch_destination(state, c, room);
            }
            sd = rw::normalized(sd);
            // one creature of the room at random (Room.abstractRoom.creatures:
            // the slugcats, then the creatures kind by kind)
            uint32_t creatures = 0;
            rooms::for_each_creature(state, room.id, [&](dcon::creature_id) { ++creatures; });
            if (creatures > 0) {
                uint32_t const pick = uint32_t(rng.range(0, int32_t(creatures)));
                uint32_t k = 0;
                dcon::creature_id other;
                rooms::for_each_creature(state, room.id, [&](dcon::creature_id o) {
                    if (k++ == pick)
                        other = o;
                });
                consider(state, c, room, other);
            }
            // a school member gone or out of sight leaves it
            school = members(state, room.id, keeper(state, c));
            if (school.n > 0) {
                auto const o = school.ids[rng.range(0, int32_t(school.n))];
                if (w.creature_get_dead(o) || !room.visual_contact(at, main_pos(state, o)))
                    reset_school(state, o);
            }
        }

        // Leech.Crawl: inching along the ground out of the water (the jungle
        // leech's way of life)
        void crawl(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            auto& x = pl(state, c);
            auto const m = main(state, c);
            vec2 const at = w.body_chunk_get_pos(m);
            vec2& mv = w.body_chunk_get_vel(m);
            bool const jungle = x.variant == variant::jungle;
            if (jungle && x.has_goal)
                x.land_walk_dir = int8_t(mathf::sign(custom::dir_vec(at, x.off_shoot_goal).x));
            // (it never goes into a pipe: creatures don't travel them here)
            if (jungle) {
                // latching on to what it bumps into and eats (Room.abstractRoom.creatures:
                // the slugcats, then the creatures kind by kind)
                if (rng.value() < 0.3f) {
                    std::vector<dcon::creature_id> all;
                    rooms::for_each_creature(state, room.id, [&](dcon::creature_id o) { all.push_back(o); });
                    uint32_t const count = uint32_t(all.size());
                    int32_t const first = rng.range(0, int32_t(count) - 1);
                    for (uint32_t i = 0; i < 5 && count > 0; ++i) {
                        auto const p = all[(i + uint32_t(first)) % count];
                        if (p == c || creature::dead(state, p) || !here(state, p, room.id)
                        || creature::static_relationship(state, c, p).type != creature::rel::eats)
                            continue;
                        bool done = false;
                        uint32_t const n = creature::chunk_count_of(state, p);
                        for (uint32_t j = 0; j < n && !done; ++j) {
                            auto const pc = creature::chunk_of(state, p, j);
                            if (custom::dist_less(at, w.body_chunk_get_pos(pc), w.body_chunk_get_rad(pc) + w.body_chunk_get_rad(m))) {
                                latch_on(state, c, p, j);
                                done = true;
                            }
                        }
                        if (done)
                            break;
                    }
                }
                if (x.has_goal) {
                    if (at.y > x.off_shoot_goal.y + 10.f && rng.value() < 0.001f)
                        physics::set_go_through_floors(state, body(state, c), true);
                    if (std::fabs(at.x - x.off_shoot_goal.x) < 10.f)
                        x.has_goal = 0;
                    if (rng.value() < 0.008f && room.get_tile(at + vec2{0.f, -18.f}).solid()) {
                        physics::set_go_through_floors(state, body(state, c), false);
                        mv += vec2{0.f, 13.f};
                    }
                } else {
                    switch_destination(state, c, room);
                }
            }
            ivec2 const t = room_view::get_tile_position(at);
            int32_t const dir = x.land_walk_dir;
            if (room.get_tile(t + ivec2{dir, 0}).t != terrain::solid && room.get_tile(t + ivec2{0, -2}).t != terrain::air
            && room.get_tile(t + ivec2{0, -1}).t == terrain::air && room.get_tile(t + ivec2{dir, -1}).t == terrain::solid)
                mv.x += float(dir) * 2.f;
            ivec2 const contact = w.body_chunk_get_contact_point(m);
            if (contact.y != -1)
                return;
            x.swim_direction = vec2{float(dir), 0.f};
            x.land_walk_cycle += 1.f / mathf::lerp(20.f, 60.f, mathf::pow(x.air_drown, 3.f));
            if (x.land_walk_cycle > 1.f) {
                state.play_sound(sound::sound_id::leech_crawl_on_ground, room.id, at);
                x.land_walk_cycle -= 1.f;
            }
            mv.x += (1.f + 0.9f * std::sin(x.land_walk_cycle * mathf::pi * 2.f)) * float(dir) * 0.2f * (1.f - mathf::pow(x.air_drown, 3.f));
            mv.x *= mathf::lerp(1.f, 0.8f, x.land_walk_cycle);
            if (contact.x == dir) {
                if (room.get_tile(at).t != terrain::slope && room.get_tile(t + ivec2{dir, 1}).t != terrain::solid) {
                    mv.y = 8.f;
                    return;
                }
                x.land_walk_dir = int8_t(-dir);
                if (jungle)
                    switch_destination(state, c, room);
            }
        }

        // Leech.PushOutOfChunk
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

        // Leech.Attached: hanging on to the slugcat, dragging it down
        void attached(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            auto& x = pl(state, c);
            auto const p = grasped(state, c);
            auto const m = main(state, c);
            if (!here(state, p, room.id) || creature::grasp_chunk(w, c, 0) >= creature::chunk_count_of(state, p)) {
                lose_all_grasps(state, c);
                return;
            }
            auto const k = creature::chunk_of(state, p, creature::grasp_chunk(w, c, 0));
            vec2& mp = w.body_chunk_get_pos(m);
            vec2& kp = w.body_chunk_get_pos(k);
            vec2 const a = custom::dir_vec(mp, kp);
            float const dist = rw::distance(mp, kp);
            float const want = w.body_chunk_get_rad(m) + w.body_chunk_get_rad(k);
            float const ratio = w.body_chunk_get_mass(m) / (w.body_chunk_get_mass(m) + w.body_chunk_get_mass(k));
            w.body_chunk_get_vel(m) += a * (dist - want) * (1.f - ratio);
            mp += a * (dist - want) * (1.f - ratio);
            w.body_chunk_get_vel(k) -= a * (dist - want) * ratio;
            kp -= a * (dist - want) * ratio;
            x.swim_direction = custom::dir_vec(mp, kp);
            uint32_t const n = creature::chunk_count_of(state, p);
            for (uint32_t i = 0; i < n; ++i)
                push_out_of_chunk(state, c, creature::chunk_of(state, p, i));
            // the pull down: harder the deeper the leech is
            float const sub = physics::submersion(state, room, m);
            w.body_chunk_get_vel(k).y -= (x.variant == variant::sea ? 0.07f : 0.025f) * sub / w.body_chunk_get_mass(k);
            bool const dead = creature::dead(state, p);
            if (x.variant == variant::jungle) {
                // it drains a slugcat's food, then its strength (a creature: a
                // little bite now and then, LeechSchool's jungle leech's)
                auto const pp = creature::player_of(state, p);
                if (!pp) {
                    if (!dead && rng.value() < 0.009f)
                        creature::violence_on(state, p, creature::grasp_chunk(w, c, 0), vec2{0.f, 0.f}, 0.02f, 3.f, creature::damage_type::bite, creature::template_of(state, c));
                    if (creature::dead(state, p)) {
                        state.play_sound(sound::sound_id::leech_detatch_npc, room.id, mp);
                        lose_all_grasps(state, c);
                    }
                    return;
                }
                int32_t& st = w.creature_get_stun(p);
                if (rng.value() < (st > 40 ? 0.009f : 0.002f)) {
                    int32_t& food = w.player_get_food(pp);
                    if (food > 0) {
                        if (rng.value() < 0.1f) {
                            st += 5;
                            --food;
                        }
                    } else {
                        w.player_set_aerobic_level(pp, 1.f);
                        w.player_set_exhausted(pp, 1);
                        st += 5;
                        if (st > 60)
                            player::die(state, pp);
                    }
                }
                if (player::dead(state, pp)) {
                    state.play_sound(sound::sound_id::leech_detatch_player, room.id, mp);
                    lose_all_grasps(state, c);
                }
                return;
            }
            // letting go now and then, out of the water, drowning, or of a dead slugcat
            bool let_go = rng.value() < 0.0025f && sub < 0.1f;
            if (!let_go && x.air_drown > 0.8f)
                let_go = rng.value() < 0.0125f;
            if (!let_go && dead)
                let_go = rng.value() < 0.01f;
            if (!let_go)
                let_go = rng.value() < 0.00083333335f;
            if (let_go) {
                state.play_sound(creature::is_slugcat(w, p) ? sound::sound_id::leech_detatch_player : sound::sound_id::leech_detatch_npc, room.id, mp);
                lose_all_grasps(state, c);
            }
        }

        // Leech.TerrainImpact
        void terrain_impact(void* user, dcon::physical_object_id, uint32_t, ivec2, float speed, bool) {
            auto const& ctx = *static_cast<std::pair<sys::game_state*, dcon::creature_id>*>(user);
            if (!grasped(*ctx.first, ctx.second) && speed > 2.f)
                stun(*ctx.first, ctx.second, 5 + int32_t(mathf::min((speed - 2.f) * 4.f, 20.f)));
        }
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 at, variant v) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::leech);
        auto const d = data(state, c);
        rw::rng rng{state.random.next() | 1u};
        int32_t seed = int32_t(rng.next() & 0x7FFFFFFFu);
        if (state.spawn_seed >= 0)
            seed = state.spawn_seed;
        physics::object_desc desc;
        desc.gravity = 0.9f;
        desc.air_friction = 0.99f;
        desc.bounce = 0.1f;
        desc.surface_friction = 0.47f;
        desc.water_friction = 0.92f;
        desc.buoyancy = 0.95f;
        auto const o = physics::create_object(state, room, desc);
        // never inside terrain
        auto const view = world::get_room_view(state, room);
        ivec2 const t0 = room_view::get_tile_position(at);
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
        physics::add_chunk(state, o, at, 4.f, v == variant::sea ? 0.15f : 0.05f);
        physics::set_go_through_floors(state, o, true);
        w.try_create_creature_body(c, o);
        auto& x = pl(state, c);
        x.variant = v;
        x.land_walk_dir = rng.value() < 0.5f ? -1 : 1;
        x.land_walk_cycle = rng.value();
        x.swim_direction = vec2{0.f, 0.f};
        x.air_drown = 0.f;
        x.charge_counter = 0;
        x.loner_counter = 0;
        x.hunt_prey = dcon::creature_id{};
        x.has_goal = 0;
        x.flee_from_rain = 0;
        w.creature_set_random(c, rng);
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_set_grasps(c, creature::grasp_pair{});
        w.leech_data_set_seed(d, seed);
        w.leech_data_set_territory(d, ivec2{-1, -1});
        w.leech_data_set_far_from_territory(d, 0);
        w.leech_data_set_avoid_shortcut(d, ivec2{-1, -1});
        // NewRoom: a school of its own
        pl(state, c).school = uint16_t(c.index());
        reset_school(state, c);
        leech_graphics::reset(state, c);
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        lose_all_grasps(state, c);
        leave_school(state, c);
        creature::erase(state, c);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto& rng = w.creature_get_random(c);
        auto& x = pl(state, c);
        auto const m = main(state, c);
        // Creature.Update + PhysicalObject.Update
        creature::stun_countdown(state, c);
        std::pair<sys::game_state*, dcon::creature_id> ctx{&state, c};
        physics::update_object(state, room, body(state, c), terrain_impact, &ctx);
        // (a school kept from another room, after a warp: one of its own)
        if (creature::room_of(state, keeper(state, c)) != room.id)
            reset_school(state, c);
        if (grasped(state, c))
            attached(state, c, room);
        else if (creature::grasped(w, c, 0))
            lose_all_grasps(state, c);
        else if (consious(state, c)) {
            // less than a minute before the rain: off to a den
            if (auto const* rc = rain::of_room(state, room.id); rc && rain::time_until_rain(*rc) < 40 * 60 && rng.value() < 0.0125f
            && dens::rain_utility(state, c, room.id) > 0.f)
                x.flee_from_rain = 1;
            if (x.flee_from_rain && x.has_goal)
                dens::home_tile(state, c, room);
            // (AbstractBehavior: now and then a trip to another room, the
            // way out its off shoot goal)
            if (!x.flee_from_rain && !x.hunt_prey) {
                migrate::idle_roll(state, c, room);
                if (auto const t = migrate::travel_tile(state, c, room)) {
                    x.off_shoot_goal = room_view::middle_of_tile(*t);
                    x.has_goal = 1;
                }
            }
            if (physics::submersion(state, room, m) > 0.2f)
                swim(state, c, room);
            else
                crawl(state, c, room);
        }
        // charging into the prey it hunts: latching on
        if (consious(state, c) && x.hunt_prey && !grasped(state, c) && x.charge_counter > 20) {
            if (auto const p = hunt_prey(state, c)) {
                vec2 const at = w.body_chunk_get_pos(m);
                uint32_t const n = creature::chunk_count_of(state, p);
                for (uint32_t j = 0; j < n; ++j) {
                    auto const k = creature::chunk_of(state, p, j);
                    if (!custom::dist_less(at, w.body_chunk_get_pos(k), w.body_chunk_get_rad(k) + w.body_chunk_get_rad(m)))
                        continue;
                    latch_on(state, c, p, j);
                    x.charge_counter = 0;
                    if (creature::dead(state, p))
                        bit_dead_prey(state, c, p);
                    x.hunt_prey = dcon::creature_id{};
                    x.land_walk_dir = rng.value() < 0.5f ? -1 : 1;
                    break;
                }
            }
        }
        // drowning in the air (not a jungle leech)
        if (!w.creature_get_dead(c) && (physics::submersion(state, room, m) > 0.f || x.variant == variant::jungle)) {
            x.air_drown = mathf::max(x.air_drown - 0.1f, 0.f);
        } else {
            x.air_drown += 0.0011111111f;
            if (x.air_drown >= 1.f && !w.creature_get_dead(c))
                die(state, c);
        }
    }

    void violence(sys::game_state& state, dcon::creature_id c, vec2 momentum, float damage, float stun_bonus) {
        auto& w = state.world;
        variant const v = pl(state, c).variant;
        auto const m = main(state, c);
        // Creature.Violence: no HealthState, so no health: a hit at its
        // instantDeathDamageLimit kills it
        w.body_chunk_get_vel(m) += rw::clamp_magnitude(momentum / w.body_chunk_get_mass(m), 10.f);
        float const dmg = damage / damage_resistance(v);
        float const st = (damage * 30.f + stun_bonus) / stun_resistance(v);
        stun(state, c, int32_t(st));
        if (dmg >= 1.f)
            die(state, c);
    }

    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id p) {
        if (creature::grasped(state.world, c, 0) == p)
            lose_all_grasps(state, c);
        if (pl(state, c).hunt_prey == p)
            pl(state, c).hunt_prey = dcon::creature_id{};
        // the school forgets it
        auto& w = state.world;
        auto const d = data(state, c);
        auto& prey = w.leech_data_get_prey(d).items;
        uint8_t& n = w.leech_data_get_prey_count(d);
        for (uint32_t i = 0; i < n;)
            if (prey[i].who == p)
                prey[i] = prey[--n];
            else
                ++i;
    }

    dcon::creature_id attached_to(sys::game_state const& state, dcon::creature_id c) noexcept {
        return grasped(state, c);
    }

    uint32_t school_size(sys::game_state const& state, dcon::creature_id c) {
        return members(state, creature::room_of(state, c), keeper(state, c)).n;
    }
}
