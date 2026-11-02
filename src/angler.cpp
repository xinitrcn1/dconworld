// The Watcher's Angler.cs + AnglerAI.cs (angler.hpp). AnglerGraphics'
// lure (lightPos) moves here, into the simulation: the AI looks from it.

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "angler.hpp"
#include "angler_graphics.hpp"
#include "dens.hpp"
#include "items.hpp"
#include "migrate.hpp"
#include "particles.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "rooms.hpp"
#include "rot.hpp"
#include "scavenger.hpp"
#include "tracker.hpp"

namespace angler {
    using creature::relationship;
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    using world::terrain;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // CreatureTemplate (Angler): baseDamageResistance, baseStunResistance,
        // instantDeathDamageLimit, visualRadius, throughSurfaceVision
        // (waterVision 1, movementBasedVision 0)
        constexpr float base_damage_resistance = 5.f;
        constexpr float base_stun_resistance = 3.f;
        constexpr float instant_death_damage_limit = 5.f;
        constexpr float visual_radius = 450.f;
        constexpr float through_surface_vision = 0.65f;
        // AnglerAI: how long a locked creature may stay away from the lure
        constexpr int32_t prey_away_limit = 44;
        // JawsSlamShut: what's swallowed (the radii of its chunks together)
        constexpr float swallow_rad = 30.f;
        // Angler.bodyChunks' radii and masses, the connections
        constexpr float chunk_rads[body_chunks] = {25.f, 20.f, 15.f};
        constexpr float chunk_masses[body_chunks] = {2.f, 1.8f, 1.2f};

        payload& pl(sys::game_state& state, dcon::creature_id c) { return state.world.creature_get_payload(c).angler; }
        payload const& pl(sys::game_state const& state, dcon::creature_id c) { return state.world.creature_get_payload(c).angler; }
        dcon::angler_data_id data(sys::game_state const& state, dcon::creature_id c) { return data_of(state.world, c); }
        vec2& pos(sys::game_state& state, dcon::creature_id c, uint32_t i) { return state.world.body_chunk_get_pos(chunk(state, c, i)); }
        vec2& vel(sys::game_state& state, dcon::creature_id c, uint32_t i) { return state.world.body_chunk_get_vel(chunk(state, c, i)); }
        vec2 pos(sys::game_state const& state, dcon::creature_id c, uint32_t i) { return state.world.body_chunk_get_pos(chunk(state, c, i)); }
        float mass(sys::game_state const& state, dcon::creature_id c, uint32_t i) { return state.world.body_chunk_get_mass(chunk(state, c, i)); }
        float rad(sys::game_state const& state, dcon::creature_id c, uint32_t i) { return state.world.body_chunk_get_rad(chunk(state, c, i)); }
        float sub(sys::game_state const& state, room_view const& room, dcon::creature_id c, uint32_t i) {
            return physics::submersion(state, room, chunk(state, c, i));
        }
        ivec2 tile_of(vec2 p) { return room_view::get_tile_position(p); }
        // abstractCreature.pos: the main chunk's tile
        ivec2 here(sys::game_state const& state, dcon::creature_id c) { return tile_of(pos(state, c, 0)); }
        float tile_dist(ivec2 a, ivec2 b) { return rw::magnitude(rw::to_vec2(a - b)); }
        bool deep_water(room_view const& room, vec2 p) { return room.get_tile(p).water == 1; }

        // Angler.GlowPos (the lure; the head before the lure exists)
        vec2 glow(sys::game_state const& state, dcon::creature_id c) { return state.world.angler_data_get_lure(data(state, c)).pos; }

        // PhysicalObject.WeightedPush
        void weighted_push(sys::game_state& state, dcon::creature_id c, uint32_t a, uint32_t b, vec2 dir, float frc) {
            float const num = mass(state, c, b) / (mass(state, c, a) + mass(state, c, b));
            vel(state, c, a) += dir * frc * num;
            vel(state, c, b) -= dir * frc * (1.f - num);
        }

        //
        // AnglerAI
        //
        // Tracker(10, 10, 1000, 0.5, 5, 5, 20), ThreatTracker(3); no PreyTracker
        // (it antagonizes: the lockedCreature does the hunting)
        tracker::params const& tracker_params() {
            static tracker::params const p = [] {
                tracker::params q;
                q.see_around_corners = 10;
                q.frames_to_remember = 1000;
                q.ghost_dismissal_range = 20;
                q.threat_max = 3;
                return q;
            }();
            return p;
        }

        // ArtificialIntelligence.VisualScore + AnglerAI.VisualScore: it sees
        // ahead of its head, less well through the surface
        bool visual_contact(sys::game_state const& state, dcon::creature_id c, room_view const& room, vec2 look) {
            vec2 const eye = pos(state, c, 0);
            if (!custom::dist_less(eye, look, visual_radius))
                return false;
            float score = mathf::inverse_lerp(visual_radius, 0.f, rw::distance(eye, look));
            if (room.water && deep_water(room, eye) != deep_water(room, look))
                score -= 1.f - through_surface_vision;
            if (pathing::narrow_space(room, tile_of(look)))
                score -= 0.5f;
            vec2 const c1 = pos(state, c, 1);
            score -= mathf::pow(mathf::inverse_lerp(1.f, -1.f, rw::dot(rw::normalized(c1 - eye), rw::normalized(c1 - look))), 0.85f);
            return score > 0.f && room.visual_contact(eye, look);
        }

