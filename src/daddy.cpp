#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>
#include <vector>

#include "daddy.hpp"
#include "migrate.hpp"
#include "dens.hpp"
#include "rooms.hpp"
#include "daddy_graphics.hpp"
#include "daddy_internal.hpp"
#include "centipede.hpp"
#include "cicada.hpp"
#include "dropbug.hpp"
#include "items.hpp"
#include "lizard.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "rot.hpp"

namespace daddy {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;
    using namespace detail;

    namespace detail {
        template_params const& params(variant v) noexcept {
            // StaticWorld: DaddyLongLegs, BrotherLongLegs, TerrorLongLegs, HunterDaddy
            static constexpr template_params p[variant_count] = {
                {200.f, 200.f, 5.5f},
                {100.f, 2.f, 2.5f},
                {400.f, 4.f, 3.f},
                {50.f, 2.f, 2.f},
            };
            return p[uint32_t(v)];
        }

        ctx::ctx(sys::game_state& s, dcon::creature_id id, room_view const& r, std::vector<tentacle_work>& t, std::vector<ivec2>& sc)
            : state{s}, d{id}, room{r}, rng{s.world.creature_get_random(id)}, z{s.world.daddy_data_get_core(creature::daddy_data(s.world, id))}, a{s.world.daddy_data_get_ai(creature::daddy_data(s.world, id))},
              seen{s.world.daddy_data_get_ai(creature::daddy_data(s.world, id)).seen}, sees{s.world.daddy_data_get_ai(creature::daddy_data(s.world, id)).sees},
              ticks_since_seen{s.world.daddy_data_get_ai(creature::daddy_data(s.world, id)).ticks_since_seen},
              last_seen_pos{s.world.daddy_data_get_ai(creature::daddy_data(s.world, id)).last_seen_pos},
              destination{s.world.creature_get_destination(id)}, tents{t}, scratch{sc} {
            n = std::min(chunk_count(s, id), max_chunks);
            for (uint32_t i = 0; i < n; ++i)
                chunks[i] = chunk(s, id, i);
            v = s.world.creature_get_payload(id).daddy.variant;
            size_class = s.world.creature_get_payload(id).daddy.size_class != 0;
            hd = s.world.creature_get_payload(id).daddy.hd != 0;
            mother = v == variant::mother;
            prox = &pathing::ai_terrain_proximity_map(s, r.id);
        }

        // DaddyLongLegs.MiddleOfBody: the chunks' centre of mass
        vec2 ctx::middle_of_body() {
            vec2 a = pos(0) * mass(0);
            for (uint32_t i = 1; i < n; ++i)
                a += pos(i) * mass(i);
            return a / total_mass();
        }
        float ctx::total_mass() const {
            float m = 0.f;
            for (uint32_t i = 0; i < n; ++i)
                m += mass(i);
            return m;
        }
        dcon::creature_id ctx::tracked() const { return creature::live(state, seen) ? seen : dcon::creature_id{}; }

        void load(ctx& x) {
            auto& w = x.state.world;
            uint32_t const nt = tentacle_count(x.state, x.d);
            x.tents.resize(nt);
            for (uint32_t i = 0; i < nt; ++i) {
                auto& tw = x.tents[i];
                tw.part = tentacle_part(x.state, x.d, i);
                tw.id = tentacle_data(w, tw.part);
                tw.t = w.daddy_tentacle_get_t(tw.id);
                auto const segs = w.daddy_tentacle_get_segments(tw.id);
                tw.segments.assign(segs.begin(), segs.end());
                auto const gp = w.daddy_tentacle_get_grab_path(tw.id);
                tw.grab_path.assign(gp.begin(), gp.end());
                auto const cs = w.daddy_tentacle_get_chunks(tw.id);
                tw.c.assign(cs.begin(), cs.end());
            }
        }

        void store(ctx& x) {
            auto& w = x.state.world;
            for (auto& tw : x.tents) {
                w.daddy_tentacle_set_t(tw.id, tw.t);
                w.daddy_tentacle_get_segments(tw.id).load_range(tw.segments.data(), tw.segments.data() + tw.segments.size());
                w.daddy_tentacle_get_grab_path(tw.id).load_range(tw.grab_path.data(), tw.grab_path.data() + tw.grab_path.size());
                auto cs = w.daddy_tentacle_get_chunks(tw.id);
                for (uint32_t i = 0; i < cs.size() && i < tw.c.size(); ++i)
                    cs.at(i) = tw.c[i];
            }
        }

        bool target_here(sys::game_state const& state, dcon::creature_id t, dcon::room_id room) noexcept {
            return creature::here(state, t, room);
        }

        noise_source* source(ai_state& a, uint16_t id) noexcept {
            if (id == 0)
                return nullptr;
            for (uint32_t i = 0; i < a.noise.count; ++i)
                if (a.noise.sources[i].id == id)
                    return &a.noise.sources[i];
            return nullptr;
        }
        noise_source const* source(ai_state const& a, uint16_t id) noexcept {
            return source(const_cast<ai_state&>(a), id);
        }

        // DaddyAI.UpdateDynamicRelationship: StaticWorld's (no Remix: a
        // big plain Daddy ignores the Brothers)
        creature::relationship relationship_to(ctx const& x, dcon::creature_id t) {
            auto const tt = creature::template_of(x.state, t);
            if (x.size_class && x.v == variant::daddy && tt == creature::tmpl::brother_long_legs)
                return creature::relationship{};
            return creature::static_relationship(creature::template_of(x.state, x.d), tt);
        }

        // Tracker.SeeCreature: what the tentacles feel is what it knows of;
        // with one representation, something new takes the place of what it
        // hasn't felt for a while (PreyTracker's pick of the most attractive)
        void see(ctx& x, dcon::creature_id t) {
            if (!t || t == x.d || relationship_to(x, t).type != creature::rel::eats)
                return;
            if (x.seen && x.seen != t && creature::live(x.state, x.seen) && x.ticks_since_seen < 40)
                return;
            x.seen = t;
            x.a.seen_this_tick = 1;
            x.sees = 1;
            x.ticks_since_seen = 0;
            x.last_seen_pos = creature::main_pos(x.state, t);
        }

        // BestGuessForPosition: where it is while it is felt, else where it
        // was last felt (the ghosts wandering off aren't ported)
        ivec2 best_guess(ctx& x) {
            if (auto const p = x.tracked(); p && x.sees && target_here(x.state, p, x.room.id))
                return room_view::get_tile_position(creature::main_pos(x.state, p));
            return room_view::get_tile_position(x.last_seen_pos);
        }
        vec2 hunted_pos(ctx& x) { return room_view::middle_of_tile(best_guess(x)); }
    }

    namespace {
        constexpr float float_max = std::numeric_limits<float>::max();
        constexpr ivec2 four_directions_and_zero[5] = {{0, 0}, {-1, 0}, {0, -1}, {1, 0}, {0, 1}};
        constexpr ivec2 zero_and_eight_directions_diagonals_last[9] = {
            {0, 0}, {-1, 0}, {0, -1}, {1, 0}, {0, 1}, {-1, -1}, {1, -1}, {1, 1}, {-1, 1}
        };
        // CreatureTemplate.damageRestistances[Explosion, 0]
        constexpr float explosion_resistance = 0.03f;

        dcon::room_id room_of(sys::game_state const& state, dcon::creature_id d) {
            return state.world.physical_object_get_room_from_object_location(body(state, d));
        }

        float float_dist(ivec2 a, ivec2 b) { return rw::magnitude(rw::to_vec2(a - b)); }

        ivec2 clamp_to_room(room_view const& room, ivec2 t) {
            return ivec2{custom::int_clamp(t.x, 0, room.width - 1), custom::int_clamp(t.y, 0, room.height - 1)};
        }

