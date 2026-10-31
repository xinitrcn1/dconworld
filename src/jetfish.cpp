#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

#include "jetfish.hpp"
#include "dens.hpp"
#include "jetfish_graphics.hpp"
#include "migrate.hpp"
#include "particles.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "rooms.hpp"
#include "tracker.hpp"

namespace jetfish {
    using creature::relationship;
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    using world::terrain;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // CreatureTemplate (JetFish): baseDamageResistance, baseStunResistance,
        // instantDeathDamageLimit, visualRadius, throughSurfaceVision
        // (waterVision 1 takes nothing off), bodySize
        constexpr float base_damage_resistance = 0.95f;
        constexpr float base_stun_resistance = 0.9f;
        constexpr float instant_death_damage_limit = 1.f;
        constexpr float visual_radius = 450.f;
        constexpr float through_surface_vision = 0.65f;
        constexpr float own_body_size = 0.8f;

        creature::jetfish_payload& pl(sys::game_state& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).jetfish;
        }
        creature::jetfish_payload const& pl(sys::game_state const& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).jetfish;
        }
        dcon::jetfish_data_id data(sys::game_state const& state, dcon::creature_id c) {
            return creature::jetfish_data(state.world, c);
        }
        vec2& pos(sys::game_state& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_pos(chunk(state, c, i));
        }
        vec2& vel(sys::game_state& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_vel(chunk(state, c, i));
        }
        vec2 pos(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_pos(chunk(state, c, i));
        }
        float sub(sys::game_state const& state, room_view const& room, dcon::creature_id c, uint32_t i) {
            return physics::submersion(state, room, chunk(state, c, i));
        }
        ivec2 tile_of(vec2 p) {
            return room_view::get_tile_position(p);
        }
        // abstractCreature.pos: the main chunk's tile
        ivec2 here(sys::game_state const& state, dcon::creature_id c) {
            return tile_of(pos(state, c, 0));
        }
        float tile_dist(ivec2 a, ivec2 b) {
            return rw::magnitude(rw::to_vec2(a - b));
        }
        bool deep_water(room_view const& room, vec2 p) {
            return room.get_tile(p).water == 1;
        }

        // the slugcat holding it (grabbedBy[i].grabber is Player)
        dcon::player_id holding_slugcat(sys::game_state const& state, dcon::creature_id c) {
            dcon::player_id out;
            creature::for_each_holder(state, creature::room_of(state, c), c, [&](dcon::creature_id h, uint32_t) {
                if (!out && creature::is_slugcat(state.world, h))
                    out = creature::player_of(state, h);
            });
            return out;
        }

        // what it holds in its mouth (grasps[0]), if it can be reached
        dcon::creature_id grasped(sys::game_state const& state, dcon::creature_id c) {
            auto const t = creature::grasped(state.world, c, 0);
            return creature::live(state, t) ? t : dcon::creature_id{};
        }

        //
        // SocialMemory
        //
        // GetRelationship: its place in the memory, -1 none
        int32_t find_social(social_memory const& m, dcon::creature_id who) {
            for (uint32_t i = 0; i < remembered; ++i)
                if (m.who[i] == who)
                    return int32_t(i);
            return -1;
        }
        // GetOrInitiateRelationship: a new one forgets the one liked least
        // strongly either way when the memory is full
        float& like_of(sys::game_state& state, dcon::creature_id c, dcon::creature_id who) {
            auto& m = state.world.jetfish_data_get_social(data(state, c));
            int32_t i = find_social(m, who);
            if (i < 0) {
                uint32_t pick = 0;
                for (uint32_t k = 0; k < remembered; ++k) {
                    if (!m.who[k] || !state.world.creature_is_valid(m.who[k])) {
                        pick = k;
                        break;
                    }
                    if (std::fabs(m.like[k]) < std::fabs(m.like[pick]))
                        pick = k;
                }
                m.who[pick] = who;
                m.like[pick] = 0.f;
                i = int32_t(pick);
            }
            return m.like[uint32_t(i)];
        }
        // SocialMemory.GetLike (0 for one it never met)
        float get_like(sys::game_state const& state, dcon::creature_id c, dcon::creature_id who) {
            auto const& m = state.world.jetfish_data_get_social(data(state, c));
            int32_t const i = find_social(m, who);
            return i < 0 ? 0.f : m.like[uint32_t(i)];
        }

        //
        // JetFishAI
        //
        // the tracker's settings: Tracker(10, 10, 250, 0.5, 5, 5, 20),
        // PreyTracker(5, 1, 5, 150, 0.05), ThreatTracker(3)
        tracker::params const& tracker_params() {
            static tracker::params const p = [] {
                tracker::params q;
                q.see_around_corners = 10;
                q.frames_to_remember = 250;
                q.ghost_dismissal_range = 20;
                q.prey_max = 5;
                q.persistance_bias = 1.f;
                q.sure_to_get = 5.f;
                q.sure_to_lose = 150.f;
                q.threat_max = 3;
                return q;
            }();
            return p;
        }

        // ArtificialIntelligence.VisualScore + JetFishAI.VisualScore: it
        // sees ahead of its head, less well through the surface
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

        // the trackers' questions, answered by the fish
        struct hook_ctx {
            sys::game_state& state;
            dcon::creature_id c;
            room_view const& room;
            // the field to where it is (PathFinder.CoordinateReachableAndGetbackable:
            // swimming is the same both ways), built on the first question
            std::optional<pathing::field_id> from_here;
        };
        // CoordinateReachableAndGetbackable
        bool reachable(hook_ctx& x, ivec2 t) {
            if (!x.room.inside(t.x, t.y) || !pathing::accessible(x.room, t, swim))
                return false;
            if (!x.from_here)
                x.from_here = pathing::field_to(x.state, x.room.id, here(x.state, x.c), swim);
            return pathing::distance(x.state, *x.from_here, t) != pathing::unreachable;
        }

        // JetFishAI.UpdateDynamicRelationship (TrackedCreatureState.alive:
        // the representation's `state`, 1 dead)
        relationship relationship_to(hook_ctx& x, creature::tracked& r) {
            auto& state = x.state;
            auto const& w = state.world;
            auto const c = x.c;
            if (r.visual && creature::live(state, r.who)) {
                r.state = creature::dead(state, r.who) ? 1 : 0;
                // a slugcat holding a jetfish: it is forgiven, slowly
                if (creature::is_slugcat(w, r.who))
                    for (uint32_t g = 0; g < 2; ++g)
                        if (auto const h = creature::grasped(w, r.who, g); h && creature::is(w, h, creature::type::jetfish)) {
                            float& like = like_of(state, c, r.who);
                            like = mathf::lerp(like, 0.f, 5e-05f);
                            break;
                        }
            }
            auto const& x_pl = pl(state, c);
            if (x_pl.get_away_counter > 0 && r.who == x_pl.get_away_from)
                return relationship{creature::rel::afraid, 0.5f};
            auto const tt = creature::template_of(state, r.who);
            if (r.state != 0 && creature::body_size(tt) < own_body_size * 1.5f)
                return relationship{creature::rel::eats, 0.5f};
            relationship rel = creature::static_relationship(state, c, r.who);
            if (rel.type == creature::rel::social_dependent) {
                float const like = get_like(state, c, r.who);
                if (like < 0.3f)
                    rel = relationship{like < -0.5f ? creature::rel::eats : creature::rel::antagonizes, mathf::pow(mathf::inverse_lerp(0.3f, -1.f, like), 2.f)};
                // the other jetfish around egg it on
                auto const& ts = tracker::of(state, c);
                ivec2 const at = here(state, c);
                for (uint32_t i = 0; i < ts.count; ++i) {
                    auto const o = ts.reps[i].who;
                    if (creature::live(state, o) && creature::is(w, o, creature::type::jetfish))
                        rel.intensity = mathf::lerp(rel.intensity, 1.f,
                            mathf::inverse_lerp(100.f, 30.f, float(custom::manhattan_distance(tracker::best_guess(ts.reps[i]), at))) * 0.5f);
                }
                // antagonized only close to the surface
                if (rel.type == creature::rel::antagonizes)
                    rel.intensity *= mathf::inverse_lerp(10.f + mathf::inverse_lerp(0.f, -1.f, like) * 40.f, 5.f,
                        float(std::abs(tracker::best_guess(r).y - x.room.default_water_level)));
            }
            if (rel.intensity == 0.f)
                rel.type = creature::rel::ignores;
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
        bool hook_reach(void* u, ivec2 t) {
            return reachable(*static_cast<hook_ctx*>(u), t);
        }
        // IUseARelationshipTracker.ModuleToTrackRelationship: Afraid a threat,
        // Eats and Antagonizes prey
        creature::track_module hook_module(relationship r) {
            if (r.type == creature::rel::afraid)
                return creature::track_module::threat;
            if (r.type == creature::rel::eats || r.type == creature::rel::antagonizes)
                return creature::track_module::prey;
            return creature::track_module::none;
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

        // JetFishAI.floatGoalPos's setter: the destination goes with it
        void set_float_goal(sys::game_state& state, dcon::creature_id c, std::optional<vec2> g) {
            state.world.jetfish_data_set_float_goal(data(state, c), g);
            if (g)
                state.world.creature_set_destination(c, tile_of(*g));
        }

        void collide_with(sys::game_state& state, dcon::creature_id c, room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j);

        // JetFishAI.Update
        void ai_update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            auto& x = pl(state, c);
            auto const d = data(state, c);
            ivec2 const at = here(state, c);
            hook_ctx hx{state, c, room, std::nullopt};
            tracker::hooks const h = make_hooks(hx);
            tracker::update(state, c, room, at, tracker_params(), h);
            auto const& ts = tracker::of(state, c);
            if (x.get_away_counter > 0)
                --x.get_away_counter;
            // UtilityComparer: threat (1), prey (0.5), rain (1), each to the
            // power 1.1 (the StuckTracker isn't ported)
            float utility = 0.f;
            behavior b = behavior(w.creature_get_behavior(c));
            bool any = false;
            auto const consider = [&](float v, behavior k) {
                v = mathf::pow(mathf::max(v, 0.f), 1.1f);
                if (!any || v > utility) {
                    utility = v;
                    b = k;
                    any = true;
                }
            };
            consider(tracker::threat_utility(ts), behavior::flee);
            // (the prey's weight of 0.5 applied after the power)
            consider(tracker::prey_utility(state, c, room, at, tracker_params()) * mathf::pow(0.5f, 1.f / 1.1f), behavior::hunt);
            consider(dens::rain_utility(state, c, room.id), behavior::escape_rain);
            // (no GoToFood: fruit doesn't meet creatures here)
            if (utility < 0.1f)
                b = behavior::idle;
            if (b != behavior::flee && grasped(state, c))
                b = behavior::return_prey;
            w.creature_set_behavior(c, uint8_t(b));
            w.creature_set_current_utility(c, utility);

            ivec2& dest = w.creature_get_destination(c);
            switch (b) {
            case behavior::idle: {
                // (AbstractBehavior: now and then a trip to another room, the
                // way out its destination when it can swim there)
                if (rng.value() < 0.01f)
                    migrate::abstract_behavior(state, c, room);
                if (auto const e = migrate::travel_tile(state, c, room); e && reachable(hx, *e)) {
                    dest = *e;
                    set_float_goal(state, c, std::nullopt);
                    break;
                }
                auto& explore = w.jetfish_data_get_explore(d);
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
                dest = tracker::flee_to(state, c, room, at, 3, 30, tracker_params(), h);
                set_float_goal(state, c, std::nullopt);
                break;
            case behavior::escape_rain:
            case behavior::return_prey:
                // DenFinder: the den (dens.hpp)
                if (auto const e = dens::home_tile(state, c, room))
                    dest = *e;
                set_float_goal(state, c, std::nullopt);
                break;
            case behavior::hunt: {
                --x.attack_counter;
                auto const* r = tracker::prey(ts);
                auto const prey = r && creature::live(state, r->who) ? r->who : dcon::creature_id{};
                if (!prey)
                    break;
                bool const seen = r->visual != 0 && creature::here(state, prey, room.id);
                vec2 const pp = seen ? creature::main_pos(state, prey) : vec2{0.f, 0.f};
                if (x.attack_counter > 0) {
                    if (seen)
                        set_float_goal(state, c, pp);
                    else
                        dest = tracker::best_guess(*r);
                } else if (seen) {
                    // backing off before the next go
                    set_float_goal(state, c, pp + custom::dir_vec(pp, pos(state, c, 0)) * 200.f);
                }
                if (x.attack_counter < -50)
                    x.attack_counter = rng.range(200, 400);
                // prey in another collision layer (a leech) is met here
                if (seen && w.creature_get_type(prey) == creature::type::leech) {
                    auto const pm = creature::chunk_of(state, prey, creature::main_chunk_of(state, prey));
                    if (custom::dist_less(pp, pos(state, c, 0), w.body_chunk_get_rad(chunk(state, c, 0)) + w.body_chunk_get_rad(pm)))
                        collide_with(state, c, room, 0, prey, 0);
                }
                break;
            }
            default:
                break;
            }
        }

        //
        // JetFish
        //
        // Creature.Stun + JetFish.Stun: half the time it lets go
        void stun(sys::game_state& state, dcon::creature_id c, int32_t st) {
            if (state.world.creature_get_random(c).value() < 0.5f)
                creature::release_all(state, c, creature::room_of(state, c));
            creature::stun(state, c, st);
        }

        // JetFish.Swim
        void swim_update(sys::game_state& state, dcon::creature_id c, room_view const& room, float speed_fac) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            auto& x = pl(state, c);
            vec2 const m = pos(state, c, 0), c1 = pos(state, c, 1);
            bool const held = creature::held(state, c);
            ivec2 const mt = tile_of(m);
            // squeezing through a corridor: along the path
            if ((pathing::narrow_space(room, mt) || room.get_tile(mt).t == terrain::shortcut_entrance)
            && room.get_tile(m + custom::dir_vec(c1, m) * 20.f).t != terrain::air) {
                auto const f = pathing::field_to(state, room.id, w.creature_get_destination(c), swim);
                bool from_tail = false;
                auto conn = pathing::next_step(state, f, mt);
                if (!conn) {
                    from_tail = true;
                    conn = pathing::next_step(state, f, tile_of(c1));
                }
                if (conn) {
                    vel(state, c, 0) += custom::dir_vec(from_tail ? c1 : m, room_view::middle_of_tile(*conn)) * 1.8f;
                    vec2 const r = custom::rnv(rng);
                    float const a = rng.value();
                    vel(state, c, 1) += r * 2.f * a + custom::dir_vec(m, c1) * rng.value();
                }
                w.body_chunk_set_terrain_squeeze(chunk(state, c, 0), 0.1f);
                w.body_chunk_set_terrain_squeeze(chunk(state, c, 1), 0.1f);
                return;
            }
            float const tail_sub = mathf::pow(sub(state, room, c, 1), 0.2f);
            x.jet_water = mathf::clamp(x.jet_water + mathf::lerp(-0.04f, 0.033333335f, tail_sub), 0.f, 1.f);
            // (no WaterJet particles out of the tentacles)
            vec2 const ahead = custom::dir_vec(c1, m);
            float const jet = mathf::inverse_lerp(0.f, 0.2f, x.jet_water);
            vel(state, c, 0) += ahead * 1.4f * (1.f - tail_sub) * jet * speed_fac;
            x.jet_active = mathf::lerp(x.jet_active, (1.f - tail_sub) * jet, 0.3f);
            x.swim_speed = mathf::lerp(mathf::lerp(1.8f, 3.2f + 6.f * x.dive_speed, x.turn_speed) * (1.f + x.surf_speed * 0.5f), 1.4f, x.slow_down_for_precision)
                * (held ? 1.f : 0.85f);
            vel(state, c, 0) += ahead * x.swim_speed * tail_sub * speed_fac;
            vel(state, c, 1) *= mathf::lerp(1.f, 0.8f, tail_sub);
            if (rw::magnitude(x.swim_dir) > 0.f)
                x.swim_dir = rw::slerp(rw::normalized(x.swim_dir), custom::rnv(rng), 0.2f);
            else
                x.swim_dir = rw::slerp(custom::dir_vec(c1, m), custom::rnv(rng), 0.3f);
            float const k = (held ? 1.f : 0.7f) * 2.5f * speed_fac;
            vel(state, c, 0) += x.swim_dir * k;
            vel(state, c, 1) -= x.swim_dir * k;
            if (sub(state, room, c, 0) < sub(state, room, c, 1) && x.swim_dir.y > 0.f)
                vel(state, c, 0).y += 4.f * sub(state, room, c, 1);
            x.swim_dir = vec2{0.f, 0.f};
        }

        // JetFish.Act
        void act(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            ai_update(state, c, room);
            auto& x = pl(state, c);
            vec2 const m = pos(state, c, 0);
            // (no GetUnstuck: the StuckTracker isn't ported)
            auto const f = pathing::field_to(state, room.id, w.creature_get_destination(c), swim);
            auto const next = pathing::next_step(state, f, tile_of(m));
            auto const& goal = w.jetfish_data_get_float_goal(data(state, c));
            if (goal && room.visual_contact(m, *goal)) {
                x.swim_dir = custom::dir_vec(m, *goal);
            } else if (next) {
                x.swim_dir = custom::dir_vec(m, room_view::middle_of_tile(*next));
                // a few steps further along, while it can see them
                ivec2 step = *next;
                for (int k = 0; k < 4; ++k) {
                    auto const n2 = pathing::next_step(state, f, step);
                    if (!n2 || !room.visual_contact(room_view::middle_of_tile(step), room_view::middle_of_tile(*n2)))
                        break;
                    x.swim_dir += custom::dir_vec(m, room_view::middle_of_tile(*n2));
                    step = *n2;
                    if (pathing::narrow_space(room, *n2)) {
                        x.slow_down_for_precision += 0.3f;
                        break;
                    }
                }
                x.swim_dir = rw::normalized(x.swim_dir);
            }
            x.slow_down_for_precision = mathf::clamp(x.slow_down_for_precision - 0.1f, 0.f, 1.f);
            // flopping about on the ground
            if (physics::object_submersion(state, room, body(state, c)) < 0.4f
            && (w.body_chunk_get_contact_point(chunk(state, c, 0)).y < 0 || w.body_chunk_get_contact_point(chunk(state, c, 1)).y < 0) && rng.value() < 0.05f) {
                float const ang = mathf::lerp(-45.f, 45.f, rng.value());
                vel(state, c, 0) += custom::deg_to_vec(ang) * mathf::lerp(6.f, 26.f, rng.value());
                // (no Jet_Fish_On_Land_Jump sound)
            }
            swim_update(state, c, room, 1.f);
        }

        // JetFish.GrabbedByPlayer: the slugcat steers it
        void grabbed_by_player(sys::game_state& state, dcon::creature_id c, room_view const& room, dcon::player_id p) {
            auto& w = state.world;
            auto& x = pl(state, c);
            auto const pc = player::creature_of(state, p);
            x.get_away_from = pc;
            x.get_away_counter = 150;
            float& like = like_of(state, c, pc);
            like = mathf::lerp(like, -1.f, 0.00025f);
            auto const& in = w.player_get_input(p);
            if (rw::magnitude(in[0].analogue_dir) > 0.f)
                x.swim_dir = rw::normalized(in[0].analogue_dir);
            else if (in[0].x != 0 || in[0].y != 0)
                x.swim_dir = rw::normalized(vec2{float(in[0].x), float(in[0].y)});
            float const weight = w.player_get_stats(p).body_weight_fac;
            for (uint32_t k = 0; k < 2; ++k) {
                float b = 0.97f;
                if (weight <= 1.f)
                    b = 0.95f;
                // (MoreSlugcats: the Gourmand's weight)
                if (weight >= 1.3f)
                    b = 0.9f;
                auto const pk = player::body_chunk(state, p, k);
                w.body_chunk_get_vel(pk) *= mathf::lerp(1.f, b, physics::submersion(state, room, pk));
            }
            if (in[0].jmp && !in[1].jmp
            && (w.body_chunk_get_contact_point(chunk(state, c, 0)).y < 0 || w.body_chunk_get_contact_point(chunk(state, c, 1)).y < 0))
                vel(state, c, 1).y += 8.f;
            swim_update(state, c, room, weight > 1.f ? 1.2f : 1.f);
        }

        // JetFish.CarryObject: the prey in front of its head
        void carry_object(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            auto const t = grasped(state, c);
            if (!t || creature::grasp_chunk(w, c, 0) >= creature::chunk_count_of(state, t)) {
                creature::release_all(state, c, room.id);
                return;
            }
            auto const gc = creature::chunk_of(state, t, creature::grasp_chunk(w, c, 0));
            vec2 const m = pos(state, c, 0);
            bool const no_den = w.creature_get_home_den(c) == dens::no_den;
            if ((physics::object_submersion(state, room, body(state, c)) < 0.1f && rng.value() < 0.025f) || !custom::dist_less(m, w.body_chunk_get_pos(gc), 100.f)
            || (no_den && rng.value() < 0.025f)) {
                creature::release_all(state, c, room.id);
                return;
            }
            w.body_chunk_set_pos(gc, m + custom::dir_vec(pos(state, c, 1), m) * 10.f);
            w.body_chunk_set_vel(gc, vel(state, c, 0));
        }

        // JetFish.Collide (and JetFishAI's for prey in another layer)
        void collide_with(sys::game_state& state, dcon::creature_id c, room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j) {
            auto& w = state.world;
            if (!consious(state, c) || !creature::live(state, t) || j >= creature::chunk_count_of(state, t))
                return;
            auto& x = pl(state, c);
            auto const mine = chunk(state, c, i);
            auto const theirs = creature::chunk_of(state, t, j);
            float const num = rw::distance(w.body_chunk_get_vel(mine), w.body_chunk_get_vel(theirs));
            if (creature::is_slugcat(w, t) && num < 8.f)
                x.grabable = std::max(x.grabable, 7);
            if (num > 12.f) {
                // rammed (no Jet_Fish_Ram_Creature sound)
                vec2 const mp = w.body_chunk_get_pos(mine);
                vec2 const at = mp + custom::dir_vec(mp, w.body_chunk_get_pos(theirs)) * w.body_chunk_get_rad(mine);
                creature::violence_on(state, t, j, w.body_chunk_get_vel(mine) * w.body_chunk_get_mass(mine), 0.1f, 10.f, creature::damage_type::blunt,
                    creature::tmpl::jet_fish);
                particles::bubbles(state, room.id, at, 5, 18.f);
                return;
            }
            // (WantToEatObject is fruit: never a creature)
            if (!creature::is(w, t, creature::type::jetfish) && w.body_chunk_get_pos(theirs).y < w.body_chunk_get_pos(mine).y && x.attack_counter > 0) {
                // pushing it under
                w.body_chunk_get_vel(theirs).y -= num / w.body_chunk_get_mass(theirs);
                w.body_chunk_get_vel(mine).y += num / 2.f;
                int32_t limit = 30;
                auto& m = w.jetfish_data_get_social(data(state, c));
                if (int32_t const k = find_social(m, t); k >= 0) {
                    float& like = m.like[uint32_t(k)];
                    if (like > -0.5f)
                        like = mathf::lerp(like, 0.f, 0.001f);
                    if (like >= 0.f)
                        limit = 10 + int32_t(20.f * mathf::inverse_lerp(1.f, 0.f, like));
                    else
                        limit = 30 + int32_t(220.f * mathf::inverse_lerp(0.f, -1.f, like));
                }
                if (x.attack_counter > limit)
                    x.attack_counter = limit;
            }
            // small or dead prey: in its mouth (no Jet_Fish_Grab sounds)
            if (i == 0 && !creature::grasped_object(w, c, 0) && !creature::held(state, c)
            && (creature::dead(state, t) || creature::mass_of(state, t) < creature::total_mass(state, c) * 0.7f)
            && creature::static_relationship(state, c, t).type == creature::rel::eats)
                creature::grab(state, c, 0, t, j);
        }
    }

    individual_variations const& ivars(sys::game_state const& state, dcon::creature_id c) noexcept {
        return state.world.jetfish_data_get_ivars(data(state, c));
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::jetfish);
        auto const d = data(state, c);
        rw::rng rng{state.random.next() | 1u};
        int32_t seed = int32_t(rng.next() & 0x7FFFFFFFu);
        if (state.spawn_seed >= 0)
            seed = state.spawn_seed;
        physics::object_desc desc;
        desc.gravity = 0.9f;
        desc.air_friction = 0.999f;
        desc.bounce = 0.1f;
        desc.surface_friction = 0.4f;
        desc.water_friction = 0.995f;
        desc.buoyancy = 0.99f;
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
        // two chunks (8.5 and 8, 0.35 each) 14 apart
        auto const c0 = physics::add_chunk(state, o, at, 8.5f, 0.35f);
        auto const c1 = physics::add_chunk(state, o, at + vec2{0.f, -1.f}, 8.f, 0.35f);
        physics::add_connection(state, o, c0, c1, 14.f, physics::connection_type::normal, 1.f, 0.5f);
        physics::set_go_through_floors(state, o, true);
        w.try_create_creature_body(c, o);
        // GenerateIVars, from the ID's seed
        individual_variations iv;
        {
            rw::unity_random r{seed};
            iv.fatness = custom::clamped_random_variation(0.5f, 0.1f, 0.5f, r) * 2.f;
            iv.whiskers = uint8_t(r.range(0, 4));
            iv.tentacle_length = r.value();
            iv.tentacle_fatness = 1.f - iv.tentacle_length;
            iv.tentacle_contour = uint8_t(r.range(0, 3));
            iv.whisker_seed = r.range(0, 2147483647);
            iv.flipper = uint8_t(r.range(0, 5));
            iv.flipper_size = mathf::lerp(0.7f, 1.1f, r.value());
            iv.flipper_orientation = r.value();
            iv.eye_color = custom::hsl2rgb(0.8333333f, 1.f, 0.5f);
        }
        // albino in Shoreline (SB) and the regions with albinos
        bool albino = false;
        if (auto const region = w.room_get_region_from_region_membership(room))
            albino = state.to_string_view(w.region_get_name(region)) == "SB" || w.region_get_albinos(region);
        if (albino)
            iv.eye_color = rw::vec3{1.f, 0.f, 0.f};
        auto& x = pl(state, c);
        x.swim_dir = vec2{0.f, 0.f};
        x.swim_speed = 0.f;
        x.jet_active = 0.f;
        x.jet_water = 0.f;
        x.slow_down_for_precision = 0.f;
        x.turn_speed = 0.f;
        x.dive_speed = 0.f;
        x.surf_speed = 0.f;
        x.grabable = 0;
        x.attack_counter = 0;
        x.get_away_counter = 0;
        x.get_away_from = dcon::creature_id{};
        x.all_dry = 0;
        x.albino = albino ? 1 : 0;
        w.creature_set_random(c, rng);
        w.creature_set_graphics_random(c, rw::rng{(rng.state ^ 0x7E7F15Bu) | 1u});
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_set_grasps(c, creature::grasp_pair{});
        w.creature_set_behavior(c, uint8_t(behavior::idle));
        w.creature_set_current_utility(c, 0.f);
        w.creature_set_destination(c, tile_of(at));
        w.jetfish_data_set_seed(d, seed);
        w.jetfish_data_set_ivars(d, iv);
        w.jetfish_data_set_trail(d, trail_state{});
        w.jetfish_data_set_float_goal(d, std::nullopt);
        w.jetfish_data_set_explore(d, std::nullopt);
        w.jetfish_data_set_social(d, social_memory{});
        tracker::clear(state, c);
        jetfish_graphics::reset_positions(state, c);
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        creature::release_target(state, c);
        creature::erase(state, c);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto& rng = w.creature_get_random(c);
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
        // Creature.Update + PhysicalObject.Update (no terrain impact sounds)
        creature::stun_countdown(state, c);
        physics::update_object(state, room, o, nullptr, nullptr);
        auto& x = pl(state, c);
        auto const d = data(state, c);
        bool const awake = consious(state, c);
        float const submersion = physics::object_submersion(state, room, o);
        // near the surface or out of the water: a slugcat can grab it
        if (!awake || here(state, c).y >= room.default_water_level - 1 || tile_of(pos(state, c, 1)).y >= room.default_water_level - 1 || submersion < 1.f)
            x.grabable = 40;
        else if (x.grabable > 0)
            --x.grabable;
        w.body_chunk_set_terrain_squeeze(chunk(state, c, 0), 1.f);
        w.body_chunk_set_terrain_squeeze(chunk(state, c, 1), 1.f);
        // the trail: how fast it turns
        auto& tr = w.jetfish_data_get_trail(d);
        for (uint32_t k = trail_length - 1; k > 0; --k)
            tr.dir[k] = tr.dir[k - 1];
        tr.dir[0] = rw::normalized(pos(state, c, 1) - pos(state, c, 0));
        if (tr.count < trail_length)
            ++tr.count;
        if (tr.count > 1) {
            float num = 0.f;
            for (uint32_t k = 0; k + 1 < tr.count; ++k)
                num += rw::dot(tr.dir[k], tr.dir[k + 1]);
            num /= float(tr.count - 1);
            num = mathf::inverse_lerp(0.99f, 0.98f, num);
            if (x.turn_speed < num)
                x.turn_speed = mathf::min(x.turn_speed + 0.05f, num);
            else
                x.turn_speed = mathf::max(x.turn_speed - 0.0125f, num);
        }
        // diving in from the air
        if (x.all_dry) {
            if (submersion == 1.f) {
                x.dive_speed = mathf::pow(mathf::inverse_lerp(1.f, -0.5f, custom::dir_vec(pos(state, c, 1), pos(state, c, 0)).y)
                    * mathf::inverse_lerp(0.f, -20.f, vel(state, c, 0).y), 0.65f);
                x.all_dry = 0;
            }
        } else if (submersion == 0.f) {
            x.all_dry = 1;
        }
        x.dive_speed = mathf::max(x.dive_speed - 0.016666668f, 0.f);
        // (no Jet_Fish_Water_Jet_LOOP)
        float surf = 0.f;
        if (submersion > 0.f && submersion < 1.f)
            surf = std::fabs(rw::normalized(pos(state, c, 0) - pos(state, c, 1)).y);
        if (x.surf_speed < surf)
            x.surf_speed = mathf::min(x.surf_speed + 0.05f, surf);
        else
            x.surf_speed = mathf::max(x.surf_speed - 0.0125f, surf);
        if (awake) {
            w.physical_object_set_water_friction(o, 0.995f);
            w.physical_object_set_water_retardation_immunity(o, 0.8f);
            if (auto const p = holding_slugcat(state, c)) {
                grabbed_by_player(state, c, room, p);
            } else {
                x.turn_speed = 0.f;
                x.dive_speed *= 0.8f;
                x.surf_speed *= 0.9f;
                act(state, c, room);
            }
        } else {
            w.physical_object_set_water_friction(o, 0.95f);
            w.physical_object_set_water_retardation_immunity(o, 0.f);
        }
        if (creature::grasped_object(w, c, 0) && w.creature_is_valid(c))
            carry_object(state, c, room);
    }

    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) {
        // what holds it and what it carries are kept apart by the carrying
        return !creature::holds(state.world, t, c) && !creature::holds(state.world, c, t);
    }

    void collide(sys::game_state& state, dcon::creature_id c, room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j) {
        collide_with(state, c, room, i, t, j);
    }

    void violence(sys::game_state& state, dcon::creature_id c, uint32_t k, vec2 momentum, float damage, float stun_bonus, creature::damage_type) {
        auto& w = state.world;
        auto& rng = w.creature_get_random(c);
        // Creature.Violence (HealthState)
        auto const hit = chunk(state, c, k < 2 ? k : 0);
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

    void release(sys::game_state&, dcon::creature_id, dcon::creature_id) {
        // only its mouth holds on: the generic grasp, let go of by
        // creature::release (getAwayFromCreature runs out on its own)
    }

    void died(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        w.physical_object_set_water_retardation_immunity(body(state, c), 0.2f);
        w.physical_object_set_buoyancy(body(state, c), 0.92f);
        pl(state, c).jet_active = 0.f;
    }

    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const view = world::get_room_view(state, room);
        ivec2 const t = tile_of(at);
        vec2 const a = rw::to_vec2(view.shortcut_entrance_hole_direction(t));
        vec2 const mid = room_view::middle_of_tile(t);
        for (uint32_t i = 0; i < 2; ++i) {
            auto const k = chunk(state, c, i);
            physics::hard_set_position(state, k, mid - a * (-1.5f + float(i)) * 15.f);
            w.body_chunk_set_last_pos(k, mid);
            w.body_chunk_set_vel(k, a * 8.f);
        }
        // JetFishAI.NewRoom: what it was swimming to is in the other room
        w.jetfish_data_set_float_goal(data(state, c), std::nullopt);
        w.jetfish_data_set_explore(data(state, c), std::nullopt);
        pl(state, c).jet_water = 0.f;
    }

    std::string describe(sys::game_state const& state, dcon::creature_id c) {
        static constexpr char const* names[] = {"idle", "flee", "hunt", "escape rain", "return prey", "go to food", "get unstuck"};
        auto const& w = state.world;
        vec2 const p = pos(state, c, 0);
        uint32_t const b = w.creature_get_behavior(c);
        return "jetfish at " + std::to_string(int(p.x)) + "," + std::to_string(int(p.y)) + " behavior " + (b < 7 ? names[b] : "?")
            + (creature::held(state, c) ? " ridden" : "") + (creature::grasped_object(w, c, 0) ? " carrying prey" : "");
    }

    bool spawn_spot(room_view const& room, ivec2 tile) {
        return room.inside(tile.x, tile.y) && !room.get_tile(tile).solid() && room.get_tile(tile).water != 0;
    }

    //
    // The player side
    //
    dcon::creature_id held_by(sys::game_state const& state, dcon::player_id p) noexcept {
        if (!p)
            return dcon::creature_id{};
        auto const c = creature::grasped(state.world, player::creature_of(state, p), 0);
        return c && creature::is(state.world, c, creature::type::jetfish) ? c : dcon::creature_id{};
    }

    bool riding(sys::game_state const& state, dcon::player_id p) noexcept {
        auto const c = held_by(state, p);
        return c && consious(state, c);
    }

    dcon::creature_id pickup_candidate(sys::game_state const& state, dcon::player_id p, room_view const& room, float& score) {
        auto const& w = state.world;
        vec2 const p0 = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
        int32_t const flip = w.player_get_flip_direction(p);
        dcon::creature_id result;
        score = 3.4e38f;
        rooms::for_each_creature(state, room.id, creature::type::jetfish, [&](dcon::creature_id c) {
            // Player.Grabability: TwoHands while grabable
            if (creature::held(state, c) || w.creature_get_away(c) != 0 || pl(state, c).grabable <= 0)
                return;
            for (uint32_t i = 0; i < 2; ++i) {
                auto const k = chunk(state, c, i);
                vec2 const cp = w.body_chunk_get_pos(k);
                float const rad = w.body_chunk_get_rad(k);
                if (!custom::dist_less(p0, cp, rad + 40.f))
                    continue;
                if (!custom::dist_less(p0, cp, rad + 20.f) && !room.visual_contact(p0, cp))
                    continue;
                float dist = rw::distance(p0, cp);
                if ((cp.x < p0.x) == (flip < 0))
                    dist -= 10.f;
                if (dist < score) {
                    result = c;
                    score = dist;
                }
            }
        });
        return result;
    }

    void grab(sys::game_state& state, dcon::player_id p, dcon::creature_id c) {
        // SlugcatGrab: chunk 0 (a TwoHands object isn't dragged by its closest)
        creature::grab(state, player::creature_of(state, p), 0, c, 0);
    }

    void release(sys::game_state& state, dcon::creature_id c) {
        uint32_t g = 0;
        auto const h = creature::holder(state, c, &g);
        if (h && creature::is_slugcat(state.world, h))
            creature::release_grasp(state, h, g);
    }

    void release_player(sys::game_state& state, dcon::player_id p) {
        if (auto const c = held_by(state, p))
            release(state, c);
    }

    void carry_update(sys::game_state& state, dcon::player_id p) {
        auto& w = state.world;
        auto const c = held_by(state, p);
        if (!c)
            return;
        auto const pc = player::creature_of(state, p);
        auto const main = player::body_chunk(state, p, 0);
        auto const gc = chunk(state, c, creature::grasp_chunk(w, pc, 0) < 2 ? creature::grasp_chunk(w, pc, 0) : 0);
        vec2& mp = w.body_chunk_get_pos(main);
        vec2& gp = w.body_chunk_get_pos(gc);
        vec2 const a = custom::dir_vec(mp, gp);
        float const dist = rw::distance(mp, gp);
        // (eatMeat is always 0: no meat eating)
        float const want = 5.f + w.body_chunk_get_rad(gc);
        float ratio = w.body_chunk_get_mass(gc) / (w.body_chunk_get_mass(main) + w.body_chunk_get_mass(gc));
        bool const entering = w.creature_get_entering_shortcut(pc).x >= 0;
        if (entering)
            ratio = 0.f;
        else if (creature::total_mass(state, c) < creature::mass_of(state, pc))
            ratio /= 2.f;
        if (!entering || dist > want) {
            mp += a * (dist - want) * ratio;
            w.body_chunk_get_vel(main) += a * (dist - want) * ratio;
            gp -= a * (dist - want) * (1.f - ratio);
            w.body_chunk_get_vel(gc) -= a * (dist - want) * (1.f - ratio);
        }
        if (w.player_get_body_mode(p) == player::body_mode_index::climbing_on_beam && w.player_get_animation(p) != player::animation_index::beam_tip
        && w.player_get_animation(p) != player::animation_index::stand_on_beam) {
            auto const room = world::get_room_view(state, creature::room_of(state, c));
            w.body_chunk_get_vel(gc).y += w.physical_object_get_g(body(state, c)) * (1.f - physics::submersion(state, room, gc)) * 0.75f;
        }
    }

    void follow_player(sys::game_state& state, dcon::player_id p, dcon::room_id room, bool spit_out) {
        auto& w = state.world;
        auto const c = held_by(state, p);
        if (!c)
            return;
        w.physical_object_set_room_from_object_location(body(state, c), room);
        if (!spit_out)
            return;
        vec2 const at = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
        for (uint32_t i = 0; i < 2; ++i) {
            physics::hard_set_position(state, chunk(state, c, i), at + vec2{0.f, float(i) * -5.f});
            w.body_chunk_set_vel(chunk(state, c, i), w.body_chunk_get_vel(player::body_chunk(state, p, 0)));
        }
        w.jetfish_data_set_float_goal(data(state, c), std::nullopt);
        w.jetfish_data_set_explore(data(state, c), std::nullopt);
        tracker::clear(state, c);
        jetfish_graphics::reset_positions(state, c);
    }
}
