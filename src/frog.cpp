// Watcher/Frog.cs, FrogAI.cs and FrogState.cs: the frog. See frog.hpp.

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>

#include "frog.hpp"
#include "dens.hpp"
#include "frog_graphics.hpp"
#include "frog_types.hpp"
#include "lizard.hpp"
#include "migrate.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "rooms.hpp"
#include "tracker.hpp"

namespace frog {
    using creature::relationship;
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // CreatureTemplate (StaticWorld.InitFrog): visualRadius; the
        // instantDeathDamageLimit (no HealthState: the poison's measure)
        constexpr float visual_radius = 2500.f;
        constexpr float instant_death_damage_limit = 1.f;
        // Frog.CreatureParams: totalMass, sizeMultiplier and the speed
        // factors (fleeSpeedFactor 1.1, the rest 1)
        constexpr float total_mass = 0.1f;
        constexpr float size_multiplier = 1.f;
        constexpr float flee_speed_factor = 1.1f;
        constexpr float float_max = std::numeric_limits<float>::max();

        creature::frog_payload& pl(sys::game_state& state, dcon::creature_id c) { return state.world.creature_get_payload(c).frog; }
        creature::frog_payload const& pl(sys::game_state const& state, dcon::creature_id c) { return state.world.creature_get_payload(c).frog; }
        dcon::frog_data_id data(sys::game_state const& state, dcon::creature_id c) { return creature::frog_data(state.world, c); }
        move_state& mv(sys::game_state& state, dcon::creature_id c) { return state.world.frog_data_get_move(data(state, c)); }
        move_state const& mv(sys::game_state const& state, dcon::creature_id c) { return state.world.frog_data_get_move(data(state, c)); }
        ai_state& ai(sys::game_state& state, dcon::creature_id c) { return state.world.frog_data_get_ai(data(state, c)); }
        vec2& pos(sys::game_state& state, dcon::creature_id c, uint32_t i) { return state.world.body_chunk_get_pos(chunk(state, c, i)); }
        vec2 pos(sys::game_state const& state, dcon::creature_id c, uint32_t i) { return state.world.body_chunk_get_pos(chunk(state, c, i)); }
        vec2& vel(sys::game_state& state, dcon::creature_id c, uint32_t i) { return state.world.body_chunk_get_vel(chunk(state, c, i)); }
        ivec2 tile_of(vec2 p) { return room_view::get_tile_position(p); }
        // abstractCreature.pos: the main chunk's tile
        ivec2 here_tile(sys::game_state const& state, dcon::creature_id c) { return tile_of(pos(state, c, 0)); }

        // AImap.TileAccessibleToCreature
        bool accessible(room_view const& room, ivec2 t) { return room.inside(t.x, t.y) && pathing::accessible(room, t, movement); }
        bool accessible(room_view const& room, vec2 p) { return accessible(room, tile_of(p)); }
        // Frog.InAccessibleTiles: the chunk's tile, or the one over it when
        // that is a slope (AItile.Accessibility.CurvedFloor)
        bool in_accessible_tiles(sys::game_state const& state, dcon::creature_id c, room_view const& room, uint32_t k) {
            vec2 const p = pos(state, c, k);
            vec2 const up = p + vec2{0.f, 20.f};
            return accessible(room, p) || (accessible(room, up) && room.get_tile(up).t == world::terrain::slope);
        }
        bool footing(sys::game_state const& state, dcon::creature_id c) { return pl(state, c).footing_counter > 20; }

        // another creature's BodyChunk.Rotation: from the chunk it is
        // connected to (its neighbour in the chunk order) to it
        vec2 chunk_rotation(sys::game_state const& state, dcon::creature_id t, uint32_t k) {
            uint32_t const n = creature::chunk_count_of(state, t);
            if (n < 2 || k >= n)
                return vec2{0.f, 1.f};
            uint32_t const o = k == 0 ? 1 : k - 1;
            auto const& w = state.world;
            return rw::normalized(w.body_chunk_get_pos(creature::chunk_of(state, t, k)) - w.body_chunk_get_pos(creature::chunk_of(state, t, o)));
        }

        // Frog.setCollisions (CollideWithObjects and canBeHitByWeapons are
        // `collides` and Violence's early out)
        void set_collisions(sys::game_state& state, dcon::creature_id c, bool on) {
            auto& w = state.world;
            for (uint32_t i = 0; i < 2; ++i) {
                auto const k = chunk(state, c, i);
                w.body_chunk_set_collide_with_terrain(k, on ? 1 : 0);
                w.body_chunk_set_collide_with_slopes(k, on ? 1 : 0);
            }
        }

        // Frog.CheckIfGrabbedByPlayer
        bool grabbed_by_player(sys::game_state const& state, dcon::creature_id c) {
            if (!creature::held(state, c))
                return false;
            bool found = false;
            creature::for_each_holder(state, creature::room_of(state, c), c, [&](dcon::creature_id h, uint32_t) {
                if (creature::is_slugcat(state.world, h))
                    found = true;
            });
            return found;
        }

        // Creature.InjectPoison (IHaveInjectedPoisonColor: the colour moves
        // towards the poison's in HSL)
        void inject_poison(sys::game_state& state, dcon::creature_id c, float amount, vec3 color) {
            auto& m = mv(state, c);
            float lrp = amount * 1.5f;
            if (state.world.creature_get_dead(c))
                lrp = 0.f;
            if (m.injected_poison <= 0.f)
                lrp = 1.f;
            vec3 const a = custom::rgb2hsl(m.poison_color);
            vec3 const b = custom::rgb2hsl(color);
            // HSLColor.Lerp (the hue the plain way)
            vec3 const h{mathf::lerp(a.x, b.x, lrp), mathf::lerp(a.y, b.y, lrp), mathf::lerp(a.z, b.z, lrp)};
            m.poison_color = custom::hsl2rgb(h.x, h.y, h.z);
            m.injected_poison += amount;
        }

        //
        // Latching on
        //
        void jump(sys::game_state& state, dcon::creature_id c, vec2 to, bool ignore_no_jumps);

        // Frog.SuckFood: true when it is done with a slugcat (KickUntilDetached)
        bool suck_food(sys::game_state& state, dcon::creature_id c, room_view const& room, rw::rng& rng) {
            auto& w = state.world;
            auto& x = pl(state, c);
            auto const t = attached_to(state, c);
            if (!t || w.creature_get_entering_shortcut(t).x >= 0 || w.creature_get_away(t) != 0)
                return false;
            // (no Frog_Drain sound)
            x.feed_size = uint8_t(std::min(int32_t(x.feed_size) + 1, max_feeds));
            vec2 const momentum = vel(state, c, 0) * w.body_chunk_get_mass(chunk(state, c, 0));
            if (auto const pp = creature::player_of(state, t)) {
                if (w.room_get_is_shelter(room.id)) {
                    x.feed_cd = 600;
                    return false;
                }
                x.feed_cd = int16_t(rng.range(2400, 4800));
                int32_t& food = w.player_get_food(pp);
                if (food - feed_take < 0 || int32_t(x.feed_size) == max_feeds) {
                    // the last of it: a hard bite, the slugcat starved and
                    // spent (no Malnourished state here; no Blink)
                    creature::violence_on(state, t, 0, momentum, 0.02f, 200.f, creature::damage_type::bite, creature::tmpl::frog);
                    w.player_set_exhausted(pp, 1);
                    w.player_set_lungs_exhausted(pp, 1);
                    x.kick_until_detached = 1;
                    return true;
                }
                creature::violence_on(state, t, 0, momentum, 0.02f, 50.f, creature::damage_type::bite, creature::tmpl::frog);
                food = std::max(0, food - feed_take);
                mv(state, c).current_target_food = food;
            } else {
                w.body_chunk_set_rad(chunk(state, c, 1),
                    mathf::lerp(3.f * size_multiplier, 3.f * size_multiplier * blood_bank_bloat(state, c), float(x.feed_size) / float(max_feeds)));
                x.feed_cd = int16_t(mathf::clamp(1600.f - creature::total_mass(state, t) * 100.f, 650.f, 1600.f));
                creature::violence_on(state, t, creature::main_chunk_of(state, t), momentum, 0.02f, 120.f, creature::damage_type::bite, creature::tmpl::frog);
            }
            return false;
        }