        // Creature.Deaf
        float deaf(ctx const& x) {
            return x.z.deaf == 0 ? 0.f : mathf::pow(mathf::inverse_lerp(0.f, 120.f, float(x.z.deaf)), 0.2f);
        }

        //
        // pathing: the template's movement over the shared distance fields
        //
        // The field it follows towards its destination. A destination that
        // keeps shifting (prey walking, noises heard, the idle spot crawling
        // away from the walls) would build a field every time: vanilla's
        // pathfinder doesn't replan at once either, it works towards a new
        // destination a few steps a frame. So a field to a goal near the
        // destination (within two tiles, or a quarter of the way there)
        // serves until the destination strays further, and a new field is
        // made at most every 10 ticks while the old one still connects
        pathing::field_id destination_field(ctx& x) {
            auto& a = x.a;
            ivec2 const dest = clamp_to_room(x.room, x.destination);
            if (a.has_path_goal) {
                auto const f = pathing::field_to(x.state, x.room.id, a.path_goal, pathing::daddy);
                if (pathing::distance(x.state, f, dest) != pathing::unreachable && (x.here() != a.path_goal || dest == a.path_goal)) {
                    ivec2 const off = dest - a.path_goal;
                    int32_t const tol = std::max(2, custom::manhattan_distance(x.here(), dest) / 4);
                    if (std::max(std::abs(off.x), std::abs(off.y)) <= tol || x.state.tick - a.path_goal_tick < 10)
                        return f;
                }
            }
            a.path_goal = dest;
            a.path_goal_tick = x.state.tick;
            a.has_path_goal = 1;
            return pathing::field_to(x.state, x.room.id, dest, pathing::daddy);
        }

        // whether the daddy's own tile connects to its destination
        bool destination_reachable(ctx& x) {
            auto const f = destination_field(x);
            ivec2 const at = x.here();
            if (pathing::distance(x.state, f, at) != pathing::unreachable)
                return true;
            for (auto const d : custom::eight_directions)
                if (x.room.inside(at.x + d.x, at.y + d.y) && pathing::distance(x.state, f, at + d) != pathing::unreachable)
                    return true;
            return false;
        }

        // PathFinder.CoordinateReachableAndGetbackable. Its moves are the same
        // both ways, so a tile connected to the destination it can get to is
        // one it can get to and back from: the destination's field answers,
        // and only a daddy cut off from its destination asks for its own tile's
        bool reachable_and_getbackable(ctx& x, ivec2 t) {
            if (!x.room.inside(t.x, t.y) || !pathing::accessible(x.room, t, pathing::daddy))
                return false;
            if (destination_reachable(x))
                return pathing::distance(x.state, destination_field(x), t) != pathing::unreachable;
            auto const home = pathing::field_to(x.state, x.room.id, clamp_to_room(x.room, x.here()), pathing::daddy);
            return pathing::distance(x.state, home, t) != pathing::unreachable;
        }

        // PathFinder.GetEffectualDestination: the destination, or where it
        // is when it can't get there
        ivec2 effectual_destination(ctx& x) {
            return destination_reachable(x) ? x.destination : x.here();
        }

        // StandardPather.FollowPath from tile `from`: the next tile towards
        // the destination
        std::optional<ivec2> follow_path(ctx& x, pathing::field_id f, ivec2 from) {
            if (!x.room.inside(from.x, from.y) || !pathing::accessible(x.room, from, pathing::daddy))
                return std::nullopt;
            if (pathing::distance(x.state, f, from) == 0)
                return std::nullopt;
            return pathing::next_step(x.state, f, from);
        }

        //
        // DaddyAI's modules
        //

        // NoiseTracker.UpdateExamineSound
        void update_examine_sound(ctx& x) {
            auto& nt = x.a.noise;
            nt.sound_to_examine = 0;
            float num = float_max;
            for (uint32_t i = 0; i < nt.count; ++i) {
                auto const& s = nt.sources[i];
                if (s.slated_for_deletion)
                    continue;
                float num2 = rw::distance(x.pos(0), s.pos);
                if (!reachable_and_getbackable(x, room_view::get_tile_position(s.pos)))
                    num2 += 500.f;
                num2 += custom::lerp_map(float(s.age), float(nt.forget_time / 2), float(nt.forget_time), 0.f, 400.f);
                if (!s.who && num2 < num) {
                    num = num2;
                    nt.sound_to_examine = s.id;
                }
            }
        }

        // NoiseTracker.NoiseMatch against the tracked slugcat
        float noise_match_player(ctx& x, vec2 at) { return rw::distance(at, hunted_pos(x)); }

        // NoiseTracker.HeardNoise (ignoreSeenNoises is off for daddies;
        // every noise here is as interesting as vanilla's default)
        void heard_noise(ctx& x, sys::game_state::noise const& noise, float hearing) {
            auto& nt = x.a.noise;
            bool const muffled = x.room.point_submerged(noise.pos) || x.room.get_tile(x.pos(0)).water == 1;
            if (!custom::dist_less(x.pos(0), noise.pos, noise.strength * (1.f - deaf(x)) * hearing * (muffled ? 0.2f : 1.f)))
                return;
            float num2 = float_max;
            noise_source* src = nullptr;
            for (uint32_t i = 0; i < nt.count; ++i) {
                auto& s = nt.sources[i];
                float const m = !s.who ? rw::distance(s.pos, noise.pos) + 200.f + float(s.age) / 5.f
                                              : noise_match_player(x, noise.pos) + float(s.age) / 5.f;
                float const lim = bool(s.who) ? custom::lerp_map(float(x.ticks_since_seen), 20.f, 600.f, 200.f, 1000.f) : 300.f;
                if (m < num2 && m < lim) {
                    num2 = m;
                    src = &s;
                }
            }
            if (src != nullptr) {
                if (!src->who && src->age > 10) {
                    nt.mysterious_noises += 1.f;
                    nt.mysterious_noise_counter = 200;
                }
            } else {
                dcon::creature_id rep;
                if (bool(x.seen)) {
                    float const m = noise_match_player(x, noise.pos);
                    float const lim = custom::lerp_map(float(x.ticks_since_seen), 20.f, 600.f, 200.f, 1000.f);
                    int32_t const unseen = x.sees ? 0 : 1;
                    if (m < lim && m <= custom::lerp_map(float(unseen), 0.f, 10.f, 1000.f, 300.f))
                        rep = x.seen;
                }
                if (!rep) {
                    nt.mysterious_noises += 1.f;
                    nt.mysterious_noise_counter = 200;
                }
                if (nt.count >= max_sources) {
                    // the oldest goes (vanilla's list has no cap)
                    uint32_t oldest = 0;
                    for (uint32_t i = 1; i < nt.count; ++i)
                        if (nt.sources[i].age > nt.sources[oldest].age)
                            oldest = i;
                    nt.sources[oldest] = nt.sources[--nt.count];
                }
                src = &nt.sources[nt.count++];
                *src = noise_source{};
                src->id = nt.next_id++;
                if (nt.next_id == 0)
                    nt.next_id = 1;
                src->who = rep;
            }
            // TheorizedSource.Refresh
            src->pos = noise.pos;
            src->age = 0;
            if (bool(src->who) && src->who == x.seen) {
                // HeardThisCreature: it knows where the slugcat is again
                x.ticks_since_seen = std::min(x.ticks_since_seen, 20);
                x.last_seen_pos = noise.pos;
            }
            uint16_t const id = src->id;
            update_examine_sound(x);
            // DaddyAI.ReactToNoise (graphicsModule != null)
            if (x.state.viewed(x.room.id))
                daddy_graphics::react_to_noise(x.state, x.d, id, noise.pos, noise.strength);
            if (x.hd) {
                x.a.react_target = room_view::get_tile_position(noise.pos);
                x.a.react_noise_time = 60;
                x.a.new_idle_pos_counter = 300;
                x.destination = x.a.react_target;
                return;
            }
            // Creature.Deafen
            int32_t const df = int32_t(custom::lerp_map(noise.strength, 2000.f, 8000.f, 10.f, 200.f));
            x.z.deaf = std::max(std::max(x.z.deaf, df), std::min(500, x.z.deaf + df / 4));
        }

