// Watcher/BigMoth.cs, BigMothAI.cs and BigMothPather.cs: the big and the
// small moth. See moth.hpp.
//
// Not vanilla (see the integration note's DIVERGENCES bullets): no sky exits
// and no abstract layer (BigMothAbstractAI): a big moth leaves rooms and
// goes home through the pipes like the rest (vanilla's big moths never enter
// one: forbidStandardShortcutEntry), its "offscreen den" is its den
// (dens.hpp) or a migration trip (migrate.hpp). The tongue reaching what it
// drinks from is decided here (TongueGraphics.Update's outness, foldness and
// tongueOverCounter, with the tip's reach taken as the tongue's length):
// vanilla's graphics module does it on screen. The blink rolls that make it
// chatter and shake are the simulation's. No noise reactions, no grabbed
// slugcat losing its footing (Player.standing), no Rope around corners
// between the held chunk and the body, no sounds without a sample here.

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <source_location>
#include <string>

#include "moth.hpp"
#include "dens.hpp"
#include "migrate.hpp"
#include "moth_graphics.hpp"
#include "moth_types.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "rooms.hpp"
#include "scavenger.hpp"
#include "tracker.hpp"

namespace moth {
    using creature::relationship;
    using creature::rel;
    using creature::tmpl;
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        constexpr float float_max = std::numeric_limits<float>::max();
        constexpr int32_t int_max = std::numeric_limits<int32_t>::max();
        constexpr uint32_t chunk_count = 5;
        // CreatureTemplate.visualRadius
        constexpr float visual_radius = 1100.f;

        // CreatureTemplate (StaticWorld.InitBigMoth / InitSmallMoth):
        // baseDamageResistance, baseStunResistance, instantDeathDamageLimit
        struct template_params {
            float damage_resistance;
            float stun_resistance;
            float instant_death;
        };
        constexpr template_params params_of[variant_count] = {{4.4f, 1.f, 4.f}, {0.3f, 0.3f, 0.3f}};

        // The pathing (BigMothPather over an Air template). The big moth's
        // AImap forbids the tiles next to the terrain (getTerrainProximity <
        // 2) and its pather prefers flying about 5 tiles over the floor: here
        // a flyer that prefers open space (CicadaPather's cost); the small
        // moth's pather adds 100 to a step more than a tile over a floor:
        // here the mid air cost (it keeps to the terrain)
        constexpr pathing::movement big_flyer{{10, 10, 10, 10, 10, 10, 0}, 0, 10};
        constexpr pathing::movement small_flyer{{10, 10, 10, 10, 10, 10, 0}, 0, 0, 50};

        // Custom.PushFromHalf
        float push_from_half(float v, float e) {
            if (v == 0.5f)
                return 0.5f;
            return v < 0.5f ? custom::lerp_map(v, 0.f, 0.5f, 0.f, 0.5f, e) : custom::lerp_map(v, 1.f, 0.5f, 1.f, 0.5f, e);
        }
        // Mathf.LerpAngle
        float lerp_angle(float a, float b, float t) {
            float d = std::fmod(b - a, 360.f);
            if (d < 0.f)
                d += 360.f;
            if (d > 180.f)
                d -= 360.f;
            return a + d * mathf::clamp01(t);
        }
        // Custom.ClosestPointOnLineSegment
        vec2 closest_point_on_segment(vec2 a, vec2 b, vec2 p) {
            vec2 const ab = b - a;
            float const l = rw::sqr_magnitude(ab);
            if (l < 1e-10f)
                return a;
            float const t = mathf::clamp01(rw::dot(p - a, ab) / l);
            return a + ab * t;
        }
        // Random.insideUnitCircle
        vec2 inside_unit_circle(rw::rng& r) { return custom::rnv(r) * std::sqrt(r.value()); }
        ivec2 tile_of(vec2 p) { return room_view::get_tile_position(p); }
        ivec2 clamp_to_room(room_view const& room, ivec2 t) {
            return ivec2{custom::int_clamp(t.x, 0, room.width - 1), custom::int_clamp(t.y, 0, room.height - 1)};
        }
        // Room.HasAnySolid (clamped into the room like GetTile)
        bool any_solid(room_view const& room, ivec2 t) {
            auto const k = room.get_tile(t).t;
            return k == world::terrain::solid || k == world::terrain::slope;
        }
        float float_dist(ivec2 a, ivec2 b) { return rw::magnitude(rw::to_vec2(a - b)); }

        // CreatureTemplate.meatPoints, roughly (rwcpp has no table of them):
        // by the body size, a small creature 1
        int32_t meat_points(tmpl t) {
            if (t == tmpl::moth_grub)
                return 5;
            if (creature::small_creature(t))
                return 1;
            return custom::int_clamp(int32_t(std::lround(creature::body_size(t) * 1.5f)), 2, 15);
        }

        // AI.SmallWantsToEat: a Rat or a Fly (no rats in rwcpp)
        bool small_wants_to_eat(sys::game_state const& state, dcon::creature_id t) {
            return state.world.creature_get_type(t) == creature::type::batfly;
        }

        struct ctx {
            sys::game_state& state;
            dcon::creature_id c;
            room_view const& room;
            rw::rng rng;
            creature::moth_payload& p;
            dcon::moth_data_id d;
            body_state& b;
            ai_state& ai;
            ivars const& iv;
            std::optional<pathing::field_id> field;
            ivec2 field_goal{-1, -1};

            ctx(sys::game_state& s, dcon::creature_id cc, room_view const& r, std::source_location l = std::source_location::current())
                : state(s), c(cc), room(r), rng(rw::random(s.seed, s.tick, uint32_t(cc.value), l)), p(s.world.creature_get_payload(cc).moth),
                  d(creature::moth_data(s.world, cc)), b(s.world.moth_data_get_body(creature::moth_data(s.world, cc))),
                  ai(s.world.moth_data_get_ai(creature::moth_data(s.world, cc))), iv(s.world.moth_data_get_ivars(creature::moth_data(s.world, cc))) {}

            dcon::data_container& w() { return state.world; }
            bool small() const { return p.variant == variant::small; }
            float scale() const { return p.scale; }
            dcon::body_chunk_id ch(uint32_t i) const { return chunk(state, c, i); }
            vec2& pos(uint32_t i) { return state.world.body_chunk_get_pos(ch(i)); }
            vec2& vel(uint32_t i) { return state.world.body_chunk_get_vel(ch(i)); }
            float mass(uint32_t i) { return state.world.body_chunk_get_mass(ch(i)); }
            float g() { return state.world.physical_object_get_g(body(state, c)); }
            behavior beh() { return behavior(state.world.creature_get_behavior(c)); }
            void set_beh(behavior v) { state.world.creature_set_behavior(c, uint8_t(v)); }
            ivec2 at() { return clamp_to_room(room, tile_of(pos(0))); }
            ivec2 destination() { return state.world.creature_get_destination(c); }
            void set_destination(ivec2 t) { state.world.creature_set_destination(c, clamp_to_room(room, t)); }
            pathing::movement const& movement() const { return small() ? small_flyer : big_flyer; }
            pathing::field_id path() {
                ivec2 const dest = destination();
                if (!field || field_goal != dest) {
                    field = pathing::field_to(state, room.id, dest, movement());
                    field_goal = dest;
                }
                return *field;
            }
            // a tile it may fly through (AImap's forbidden tiles for the big
            // moth: next to the terrain)
            bool accessible(ivec2 t) {
                if (!room.inside(t.x, t.y) || !pathing::accessible(room, t, movement()))
                    return false;
                return small() || pathing::ai_terrain_proximity(state, room.id, t) >= 2;
            }
            // PathFinder.CoordinateReachable(AndGetbackable): judged by the
            // field to its destination (a flyer can go back the way it came)
            bool reachable(ivec2 t) { return accessible(t) && pathing::distance(state, path(), t) != pathing::unreachable; }
            // the creature `grasps[0]` holds (none for nothing, or an item)
            dcon::creature_id held() { return creature::grasped(state.world, c, 0); }
        };

        //
        // Social memory (SocialMemory, as far as BigMothAI uses it)
        //
        social* find_social(ai_state& ai, dcon::creature_id t) {
            for (auto& s : ai.memory)
                if (s.who == t)
                    return &s;
            return nullptr;
        }
        float temp_like(ai_state& ai, dcon::creature_id t) {
            auto const* s = find_social(ai, t);
            return s ? s->temp_like : 0.f;
        }
        // GetOrInitiateRelationship (the weakest feeling forgotten for a new one)
        social& social_of(ai_state& ai, dcon::creature_id t) {
            if (auto* s = find_social(ai, t))
                return *s;
            social* slot = &ai.memory[0];
            for (auto& s : ai.memory) {
                if (!s.who) {
                    slot = &s;
                    break;
                }
                if (std::fabs(s.like) + std::fabs(s.temp_like) < std::fabs(slot->like) + std::fabs(slot->temp_like))
                    slot = &s;
            }
            *slot = social{t, 0.f, 0.f};
            return *slot;
        }
        void influence(ai_state& ai, dcon::creature_id t, float temp, float like) {
            auto& s = social_of(ai, t);
            s.temp_like = mathf::clamp(s.temp_like + temp, -1.f, 1.f);
            s.like = mathf::clamp(s.like + like, -1.f, 1.f);
        }
        int32_t thrown_timer(ai_state const& ai, dcon::creature_id t) {
            for (auto const& s : ai.thrown_away)
                if (s.who == t)
                    return s.timer;
            return 0;
        }

        //
        // BigMothAI: the trackers
        //
        // Tracker(10, 10, 450, 0.5, 5, 5, 10), ThreatTracker(3),
        // PreyTracker(5, 1, 5, 150, 0.05)
        tracker::params const& tracker_params() {
            static tracker::params const p = [] {
                tracker::params q;
                q.see_around_corners = 10;
                q.frames_to_remember = 450;
                q.ghost_dismissal_range = 10;
                q.prey_max = 5;
                q.persistance_bias = 1.f;
                q.sure_to_get = 5.f;
                q.sure_to_lose = 150.f;
                q.threat_max = 3;
                return q;
            }();
            return p;
        }

        // ArtificialIntelligence.VisualScore/VisualContact from the head
        bool visual_contact(ctx& x, vec2 look) {
            vec2 const eye = x.pos(0);
            if (!custom::dist_less(eye, look, visual_radius))
                return false;
            float score = mathf::inverse_lerp(visual_radius, 0.f, rw::distance(eye, look));
            if (pathing::narrow_space(x.room, clamp_to_room(x.room, tile_of(look))))
                score -= 0.5f;
            return score > 0.f && x.room.visual_contact(eye, look);
        }

