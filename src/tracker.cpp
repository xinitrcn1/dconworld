#include <algorithm>
#include <cmath>
#include <limits>

#include "tracker.hpp"
#include "pathing.hpp"
#include "rooms.hpp"

namespace tracker {
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;
    using creature::rel;

    track_module default_module(relationship r) noexcept {
        switch (r.type) {
        case rel::afraid: return track_module::threat;
        case rel::eats:
        case rel::attacks: return track_module::prey;
        case rel::agressive_rival: return track_module::aggression;
        default: return track_module::none;
        }
    }

    track_module module_without_aggression(relationship r) noexcept {
        auto const m = default_module(r);
        return m == track_module::aggression ? track_module::none : m;
    }

    tracked* find(tracker_state& s, dcon::creature_id t) noexcept {
        for (uint32_t i = 0; i < s.count; ++i)
            if (s.reps[i].who == t)
                return &s.reps[i];
        return nullptr;
    }

    tracked const* find(tracker_state const& s, dcon::creature_id t) noexcept {
        for (uint32_t i = 0; i < s.count; ++i)
            if (s.reps[i].who == t)
                return &s.reps[i];
        return nullptr;
    }

    float chance_of_finding(tracked const& r, int32_t see_around_corners) noexcept {
        if (r.visual)
            return 1.f;
        // TicksSinceSeen counts from seeAroundCorners on
        int32_t const ticks = r.ticks_since_seen < see_around_corners ? 0 : r.ticks_since_seen - see_around_corners;
        float const num = (10.f + float(ticks)) / 4.f;
        if (num < 45.f)
            return mathf::clamp(1.f / -(1.f + std::exp(-(num / 12.f - 5.f))) + 1.007f, 0.f, 1.f);
        return 1.f / (num - 7.f) * 30.f;
    }

    relationship dynamic_relationship(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) noexcept {
        if (auto const* r = find(of(state, c), t))
            return r->rel;
        return creature::static_relationship(state, c, t);
    }

    namespace {
        // What one module update works with; representations to drop are
        // marked (deleteMeNextFrame) and removed in one go
        struct ctx {
            sys::game_state& state;
            dcon::creature_id c;
            room_view const& room;
            ivec2 at;
            params const& p;
            hooks const& h;
            tracker_state& s;
            rw::rng& rng;
            uint32_t doomed = 0;

            ctx(sys::game_state& st, dcon::creature_id cr, room_view const& rm, ivec2 a, params const& pp, hooks const& hh)
                : state(st), c(cr), room(rm), at(a), p(pp), h(hh), s(st.world.creature_get_tracker(cr)), rng(st.world.creature_get_random(cr)) {}

            bool sees(dcon::body_chunk_id k) const { return h.sees && h.sees(h.user, k); }
            bool sees_any_chunk(dcon::creature_id t) const {
                uint32_t const n = creature::chunk_count_of(state, t);
                for (uint32_t i = 0; i < n; ++i)
                    if (sees(creature::chunk_of(state, t, i)))
                        return true;
                return false;
            }
            bool reach(ivec2 t) const { return !h.reach || h.reach(h.user, t); }
            void doom(uint32_t i) { doomed |= 1u << i; }
            bool is_doomed(uint32_t i) const { return (doomed >> i) & 1u; }
            // removes the marked ones, keeping the order
            void compact() {
                if (!doomed)
                    return;
                uint32_t n = 0;
                for (uint32_t i = 0; i < s.count; ++i)
                    if (!is_doomed(i))
                        s.reps[n++] = s.reps[i];
                for (uint32_t i = n; i < s.count; ++i)
                    s.reps[i] = tracked{};
                s.count = uint8_t(n);
                doomed = 0;
                if (s.prey && !find(s, s.prey))
                    s.prey = dcon::creature_id{};
                if (s.threat && !find(s, s.threat))
                    s.threat = dcon::creature_id{};
                if (s.aggression && !find(s, s.aggression))
                    s.aggression = dcon::creature_id{};
            }
        };

        // ArtificialIntelligence.TrackerToDiscardDeadCreature
        bool discard_dead(ctx const& x, tracked const& r) {
            return x.p.discard_dead_always || r.rel.type != rel::eats;
        }