        // NoiseTracker.Update
        void noise_tracker_update(ctx& x) {
            auto& nt = x.a.noise;
            for (int32_t i = int32_t(nt.count) - 1; i >= 0; --i) {
                auto& s = nt.sources[i];
                if (s.slated_for_deletion) {
                    for (int32_t k = i; k + 1 < int32_t(nt.count); ++k)
                        nt.sources[k] = nt.sources[k + 1];
                    --nt.count;
                    update_examine_sound(x);
                    continue;
                }
                ++s.age;
                if (bool(s.who) && s.who != x.seen)
                    s.who = dcon::creature_id{};
                if (s.age > nt.forget_time)
                    s.slated_for_deletion = 1;
            }
            if (auto* s = source(x.a, nt.sound_to_examine); s && custom::manhattan_distance(x.here(), room_view::get_tile_position(s->pos)) < 3) {
                s->slated_for_deletion = 1;
                update_examine_sound(x);
            }
            if (nt.mysterious_noise_counter > 0)
                --nt.mysterious_noise_counter;
            else if (nt.mysterious_noises > 0.f)
                nt.mysterious_noises = mathf::max(0.f, nt.mysterious_noises - 0.0125f);
        }

        float noise_utility(ctx const& x) {
            if (x.a.noise.sound_to_examine == 0)
                return 0.f;
            return mathf::pow(mathf::inverse_lerp(1.f, 20.f, x.a.noise.mysterious_noises), 0.5f);
        }

        // the representation's ghost generation: a ghost spreads every 10 ticks
        float generation(ctx const& x) { return float(x.ticks_since_seen) / 10.f; }

        // Tracker.CreatureRepresentation.EstimatedChanceOfFinding
        float chance_of_finding(ctx const& x) {
            if (x.sees)
                return 1.f;
            float const num = (generation(x) * 10.f + float(x.ticks_since_seen)) / 4.f;
            if (num < 45.f)
                return mathf::clamp(1.f / -(1.f + std::exp(-(num / 12.f - 5.f))) + 1.007f, 0.f, 1.f);
            return 1.f / (num - 7.f) * 30.f;
        }

        // PreyTracker.Utility (PreyTracker(5, 1.1, 60, 800, 0.75), the
        // Hunter's 600 to lose it; tile distances; it eats slugcats at 1)
        float prey_utility(ctx& x) {
            auto const p = x.tracked();
            if (!p || !target_here(x.state, p, x.room.id))
                return 0.f;
            float const dist = mathf::max(1.f, float_dist(x.here(), best_guess(x)));
            float const sure_lose = x.hd ? 600.f : 800.f;
            float const num = mathf::lerp(1.f - mathf::inverse_lerp(60.f, sure_lose, dist), mathf::lerp(60.f, sure_lose, 0.25f) / dist, 0.5f);
            return mathf::min(num, chance_of_finding(x) * mathf::inverse_lerp(50.f, 25.f, generation(x)));
        }

        // DaddyAI.Update
        void ai_update(ctx& x) {
            auto& a = x.a;
            bool const listens = x.size_class || x.hd;
            float const hearing = x.hd ? 5.f : (x.z.eyes_closed < 1 ? 2.f : 0.f);
            if (listens)
                for (auto const& n : x.state.noises(x.room.id))
                    if (n.source != body(x.state, x.d))
                        heard_noise(x, n, hearing);
            x.set_current(behavior::idle);
            --a.react_noise_time;
            // UtilityComparer: prey at 1, rain at 1, noise at 0.1
            float const prey = prey_utility(x);
            float const rain = dens::rain_utility(x.state, x.d, x.room.id);
            float const noise = listens ? noise_utility(x) * 0.1f : 0.f;
            if (mathf::max(prey, mathf::max(rain, noise)) > 0.01f)
                x.set_current(rain > prey && rain >= noise ? behavior::escape_rain : prey >= noise ? behavior::hunt : behavior::examine_sound);
            if (x.hd && x.current() == behavior::idle && (a.react_noise_time > 0 || (listens && noise_utility(x) > 0.f)))
                x.set_current(behavior::examine_sound);
            ivec2& dest = x.destination;
            // (AbstractBehavior: now and then a trip to another room)
            if (x.current() == behavior::idle)
                migrate::idle_roll(x.state, x.d, x.room);
            if (x.current() == behavior::idle && migrate::want_to(x.state, x.d)) {
                if (auto const t = migrate::travel_tile(x.state, x.d, x.room))
                    dest = *t;
            } else if (x.current() == behavior::idle) {
                --a.new_idle_pos_counter;
                if (a.new_idle_pos_counter < 1 || !reachable_and_getbackable(x, dest)) {
                    int32_t const tx = x.rng.range(0, x.room.width);
                    int32_t const ty = x.rng.range(0, x.room.height);
                    if (reachable_and_getbackable(x, ivec2{tx, ty})) {
                        dest = ivec2{tx, ty};
                        a.new_idle_pos_counter = x.hd ? x.rng.range(30, 500) : x.rng.range(300, 2000);
                    }
                } else if (x.proximity(dest) < 6) {
                    ivec2 const c = dest + custom::four_directions[x.rng.range(0, 4)];
                    if (reachable_and_getbackable(x, c) && x.proximity(c) > x.proximity(dest))
                        dest = c;
                }
                if (!x.size_class && float_dist(dest, x.here()) < 7.f && x.room.visual_contact(room_view::middle_of_tile(x.here()), room_view::middle_of_tile(dest))) {
                    int32_t const dx = x.rng.range(-20, 21);
                    int32_t const dy = x.rng.range(-20, 21);
                    ivec2 const c = dest + ivec2{dx, dy};
                    if (reachable_and_getbackable(x, c) && float_dist(c, x.here()) >= 7.f)
                        dest = c;
                }
            } else if (x.current() == behavior::escape_rain) {
                // DenFinder.GetDenPosition (dens.hpp)
                if (auto const t = dens::home_tile(x.state, x.d, x.room))
                    dest = *t;
            } else if (x.current() == behavior::examine_sound) {
                if (a.react_noise_time > 0)
                    dest = a.react_target;
                else if (auto const* s = source(a, a.noise.sound_to_examine))
                    dest = room_view::get_tile_position(s->pos);
            } else if (x.current() == behavior::hunt) {
                ivec2 const g = best_guess(x);
                bool flag = false;
                for (int32_t k = 0; k < 5 && !flag; ++k)
                    if (reachable_and_getbackable(x, g + four_directions_and_zero[k])) {
                        dest = g + four_directions_and_zero[k];
                        flag = true;
                    }
                for (uint32_t k = 0; k < x.tents.size() && !flag; ++k) {
                    auto const& tw = x.tents[k];
                    if (tw.t.hunt != x.seen || tw.grab_path.empty() || !pathing::accessible(x.room, tw.grab_path[0], pathing::daddy))
                        continue;
                    for (size_t m = 0; m + 1 < tw.grab_path.size() && !flag; ++m) {
                        bool flag2 = false;
                        for (int32_t q = 0; q < 5 && !flag2; ++q)
                            flag2 = reachable_and_getbackable(x, tw.grab_path[m + 1] + four_directions_and_zero[q]);
                        if (!flag2) {
                            dest = tw.grab_path[m];
                            flag = true;
                        }
                    }
                }
                if (!flag)
                    dest = g;
            }
            // a random representation, forgotten when its ghosts are too old
            if (bool(x.seen)) {
                x.rng.range(0, 1);
                if (generation(x) > 100.f || !creature::live(x.state, x.seen) || creature::eaten(x.state, x.seen))
                    x.seen = dcon::creature_id{};
            }
            // ArtificialIntelligence.Update: the modules
            if (!a.seen_this_tick) {
                x.sees = 0;
                ++x.ticks_since_seen;
            }
            a.seen_this_tick = 0;
            if (listens)
                noise_tracker_update(x);
        }