        // BigMothAI.UpdateDynamicRelationship (no vulture masks here)
        relationship relate(ctx& x, creature::tracked const& r) {
            auto const t = r.who;
            auto const tt = creature::template_of(x.state, t);
            if (creature::small_creature(tt))
                return relationship{rel::ignores, 0.f};
            relationship res = creature::static_relationship(x.state, x.c, t);
            // (the ChildTrackerModule: its grubs, found among the tracked ones)
            if (tt == tmpl::moth_grub)
                return relationship{rel::ignores, 0.f};
            bool const alive = !creature::dead(x.state, t);
            if (res.type == rel::afraid || res.type == rel::uncomfortable) {
                if (!alive) {
                    res.intensity = 0.f;
                } else if (!x.small()) {
                    float const tl = temp_like(x.ai, t);
                    if (tl > 0.15f)
                        res.type = rel::ignores;
                    else if (tl < -0.9f)
                        res.type = rel::eats;
                    else if (tl < -0.15f)
                        res.type = rel::attacks;
                }
                if (thrown_timer(x.ai, t) > 0) {
                    res.type = rel::ignores;
                    res.intensity = 0.f;
                }
            } else if (res.type == rel::antagonizes) {
                if (temp_like(x.ai, t) < -0.2f)
                    res.type = rel::afraid;
            }
            if (!alive && meat_points(tt) >= 2 && tt != tmpl::big_moth && tt != tmpl::small_moth && tt != tmpl::moth_grub) {
                res.type = rel::eats;
                // (meatLeft: what this moth hasn't drunk of it yet)
                bool const left = !(x.b.drink == t && x.b.meat_taken >= meat_points(tt));
                res.intensity = mathf::inverse_lerp(0.f, 5.f, float(meat_points(tt))) * (left ? 1.f : 0.5f);
            }
            return res;
        }
        // IUseARelationshipTracker.ModuleToTrackRelationship
        creature::track_module module_of(relationship r) noexcept {
            if (r.type == rel::eats)
                return creature::track_module::prey;
            if (r.type == rel::afraid || r.type == rel::uncomfortable || r.type == rel::attacks || r.type == rel::antagonizes)
                return creature::track_module::threat;
            return creature::track_module::none;
        }
        bool hook_sees(void* u, dcon::body_chunk_id k) {
            auto& x = *static_cast<ctx*>(u);
            return visual_contact(x, x.state.world.body_chunk_get_pos(k));
        }
        bool hook_sees_point(void* u, vec2 p) { return visual_contact(*static_cast<ctx*>(u), p); }
        relationship hook_relate(void* u, creature::tracked const& r) { return relate(*static_cast<ctx*>(u), r); }
        bool hook_reach(void* u, ivec2 t) { return static_cast<ctx*>(u)->reachable(t); }
        tracker::hooks make_hooks(ctx& x) {
            tracker::hooks h;
            h.user = &x;
            h.sees = hook_sees;
            h.sees_point = hook_sees_point;
            h.relate = hook_relate;
            h.module = module_of;
            h.reach = hook_reach;
            return h;
        }

        // CreatureLooker.ReevaluateLookObject with BigMothAI's
        // CreatureInterestBonus (never a small creature, half for another
        // big moth, ten times the focus creature)
        void reevaluate_look(ctx& x) {
            auto const& w = x.state.world;
            auto const& ts = tracker::of(x.state, x.c);
            x.ai.look_creature = dcon::creature_id{};
            float best = 0.f;
            bool const flag = x.rng.value() < 0.5f;
            ivec2 const at = x.at();
            auto const me = creature::template_of(x.state, x.c);
            for (uint32_t i = 0; i < ts.count; ++i) {
                auto const& r = ts.reps[i];
                if (!creature::live(x.state, r.who))
                    continue;
                auto const tt = creature::template_of(x.state, r.who);
                relationship const rl = creature::static_relationship(me, tt);
                float num2 = 1.f + mathf::lerp(creature::body_size(tt), 1.f, 0.5f) * rl.intensity * tracker::chance_of_finding(r, tracker_params().see_around_corners);
                if (flag)
                    num2 += x.rng.value();
                if (r.visual && creature::here(x.state, r.who, x.room.id))
                    num2 *= 1.f + rw::magnitude(w.body_chunk_get_vel(creature::chunk_of(x.state, r.who, creature::main_chunk_of(x.state, r.who))));
                num2 /= mathf::lerp(rw::magnitude(rw::to_vec2(tracker::best_guess(r) - at)), 1.f, 0.6f);
                if (rl.type == rel::ignores)
                    num2 /= 2.f;
                if (creature::small_creature(tt))
                    num2 = -1.f;
                else if (tt == tmpl::big_moth)
                    num2 *= 0.5f;
                else if (r.who == x.ai.focus)
                    num2 *= 10.f;
                if (num2 > best) {
                    best = num2;
                    x.ai.look_creature = r.who;
                }
            }
        }
        // CreatureLooker.Update (chanceToLookAtNothing 0.0025, 30 frames
        // between focus changes)
        void looker_update(ctx& x) {
            if (x.ai.look_creature && !tracker::find(tracker::of(x.state, x.c), x.ai.look_creature))
                x.ai.look_creature = dcon::creature_id{};
            if (x.ai.look_focus_delay > 0)
                --x.ai.look_focus_delay;
            if (x.ai.look_focus_delay < 1 && x.rng.value() < 1.f / 30.f) {
                x.ai.look_focus_delay = 30;
                if (x.rng.value() > 0.0025f)
                    reevaluate_look(x);
                else
                    x.ai.look_creature = dcon::creature_id{};
            }
        }

        // StuckTracker.Update (trackPastPositions) with its
        // GetUnstuckPosCalculator
        void stuck_update(ctx& x, bool satisfied) {
            auto& s = x.ai.stuck;
            ivec2 const at = x.at();
            if (s.counter > 0) {
                ivec2 const test = clamp_to_room(x.room, tile_of(x.pos(0) + custom::rnv(x.rng) * x.rng.value() * 15.f * 20.f));
                auto const score = [&](ivec2 t) {
                    if (!x.reachable(t))
                        return float_max;
                    float num = std::fabs(10.f - float_dist(t, at));
                    if (float_dist(t, at) < 20.f && x.room.visual_contact(room_view::middle_of_tile(t), room_view::middle_of_tile(at)))
                        num -= 1000.f;
                    return num;
                };
                if (score(test) < score(s.unstuck_goal))
                    s.unstuck_goal = test;
            }
            int32_t close = 0;
            if (!satisfied && custom::manhattan_distance(at, x.destination()) > 3) {
                s.head = (s.head + stuck_tracker::tracked - 1) % stuck_tracker::tracked;
                s.past[s.head] = at;
                s.count = std::min(s.count + 1, stuck_tracker::tracked);
                for (int32_t j = 40; j < s.count; ++j)
                    if (float_dist(at, s.past[(s.head + j) % stuck_tracker::tracked]) < 4.f)
                        ++close;
            }
            s.counter = custom::int_clamp(close > 30 ? s.counter + 1 : s.counter - 2, 0, 200);
        }
        float stuck_utility(ctx& x) { return mathf::inverse_lerp(100.f, 200.f, float(x.ai.stuck.counter)); }

        // ChildTrackerModule.GetChildToCarry: a grub of the room it tracks,
        // that nothing holds
        dcon::creature_id child_to_carry(ctx& x) {
            auto ok = [&](dcon::creature_id t) {
                return t && creature::is(x.state.world, t, creature::type::moth_grub) && creature::here(x.state, t, x.room.id) && !creature::held(x.state, t)
                    && tracker::find(tracker::of(x.state, x.c), t) != nullptr;
            };
            if (!ok(x.ai.picked_child)) {
                x.ai.picked_child = dcon::creature_id{};
                auto const& ts = tracker::of(x.state, x.c);
                dcon::creature_id kids[creature::max_tracked];
                int32_t n = 0;
                for (uint32_t i = 0; i < ts.count; ++i)
                    if (ok(ts.reps[i].who))
                        kids[n++] = ts.reps[i].who;
                if (n > 0)
                    x.ai.picked_child = kids[x.rng.range(0, n)];
            }
            return x.ai.picked_child;
        }

        //
        // BigMoth: the tongue and the grasp
        //
        void release_tongue(ctx& x) {
            x.b.drink = dcon::creature_id{};
            x.b.drinking = 0;
            x.b.eat_counter = 0;
            x.b.meat_taken = 0;
        }
        // BigMoth.DrinkChunk
        void drink_chunk(ctx& x) {
            x.b.drinking = 1;
            x.b.tongue_hold = 1.f;
            x.state.play_sound(creature::is_slugcat(x.state.world, x.b.drink) ? sound::sound_id::leech_attatch_player : sound::sound_id::leech_attatch_npc,
                x.room.id, x.pos(0), 2.f, 1.f);
        }
        bool drink_valid(ctx& x) {
            return x.b.drink && creature::here(x.state, x.b.drink, x.room.id) && x.b.drink_chunk < creature::chunk_count_of(x.state, x.b.drink);
        }
        dcon::body_chunk_id drink_body_chunk(ctx& x) { return creature::chunk_of(x.state, x.b.drink, x.b.drink_chunk); }

        // BigMoth.CritGrabbedToMe
        bool crit_grabbed_to_me(ctx& x, dcon::creature_id t) { return creature::holds(x.state.world, t, x.c); }

        // MothLeg.TryGrab
        bool leg_try_grab(ctx& x, leg& l, dcon::creature_id t) {
            if (l.grabbed_chunk >= 0 && l.grabbed == t)
                return true;
            uint32_t const n = creature::chunk_count_of(x.state, t);
            for (uint32_t i = 0; i < n; ++i) {
                auto const k = creature::chunk_of(x.state, t, i);
                vec2 const cp = x.state.world.body_chunk_get_pos(k);
                if (rw::distance(closest_point_on_segment(l.connection_pos, l.foot_pos, cp), cp) < x.state.world.body_chunk_get_rad(k) + 10.f) {
                    l.grabbed = t;
                    l.grabbed_chunk = int8_t(i);
                    return true;
                }
            }
            return false;
        }

        // BigMothAI.TryGrab
        void try_grab(ctx& x, dcon::creature_id t, bool front_legs_only, bool force_others_to_let_go = false, bool force_all_grab_on_grip = false) {
            if (x.ai.grab_cool_down > 0)
                return;
            if (!t || t == x.c || !creature::here(x.state, t, x.room.id) || crit_grabbed_to_me(x, t))
                return;
            bool flag = false;
            for (uint32_t i = 0; i < 2; ++i)
                for (uint32_t j = 0; j < 2; ++j) {
                    auto& l = x.b.legs[i][j];
                    if (front_legs_only && i == 0) {
                        if (x.rng.value() < 0.1f)
                            l.grabbed_chunk = -1;
                    } else if (l.grabbed_chunk < 0 && leg_try_grab(x, l, t) && x.held() != t) {
                        if (force_others_to_let_go)
                            creature::for_each_holder(x.state, x.room.id, t, [&](dcon::creature_id h, uint32_t g) {
                                if (h != x.c)
                                    creature::release_grasp(x.state, h, g);
                            });
                        flag = true;
                        x.b.hold_through_wall_counter = 0;
                        creature::grab(x.state, x.c, 0, t, uint32_t(l.grabbed_chunk));
                        // (Moth_Grab has no sample here)
                    }
                }
            if (force_all_grab_on_grip && x.held() == t && flag)
                for (auto& pr : x.b.legs)
                    for (auto& l : pr) {
                        l.grabbed = t;
                        l.grabbed_chunk = int8_t(creature::grasp_chunk(x.state.world, x.c, 0));
                    }
        }

        // BigMothAI.ThrowGrabbed
        void throw_grabbed(ctx& x, float drop_speed) {
            auto const t = x.held();
            if (!t) {
                creature::release_grasp(x.state, x.c, 0);
                return;
            }
            auto& w = x.state.world;
            if (drop_speed > 0.f) {
                uint32_t const n = creature::chunk_count_of(x.state, t);
                for (uint32_t i = 0; i < n; ++i) {
                    vec2& v = w.body_chunk_get_vel(creature::chunk_of(x.state, t, i));
                    v = vec2{v.x, -drop_speed};
                }
                creature::for_each_holder(x.state, x.room.id, t, [&](dcon::creature_id h, uint32_t g) {
                    if (h != x.c && creature::is(w, h, creature::type::moth))
                        creature::release_grasp(x.state, h, g);
                });
            }
            if (tracker::find(tracker::of(x.state, x.c), t)) {
                auto* slot = &x.ai.thrown_away[0];
                for (auto& s : x.ai.thrown_away)
                    if (s.who == t || s.timer <= 0) {
                        slot = &s;
                        break;
                    }
                *slot = thrown{t, x.rng.range(200, 800)};
                tracker::forget(x.state, x.c, t);
            }
            x.state.play_sound(sound::sound_id::slugcat_throw_creature, x.room.id, x.pos(2));
            creature::release_grasp(x.state, x.c, 0);
            x.ai.grab_cool_down = 40;
        }