        // Frog.Attach to `t`'s chunk `k`
        void attach(sys::game_state& state, dcon::creature_id c, room_view const& room, dcon::creature_id t, uint32_t k, bool suck, rw::rng& rng) {
            auto& w = state.world;
            auto& x = pl(state, c);
            auto& m = mv(state, c);
            if (!t || t == c || m.immune_to_latch == t || !creature::here(state, t, room.id))
                return;
            if (creature::held(state, t))
                return;
            // (Malnourished slugcats aren't told apart: there is no such state)
            x.throw_latch = 0;
            x.throw_latch_grace = 0;
            set_collisions(state, c, false);
            x.time_attached = 0;
            // (no Frog_Attach sound)
            if (!creature::grasped_object(w, c, 0))
                creature::grab(state, c, 0, t, 0);
            if (k >= creature::chunk_count_of(state, t))
                k = 0;
            vec2 const kp = w.body_chunk_get_pos(creature::chunk_of(state, t, k));
            vec2 const lhs = custom::perpendicular_vector(chunk_rotation(state, t, k));
            vec2 const rhs = kp - w.body_chunk_get_last_last_pos(chunk(state, c, 0));
            x.attach_side = rw::dot(lhs, rhs) > 0.f ? -1 : 1;
            // MakeUmbilical (the graphics module makes it)
            x.make_new_umbilical = 1;
            if (!suck)
                return;
            suck_food(state, c, room, rng);
            if (!w.creature_is_valid(t) || !creature::live(state, t))
                return;
            vec2 const momentum = vel(state, c, 0) * w.body_chunk_get_mass(chunk(state, c, 0));
            creature::violence_on(state, t, k, momentum, 0.02f, 50.f, creature::damage_type::bite, creature::tmpl::frog);
        }

        // Frog.Detach
        void detach(sys::game_state& state, dcon::creature_id c, room_view const& room, bool suck_food_on_exit, rw::rng& rng) {
            auto& w = state.world;
            auto& x = pl(state, c);
            auto const t = attached_to(state, c);
            if (!creature::grasped_object(w, c, 0)) {
                set_collisions(state, c, true);
                return;
            }
            if (suck_food_on_exit && t)
                suck_food(state, c, room, rng);
            x.time_attached = 0;
            if (t)
                state.play_sound(creature::is_slugcat(w, t) ? sound::sound_id::leech_detatch_player : sound::sound_id::leech_detatch_npc, room.id, pos(state, c, 0));
            creature::release_grasp(state, c, 0);
            set_collisions(state, c, true);
        }

        // Frog.Attached: hanging on to its host, sucking now and then
        void attached(sys::game_state& state, dcon::creature_id c, room_view const& room, rw::rng& rng) {
            auto& w = state.world;
            auto& x = pl(state, c);
            auto const t = attached_to(state, c);
            if (!t || !creature::here(state, t, room.id)) {
                // its host left the room (into a pipe, a den): it lets go
                detach(state, c, room, false, rng);
                return;
            }
            if (x.feed_cd > 0)
                --x.feed_cd;
            else if (suck_food(state, c, room, rng))
                return;
            if (x.time_attached < 30000)
                ++x.time_attached;
            float const struggle = attached_to(state, c) ? 1.f - dig_in(state, c) : 0.f;
            if (struggle > 0.f && rng.value() < 0.1f)
                x.struggle_dir = mathf::lerp(-1.f, 1.f, rng.value());
            if (w.creature_get_entering_shortcut(t).x >= 0)
                x.make_new_umbilical = 1;
            uint32_t k = creature::grasp_chunk(w, c, 0);
            if (k >= creature::chunk_count_of(state, t))
                k = 0;
            auto const kc = creature::chunk_of(state, t, k);
            vec2 const rot = chunk_rotation(state, t, k);
            vec2 const side = custom::perpendicular_vector(rot) * float(x.attach_side);
            // (the slugcat's chunk, never its graphics' drawPositions: the
            // simulation doesn't read the graphics)
            vec2 v2 = w.body_chunk_get_pos(kc) + side * (w.body_chunk_get_rad(kc) - dig_in(state, c) * 5.f);
            auto const m0 = chunk(state, c, 0);
            vec2 const a = custom::dir_vec(pos(state, c, 0), v2);
            float const d = rw::distance(pos(state, c, 0), v2);
            float const num = w.body_chunk_get_mass(m0) / (w.body_chunk_get_mass(m0) + w.body_chunk_get_mass(kc));
            vel(state, c, 0) += a * d * (1.f - num);
            pos(state, c, 0) += a * d * (1.f - num);
            // WeightedPush(1, 0, side, 8)
            {
                float const m1 = w.body_chunk_get_mass(chunk(state, c, 1));
                float const n = w.body_chunk_get_mass(m0) / (m1 + w.body_chunk_get_mass(m0));
                vel(state, c, 1) += side * 8.f * n;
                vel(state, c, 0) -= side * 8.f * (1.f - n);
            }
            vec2 const host_vel = w.body_chunk_get_vel(kc);
            for (uint32_t i = 0; i < 2; ++i)
                vel(state, c, i) = rw::lerp(vel(state, c, i), host_vel * 0.5f, 0.4f);
            vel(state, c, 1) += rot * x.struggle_dir * struggle * 2.f;
            vel(state, c, 1) *= 0.7f;
            if (creature::is_slugcat(w, t) && w.creature_get_stun(t) >= 120 && !x.kick_until_detached) {
                detach(state, c, room, creature::dead(state, t), rng);
                return;
            }
            if (x.kick_until_detached && consious(state, t)) {
                detach(state, c, room, false, rng);
                jump(state, c, pos(state, c, 0) + vec2{float(rng.range(-5, 5)), 0.f}, true);
                return;
            }
            if (x.kick_until_detached && w.creature_get_dead(c)) {
                if (++x.kick_until_dead > 240) {
                    x.kick_until_dead = 0;
                    detach(state, c, room, true, rng);
                    jump(state, c, pos(state, c, 0) + vec2{float(rng.range(-5, 5)), 0.f}, true);
                }
            }
        }

        // Custom.CirclesCollisionTime: when along the move (0..1) a circle
        // moving from `p` by `d` first touches the one at `q`, -1 never
        float circles_collision_time(vec2 p, vec2 q, vec2 d, float r1, float r2) {
            vec2 const f = p - q;
            float const r = r1 + r2;
            float const a = rw::dot(d, d);
            float const b = 2.f * rw::dot(f, d);
            float const cc = rw::dot(f, f) - r * r;
            if (a <= 0.f)
                return -1.f;
            float const disc = b * b - 4.f * a * cc;
            if (disc < 0.f)
                return -1.f;
            return (-b - std::sqrt(disc)) / (2.f * a);
        }