        //
        // DaddyLongLegs
        //

        // Creature.Die
        void die(ctx& x) {
            x.state.world.creature_set_dead(x.d, 1);
        }

        // DaddyLongLegs.Stun
        void stun(ctx& x, int32_t st) {
            for (auto& tw : x.tents) {
                if (x.rng.value() < 0.5f || !x.size_class) {
                    tw.t.needed_for_locomotion = 1;
                    switch_task(x, tw, task::locomotion);
                }
            }
            if (!x.hd)
                st = x.size_class ? 0 : st / 3;
            int32_t& s = x.state.world.creature_get_stun(x.d);
            s = std::max(s, st);
        }

        // DaddyLongLegs.CheckTentaclesForAccessibleTerrain: the tentacle chunk
        // closest to the destination on a tile it can stand on
        std::optional<ivec2> check_tentacles_for_accessible_terrain(ctx& x) {
            vec2 p = x.pos(0);
            float num = float_max;
            vec2 const goal = room_view::middle_of_tile(x.destination);
            for (auto const& tw : x.tents)
                for (auto const& c : tw.c) {
                    ivec2 const t = room_view::get_tile_position(c.pos);
                    if (x.room.inside(t.x, t.y) && pathing::accessible(x.room, t, pathing::daddy) && custom::dist_less(c.pos, goal, num)) {
                        p = c.pos;
                        num = rw::distance(c.pos, goal);
                    }
                }
            if (num < float_max)
                return room_view::get_tile_position(p);
            return std::nullopt;
        }

        // DaddyLongLegs.Act
        void act(ctx& x, int32_t legs_grabbing) {
            auto& z = x.z;
            ai_update(x);
            float num = 0.6f;
            int32_t const num2 = 3;
            float num3 = 1.2f;
            float d = 0.2f;
            if (x.hd) {
                num = 0.5f;
                num3 = 1.1f;
                d = 1.f;
            }
            int32_t const nt = int32_t(x.tents.size());
            ivec2 const eff = effectual_destination(x);
            ivec2 const at = x.here();
            // (no stuckPos: the placed object daddies are kept to isn't ported)
            if (z.not_following_path_counter < 200 && eff != x.destination)
                ++z.not_following_path_counter;
            else if (z.not_following_path_counter > 0)
                --z.not_following_path_counter;
            if (z.not_following_path_counter > 100)
                for (uint32_t i = 0; i < x.n && legs_grabbing == 0; ++i) {
                    ivec2 const cp = x.state.world.body_chunk_get_contact_point(x.chunks[i]);
                    if (cp.x != 0 || cp.y != 0)
                        legs_grabbing = 1;
                }
            int32_t num5 = 0;
            if ((custom::manhattan_distance(at, eff) > num2 || z.not_following_path_counter > 100) && legs_grabbing > 0) {
                z.past_head = (z.past_head + int32_t(past_positions) - 1) % int32_t(past_positions);
                z.past[z.past_head] = at;
                z.past_count = std::min(z.past_count + 1, int32_t(past_positions));
                for (int32_t m = 40; m < z.past_count; ++m)
                    if (custom::dist_less(rw::to_vec2(at), rw::to_vec2(z.past[(z.past_head + m) % int32_t(past_positions)]), 4.f))
                        ++num5;
            }
            if (num5 > 30)
                ++z.stuck_counter;
            else
                z.stuck_counter -= 2;
            z.stuck_counter = custom::int_clamp(z.stuck_counter, 0, 200);
            if (z.stuck_counter > 100)
                for (uint32_t i = 0; i < x.n; ++i) {
                    vec2 const r = custom::rnv(x.rng);
                    x.vel(i) += r * 3.f * x.rng.value() * mathf::inverse_lerp(100.f, 200.f, float(z.stuck_counter));
                }
            if ((legs_grabbing > nt / 2 && z.moving) || z.stuck_counter > 100) {
                float best = std::numeric_limits<float>::lowest();
                int32_t num7 = -1;
                for (int32_t k = 0; k < nt; ++k) {
                    auto const& tw = x.tents[size_t(k)];
                    if (tw.t.at_grab_dest && !tw.t.hunt && release_score(tw) > best) {
                        best = release_score(tw);
                        num7 = k;
                    }
                }
                if (num7 > -1)
                    update_climb_grab_pos(x, x.tents[size_t(num7)]);
            }
            float num9 = 0.f, num10 = 0.f;
            for (auto const& tw : x.tents) {
                float num12 = mathf::pow(tw.t.chunks_gripping, 0.5f);
                if (tw.t.at_grab_dest && tw.t.float_grab_dest) {
                    num10 += mathf::pow(mathf::inverse_lerp(custom::lerp_map(float(z.stuck_counter), 0.f, 100.f, -0.1f, -1.f), 0.85f,
                                 rw::dot(rw::normalized(*tw.t.float_grab_dest - x.pos(0)), z.move_direction)), 0.8f) / float(nt);
                    num12 = mathf::lerp(num12, 1.f, 0.75f);
                }
                num9 += num12 / float(nt);
            }
            num10 = mathf::pow(num10 * num9, custom::lerp_map(float(z.stuck_counter), 100.f, 200.f, 0.8f, 0.1f));
            num9 = mathf::pow(num9, 0.3f);
            num10 = mathf::max(num10, z.squeeze_fac);
            num9 = mathf::max(num9, z.squeeze_fac);
            num9 = mathf::max(num9, z.unconditional_support);
            num10 = mathf::max(num10, z.unconditional_support);
            float num13 = 0.f;
            for (auto const& tw : x.tents)
                if (tw.t.needed_for_locomotion)
                    num13 += 1.f / float(nt);
            if (num9 < 1.f - num13) {
                float num15 = std::numeric_limits<float>::lowest();
                int32_t num16 = x.rng.range(0, nt);
                for (int32_t k = 0; k < nt; ++k) {
                    auto const& tw = x.tents[size_t(k)];
                    if (tw.t.needed_for_locomotion)
                        continue;
                    float num18 = 1000.f / mathf::lerp(tw.t.ideal_length * float(x.proximity(room_view::get_tile_position(tw.c.back().pos))), 200.f, 0.8f);
                    if (tw.t.current == task::grabbing)
                        num18 *= 0.01f;
                    if (tw.t.current == task::hunt)
                        num18 *= 0.1f;
                    if (tw.t.current == task::examine_sound)
                        num18 *= 0.6f;
                    if (num18 > num15) {
                        num15 = num18;
                        num16 = k;
                    }
                }
                x.tents[size_t(num16)].t.needed_for_locomotion = 1;
            } else if (num9 > 0.85f) {
                x.tents[size_t(x.rng.range(0, nt))].t.needed_for_locomotion = 0;
            }
            auto const o = body(x.state, x.d);
            float const g = x.state.world.physical_object_get_g(o);
            float const buoyancy = x.state.world.physical_object_get_buoyancy(o);
            for (uint32_t i = 0; i < x.n; ++i) {
                x.vel(i) *= mathf::lerp(1.f, mathf::lerp(0.95f, 0.8f, z.squeeze_fac), num9);
                x.vel(i).y += (g - buoyancy * physics::submersion(x.state, x.room, x.chunks[i])) * num9 * num3;
            }
            std::optional<std::pair<ivec2, ivec2>> mc; // MovementConnection: start, dest
            if (custom::manhattan_distance(at, eff) < num2) {
                vec2 const goal = room_view::middle_of_tile(eff);
                for (uint32_t i = 0; i < x.n; ++i)
                    x.vel(i) += rw::clamp_magnitude(goal - x.pos(0), 30.f) / 30.f * num * num10;
            } else {
                auto const f = destination_field(x);
                for (uint32_t i = 0; i < x.n && !mc; ++i)
                    for (int32_t k = 0; k < 9 && !mc; ++k) {
                        ivec2 const from = room_view::get_tile_position(x.pos(i) + rw::to_vec2(zero_and_eight_directions_diagonals_last[k]) * 20.f);
                        if (auto const step = follow_path(x, f, from))
                            mc = std::pair{from, *step};
                    }
                if (!mc)
                    if (auto const t = check_tentacles_for_accessible_terrain(x))
                        mc = std::pair{x.here(), *t};
            }
            z.moving = mc ? 1 : 0;
            if (mc) {
                // (no shortcuts nor squeezes through them here)
                physics::set_go_through_floors(x.state, o, mc->second.y < mc->first.y);
                vec2 const goal = room_view::middle_of_tile(mc->second);
                for (uint32_t i = 0; i < x.n; ++i)
                    x.vel(i) += custom::dir_vec(x.pos(0), goal) * num * num10;
                vec2 a = custom::dir_vec(rw::to_vec2(mc->first), rw::to_vec2(mc->second));
                auto const f = destination_field(x);
                ivec2 cur = mc->second;
                for (int32_t k = 0; k < 10; ++k) {
                    auto const step = follow_path(x, f, cur);
                    if (!step)
                        break;
                    a += custom::dir_vec(rw::to_vec2(cur), rw::to_vec2(*step));
                    cur = *step;
                }
                z.move_direction = rw::normalized(z.move_direction + rw::normalized(a) * d);
            } else {
                z.move_direction = rw::normalized(z.move_direction + vec2{0.f, -(num / 10.f)});
            }
        }