        // BigMoth.GrabUpdate (no Rope around corners: the straight distance)
        void grab_update(ctx& x) {
            auto& w = x.state.world;
            auto const t = x.held();
            if (!t || !creature::here(x.state, t, x.room.id)) {
                creature::release_grasp(x.state, x.c, 0);
                return;
            }
            if (w.creature_get_entering_shortcut(t).x >= 0 || crit_grabbed_to_me(x, t)) {
                creature::release_grasp(x.state, x.c, 0);
                return;
            }
            // (a held slugcat losing its footing, Player.standing, isn't done)
            uint32_t const gi = creature::grasp_chunk(w, x.c, 0);
            if (gi >= creature::chunk_count_of(x.state, t)) {
                creature::release_grasp(x.state, x.c, 0);
                return;
            }
            auto const gc = creature::chunk_of(x.state, t, gi);
            vec2& gp = w.body_chunk_get_pos(gc);
            vec2& gv = w.body_chunk_get_vel(gc);
            if (!x.room.visual_contact(gp, x.pos(2)) && !x.room.visual_contact(gp, x.pos(1))) {
                if (++x.b.hold_through_wall_counter > 70) {
                    creature::release_grasp(x.state, x.c, 0);
                    return;
                }
            } else {
                x.b.hold_through_wall_counter = std::max(x.b.hold_through_wall_counter - 1, 0);
            }
            float num = 0.f;
            for (auto const& pr : x.b.legs)
                for (auto const& l : pr)
                    if (l.grabbed_chunk >= 0)
                        num += 1.f;
            num /= 4.f;
            bool const grub = creature::is(w, t, creature::type::moth_grub);
            if (grub)
                num = mathf::min(num * 2.f, 1.f);
            float const num2 = w.body_chunk_get_mass(gc) * (grub ? 0.35f : 1.f);
            float const num3 = num2 / (x.mass(2) * (x.small() ? (x.room.pixel_height() < x.pos(0).y ? 2.f : 1.4f) : 1.f) + num2);
            vec2 const a = (gv - x.vel(2)) * 0.2f * num;
            x.vel(2) += a * num3;
            gv -= a * (1.f - num3);
            float const num4 = rw::distance(x.pos(2), gp);
            vec2 const vector = custom::dir_vec(x.pos(2), gp);
            float const num5 = 100.f * x.scale() * (x.small() ? 0.f : 1.f) * (grub ? 0.8f : 1.f);
            if (num4 > 170.f * x.scale() * (x.small() ? 3.f : 1.f)) {
                creature::release_grasp(x.state, x.c, 0);
                return;
            }
            if (num4 >= num5 || !small_wants_to_eat(x.state, t)) {
                float const dd = (num4 - num5) * 0.25f * num;
                x.pos(2) += vector * dd * num3;
                x.vel(2) += vector * dd * num3;
                gp -= vector * dd * (1.f - num3);
                gv -= vector * dd * (1.f - num3);
            }
        }

        //
        // MothLeg
        //
        void leg_set_ideal(leg& l, vec2 at) {
            l.ideal_step_pos = at;
            l.has_ideal = 1;
        }
        // MothLeg.TryPlaceFoot
        bool try_place_foot(ctx& x, leg& l, vec2 ideal) {
            l.allow_step_to_trigger_other = 1;
            if (auto const hit = scavenger::exact_terrain_ray_trace_pos(x.room, l.connection_pos, ideal)) {
                vec2 const a = *hit - custom::dir_vec(l.connection_pos, ideal) * 5.f;
                if (x.room.get_tile(a + vec2{0.f, -10.f}).solid()) {
                    l.stretch_counter = 0;
                    l.secured_foot = 0;
                    l.step_pos = *hit;
                    l.has_step = 1;
                    return true;
                }
            }
            return false;
        }
        // BigMoth.LegSecuredFoot
        void leg_secured_foot(ctx& x, uint32_t pair, uint32_t idx) {
            // (Moth_Step has no sample here)
            auto& l = x.b.legs[pair][idx];
            if (pair == 0 && l.allow_step_to_trigger_other) {
                float const num = l.foot_pos.x >= l.last_foot_pos.x ? 1.f : -1.f;
                leg_set_ideal(x.b.legs[1][idx],
                    vec2{l.foot_pos.x + l.step_radius * num * 2.f * (0.5f + mathf::max(x.b.run_speed - 0.5f, 0.f) * 0.5f), l.foot_pos.y});
            }
        }
        // MothLeg.StepMode
        void step_mode(ctx& x, uint32_t pair, uint32_t idx) {
            auto& l = x.b.legs[pair][idx];
            float const sc = x.scale();
            float num = 1.f;
            if (l.last_step_dist_to_new_step > 10.f * (0.5f + 0.5f * sc)) {
                l.step_progress_counter += 20.f * x.b.run_speed * (0.5f + 0.5f * sc);
                float const num2 = mathf::clamp01(l.step_progress_counter / l.last_step_dist_to_new_step);
                l.foot_pos.y += std::sin(num2 * mathf::pi) * 7.f * (pair == 0 ? 0.7f : 1.f) * (x.b.run_speed + mathf::pow(x.b.run_speed, 2.f)) * (0.5f + 0.5f * sc);
                num = 0.25f + 0.75f * num2;
            }
            l.foot_pos = custom::move_towards(l.foot_pos, l.step_pos, 24.f * num * x.b.run_speed * (0.5f + 0.5f * sc));
            if (rw::distance(l.foot_pos, l.step_pos) < 7.f * sc || l.secured_foot) {
                l.foot_pos = l.step_pos;
                if (!l.secured_foot) {
                    l.secured_foot = 1;
                    leg_secured_foot(x, pair, idx);
                    l.allow_step_to_trigger_other = 0;
                }
            }
            vec2 const vector = custom::dir_vec(l.connection_pos, l.step_pos);
            float const num3 = rw::distance(l.connection_pos, l.step_pos);
            if (num3 > l.max_reach * 0.85f) {
                if (++l.stretch_counter > 70) {
                    l.stretch_counter = 0;
                    try_place_foot(x, l, l.connection_pos + rw::normalized(vec2{vector.x * 0.5f, vector.y}) * num3);
                    return;
                }
            } else {
                l.stretch_counter = 0;
            }
            if (consious(x.state, x.c)) {
                l.last_aware_leg_extension = rw::distance(l.connection_pos, l.foot_pos);
                l.leg_tension = 1.f;
                return;
            }
            if (l.secured_foot && l.has_step) {
                l.leg_tension -= 0.03f * x.rng.value() * (0.5f + l.leg_tension * 0.5f);
                if (l.leg_tension * sc <= 0.01f) {
                    l.leg_tension = 0.f;
                    l.has_step = 0;
                    return;
                }
                if (num3 < l.last_aware_leg_extension) {
                    float const dd = 0.5f * l.leg_tension * sc;
                    x.vel(l.connected_chunk) *= 0.9f;
                    x.vel(l.connected_chunk) -= vector * (l.last_aware_leg_extension - num3) * dd;
                }
            } else {
                l.has_step = 0;
            }
        }
        // MothLeg.FreeMode
        void free_mode(ctx& x, leg& l) {
            l.stretch_counter = 0;
            float const num = rw::distance(l.connection_pos, l.foot_pos);
            vec2 const a = custom::dir_vec(l.connection_pos, l.foot_pos);
            l.foot_pos += a * (l.max_reach * 0.5f - num) * 0.3f / x.scale();
            l.foot_pos.y -= x.g() * x.room.gravity * 10.f;
        }
        // MothLeg.Update
        void leg_update(ctx& x, uint32_t pair, uint32_t idx) {
            auto& l = x.b.legs[pair][idx];
            l.last_connection_pos = l.connection_pos;
            l.last_foot_pos = l.foot_pos;
            vec2 const vector = custom::dir_vec(x.pos(2), x.pos(1));
            vec2 const a = custom::perpendicular_vector(vector);
            float const dd = 1.f - std::fabs(x.p.flip);
            bool const back = pair == 0;
            l.connection_pos = x.pos(l.connected_chunk) + (a * 5.f * (idx % 2 == 0 ? 1.f : -1.f) * dd + (back ? -vector * 5.f : vector * 5.f)) * x.scale();
            if (l.grabbed_chunk >= 0) {
                auto const t = x.held();
                if (!t || t != l.grabbed || uint32_t(l.grabbed_chunk) >= creature::chunk_count_of(x.state, t)) {
                    l.grabbed_chunk = -1;
                    l.grabbed = dcon::creature_id{};
                } else {
                    l.has_ideal = 0;
                    l.has_step = 0;
                    l.foot_pos = x.state.world.body_chunk_get_pos(creature::chunk_of(x.state, t, uint32_t(l.grabbed_chunk)));
                }
            }
            if (l.has_ideal) {
                vec2 const a2 = custom::dir_vec(l.connection_pos, l.ideal_step_pos);
                bool const had = l.has_step != 0;
                vec2 const prev = l.step_pos;
                if (try_place_foot(x, l, l.connection_pos + a2 * l.max_reach * (x.b.want_to_fly ? 0.7f : 1.f))) {
                    if (had) {
                        l.step_progress_counter = 0.f;
                        l.last_step_dist_to_new_step = rw::distance(prev, l.step_pos);
                    }
                    l.has_ideal = 0;
                } else {
                    float const num = l.ideal_step_pos.x > x.pos(2).x ? -1.f : 1.f;
                    l.ideal_step_pos += vec2{num * 2.f, 0.f};
                }
            }
            if (l.has_step && rw::distance(l.connection_pos, l.step_pos) > l.max_reach)
                l.has_step = 0;
            if (l.has_step)
                step_mode(x, pair, idx);
            else
                free_mode(x, l);
            l.foot_pos += x.vel(l.connected_chunk) * 0.3f;
            if (auto const hit = scavenger::exact_terrain_ray_trace_pos(x.room, l.connection_pos, l.foot_pos))
                l.foot_pos = *hit;
            float const num2 = rw::distance(l.connection_pos, l.foot_pos);
            vec2 const a3 = custom::dir_vec(l.connection_pos, l.foot_pos);
            if (num2 > l.max_reach) {
                l.foot_pos = l.connection_pos + a3 * l.max_reach;
                l.has_step = 0;
            }
        }
        //
        // MothWingPair.Update (the woosh sound loop isn't played)
        //
        void wing_update(ctx& x, uint32_t pi) {
            auto& wp = x.b.wings[pi];
            float const num2 = x.b.fly_upness * (pi == 0 ? 0.7f : 1.f) * (0.5f + x.b.run_speed) * (x.small() ? 3.f : 1.f);
            if (!x.b.want_to_fly)
                wp.mode = wing_mode::ready_flap;
            for (uint32_t i = 1; i < 3; ++i) {
                x.vel(i).y *= 0.97f + 0.03f * (1.f - std::sin(wp.flapness * mathf::pi));
                if (x.b.narrow_upcoming)
                    x.vel(i) *= 0.8f;
            }
            if (wp.mode == wing_mode::glide) {
                wp.flapness = mathf::lerp(wp.flapness, 0.5f, 0.05f);
                wp.flap_vel = 0.f;
            } else if (wp.mode == wing_mode::ready_flap) {
                wp.flap_vel += (x.small() ? 0.012f : 0.007f) * num2;
                if (wp.flapness >= 1.f) {
                    wp.flapness = 1.f;
                    wp.flap_vel = 0.f;
                    if (pi == 0)
                        wp.mode = wing_mode::flap;
                } else if (wp.flapness < 0.f) {
                    wp.flapness = 0.f;
                }
            } else {
                wp.flap_vel -= 0.01f * num2;
                vec2 const a = vec2{x.b.move_direction.x * std::fabs(wp.flap_vel) * 10.f, std::fabs(wp.flap_vel) * 40.f * (pi == 0 ? 0.7f : 1.f)}
                             * (1.f - mathf::max(x.b.run_speed - 0.5f, 0.f) * 0.3f) * (x.b.narrow_upcoming ? 3.f : 1.f);
                for (uint32_t j = 0; j < 3; ++j)
                    x.vel(j) += a * (j == 1 ? 1.5f : 0.25f) * (x.small() ? 0.5f : 1.f);
                // (Moth_Wingflaps has no sample here)
                if (wp.flapness <= 0.f) {
                    wp.flapness = 0.f;
                    wp.flap_vel = 0.f;
                    wp.mode = wing_mode::glide;
                    if (pi == 0 && x.b.wings[1].mode == wing_mode::ready_flap)
                        x.b.wings[1].mode = wing_mode::flap;
                }
            }
            if (!x.b.want_to_fly) {
                wp.flapness = mathf::lerp(wp.flapness, 0.f, 0.1f);
                wp.flap_vel = 0.f;
            }
            wp.flapness += wp.flap_vel;
        }