        // SharedPhysics.TraceProjectileAgainstBodyChunks for the frog (its
        // HitThisChunk: the first frame only what lies ahead of the throw):
        // the creatures of collision layer 1 (not the leeches, overseers and
        // frogs of layer 0), not its thrower nor what the thrower holds
        bool trace(sys::game_state const& state, dcon::creature_id c, room_view const& room, vec2 from, vec2 to, float rad, dcon::creature_id& hit, uint32_t& hit_chunk) {
            auto const& w = state.world;
            auto const& m = mv(state, c);
            float best = float_max;
            vec2 const d = to - from;
            vec2 const first_last = w.body_chunk_get_last_pos(chunk(state, c, 0));
            vec2 const throw_dir = rw::normalized(rw::to_vec2(m.throw_dir));
            rooms::for_each_creature(state, room.id, [&](dcon::creature_id t) {
                if (t == c || t == m.thrown_by || !creature::here(state, t, room.id))
                    return;
                auto const tt = w.creature_get_type(t);
                if (tt == creature::type::frog || tt == creature::type::leech || tt == creature::type::overseer || tt == creature::type::garbage_worm)
                    return;
                if (m.thrown_by && w.creature_is_valid(m.thrown_by) && creature::holds(w, m.thrown_by, t))
                    return;
                uint32_t const n = creature::chunk_count_of(state, t);
                for (uint32_t i = 0; i < n; ++i) {
                    auto const k = creature::chunk_of(state, t, i);
                    vec2 const kp = w.body_chunk_get_pos(k);
                    if (m.has_first_frame && rw::dot(rw::normalized(kp - first_last), throw_dir) <= 0.3f)
                        continue;
                    float const f = circles_collision_time(from, kp, d, rad, w.body_chunk_get_rad(k));
                    if (f > 0.f && f < 1.f && f < best) {
                        best = f;
                        hit = t;
                        hit_chunk = i;
                    }
                }
            });
            return best < float_max;
        }

        // Frog.ThrowUpdate (+ HitSomething): flying, it latches on to what
        // it hits
        void throw_update(sys::game_state& state, dcon::creature_id c, room_view const& room, rw::rng& rng) {
            auto& w = state.world;
            auto& m = mv(state, c);
            auto const k1 = chunk(state, c, 1);
            vec2 const p1 = w.body_chunk_get_pos(k1);
            vec2 const to = p1 + w.body_chunk_get_vel(k1);
            vec2 const from = m.has_first_frame ? m.first_frame_trace_from : p1;
            dcon::creature_id hit;
            uint32_t hk = 0;
            bool const any = trace(state, c, room, from, to, w.body_chunk_get_rad(k1) + 7.5f, hit, hk);
            m.has_first_frame = 0;
            if (!any)
                return;
            if (!blood_bank(state, c) && !attached_to(state, c))
                attach(state, c, room, hit, hk, true, rng);
            // (no SocialEventRecognizer)
            mv(state, c).thrown_by = dcon::creature_id{};
        }

        // Frog.Jump towards `to`
        void jump(sys::game_state& state, dcon::creature_id c, vec2 to, bool ignore_no_jumps) {
            auto& x = pl(state, c);
            if (x.no_jumps > 0 && !ignore_no_jumps)
                return;
            x.kick_until_detached = 0;
            vec2 v = custom::dir_vec(pos(state, c, 0), to);
            // (vanilla maps the target's y, a room position: all but the
            // lowest rows give 1.2)
            float num = custom::lerp_map(to.y, -1.f, 1.f, 0.7f, 1.2f, 1.1f);
            if (blood_bank(state, c))
                num *= 0.65f;
            v = vec2{v.x * 1.3f, v.y + 1.25f};
            x.footing_counter = 0;
            vel(state, c, 0) *= 0.5f;
            vel(state, c, 1) -= v * 0.5f;
            vel(state, c, 0) += v * 8.f * num;
            // (no Frog_Jump sound)
            x.jumping = 1;
            x.jump_stun = int16_t(blood_bank(state, c) ? 15 : 30);
            x.sitting = 0.f;
            x.no_jumps = 75;
        }

        //
        // FrogAI
        //
        // Tracker(10, 5, -1, 0.5, 5, 5, 10), PreyTracker(5, 2, 3, 70, 0.5),
        // ThreatTracker(3)
        tracker::params const& tracker_params() {
            static tracker::params const p = [] {
                tracker::params q;
                q.see_around_corners = 10;
                q.frames_to_remember = -1;
                q.ghost_dismissal_range = 10;
                q.prey_max = 5;
                q.persistance_bias = 2.f;
                q.sure_to_get = 3.f;
                q.sure_to_lose = 70.f;
                q.threat_max = 3;
                return q;
            }();
            return p;
        }
        struct hook_ctx {
            sys::game_state& state;
            dcon::creature_id c;
            room_view const& room;
            // the field to where it is (PathFinder.CoordinateReachableAndGetbackable,
            // judged by the way back), built on the first question
            std::optional<pathing::field_id> from_here;
        };
        bool reachable(hook_ctx& x, ivec2 t) {
            if (!accessible(x.room, t))
                return false;
            if (!x.from_here)
                x.from_here = pathing::field_to(x.state, x.room.id, here_tile(x.state, x.c), movement);
            return pathing::distance(x.state, *x.from_here, t) != pathing::unreachable;
        }
        // ArtificialIntelligence.VisualContact (waterVision 0: nothing under
        // the surface)
        bool hook_sees_point(void* u, vec2 p) {
            auto& x = *static_cast<hook_ctx*>(u);
            vec2 const eye = pos(x.state, x.c, 0);
            if (!custom::dist_less(eye, p, visual_radius))
                return false;
            if (x.room.water && x.room.point_submerged(p))
                return false;
            return x.room.visual_contact(eye, p);
        }
        bool hook_sees(void* u, dcon::body_chunk_id k) {
            auto& x = *static_cast<hook_ctx*>(u);
            return hook_sees_point(u, x.state.world.body_chunk_get_pos(k));
        }
        // FrogAI.UpdateDynamicRelationship: a blood bank fears slugcats,
        // scavengers and vultures (the plain templates); the static one else
        relationship hook_relate(void* u, creature::tracked const& rc) {
            auto& x = *static_cast<hook_ctx*>(u);
            auto& r = const_cast<creature::tracked&>(rc);
            if (r.visual && creature::live(x.state, r.who))
                r.state = creature::dead(x.state, r.who) ? 1 : 0;
            if (blood_bank(x.state, x.c) && creature::live(x.state, r.who)) {
                auto const tt = creature::template_of(x.state, r.who);
                if (tt == creature::tmpl::slugcat || tt == creature::tmpl::scavenger || tt == creature::tmpl::vulture)
                    return relationship{creature::rel::afraid, 1.f};
            }
            return creature::static_relationship(x.state, x.c, r.who);
        }
        bool hook_reach(void* u, ivec2 t) { return reachable(*static_cast<hook_ctx*>(u), t); }
        tracker::hooks make_hooks(hook_ctx& x) {
            tracker::hooks h;
            h.user = &x;
            h.sees = hook_sees;
            h.sees_point = hook_sees_point;
            h.relate = hook_relate;
            // ModuleToTrackRelationship: Afraid a threat, Eats and Attacks prey
            h.module = tracker::module_without_aggression;
            h.reach = hook_reach;
            return h;
        }