        // the trackers' questions, answered by the angler
        struct hook_ctx {
            sys::game_state& state;
            dcon::creature_id c;
            room_view const& room;
            // the field to where it is (CoordinateReachableAndGetbackable:
            // swimming is the same both ways), built on the first question
            std::optional<pathing::field_id> from_here;
        };
        // PathFinder.CoordinateReachableAndGetbackable
        bool reachable(hook_ctx& x, ivec2 t) {
            if (!x.room.inside(t.x, t.y) || !pathing::accessible(x.room, t, swim))
                return false;
            if (!x.from_here)
                x.from_here = pathing::field_to(x.state, x.room.id, here(x.state, x.c), swim);
            return pathing::distance(x.state, *x.from_here, t) != pathing::unreachable;
        }

        // AnglerAI.UpdateDynamicRelationship (it runs the lure's game: locking
        // on, the prey's time away, the anger)
        relationship relationship_to(hook_ctx& x, creature::tracked& r) {
            auto& state = x.state;
            auto const& w = state.world;
            auto const c = x.c;
            auto& p = pl(state, c);
            // TrackedCreatureState.alive (the representation's `state`, 1 dead)
            if (r.visual && creature::live(state, r.who))
                r.state = creature::dead(state, r.who) ? 1 : 0;
            relationship rel = creature::static_relationship(state, c, r.who);
            if (rel.type == creature::rel::antagonizes) {
                if (!p.locked || p.locked == r.who) {
                    bool hidden = false;
                    if (auto const pp = creature::player_of(state, r.who))
                        hidden = w.player_get_is_camo(pp) != 0;
                    if (p.locked == r.who)
                        rel.intensity = 1.f;
                    vec2 const g = glow(state, c);
                    // in the room, out of the pipes
                    bool const in_room = creature::here(state, r.who, x.room.id);
                    vec2 const tp = in_room ? creature::main_pos(state, r.who) : vec2{0.f, 0.f};
                    // the lure's light falls on it
                    bool const lit = in_room && x.room.visual_contact(g, tp) && !hidden;
                    float const range = (r.visual || lit) ? 170.f : 80.f;
                    bool const away = p.prey_away_timer >= prey_away_limit;
                    float const dist = in_room ? rw::distance(g, w.body_chunk_get_pos(creature::chunk_of(state, r.who, 0))) : 3.4e38f;
                    if (away || p.anger_timer > 0) {
                        p.prey_away_timer = 0;
                        rel.type = creature::rel::eats;
                        if (away) {
                            auto rng = rw::random(state.seed, state.tick, rw::ids(uint32_t(c.value), uint32_t(r.who.value)));
                            p.anger_timer = int16_t(rng.range(200, 800));
                            // (Shriek: no Angler_Shriek sample here)
                        }
                    } else if (dist >= range || hidden) {
                        if (p.locked)
                            ++p.prey_away_timer;
                    } else if (lit) {
                        p.locked = r.who;
                        p.prey_away_timer = 0;
                    }
                    if (!in_room)
                        p.prey_away_timer = 0;
                    // angerRiskTarget (for the graphics' lure colour)
                    if (p.locked == r.who)
                        p.anger_risk_target = in_room ? mathf::clamp01(mathf::max(dist / range, float(p.prey_away_timer) / float(prey_away_limit))) : 0.f;
                } else {
                    rel.intensity *= p.locked ? 0.f : 1.f;
                }
            }
            if (rel.type == creature::rel::afraid && r.state != 0)
                rel.intensity = 0.f;
            return rel;
        }

        bool hook_sees(void* u, dcon::body_chunk_id k) {
            auto& x = *static_cast<hook_ctx*>(u);
            return visual_contact(x.state, x.c, x.room, x.state.world.body_chunk_get_pos(k));
        }
        bool hook_sees_point(void* u, vec2 p) {
            auto& x = *static_cast<hook_ctx*>(u);
            return visual_contact(x.state, x.c, x.room, p);
        }
        relationship hook_relate(void* u, creature::tracked const& r) {
            auto& x = *static_cast<hook_ctx*>(u);
            return relationship_to(x, const_cast<creature::tracked&>(r));
        }
        bool hook_reach(void* u, ivec2 t) { return reachable(*static_cast<hook_ctx*>(u), t); }
        // IUseARelationshipTracker.ModuleToTrackRelationship: Afraid a threat, nothing else
        creature::track_module hook_module(relationship r) {
            return r.type == creature::rel::afraid ? creature::track_module::threat : creature::track_module::none;
        }
        tracker::hooks make_hooks(hook_ctx& x) {
            tracker::hooks h;
            h.user = &x;
            h.sees = hook_sees;
            h.sees_point = hook_sees_point;
            h.relate = hook_relate;
            h.module = hook_module;
            h.reach = hook_reach;
            return h;
        }

        // AnglerAI.floatGoalPos's setter: the destination goes with it
        void set_float_goal(sys::game_state& state, dcon::creature_id c, std::optional<vec2> g) {
            state.world.angler_data_set_float_goal(data(state, c), g);
            if (g)
                state.world.creature_set_destination(c, tile_of(*g));
        }