        // BigMoth.Fly
        void fly(ctx& x) {
            if (x.b.want_to_fly && x.b.wings[0].mode != wing_mode::flap)
                x.b.wings[0].mode = wing_mode::ready_flap;
            if (x.b.wings[0].mode != wing_mode::glide && x.b.wings[1].mode != wing_mode::flap)
                x.b.wings[1].mode = wing_mode::ready_flap;
        }

        // BigMoth.Walk
        void walk(ctx& x) {
            auto& w = x.state.world;
            for (auto& pr : x.b.legs)
                for (auto& l : pr) {
                    if (!l.has_step)
                        leg_set_ideal(l, l.connection_pos + rw::normalized(vec2{x.rng.value() * 2.f - 1.f, -1.f}) * l.max_reach);
                    else if (l.step_pos.y > x.pos(2).y && x.rng.value() < 0.1f)
                        l.has_step = 0;
                }
            int32_t num = 0;
            x.b.feet_avg_pos = vec2{0.f, 0.f};
            x.b.legs_on_ground = 0;
            for (auto const& pr : x.b.legs)
                for (auto const& l : pr)
                    if (l.has_step) {
                        if (rw::distance(l.connection_pos, l.step_pos) > l.max_reach * 0.8f)
                            ++num;
                        x.b.feet_avg_pos += l.step_pos;
                        ++x.b.legs_on_ground;
                    }
            if (x.b.legs_on_ground > 0)
                x.b.feet_avg_pos /= float(x.b.legs_on_ground);
            if (num + (4 - x.b.legs_on_ground) > 2 && !x.b.drinking)
                x.b.want_to_fly_counter = 7;
            auto& l00 = x.b.legs[0][0];
            auto& l01 = x.b.legs[0][1];
            if (l00.has_step && l01.has_step) {
                float const xx = x.pos(l00.connected_chunk).x;
                int32_t const num2 = l00.step_pos.x > l01.step_pos.x ? 1 : 0;
                int32_t const num3 = num2 == 0 ? 1 : 0;
                float num4 = 0.f;
                int32_t num5 = -1;
                if (x.b.legs[0][num2].step_pos.x > xx) {
                    num4 = -1.f;
                    num5 = num3;
                } else if (x.b.legs[0][num3].step_pos.x < xx) {
                    num4 = 1.f;
                    num5 = num2;
                }
                if (num5 >= 0)
                    leg_set_ideal(x.b.legs[0][num5],
                        vec2{xx + num4 * l00.step_radius * (0.5f + mathf::max(x.b.run_speed - 0.5f, 0.f) * 0.5f), x.b.feet_avg_pos.y});
            }
            if (x.b.legs_on_ground >= 2) {
                auto const o = body(x.state, x.c);
                for (uint32_t m = 0; m < chunk_count; ++m) {
                    float num6 = 1.5f;
                    if (m == 3)
                        num6 *= 0.7f;
                    else if (m == 4)
                        num6 *= 0.4f;
                    x.vel(m).y += (w.physical_object_get_g(o) - w.physical_object_get_buoyancy(o) * physics::submersion(x.state, x.room, x.ch(m))) * num6;
                }
                for (uint32_t n = 1; n < 3; ++n) {
                    vec2 const mv = x.b.move_direction * 0.8f * x.b.run_speed * (0.5f + 0.5f * x.scale());
                    x.vel(n) *= 0.8f;
                    x.pos(n) += mv;
                    x.vel(n) += mv * ((x.b.narrow_upcoming && n == 1) ? 5.f : 1.f);
                }
                return;
            }
            x.b.want_to_fly_counter = 14;
        }

        // PhysicalObject.WeightedPush
        void weighted_push(ctx& x, uint32_t a, uint32_t b, vec2 dir, float frc) {
            float const num = x.mass(b) / (x.mass(a) + x.mass(b));
            x.vel(a) += dir * frc * num;
            x.vel(b) -= dir * frc * (1.f - num);
        }

        //
        // BigMothAI scores
        //
        // BigMothAI.IdleScore (no NoiseTracker sources)
        float idle_score(ctx& x, ivec2 t) {
            if (t.x < 0 || !x.reachable(t))
                return float_max;
            auto const& ts = tracker::of(x.state, x.c);
            int32_t const sac = tracker_params().see_around_corners;
            float num = 1.f;
            if (x.reachable(t + ivec2{0, -1}))
                num += 10.f;
            if (pathing::narrow_space(x.room, t))
                num += 50.f;
            if (pathing::floor_altitude(x.room, t) > (x.small() ? 1 : 5)) {
                bool flag = false;
                if (!x.small()) {
                    // (sic) vanilla looks at x -3 and 3, not around the tile
                    for (int32_t i = 0; i < 2; ++i) {
                        ivec2 const p = clamp_to_room(x.room, ivec2{i == 0 ? -3 : 3, t.y});
                        if (!any_solid(x.room, p) && pathing::floor_altitude(x.room, p) < 5) {
                            flag = true;
                            break;
                        }
                    }
                }
                if (!flag)
                    num += 100.f;
            }
            num += tracker::threat_of_tile(ts, t, sac) * 1000.f;
            num += tracker::threat_of_tile(ts, clamp_to_room(x.room, tile_of((room_view::middle_of_tile(t) + room_view::middle_of_tile(x.at())) / 2.f)), sac) * 1000.f;
            return num;
        }
        // BigMothAI.CliffScore: (score, the open height over the bottom row)
        ivec2 cliff_score(ctx& x, ivec2 coord) {
            if (coord.x < 0)
                return ivec2{int_max, 0};
            coord.y = 0;
            if (!x.reachable(coord))
                return ivec2{int_max, 0};
            ivec2 const iv{coord.x, 0};
            int32_t num = std::abs(iv.x - tile_of(x.pos(2)).x);
            for (int32_t i = 1; i < 10; ++i)
                if (any_solid(x.room, iv + ivec2{i, 0}) || any_solid(x.room, iv - ivec2{i, 0})) {
                    num += (9 - i) * 5;
                    break;
                }
            int32_t num2 = 0;
            for (int32_t num3 = 1; num3 < x.room.height - 7 && !any_solid(x.room, iv + ivec2{0, num3}) && !any_solid(x.room, iv + ivec2{0, num3 + 5}); ++num3)
                num2 = num3;
            num += x.room.height - num2;
            return ivec2{num, num2};
        }
        // BigMothAI.SafeBabyPlaceScore
        float safe_baby_place_score(ctx& x, ivec2 t) {
            if (t.x < 0 || !x.reachable(t))
                return float_max;
            float num = 1.f;
            if (x.room.get_tile(t + ivec2{0, -2}).solid()) {
                for (int32_t i = 1; i < 8; ++i)
                    if (!x.room.get_tile(t + ivec2{i, -2}).solid() || !x.room.get_tile(t + ivec2{-i, -2}).solid())
                        return num + float((7 - i) * 5);
                return num;
            }
            return float_max;
        }
        ivec2 random_tile(ctx& x) { return ivec2{x.rng.range(0, x.room.width), x.rng.range(0, x.room.height)}; }

        // the open column over the room nearest `from_x` (the top tiles
        // open): where it carries prey up through (the sky over the room)
        int32_t sky_column(room_view const& room, int32_t from_x) {
            int32_t const y = room.height - 1;
            auto const open = [&](int32_t xx) {
                for (int32_t k = 0; k < 4; ++k)
                    if (room.get_tile(xx, y - k).solid())
                        return false;
                return true;
            };
            for (int32_t dd = 0; dd < room.width; ++dd) {
                int32_t const a = from_x - dd, b = from_x + dd;
                if (a >= 0 && a < room.width && open(a))
                    return a;
                if (b >= 0 && b < room.width && open(b))
                    return b;
            }
            return -1;
        }