        // FrogAI.IdleSpotScore: lower is better (no AImap visibility here)
        float idle_spot_score(hook_ctx& x, ivec2 coord) {
            auto const& a = x.state.world.frog_data_get_ai(data(x.state, x.c));
            if (!x.room.inside(coord.x, coord.y))
                return 50.f;
            if (!reachable(x, coord) || (a.has_forbidden && coord == a.forbidden_idle_spot))
                return float_max;
            float num = 10.f;
            if (pathing::narrow_space(x.room, coord))
                num += 10.f;
            if (x.room.get_tile(coord).water != 0)
                num += 100.f;
            auto const& ts = tracker::of(x.state, x.c);
            if (tracker::threat_utility(ts) > 0.f)
                num += mathf::clamp(tracker::threat_of_area(ts, coord, tracker_params().see_around_corners), 0.f, 100.f);
            return num;
        }

        // FrogAI.PackLeader: the fullest blood bank of the frogs it tracks
        dcon::creature_id pack_leader_search(sys::game_state const& state, dcon::creature_id c) {
            auto const& ts = tracker::of(state, c);
            dcon::creature_id result;
            int32_t num = 0;
            for (uint32_t i = 0; i < ts.count; ++i) {
                auto const t = ts.reps[i].who;
                if (!creature::live(state, t) || state.world.creature_get_dead(t) || !creature::is(state.world, t, creature::type::frog))
                    continue;
                if (blood_bank(state, t) && int32_t(pl(state, t).feed_size) > num) {
                    num = int32_t(pl(state, t).feed_size);
                    result = t;
                }
            }
            return result;
        }

        // FrogAI.Update (DetermineBehavior and Act within)
        void ai_update(sys::game_state& state, dcon::creature_id c, room_view const& room, rw::rng& rng) {
            auto& w = state.world;
            auto& x = pl(state, c);
            auto& a = ai(state, c);
            ivec2 const at = here_tile(state, c);
            auto const& p = tracker_params();
            hook_ctx hx{state, c, room, std::nullopt};
            tracker::hooks const h = make_hooks(hx);
            tracker::update(state, c, room, at, p, h);
            auto const& ts = tracker::of(state, c);
            if (a.panic > 0)
                --a.panic;

            // UtilityComparer: rain (1), threat (0.4, Lerp 0.5 up, Tick down),
            // prey (0.4, Lerp 0.15)
            float const threat_w = tracker::threat_utility(ts) * 0.4f;
            float const down = 1.f / (float(a.last_distress_length + 20) * 3.f);
            a.threat_smoothed = a.threat_smoothed < threat_w ? mathf::lerp(a.threat_smoothed, threat_w, 0.5f) : mathf::max(a.threat_smoothed - down, threat_w);
            a.prey_smoothed = mathf::lerp(a.prey_smoothed, tracker::prey_utility(state, c, room, at, p) * 0.4f, 0.15f);
            float const rain = dens::rain_utility(state, c, room.id);
            a.fear = a.threat_smoothed / 0.4f;
            a.hunger = a.prey_smoothed / 0.4f;
            a.rain_fear = rain;
            float const combined_fear = mathf::max(a.fear, mathf::pow(a.rain_fear, 3.f));
            a.excitement = mathf::lerp(a.excitement, mathf::max(a.hunger, combined_fear), 0.1f);
            if (a.fear > 0.8f)
                a.last_distress_length = custom::int_clamp(a.last_distress_length + 1, 0, 500);
            else if (a.fear < 0.2f)
                a.last_distress_length = 0;

            // DetermineBehavior
            behavior b = behavior::escape_rain;
            float utility = rain;
            if (a.threat_smoothed > utility) {
                utility = a.threat_smoothed;
                b = behavior::flee;
            }
            if (a.prey_smoothed > utility) {
                utility = a.prey_smoothed;
                b = behavior::hunt;
            }
            if (utility < 0.05f) {
                utility = 0.05f;
                b = behavior::idle;
            }
            if (b != behavior::flee && creature::grasped_object(w, c, 0)) {
                auto const g = creature::grasped(w, c, 0);
                if (!g || tracker::dynamic_relationship(state, c, g).type == creature::rel::eats) {
                    b = behavior::return_prey;
                    utility = 1.f;
                }
            }
            // TravelUtility: a trip to another room (migrate.hpp) wins over
            // idling
            std::optional<ivec2> travel;
            if (b == behavior::idle) {
                if (a.idle_counter <= 0 && rng.value() < 0.05f)
                    migrate::abstract_behavior(state, c, room);
                travel = migrate::travel_tile(state, c, room);
                if (travel && reachable(hx, *travel))
                    b = behavior::travelling;
                else
                    travel = std::nullopt;
            }
            w.creature_set_behavior(c, uint8_t(b));
            w.creature_set_current_utility(c, utility);

            // FrogAI.Act
            if (creature::grasped_object(w, c, 0))
                return;
            ivec2& dest = w.creature_get_destination(c);
            switch (b) {
            case behavior::flee: {
                dest = tracker::flee_to(state, c, room, at, 7, 30, p, h);
                if (tracker::threat_of_tile(ts, at, p.see_around_corners) > 1.f)
                    a.panic = a.panic < 40 ? 40 : a.panic + 2;
                x.run_speed = mathf::lerp(x.run_speed, mathf::pow(float(a.panic), 0.1f), 0.5f);
                break;
            }
            case behavior::travelling:
                x.run_speed = mathf::lerp(x.run_speed, 0.75f, 0.01f);
                dest = *travel;
                break;
            case behavior::hunt: {
                auto const* prey = tracker::prey(ts);
                if (!prey || !creature::live(state, prey->who))
                    return;
                auto const t = prey->who;
                // what something holds is left alone (Malnourished slugcats
                // aren't told apart)
                if (creature::held(state, t)) {
                    tracker::forget(state, c, t);
                    return;
                }
                if (creature::here(state, t, room.id)) {
                    uint32_t const n = creature::chunk_count_of(state, t);
                    vec2 const kp = w.body_chunk_get_pos(creature::chunk_of(state, t, uint32_t(rng.range(0, int32_t(n)))));
                    vec2 const main = pos(state, c, 0), p1 = pos(state, c, 1);
                    vec2 const behind = p1 - custom::dir_vec(p1, kp) * 30.f;
                    if (custom::dist_less(main, kp, 120.f) && (accessible(room, behind) || room.get_tile(behind).solid()) && room.visual_contact(main, kp)) {
                        // (InitiateJump: Frog.Attack, which would act on it,
                        // is never called)
                        if (rw::dot(rw::normalized(main - kp), rw::normalized(p1 - main)) <= 0.2f) {
                            vel(state, c, 0) += custom::dir_vec(main, kp) * 2.f;
                            vel(state, c, 1) -= custom::dir_vec(main, kp) * 2.f;
                        }
                    }
                }
                dest = tracker::best_guess(*prey);
                x.run_speed = mathf::lerp(x.run_speed, mathf::max(a.hunger, 0.3f), 0.1f);
                break;
            }
            case behavior::idle: {
                --a.find_leader_tick;
                if (a.pack_leader && creature::live(state, a.pack_leader) && !w.creature_get_dead(a.pack_leader)) {
                    if (creature::here(state, a.pack_leader, room.id)) {
                        x.run_speed = mathf::lerp(x.run_speed, 0.25f, 0.5f);
                        dest = tile_of(creature::main_pos(state, a.pack_leader));
                        return;
                    }
                    // (a leader in another room: no migration after it)
                } else if (a.find_leader_tick <= 0) {
                    a.find_leader_tick = 1600 - 30;
                    a.pack_leader = pack_leader_search(state, c);
                }
                bool flag = true;
                int32_t const num = a.idle_counter;
                --a.idle_counter;
                if (custom::manhattan_distance(at, dest) < 5) {
                    --a.idle_restlessness;
                    flag = a.idle_counter < 1 || a.idle_restlessness > 0;
                    // (ComfortableIdlePosition: always, it is amphibious)
                    a.unable_to_find_comfortable = 0;
                    if (a.idle_restlessness < 40 && rng.value() < 0.0016666667f)
                        a.idle_restlessness = rng.range(1, 40);
                }
                if (idle_spot_score(hx, dest) == float_max)
                    a.idle_counter = 0;
                if (a.idle_counter < 1) {
                    if (num > 0) {
                        a.forbidden_idle_spot = dest;
                        a.has_forbidden = 1;
                    }
                    ivec2 const coord{rng.range(0, room.width), rng.range(0, room.height)};
                    if (idle_spot_score(hx, coord) < idle_spot_score(hx, dest)) {
                        dest = coord;
                        a.idle_spot_win_streak = 0;
                    } else if (++a.idle_spot_win_streak > 50) {
                        a.idle_counter = rng.range(1000, 4000);
                        a.idle_spot_win_streak = 0;
                    }
                }
                if (flag) {
                    if (!a.has_forbidden || a.forbidden_idle_spot != dest)
                        x.run_speed = mathf::lerp(x.run_speed, 0.25f, 0.5f);
                } else {
                    x.run_speed = mathf::lerp(x.run_speed, 0.f, 0.5f);
                }
                break;
            }
            case behavior::escape_rain:
                if (auto const e = dens::home_tile(state, c, room))
                    dest = *e;
                x.run_speed = mathf::lerp(x.run_speed, 1.f, 0.1f);
                break;
            case behavior::return_prey:
                if (auto const e = dens::home_tile(state, c, room))
                    dest = *e;
                x.run_speed = mathf::lerp(x.run_speed, mathf::pow(mathf::max(combined_fear, 0.5f), 0.5f), 0.1f);
                break;
            case behavior::investigate_sound:
                break;
            }
        }