        // `p` is gone into the daddy (Player.PermaDie, Destroy): everything
        // else lets go of it; a creature is gone at the end of the tick
        void finish(ctx& x, dcon::creature_id p) {
            for (auto& tw : x.tents) {
                if (tw.t.grab == p)
                    tw.t.grab = dcon::creature_id{};
                if (tw.t.hunt == p)
                    tw.t.hunt = dcon::creature_id{};
            }
            if (x.seen == p)
                x.seen = dcon::creature_id{};
            // it drops what it holds, the other kinds let go of it (the
            // daddies did above), the rot too
            creature::lose_all_grasps(x.state, p);
            rooms::for_each_creature(x.state, x.room.id, [&](dcon::creature_id o) {
                if (x.state.world.creature_get_type(o) != creature::type::daddy && o != p)
                    creature::release(x.state, o, p);
            });
            rot::release(x.state, p);
            rooms::for_each_creature(x.state, x.room.id, ::creature::type::daddy, [&](dcon::creature_id o) {
                if (o == x.d)
                    return;
                auto& core = x.state.world.daddy_data_get_core(creature::daddy_data(x.state.world, o));
                for (uint32_t i = 0; i < core.eat_count;)
                    if (core.eats[i].who == p)
                        core.eats[i] = core.eats[--core.eat_count];
                    else
                        ++i;
                auto& oa = x.state.world.daddy_data_get_ai(creature::daddy_data(x.state.world, o));
                if (oa.seen == p)
                    oa.seen = dcon::creature_id{};
                for (uint32_t i = 0; i < tentacle_count(x.state, o); ++i) {
                    auto& t = x.state.world.daddy_tentacle_get_t(tentacle_of(x.state, o, i));
                    if (t.grab == p)
                        t.grab = dcon::creature_id{};
                }
            });
            creature::kill(x.state, p, creature::template_of(x.state, x.d));
            if (auto const pl = creature::player_of(x.state, p)) {
                // a slugcat stays, dead and gone
                x.state.world.player_set_eaten(pl, 1);
            } else {
                auto const c = p;
                x.state.defer(x.room.id, [c](sys::game_state& st) {
                    if (st.world.creature_is_valid(c))
                        creature::destroy(st, c);
                });
            }
        }

        // DaddyLongLegs.Eat
        void eat(ctx& x) {
            auto& w = x.state.world;
            auto& z = x.z;
            vec2 const middle = x.middle_of_body();
            for (int32_t i = int32_t(z.eat_count) - 1; i >= 0; --i) {
                auto& e = z.eats[i];
                auto const p = e.who;
                if (!target_here(x.state, p, x.room.id) || e.chunk >= creature::chunk_count_of(x.state, p)) {
                    z.eats[i] = z.eats[--z.eat_count];
                    continue;
                }
                if (e.progression > 1.f) {
                    // digestion by what it ate (the slugcat's 1)
                    if (!x.size_class)
                        z.digesting_counter = int32_t(custom::lerp_map(creature::is_slugcat(x.state.world, p) ? 1.f : creature::mass_of(x.state, p), 0.2f, 5.f, 50.f, 1100.f));
                    z.eats[i] = z.eats[--z.eat_count];
                    finish(x, p);
                    continue;
                }
                z.eyes_closed = std::max(z.eyes_closed, 15);
                float const prog = e.progression;
                e.progression += 0.0125f;
                auto const o = creature::body_of(x.state, p);
                if (prog <= 0.5f && e.progression > 0.5f) {
                    creature::kill(x.state, p, creature::template_of(x.state, x.d));
                    w.physical_object_for_each_chunk_connection_as_owner(o, [&](dcon::chunk_connection_id k) {
                        w.chunk_connection_set_type(k, physics::connection_type::pull);
                    });
                }
                float const dd = e.distance * (1.f - e.progression);
                auto const ec = creature::chunk_of(x.state, p, e.chunk);
                w.body_chunk_set_vel(ec, vec2{0.f, 0.f});
                w.body_chunk_set_pos(ec, middle + custom::dir_vec(middle, w.body_chunk_get_pos(ec)) * dd);
                uint32_t const n = creature::chunk_count_of(x.state, p);
                for (uint32_t k = 0; k < n; ++k) {
                    auto const c = creature::chunk_of(x.state, p, k);
                    w.body_chunk_get_vel(c) *= 1.f - e.progression;
                    vec2 const cp = w.body_chunk_get_pos(c);
                    w.body_chunk_set_pos(c, rw::lerp(cp, middle + custom::dir_vec(middle, cp) * dd, e.progression));
                }
                // (the slugcat's graphics body parts aren't pulled in with it)
            }
        }

        struct impact_ctx {
            sys::game_state* state;
            dcon::creature_id d;
        };

        // DaddyLongLegs.TerrainImpact
        void terrain_impact(void* user, dcon::physical_object_id o, uint32_t, ivec2, float speed, bool) {
            auto& x = *static_cast<impact_ctx*>(user);
            auto& state = *x.state;
            // (it doesn't call PhysicalObject.TerrainImpact: no noise of the impact)
            if (auto& n = state.runtime(room_of(state, x.d)).noises; !n.empty() && n.back().source == o)
                n.pop_back();
            state.play_sound(speed < 8.f ? sound::sound_id::cicada_light_terrain_impact : sound::sound_id::cicada_heavy_terrain_impact,
                room_of(state, x.d), state.world.body_chunk_get_pos(chunk(state, x.d, 0)));
        }