        //
        // BigMothAI.Update
        //
        void ai_update(ctx& x) {
            auto& w = x.state.world;
            auto& ai = x.ai;
            if (ai.focus)
                ai.last_focus = ai.focus;
            ++ai.time_in_room;
            for (auto& s : ai.thrown_away)
                if (s.timer > 0 && --s.timer == 0)
                    s.who = dcon::creature_id{};
            ivec2 const at = x.at();
            tracker::hooks const h = make_hooks(x);
            tracker::update(x.state, x.c, x.room, at, tracker_params(), h);
            looker_update(x);
            bool const close_to_dest = x.destination() != ivec2{-1, -1} && x.beh() != behavior::get_unstuck
                && rw::distance(room_view::middle_of_tile(x.destination()), x.pos(2)) < 95.f;
            stuck_update(x, close_to_dest);
            ai.grab_cool_down = std::max(ai.grab_cool_down - 1, 0);
            auto const& ts = tracker::of(x.state, x.c);
            int32_t const sac = tracker_params().see_around_corners;
            // the UtilityComparer: threat (smoothed, its weight the threat
            // of its tile), rain, prey (0.4), stuck (none close to its goal)
            float const prey_raw = tracker::prey_utility(x.state, x.c, x.room, at, tracker_params());
            float threat_weight = custom::lerp_map(tracker::threat_of_tile(ts, at, sac), 0.1f, 2.f, 0.1f, 1.f, 0.5f);
            if (x.small()) {
                threat_weight = prey_raw > 0.2f ? 0.3f : 1.f;
                for (auto& s : ai.memory)
                    if (s.who)
                        s.temp_like = custom::lerp_and_tick(s.temp_like, mathf::max(s.temp_like, 0.f), 0.001f, 0.001f);
            }
            float const threat_now = mathf::pow(mathf::max(tracker::threat_utility(ts), 0.f), 1.1f) * threat_weight;
            if (threat_now > ai.threat_smoothed)
                ai.threat_smoothed = mathf::lerp(ai.threat_smoothed, threat_now, 0.5f);
            else
                ai.threat_smoothed = mathf::max(threat_now, ai.threat_smoothed - 0.005f);
            float const u_threat = ai.threat_smoothed;
            float const u_rain = mathf::pow(mathf::max(dens::rain_utility(x.state, x.c, x.room.id), 0.f), 1.1f);
            float const u_prey = mathf::pow(mathf::max(prey_raw, 0.f), 1.1f) * 0.4f;
            float const u_stuck = mathf::pow(stuck_utility(x), 1.1f) * (close_to_dest ? 0.f : 1.f);
            behavior b = x.beh();
            float current = 0.f;
            int32_t best = -1;
            float const us[4] = {u_threat, u_rain, u_prey, u_stuck};
            for (int32_t i = 0; i < 4; ++i)
                if (us[i] > current) {
                    current = us[i];
                    best = i;
                }
            auto const* threat = tracker::threat(ts);
            if (best == 0) {
                if (threat && threat->rel.type != rel::afraid) {
                    // (BigMothState.hatchedCycle is always < 0 here: a slugcat
                    // near its grubs is attacked too)
                    if (!x.small() && (child_to_carry(x) || threat->rel.type == rel::attacks))
                        b = behavior::attack;
                    else if (x.small() && threat->rel.type == rel::antagonizes)
                        b = behavior::stalk;
                    else
                        b = behavior::idle;
                } else {
                    b = behavior::flee;
                }
            } else if (best == 1) {
                b = behavior::escape_rain;
            } else if (best == 2) {
                b = behavior::eat;
            } else if (best == 3) {
                b = behavior::get_unstuck;
            }
            w.creature_set_current_utility(x.c, current);
            if (current < 0.02f && b != behavior::attack && b != behavior::eat)
                b = behavior::idle;
            // the focus a behaviour needs, gone: idle
            if ((b == behavior::attack || b == behavior::stalk) && !threat)
                b = behavior::idle;
            if (b == behavior::eat && !tracker::prey(ts))
                b = behavior::idle;
            if (!x.b.drinking)
                x.b.drink = dcon::creature_id{};
            auto const held = x.held();
            bool const holds_grub = held && creature::is(w, held, creature::type::moth_grub);
            bool flag = false;
            if (!x.small() && b == behavior::escape_rain) {
                if (child_to_carry(x) && !holds_grub) {
                    b = behavior::idle;
                    flag = true;
                }
            } else {
                if (held && x.small())
                    b = behavior::stow_food;
                if (!ai.trying_to_leave_room && ai.time_in_room < ai.time_to_tire && holds_grub)
                    b = behavior::set_down_baby;
                auto const* prey = tracker::prey(ts);
                bool const prey_dead = prey && creature::dead(x.state, prey->who);
                if (!x.small() && x.b.food_stored > 0 && child_to_carry(x) && (b == behavior::idle || (b == behavior::eat && prey_dead)))
                    b = behavior::feed_baby;
                else if (x.b.drink && creature::is(w, x.b.drink, creature::type::moth_grub))
                    release_tongue(x);
            }
            bool flag2 = false;
            if (b == behavior::idle && ai.last_focus && creature::here(x.state, ai.last_focus, x.room.id) && tracker::find(ts, ai.last_focus)) {
                relationship const dr = tracker::dynamic_relationship(x.state, x.c, ai.last_focus);
                if (dr.type != rel::ignores && custom::dist_less(x.pos(0), creature::main_pos(x.state, ai.last_focus), 200.f)) {
                    b = dr.type == rel::eats ? behavior::eat : behavior::attack;
                    ai.focus = ai.last_focus;
                    flag2 = true;
                }
            }
            x.set_beh(b);
            auto const focus_rep = [&]() { return ai.focus ? tracker::find(ts, ai.focus) : nullptr; };
            switch (b) {
            case behavior::idle: {
                x.b.run_speed = custom::lerp_and_tick(x.b.run_speed, 0.5f, 0.01f, 0.1f);
                ivec2 const coord = random_tile(x);
                if (idle_score(x, coord) < idle_score(x, ai.temp_idle_pos))
                    ai.temp_idle_pos = coord;
                bool flag3 = false;
                bool flag4 = false;
                if (held) {
                    if (holds_grub) {
                        vec2 const gp = w.body_chunk_get_pos(creature::chunk_of(x.state, held, std::min<uint32_t>(2, creature::chunk_count_of(x.state, held) - 1)));
                        try_grab(x, held, x.room.get_tile(gp + vec2{0.f, -20.f}).solid() || x.room.get_tile(gp + vec2{0.f, -40.f}).solid());
                        flag4 = true;
                    } else {
                        throw_grabbed(x, 0.f);
                    }
                }
                if (flag)
                    x.b.run_speed = 1.f;
                if (ai.time_in_room > ai.time_to_tire || flag) {
                    if (!x.held() && !x.small()) {
                        if (auto const kid = child_to_carry(x)) {
                            flag3 = true;
                            if (auto const* kr = tracker::find(ts, kid))
                                x.set_destination(tracker::best_guess(*kr));
                            try_grab(x, kid, false, true);
                        }
                    }
                    // (the offscreen den: a trip to another room here)
                    if (!flag3) {
                        if (ai.idle_pos_counter <= 0) {
                            ai.time_in_room = 0;
                            ai.trying_to_leave_room = 0;
                        } else if (!ai.trying_to_leave_room) {
                            ai.idle_pos_counter = x.rng.range(500, 1000);
                            ai.time_to_tire = x.rng.range(400, 1000);
                            ai.time_in_room = std::max(ai.time_to_tire + 1, ai.time_in_room);
                            ai.trying_to_leave_room = 1;
                            if (!migrate::want_to(x.state, x.c))
                                migrate::random_move(x.state, x.c, x.room, 1);
                        }
                        if (ai.trying_to_leave_room) {
                            if (auto const e = migrate::travel_tile(x.state, x.c, x.room)) {
                                x.set_destination(*e);
                            } else {
                                ai.time_in_room = 0;
                                ai.trying_to_leave_room = 0;
                            }
                        }
                    }
                } else if (idle_score(x, ai.temp_idle_pos) < idle_score(x, x.destination()) + custom::lerp_map(float(ai.idle_pos_counter), 0.f, 300.f, 100.f, -300.f)) {
                    x.set_destination(ai.temp_idle_pos);
                    ai.idle_pos_counter = x.rng.range(200, 1000);
                    ai.temp_idle_pos = random_tile(x);
                }
                if (!flag3)
                    ai.idle_pos_counter -= (flag4 && x.rng.value() < 0.4f) ? 0 : 1;
                break;
            }
            case behavior::flee:
                --ai.time_to_tire;
                x.b.run_speed = custom::lerp_and_tick(x.b.run_speed, 1.f, 0.01f, 0.1f);
                if (threat)
                    ai.focus = threat->who;
                x.set_destination(tracker::flee_to(x.state, x.c, x.room, at, 10, 30, tracker_params(), h));
                break;
            case behavior::eat: {
                if (!flag2)
                    ai.focus = ts.prey;
                ai.time_in_room = 0;
                auto const* fr = focus_rep();
                if (!fr)
                    break;
                float y = 250.f * x.scale();
                auto const hit = scavenger::exact_terrain_ray_trace_pos(x.room, ai.drink_spot, ai.drink_spot - vec2{0.f, y});
                auto const t = fr->who;
                bool const there = creature::here(x.state, t, x.room.id);
                if (there) {
                    try_grab(x, t, hit.has_value());
                    if (x.held())
                        ai.drink_spot = w.body_chunk_get_pos(creature::chunk_of(x.state, x.held(), creature::grasp_chunk(w, x.c, 0)));
                } else if (x.held() && x.held() == t && x.rng.value() < 0.05f) {
                    creature::release_grasp(x.state, x.c, 0);
                }
                if (x.b.drinking) {
                    x.b.run_speed = custom::lerp_and_tick(x.b.run_speed, 0.3f, 0.005f, 0.05f);
                    if (hit)
                        y = std::fabs(hit->y - ai.drink_spot.y) * 0.9f;
                    x.set_destination(tile_of(ai.drink_spot - vec2{0.f, y}));
                } else {
                    x.b.run_speed = custom::lerp_and_tick(x.b.run_speed, 0.7f, 0.01f, 0.1f);
                    x.set_destination(tracker::best_guess(*fr));
                    if (!x.b.drink && there) {
                        vec2 const p0 = x.pos(0);
                        uint32_t const n = creature::chunk_count_of(x.state, t);
                        uint32_t num = 0;
                        float num2 = rw::distance(p0, w.body_chunk_get_pos(creature::chunk_of(x.state, t, 0)));
                        for (uint32_t j = 1; j < n; ++j) {
                            float const num3 = rw::distance(p0, w.body_chunk_get_pos(creature::chunk_of(x.state, t, j)));
                            if (num3 < num2) {
                                num = j;
                                num2 = num3;
                            }
                        }
                        auto const k = creature::chunk_of(x.state, t, num);
                        if (visual_contact(x, w.body_chunk_get_pos(k)) && num2 < 400.f * x.scale() + w.body_chunk_get_rad(k)) {
                            ai.drink_spot = x.pos(2);
                            x.b.drink = t;
                            x.b.drink_chunk = uint8_t(num);
                            x.set_destination(tile_of(w.body_chunk_get_pos(k) + vec2{0.f, 80.f * x.scale()}));
                        }
                    }
                }
                break;
            }
            case behavior::attack: {
                x.b.run_speed = custom::lerp_and_tick(x.b.run_speed, 1.f, 0.01f, 0.1f);
                if (!flag2 && threat)
                    ai.focus = threat->who;
                x.b.shake = mathf::max(x.b.shake, 0.4f * x.iv.aggression);
                if (x.rng.value() < 0.03f * (x.iv.dominance * 0.5f + 0.5f))
                    x.b.shake = 0.2f * x.iv.dominance + x.rng.value() * 0.8f;
                auto const* fr = focus_rep();
                if (!fr)
                    break;
                if (creature::here(x.state, fr->who, x.room.id))
                    try_grab(x, fr->who, false);
                if (!x.held()) {
                    x.set_destination(tracker::best_guess(*fr));
                } else if (ai.give_up_on_cliff_timer > 0) {
                    ivec2 const coord2{x.rng.range(0, x.room.width), 0};
                    ivec2 iv = cliff_score(x, ai.temp_cliff_pos);
                    ivec2 const iv2 = cliff_score(x, coord2);
                    if (iv2.x < iv.x) {
                        ai.temp_cliff_pos = ivec2{coord2.x, iv2.y};
                        iv = iv2;
                    }
                    if (iv.x == int_max) {
                        --ai.give_up_on_cliff_timer;
                        x.set_destination(tile_of(x.pos(2) + vec2{0.f, 100.f}));
                    } else {
                        vec2 const v2 = room_view::middle_of_tile(ivec2{ai.temp_cliff_pos.x, 0});
                        if (x.room.visual_contact(x.pos(2) + vec2{20.f, 0.f}, v2) && x.room.visual_contact(x.pos(2) - vec2{20.f, 0.f}, v2)
                        && std::fabs(v2.x - x.pos(2).x) < 60.f && x.rng.value() < 0.03f)
                            throw_grabbed(x, 0.f);
                        x.set_destination(ai.temp_cliff_pos);
                    }
                    if (ai.temp_cliff_pos.x < 0)
                        --ai.give_up_on_cliff_timer;
                } else if (!x.small()) {
                    // (the offscreen den over the sky exits: up over the
                    // room, where it lets go of it from high up)
                    int32_t const col = sky_column(x.room, custom::int_clamp(at.x, 0, x.room.width - 1));
                    if (col >= 0) {
                        if (x.pos(0).y + mathf::pow(x.rng.value(), 2.f) * 100.f > x.room.pixel_height() - 60.f) {
                            x.set_destination(tile_of(x.pos(0)));
                            throw_grabbed(x, 44.f + x.rng.value() * 12.f);
                        } else {
                            x.set_destination(ivec2{col, x.room.height - 1});
                        }
                    } else if (auto const e = dens::home_tile(x.state, x.c, x.room)) {
                        x.set_destination(*e);
                    } else {
                        throw_grabbed(x, 0.f);
                    }
                } else {
                    throw_grabbed(x, 0.f);
                }
                break;
            }
            case behavior::stalk: {
                if (!flag2 && threat)
                    ai.focus = threat->who;
                auto const* fr = focus_rep();
                if (!fr)
                    break;
                ivec2 const wc = tracker::best_guess(*fr);
                // (what it carries that a small moth would eat: no rats here)
                if (creature::here(x.state, fr->who, x.room.id) && custom::manhattan_distance(at, wc) < 10) {
                    x.b.run_speed = custom::lerp_and_tick(x.b.run_speed, 1.f, 0.01f, 0.1f);
                    x.set_destination(tracker::flee_to(x.state, x.c, x.room, at, 10, 30, tracker_params(), h));
                } else {
                    x.b.run_speed = custom::lerp_and_tick(x.b.run_speed, 0.7f, 0.01f, 0.1f);
                    x.set_destination(wc);
                }
                break;
            }
            case behavior::stow_food:
                x.b.run_speed = custom::lerp_and_tick(x.b.run_speed, 1.f, 0.01f, 0.1f);
                if (auto const e = dens::home_tile(x.state, x.c, x.room))
                    x.set_destination(*e);
                break;
            case behavior::set_down_baby: {
                x.b.run_speed = custom::lerp_and_tick(x.b.run_speed, 0.6f, 0.01f, 0.1f);
                ivec2 const coord3 = random_tile(x);
                if (safe_baby_place_score(x, coord3) < safe_baby_place_score(x, ai.temp_safe_baby_pos))
                    ai.temp_safe_baby_pos = coord3;
                if (ai.temp_safe_baby_pos.x >= 0) {
                    vec2 const bb = room_view::middle_of_tile(ai.temp_safe_baby_pos) + vec2{0.f, 20.f};
                    if (x.room.visual_contact(x.pos(2), bb) && custom::dist_less(x.pos(2), bb, 50.f) && x.rng.value() < 0.08f)
                        throw_grabbed(x, 0.f);
                    x.set_destination(ai.temp_safe_baby_pos + ivec2{0, 1});
                }
                break;
            }
            case behavior::feed_baby: {
                x.b.run_speed = custom::lerp_and_tick(x.b.run_speed, 0.4f, 0.1f, 0.1f);
                auto const kid = child_to_carry(x);
                if (!kid)
                    break;
                if (x.b.drink && (x.b.drink != kid || x.b.drink_chunk != 0))
                    release_tongue(x);
                vec2 const kp = w.body_chunk_get_pos(creature::chunk_of(x.state, kid, 0));
                if (visual_contact(x, kp) && custom::dist_less(x.pos(0), kp, 400.f * x.scale())) {
                    x.b.drink = kid;
                    x.b.drink_chunk = 0;
                }
                x.set_destination(tile_of(kp));
                break;
            }
            case behavior::escape_rain:
                x.b.run_speed = custom::lerp_and_tick(x.b.run_speed, 0.7f, 0.01f, 0.1f);
                if (auto const e = dens::home_tile(x.state, x.c, x.room))
                    x.set_destination(*e);
                if (x.held() && !creature::is(w, x.held(), creature::type::moth_grub))
                    throw_grabbed(x, 0.f);
                break;
            case behavior::get_unstuck:
                x.set_destination(ai.stuck.unstuck_goal);
                break;
            }
            ai.fear = custom::lerp_and_tick(ai.fear, mathf::max(u_threat, mathf::pow(tracker::panic(ts), 0.7f)), 0.07f, 0.033333335f);
        }