        //
        // Frog
        //
        // StandardPather.FollowPath from the tile `from`
        movement_connection follow_path(sys::game_state const& state, pathing::field_id f, ivec2 from) {
            auto const step = pathing::next_step(state, f, from);
            if (!step)
                return movement_connection{};
            return movement_connection{from, *step, true};
        }

        // Frog.MoveTowards
        void move_towards(sys::game_state& state, dcon::creature_id c, room_view const& room, vec2 to) {
            auto& x = pl(state, c);
            vec2 v = custom::dir_vec(pos(state, c, 0), to);
            x.walk_dir = v;
            if (pathing::tile_access(room, tile_of(pos(state, c, 1))) >= pathing::access::climb)
                v *= 0.5f;
            if (!footing(state, c))
                v *= 0.3f;
            if (physics::is_tile_solid(state, room, chunk(state, c, 1), 0, -1)
            && ((v.x < -0.5f && pos(state, c, 0).x > pos(state, c, 1).x + 5.f) || (v.x > 0.5f && pos(state, c, 0).x < pos(state, c, 1).x - 5.f))) {
                float const s = v.x < 0.f ? -1.f : 1.f;
                vel(state, c, 0).x -= s * 1.3f;
                vel(state, c, 1).x += s * 0.5f;
                if (!physics::is_tile_solid(state, room, chunk(state, c, 0), 0, 1))
                    vel(state, c, 0).y += 3.2f;
            }
            behavior const b = behavior(state.world.creature_get_behavior(c));
            float const d = (b == behavior::flee || b == behavior::escape_rain) ? flee_speed_factor : 1.f;
            vel(state, c, 0) += v * 2.1f * x.run_speed * d;
            vel(state, c, 1) -= v * 0.5f * x.run_speed * d;
            physics::set_go_through_floors(state, body(state, c), to.y < pos(state, c, 0).y - 5.f);
        }

        // Frog.Swim
        void swim(sys::game_state& state, dcon::creature_id c, room_view const& room, pathing::field_id f) {
            auto const& x = pl(state, c);
            vel(state, c, 0).y += 1.5f;
            auto conn = follow_path(state, f, tile_of(pos(state, c, 0)));
            if (!conn.valid)
                conn = follow_path(state, f, tile_of(pos(state, c, 1)));
            ivec2 const at = here_tile(state, c);
            if (!conn.valid && std::abs(at.y - room.default_water_level) < 4)
                conn = follow_path(state, f, ivec2{at.x, room.default_water_level});
            if (conn.valid) {
                float const s0 = physics::submersion(state, room, chunk(state, c, 0));
                vel(state, c, 0).x -= mathf::sign(room_view::middle_of_tile(conn.start).x - room_view::middle_of_tile(conn.dest).x) * 1.6f * s0;
                jump(state, c, room_view::middle_of_tile(conn.dest), false);
                return;
            }
            vec2 const a = custom::dir_vec(pos(state, c, 0), room_view::middle_of_tile(state.world.creature_get_destination(c)));
            ivec2 const mt = tile_of(pos(state, c, 0));
            bool const surface = room.get_tile(mt).water == 2;
            vel(state, c, 0) += a * 1.3f * mathf::lerp(0.5f, 1.f, x.run_speed) * (surface ? 0.7f : 1.f);
        }

        // the croak roll (Frog.Update, Frog.Act; no croak sounds)
        void maybe_croak(sys::game_state& state, dcon::creature_id c, bool may, rw::rng& rng) {
            auto& x = pl(state, c);
            if (x.croak_counter < -80 && rng.value() < 0.01f && may) {
                x.croak_duration = int16_t(rng.range(15, 25));
                x.croak_counter = x.croak_duration;
            }
        }