        // TrackedPrey.Reachable
        float prey_reachable(ctx const& x, tracked const& r) {
            if (x.room.get_tile(best_guess(r)).solid())
                return 0.f;
            if (x.p.give_up_unreachable < 0)
                return 1.f;
            return mathf::inverse_lerp(float(x.p.give_up_unreachable), 0.f, float(r.unreachable_counter)) * mathf::inverse_lerp(200.f, 100.f, float(r.at_position_cant_see));
        }

        bool can_get_to(ctx const& x, tracked const& r) {
            ivec2 const g = best_guess(r);
            for (auto const d : custom::eight_directions_and_zero)
                if (x.reach(g + d))
                    return true;
            for (auto const d : custom::four_directions)
                if (x.reach(g + d * 2))
                    return true;
            return false;
        }

        // TrackedPrey.Attractiveness (no mud, no other rooms; MMF's
        // interest in others' catches is off)
        float attractiveness(ctx const& x, tracked const& r) {
            float num = r.rel.intensity;
            ivec2 const g = best_guess(r);
            float dist = rw::magnitude(rw::to_vec2(x.at - g));
            dist = mathf::lerp(mathf::pow(dist, 1.5f), 1.f, 0.5f);
            if (x.h.reach) {
                // CoordinateReachable and CoordinatePossibleToGetBackFrom
                if (!x.reach(g))
                    num /= 4.f;
                if (!can_get_to(x, r))
                    num /= 2.f;
            }
            num *= chance_of_finding(r, x.p.see_around_corners);
            num *= prey_reachable(x, r);
            return num / dist;
        }

        // ThreatCreature.CurrentThreat
        float current_threat(tracked const& r, int32_t see_around_corners) {
            return mathf::inverse_lerp(0.05f, 0.5f, mathf::pow(chance_of_finding(r, see_around_corners), 0.75f) * r.rel.intensity);
        }

        // a representation sorted into a module (SortCreatureIntoModule),
        // the module's limit kept (AddPrey, AddThreatCreature abandoning
        // the least interesting: ModuleHasAbandonedCreature forgets it)
        void sort_into_module(ctx& x, uint32_t i, relationship nr) {
            auto& r = x.s.reps[i];
            track_module const m = x.h.module ? x.h.module(nr) : default_module(nr);
            if (r.module == m)
                return;
            r.module = m;
            if (m == track_module::prey) {
                r.unreachable_counter = 0;
                r.at_position_cant_see = 0;
                r.last_best_guess = ivec2{-1, -1};
                int32_t n = 0;
                float worst = std::numeric_limits<float>::max();
                int32_t worst_i = -1;
                for (uint32_t j = 0; j < x.s.count; ++j)
                    if (!x.is_doomed(j) && x.s.reps[j].module == track_module::prey) {
                        ++n;
                        tracked q = x.s.reps[j];
                        if (j == i)
                            q.rel = nr;
                        float const a = attractiveness(x, q);
                        if (a < worst) {
                            worst = a;
                            worst_i = int32_t(j);
                        }
                    }
                if (n > x.p.prey_max && worst_i >= 0)
                    x.doom(uint32_t(worst_i));
            } else if (m == track_module::threat) {
                x.s.threat_reset_counter = 0;
                int32_t n = 0;
                float least = std::numeric_limits<float>::max();
                int32_t least_i = -1;
                for (uint32_t j = 0; j < x.s.count; ++j)
                    if (!x.is_doomed(j) && x.s.reps[j].module == track_module::threat) {
                        ++n;
                        tracked q = x.s.reps[j];
                        if (j == i)
                            q.rel = nr;
                        float const t = current_threat(q, x.p.see_around_corners);
                        if (t < least) {
                            least = t;
                            least_i = int32_t(j);
                        }
                    }
                if (n > x.p.threat_max && least_i >= 0)
                    x.doom(uint32_t(least_i));
            } else if (m == track_module::aggression) {
                // AddCreature(rep, 0.1, intensity)
                r.anger = 0.1f;
                r.base_anger = nr.intensity;
            }
        }