        // BigMothPather.FollowPath from `from`: the tile to go to (the
        // goal itself there: a flyer keeps steering about it)
        std::optional<ivec2> follow_path(ctx& x, ivec2 from) {
            from = clamp_to_room(x.room, from);
            if (!pathing::accessible(x.room, from, x.movement()))
                return std::nullopt;
            auto const f = x.path();
            if (auto const s = pathing::next_step(x.state, f, from))
                return s;
            if (pathing::distance(x.state, f, from) == 0)
                return from;
            return std::nullopt;
        }

        // BigMoth.Act
        void act(ctx& x) {
            auto& w = x.state.world;
            ai_update(x);
            float num = 1.f;
            vec2 pos = x.pos(2);
            if (x.b.narrow_upcoming && x.small())
                pos = x.pos(1);
            auto conn = follow_path(x, tile_of(pos));
            if (!conn)
                for (uint32_t i = 0; i < 4 && !conn; ++i)
                    conn = follow_path(x, tile_of(pos + rw::to_vec2(custom::four_directions[i]) * 20.f));
            // (pipes: dens.hpp squeezes it into the one it goes home or
            // travels through)
            if (conn) {
                vec2 vector = room_view::middle_of_tile(*conn);
                x.b.narrow_upcoming = 0;
                int32_t num2 = 1;
                ivec2 c2 = *conn;
                for (int32_t j = 0; j < 3; ++j) {
                    if (!x.b.narrow_upcoming
                    && ((x.room.get_tile(c2 + ivec2{0, 1}).solid() && x.room.get_tile(c2 + ivec2{0, -1}).solid())
                        || (x.room.get_tile(c2 + ivec2{1, 0}).solid() && x.room.get_tile(c2 + ivec2{-1, 0}).solid())))
                        x.b.narrow_upcoming = 1;
                    auto const nx = follow_path(x, c2);
                    if (!nx || *nx == c2 || !x.room.inside(nx->x, nx->y))
                        break;
                    c2 = *nx;
                    vector += room_view::middle_of_tile(c2);
                    ++num2;
                }
                vector /= float(num2);
                if (!x.small()) {
                    int32_t num3 = 0;
                    for (int32_t num4 = 0; num4 < 5 && !x.room.get_tile(vector + vec2{0.f, 20.f * float(1 + num4)}).solid(); ++num4)
                        ++num3;
                    num = float(num3 - 1) / 4.f;
                } else if (x.b.narrow_upcoming) {
                    num = 0.f;
                }
                vec2 vector2 = vector - pos;
                vec2 const dest_mid = room_view::middle_of_tile(x.destination());
                if (x.b.want_to_fly && x.room.visual_contact(pos, dest_mid))
                    vector2 = vector2 * 0.3f + rw::clamp_magnitude(dest_mid - pos, 20.f) * 0.7f;
                vector2 = rw::clamp_magnitude(vector2, 10.f) / 10.f;
                x.b.move_direction = custom::move_towards(x.b.move_direction, vector2, 0.1f);
                ivec2 const vt = tile_of(vector);
                if (!x.room.inside(vt.x, vt.y))
                    x.b.move_direction = vector2;
                if (x.b.fly_upness < vector2.y)
                    x.b.fly_upness = mathf::lerp(x.b.fly_upness, vector2.y, 0.5f);
                else
                    x.b.fly_upness = mathf::lerp(x.b.fly_upness, 0.15f, 0.015f);
                if (x.beh() == behavior::get_unstuck) {
                    float const dd = mathf::min(1.f - float(x.b.stuck_counter) / 70.f, 0.f) + 1.f;
                    for (uint32_t k = 1; k < 3; ++k) {
                        x.pos(k) += vector2 * dd;
                        x.vel(k) += vector2 * dd;
                    }
                    ++x.b.stuck_counter;
                    x.b.blink_counter = std::max(x.b.blink_counter, 5);
                } else {
                    x.b.stuck_counter = 0;
                }
            }
            walk(x);
            fly(x);
            if (x.b.random_look_counter == 0) {
                if (x.rng.value() < 0.02f) {
                    x.b.random_look_at_creature = x.rng.value() < 0.7f ? 1 : 0;
                    x.b.random_look_counter = x.rng.range(20, 400);
                    x.b.random_look_pos = vec2{x.room.pixel_width() * x.rng.value(), x.room.pixel_height() * x.rng.value()};
                }
            } else {
                --x.b.random_look_counter;
            }
            bool flag = x.beh() != behavior::idle;
            vec2 vector3 = x.pos(1) + x.b.move_direction * 300.f;
            if (x.b.random_look_counter > 0) {
                vector3 = x.b.random_look_pos;
                if (x.b.random_look_at_creature)
                    flag = true;
            }
            if (flag && x.ai.look_creature) {
                if (auto const* r = tracker::find(tracker::of(x.state, x.c), x.ai.look_creature)) {
                    if (r->visual && creature::here(x.state, r->who, x.room.id))
                        vector3 = creature::main_pos(x.state, r->who);
                    else
                        vector3 = room_view::middle_of_tile(tracker::best_guess(*r));
                }
            }
            if (drink_valid(x))
                vector3 = x.pos(1) + (w.body_chunk_get_pos(drink_body_chunk(x)) - x.pos(1)) * 10.f;
            vector3 = rw::lerp(vector3, x.pos(1), (x.pos(1).x - vector3.x) * x.p.flip > 0.f ? 0.5f : 0.f);
            x.b.look_pos = custom::move_towards(x.b.look_pos, vector3, mathf::max(mathf::min(rw::distance(x.b.look_pos, vector3) * 0.1f, 200.f), 5.f));
            num *= 1.f - mathf::max(x.b.run_speed - 0.5f, 0.f) * 0.8f;
            if (num > x.b.stance)
                x.b.stance = mathf::lerp(x.b.stance, num, 0.02f);
            else
                x.b.stance = mathf::lerp(x.b.stance, num, 0.08f);
            x.b.stance = mathf::clamp01(x.b.stance);
            x.b.flipness = mathf::lerp(x.b.flipness, x.b.move_direction.x, 0.02f * std::fabs(x.b.move_direction.x));
            x.b.flipness = mathf::clamp(x.b.flipness, -1.f, 1.f);
            if (x.b.flipness > 0.01f)
                x.b.flip_side = 1;
            else if (x.b.flipness < -0.01f)
                x.b.flip_side = -1;
            x.p.flip = mathf::clamp(x.b.flipness, x.b.flip_side == 1 ? 0.01f : -1.f, x.b.flip_side == -1 ? -0.01f : 1.f);
            vec2 vector4{0.f, 1.f};
            if (x.b.legs_on_ground > 0) {
                float const num5 = x.pos(2).x + x.p.flip * 57.f * (0.5f + 0.5f * x.scale());
                vector4 = custom::dir_vec(x.b.feet_avg_pos, vec2{num5, x.pos(2).y});
                float const num6 = mathf::clamp01(std::fabs(x.b.feet_avg_pos.x - num5) / (70.f * (0.5f + 0.5f * x.scale())));
                vector4 = rw::normalized(vec2{0.f, 1.f} * (1.f - num6) + vector4 * num6);
            }
            float const num7 = 0.6f * x.b.stance;
            vec2 const normalized = rw::normalized(vec2{(1.f - num7) * x.p.flip, num7 + (1.f - std::fabs(x.p.flip)) * 0.1f});
            x.b.ground_angle = lerp_angle(x.b.ground_angle, custom::vec_to_deg(vector4), 0.1f);
            vec2 const vector5 = custom::deg_to_vec(x.b.ground_angle + custom::vec_to_deg(normalized));
            vec2 const normalized2 = rw::normalized(vector5
                + rw::clamp_magnitude((x.b.look_pos - x.pos(1)) / 700.f, 1.f) * (0.7f + mathf::clamp01(rw::dot(vector5, custom::dir_vec(x.pos(1), x.b.look_pos))) * 2.f));
            weighted_push(x, 0, 1, normalized2, 3.f);
            weighted_push(x, 1, 2, vector5, 3.f);
            weighted_push(x, 2, 3, vector5, 2.f);
            weighted_push(x, 3, 4, vector5, 2.f);
            x.b.shake = custom::lerp_and_tick(x.b.shake, 0.f, 0.008f, 0.005f);
            if (x.b.shake > 0.01f)
                for (uint32_t l = 0; l < chunk_count; ++l)
                    x.vel(l) += inside_unit_circle(x.rng) * x.b.shake * 3.f * x.scale();
            // (the antennas' shake: moth_graphics' update reads `shake`)
        }

        // BigMothGraphics.Update's blink rolls (its chatter: the shake)
        void blink_update(ctx& x) {
            behavior const b = x.beh();
            if (x.b.blink_counter > 0) {
                --x.b.blink_counter;
            } else if (x.rng.value() < (b == behavior::attack ? 0.02f : 0.01f)) {
                // (Moth_Chatter_Loud, Moth_Clicking, Moth_Chatter have no sample here)
                if (b == behavior::attack) {
                    if (x.rng.value() < 0.8f)
                        x.b.shake = 1.f;
                } else if (b == behavior::eat) {
                    if (x.rng.value() < 0.3f)
                        x.b.shake = 0.7f;
                } else if (x.rng.value() < 0.4f) {
                    x.b.shake = 0.3f;
                }
                x.b.blink_counter = b == behavior::attack ? 20 : 10;
            }
            if (!consious(x.state, x.c))
                x.b.blink_counter = 10;
        }

        // TongueGraphics.Update's reach: out while it eats or has something
        // to drink, folded unless it has; the tip over the chunk 30 ticks
        // makes it drink (DrinkChunk)
        void tongue_reach(ctx& x) {
            float const segs = float((x.small() ? 6 : 12) - 1);
            x.b.tongue_outness = custom::lerp_and_tick(x.b.tongue_outness, (x.beh() == behavior::eat || x.b.drink) ? 1.f : 0.f, 0.008f, 0.001f);
            x.b.tongue_foldness = custom::lerp_and_tick(x.b.tongue_foldness, x.b.drink ? 0.f : 1.f, 0.008f, 0.001f);
            if (drink_valid(x)) {
                if (!x.b.drinking && x.b.tongue_outness > 0.5f && x.b.tongue_foldness < 0.9f) {
                    auto const k = drink_body_chunk(x);
                    float const reach = segs * 10.f * x.b.tongue_outness * x.scale() * (1.f - x.b.tongue_foldness);
                    bool const flag = custom::dist_less(x.pos(0), x.state.world.body_chunk_get_pos(k), reach + x.state.world.body_chunk_get_rad(k) + 10.f);
                    if (flag && x.b.tongue_over_counter >= 30)
                        drink_chunk(x);
                    x.b.tongue_over_counter = custom::int_clamp(x.b.tongue_over_counter + (flag ? 2 : -1), 0, 30);
                }
                if (x.b.drinking)
                    x.b.tongue_over_counter = 0;
            } else {
                x.b.tongue_over_counter = 0;
            }
        }