        // Frog.Act
        void act(sys::game_state& state, dcon::creature_id c, room_view const& room, rw::rng& rng) {
            auto& w = state.world;
            auto& x = pl(state, c);
            if (physics::object_submersion(state, room, body(state, c)) > 0.3f) {
                auto const f = pathing::field_to(state, room.id, w.creature_get_destination(c), movement);
                swim(state, c, room, f);
                ai_update(state, c, room, rng);
                return;
            }
            ai_update(state, c, room, rng);
            auto& m = mv(state, c);
            if (x.special_move_counter > 0) {
                --x.special_move_counter;
                vec2 const to = room_view::middle_of_tile(m.special_move_destination);
                move_towards(state, c, room, to);
                if (custom::dist_less(pos(state, c, 0), to, 5.f))
                    x.special_move_counter = 0;
                return;
            }
            if (!in_accessible_tiles(state, c, room, 0) && !in_accessible_tiles(state, c, room, 1))
                x.footing_counter = 0;
            if (x.no_jumps > 0 && x.croak_counter <= 0)
                --x.no_jumps;
            if (x.jump_stun > 0)
                x.jump_stun = int16_t(x.jumping ? std::max(0, int32_t(x.jump_stun) - 1) : std::max(0, int32_t(x.jump_stun) - rng.range(6, 10)));
            maybe_croak(state, c, x.sitting == 1.f || grabbed_by_player(state, c), rng);
            if (x.jumping || x.throw_latch) {
                ivec2 const cp = w.body_chunk_get_contact_point(chunk(state, c, 0));
                if (cp.x != 0 || cp.y != 0 || !consious(state, c)) {
                    x.throw_latch_grace = int16_t(x.throw_latch ? 40 : 0);
                    if (x.throw_latch_grace == 0)
                        x.done_with_jump = 1;
                    m.immune_to_latch = dcon::creature_id{};
                    x.throw_latch = 0;
                    m.thrown_by = dcon::creature_id{};
                    x.jumping = 0;
                    return;
                }
                throw_update(state, c, room, rng);
                if (!w.creature_is_valid(c) || attached_to(state, c))
                    return;
            }
            if (x.jump_stun > 0 || x.throw_latch)
                return;
            auto const f = pathing::field_to(state, room.id, w.creature_get_destination(c), movement);
            auto conn = follow_path(state, f, tile_of(pos(state, c, 1)));
            if (!conn.valid)
                conn = follow_path(state, f, tile_of(pos(state, c, 0)));
            // (no Watcher TerrainManager obstruction to climb out of)
            if (w.body_chunk_get_contact_point(chunk(state, c, 1)).y == -1) {
                float num = 0.f;
                if (conn.valid)
                    num = rw::normalized(rw::to_vec2(conn.dest - conn.start)).x;
                float const num2 = std::sqrt(x.sitting);
                vel(state, c, 0).x += num2 * num * 0.25f;
                vel(state, c, 0).y += num2 * 1.5f;
                vel(state, c, 1).x -= num2 * num * 0.25f;
                vel(state, c, 1).y -= num2 * 1.5f;
            }
            bool flag = false;
            if (conn.valid) {
                ivec2 const step = conn.dest - conn.start;
                if (step.x != 0 && step.y != 0) {
                    // OpenDiagonal and the reaches: a special move
                    x.special_move_counter = 30;
                    m.special_move_destination = conn.dest;
                } else {
                    vec2 const v = room_view::middle_of_tile(conn.dest);
                    if (conn.start.y < conn.dest.y || !physics::is_tile_solid(state, room, chunk(state, c, 0), 0, -1) || conn.start.x == conn.dest.x || x.jump_stun > 0) {
                        for (uint32_t k = 0; k < 2; ++k) {
                            if (conn.start.x == conn.dest.x)
                                vel(state, c, k).x += mathf::min((v.x - pos(state, c, k).x) / 8.f, 1.2f);
                            else if (conn.start.y == conn.dest.y)
                                vel(state, c, k).y += mathf::min((v.y - pos(state, c, k).y) / 8.f, 1.2f);
                        }
                        auto const& last = m.last_followed;
                        if (last.valid && (footing(state, c) || in_accessible_tiles(state, c, room, 0))
                        && ((conn.start.x != conn.dest.x && last.start.x == last.dest.x) || (conn.start.y != conn.dest.y && last.start.y == last.dest.y))) {
                            vel(state, c, 0) *= 0.7f;
                            vel(state, c, 1) *= 0.5f;
                        }
                        // DropToFloor
                        if (step.y < -1)
                            x.footing_counter = 0;
                        x.walking = 1;
                        move_towards(state, c, room, v);
                    } else {
                        jump(state, c, v, false);
                        flag = true;
                    }
                }
                m.last_followed = conn;
            } else {
                physics::set_go_through_floors(state, body(state, c), false);
            }
            if ((!conn.valid || flag) && physics::is_tile_solid(state, room, chunk(state, c, 1), 0, -1)) {
                x.sitting = custom::lerp_and_tick(x.sitting, 1.f, 0.1f, 0.05f);
                return;
            }
            x.sitting = mathf::max(x.sitting - 0.1f, 0.f);
        }