        // AnglerAI.Update
        void ai_update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
            auto& p = pl(state, c);
            auto const d = data(state, c);
            ivec2 const at = here(state, c);
            p.anger_risk_target = 0.f;
            hook_ctx hx{state, c, room, std::nullopt};
            tracker::hooks const h = make_hooks(hx);
            tracker::update(state, c, room, at, tracker_params(), h);
            auto const& ts = tracker::of(state, c);
            // the locked creature's representation (none: forgotten)
            auto const locked_rep = [&]() -> creature::tracked const* {
                return p.locked && creature::live(state, p.locked) ? tracker::find(ts, p.locked) : nullptr;
            };
            if (p.anger_timer > 0) {
                auto const* lr = locked_rep();
                if (!lr || !lr->visual) {
                    --p.anger_timer;
                    if (p.anger_timer == 0)
                        p.locked = dcon::creature_id{};
                }
            }
            behavior b = behavior(w.creature_get_behavior(c));
            // UtilityComparer: threat (weight 0 while stalking), rain, each to
            // the power 1.1 (the StuckTracker isn't ported)
            float const threat_weight = b == behavior::stalk ? 0.f : 1.f;
            float utility = 0.f;
            bool any = false;
            auto const consider = [&](float v, behavior k) {
                v = mathf::pow(mathf::max(v, 0.f), 1.1f);
                if (!any || v > utility) {
                    utility = v;
                    b = k;
                    any = true;
                }
            };
            consider(tracker::threat_utility(ts) * threat_weight, behavior::flee);
            consider(dens::rain_utility(state, c, room.id), behavior::escape_rain);
            if (utility < 0.1f)
                b = behavior::idle;
            if (p.locked) {
                auto const* lr = locked_rep();
                if (!lr)
                    p.locked = dcon::creature_id{};
                else if (lr->rel.type == creature::rel::antagonizes)
                    b = behavior::stalk;
                else if (b != behavior::get_unstuck)
                    b = behavior::hunt;
            } else {
                p.prey_away_timer = 0;
            }
            w.creature_set_behavior(c, uint8_t(b));
            w.creature_set_current_utility(c, utility);