        // Tracker.CreatureNoticed
        tracked* notice(ctx& x, dcon::creature_id t) {
            auto& w = x.state.world;
            relationship const st = creature::static_relationship(x.state, x.c, t);
            if (creature::dead(x.state, t) && (x.p.discard_dead_always || st.type != rel::eats))
                return nullptr;
            bool const holding = creature::holds(w, x.c, t);
            tracked r;
            r.who = t;
            r.visual = 1;
            r.rel = st;
            r.last_seen = creature::main_pos(x.state, t);
            if (x.h.spotted)
                x.h.spotted(x.h.user, r, !holding);
            x.compact();
            if (x.s.count >= creature::max_tracked) {
                // the least interesting goes (maybe the new one)
                auto const score = [&](tracked const& q) {
                    float num = q.rel.intensity * 100000.f + (q.visual ? 2.f : 1.f) / (1.f + rw::magnitude(rw::to_vec2(best_guess(q) - x.at)));
                    return num / mathf::lerp(float(q.forget_counter), 100.f, 0.7f);
                };
                float least = score(r);
                int32_t least_i = -1;
                for (uint32_t j = 0; j < x.s.count; ++j)
                    if (float const v = score(x.s.reps[j]); v < least) {
                        least = v;
                        least_i = int32_t(j);
                    }
                if (least_i < 0)
                    return nullptr;
                x.doom(uint32_t(least_i));
                x.compact();
            }
            uint32_t const i = x.s.count++;
            x.s.reps[i] = r;
            x.s.reps[i].module = track_module::none;
            // EstablishDynamicRelationship: sorted with the static relationship
            sort_into_module(x, i, st);
            bool const kept = !x.is_doomed(i);
            dcon::creature_id const who = r.who;
            x.compact();
            return kept ? find(x.s, who) : nullptr;
        }

        // CreatureRepresentation.Update
        void rep_update(ctx& x, uint32_t i) {
            auto& r = x.s.reps[i];
            ++r.age;
            bool const was = r.visual != 0;
            ++r.ticks_since_seen;
            bool const there = creature::here(x.state, r.who, x.room.id);
            if (r.ticks_since_seen > x.p.see_around_corners) {
                r.visual = 0;
                if (there && x.sees_any_chunk(r.who)) {
                    r.ticks_since_seen = 0;
                    r.visual = 1;
                }
            }
            if (!r.visual) {
                ++r.forget_counter;
                if (x.p.frames_to_remember > -1 && r.forget_counter > x.p.frames_to_remember)
                    x.doom(i);
                return;
            }
            if (!there) {
                r.visual = 0;
                return;
            }
            r.last_seen = creature::main_pos(x.state, r.who);
            if (!was && x.h.spotted)
                x.h.spotted(x.h.user, r, false);
            r.forget_counter = 0;
        }

        // the room's `n`th creature (Room.creatures: the slugcats, then the
        // creatures kind by kind)
        uint32_t room_population(sys::game_state const& state, dcon::room_id room) {
            uint32_t n = 0;
            rooms::for_each_creature(state, room, [&](dcon::creature_id) { ++n; });
            return n;
        }
        dcon::creature_id room_member(sys::game_state const& state, dcon::room_id room, uint32_t n) {
            dcon::creature_id t;
            uint32_t i = 0;
            rooms::for_each_creature(state, room, [&](dcon::creature_id o) {
                if (i++ == n)
                    t = o;
            });
            return t;
        }

        // Tracker.Update
        void tracker_update(ctx& x) {
            if (x.s.count > 0) {
                uint32_t const i = uint32_t(x.rng.range(0, x.s.count));
                auto& r = x.s.reps[i];
                ivec2 const g = best_guess(r);
                vec2 const gp = room_view::middle_of_tile(g);
                // a creature not where it was thought to be is forgotten
                if (!r.visual && x.h.sees_point && x.h.sees_point(x.h.user, gp)
                && (custom::manhattan_distance(x.at, g) < 2 || rw::magnitude(rw::to_vec2(x.at - g)) < float(x.p.ghost_dismissal_range)))
                    x.doom(i);
                if (r.visual && creature::dead(x.state, r.who) && discard_dead(x, r))
                    x.doom(i);
            }
            uint32_t const pop = room_population(x.state, x.room.id);
            if (pop == 0) {
                x.compact();
                return;
            }
            dcon::creature_id const t = room_member(x.state, x.room.id, uint32_t(x.rng.range(0, int32_t(pop))));
            bool notice_it = t && t != x.c;
            for (int32_t i = int32_t(x.s.count) - 1; i >= 0; --i) {
                rep_update(x, uint32_t(i));
                if (x.s.reps[i].who == t)
                    notice_it = false;
            }
            x.compact();
            if (notice_it && creature::here(x.state, t, x.room.id) && x.sees_any_chunk(t))
                notice(x, t);
        }