        // DaddyLongLegs.Update
        void update(ctx& x) {
            auto& w = x.state.world;
            auto& z = x.z;
            // Creature.Update + PhysicalObject.Update
            creature::stun_countdown(x.state, x.d);
            if (z.deaf > 0)
                --z.deaf;
            impact_ctx ic{&x.state, x.d};
            auto const o = body(x.state, x.d);
            physics::update_object(x.state, x.room, o, &terrain_impact, &ic);

            z.unconditional_support = mathf::max(0.f, z.unconditional_support - 0.025f);
            if (z.squeeze)
                z.squeeze_fac = mathf::min(1.f, z.squeeze_fac + 0.02f);
            else
                z.squeeze_fac = mathf::max(0.f, z.squeeze_fac - 0.033333335f);
            if (z.squeeze_fac > 0.8f)
                for (auto& tw : x.tents)
                    for (auto& c : tw.c)
                        c.pos = rw::lerp(c.pos, x.pos(0), custom::lerp_map(z.squeeze_fac, 0.8f, 1.f, 0.f, 0.5f));
            for (uint32_t k = 0; k < x.n; ++k)
                w.body_chunk_set_terrain_squeeze(x.chunks[k], 1.f - z.squeeze_fac);
            w.physical_object_for_each_chunk_connection_as_owner(o, [&](dcon::chunk_connection_id k) {
                w.chunk_connection_set_type(k, z.squeeze ? physics::connection_type::pull : physics::connection_type::normal);
            });
            z.squeeze = 0;
            int32_t num = 0;
            for (auto& tw : x.tents) {
                // (Downpour) tentacles heal
                float& h = w.creature_part_get_health(tw.part);
                if (h < 1.f) {
                    if (x.mother || x.hd)
                        h += 0.0012f;
                    else if (x.size_class)
                        h += 0.0003f;
                }
                if (h > 1.f)
                    h = 1.f;
                tentacle_update(x, tw);
                if (tw.t.at_grab_dest)
                    ++num;
                tw.t.retract_fac = mathf::clamp01(z.squeeze_fac);
            }
            if (z.digesting_counter > 0) {
                --z.digesting_counter;
                if (z.digesting_counter > 30)
                    z.eyes_closed = std::max(10, z.eyes_closed);
                int32_t& s = w.creature_get_stun(x.d);
                s = std::max(10, s);
            }
            if (z.eyes_closed > 0)
                --z.eyes_closed;
            eat(x);
            if (x.consious()) {
                act(x, num);
                // (Room.BackgroundNoise is 0 here)
            } else {
                if (x.hd && !x.dead())
                    x.seen = dcon::creature_id{};
                z.eyes_closed = std::max(z.eyes_closed, 15);
            }
        }


        bool being_eaten(sys::game_state const& state, dcon::creature_id p, dcon::room_id room) {
            bool found = false;
            rooms::for_each_creature(state, room, ::creature::type::daddy, [&](dcon::creature_id d) {
                auto const& core = state.world.daddy_data_get_core(creature::daddy_data(state.world, d));
                for (uint32_t i = 0; i < core.eat_count; ++i)
                    found = found || core.eats[i].who == p;
            });
            return found;
        }

        // DaddyLongLegs.Collide with a creature or slugcat: myChunk `i`, their chunk `j`
        void collide_with(sys::game_state& state, dcon::creature_id d, uint32_t i, dcon::creature_id ref, uint32_t j) {
            auto& w = state.world;
            auto& z = w.daddy_data_get_core(creature::daddy_data(w, d));
            // the tracker knows it and it eats it (CheckDaddyConsumption, the
            // weight limit, is Remix's)
            if (w.daddy_data_get_ai(creature::daddy_data(w, d)).seen != ref)
                return;
            if (!w.creature_get_payload(d).daddy.size_class && z.digesting_counter > 0)
                return;
            bool flag = false;
            for (uint32_t k = 0; k < tentacle_count(state, d) && !flag; ++k)
                flag = w.daddy_tentacle_get_t(tentacle_of(state, d, k)).grab == ref;
            for (uint32_t k = 0; k < z.eat_count && flag; ++k)
                if (z.eats[k].who == ref)
                    flag = false;
            if (!flag || z.eat_count >= max_eats)
                return;
            vec2 const middle = middle_of_body(state, d);
            vec2 const cp = w.body_chunk_get_pos(creature::chunk_of(state, ref, j));
            z.eats[z.eat_count++] = eat_object{ref, uint8_t(j), rw::distance(middle, cp), 0.f};
            state.play_sound(w.creature_get_payload(d).daddy.size_class ? sound::sound_id::daddy_digestion_init : sound::sound_id::bro_digestion_init,
                room_of(state, d), w.body_chunk_get_pos(chunk(state, d, i)));
        }

        // Custom.LineIntersection of the segments a0-a1 and b0-b1, if they cross
        std::optional<vec2> segment_intersection(vec2 a0, vec2 a1, vec2 b0, vec2 b1) {
            vec2 const r = a1 - a0, s = b1 - b0;
            float const den = r.x * s.y - r.y * s.x;
            if (den == 0.f)
                return std::nullopt;
            float const t = ((b0.x - a0.x) * s.y - (b0.y - a0.y) * s.x) / den;
            float const u = ((b0.x - a0.x) * r.y - (b0.y - a0.y) * r.x) / den;
            if (t < 0.f || t > 1.f || u < 0.f || u > 1.f)
                return std::nullopt;
            return a0 + r * t;
        }

        std::vector<tentacle_work>& work_buffer() {
            static thread_local std::vector<tentacle_work> buf;
            return buf;
        }
        std::vector<ivec2>& scratch_buffer() {
            static thread_local std::vector<ivec2> buf;
            return buf;
        }
    }

    char const* to_string(variant v) noexcept {
        switch (v) {
        case variant::daddy: return "daddy";
        case variant::brother: return "brother";
        case variant::mother: return "mother";
        case variant::hunter: return "hunter";
        // non-vanilla
        case variant::grandmother: return "grandmother";
        }
        return "?";
    }

    bool parse_variant(char const* name, variant& out) noexcept {
        for (uint32_t i = 0; i < variant_count; ++i)
            if (std::strcmp(name, to_string(variant(i))) == 0) {
                out = variant(i);
                return true;
            }
        if (std::strcmp(name, "terror") == 0) {
            out = variant::mother;
            return true;
        }
        return false;
    }

    uint32_t tube_count(sys::game_state const& state, dcon::creature_id d) noexcept {
        auto const& p = state.world.creature_get_payload(d).daddy;
        return uint32_t(p.tentacle_count) + p.dead_leg_count + p.dangler_count;
    }

    vec2 middle_of_body(sys::game_state const& state, dcon::creature_id d) noexcept {
        auto const& w = state.world;
        uint32_t const n = chunk_count(state, d);
        vec2 a{0.f, 0.f};
        float m = 0.f;
        for (uint32_t i = 0; i < n; ++i) {
            auto const c = chunk(state, d, i);
            a += w.body_chunk_get_pos(c) * w.body_chunk_get_mass(c);
            m += w.body_chunk_get_mass(c);
        }
        return a / m;
    }

    bool holds(sys::game_state const& state, dcon::creature_id d, dcon::creature_id p) noexcept {
        for (uint32_t i = 0; i < tentacle_count(state, d); ++i)
            if (state.world.daddy_tentacle_get_t(tentacle_of(state, d, i)).grab == p)
                return true;
        return false;
    }

    bool eating(sys::game_state const& state, dcon::creature_id d, dcon::creature_id p) noexcept {
        auto const& z = state.world.daddy_data_get_core(creature::daddy_data(state.world, d));
        for (uint32_t i = 0; i < z.eat_count; ++i)
            if (z.eats[i].who == p)
                return true;
        return false;
    }