        // BigMoth.Update's drinking: the tongue pulls it and the moth
        // together, drinks the health of a live creature, the meat of a dead
        // one, feeds a grub
        void drink_update(ctx& x) {
            auto& w = x.state.world;
            if (x.b.drink && !drink_valid(x)) {
                release_tongue(x);
                return;
            }
            if (!x.b.drink || !x.b.drinking)
                return;
            auto const t = x.b.drink;
            // a slugcat taking it from it: a thief (SocialEvent Theft)
            creature::for_each_holder(x.state, x.room.id, t, [&](dcon::creature_id hh, uint32_t) {
                if (hh == x.c)
                    return;
                for (auto const k : x.ai.known_thieves)
                    if (k == hh)
                        return;
                // (BigMothAI.SocialEvent: Theft, against the moth itself)
                if (!x.small())
                    influence(x.ai, hh, -0.2f, -0.02f * mathf::lerp(1.f, 0.1f, 0.f));
                for (auto& k : x.ai.known_thieves)
                    if (!k || !creature::live(x.state, k)) {
                        k = hh;
                        break;
                    }
            });
            auto const k = drink_body_chunk(x);
            vec2& kp = w.body_chunk_get_pos(k);
            vec2& kv = w.body_chunk_get_vel(k);
            x.b.tongue_hold = mathf::max(x.b.tongue_hold - 0.02f, 0.f);
            float const num = rw::distance(x.pos(0), kp);
            if (num > 120.f * x.scale()) {
                vec2 const a = custom::dir_vec(x.pos(0), kp);
                float const num2 = (num - 120.f * x.scale()) * 0.2f;
                x.b.tongue_hold += num2 * 0.01f;
                float const num3 = w.body_chunk_get_mass(k) / (x.mass(0) + w.body_chunk_get_mass(k));
                x.pos(0) += a * num2 * num3;
                x.vel(0) += a * num2 * num3;
                kp -= a * num2 * (1.f - num3);
                kv -= a * num2 * (1.f - num3);
            }
            if (consious(x.state, x.c)) {
                if (creature::is(w, t, creature::type::moth_grub)) {
                    if (float(++x.b.eat_counter) > 200.f / x.scale()) {
                        x.b.food_stored = std::max(x.b.food_stored - 1, 0);
                        x.b.eat_counter = 0;
                        auto& gp = w.creature_get_payload(t).moth_grub;
                        gp.look_off_time = 20;
                        gp.look_off_way = rw::lerp(gp.look_off_way, custom::dir_vec(w.body_chunk_get_pos(creature::chunk_of(x.state, t, 0)), x.pos(0)), 0.25f);
                    }
                } else if (!creature::is_slugcat(w, t) && !w.creature_get_dead(t)) {
                    // (a HealthState: the slugcat's PlayerState isn't one)
                    w.creature_get_health(t) -= 0.075f * x.scale() / (16.f * mathf::max(1.f, creature::total_mass(x.state, x.c)));
                } else if (w.creature_get_dead(t) && x.b.meat_taken < meat_points(creature::template_of(x.state, t))) {
                    if (float(++x.b.eat_counter) > 200.f / x.scale()) {
                        ++x.b.food_stored;
                        ++x.b.meat_taken;
                        x.b.eat_counter = 0;
                    }
                }
            }
            if (x.b.tongue_hold > 1.f || !x.room.visual_contact(x.pos(0), kp))
                release_tongue(x);
        }

        void on_terrain_impact(void* u, dcon::physical_object_id, uint32_t local_chunk, ivec2, float, bool) {
            // BigMoth.TerrainImpact: the head blinks
            if (local_chunk == 0)
                static_cast<body_state*>(u)->blink_counter = std::max(static_cast<body_state*>(u)->blink_counter, 7);
        }