        // RelationshipTracker.Update
        void relationship_update(ctx& x) {
            for (int32_t i = int32_t(x.s.count) - 1; i >= 0; --i) {
                if (x.is_doomed(uint32_t(i)))
                    continue;
                auto& r = x.s.reps[i];
                relationship const nr = x.h.relate ? x.h.relate(x.h.user, r) : creature::static_relationship(x.state, x.c, r.who);
                if (nr.type != r.rel.type)
                    sort_into_module(x, uint32_t(i), nr);
                r.rel = nr;
            }
            x.compact();
        }

        // TrackedPrey.Update
        void tracked_prey_update(ctx& x, tracked& r) {
            ivec2 const g = best_guess(r);
            bool ok = x.reach(g);
            for (int i = 0; i < 4 && !ok; ++i)
                ok = x.reach(g + custom::four_directions[i]);
            r.unreachable_counter = ok ? 0 : r.unreachable_counter + 1;
            ivec2 const dest = x.state.world.creature_get_destination(x.c);
            if (r.last_best_guess == g && rw::magnitude(rw::to_vec2(x.at - g)) < 5.f && rw::magnitude(rw::to_vec2(dest - g)) < 5.f
            && x.room.visual_contact(room_view::middle_of_tile(x.at), room_view::middle_of_tile(g)))
                r.at_position_cant_see += 5;
            else
                --r.at_position_cant_see;
            r.at_position_cant_see = custom::int_clamp(r.at_position_cant_see, 0, 200);
            r.last_best_guess = g;
        }

        // PreyTracker.Update
        void prey_update(ctx& x) {
            float best = std::numeric_limits<float>::lowest();
            dcon::creature_id cur;
            for (int32_t i = int32_t(x.s.count) - 1; i >= 0; --i) {
                auto& r = x.s.reps[i];
                if (r.module != track_module::prey)
                    continue;
                tracked_prey_update(x, r);
                float a = attractiveness(x, r);
                r.forget_counter = 0;
                if (r.who == x.s.prey)
                    a *= x.p.persistance_bias;
                if (a > best) {
                    best = a;
                    cur = r.who;
                }
            }
            x.s.prey = cur;
            constexpr float speed = 0.0125f;
            if (auto const* r = find(x.s, cur); r && !can_get_to(x, *r))
                x.s.frustration = mathf::clamp(x.s.frustration + speed, 0.f, 1.f);
            else
                x.s.frustration = mathf::clamp(x.s.frustration - speed * 4.f, 0.f, 1.f);
        }

        // ThreatTracker.Update
        void threat_update(ctx& x) {
            auto& s = x.s;
            s.current_threat = threat_of_area(s, x.at, x.p.see_around_corners);
            if (s.current_threat <= 0.f)
                ++s.threat_reset_counter;
            else
                s.threat_reset_counter = 0;
            if (s.flee_anti_flicker > 0)
                --s.flee_anti_flicker;
            bool any = false;
            for (uint32_t i = 0; i < s.count; ++i)
                any = any || s.reps[i].module == track_module::threat;
            if (s.threat_reset_counter < 200) {
                float num = 0.f;
                s.threat = dcon::creature_id{};
                for (int32_t i = int32_t(s.count) - 1; i >= 0; --i) {
                    auto& r = s.reps[i];
                    if (r.module != track_module::threat)
                        continue;
                    r.forget_counter = 0;
                    if (float const t = current_threat(r, x.p.see_around_corners); t > num) {
                        num = t;
                        s.threat = r.who;
                    }
                }
                if (s.threat)
                    s.current_threat = mathf::pow(s.current_threat, 1.f / (1.f + num * 3.f));
            } else if (any) {
                for (uint32_t i = 0; i < s.count; ++i)
                    if (s.reps[i].module == track_module::threat)
                        x.doom(i);
                x.compact();
                s.threat = dcon::creature_id{};
            }
        }