            ivec2& dest = w.creature_get_destination(c);
            switch (b) {
            case behavior::idle: {
                if (p.anger_timer > 0) {
                    if (p.jaw_state == 0)
                        ++p.jaw_state;
                } else if (!p.locked && p.jaw_state == 2) {
                    p.jaw_state = 0;
                }
                p.swim_speed_goal = p.anger_timer > 0 ? 1.f : 0.25f;
                // (AbstractBehavior: now and then a trip to another room, the
                // way out its destination when it can swim there)
                if (rng.value() < 0.01f)
                    migrate::abstract_behavior(state, c, room);
                if (auto const e = migrate::travel_tile(state, c, room); e && reachable(hx, *e)) {
                    dest = *e;
                    set_float_goal(state, c, std::nullopt);
                    break;
                }
                auto& explore = w.angler_data_get_explore(d);
                if (explore) {
                    dest = *explore;
                    if (custom::manhattan_distance(at, *explore) < 5
                    || (rng.value() < 0.0125f && pathing::accessible(room, at, swim) && !reachable(hx, *explore)))
                        explore = std::nullopt;
                } else if (custom::manhattan_distance(at, dest) < 5 || !reachable(hx, dest)) {
                    vec2 const m = pos(state, c, 0);
                    vec2 const inside{mathf::clamp(m.x, 0.f, room.pixel_width()), mathf::clamp(m.y, 0.f, room.pixel_height())};
                    ivec2 const t = tile_of(inside + custom::rnv(rng) * 200.f);
                    if (room.inside(t.x, t.y) && reachable(hx, t) && room.visual_contact(room_view::middle_of_tile(at), room_view::middle_of_tile(t)))
                        dest = t;
                }
                if (rng.value() < 1.f / (explore ? 1600.f : 80.f)) {
                    int32_t const tx = rng.range(0, room.width);
                    ivec2 const t{tx, rng.range(0, room.height)};
                    if (pathing::accessible(room, t, swim) && reachable(hx, t))
                        explore = t;
                }
                if (tile_dist(at, dest) < 20.f && room.visual_contact(room_view::middle_of_tile(at), room_view::middle_of_tile(dest)))
                    set_float_goal(state, c, room_view::middle_of_tile(dest));
                else
                    set_float_goal(state, c, std::nullopt);
                break;
            }
            case behavior::flee:
                p.swim_speed_goal = 0.5f;
                dest = tracker::flee_to(state, c, room, at, 3, 30, tracker_params(), h);
                set_float_goal(state, c, std::nullopt);
                break;
            case behavior::escape_rain:
                p.swim_speed_goal = 0.5f;
                // DenFinder: the den (dens.hpp)
                if (auto const e = dens::home_tile(state, c, room))
                    dest = *e;
                set_float_goal(state, c, std::nullopt);
                break;
            case behavior::hunt: {
                p.swim_speed_goal = 1.f;
                auto const* lr = locked_rep();
                if (lr && lr->visual && creature::here(state, p.locked, room.id))
                    set_float_goal(state, c, creature::main_pos(state, p.locked));
                else if (lr)
                    dest = tracker::best_guess(*lr);
                if (p.jaw_state == 0)
                    p.jaw_state = 1;
                break;
            }
            case behavior::stalk: {
                auto const* lr = locked_rep();
                bool const there = creature::here(state, p.locked, room.id);
                if (there) {
                    vec2 const tp = w.body_chunk_get_pos(creature::chunk_of(state, p.locked, 0));
                    p.swim_speed_goal = 0.29f * mathf::min(rw::distance(pos(state, c, 0), tp) - 80.f, 85.f) / 85.f;
                }
                if (lr && lr->visual && there)
                    set_float_goal(state, c, creature::main_pos(state, p.locked));
                else if (lr)
                    dest = tracker::best_guess(*lr);
                break;
            }
            default:
                break;
            }
        }

        //
        // Angler
        //
        // Creature.Stun
        void stun(sys::game_state& state, dcon::creature_id c, int32_t st) { creature::stun(state, c, st); }

        // `t` went down the angler's throat: everything lets go of it; a
        // slugcat is eaten (dead and gone), a creature gone at the end of the
        // tick
        void swallow(sys::game_state& state, dcon::creature_id c, room_view const& room, dcon::creature_id t) {
            auto& w = state.world;
            auto& p = pl(state, c);
            if (p.locked == t)
                p.locked = dcon::creature_id{};
            tracker::forget(state, c, t);
            creature::release_target(state, t, room.id);
            rot::release(state, t);
            if (auto const pp = creature::player_of(state, t)) {
                items::release_all(state, pp);
                creature::kill(state, t, creature::template_of(state, c));
                w.player_set_eaten(pp, 1);
                return;
            }
            creature::kill(state, t, creature::template_of(state, c));
            state.defer(room.id, [t](sys::game_state& st) {
                if (st.world.creature_is_valid(t))
                    creature::destroy(st, t);
            });
        }

        // Angler.JawsSlamShut: whatever small enough is in front of the head
        // goes in
        void jaws_slam_shut(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
            vec2 const head = pos(state, c, 0);
            vec2 const a = custom::dir_vec(pos(state, c, 1), head);
            vec2 const tip = head + a * 60.f;
            // Custom.ClosestPointOnLineSegment
            auto const closest = [&](vec2 q) {
                vec2 const ab = tip - head;
                float const t = mathf::clamp01(rw::dot(q - head, ab) / mathf::max(rw::dot(ab, ab), 1e-6f));
                return head + ab * t;
            };
            // whether the chunk at `q` of radius `r` is in the jaws
            auto const in_jaws = [&](vec2 q, float r) {
                vec2 const v = closest(q);
                float const num = rw::distance(head, v) / 60.f;
                return rw::distance(v, q) < r + num * 25.f;
            };
            bool flag = false;
            static thread_local std::vector<dcon::creature_id> eaten;
            eaten.clear();
            rooms::for_each_creature(state, room.id, [&](dcon::creature_id t) {
                if (t == c || !creature::here(state, t, room.id))
                    return;
                uint32_t const n = creature::chunk_count_of(state, t);
                for (uint32_t k = 0; k < n; ++k) {
                    auto const ck = creature::chunk_of(state, t, k);
                    if (!in_jaws(w.body_chunk_get_pos(ck), w.body_chunk_get_rad(ck)))
                        continue;
                    float total = 0.f;
                    for (uint32_t l = 0; l < n; ++l)
                        total += w.body_chunk_get_rad(creature::chunk_of(state, t, l));
                    if (total < swallow_rad) {
                        flag = true;
                        eaten.push_back(t);
                    }
                    break;
                }
            });
            for (auto const t : eaten)
                swallow(state, c, room, t);
            // the loose items too (what a hand holds stays in it; no EnergyCell
            // is ported)
            rooms::for_each_item(state, room.id, [&](dcon::item_id it) {
                if (items::grabber(state, it))
                    return;
                auto const ck = items::chunk(state, it);
                if (!ck || w.body_chunk_get_rad(ck) >= swallow_rad || !in_jaws(w.body_chunk_get_pos(ck), w.body_chunk_get_rad(ck)))
                    return;
                flag = true;
                state.defer(room.id, [it](sys::game_state& st) {
                    if (st.world.item_is_valid(it))
                        items::destroy(st, it);
                });
            });
            // the water churned up (Bubbles under water; no WaterDrips, no ShockWave)
            int32_t const num3 = rng.range(5, 11) * (flag ? 3 : 1);
            vec2 const at = head + a * 40.f;
            if (room.point_submerged(at))
                particles::bubbles(state, room.id, at, num3, 40.f);
        }

        // Angler.Act
        void act(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
            ai_update(state, c, room);
            auto& p = pl(state, c);
            vec2 const head = pos(state, c, 0);
            vec2 const vector = custom::dir_vec(pos(state, c, 1), head);
            vec2 const vector2 = custom::perpendicular_vector(vector);
            ++p.consious_counter;
            float const cc = float(p.consious_counter);
            // the jaws
            switch (p.jaw_state) {
            case 0: {
                float const num = p.locked ? 0.3f : 0.15f;
                p.mouth_openness = 0.1f + (std::sin(cc * 0.07f) + 1.f) / 2.f
                    * (0.25f + (mathf::max(std::fabs(std::sin(cc * 0.005f)), 0.5f) - 0.5f) / 0.5f * 0.75f * num);
                p.jaw_progress = 0;
                break;
            }
            case 1: {
                constexpr int32_t num2 = 44;
                ++p.jaw_progress;
                p.mouth_openness = custom::lerp_and_tick(p.mouth_openness, mathf::pow(float(p.jaw_progress) / float(num2) - 1.f, 5.f) + 1.f, 0.1f, 0.02f);
                if (p.jaw_progress >= num2) {
                    ++p.jaw_state;
                    p.jaw_progress = 0;
                }
                break;
            }
            case 2:
                p.mouth_openness = mathf::clamp01(1.f - std::fabs(std::sin(cc * 0.8f) * 0.05f + std::sin(cc * 0.175f) * 0.14f));
                p.jaw_progress = 0;
                break;
            case 3: {
                constexpr int32_t num3 = 4;
                ++p.jaw_progress;
                p.mouth_openness = 1.f - mathf::pow(float(p.jaw_progress) / float(num3), 2.f);
                if (p.jaw_progress >= num3) {
                    ++p.jaw_state;
                    p.jaw_progress = 0;
                    // (no Angler_Bite sample here)
                    jaws_slam_shut(state, c, room);
                }
                break;
            }
            case 4: {
                constexpr int32_t num4 = 12;
                ++p.jaw_progress;
                p.mouth_openness = (1.f - (mathf::pow(-float(p.jaw_progress) / float(num4), 3.f) + 1.f)) * 0.25f;
                if (p.jaw_progress >= num4) {
                    p.jaw_state = 0;
                    p.jaw_progress = 0;
                }
                break;
            }
            default:
                p.jaw_state = 0;
                break;
            }
            if (!w.creature_is_valid(c))
                return;
            // (no GetUnstuck: the StuckTracker isn't ported)
            // FishPather.FollowPath from where it is, or from a spot next to it
            auto const f = pathing::field_to(state, room.id, w.creature_get_destination(c), swim);
            std::optional<ivec2> next = pathing::next_step(state, f, tile_of(head));
            for (int32_t j = 0; j < 8 && !next; ++j)
                next = pathing::next_step(state, f, tile_of(head + rw::to_vec2(custom::eight_directions[j]) * 20.f));
            auto const& goal = w.angler_data_get_float_goal(data(state, c));
            if (goal && room.visual_contact(head, *goal)) {
                p.swim_dir = custom::dir_vec(head, *goal);
            } else if (next) {
                p.swim_dir = custom::dir_vec(head, room_view::middle_of_tile(*next));
                // a few steps further along, while it can see them
                ivec2 step = *next;
                for (int32_t k = 0; k < 4; ++k) {
                    auto const n2 = pathing::next_step(state, f, step);
                    if (!n2 || !room.visual_contact(room_view::middle_of_tile(*next), room_view::middle_of_tile(*n2)))
                        break;
                    p.swim_dir += custom::dir_vec(head, room_view::middle_of_tile(*n2));
                    step = *n2;
                }
                p.swim_dir = rw::normalized(p.swim_dir);
            }
            // squeezing: the next steps go through a narrow space or a pipe's mouth
            if (next) {
                ivec2 step = *next;
                for (int32_t l = 0; l < 2; ++l) {
                    auto const n3 = pathing::next_step(state, f, step);
                    if (!n3)
                        break;
                    if (pathing::narrow_space(room, *n3) || room.get_tile(*n3).t == terrain::shortcut_entrance)
                        p.squeeze = 1;
                    step = *n3;
                }
            }
            // the flip: on its side when swimming sideways, flopping out of the water
            float num6 = p.flip;
            if (physics::object_submersion(state, room, body(state, c)) < 0.4f) {
                if (w.body_chunk_get_contact_point(chunk(state, c, 0)).y < 0 && rng.value() < 0.05f) {
                    float const ang = mathf::lerp(-45.f, 45.f, rng.value());
                    vel(state, c, 0) += custom::deg_to_vec(ang) * mathf::lerp(6.f, 26.f, rng.value());
                    num6 = vector2.y > 0.f ? 1.f : -1.f;
                    p.flip = custom::lerp_and_tick(p.flip, num6, rng.value() * 0.5f, 0.075f);
                }
            } else {
                num6 = std::fabs(p.swim_dir.x) > 0.2f ? ((p.swim_dir.x > 0.f ? 0.9f : -0.9f) + std::sin(cc * 0.055f) * 0.1f) : p.flip;
                num6 *= mathf::clamp01(rw::dot(vector, p.swim_dir)) * 0.8f + 0.2f;
            }
            p.flip = custom::lerp_and_tick(p.flip, num6, 0.018f, 0.005f);
            p.rudder_turn_way = custom::lerp_and_tick(p.rudder_turn_way, rw::dot(vector2, p.swim_dir), 0.2f, 0.002f);
            if (rw::dot(vector, p.swim_dir) < 0.f)
                p.rudder_turn_way = p.rudder_turn_way > 0.f ? 1.f : -1.f;
            bool const stalking = behavior(w.creature_get_behavior(c)) == behavior::stalk;
            weighted_push(state, c, 0, 1, p.swim_dir,
                4.f * ((1.f - std::fabs(p.flip)) * 0.5f + 0.5f) * sub(state, room, c, 0) * (0.7f + 0.3f * p.swim_speed_goal) + (stalking ? 0.5f : 0.f));
            if (p.squeeze) {
                vel(state, c, 0) *= 0.85f;
                pos(state, c, 0) += p.swim_dir * 2.5f;
                vel(state, c, 0) += p.swim_dir * 2.5f;
            }
            float num7 = 1.f;
            ivec2 const dest = w.creature_get_destination(c);
            if (room.inside(dest.x, dest.y))
                num7 *= mathf::min(rw::distance(pos(state, c, 0), room_view::middle_of_tile(dest)), 70.f) / 70.f;
            p.swim_speed = custom::lerp_and_tick(p.swim_speed, p.swim_speed_goal * num7, 0.01f, 0.05f);
        }

        // AnglerGraphics.Update's lure (lightPos, lightVel): drawn to a point
        // ahead of the head at the arm's reach, kept out of the walls
        void lure_update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto const d = data(state, c);
            auto const& lk = w.angler_data_get_looks(d);
            auto& l = w.angler_data_get_lure(d);
            auto const& p = pl(state, c);
            vec2 const c0 = pos(state, c, 0);
            vec2 const v2 = custom::dir_vec(pos(state, c, 1), c0);
            vec2 const a2 = custom::perpendicular_vector(v2);
            float const arm = lk.arm_low_length + lk.arm_high_length;
            float const r0 = rad(state, c, 0);
            vec2 const a3 = c0 + v2 * (r0 + arm * mathf::min(arm / 120.f, 0.87f) + std::cos(p.flip * mathf::pi * 0.5f) * 10.f)
                + a2 * ((30.f + 10.f * p.mouth_openness) * std::sin(p.flip * mathf::pi * 0.5f));
            l.last_pos = l.pos;
            bool const submerged = room.point_submerged(l.pos);
            l.vel *= 0.8f - (submerged ? 0.2f : 0.f);
            l.vel += (a3 - l.pos) * (0.3f - (submerged ? 0.1f : 0.f));
            l.pos += l.vel;
            if (auto const hit = scavenger::exact_terrain_ray_trace_pos(room, c0 + a2 * p.flip * r0 * 0.7f, l.pos)) {
                l.pos = *hit;
                l.vel *= 0.01f;
            }
        }

        // the lure back at the head (AnglerGraphics.Reset)
        void lure_reset(sys::game_state& state, dcon::creature_id c) {
            auto& l = state.world.angler_data_get_lure(data(state, c));
            l.pos = pos(state, c, 0);
            l.last_pos = l.pos;
            l.vel = vec2{0.f, 0.f};
        }

        // AnglerGraphics' constructor rolls, from the ID's seed
        looks roll_looks(int32_t seed) {
            rw::unity_random r{seed};
            looks lk;
            lk.number_of_props = uint8_t(r.range(3, 7));
            int32_t const num = r.range(7, 14);
            lk.bottom_count = uint8_t(std::clamp<int32_t>(num - r.range(0, 4), 1, int32_t(max_teeth)));
            lk.top_count = uint8_t(std::clamp<int32_t>(num - r.range(0, 4), 1, int32_t(max_teeth)));
            float const bl = float(lk.bottom_count) - 1.f;
            for (uint32_t i = 0; i < lk.bottom_count; ++i) {
                float const num2 = mathf::pow(1.f - float(i) / bl - 1.f, 4.f) * 0.65f + 0.35f;
                float const v0 = r.value();
                float const v1 = r.value();
                float const v2 = r.value();
                float const v3 = r.value();
                lk.bottom_teeth[i] = tooth{21.f * num2 + v0 * 5.f, 1.5f - v1 * 0.5f, 0.15f + v2 * 0.07f, 0.9f + v3 * 0.7f};
            }
            for (uint32_t j = 0; j < lk.top_count; ++j) {
                // (over the bottom teeth's count, as vanilla)
                float const num3 = mathf::pow(1.f - float(j) / bl - 1.f, 4.f) * 0.65f + 0.35f;
                float const v0 = r.value();
                float const v1 = r.value();
                float const v2 = r.value();
                float const v3 = r.value();
                lk.top_teeth[j] = tooth{18.f * num3 + v0 * 5.f, 1.5f - v1 * 0.5f, 0.09f + v2 * 0.07f, 0.5f + v3 * 0.4f};
            }
            lk.eye_size = 0.5f + r.value() * 0.65f;
            // HSLColor(0.027 + 0.153 * value, 1, 0.5)
            lk.eye_color = custom::hsl2rgb(0.027f + 0.153f * r.value(), 1.f, 0.5f);
            lk.rudder_length = r.value();
            lk.cone_rad = r.range(15.f, 24.f);
            lk.arm_low_length = r.range(25.f, 60.f);
            lk.arm_high_length = r.range(25.f, 70.f);
            lk.arm_low_extra = r.range(5.f, 25.f);
            return lk;
        }
    }

    vec2 glow_pos(sys::game_state const& state, dcon::creature_id c, float alpha) noexcept {
        auto const& l = state.world.angler_data_get_lure(data(state, c));
        return rw::lerp(l.last_pos, l.pos, alpha);
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::angler);
        auto const d = data(state, c);
        auto rng = rw::random(state.seed, 0, uint32_t(c.value));
        int32_t seed = int32_t(rng.next() & 0x7FFFFFFFu);
        if (state.spawn_seed >= 0)
            seed = state.spawn_seed;
        physics::object_desc desc;
        desc.gravity = 0.9f;
        desc.air_friction = 0.999f;
        desc.bounce = 0.1f;
        desc.surface_friction = 0.4f;
        desc.water_friction = 0.97f;
        desc.buoyancy = 0.9f;
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
        // three chunks: head (25, 2), middle (20, 1.8), tail (15, 1.2)
        dcon::body_chunk_id cs[body_chunks];
        for (uint32_t i = 0; i < body_chunks; ++i)
            cs[i] = physics::add_chunk(state, o, at + vec2{0.f, -float(i)}, chunk_rads[i], chunk_masses[i]);
        physics::add_connection(state, o, cs[0], cs[1], 20.f, physics::connection_type::normal, 1.f, 0.5f);
        physics::add_connection(state, o, cs[1], cs[2], 24.f, physics::connection_type::normal, 1.f, 0.5f);
        physics::add_connection(state, o, cs[0], cs[2], 44.f, physics::connection_type::push, 1.f, 0.5f);
        physics::set_go_through_floors(state, o, true);
        w.try_create_creature_body(c, o);
        auto& p = pl(state, c);
        uint16_t const dd = p.data;
        p = payload{};
        p.data = dd;
        p.swim_dir = vec2{0.f, 0.f};
        p.flip = 0.f;
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_set_grasps(c, creature::grasp_pair{});
        w.creature_set_behavior(c, uint8_t(behavior::idle));
        w.creature_set_current_utility(c, 0.f);
        w.creature_set_destination(c, tile_of(at));
        w.angler_data_set_seed(d, seed);
        w.angler_data_set_looks(d, roll_looks(seed));
        w.angler_data_set_float_goal(d, std::nullopt);
        w.angler_data_set_explore(d, std::nullopt);
        lure_reset(state, c);
        tracker::clear(state, c);
        angler_graphics::reset_positions(state, c);
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
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
                stun(state, c, rng.range(1, rng.range(1, 27 - custom::int_clamp(int32_t(20.f * health), 0, 10))));
            if (health > 0.f && health < 1.f && rng.value() < 0.01f)
                health = mathf::min(1.f, health + 1.f / mathf::lerp(550.f, 70.f, health));
        }
        // Creature.Update + PhysicalObject.Update
        creature::stun_countdown(state, c);
        physics::update_object(state, room, o, nullptr, nullptr);
        auto& p = pl(state, c);
        // squeezing through a narrow way (or into a pipe)
        if (p.squeeze || w.creature_get_entering_shortcut(c).x >= 0)
            p.squeeze_fac = mathf::min(1.f, p.squeeze_fac + 0.02f);
        else
            p.squeeze_fac = mathf::max(0.f, p.squeeze_fac - 0.033333335f);
        for (uint32_t i = 0; i < body_chunks; ++i)
            w.body_chunk_set_terrain_squeeze(chunk(state, c, i), 1.f - p.squeeze_fac);
        p.squeeze = 0;
        vec2 const lhs = custom::dir_vec(pos(state, c, 1), pos(state, c, 0));
        if (consious(state, c)) {
            act(state, c, room);
            if (!w.creature_is_valid(c))
                return;
        } else {
            p.jaw_state = 0;
            p.jaw_progress = 0;
            p.mouth_openness = custom::lerp_and_tick(p.mouth_openness,
                mathf::clamp01((1.f - rw::dot(lhs, custom::dir_vec(pos(state, c, 2), pos(state, c, 0)))) * 5.f + 0.2f), 0.08f, 0.02f);
            p.swim_speed = custom::lerp_and_tick(p.swim_speed, 0.f, 0.0032f, 0.00032f);
        }
        // the propeller's push, by how deep the body is
        float const num = (0.5f + sub(state, room, c, 0) * 0.5f) * sub(state, room, c, 1) * sub(state, room, c, 2);
        vec2 const ahead = rw::normalized(custom::dir_vec(pos(state, c, 1), pos(state, c, 0)) + p.swim_dir * 0.4f);
        vel(state, c, 0) += ahead * (mathf::pow(p.swim_speed * 2.f, 2.f) * 2.f * num);
        vec2 const b = (vel(state, c, 0) + vel(state, c, 2)) * 0.5f;
        float const m0 = mass(state, c, 0), m2 = mass(state, c, 2);
        float const pn = mathf::pow(num, 0.2f);
        vel(state, c, 0) = rw::lerp(vel(state, c, 0), b, pn * (m2 / (m0 + m2)) * 0.25f);
        vel(state, c, 2) = rw::lerp(vel(state, c, 2), b, pn * (1.f - m2 / (m0 + m2)) * 0.25f) * (1.f - 0.1f * pn);
        lure_update(state, c, room);
    }

    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) {
        return true;
    }

    void collide(sys::game_state& state, dcon::creature_id c, room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j) {
        auto& w = state.world;
        if (!creature::live(state, t) || j >= creature::chunk_count_of(state, t))
            return;
        auto& p = pl(state, c);
        // bumping into the locked creature angers it (no Shriek sample)
        if (p.locked && t == p.locked) {
            auto rng = rw::random(state.seed, state.tick, rw::ids(uint32_t(c.value), uint32_t(t.value)));
            p.anger_timer = int16_t(rng.range(200, 700));
        }
        // the open jaws, side on, snap at what they touch in front
        if (p.jaw_state == 2 && std::fabs(p.flip) > 0.55f) {
            vec2 const head = pos(state, c, 0);
            vec2 const other = w.body_chunk_get_pos(creature::chunk_of(state, t, j));
            if (rw::dot(custom::dir_vec(pos(state, c, 1), head), custom::dir_vec(head, other)) > 0.1f)
                ++p.jaw_state;
        }
    }

    void violence(sys::game_state& state, dcon::creature_id c, uint32_t k, vec2 momentum, float damage, float stun_bonus, creature::damage_type type) {
        auto& w = state.world;
        auto rng = rw::random(state.seed, state.tick, rw::ids(uint32_t(c.value), k));
        auto& p = pl(state, c);
        if (k >= body_chunks)
            k = 0;
        if (damage > 0.05f)
            p.anger_timer = int16_t(rng.range(200, 500));
        // a stab into the open mouth (the source in front of the head: where
        // the momentum came from)
        if (k == 0 && type == creature::damage_type::stab && rw::magnitude(momentum) > 0.f) {
            vec2 const lhs = custom::dir_vec(pos(state, c, 1), pos(state, c, 0));
            vec2 const from = -rw::normalized(momentum);
            if (rw::dot(lhs, from) > 1.f - 0.5f * p.mouth_openness * (0.3f + 0.7f * std::fabs(p.flip))) {
                damage *= 2.f;
                stun_bonus *= 2.f;
            }
        }
        // Creature.Violence (HealthState)
        auto const hit = chunk(state, c, k);
        w.body_chunk_get_vel(hit) += rw::clamp_magnitude(momentum / w.body_chunk_get_mass(hit), 10.f);
        float const dmg = damage / base_damage_resistance;
        float st = (damage * 30.f + stun_bonus) / base_stun_resistance;
        float& health = w.creature_get_health(c);
        st *= 1.5f + mathf::inverse_lerp(0.5f, 0.f, health) * rng.value();
        stun(state, c, int32_t(st));
        health -= dmg;
        if (rng.value() < -health || health < -1.f || (health < 0.f && rng.value() < 0.33f) || dmg >= instant_death_damage_limit)
            creature::die(state, c);
    }

    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t) {
        auto& p = pl(state, c);
        if (p.locked == t) {
            p.locked = dcon::creature_id{};
            p.prey_away_timer = 0;
        }
    }

    void died(sys::game_state& state, dcon::creature_id c) {
        auto& p = pl(state, c);
        state.world.physical_object_set_buoyancy(body(state, c), 0.92f);
        p.jaw_state = 0;
        p.jaw_progress = 0;
    }

    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const view = world::get_room_view(state, room);
        ivec2 const t = tile_of(at);
        vec2 const a = rw::to_vec2(view.shortcut_entrance_hole_direction(t));
        vec2 const mid = room_view::middle_of_tile(t);
        for (uint32_t i = 0; i < body_chunks; ++i) {
            auto const k = chunk(state, c, i);
            physics::hard_set_position(state, k, mid + a * float(2 - int32_t(i)) * 20.f);
            w.body_chunk_set_last_pos(k, mid);
            w.body_chunk_set_vel(k, a * 3.f);
        }
        // AnglerAI.NewRoom: what it was swimming to is in the other room
        w.angler_data_set_float_goal(data(state, c), std::nullopt);
        w.angler_data_set_explore(data(state, c), std::nullopt);
        lure_reset(state, c);
        angler_graphics::reset_positions(state, c);
    }

    std::string describe(sys::game_state const& state, dcon::creature_id c) {
        static constexpr char const* names[] = {"idle", "flee", "hunt", "stalk", "escape rain", "get unstuck"};
        auto const& w = state.world;
        auto const& p = pl(state, c);
        vec2 const at = pos(state, c, 0);
        uint32_t const b = w.creature_get_behavior(c);
        return "angler at " + std::to_string(int(at.x)) + "," + std::to_string(int(at.y)) + " behavior " + (b < 6 ? names[b] : "?")
            + " jaw " + std::to_string(int(p.jaw_state)) + (p.locked ? " locked" : "") + (p.anger_timer > 0 ? " angry " + std::to_string(int(p.anger_timer)) : "");
    }

    bool spawn_spot(room_view const& room, ivec2 tile) {
        if (!room.inside(tile.x, tile.y) || room.get_tile(tile).solid() || room.get_tile(tile).water == 0)
            return false;
        // room for its 25 px head: no wall in the tiles around
        for (auto const dv : custom::eight_directions)
            if (!room.inside(tile.x + dv.x, tile.y + dv.y) || room.get_tile(tile + dv).solid())
                return false;
        return true;
    }
}