        // FrogGraphics.GenerateColors and the horns, from the ID's seed
        looks generate_looks(int32_t seed) {
            rw::unity_random r{seed};
            looks l;
            vec3 body, belly, eye;
            if (r.value() < 0.05f) {
                body = vec3{0.2f, 0.17f, 0.15f};
                belly = vec3{0.12f, 0.09f, 0.41f};
                eye = body;
                eye.z += 0.5f;
            } else {
                body = vec3{0.06f, 0.47f, 0.38f};
                belly = vec3{0.09f, 0.45f, 0.74f};
                eye = body;
            }
            float const num = custom::clamped_random_variation(0.5f, 0.1f, 0.2f, r) - 0.5f;
            body.x = custom::decimal(body.x + num);
            body.y = custom::clamped_random_variation(body.y, 0.1f, 0.1f, r);
            body.z = custom::clamped_random_variation(body.z, 0.2f, 0.1f, r);
            belly.x = custom::decimal(belly.x + num);
            belly.y = custom::clamped_random_variation(belly.y, 0.1f, 0.1f, r);
            belly.z = custom::clamped_random_variation(belly.z, 0.2f, 0.1f, r);
            l.body = custom::hsl2rgb(body.x, body.y, body.z);
            l.belly = custom::hsl2rgb(belly.x, belly.y, belly.z);
            l.eye = custom::hsl2rgb(eye.x, eye.y, mathf::clamp01(eye.z));
            l.horns = int32_t(std::nearbyint(3.f * r.value()));
            l.horn_scale = r.range(0.8f, 1.2f);
            return l;
        }
    }

    dcon::creature_id attached_to(sys::game_state const& state, dcon::creature_id c) noexcept {
        auto const t = creature::grasped(state.world, c, 0);
        return creature::live(state, t) ? t : dcon::creature_id{};
    }

    bool blood_bank(sys::game_state const& state, dcon::creature_id c) noexcept { return pl(state, c).feed_size >= 3; }

    float blood_bank_bloat(sys::game_state const& state, dcon::creature_id c) noexcept {
        return mathf::lerp(0.f, 3.f, float(pl(state, c).feed_size) / float(max_feeds));
    }

    float croaking(sys::game_state const& state, dcon::creature_id c) noexcept {
        auto const& x = pl(state, c);
        return x.croak_duration > 0 ? mathf::max(float(x.croak_counter) / float(x.croak_duration), 0.f) : 0.f;
    }

    float dig_in(sys::game_state const& state, dcon::creature_id c) noexcept {
        float const t = mathf::inverse_lerp(20.f, 120.f, float(pl(state, c).time_attached));
        // Mathf.SmoothStep(0, 1, t)
        return t * t * (3.f - 2.f * t);
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::frog);
        auto const d = data(state, c);
        // the creature's rolls at its making (tick 0: from its identity)
        auto rng = rw::random(state.seed, 0, uint32_t(c.value));
        int32_t seed = int32_t(rng.next() & 0x7FFFFFFFu);
        if (state.spawn_seed >= 0)
            seed = state.spawn_seed;
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
        // two chunks: the head (2.75) and the belly (3, swelling with the
        // feeds), half the mass each, 6 apart (a Pull connection)
        auto const c0 = physics::add_chunk(state, o, at, 2.75f * size_multiplier, total_mass / 2.f);
        auto const c1 = physics::add_chunk(state, o, at + vec2{0.f, -1.f}, 3.f * size_multiplier, total_mass / 2.f);
        physics::add_connection(state, o, c0, c1, 6.f * size_multiplier, physics::connection_type::pull, 1.f, 0.5f);
        w.try_create_creature_body(c, o);
        auto& x = pl(state, c);
        uint16_t const keep = x.data;
        x = creature::frog_payload{};
        x.data = keep;
        x.bites = uint8_t(start_bites);
        x.no_jumps = 5;
        x.attach_side = 1;
        x.walk_dir = vec2{0.f, 0.f};
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_set_grasps(c, creature::grasp_pair{});
        w.creature_set_behavior(c, uint8_t(behavior::idle));
        w.creature_set_current_utility(c, 0.f);
        w.creature_set_destination(c, tile_of(at));
        w.frog_data_set_seed(d, seed);
        w.frog_data_set_move(d, move_state{});
        w.frog_data_set_ai(d, ai_state{});
        w.frog_data_set_looks(d, generate_looks(seed));
        w.frog_data_set_graphics(d, graphics_state{});
        tracker::clear(state, c);
        frog_graphics::reset_positions(state, c);
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        creature::release_grasp(state, c, 0);
        creature::release_target(state, c);
        creature::erase(state, c);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        auto const o = body(state, c);
        auto& x = pl(state, c);
        // Creature.Update: the stun, the injected poison (no HealthState: it
        // stays, makes the frog faint now and then unless it is latched on,
        // and kills it at the instant death limit)
        creature::stun_countdown(state, c);
        {
            float const p = mv(state, c).injected_poison;
            if (p > 0.f) {
                float const num4 = p / instant_death_damage_limit;
                if (consious(state, c) && rng.value() < 0.03f * num4 * (0.5f + num4 * 0.5f) && !creature::grasped_object(w, c, 0))
                    creature::stun(state, c, rng.range(20, int32_t(std::nearbyint(80.f * rng.value()))));
                if (num4 >= 1.f && !w.creature_get_dead(c))
                    creature::die(state, c);
            }
        }
        // PhysicalObject.Update (Frog.TerrainImpact does nothing)
        physics::update_object(state, room, o, nullptr, nullptr);
        if (!w.creature_is_valid(c))
            return;
        // Frog.Stun: a stunned frog lets go
        if (w.creature_get_stun(c) > 0 && creature::grasped_object(w, c, 0))
            detach(state, c, room, false, rng);

        // (attachOnSpawn: no saved attachments)
        if (x.throw_latch || x.throw_latch_grace > 0) {
            x.throw_latch_grace = int16_t(std::max(int32_t(x.throw_latch_grace) - 1, 0));
            throw_update(state, c, room, rng);
        }
        if (!grabbed_by_player(state, c) && !x.throw_latch && x.throw_latch_grace == 0)
            mv(state, c).immune_to_latch = dcon::creature_id{};
        if (auto const t = attached_to(state, c)) {
            // (a slugcat's injected poison and Malnourished state aren't kept:
            // nothing to drain or restore there)
            if (creature::template_of(state, t) == creature::tmpl::basilisk_lizard)
                inject_poison(state, c, 0.005f, lizard::params(lizard::breed::basilisk).standard_color);
        }
        w.physical_object_set_g(o, creature::grasped_object(w, c, 0) ? 0.2f : 0.9f);
        x.walking = 0;
        if (creature::grasped_object(w, c, 0)) {
            attached(state, c, room, rng);
        } else if (consious(state, c)) {
            if (in_accessible_tiles(state, c, room, 0)) {
                if (x.footing_counter < 30000)
                    ++x.footing_counter;
                act(state, c, room, rng);
            } else {
                x.footing_counter = 0;
            }
        } else {
            x.footing_counter = 0;
            maybe_croak(state, c, grabbed_by_player(state, c) && !w.creature_get_dead(c), rng);
        }
        if (!w.creature_is_valid(c))
            return;
        // standing: its gravity cancelled, more so sitting
        if (footing(state, c) && !x.throw_latch) {
            float const g = w.physical_object_get_g(o);
            for (uint32_t i = 0; i < 2; ++i) {
                vel(state, c, i) *= 0.8f;
                vel(state, c, i).y += g * (1.f - x.sitting);
            }
        }
        if (x.croak_counter > -30000)
            --x.croak_counter;
        if (x.pain_sound_cooldown > 0)
            --x.pain_sound_cooldown;
        if (x.footing_counter == 0)
            x.sitting = 0.f;
        // scrambling in the air, fleeing
        if (consious(state, c) && !footing(state, c) && behavior(w.creature_get_behavior(c)) == behavior::flee)
            for (uint32_t i = 0; i < 2; ++i)
                if (accessible(room, pos(state, c, i)))
                    vel(state, c, i) += custom::deg_to_vec(rng.value() * 360.f) * rng.value() * 5.f;
    }

    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) {
        // latched on it collides with nothing (setCollisions(false))
        if (creature::grasped_object(state.world, c, 0))
            return false;
        return !creature::holds(state.world, t, c);
    }

    void collide(sys::game_state& state, dcon::creature_id c, room_view const& room, uint32_t, dcon::creature_id t, uint32_t j) {
        auto& x = pl(state, c);
        if (x.throw_latch && !blood_bank(state, c)) {
            auto rng = rw::random(state.seed, state.tick, rw::ids(uint32_t(c.value), uint32_t(t.value)));
            attach(state, c, room, t, j, true, rng);
            x.throw_latch = 0;
        }
        // (CreatureParams.stunOnCollideVelocity is 0: no stunning bumps)
    }

    void violence(sys::game_state& state, dcon::creature_id c, uint32_t k, vec2 momentum, float damage, float, creature::damage_type) {
        auto& w = state.world;
        // latched on, nothing touches it
        if (creature::grasped_object(w, c, 0))
            return;
        auto const hit = chunk(state, c, k < 2 ? k : 0);
        w.body_chunk_get_vel(hit) += rw::clamp_magnitude(momentum / w.body_chunk_get_mass(hit), 10.f);
        // (no Frog_Pain sound) Creature.Violence over baseDamageResistance
        // and baseStunResistance 0: any damage is past the instant death
        // limit, and the stun is never a positive number
        if (damage > 0.f)
            creature::die(state, c);
    }

    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t) {
        if (creature::grasped(state.world, c, 0) != t)
            return;
        creature::release_grasp(state, c, 0);
        pl(state, c).time_attached = 0;
        set_collisions(state, c, true);
    }

    void died(sys::game_state& state, dcon::creature_id c) {
        // Frog.Die: Detach (Creature.Die let go of everything already)
        auto& x = pl(state, c);
        x.time_attached = 0;
        x.throw_latch = 0;
        x.jumping = 0;
        if (creature::grasped_object(state.world, c, 0))
            creature::release_grasp(state, c, 0);
        set_collisions(state, c, true);
        state.world.physical_object_set_g(body(state, c), 0.9f);
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
        // FrogAI.NewRoom: what it walked to is in the other room
        auto& x = pl(state, c);
        x.special_move_counter = 0;
        x.jumping = 0;
        x.throw_latch = 0;
        x.throw_latch_grace = 0;
        auto& m = mv(state, c);
        m.last_followed = movement_connection{};
        m.thrown_by = dcon::creature_id{};
        m.immune_to_latch = dcon::creature_id{};
        auto& a2 = ai(state, c);
        a2.idle_counter = 0;
        a2.has_forbidden = 0;
        a2.pack_leader = dcon::creature_id{};
        w.creature_set_destination(c, t);
    }

    std::string describe(sys::game_state const& state, dcon::creature_id c) {
        static constexpr char const* names[] = {"idle", "travelling", "flee", "hunt", "return prey", "investigate sound", "escape rain"};
        auto const& w = state.world;
        vec2 const p = pos(state, c, 0);
        uint32_t const b = w.creature_get_behavior(c);
        auto const& x = pl(state, c);
        std::string s = "frog at " + std::to_string(int(p.x)) + "," + std::to_string(int(p.y)) + " behavior " + (b < 7 ? names[b] : "?") + " fed "
            + std::to_string(int(x.feed_size));
        if (attached_to(state, c))
            s += std::string(" latched on a ") + creature::to_string(creature::type_of(w, attached_to(state, c)));
        if (blood_bank(state, c))
            s += " blood bank";
        if (x.sitting > 0.5f)
            s += " sitting";
        if (x.throw_latch)
            s += " thrown";
        return s;
    }

    bool spawn_spot(room_view const& room, ivec2 tile) {
        return room.inside(tile.x, tile.y) && !room.get_tile(tile).solid() && room.get_tile(tile + ivec2{0, -1}).solid() && pathing::accessible(room, tile, movement)
            && room.get_tile(tile).water == 0;
    }

    //
    // The slugcat side
    //
    dcon::creature_id held_in_hand(sys::game_state const& state, dcon::player_id p, uint32_t hand) noexcept {
        if (!p)
            return dcon::creature_id{};
        auto const c = creature::grasped(state.world, player::creature_of(state, p), hand);
        return c && creature::type_of(state.world, c) == creature::type::frog ? c : dcon::creature_id{};
    }

    dcon::creature_id held_by(sys::game_state const& state, dcon::player_id p) noexcept {
        if (auto const c = held_in_hand(state, p, 0))
            return c;
        return held_in_hand(state, p, 1);
    }

    dcon::creature_id pickup_candidate(sys::game_state const& state, dcon::player_id p, room_view const& room, float& score) {
        auto const& w = state.world;
        score = float_max;
        vec2 const p0 = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
        int32_t const flip = w.player_get_flip_direction(p);
        dcon::creature_id result;
        rooms::for_each_creature(state, room.id, creature::type::frog, [&](dcon::creature_id c) {
            // Player.Grabability: OneHand, CantGrab latched on
            if (creature::grasped_object(w, c, 0) || creature::held(state, c) || w.creature_get_away(c) != 0)
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

    void grab(sys::game_state& state, dcon::player_id p, dcon::creature_id c, uint32_t hand) {
        creature::grab(state, player::creature_of(state, p), hand, c, 0);
        auto& x = pl(state, c);
        x.jumping = 0;
        x.special_move_counter = 0;
    }

    void release(sys::game_state& state, dcon::creature_id c) {
        uint32_t g = 0;
        if (auto const h = creature::holder(state, c, &g))
            creature::release_grasp(state, h, g);
    }

    void release_player(sys::game_state& state, dcon::player_id p) {
        for (uint32_t hand = 0; hand < 2; ++hand)
            if (held_in_hand(state, p, hand))
                creature::release_grasp(state, player::creature_of(state, p), hand);
    }

    void carry_update(sys::game_state& state, dcon::player_id p, uint32_t hand, vec2 hand_pos) {
        auto& w = state.world;
        auto const c = held_in_hand(state, p, hand);
        if (!c)
            return;
        auto const k = chunk(state, c, 0);
        w.body_chunk_set_last_pos(k, w.body_chunk_get_pos(k));
        w.body_chunk_set_pos(k, hand_pos);
        w.body_chunk_set_vel(k, w.body_chunk_get_vel(player::body_chunk(state, p, 0)));
        // Player.Update's grasp loop: held, it latches on to anything but
        // its holder
        mv(state, c).immune_to_latch = player::creature_of(state, p);
        pl(state, c).throw_latch = 1;
    }

    void follow_player(sys::game_state& state, dcon::player_id p, dcon::room_id room, bool spit_out) {
        auto& w = state.world;
        for (uint32_t hand = 0; hand < 2; ++hand) {
            auto const c = held_in_hand(state, p, hand);
            if (!c)
                continue;
            w.physical_object_set_room_from_object_location(body(state, c), room);
            if (!spit_out)
                continue;
            vec2 const at = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
            for (uint32_t i = 0; i < 2; ++i) {
                physics::hard_set_position(state, chunk(state, c, i), at + vec2{0.f, float(i) * -5.f});
                w.body_chunk_set_vel(chunk(state, c, i), w.body_chunk_get_vel(player::body_chunk(state, p, 0)));
            }
            auto& m = mv(state, c);
            m.last_followed = movement_connection{};
            auto& a = ai(state, c);
            a.idle_counter = 0;
            a.has_forbidden = 0;
            a.pack_leader = dcon::creature_id{};
            w.creature_set_destination(c, tile_of(at));
            tracker::clear(state, c);
            frog_graphics::reset_positions(state, c);
        }
    }

    bool thrown(sys::game_state& state, dcon::player_id p, dcon::creature_id c, ivec2 throw_dir, float force) {
        auto& w = state.world;
        if (blood_bank(state, c))
            return false;
        auto const pc = player::creature_of(state, p);
        auto const room = world::get_room_view(state, creature::room_of(state, c));
        vec2 const p0 = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
        vec2 const pv = w.body_chunk_get_vel(player::body_chunk(state, p, 0));
        vec2 v2 = p0 + rw::to_vec2(throw_dir) * 10.f + vec2{0.f, 4.f};
        if (room.get_tile(v2).solid())
            v2 = p0;
        release(state, c);
        auto& x = pl(state, c);
        auto& m = mv(state, c);
        m.immune_to_latch = pc;
        x.throw_latch = 1;
        x.jump_stun = 40;
        // Frog.Thrown
        m.thrown_by = pc;
        m.throw_dir = throw_dir;
        m.first_frame_trace_from = p0 - rw::to_vec2(throw_dir) * 10.f;
        m.has_first_frame = 1;
        auto const k0 = chunk(state, c, 0);
        vec2 const thrown_pos = v2 + vec2{0.f, 0.5f};
        physics::hard_set_position(state, k0, thrown_pos);
        w.body_chunk_set_last_pos(k0, thrown_pos);
        vec2& v = w.body_chunk_get_vel(k0);
        if (throw_dir.x != 0) {
            v.y = pv.y * 0.5f;
            v.x = pv.x * 0.2f;
            v.x += float(throw_dir.x) * 40.f * force;
            v.y += 3.f;
        } else if (throw_dir.y != 0) {
            v.x = pv.x * 0.5f;
            v.y = float(throw_dir.y) * 40.f * force;
        }
        return true;
    }

    void tossed(sys::game_state& state, dcon::player_id p, dcon::creature_id c) {
        mv(state, c).immune_to_latch = player::creature_of(state, p);
        auto& x = pl(state, c);
        x.throw_latch = 1;
        x.jump_stun = 40;
    }

    bool edible(sys::game_state const& state, dcon::creature_id c) noexcept { return blood_bank(state, c); }

    bool bit_by_player(sys::game_state& state, dcon::creature_id c, dcon::player_id p) {
        auto& w = state.world;
        auto& x = pl(state, c);
        if (x.bites > 0)
            --x.bites;
        if (!w.creature_get_dead(c))
            creature::die(state, c);
        // (no Slugcat_Bite_Fly sounds) MoveFromOutsideMyUpdate: to the mouth
        vec2 const mouth = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
        auto const k = chunk(state, c, 0);
        w.body_chunk_set_pos(k, mouth);
        w.body_chunk_set_last_pos(k, mouth);
        return x.bites < 1;
    }
}
