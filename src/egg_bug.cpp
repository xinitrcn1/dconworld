// EggBug.cs + EggBugAI (the egg bug and Downpour's fire bug), simplified
// where it doesn't touch the slugcat (egg_bug.hpp)

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "egg_bug.hpp"
#include "dens.hpp"
#include "egg_bug_egg.hpp"
#include "egg_bug_graphics.hpp"
#include "items.hpp"
#include "migrate.hpp"
#include "particles.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "rooms.hpp"
#include "tracker.hpp"

namespace egg_bug {
    using creature::relationship;
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // CreatureTemplate: baseDamageResistance, baseStunResistance,
        // instantDeathDamageLimit (EggBug, FireBug), visualRadius (both)
        struct template_params {
            float base_damage_resistance;
            float base_stun_resistance;
            float instant_death_damage_limit;
        };
        constexpr template_params templates[variant_count] = {
            {0.15f, 0.3f, 0.7f},
            {1.4f, 1.3f, 4.f},
        };
        constexpr float visual_radius = 1200.f;
        // EggBugGraphics.HUE_OFF
        constexpr float hue_off = 1.5f;

        creature::egg_bug_payload& pl(sys::game_state& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).egg_bug;
        }
        creature::egg_bug_payload const& pl(sys::game_state const& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).egg_bug;
        }
        dcon::egg_bug_data_id data(sys::game_state const& state, dcon::creature_id c) {
            return creature::egg_bug_data(state.world, c);
        }
        template_params const& tp(sys::game_state const& state, dcon::creature_id c) {
            return templates[uint32_t(pl(state, c).variant) % variant_count];
        }
        vec2& pos(sys::game_state& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_pos(chunk(state, c, i));
        }
        vec2 pos(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_pos(chunk(state, c, i));
        }
        vec2& vel(sys::game_state& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_vel(chunk(state, c, i));
        }
        ivec2 tile_of(vec2 p) {
            return room_view::get_tile_position(p);
        }
        // abstractCreature.pos: the main chunk's tile
        ivec2 here(sys::game_state const& state, dcon::creature_id c) {
            return tile_of(pos(state, c, 0));
        }
        // AImap.TileAccessibleToCreature
        bool accessible(sys::game_state const& state, dcon::creature_id c, room_view const& room, ivec2 t) {
            return room.inside(t.x, t.y) && pathing::accessible(room, t, movement(state, c));
        }
        bool accessible(sys::game_state const& state, dcon::creature_id c, room_view const& room, vec2 p) {
            return accessible(state, c, room, tile_of(p));
        }

        // the tracker's settings: Tracker(10, 10, 450 (FireBug 150), 0.5, 5,
        // 5, 10), ThreatTracker(3), the fire bug's PreyTracker(5, 0.9, 3, 70)
        tracker::params make_params(int32_t remember) {
            tracker::params q;
            q.see_around_corners = 10;
            q.frames_to_remember = remember;
            q.ghost_dismissal_range = 10;
            q.prey_max = 5;
            q.persistance_bias = 0.9f;
            q.sure_to_get = 3.f;
            q.sure_to_lose = 70.f;
            q.threat_max = 3;
            return q;
        }
        tracker::params const& tracker_params(sys::game_state const& state, dcon::creature_id c) {
            static tracker::params const egg = make_params(450);
            static tracker::params const fb = make_params(150);
            return fire(state, c) ? fb : egg;
        }

        //
        // EggBug's own little things
        //
        void lose_all_grasps(sys::game_state& state, dcon::creature_id c) {
            creature::release_grasp(state, c, 0);
            state.world.egg_bug_data_get_fire(data(state, c)).stab_count = 0;
        }

        // EggBugGraphics.ShowEggs
        bool show_eggs(sys::game_state const& state, dcon::creature_id c) {
            return fire(state, c) ? pl(state, c).eggs_left > 0 : !state.world.creature_get_dead(c);
        }

        // EggBugEgg.LiquidDrip's colour without the palette: the eggs'
        // colour (EggColors[1]) darkened
        vec3 drip_color(sys::game_state const& state, dcon::creature_id c) {
            float const h = pl(state, c).hue + (fire(state, c) ? hue_off : 1.5f);
            return custom::hsl2rgb(custom::decimal(h), 1.f, 0.5f) * 0.6f;
        }

        // EggBug.Squirt / EggBugGraphics.Squirt: drips off the eggs
        // (cosmetic, where a camera looks, rolled from the graphics'
        // generator; the eggs taken on the body, never the graphics
        // module's: the simulation doesn't read it)
        void squirt(sys::game_state& state, dcon::creature_id c, float intensity) {
            auto& w = state.world;
            auto const room = creature::room_of(state, c);
            if (!state.viewed(room) || !show_eggs(state, c))
                return;
            auto& rng = w.creature_get_graphics_random(c);
            vec3 const col = drip_color(state, c);
            vec2 const c0 = pos(state, c, 0), c1 = pos(state, c, 1);
            vec2 const back = custom::dir_vec(c0, c1);
            vec2 const side = custom::perpendicular_vector(back);
            for (int s = 0; s < 2; ++s)
                for (int e = 0; e < 3; ++e) {
                    if (rng.value() >= mathf::lerp(0.07f, 0.4f, intensity))
                        continue;
                    vec2 const at = c1 + back * (2.f + 4.f * float(e)) + side * (s == 0 ? -5.f : 5.f);
                    vec2 d = custom::dir_vec(c1, at) * mathf::lerp(3.f, 7.f, mathf::pow(rng.value(), 1.5f - 0.6f * intensity))
                        + custom::rnv(rng) * rng.value() * 4.f;
                    d *= mathf::pow(rng.value(), mathf::lerp(1.4f, 0.4f, intensity));
                    particles::spawn(state, room, particles::kind::drop, at, d, 1.5f, 0.75f, col);
                }
        }

        // EggBug.TryJump: a leap away from `away`
        void try_jump(sys::game_state& state, dcon::creature_id c, room_view const& room, vec2 away) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            auto& x = pl(state, c);
            if (consious(state, c))
                squirt(state, c, 0.5f + 0.5f * x.fear);
            if (!consious(state, c) || x.no_jumps > 0 || (!accessible(state, c, room, pos(state, c, 0)) && !accessible(state, c, room, pos(state, c, 1)))
            || pathing::narrow_space(room, tile_of(pos(state, c, 1))))
                return;
            // (no Egg_Bug_Scurry sound)
            vec2 v = custom::dir_vec(away, (pos(state, c, 0) + pos(state, c, 1)) / 2.f);
            v += custom::rnv(rng) * 0.3f;
            v = rw::normalized(v);
            v = rw::slerp(v, vec2{0.f, 1.f}, custom::lerp_map(v.y, -0.5f, 0.5f, 0.7f, 0.3f));
            vel(state, c, 0) *= 0.5f;
            vel(state, c, 1) *= 0.5f;
            vel(state, c, 0) += v * 17.f + custom::rnv(rng) * 5.f * rng.value();
            vel(state, c, 1) += v * 17.f + custom::rnv(rng) * 5.f * rng.value();
            x.footing_counter = 0;
            vec2 const a = custom::perpendicular_vector(v) * (rng.value() < 0.5f ? -1.f : 1.f);
            vel(state, c, 0) += a * 11.f;
            vel(state, c, 1) -= a * 11.f;
            x.no_jumps = 90;
        }

        // EggBug.Suprise
        void suprise(sys::game_state& state, dcon::creature_id c, room_view const& room, vec2 at) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            auto& x = pl(state, c);
            if (!consious(state, c))
                return;
            if (custom::dist_less(at, pos(state, c, 0), 300.f))
                for (uint32_t i = 0; i < 2; ++i)
                    if (accessible(state, c, room, pos(state, c, i)))
                        vel(state, c, i) += (custom::rnv(rng) * 4.f + custom::dir_vec(at, pos(state, c, i)) * 2.f) * (0.5f + 0.5f * x.fear);
            x.shake = int16_t(std::max(int32_t(x.shake), rng.range(5, 15)));
            x.fear = custom::lerp_and_tick(x.fear, 1.f, 0.3f, 0.14285715f);
            squirt(state, c, x.fear);
        }

        //
        // EggBugAI
        //
        struct hook_ctx {
            sys::game_state& state;
            dcon::creature_id c;
            room_view const& room;
            // the field to where it is (PathFinder.CoordinateReachableAndGetbackable,
            // judged by the way back), built on the first question
            std::optional<pathing::field_id> from_here;
        };
        bool reachable(hook_ctx& x, ivec2 t) {
            if (!accessible(x.state, x.c, x.room, t))
                return false;
            if (!x.from_here)
                x.from_here = pathing::field_to(x.state, x.room.id, here(x.state, x.c), movement(x.state, x.c));
            return pathing::distance(x.state, *x.from_here, t) != pathing::unreachable;
        }

        // ArtificialIntelligence.VisualContact (EggBugAI has no VisualScore
        // of its own): within the visual radius and in sight
        bool hook_sees_point(void* u, vec2 p) {
            auto& x = *static_cast<hook_ctx*>(u);
            return custom::dist_less(pos(x.state, x.c, 0), p, visual_radius) && x.room.visual_contact(pos(x.state, x.c, 0), p);
        }
        bool hook_sees(void* u, dcon::body_chunk_id k) {
            auto& x = *static_cast<hook_ctx*>(u);
            return hook_sees_point(u, x.state.world.body_chunk_get_pos(k));
        }
        // EggBugAI.UpdateDynamicRelationship: the static one, nothing to
        // fear of the dead (TrackedCreatureState.alive: the representation's
        // `state`, 1 dead)
        relationship hook_relate(void* u, creature::tracked const& rc) {
            auto& x = *static_cast<hook_ctx*>(u);
            auto& r = const_cast<creature::tracked&>(rc);
            if (r.visual && creature::live(x.state, r.who))
                r.state = creature::dead(x.state, r.who) ? 1 : 0;
            relationship rel = creature::static_relationship(x.state, x.c, r.who);
            if (rel.type == creature::rel::afraid && r.state != 0)
                rel.intensity = 0.f;
            return rel;
        }
        // IUseARelationshipTracker.ModuleToTrackRelationship: Afraid a
        // threat, the fire bug's Eats prey
        creature::track_module module_egg(relationship r) {
            return r.type == creature::rel::afraid ? creature::track_module::threat : creature::track_module::none;
        }
        creature::track_module module_fire(relationship r) {
            if (r.type == creature::rel::eats)
                return creature::track_module::prey;
            return module_egg(r);
        }
        // EggBugAI.CreatureSpotted
        void hook_spotted(void* u, creature::tracked const& r, bool first) {
            auto& x = *static_cast<hook_ctx*>(u);
            auto& state = x.state;
            auto const c = x.c;
            auto& rng = state.world.creature_get_random(c);
            if (!first && rng.value() > pl(state, c).fear)
                return;
            relationship const rel = r.rel;
            if (rel.type == creature::rel::ignores)
                return;
            vec2 const at = room_view::middle_of_tile(tracker::best_guess(r));
            if (first && rel.type == creature::rel::afraid && rel.intensity > 0.06f
            && custom::dist_less(pos(state, c, 0), at, custom::lerp_map(rel.intensity, 0.06f, 0.5f, 50.f, 300.f)))
                try_jump(state, c, x.room, at);
            if (rel.intensity > (first ? 0.02f : 0.1f))
                suprise(state, c, x.room, at);
        }
        bool hook_reach(void* u, ivec2 t) {
            return reachable(*static_cast<hook_ctx*>(u), t);
        }
        tracker::hooks make_hooks(hook_ctx& x) {
            tracker::hooks h;
            h.user = &x;
            h.sees = hook_sees;
            h.sees_point = hook_sees_point;
            h.relate = hook_relate;
            h.module = fire(x.state, x.c) ? module_fire : module_egg;
            h.spotted = hook_spotted;
            h.reach = hook_reach;
            return h;
        }

        // EggBugAI.IdleScore: lower is better
        float idle_score(hook_ctx& x, ivec2 coord) {
            auto& state = x.state;
            auto const& ts = tracker::of(state, x.c);
            auto const& p = tracker_params(state, x.c);
            if (!x.room.inside(coord.x, coord.y) || !reachable(x, coord) || pathing::tile_access(x.room, coord) >= pathing::access::wall)
                return std::numeric_limits<float>::max();
            float num = 1.f;
            if (reachable(x, coord + ivec2{0, -1}))
                num += 10.f;
            if (pathing::narrow_space(x.room, coord))
                num += 50.f;
            num += tracker::threat_of_tile(ts, coord, p.see_around_corners) * 1000.f;
            num += tracker::threat_of_tile(ts, tile_of((room_view::middle_of_tile(coord) + room_view::middle_of_tile(here(state, x.c))) / 2.f), p.see_around_corners) * 1000.f;
            // (the NoiseTracker's sources: this tick's noises)
            for (auto const& n : state.noises(x.room.id))
                num += custom::lerp_map(rw::distance(room_view::middle_of_tile(coord), n.pos), 40.f, 400.f, 100.f, 0.f);
            return num;
        }

        // EggBugAI.LethalBehavior: after the prey, and into its jaws when close
        void lethal_behavior(sys::game_state& state, dcon::creature_id c, room_view const& room, creature::tracked const& target) {
            auto& w = state.world;
            w.creature_set_destination(c, tracker::best_guess(target));
            auto const t = target.who;
            if (!creature::here(state, t, room.id) || creature::dead(state, t) || creature::grasped_object(w, c, 0))
                return;
            vec2 const main = pos(state, c, 0);
            vec2 const mouth = main + custom::dir_vec(pos(state, c, 1), main) * 20.f;
            uint32_t const n = creature::chunk_count_of(state, t);
            for (uint32_t i = 0; i < n; ++i) {
                if (!custom::dist_less(w.body_chunk_get_pos(creature::chunk_of(state, t, i)), main, 15.f))
                    continue;
                for (uint32_t j = 0; j < n; ++j) {
                    auto const k = creature::chunk_of(state, t, j);
                    if (custom::dist_less(mouth, w.body_chunk_get_pos(k), w.body_chunk_get_rad(k) + 25.f)) {
                        creature::grab(state, c, 0, t, j);
                        return;
                    }
                }
            }
        }

        // EggBugAI.ReactToNoise, for the noises of this tick it hears
        // (NoiseTracker: hearingSkill 2 sitting, else 0.2)
        void hear_noises(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            auto& x = pl(state, c);
            auto& ai = w.egg_bug_data_get_ai(data(state, c));
            float const hearing = x.sitting ? 2.f : 0.2f;
            vec2 const main = pos(state, c, 0);
            for (auto const& n : state.noises(room.id)) {
                if (n.source == body(state, c) || !custom::dist_less(n.pos, main, n.strength * hearing))
                    continue;
                w.egg_bug_data_get_move(data(state, c)).antenna_dir = n.pos;
                x.antenna_attention = 1.f;
                if (ai.noise_reaction_delay > 0)
                    continue;
                if (n.strength > 160.f && custom::dist_less(n.pos, main, mathf::lerp(x.sitting ? 300.f : 100.f, 600.f, x.fear)))
                    try_jump(state, c, room, n.pos);
                suprise(state, c, room, n.pos);
                ai.noise_reaction_delay = rng.range(0, 30);
            }
        }

        // EggBugAI.Update
        void ai_update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            auto const d = data(state, c);
            ivec2 const at = here(state, c);
            auto const& p = tracker_params(state, c);
            hook_ctx hx{state, c, room, std::nullopt};
            tracker::hooks const h = make_hooks(hx);
            tracker::update(state, c, room, at, p, h);
            auto const& ts = tracker::of(state, c);
            auto& x = pl(state, c);
            auto& ai = w.egg_bug_data_get_ai(d);
            bool const fb = fire(state, c);

            // UtilityComparer: threat (smoothed up by Lerp 0.5, down by Tick
            // 0.005; its weight from the threat where it stands), rain, the
            // fire bug's prey (Lerp 0.15, weight 0.6)
            float const threat = tracker::threat_utility(ts);
            float const weight = custom::lerp_map(tracker::threat_of_tile(ts, at, p.see_around_corners), 0.1f, 2.f, 0.1f, 1.f, 0.5f);
            float const tt = threat * weight;
            ai.threat_smoothed = tt > ai.threat_smoothed ? mathf::lerp(ai.threat_smoothed, tt, 0.5f) : mathf::max(ai.threat_smoothed - 0.005f, tt);
            float const rain = dens::rain_utility(state, c, room.id);
            float prey = 0.f;
            if (fb) {
                ai.prey_smoothed = mathf::lerp(ai.prey_smoothed, tracker::prey_utility(state, c, room, at, p) * 0.6f, 0.15f);
                prey = ai.prey_smoothed;
            }
            enum class top { threat, rain, prey } best = top::threat;
            float utility = ai.threat_smoothed;
            if (rain > utility) {
                utility = rain;
                best = top::rain;
            }
            if (fb && prey > utility) {
                utility = prey;
                best = top::prey;
            }
            w.creature_set_current_utility(c, utility);

            behavior b = behavior(w.creature_get_behavior(c));
            bool const no_eggs = fb && x.eggs_left <= 0;
            if (no_eggs && utility < 0.02f)
                b = behavior::hunt;
            auto const* prey_rep = tracker::prey(ts);
            if (best == top::threat) {
                b = behavior::flee;
            } else if (best == top::rain) {
                b = behavior::escape_rain;
            } else if (no_eggs && prey_rep && creature::live(state, prey_rep->who) && !creature::dead(state, prey_rep->who) && !grasped(state, c)) {
                b = behavior::kill;
            }
            if (!no_eggs && utility < 0.02f)
                b = behavior::idle;
            w.creature_set_behavior(c, uint8_t(b));

            ivec2& dest = w.creature_get_destination(c);
            switch (b) {
            case behavior::idle:
            case behavior::hunt: {
                x.run_speed = custom::lerp_and_tick(x.run_speed, 0.5f + 0.5f * mathf::max(threat, x.fear), 0.01f, 0.016666668f);
                if (ai.walk_with_bug) {
                    dest = *ai.walk_with_bug;
                    if (rng.value() < 0.02f || custom::manhattan_distance(at, *ai.walk_with_bug) < 4)
                        ai.walk_with_bug = std::nullopt;
                    break;
                }
                // (leaving the room: AbstractBehavior, now and then, the
                // way out its destination)
                if (ai.idle_pos_counter <= 0 && rng.value() < 0.05f)
                    migrate::abstract_behavior(state, c, room);
                if (auto const e = migrate::travel_tile(state, c, room); e && reachable(hx, *e)) {
                    dest = *e;
                    --ai.idle_pos_counter;
                    break;
                }
                ivec2 const coord{rng.range(0, room.width), rng.range(0, room.height)};
                if (idle_score(hx, coord) < idle_score(hx, ai.temp_idle_pos))
                    ai.temp_idle_pos = coord;
                if (idle_score(hx, ai.temp_idle_pos) < idle_score(hx, dest) + custom::lerp_map(float(ai.idle_pos_counter), 0.f, 300.f, 100.f, -300.f)) {
                    dest = ai.temp_idle_pos;
                    ai.idle_pos_counter = rng.range(200, 800);
                    ai.temp_idle_pos = ivec2{rng.range(0, room.width), rng.range(0, room.height)};
                }
                --ai.idle_pos_counter;
                break;
            }
            case behavior::flee: {
                x.run_speed = custom::lerp_and_tick(x.run_speed, 1.f, 0.01f, 0.1f);
                dest = tracker::flee_to(state, c, room, at, 10, 30, p, h);
                // something dangerous right there: a leap away
                if (auto const* r = tracker::threat(ts); r && rng.value() < tracker::panic(ts) && creature::here(state, r->who, room.id)) {
                    uint32_t const n = creature::chunk_count_of(state, r->who);
                    auto const k = creature::chunk_of(state, r->who, uint32_t(rng.range(0, int32_t(n))));
                    vec2 const kp = w.body_chunk_get_pos(k);
                    if (custom::dist_less(pos(state, c, 0), kp, w.body_chunk_get_rad(chunk(state, c, 0)) + w.body_chunk_get_rad(k) + 40.f * x.fear))
                        try_jump(state, c, room, kp);
                }
                break;
            }
            case behavior::escape_rain:
            case behavior::return_prey:
                x.run_speed = custom::lerp_and_tick(x.run_speed, 1.f, 0.01f, 0.1f);
                // DenFinder: the den (dens.hpp)
                if (auto const e = dens::home_tile(state, c, room))
                    dest = *e;
                break;
            case behavior::kill:
                if (prey_rep)
                    lethal_behavior(state, c, room, *prey_rep);
                x.run_speed = custom::lerp_and_tick(x.run_speed, 1.f, 0.025f, 0.1f);
                break;
            }
            x.fear = custom::lerp_and_tick(x.fear, mathf::max(ai.threat_smoothed, mathf::pow(tracker::panic(ts), 0.7f)), 0.07f, 0.033333335f);
            if (ai.noise_reaction_delay > 0)
                --ai.noise_reaction_delay;
            hear_noises(state, c, room);
        }

        // EggBugAI.CollideWithKin: following the dominant one's way
        void collide_with_kin(sys::game_state& state, dcon::creature_id c, dcon::creature_id other) {
            auto& w = state.world;
            if (fire(state, c) != fire(state, other))
                return;
            ivec2 const theirs = w.creature_get_destination(other);
            if (custom::manhattan_distance(here(state, c), theirs) < 4)
                return;
            if ((w.egg_bug_data_get_dominance(data(state, other)) > w.egg_bug_data_get_dominance(data(state, c)) && !pl(state, other).sitting) || pl(state, c).sitting)
                w.egg_bug_data_get_ai(data(state, c)).walk_with_bug = theirs;
        }

        //
        // EggBug
        //
        // StandardPather.FollowPath from the tile `from`
        movement_connection follow_path(sys::game_state const& state, pathing::field_id f, ivec2 from) {
            auto const step = pathing::next_step(state, f, from);
            if (!step)
                return movement_connection{};
            return movement_connection{from, *step, true};
        }

        // EggBug.MoveTowards (no graphics module: the legs' grip count is
        // vanilla's offscreen 0.85)
        void move_towards(sys::game_state& state, dcon::creature_id c, room_view const& room, vec2 to) {
            auto& x = pl(state, c);
            vec2 dir = custom::dir_vec(pos(state, c, 0), to);
            if (!footing(state, c))
                dir *= 0.3f;
            if (physics::is_tile_solid(state, room, chunk(state, c, 1), 0, -1)
            && ((dir.x < -0.5f && pos(state, c, 0).x > pos(state, c, 1).x + 5.f) || (dir.x > 0.5f && pos(state, c, 0).x < pos(state, c, 1).x - 5.f))) {
                // turning around on the floor: the head goes over the body
                float const s = dir.x < 0.f ? -1.f : 1.f;
                vel(state, c, 0).x -= s * 1.3f;
                vel(state, c, 1).x += s * 0.5f;
                if (!physics::is_tile_solid(state, room, chunk(state, c, 0), 0, 1))
                    vel(state, c, 0).y += 3.2f;
            }
            float const num = mathf::pow(0.85f, 0.6f);
            vel(state, c, 0) += dir * 6.2f * x.run_speed * num;
            vel(state, c, 1) -= dir * 1.f * x.run_speed * num;
            physics::set_go_through_floors(state, body(state, c), to.y < pos(state, c, 0).y - 5.f);
        }

        // EggBug.Run. The path fields' steps are Standard moves, open
        // diagonals (a special move) and drops (DropToFloor); no shortcuts
        // or reaches
        void run(sys::game_state& state, dcon::creature_id c, room_view const& room, movement_connection const& following) {
            auto& w = state.world;
            auto& x = pl(state, c);
            auto& mv = w.egg_bug_data_get_move(data(state, c));
            ivec2 const step = following.dest - following.start;
            if (step.x != 0 && step.y != 0) {
                x.special_move_counter = 30;
                mv.special_move_destination = following.dest;
            } else {
                vec2 const target = room_view::middle_of_tile(following.dest);
                if (footing(state, c)) {
                    for (uint32_t j = 0; j < 2; ++j) {
                        if (following.start.x == following.dest.x)
                            vel(state, c, j).x += mathf::min((target.x - pos(state, c, j).x) / 8.f, 1.2f);
                        else if (following.start.y == following.dest.y)
                            vel(state, c, j).y += mathf::min((target.y - pos(state, c, j).y) / 8.f, 1.2f);
                    }
                }
                auto const& last = mv.last_followed;
                if (last.valid && (footing(state, c) || accessible(state, c, room, pos(state, c, 0)))
                && ((following.start.x != following.dest.x && last.start.x == last.dest.x) || (following.start.y != following.dest.y && last.start.y == last.dest.y))) {
                    vel(state, c, 0) *= 0.7f;
                    vel(state, c, 1) *= 0.5f;
                }
                // DropToFloor
                if (step.y < -1)
                    x.footing_counter = 0;
                move_towards(state, c, room, target);
            }
            mv.last_followed = following;
        }

        // EggBug.Swim
        void swim(sys::game_state& state, dcon::creature_id c, room_view const& room, pathing::field_id f) {
            auto& x = pl(state, c);
            float const s0 = physics::submersion(state, room, chunk(state, c, 0));
            float const s1 = physics::submersion(state, room, chunk(state, c, 1));
            vel(state, c, 0) *= 1.f - 0.05f * s0;
            vel(state, c, 1) *= 1.f - 0.1f * s1;
            physics::set_go_through_floors(state, body(state, c), true);
            auto conn = follow_path(state, f, tile_of(pos(state, c, 0)));
            if (!conn.valid)
                conn = follow_path(state, f, tile_of(pos(state, c, 1)));
            ivec2 const at = here(state, c);
            if (!conn.valid && std::abs(at.y - room.default_water_level) < 4)
                conn = follow_path(state, f, ivec2{at.x, room.default_water_level});
            if (!conn.valid) {
                vel(state, c, 0).y += 0.5f;
                return;
            }
            if (conn.start.y == conn.dest.y && conn.dest.y == room.default_water_level) {
                float const side = mathf::sign(room_view::middle_of_tile(conn.start).x - room_view::middle_of_tile(conn.dest).x);
                vel(state, c, 0).x -= side * 1.6f * s0;
                vel(state, c, 1).x += side * 0.5f * s1;
                x.footing_counter = 0;
                return;
            }
            vel(state, c, 0) *= 0.9f;
            vel(state, c, 0) += custom::dir_vec(pos(state, c, 0), room_view::middle_of_tile(conn.dest)) * 1.4f;
            x.footing_counter = int16_t(std::max(int32_t(x.footing_counter), 25));
            run(state, c, room, conn);
            x.out_of_water_footing = 20;
        }

        // EggBug.Act
        void act(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& x = pl(state, c);
            auto& mv = w.egg_bug_data_get_move(data(state, c));
            auto const f = pathing::field_to(state, room.id, w.creature_get_destination(c), movement(state, c));
            if (physics::object_submersion(state, room, body(state, c)) > 0.3f) {
                x.swimming = 1;
                swim(state, c, room, f);
                ai_update(state, c, room);
                return;
            }
            x.swimming = 0;
            if (x.special_move_counter > 0) {
                --x.special_move_counter;
                vec2 const to = room_view::middle_of_tile(mv.special_move_destination);
                move_towards(state, c, room, to);
                x.travel_dir = rw::lerp(x.travel_dir, custom::dir_vec(pos(state, c, 0), to), 0.4f);
                if (custom::dist_less(pos(state, c, 0), to, 5.f))
                    x.special_move_counter = 0;
            } else {
                if (!accessible(state, c, room, pos(state, c, 0)) && !accessible(state, c, room, pos(state, c, 1)))
                    x.footing_counter = int16_t(custom::int_clamp(int32_t(x.footing_counter) - 3, 0, 35));
                ivec2 const dest = w.creature_get_destination(c);
                bool const fb = fire(state, c);
                if ((fb && creature::grasped_object(w, c, 0))
                || ((!fb || x.eggs_left > 0) && (tile_of(pos(state, c, 0)) == dest || tile_of(pos(state, c, 1)) == dest) && tracker::threat_utility(tracker::of(state, c)) < 0.5f)) {
                    x.sitting = 1;
                    physics::set_go_through_floors(state, body(state, c), false);
                } else {
                    auto conn = follow_path(state, f, tile_of(pos(state, c, 0)));
                    if (!conn.valid)
                        conn = follow_path(state, f, tile_of(pos(state, c, 1)));
                    if (conn.valid) {
                        run(state, c, room, conn);
                        x.travel_dir = rw::lerp(x.travel_dir, custom::dir_vec(pos(state, c, 0), room_view::middle_of_tile(conn.dest)), 0.4f);
                    } else {
                        physics::set_go_through_floors(state, body(state, c), false);
                    }
                }
            }
            if (fire(state, c) && x.eggs_left <= 0 && !creature::grasped_object(w, c, 0))
                x.sitting = 0;
            ai_update(state, c, room);
            // (no Egg_Bug_Scurry sound at each step)
            auto const head = chunk(state, c, 0);
            if (consious(state, c) && !custom::dist_less(w.body_chunk_get_pos(head), w.body_chunk_get_last_pos(head), 5.f))
                x.run_cycle += x.run_speed / 10.f;
            if (x.sitting) {
                vec2 a{0.f, 0.f};
                ivec2 const t = here(state, c);
                for (auto const dv : custom::eight_directions)
                    if (room.get_tile(t + dv).solid())
                        a -= rw::to_vec2(dv);
                mv.away_from_terrain_dir = rw::lerp(mv.away_from_terrain_dir, rw::normalized(a), 0.1f);
            } else {
                mv.away_from_terrain_dir *= 0.7f;
            }
        }

        // the eggs, or the fire bug's spears, made after the rooms' update
        struct drop_item {
            vec2 pos;
            vec2 vel;
            vec2 rotation;
            float rot_vel;
        };

        // EggBug.DropEggs (no graphics module: from the body, all at once;
        // the fire bug's blast goes off either way)
        void drop_eggs(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            auto& x = pl(state, c);
            bool const fb = fire(state, c);
            if (fb && x.eggs_left <= 0)
                return;
            x.drop_eggs = 0;
            if (fb) {
                vec2 const at = pos(state, c, 0);
                explosion e;
                e.life_time = 5;
                e.rad = 200.f;
                e.force = 10.f;
                e.damage = 0.25f;
                e.stun = 60.f;
                e.min_stun = 0.f;
                e.color = vec3{1.f, 1.f, 1.f};
                explode(state, room, at, e, body(state, c));
                state.play_sound(sound::sound_id::fire_spear_explode, room.id, at);
                state.add_noise(room.id, at, 8000.f, body(state, c));
            }
            int32_t const n = fb ? x.eggs_left : 6;
            std::vector<drop_item> eggs;
            eggs.reserve(size_t(n));
            vec2 const c1 = pos(state, c, 1), v1 = vel(state, c, 1);
            for (int32_t k = 0; k < n; ++k) {
                drop_item it;
                it.pos = c1 + custom::rnv(rng) * 4.f * rng.value();
                float const a = 180.f * mathf::pow(rng.value(), 3.f) * (rng.value() < 0.5f ? -1.f : 1.f);
                it.vel = v1 + custom::deg_to_vec(a) * (fb ? 4.f : 16.f) * mathf::pow(rng.value(), 0.3f);
                it.rotation = custom::rnv(rng);
                it.rot_vel = mathf::lerp(-20.f, 20.f, rng.value());
                eggs.push_back(it);
            }
            x.eggs_left = 0;
            // (no Egg_Bug_Drop_Eggs sound)
            float const hue = x.hue;
            auto const type = fb ? world::item_type::fire_egg : world::item_type::egg_bug_egg;
            state.defer(room.id, [eggs = std::move(eggs), hue, type, rid = room.id](sys::game_state& s) {
                for (auto const& e : eggs) {
                    auto const it = items::create(s, rid, e.pos, type);
                    egg_bug_egg::dropped(s, it, hue, e.vel, e.rotation, e.rot_vel);
                }
            });
        }

        // EggBug.DropSpears: the fire bug's spines, six spears (plain ones:
        // the bug spear's hue and smoke trail aren't ported)
        void drop_spears(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            pl(state, c).drop_spears = 0;
            std::vector<drop_item> spears;
            vec2 const c1 = pos(state, c, 1), v1 = vel(state, c, 1);
            for (int k = 0; k < 6; ++k) {
                drop_item it;
                it.pos = c1 + custom::rnv(rng) * 4.f * rng.value();
                float const a = 180.f * mathf::pow(rng.value(), 3.f) * (rng.value() >= 0.5f ? 1.f : -1.f);
                it.vel = v1 + custom::deg_to_vec(a) * 16.f * mathf::pow(rng.value(), 0.3f);
                it.rotation = custom::rnv(rng);
                it.rot_vel = 0.f;
                spears.push_back(it);
            }
            state.defer(room.id, [spears = std::move(spears), rid = room.id](sys::game_state& s) {
                for (auto const& e : spears) {
                    auto const it = items::create(s, rid, e.pos, world::item_type::spear);
                    s.world.body_chunk_set_vel(items::chunk(s, it), e.vel);
                    s.world.item_set_rotation(it, e.rotation);
                    s.world.item_set_last_rotation(it, e.rotation);
                }
            });
        }

        // Creature.Violence on what it stabs (the push clamped like
        // vanilla's for a slugcat)
        void stab(sys::game_state& state, dcon::creature_id t, uint32_t k, vec2 momentum, float damage) {
            auto& w = state.world;
            if (creature::is_slugcat(w, t)) {
                auto const kc = creature::chunk_of(state, t, k);
                momentum = rw::clamp_magnitude(momentum / w.body_chunk_get_mass(kc), 10.f) * w.body_chunk_get_mass(kc);
            }
            creature::violence_on(state, t, k, momentum, damage, 0.f, creature::damage_type::stab, creature::tmpl::fire_bug);
        }

        // EggBug.CarryObject: the fire bug's prey in its jaws, stabbed with
        // the spines
        void carry_object(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            auto& fs = w.egg_bug_data_get_fire(data(state, c));
            auto const t = grasped(state, c);
            uint32_t const k = creature::grasp_chunk(w, c, 0);
            if (!t || !creature::here(state, t, room.id) || k >= creature::chunk_count_of(state, t)) {
                lose_all_grasps(state, c);
                return;
            }
            if (rng.value() < 0.025f && tracker::dynamic_relationship(state, c, t).type != creature::rel::eats) {
                lose_all_grasps(state, c);
                return;
            }
            auto const main = chunk(state, c, 0);
            vec2 const mp = w.body_chunk_get_pos(main);
            vec2 const hold = mp + custom::dir_vec(pos(state, c, 1), mp) * 1.f;
            auto const gc = creature::chunk_of(state, t, k);
            vec2 const rel = w.body_chunk_get_vel(gc) - w.body_chunk_get_vel(main);
            w.body_chunk_set_vel(gc, w.body_chunk_get_vel(main));
            if (w.creature_get_entering_shortcut(c).x < 0
            && (rw::magnitude(rel) * w.body_chunk_get_mass(gc) > 30.f || !custom::dist_less(hold, w.body_chunk_get_pos(gc), 70.f + w.body_chunk_get_rad(gc)))) {
                lose_all_grasps(state, c);
                return;
            }
            // BodyChunk.MoveFromOutsideMyUpdate
            w.body_chunk_set_pos(gc, hold);
            // PhysicalObject.PushOutOf: its other chunks out of the bug
            uint32_t const n = creature::chunk_count_of(state, t);
            for (uint32_t i = 0; i < 2; ++i) {
                vec2 const bp = pos(state, c, i);
                float const rad = w.body_chunk_get_rad(chunk(state, c, i));
                for (uint32_t j = 0; j < n; ++j) {
                    if (j == k)
                        continue;
                    auto const other = creature::chunk_of(state, t, j);
                    vec2& op = w.body_chunk_get_pos(other);
                    float const r = rad + w.body_chunk_get_rad(other);
                    if (custom::dist_less(op, bp, r)) {
                        float const dist = rw::distance(op, bp);
                        vec2 const dir = custom::dir_vec(bp, op);
                        op += dir * (r - dist);
                        w.body_chunk_get_vel(other) += dir * (r - dist);
                    }
                }
            }
            ++fs.stab_ticker;
            if (fs.stab_ticker >= fs.next_stab) {
                int32_t const s = rng.range(0, 2), e = rng.range(0, 3);
                if (!fs.stab_spine[s][e]) {
                    fs.stab_spine[s][e] = 1;
                    vec2 const dir = custom::dir_vec(mp, w.body_chunk_get_pos(gc));
                    // two light stabs, then half of them deep
                    if (fs.stab_count < 2 || rng.value() < 0.5f)
                        stab(state, t, k, dir * 2.5f, 0.5f);
                    else
                        stab(state, t, k, dir * 10.f, 1.5f);
                    // (no CreatureSpasmers, no Spear_Stick_In_Creature sound)
                    vel(state, c, 0) += custom::rnv(rng) * 5.f;
                    if (state.viewed(room.id)) {
                        auto& gr = w.creature_get_graphics_random(c);
                        vec2 const v0 = vel(state, c, 0);
                        for (int q = 0; q < 8; ++q)
                            particles::spawn(state, room.id, particles::kind::drop, w.body_chunk_get_pos(gc),
                                -v0 * gr.value() * 0.5f + custom::deg_to_vec(360.f * gr.value()) * rw::magnitude(v0) * gr.value() * 0.5f, 1.5f, 0.6f,
                                vec3{0.5f, 0.05f, 0.05f});
                    }
                    fs.next_stab = rng.range(3, 30);
                    fs.stab_ticker = 0;
                    ++fs.stab_count;
                }
            }
            if (creature::live(state, t) && creature::dead(state, t) && ++fs.overdead_ticker > 180) {
                lose_all_grasps(state, c);
                fs.overdead_ticker = 0;
            }
        }

        // Creature.Violence (HealthState, quickDeath)
        void base_violence(sys::game_state& state, dcon::creature_id c, uint32_t k, vec2 momentum, float damage, float stun_bonus) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            auto const& t = tp(state, c);
            auto const hit = chunk(state, c, k < 2 ? k : 0);
            w.body_chunk_get_vel(hit) += rw::clamp_magnitude(momentum / w.body_chunk_get_mass(hit), 10.f);
            float const dmg = damage / t.base_damage_resistance;
            float st = (damage * 30.f + stun_bonus) / t.base_stun_resistance;
            float& health = w.creature_get_health(c);
            st *= 1.5f + mathf::inverse_lerp(0.5f, 0.f, health) * rng.value();
            creature::stun(state, c, int32_t(st));
            health -= dmg;
            if (rng.value() < -health || health < -1.f || (health < 0.f && rng.value() < 0.33f) || dmg >= t.instant_death_damage_limit)
                creature::die(state, c);
        }

        // Explosion.PushAngle
        vec2 push_angle(vec2 a, vec2 b) {
            return rw::slerp(rw::normalized(b - a), vec2{0.f, 1.f}, 0.2f);
        }
    }

    pathing::movement const& movement(sys::game_state const& state, dcon::creature_id c) noexcept {
        return fire(state, c) ? fire_movement : egg_movement;
    }

    bool footing(sys::game_state const& state, dcon::creature_id c) noexcept {
        auto const& x = pl(state, c);
        return x.footing_counter > 20 || x.out_of_water_footing > 0;
    }

    dcon::creature_id grasped(sys::game_state const& state, dcon::creature_id c) noexcept {
        auto const t = creature::grasped(state.world, c, 0);
        return creature::live(state, t) ? t : dcon::creature_id{};
    }

    void explode(sys::game_state& state, room_view const& room, vec2 at, explosion const& e, dcon::physical_object_id source) {
        auto& w = state.world;
        // the objects in reach (gathered first: what the blast does to a
        // creature mustn't change what is walked)
        std::vector<dcon::physical_object_id> objects;
        w.room_for_each_object_location(room.id, [&](dcon::object_location_id l) {
            auto const o = w.object_location_get_object(l);
            if (o != source)
                objects.push_back(o);
        });
        for (int32_t frame = 0; frame <= e.life_time; ++frame) {
            float const num = e.rad * (0.25f + 0.75f * std::sin(mathf::inverse_lerp(0.f, float(e.life_time), float(frame)) * mathf::pi));
            for (auto const o : objects) {
                if (!w.physical_object_is_valid(o))
                    continue;
                auto const cr = creature::of_body(w, o);
                bool const slugcat = cr && creature::is_slugcat(w, cr);
                auto const name = slugcat ? w.player_get_stats(creature::player_of(state, cr)).name : player::slugcat_name::white;
                bool const artificer = slugcat && name == player::slugcat_name::artificer;
                float num2 = 0.f;
                int32_t num4 = -1;
                w.physical_object_for_each_chunk_ownership_as_owner(o, [&](dcon::chunk_ownership_id co) {
                    auto const k = w.chunk_ownership_get_chunk(co);
                    vec2& kp = w.body_chunk_get_pos(k);
                    float const d = rw::distance(at, kp);
                    if (d >= num)
                        return;
                    float num6 = mathf::inverse_lerp(num, num * 0.25f, d);
                    if (!room.visual_contact(at, kp))
                        num6 -= 0.5f;
                    if (num6 <= 0.f)
                        return;
                    float const force = artificer ? e.force * 0.25f : e.force;
                    vec2 const push = push_angle(at, kp) * (force / w.body_chunk_get_mass(k)) * num6;
                    w.body_chunk_get_vel(k) += push;
                    kp += push * 0.1f;
                    if (num6 > num2) {
                        num2 = num6;
                        num4 = int32_t(w.body_chunk_get_index(k));
                    }
                });
                if (cr && cr == e.kill_tag)
                    num2 *= e.kill_tag_factor;
                if (num4 < 0 || !cr || !w.creature_is_valid(cr))
                    continue;
                float dmg = e.damage;
                if (artificer)
                    dmg *= 0.2f;
                if (slugcat && name == player::slugcat_name::saint)
                    dmg *= 4.f;
                // (a HealthState's damage spread over the frames)
                creature::violence(state, cr, creature::hit_part{num4}, vec2{0.f, 0.f}, num2 * dmg / (slugcat ? 1.f : float(e.life_time)), num2 * e.stun,
                    creature::damage_type::explosion);
                if (e.min_stun > 0.f && !artificer)
                    creature::stun(state, cr, int32_t(e.min_stun * mathf::inverse_lerp(0.f, 0.5f, num2)));
            }
        }
        // the look of it (rolled from the room's cosmetic generator, only
        // where a camera looks): ExplosionLight, Sparks, smoke
        if (!state.viewed(room.id))
            return;
        auto& r = state.runtime(room.id).cosmetic;
        particles::spawn(state, room.id, particles::kind::flash, at, vec2{0.f, 0.f}, e.rad * 0.5f, 0.3f, e.color, 1.f);
        particles::spawn(state, room.id, particles::kind::flash, at, vec2{0.f, 0.f}, e.rad * 0.25f, 0.15f, vec3{1.f, 1.f, 1.f}, 1.f);
        for (int i = 0; i < 20; ++i) {
            vec2 const a = custom::rnv(r);
            particles::spawn(state, room.id, particles::kind::spark, at + a * r.value() * 40.f, a * mathf::lerp(4.f, 30.f, r.value()), 1.8f, 0.4f,
                e.color + (vec3{1.f, 1.f, 1.f} - e.color) * r.value());
        }
        for (int i = 0; i < 14; ++i)
            particles::spawn(state, room.id, particles::kind::dust, at, custom::rnv(r) * 5.f * r.value(), 10.f, 1.2f, vec3{0.3f, 0.28f, 0.27f}, 0.5f);
        if (room.water && room.point_submerged(at))
            particles::bubbles(state, room.id, at, 30, e.rad * 0.3f);
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 at, variant v) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::egg_bug);
        auto const d = data(state, c);
        rw::rng rng{state.random.next() | 1u};
        int32_t seed = int32_t(rng.next() & 0x7FFFFFFFu);
        if (state.spawn_seed >= 0)
            seed = state.spawn_seed;
        bool const fb = v == variant::fire_bug;
        physics::object_desc desc;
        desc.air_friction = 0.999f;
        desc.gravity = 0.9f;
        desc.bounce = 0.1f;
        desc.surface_friction = 0.4f;
        desc.water_friction = 0.96f;
        desc.buoyancy = 0.95f;
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
        // two chunks: the head (5, a third of 0.4) and the body (8, two
        // thirds), 12 apart (the fire bug 24)
        constexpr float mass = 0.4f;
        auto const c0 = physics::add_chunk(state, o, at, 5.f, mass * 0.33333334f);
        auto const c1 = physics::add_chunk(state, o, at + vec2{0.f, -1.f}, 8.f, mass * 0.6666667f);
        physics::add_connection(state, o, c0, c1, fb ? 24.f : 12.f, physics::connection_type::normal, 1.f, 0.5f);
        w.try_create_creature_body(c, o);
        auto& x = pl(state, c);
        x.variant = v;
        // the hue, rolled from the ID's seed
        {
            rw::unity_random r{seed};
            x.hue = mathf::lerp(fb ? 0.35f : -0.15f, fb ? 0.6f : 0.1f, custom::clamped_random_variation(0.5f, 0.5f, 2.f, r));
        }
        x.run_speed = 0.f;
        x.run_cycle = 0.f;
        x.fear = 0.f;
        x.antenna_attention = 0.f;
        x.travel_dir = vec2{0.f, 0.f};
        x.footing_counter = 0;
        x.out_of_water_footing = 0;
        x.special_move_counter = 0;
        x.no_jumps = 0;
        x.shake = 0;
        x.time_without_eggs = 0;
        x.eggs_left = 6;
        x.drop_eggs = 1;
        x.drop_spears = fb ? 1 : 0;
        x.sitting = 0;
        x.swimming = 0;
        w.creature_set_random(c, rng);
        w.creature_set_graphics_random(c, rw::rng{(rng.state ^ 0xE66B06u) | 1u});
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_set_grasps(c, creature::grasp_pair{});
        w.creature_set_behavior(c, uint8_t(behavior::idle));
        w.creature_set_current_utility(c, 0.f);
        w.creature_set_destination(c, tile_of(at));
        w.egg_bug_data_set_seed(d, seed);
        // AbstractCreature.personality.dominance
        w.egg_bug_data_set_dominance(d, w.creature_get_random(c).value());
        w.egg_bug_data_set_move(d, move_state{});
        ai_state ai;
        ai.temp_idle_pos = tile_of(at);
        w.egg_bug_data_set_ai(d, ai);
        fire_state fs;
        if (fb)
            fs.next_stab = w.creature_get_random(c).range(15, 60);
        w.egg_bug_data_set_fire(d, fs);
        w.egg_bug_data_set_graphics(d, graphics_state{});
        tracker::clear(state, c);
        egg_bug_graphics::reset_positions(state, c);
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        lose_all_grasps(state, c);
        creature::release_target(state, c);
        creature::erase(state, c);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto& rng = w.creature_get_random(c);
        auto const o = body(state, c);
        auto& x = pl(state, c);
        bool const fb = fire(state, c);
        // EggBug.Update's HealthState: hurt, it may die or faint, else heals
        {
            float& health = w.creature_get_health(c);
            if (!w.creature_get_dead(c) && health < 0.f && rng.value() < -health && rng.value() < 0.025f)
                creature::die(state, c);
            if (!w.creature_get_dead(c) && rng.value() * 0.7f > health && rng.value() < 0.125f)
                creature::stun(state, c, rng.range(1, rng.range(1, 27 - custom::int_clamp(int32_t(20.f * health), 0, 10))));
            if (!w.creature_get_dead(c) && health > 0.f && health < 1.f && rng.value() < 0.02f)
                health = mathf::min(1.f, health + 1.f / mathf::lerp(140.f, 50.f, health));
        }
        if (x.out_of_water_footing > 0)
            --x.out_of_water_footing;
        if (x.no_jumps > 0 && footing(state, c))
            --x.no_jumps;
        // (no LickedByPlayer: Saint's tongue doesn't set the fire bug off)
        if (x.drop_eggs && w.creature_get_dead(c))
            drop_eggs(state, c, room);
        if (fb) {
            if (x.eggs_left == 0 && x.time_without_eggs < 30000)
                ++x.time_without_eggs;
            if (x.drop_spears && w.creature_get_dead(c))
                drop_spears(state, c, room);
            // FireSmoke: it bleeds smoke once it lost eggs (cosmetic)
            if (x.eggs_left < 6 && !w.creature_get_dead(c) && state.viewed(room.id)) {
                auto& gr = w.creature_get_graphics_random(c);
                if (gr.value() < 0.25f)
                    particles::spawn(state, room.id, particles::kind::dust, pos(state, c, 0), custom::rnv(gr) * 0.8f, 6.f, 1.f,
                        custom::hsl2rgb(custom::decimal(x.hue + hue_off), 1.f, 0.5f), 0.4f);
            }
        }
        // shivering, stunned
        if (!w.creature_get_dead(c) && w.creature_get_stun(c) > rng.range(20, 80)) {
            x.shake = int16_t(std::max(int32_t(x.shake), 10));
            for (uint32_t i = 0; i < 2; ++i) {
                ivec2 const cp = w.body_chunk_get_contact_point(chunk(state, c, i));
                if (cp.x != 0 || cp.y != 0)
                    vel(state, c, i) += (custom::rnv(rng) - rw::to_vec2(cp)) * rng.value() * 3.f;
            }
        }
        if (x.shake > 0) {
            --x.shake;
            if (!w.creature_get_dead(c))
                for (uint32_t i = 0; i < 2; ++i)
                    if (accessible(state, c, room, pos(state, c, i)))
                        vel(state, c, i) += custom::rnv(rng) * 2.f;
        }
        // Creature.Update + PhysicalObject.Update (no TerrainImpact squirts)
        creature::stun_countdown(state, c);
        physics::update_object(state, room, o, nullptr, nullptr);
        if (!w.creature_is_valid(c))
            return;

        x.sitting = 0;
        x.antenna_attention = mathf::max(0.f, x.antenna_attention - 0.016666668f);
        if (creature::held(state, c)) {
            // wriggling in a grip
            if (!w.creature_get_dead(c)) {
                for (uint32_t i = 0; i < 2; ++i)
                    vel(state, c, i) += custom::rnv(rng) * 2.f;
                ai_update(state, c, room);
            }
            x.footing_counter = 0;
            x.travel_dir = vec2{0.f, 0.f};
        }
        if (consious(state, c)) {
            if (x.footing_counter < 30000)
                ++x.footing_counter;
            act(state, c, room);
        } else {
            x.footing_counter = 0;
        }
        if (fb) {
            if (creature::grasped_object(w, c, 0))
                carry_object(state, c, room);
            // the spines where the eggs were: out, and back in after a stab
            auto& fs = w.egg_bug_data_get_fire(data(state, c));
            for (int s = 0; s < 2; ++s)
                for (int e = 0; e < 3; ++e) {
                    if (x.eggs_left <= s * 3 + e) {
                        float& ext = fs.spine_extensions[s][e];
                        if (fs.stab_spine[s][e]) {
                            ext = mathf::lerp(ext, 0.f, 0.5f);
                            if (ext <= 0.2f)
                                fs.stab_spine[s][e] = 0;
                        } else {
                            ext = mathf::lerp(ext, 1.f, 0.25f);
                        }
                    }
                    if (w.creature_get_dead(c))
                        fs.spine_extensions[s][e] = 0.f;
                }
        }
        // standing: gravity cancelled (PhysicalObject.gravity, not the room's)
        if (footing(state, c)) {
            float const g = w.physical_object_get_g(o);
            for (uint32_t i = 0; i < 2; ++i) {
                vel(state, c, i) *= 0.8f;
                vel(state, c, i).y += g;
            }
        }
        x.travel_dir *= x.sitting ? 0.5f : 0.995f;
        // scrambling in the air, fleeing
        if (consious(state, c) && !footing(state, c) && behavior(w.creature_get_behavior(c)) == behavior::flee)
            for (uint32_t i = 0; i < 2; ++i)
                if (accessible(state, c, room, pos(state, c, i)))
                    vel(state, c, i) += custom::deg_to_vec(rng.value() * 360.f) * rng.value() * 5.f;
    }

    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) {
        // what it holds is kept out of it by CarryObject
        return !creature::holds(state.world, c, t);
    }

    void collide(sys::game_state&, dcon::creature_id, room_view const&, uint32_t, dcon::creature_id, uint32_t) {
        // (Creature.Collide: nothing of its own with other kinds)
    }

    void collide_kin(sys::game_state& state, dcon::creature_id c, uint32_t i, dcon::creature_id other, uint32_t j) {
        if (fire(state, c) != fire(state, other))
            return;
        // EggBug.Collide of the upper one: it climbs over
        auto& w = state.world;
        auto const a = chunk(state, c, i), b = chunk(state, other, j);
        bool const above = w.body_chunk_get_pos(a).y > w.body_chunk_get_pos(b).y;
        w.body_chunk_get_vel(above ? a : b).y += 2.f;
        w.body_chunk_get_vel(above ? b : a).y -= 2.f;
    }

    void touched_kin(sys::game_state& state, dcon::creature_id c, dcon::creature_id other) {
        collide_with_kin(state, c, other);
        collide_with_kin(state, other, c);
    }

    void violence(sys::game_state& state, dcon::creature_id c, uint32_t k, vec2 momentum, float damage, float stun_bonus, creature::damage_type type) {
        auto& x = pl(state, c);
        bool const fb = fire(state, c);
        // a fire bug with its eggs shrugs the hit off
        if (!fb || x.time_without_eggs >= 10)
            base_violence(state, c, k, momentum, damage, stun_bonus);
        if (!state.world.creature_is_valid(c))
            return;
        if (damage > 0.05f || (fb && (type == creature::damage_type::blunt || type == creature::damage_type::stab))) {
            squirt(state, c, mathf::inverse_lerp(0.2f, 1.f, damage));
            if (fb && x.drop_eggs && state.world.creature_get_away(c) == 0)
                drop_eggs(state, c, world::get_room_view(state, creature::room_of(state, c)));
        }
    }

    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t) {
        if (creature::grasped(state.world, c, 0) == t)
            lose_all_grasps(state, c);
    }

    void died(sys::game_state& state, dcon::creature_id c) {
        // (Creature.Die let go of everything already)
        auto& w = state.world;
        auto& x = pl(state, c);
        if (w.creature_get_away(c) != 0)
            return; // EggBug.Update drops them once it is out in a room
        auto const room = world::get_room_view(state, creature::room_of(state, c));
        if (x.drop_eggs)
            drop_eggs(state, c, room);
        if (fire(state, c) && x.drop_spears)
            drop_spears(state, c, room);
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
            w.body_chunk_set_vel(k, a * 2.f);
        }
        // EggBugAI.NewRoom: what it walked to is in the other room
        auto& x = pl(state, c);
        x.special_move_counter = 0;
        auto& ai = w.egg_bug_data_get_ai(data(state, c));
        ai.walk_with_bug = std::nullopt;
        ai.temp_idle_pos = t;
        ai.idle_pos_counter = 0;
        w.egg_bug_data_get_move(data(state, c)).last_followed = movement_connection{};
    }

    void flying_weapon(sys::game_state& state, dcon::creature_id c, vec2 at, vec2 v) {
        if (state.world.creature_get_away(c) != 0)
            return;
        auto const& x = pl(state, c);
        vec2 const head = pos(state, c, 0);
        // (no Remix: only a weapon flying rightwards past it)
        if (!(v.x > 0.f && head.x > at.x))
            return;
        if (!custom::dist_less(head, at + vec2{mathf::sign(v.x) * 300.f, 0.f}, 300.f))
            return;
        auto const room = world::get_room_view(state, creature::room_of(state, c));
        if (at.y <= head.y + 50.f && (!fire(state, c) || x.eggs_left == 0)) {
            try_jump(state, c, room, at);
            return;
        }
        squirt(state, c, 1.f);
    }

    std::string describe(sys::game_state const& state, dcon::creature_id c) {
        static constexpr char const* names[] = {"idle", "flee", "hunt", "escape rain", "return prey", "kill"};
        auto const& w = state.world;
        vec2 const p = pos(state, c, 0);
        uint32_t const b = w.creature_get_behavior(c);
        auto const& x = pl(state, c);
        std::string s = std::string(fire(state, c) ? "fire bug" : "egg bug") + " at " + std::to_string(int(p.x)) + "," + std::to_string(int(p.y)) + " behavior "
            + (b < 6 ? names[b] : "?") + " eggs " + std::to_string(int(x.eggs_left));
        if (creature::grasped_object(w, c, 0))
            s += " holding prey";
        if (x.sitting)
            s += " sitting";
        return s;
    }

    bool spawn_spot(room_view const& room, ivec2 tile) {
        return room.inside(tile.x, tile.y) && !room.get_tile(tile).solid() && room.get_tile(tile + ivec2{0, -1}).solid()
            && pathing::accessible(room, tile, egg_movement) && room.get_tile(tile).water == 0;
    }
}