    uint32_t legs_gripping(sys::game_state const& state, dcon::creature_id d) noexcept {
        uint32_t n = 0;
        for (uint32_t i = 0; i < tentacle_count(state, d); ++i)
            n += state.world.daddy_tentacle_get_t(tentacle_of(state, d, i)).at_grab_dest ? 1 : 0;
        return n;
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 at, variant v) {
        auto& w = state.world;
        auto const view = world::get_room_view(state, room);
        // never inside terrain
        if (view.get_tile(at).solid()) {
            ivec2 const t0 = room_view::get_tile_position(at);
            for (int r = 1; r < 8 && view.get_tile(at).solid(); ++r)
                for (int dy = -r; dy <= r; ++dy)
                    for (int dx = -r; dx <= r; ++dx)
                        if (view.get_tile(at).solid() && !view.get_tile(t0 + ivec2{dx, dy}).solid())
                            at = room_view::middle_of_tile(t0 + ivec2{dx, dy});
        }
        auto const d = creature::make(state, creature::type::daddy);
        rw::rng rng{state.random.next() | 1u};
        // DaddyLongLegs constructor. SizeClass: the Daddy and the Mother (and
        // a Brother before Artificer's time, which isn't modelled); colorClass
        // everywhere but Garbage Wastes and Silent Construct
        auto const region = w.room_get_region_from_region_membership(room);
        std::string_view const rname = region ? state.to_string_view(w.region_get_name(region)) : std::string_view{};
        bool const size_class = v == variant::grandmother || v == variant::mother || v == variant::daddy;
        bool color_class = size_class && (!region || (rname != "GW" && rname != "CL"));
        if (rname == "RM" || v == variant::mother || v == variant::grandmother)
            color_class = true;
        bool const hd = v == variant::hunter;
        vec3 effect, eye;
        if (hd) {
            effect = eye = vec3{0.57255f, 0.11373f, 0.22745f};
        } else if (v == variant::mother) {
            effect = eye = vec3{0.3f, 0.f, 1.f};
        } else if (v == variant::grandmother) {
            effect = eye = vec3{1.f, 0.f, 0.f};
        } else if (size_class) {
            effect = region ? w.region_get_corruption_effect_color(region) : vec3{0.f, 0.f, 1.f};
            eye = region ? w.region_get_corruption_eye_color(region) : vec3{0.f, 0.f, 1.f};
        } else {
            effect = vec3{0.7f, 0.7f, 0.4f};
            eye = vec3{0.5f, 0.3f, 0.f};
        }
        w.creature_get_payload(d).daddy.variant = v;
        w.creature_get_payload(d).daddy.size_class = size_class;
        w.creature_get_payload(d).daddy.color_class = color_class;
        w.creature_get_payload(d).daddy.hd = hd;
        w.creature_get_payload(d).daddy.effect_color = effect;
        w.creature_get_payload(d).daddy.eye_color = eye;
        w.creature_set_stun(d, 0);
        w.creature_set_dead(d, 0);
        w.creature_set_health(d, 1.f);
        w.daddy_data_set_core(creature::daddy_data(w, d), body_state{});
        ai_state ai;
        ai.noise.forget_time = hd ? 640 : 320;
        w.daddy_data_set_ai(creature::daddy_data(w, d), ai);
        w.creature_set_destination(d, room_view::get_tile_position(at));

        // Random.InitState(abstractCreature.ID.RandomSeed): the layout rolls
        rw::unity_random r{int32_t(rng.next() & 0x7FFFFFFFu)};
        if (state.spawn_seed >= 0)
            r = rw::unity_random{state.spawn_seed};
        w.creature_get_payload(d).daddy.graphics_seed = r.range(0, std::numeric_limits<int32_t>::max());
        float num = size_class ? 12.f : 8.f;
        int32_t num2 = 8, num3 = 7;
        if (v == variant::grandmother) {
            num2 = 40;
            num3 = 39;
            num = 25.f;
        } else if (v == variant::mother) {
            num2 = 16;
            num3 = 11;
            num = 18.f;
        } else if (hd) {
            num2 = num3 = 6;
            num = 4.f;
        }
        int32_t const n = r.range(4, size_class ? num2 : num3);
        physics::object_desc desc;
        desc.air_friction = 0.999f;
        desc.gravity = 0.9f;
        desc.bounce = 0.1f;
        desc.surface_friction = 0.4f;
        desc.water_friction = 0.96f;
        desc.buoyancy = 0.95f;
        if (v == variant::mother || v == variant::grandmother) {
            desc.gravity = 0.75f;
            desc.water_friction = 0.99f;
            desc.buoyancy = 0.75f;
        }
        auto const o = physics::create_object(state, room, desc);
        std::vector<vec2> list;
        std::vector<dcon::body_chunk_id> cs;
        for (int32_t i = 0; i < n; ++i) {
            float const num4 = float(i) / float(n - 1);
            float const num5 = mathf::lerp(num * 0.2f, num * mathf::lerp(0.3f, 1.f, num4), mathf::pow(r.value(), 1.f - num4));
            num -= num5;
            float const rad = num5 * 3.5f + 3.5f;
            // Creature.PlaceInRoom puts every chunk a pixel from the tile's middle
            cs.push_back(physics::add_chunk(state, o, at, rad, (hd && i < 2) ? 20.f : num5));
            list.push_back(custom::rnv(r) * rad);
        }
        for (int32_t j = 0; j < 5; ++j) {
            for (int32_t k = 0; k < n; ++k)
                for (int32_t l = 0; l < n; ++l) {
                    float const rr = (w.body_chunk_get_rad(cs[size_t(k)]) + w.body_chunk_get_rad(cs[size_t(l)])) * 0.85f;
                    if (k != l && rw::distance(list[size_t(k)], list[size_t(l)]) < rr)
                        list[size_t(l)] -= custom::dir_vec(list[size_t(l)], list[size_t(k)]) * (rr - rw::distance(list[size_t(k)], list[size_t(l)]));
                }
            for (auto& p : list)
                p *= 0.9f;
        }
        for (int32_t a = 0; a < n; ++a)
            for (int32_t b = a + 1; b < n; ++b)
                physics::add_connection(state, o, cs[size_t(a)], cs[size_t(b)], rw::distance(list[size_t(a)], list[size_t(b)]), physics::connection_type::normal, 1.f, -1.f);
        w.try_create_creature_body(d, o);
        int32_t num8 = 13, num9 = 10;
        float num10 = 400.f;
        if (v == variant::mother) {
            num8 = 13;
            num9 = 12;
            num10 = 1000.f;
        } else if (v == variant::grandmother) {
            num8 = num9 = 40;
            num10 = 10000.f;
        }
        int32_t nt;
        float num11;
        if (hd) {
            nt = 4;
            num11 = mathf::lerp(600.f, float(nt) * 115.f, 0.5f);
        } else {
            nt = r.range(5, size_class ? num8 : num9);
            num11 = mathf::lerp(size_class ? 3000.f : 1600.f, float(nt) * (size_class ? num10 : 300.f), 0.5f);
        }
        std::vector<float> lengths(size_t(nt), num11 / float(nt));
        for (int32_t k = 0; k < 5 * nt; ++k) {
            int32_t const num14 = r.range(0, nt);
            float const num15 = lengths[size_t(num14)] * r.value() * 0.3f;
            if (lengths[size_t(num14)] - num15 > 100.f) {
                lengths[size_t(r.range(0, nt))] += num15;
                lengths[size_t(num14)] -= num15;
            }
        }
        w.creature_get_payload(d).daddy.tentacle_count = uint8_t(nt);
        for (int32_t k = 0; k < nt; ++k) {
            // the tentacle part (its number, stun and health) and its data
            auto const part = creature::add_part(state, d, creature::part_kind::daddy_tentacle, uint8_t(k));
            auto const t = w.create_daddy_tentacle();
            w.creature_part_get_payload(part).daddy_tentacle = creature::daddy_tentacle_payload{uint16_t(t.index())};
            tentacle tt;
            tt.ideal_length = lengths[size_t(k)];
            tt.connected_chunk = uint8_t(k % n);
            tt.tentacle_dir = custom::deg_to_vec(mathf::lerp(0.f, 360.f, float(k) / float(nt)));
            w.daddy_tentacle_set_t(t, tt);
            // (Remix would give it at least 3 chunks)
            int32_t const nc = std::max(1, int32_t(tt.ideal_length / 40.f));
            auto chunks = w.daddy_tentacle_get_chunks(t);
            for (int32_t i = 0; i < nc; ++i) {
                tentacle_chunk c;
                c.t_pos = float(i + 1) / float(nc);
                c.rad = 3.f;
                chunks.push_back(c);
            }
        }
        w.creature_set_random(d, rng);

        place_in_room(state, d, room, at);
        daddy_graphics::create(state, d);
        return d;
    }