        // the moths of the room that saw `who` lately hear of what it did
        // to `victim` (BigMothAI.SocialEvent). `id`: 0 NonLethalAttack, 1
        // LethalAttack, 2 Killing
        void social_event(sys::game_state& state, dcon::room_id room, dcon::creature_id who, dcon::creature_id victim, int32_t id) {
            auto& w = state.world;
            rooms::for_each_creature(state, room, creature::type::moth, [&](dcon::creature_id m) {
                auto const* r = tracker::find(tracker::of(state, m), who);
                if (!r || r->ticks_since_seen > 80)
                    return;
                bool const small = w.creature_get_payload(m).moth.variant == variant::small;
                auto& ai = w.moth_data_get_ai(creature::moth_data(w, m));
                auto const& iv = w.moth_data_get_ivars(creature::moth_data(w, m));
                float const num = (small ? 1.f : 0.3f) * (1.f - iv.bravery);
                float num2 = id == 0 ? 0.025f * num : id == 1 ? 0.25f : 0.95f;
                if (small)
                    num2 *= 2.f;
                if (victim != m && !creature::is(w, victim, creature::type::moth_grub) && id != 2)
                    num2 /= 3.f;
                influence(ai, who, -num2, -num2 * 0.1f * (small ? 0.25f : 1.f));
            });
        }
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 at, variant v) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::moth);
        auto const d = creature::moth_data(w, c);
        auto rng = rw::random(state.seed, 0, uint32_t(c.value));
        int32_t seed = int32_t(rng.next() & 0x7FFFFFFFu);
        if (state.spawn_seed >= 0)
            seed = state.spawn_seed;
        bool const small = v == variant::small;
        // AbstractCreature.Personality (its own InitState(seed))
        ivars iv;
        {
            rw::unity_random r{seed};
            float sympathy = push_from_half(r.value(), 1.5f);
            float const energy = push_from_half(r.value(), 1.5f);
            float const bravery = push_from_half(r.value(), 1.5f);
            float const n0 = r.value();
            float const n1 = mathf::pow(r.value(), 0.25f);
            (void)n0;
            (void)n1;
            float const a0 = r.value();
            float const a1 = mathf::pow(r.value(), 0.25f);
            float aggression = mathf::lerp(a0, (energy + bravery) / 2.f * (1.f - sympathy), a1);
            float const d0 = r.value();
            float const d1 = mathf::pow(r.value(), 0.25f);
            iv.dominance = mathf::lerp(d0, (energy + bravery + aggression) / 3.f, d1);
            iv.aggression = push_from_half(aggression, 2.5f);
            iv.sympathy = sympathy;
            iv.bravery = bravery;
        }
        // BigMoth's constructor and GenerateIVars (InitState(seed); no
        // babyness: no hatching here)
        float scale = small ? 0.3f : 1.f;
        {
            rw::unity_random r{seed};
            auto const v2 = [&] { return mathf::pow(r.value(), 2.f); };
            float const p1 = v2();
            float const p2 = v2();
            scale *= 0.9f + ((iv.dominance - p1) * 0.6f + (1.f - p2) * 0.4f) * 0.2f;
            iv.antenna_length = std::max(int32_t(std::lround(float(r.range(4, 7)) * scale)), 3);
            iv.antenna_length = std::min<int32_t>(iv.antenna_length, int32_t(max_antenna_segments));
            iv.antenna_width = r.range(2.f, 7.f) * scale;
            vec3 color{1.f, 1.f, 1.f};
            auto const lerp3 = [](vec3 a, vec3 b, float t) { return a + (b - a) * mathf::clamp01(t); };
            vec3 const bb = lerp3(vec3{0.5f, 0.25f, 0.1f}, vec3{0.6f, 0.5f, 0.4f}, r.value());
            if (small) {
                float const t0 = v2();
                vec3 const grey = lerp3(vec3{0.35f, 0.35f, 0.35f}, vec3{0.433f, 0.433f, 0.433f}, t0);
                color = lerp3(grey, bb, v2() * 0.1f);
            } else {
                color = lerp3(color, bb, v2() * 0.17f);
            }
            float const dom_roll = v2();
            color = lerp3(color, vec3{1.f, 0.7f, 0.f}, (iv.dominance - dom_roll) * 0.1f);
            float const hue = r.value() * 0.17f;
            float const hue_t = r.value() * (small ? 0.2f : 0.4f);
            vec3 const sec = lerp3(lerp3(color, custom::hsl2rgb(hue, 1.f, 0.5f), hue_t), small ? vec3{1.f, 1.f, 1.f} : vec3{0.f, 0.f, 0.f}, small ? 0.7f : 0.3f);
            bool const tag_roll = r.value() < 0.1f;
            iv.body_color = color;
            iv.secondary_color = sec;
            iv.knee_colorness = r.value();
            float const ch0 = r.value();
            float const ch1 = r.value();
            iv.chub = mathf::lerp(ch0, 0.5f, ch1);
            iv.leg_puff = v2();
            int32_t const tagged = r.range(0, 4);
            (void)tagged;
            iv.tagged_leg = -1;
            // BigMothState.tagOwner: a GA or DG tag on a leg, one in ten in
            // Ancient Urban
            bool ancient_urban = false;
            if (auto const region = w.room_get_region_from_region_membership(room)) {
                auto const name = state.to_string_view(w.region_get_name(region));
                ancient_urban = name.size() == 4 && (name[0] | 0x20) == 'w' && (name[1] | 0x20) == 'a' && (name[2] | 0x20) == 'u' && (name[3] | 0x20) == 'a';
            }
            if (tag_roll && ancient_urban) {
                bool const ga = r.value() > 0.4f;
                // (TagOwner.GA tags leg 2; a DG tag keeps the rolled leg)
                iv.tagged_leg = ga ? 2 : tagged;
                iv.leg_tag_color = ga ? vec3{0.2f, 0.8196f, 0.1843f} : vec3{0.f, 0.2745f, 0.6549f};
            }
        }
        physics::object_desc desc;
        desc.gravity = 0.9f;
        desc.air_friction = 0.99f;
        desc.bounce = 0.1f;
        desc.surface_friction = 0.35f;
        desc.water_friction = 0.9f;
        desc.buoyancy = 1.2f;
        auto const o = physics::create_object(state, room, desc);
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
        // five chunks: the head, the thorax's two, the abdomen's two; laid
        // out a little apart along the body so the connections have a
        // direction to work in
        constexpr float num = 5.5f;
        float const num2 = scale * scale;
        float const rads[chunk_count] = {9.f, 13.f, 13.f, 13.f, 11.f};
        float const masses[chunk_count] = {0.05f, 0.3f, 0.3f, 0.2f, 0.15f};
        float const offs[chunk_count] = {20.f, 0.f, -20.f, -45.f, -70.f};
        dcon::body_chunk_id k[chunk_count];
        for (uint32_t i = 0; i < chunk_count; ++i) {
            k[i] = physics::add_chunk(state, o, at + vec2{offs[i] * scale * 0.5f, 0.f}, rads[i] * scale, num * masses[i] * num2);
            w.body_chunk_set_restrict_in_room_range(k[i], 2400.f);
        }
        using physics::connection_type;
        physics::add_connection(state, o, k[0], k[1], 20.f * scale, connection_type::pull, 1.f, -1.f);
        physics::add_connection(state, o, k[1], k[2], 20.f * scale, connection_type::normal, 1.f, -1.f);
        physics::add_connection(state, o, k[2], k[3], 30.f * scale, connection_type::normal, 1.f, -1.f);
        physics::add_connection(state, o, k[3], k[4], 30.f * scale, connection_type::normal, 1.f, -1.f);
        physics::add_connection(state, o, k[1], k[3], 50.f * scale, connection_type::push, 1.f, -1.f);
        physics::add_connection(state, o, k[2], k[4], 60.f * scale, connection_type::push, 1.f, -1.f);
        physics::set_go_through_floors(state, o, true);
        w.try_create_creature_body(c, o);
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_set_grasps(c, creature::grasp_pair{});
        w.creature_set_behavior(c, uint8_t(behavior::idle));
        w.creature_set_current_utility(c, 0.f);
        w.creature_set_destination(c, clamp_to_room(view, tile_of(at)));
        auto& p = w.creature_get_payload(c).moth;
        uint16_t const keep = p.data;
        p = creature::moth_payload{};
        p.data = keep;
        p.variant = v;
        p.scale = scale;
        p.flip = 0.01f;
        w.moth_data_set_seed(d, seed);
        w.moth_data_set_ivars(d, iv);
        body_state b;
        for (uint32_t j = 0; j < 2; ++j)
            for (uint32_t m = 0; m < 2; ++m) {
                auto& l = b.legs[j][m];
                // MothLeg's constructor
                if (j == 0) {
                    l.max_reach = 144.f * scale;
                    l.step_radius = 100.f * scale;
                    l.connected_chunk = 2;
                } else {
                    l.max_reach = 190.f * scale;
                    l.step_radius = 200.f * scale;
                    l.connected_chunk = 1;
                }
                l.connection_pos = w.body_chunk_get_pos(k[l.connected_chunk]);
                l.last_connection_pos = l.connection_pos;
                l.foot_pos = l.connection_pos;
                l.last_foot_pos = l.foot_pos;
            }
        for (auto& wp : b.wings)
            wp.mode = wing_mode::ready_flap;
        b.look_pos = at;
        w.moth_data_set_body(d, b);
        // BigMothAI's constructor and NewRoom
        ai_state ai;
        ai.time_to_tire = rng.range(400, 7000);
        ai.give_up_on_cliff_timer = rng.range(200, 400);
        w.moth_data_set_ai(d, ai);
        w.moth_data_set_graphics(d, graphics_state{});
        tracker::clear(state, c);
        moth_graphics::reset_positions(state, c);
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        creature::release_target(state, c);
        creature::erase(state, c);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        ctx x{state, c, room};
        // Creature.Update's HealthState: hurt, it may die or faint, else heals
        if (!w.creature_get_dead(c)) {
            float& health = w.creature_get_health(c);
            if (health < 0.f && x.rng.value() < -health && x.rng.value() < 1.f / float(consious(state, c) ? 80 : 800))
                creature::die(state, c);
            if (x.rng.value() < 0.033333335f && (x.rng.value() * 0.2f > health || x.rng.value() < -health))
                creature::stun(state, c, x.rng.range(1, x.rng.range(1, 27 - custom::int_clamp(int32_t(20.f * health), 0, 10))));
            if (health > 0.f && health < 1.f && x.rng.value() < 0.01f)
                health = mathf::min(1.f, health + 1.f / mathf::lerp(550.f, 70.f, health));
        }
        // BigMoth.Stun: stunned, it lets go
        if (w.creature_get_stun(c) > 0 && x.held())
            creature::release_grasp(state, c, 0);
        // Creature.Update + PhysicalObject.Update
        creature::stun_countdown(state, c);
        physics::update_object(state, room, body(state, c), on_terrain_impact, &x.b);
        drink_update(x);
        if (x.held())
            grab_update(x);
        if (x.b.want_to_fly_counter > 0)
            --x.b.want_to_fly_counter;
        x.b.want_to_fly = x.b.want_to_fly_counter > 0 ? 1 : 0;
        for (uint32_t k = 0; k < 2; ++k)
            for (uint32_t l = 0; l < 2; ++l)
                leg_update(x, k, l);
        for (uint32_t m = 0; m < 2; ++m)
            wing_update(x, m);
        blink_update(x);
        if (consious(state, c))
            act(x);
        else
            weighted_push(x, 0, 1, custom::dir_vec(x.pos(2), x.pos(1)), 3.f);
        tongue_reach(x);
    }

    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) {
        return !creature::holds(state.world, t, c) && !creature::holds(state.world, c, t);
    }

    void collide(sys::game_state& state, dcon::creature_id c, room_view const&, uint32_t i, dcon::creature_id t, uint32_t j) {
        auto& w = state.world;
        if (i >= chunk_count || !creature::live(state, t) || j >= creature::chunk_count_of(state, t))
            return;
        auto const mine = chunk(state, c, i);
        auto const theirs = creature::chunk_of(state, t, j);
        float const num = w.body_chunk_get_mass(theirs) / (w.body_chunk_get_mass(mine) + w.body_chunk_get_mass(theirs));
        vec2 const a = (w.body_chunk_get_vel(theirs) - w.body_chunk_get_vel(mine)) * 0.1f;
        w.body_chunk_get_vel(mine) += a * num;
        w.body_chunk_get_vel(theirs) -= a * (1.f - num);
        if (i == 0) {
            auto& b = w.moth_data_get_body(creature::moth_data(w, c));
            b.blink_counter = std::max(b.blink_counter, 7);
        }
    }

    void violence(sys::game_state& state, dcon::creature_id c, uint32_t k, vec2 momentum, float damage, float stun_bonus, creature::damage_type) {
        auto& w = state.world;
        auto rng = rw::random(state.seed, state.tick, rw::ids(uint32_t(c.value), k));
        if (k >= chunk_count)
            k = 0;
        auto const& pl = w.creature_get_payload(c).moth;
        auto const& tp = params_of[uint32_t(pl.variant) % variant_count];
        // BigMoth.Violence: the head takes double, the abdomen less
        float num = 1.f;
        if (k == 0)
            num = 2.f;
        else if (k == 3)
            num = 0.7f;
        else if (k == 4)
            num = 0.3f;
        damage *= num;
        stun_bonus *= num;
        bool const was_dead = w.creature_get_dead(c) != 0;
        // Creature.Violence (HealthState)
        auto const hit = chunk(state, c, k);
        w.body_chunk_get_vel(hit) += rw::clamp_magnitude(momentum / w.body_chunk_get_mass(hit), 10.f);
        float const dmg = damage / tp.damage_resistance;
        float st = (damage * 30.f + stun_bonus) / tp.stun_resistance;
        float& health = w.creature_get_health(c);
        st *= 1.5f + mathf::inverse_lerp(0.5f, 0.f, health) * rng.value();
        creature::stun(state, c, int32_t(st));
        health -= dmg;
        if (rng.value() < -health || health < -1.f || (health < 0.f && rng.value() < 0.33f) || dmg >= tp.instant_death)
            creature::die(state, c);
        // BigMoth.Stun: stunned, it lets go
        if (w.creature_get_stun(c) > 0)
            creature::lose_all_grasps(state, c);
        // SocialEventRecognizer (Violence carries no source here: the
        // closest slugcat of the room did it)
        auto const room = creature::room_of(state, c);
        dcon::creature_id who;
        float best = float_max;
        vec2 const hp = w.body_chunk_get_pos(hit);
        rooms::for_each_creature(state, room, creature::type::slugcat, [&](dcon::creature_id s) {
            if (!creature::here(state, s, room))
                return;
            float const dd = rw::distance(creature::main_pos(state, s), hp);
            if (dd < best) {
                best = dd;
                who = s;
            }
        });
        if (who && damage > 0.f)
            social_event(state, room, who, c, (!was_dead && w.creature_get_dead(c)) ? 2 : damage >= 0.5f ? 1 : 0);
    }

    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t) {
        auto& w = state.world;
        if (creature::grasped(w, c, 0) == t)
            creature::release_grasp(state, c, 0);
        auto& b = w.moth_data_get_body(creature::moth_data(w, c));
        if (b.drink == t) {
            b.drink = dcon::creature_id{};
            b.drinking = 0;
            b.eat_counter = 0;
            b.meat_taken = 0;
        }
        for (auto& pr : b.legs)
            for (auto& l : pr)
                if (l.grabbed == t) {
                    l.grabbed = dcon::creature_id{};
                    l.grabbed_chunk = -1;
                }
    }

    void died(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        auto& b = w.moth_data_get_body(creature::moth_data(w, c));
        b.drink = dcon::creature_id{};
        b.drinking = 0;
    }

    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const view = world::get_room_view(state, room);
        ivec2 const t = tile_of(at);
        vec2 const dir = rw::to_vec2(view.shortcut_entrance_hole_direction(t));
        vec2 const mid = room_view::middle_of_tile(t);
        // Creature.SpitOutOfShortCut: the head first, out of the hole
        for (uint32_t i = 0; i < chunk_count; ++i) {
            auto const k = chunk(state, c, i);
            physics::hard_set_position(state, k, mid + dir * (i == 0 ? 5.f : -float(i) * 2.f));
            w.body_chunk_set_last_pos(k, mid);
            w.body_chunk_set_vel(k, dir * 2.f);
        }
        auto const d = creature::moth_data(w, c);
        auto& b = w.moth_data_get_body(d);
        // BigMoth.NewRoom: the legs reset; its tongue lets go
        for (auto& pr : b.legs)
            for (auto& l : pr) {
                l.connection_pos = w.body_chunk_get_pos(chunk(state, c, l.connected_chunk));
                l.last_connection_pos = l.connection_pos;
                l.foot_pos = l.connection_pos;
                l.last_foot_pos = l.foot_pos;
                l.has_step = 0;
                l.has_ideal = 0;
                l.grabbed_chunk = -1;
                l.grabbed = dcon::creature_id{};
            }
        b.drink = dcon::creature_id{};
        b.drinking = 0;
        b.look_pos = mid + dir * 40.f;
        // BigMothAI.NewRoom
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        auto& ai = w.moth_data_get_ai(d);
        ai.temp_cliff_pos = ivec2{-1, -1};
        ai.temp_safe_baby_pos = ivec2{-1, -1};
        ai.temp_idle_pos = ivec2{-1, -1};
        ai.time_to_tire = rng.range(400, 7000);
        ai.give_up_on_cliff_timer = rng.range(200, 400);
        ai.time_in_room = 0;
        ai.trying_to_leave_room = 0;
        ai.look_creature = dcon::creature_id{};
        ai.focus = dcon::creature_id{};
        ai.last_focus = dcon::creature_id{};
        ai.picked_child = dcon::creature_id{};
        ai.stuck = stuck_tracker{};
        w.creature_set_destination(c, ivec2{custom::int_clamp(t.x, 0, view.width - 1), custom::int_clamp(t.y, 0, view.height - 1)});
        tracker::clear(state, c);
    }

    std::string describe(sys::game_state const& state, dcon::creature_id c) {
        static constexpr char const* names[] = {"idle", "flee", "attack", "eat", "stalk", "stow food", "set down baby", "feed baby", "escape rain", "get unstuck"};
        auto const& w = state.world;
        auto const& pl = w.creature_get_payload(c).moth;
        auto const& b = w.moth_data_get_body(creature::moth_data(w, c));
        vec2 const h = w.body_chunk_get_pos(chunk(state, c, 0));
        uint32_t const bh = w.creature_get_behavior(c);
        std::string s = std::string(pl.variant == variant::small ? "small moth" : "big moth") + " at " + std::to_string(int(h.x)) + "," + std::to_string(int(h.y))
                      + " behavior " + (bh < 10 ? names[bh] : "?") + (b.want_to_fly ? " flying" : " walking");
        if (creature::grasped(w, c, 0))
            s += " holding " + std::string(creature::to_string(w.creature_get_type(creature::grasped(w, c, 0))));
        if (b.drinking)
            s += " drinking";
        if (b.food_stored > 0)
            s += " food " + std::to_string(b.food_stored);
        return s;
    }

    bool spawn_spot(room_view const& room, ivec2 tile) {
        if (!room.inside(tile.x, tile.y) || room.get_tile(tile).water != 0 || !room.get_tile(tile + ivec2{0, -1}).solid())
            return false;
        for (int32_t dy = 0; dy < 4; ++dy)
            for (int32_t dx = -2; dx <= 2; ++dx)
                if (room.get_tile(tile + ivec2{dx, dy}).solid())
                    return false;
        return true;
    }

    variant variant_of(sys::game_state const& state, dcon::creature_id c) noexcept { return state.world.creature_get_payload(c).moth.variant; }

    float scale_of(sys::game_state const& state, dcon::creature_id c) noexcept { return state.world.creature_get_payload(c).moth.scale; }
}