        // AgressionTracker.Update
        void aggression_update(ctx& x) {
            auto& s = x.s;
            s.highest_anger = 0.f;
            s.aggression = dcon::creature_id{};
            auto const* pr = prey(s);
            auto const tp = creature::template_of(x.state, x.c);
            float const attraction = creature::attraction_value(x.state.world.room_get_attraction(x.room.id).v[size_t(tp)]);
            for (int32_t i = int32_t(s.count) - 1; i >= 0; --i) {
                auto& r = s.reps[i];
                if (r.module != track_module::aggression)
                    continue;
                ivec2 const g = best_guess(r);
                float num = r.base_anger * mathf::inverse_lerp(10.f + r.base_anger * 70.f, 5.f, float(custom::manhattan_distance(x.at, g)));
                if (pr)
                    num = mathf::lerp(num, mathf::inverse_lerp(10.f, 3.f, float(custom::manhattan_distance(g, best_guess(*pr)))), 0.5f);
                num *= mathf::inverse_lerp(0.3f, 0.6f, attraction);
                if (r.anger < num)
                    r.anger = mathf::clamp(r.anger + x.p.anger_up, 0.f, num);
                else if (r.anger > num)
                    r.anger = mathf::clamp(r.anger - x.p.anger_down, num, 1.f);
                r.forget_counter = 0;
                if (r.anger > s.highest_anger) {
                    s.highest_anger = r.anger;
                    s.aggression = r.who;
                }
            }
        }
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room, ivec2 at, params const& p, hooks const& h) {
        ctx x{state, c, room, at, p, h};
        tracker_update(x);
        relationship_update(x);
        prey_update(x);
        threat_update(x);
        aggression_update(x);
    }

    void see(sys::game_state& state, dcon::creature_id c, room_view const& room, dcon::creature_id t, hooks const& h) {
        if (!t || t == c)
            return;
        auto& s = of(state, c);
        if (auto* r = find(s, t)) {
            // MakeVisible
            r->ticks_since_seen = 0;
            r->visual = 1;
            return;
        }
        if (!creature::here(state, t, room.id))
            return;
        params const p;
        ctx x{state, c, room, world::room_view::get_tile_position(creature::main_pos(state, c)), p, h};
        notice(x, t);
    }

    void forget(sys::game_state& state, dcon::creature_id c, dcon::creature_id t) {
        auto& s = of(state, c);
        for (uint32_t i = 0; i < s.count; ++i)
            if (s.reps[i].who == t) {
                for (uint32_t j = i + 1; j < s.count; ++j)
                    s.reps[j - 1] = s.reps[j];
                s.reps[--s.count] = tracked{};
                break;
            }
        if (s.prey == t)
            s.prey = dcon::creature_id{};
        if (s.threat == t)
            s.threat = dcon::creature_id{};
        if (s.aggression == t)
            s.aggression = dcon::creature_id{};
    }

    void clear(sys::game_state& state, dcon::creature_id c) {
        state.world.creature_set_tracker(c, tracker_state{});
    }

    float prey_utility(sys::game_state& state, dcon::creature_id c, room_view const& room, ivec2 at, params const& p) {
        auto const& s = of(state, c);
        auto const* r = prey(s);
        if (!r)
            return 0.f;
        float const d = mathf::max(0.01f, rw::magnitude(rw::to_vec2(at - best_guess(*r))));
        float const num = mathf::lerp(1.f - mathf::inverse_lerp(p.sure_to_get, p.sure_to_lose, d), mathf::lerp(p.sure_to_get, p.sure_to_lose, 0.25f) / d, 0.5f);
        float reach = 1.f;
        if (room.get_tile(best_guess(*r)).solid())
            reach = 0.f;
        else if (p.give_up_unreachable >= 0)
            reach = mathf::inverse_lerp(float(p.give_up_unreachable), 0.f, float(r->unreachable_counter)) * mathf::inverse_lerp(200.f, 100.f, float(r->at_position_cant_see));
        return mathf::min(num, chance_of_finding(*r, p.see_around_corners)) * mathf::pow(r->rel.intensity, 0.75f) * reach;
    }

    bool can_get_to_prey(tracked const& r, hooks const& h) {
        if (!h.reach)
            return true;
        ivec2 const g = best_guess(r);
        for (auto const d : custom::eight_directions_and_zero)
            if (h.reach(h.user, g + d))
                return true;
        for (auto const d : custom::four_directions)
            if (h.reach(h.user, g + d * 2))
                return true;
        return false;
    }

    float threat_utility(tracker_state const& s) noexcept {
        return mathf::clamp(mathf::lerp(-0.01f, 1.01f, s.current_threat), 0.f, 1.f);
    }