    void place_in_room(sys::game_state& state, dcon::creature_id d, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const view = world::get_room_view(state, room);
        // Creature.PlaceInRoom: the chunks a random pixel off the tile's middle
        vec2 const mid = room_view::middle_of_tile(at);
        uint32_t const n = chunk_count(state, d);
        for (uint32_t i = 0; i < n; ++i) {
            auto const c = chunk(state, d, i);
            physics::hard_set_position(state, c, mid + custom::rnv(w.creature_get_random(d)));
            w.body_chunk_set_vel(c, vec2{0.f, 0.f});
        }
        // NewRoom (the tentacles, pastPositions) and PlaceInRoom
        auto& tents = work_buffer();
        ctx x(state, d, view, tents, scratch_buffer());
        load(x);
        for (auto& tw : tents)
            tentacle_new_room(x, tw);
        vec2 const middle = x.middle_of_body();
        for (auto& tw : tents)
            tentacle_place(x, tw, middle);
        x.z.unconditional_support = 1.f;
        store(x);
    }

    void destroy(sys::game_state& state, dcon::creature_id d) {
        creature::erase(state, d);
    }

    void update(sys::game_state& state, dcon::creature_id d, room_view const& room) {
        auto& tents = work_buffer();
        ctx x(state, d, room, tents, scratch_buffer());
        load(x);
        update(x);
        store(x);
    }

    bool collides(sys::game_state const& state, dcon::creature_id d, dcon::creature_id t, dcon::room_id room) {
        (void)d;
        // what is being eaten is out of the collision layer
        return target_here(state, t, room) && !being_eaten(state, t, room);
    }

    void collide(sys::game_state& state, dcon::creature_id d, uint32_t i, dcon::creature_id t, uint32_t j) {
        collide_with(state, d, i, t, j);
    }

    bool trace_tentacles(sys::game_state const& state, dcon::room_id room, vec2 from, vec2 to, dcon::creature_id& hit, hit_part& part) {
        auto const& w = state.world;
        float best = 2.f;
        vec2 const dv = to - from;
        float const len2 = rw::sqr_magnitude(dv);
        // the tentacles, as appendages: the projectile's line crossing one of
        // their segments (the connected chunk, then the tentacle chunks)
        rooms::for_each_creature(state, room, ::creature::type::daddy, [&](dcon::creature_id d) {
            for (uint32_t k = 0; k < tentacle_count(state, d); ++k) {
                auto const t = tentacle_of(state, d, k);
                auto const cs = w.daddy_tentacle_get_chunks(t);
                vec2 prev = w.body_chunk_get_pos(chunk(state, d, w.daddy_tentacle_get_t(t).connected_chunk));
                for (uint32_t s = 0; s < cs.size(); ++s) {
                    vec2 const cur = cs.at(s).pos;
                    if (auto const hp = segment_intersection(from, to, prev, cur)) {
                        float const f = len2 > 0.f ? rw::distance(from, *hp) / std::sqrt(len2) : 0.f;
                        if (f < best) {
                            best = f;
                            hit = d;
                            float const seg = rw::distance(prev, cur);
                            part = hit_part{-1, int32_t(k), int32_t(s), seg > 0.f ? rw::distance(prev, *hp) / seg : 0.f};
                        }
                    }
                    prev = cur;
                }
            }
        });
        return best <= 1.f;
    }

    void violence(sys::game_state& state, dcon::creature_id d, hit_part const& part, vec2 momentum, float damage, float stun_bonus, bool explosion) {
        auto& w = state.world;
        auto const view = world::get_room_view(state, room_of(state, d));
        auto& tents = work_buffer();
        ctx x(state, d, view, tents, scratch_buffer());
        load(x);
        if (part.tentacle >= 0 && part.tentacle < int32_t(tents.size())) {
            // DaddyLongLegs.Violence on a tentacle: it takes the hit, not the body
            damage /= x.size_class ? 2.2f : 1.7f;
            stun_bonus /= x.size_class ? 2.f : 0.9f;
            auto& tw = tents[size_t(part.tentacle)];
            w.creature_part_get_health(tw.part) -= damage;
            int32_t& st = w.creature_part_get_stun(tw.part);
            st = std::max(st, int32_t(damage * 48.f + stun_bonus));
            damage = 0.f;
            stun_bonus = 0.f;
            // DaddyLongLegs.ApplyForceOnAppendage
            int32_t const ps = part.prev_segment;
            float const k = part.distance_to_next;
            if (ps > 0) {
                auto& c = tw.c[size_t(ps - 1)];
                c.pos += momentum / 0.04f * (1.f - k);
                c.vel += momentum / 0.04f * (1.f - k);
            } else {
                auto const cc = x.chunks[tw.t.connected_chunk];
                w.body_chunk_get_pos(cc) += momentum / w.body_chunk_get_mass(cc) * (1.f - k);
                w.body_chunk_get_vel(cc) += momentum / w.body_chunk_get_mass(cc) * (1.f - k);
            }
            if (ps < int32_t(tw.c.size())) {
                tw.c[size_t(ps)].pos += momentum / 0.04f * k;
                tw.c[size_t(ps)].vel += momentum / 0.04f * k;
            }
        } else if (x.hd) {
            stun(x, 200);
        }
        if (x.mother)
            damage /= 4.f;
        // Creature.Violence
        if (part.chunk >= 0 && part.chunk < int32_t(x.n))
            x.vel(uint32_t(part.chunk)) += rw::clamp_magnitude(momentum / x.mass(uint32_t(part.chunk)), 10.f);
        auto const& tp = params(x.v);
        float num = damage / tp.damage_resistance;
        float num2 = (damage * 30.f + stun_bonus) / tp.stun_resistance;
        float& health = w.creature_get_health(d);
        num2 *= 1.5f + mathf::inverse_lerp(0.5f, 0.f, health) * x.rng.value();
        if (explosion)
            num /= explosion_resistance;
        stun(x, int32_t(num2));
        health -= num;
        if (x.rng.value() < -health || health < -1.f || (health < 0.f && x.rng.value() < 0.33f))
            die(x);
        store(x);
    }

    void release(sys::game_state& state, dcon::creature_id d, dcon::creature_id ref) {
        auto& w = state.world;
        {
            auto& core = w.daddy_data_get_core(creature::daddy_data(w, d));
            for (uint32_t i = 0; i < core.eat_count;)
                if (core.eats[i].who == ref)
                    core.eats[i] = core.eats[--core.eat_count];
                else
                    ++i;
            if (auto& a = w.daddy_data_get_ai(creature::daddy_data(w, d)); a.seen == ref)
                a.seen = dcon::creature_id{};
            for (uint32_t i = 0; i < tentacle_count(state, d); ++i) {
                auto& t = w.daddy_tentacle_get_t(tentacle_of(state, d, i));
                if (t.grab == ref)
                    t.grab = dcon::creature_id{};
                if (t.hunt == ref)
                    t.hunt = dcon::creature_id{};
            }
        }
    }
}