    float threat_of_tile(tracker_state const& s, ivec2 tile, int32_t see_around_corners) noexcept {
        float num = 0.f;
        for (uint32_t i = 0; i < s.count; ++i) {
            auto const& r = s.reps[i];
            if (r.module != track_module::threat)
                continue;
            // the ThreatPoint at the best guess, with the CurrentThreat as
            // its severity (the threat creature's accessibility and the
            // tile's visibility aren't mapped)
            float d = rw::magnitude(rw::to_vec2(tile - best_guess(r)));
            d = mathf::pow(d, 1.25f);
            num += mathf::pow(current_threat(r, see_around_corners), 1.5f) * 10.f / mathf::max(1.f, d);
        }
        return num;
    }

    float threat_of_area(tracker_state const& s, ivec2 tile, int32_t see_around_corners) noexcept {
        float num = 0.f;
        for (auto const d : custom::eight_directions_and_zero)
            num += threat_of_tile(s, tile + d, see_around_corners);
        return num / 9.f;
    }

    ivec2 flee_to(sys::game_state& state, dcon::creature_id c, room_view const& room, ivec2 at, int32_t reevaluations, int32_t max_distance, params const& p,
        hooks const& h) {
        auto& s = of(state, c);
        auto& rng = state.world.creature_get_random(c);
        int32_t const sac = p.see_around_corners;
        // EvaluateFlightDestThreat over the straight line there (vanilla
        // takes it when every tile on it is accessible, a QuickPath else)
        auto const evaluate = [&](ivec2 dest) {
            if (dest.x < 0 || !room.inside(dest.x, dest.y) || custom::manhattan_distance(dest, at) >= max_distance * 2)
                return std::numeric_limits<float>::max();
            if (h.reach && !h.reach(h.user, dest))
                return std::numeric_limits<float>::max();
            int32_t const n = std::max(std::abs(dest.x - at.x), std::abs(dest.y - at.y)) + 1;
            float num = 0.f;
            for (int32_t i = 0; i < n; ++i) {
                float const f = n > 1 ? float(i) / float(n - 1) : 0.f;
                ivec2 const t{at.x + int32_t(std::lround(float(dest.x - at.x) * f)), at.y + int32_t(std::lround(float(dest.y - at.y) * f))};
                num += threat_of_tile(s, t, sac);
            }
            if (n < 2 && threat_of_tile(s, dest, sac) < 0.5f)
                num += 100.f;
            if (threat_of_area(s, dest, sac) > threat_of_area(s, at, sac))
                num += 1000.f;
            return num / float(n) + threat_of_area(s, dest, sac);
        };
        ivec2 test = s.flee_dest;
        float best = evaluate(test);
        int32_t evaluated = 0;
        for (int32_t j = 0; j < reevaluations * 5; ++j) {
            int32_t const dx = rng.range(0, max_distance) * (rng.value() < 0.5f ? -1 : 1);
            int32_t const dy = rng.range(0, max_distance) * (rng.value() < 0.5f ? -1 : 1);
            ivec2 const t{custom::int_clamp(at.x + dx, 0, room.width - 1), custom::int_clamp(at.y + dy, 0, room.height - 1)};
            if (rw::magnitude(rw::to_vec2(test - t)) > 3.f && !room.get_tile(t).solid()) {
                if (float const v = evaluate(t); v < best) {
                    test = t;
                    best = v;
                }
                ++evaluated;
            }
            if (evaluated >= reevaluations)
                break;
        }
        if (s.flee_anti_flicker < 1 && test != s.flee_dest && best < evaluate(s.flee_dest) - 0.5f * mathf::inverse_lerp(1.f, 0.5f, s.current_threat)) {
            s.flee_dest = test;
            s.flee_anti_flicker = 20;
        }
        return s.flee_dest.x < 0 ? at : s.flee_dest;
    }

    float aggression_utility(tracker_state const& s) noexcept {
        return mathf::inverse_lerp(0.35f, 1.f, s.highest_anger);
    }

    void increment_anger(sys::game_state& state, dcon::creature_id c, dcon::creature_id t, float add) {
        auto* r = find(of(state, c), t);
        if (r && r->module == track_module::aggression)
            r->anger = mathf::clamp(r->anger + add, 0.f, 1.f);
    }

    void set_anger(sys::game_state& state, dcon::creature_id c, dcon::creature_id t, float anger, float base) {
        auto* r = find(of(state, c), t);
        if (!r || r->module != track_module::aggression)
            return;
        r->anger = mathf::clamp(anger, 0.f, 1.f);
        if (base >= 0.f)
            r->base_anger = mathf::clamp(base, 0.f, 1.f);
    }
}
