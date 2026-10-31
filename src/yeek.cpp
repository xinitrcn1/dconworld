#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

#include "yeek.hpp"
#include "dens.hpp"
#include "migrate.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "rooms.hpp"
#include "sound_types.hpp"
#include "tracker.hpp"
#include "yeek_graphics.hpp"
#include "yeek_types.hpp"

namespace yeek {
    using creature::relationship;
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    using world::terrain;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // CreatureTemplate (Yeek): baseDamageResistance, baseStunResistance,
        // instantDeathDamageLimit, visualRadius; Template.meatPoints
        constexpr float base_damage_resistance = 0.5f;
        constexpr float base_stun_resistance = 0.2f;
        constexpr float instant_death_damage_limit = 1.f;
        constexpr float visual_radius = 2100.f;
        constexpr uint8_t meat_points = 2;
        // YeekGraphics.CanAdvanceClimb (both legs at their goal: the feet
        // move 8 a tick, a step takes them about two), without the graphics
        constexpr uint8_t climb_step_ticks = 2;

        creature::yeek_payload& pl(sys::game_state& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).yeek;
        }
        creature::yeek_payload const& pl(sys::game_state const& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).yeek;
        }
        dcon::yeek_data_id data(sys::game_state const& state, dcon::creature_id c) {
            return creature::yeek_data(state.world, c);
        }
        idle_state& idle(sys::game_state& state, dcon::creature_id c) {
            return state.world.yeek_data_get_idle(data(state, c));
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
        ivec2 tile_of(vec2 p) {
            return room_view::get_tile_position(p);
        }
        // abstractCreature.pos: the main chunk's tile
        ivec2 here(sys::game_state const& state, dcon::creature_id c) {
            return tile_of(pos(state, c, 0));
        }
        vec2 mid(ivec2 t) {
            return room_view::middle_of_tile(t);
        }
        vec2 mid(vec2 p) {
            return room_view::middle_of_tile(p);
        }
        // Random.Range(float, float)
        float frange(rw::rng& r, float lo, float hi) {
            return lo + (hi - lo) * r.value();
        }
        // the object's gravity (PhysicalObject.gravity, 0.9)
        float grav(sys::game_state const& state, dcon::creature_id c) {
            return state.world.physical_object_get_g(body(state, c));
        }
        bool go_through_floors(sys::game_state const& state, dcon::creature_id c) {
            return state.world.body_chunk_get_go_through_floors(chunk(state, c, 0)) != 0;
        }
        void set_go_through_floors(sys::game_state& state, dcon::creature_id c, bool v) {
            physics::set_go_through_floors(state, body(state, c), v);
        }
        bool narrow(room_view const& room, ivec2 t) {
            return room.inside(t.x, t.y) && pathing::narrow_space(room, t);
        }
        void set_connection_distance(sys::game_state& state, dcon::creature_id c, float d) {
            auto& w = state.world;
            w.physical_object_for_each_chunk_connection_as_owner(body(state, c), [&](dcon::chunk_connection_id k) { w.chunk_connection_set_distance(k, d); });
        }
        // PhysicalObject.WeightedPush
        void weighted_push(sys::game_state& state, dcon::creature_id c, uint32_t a, uint32_t b, vec2 dir, float frc) {
            auto& w = state.world;
            float const ma = w.body_chunk_get_mass(chunk(state, c, a)), mb = w.body_chunk_get_mass(chunk(state, c, b));
            float const num = mb / (ma + mb);
            vel(state, c, a) += dir * frc * num;
            vel(state, c, b) -= dir * frc * (1.f - num);
        }

        // the slugcat holding it (grabbedBy[0].grabber is Player)
        dcon::player_id holding_slugcat(sys::game_state const& state, dcon::creature_id c) {
            auto const h = creature::holder(state, c);
            return h && creature::is_slugcat(state.world, h) ? creature::player_of(state, h) : dcon::player_id{};
        }

        // Yeek.SetPlayerHoldingBodyMass, SetStandardBodyMass
        void set_player_holding_mass(sys::game_state& state, dcon::creature_id c) {
            auto& x = pl(state, c);
            if (!x.standard_mass)
                return;
            x.standard_mass = 0;
            for (uint32_t i = 0; i < 2; ++i)
                state.world.body_chunk_set_mass(chunk(state, c, i), 0.01f);
        }
        void set_standard_mass(sys::game_state& state, dcon::creature_id c) {
            auto& x = pl(state, c);
            if (x.standard_mass)
                return;
            x.standard_mass = 1;
            state.world.body_chunk_set_mass(chunk(state, c, 0), 0.6f);
            state.world.body_chunk_set_mass(chunk(state, c, 1), 0.05f);
        }

        // Yeek.EndClimb
        void end_climb(creature::yeek_payload& x) {
            x.climbing = 0;
            x.interest_in_climbing_poles = -1.f;
        }

        // Yeek.CheckOnGround
        bool check_on_ground(sys::game_state const& state, dcon::creature_id c, room_view const& room, vec2 p) {
            auto const& w = state.world;
            if (w.creature_get_entering_shortcut(c).x >= 0)
                return false;
            if (auto const pp = holding_slugcat(state, c))
                return w.player_get_lower_body_frames_off_ground(pp) < 10;
            float const rad = w.body_chunk_get_rad(chunk(state, c, 0));
            auto const t = [&](vec2 o) { return room.get_tile(p + o).t; };
            vec2 const below{0.f, -20.f}, left{-rad, -10.f}, right{rad, -10.f}, under{0.f, -10.f};
            bool const slope = t(below) == terrain::slope || t(left) == terrain::slope || t(right) == terrain::slope;
            bool const solid = t(below) == terrain::solid || t(left) == terrain::solid || t(right) == terrain::solid;
            bool entrance = false;
            ivec2 const bt = tile_of(p + below);
            if (room.inside(bt.x, bt.y))
                entrance = room.get_tile(bt).t == terrain::shortcut_entrance;
            bool const floor = t(under) == terrain::floor || t(left) == terrain::floor || t(right) == terrain::floor;
            return (!go_through_floors(state, c) && floor) || solid || slope || entrance;
        }

        // Yeek.YeekCall (no Small_Needle_Worm_Little_Trumpet nor
        // Big_Needle_Worm_Small_Trumpet sound)
        void yeek_call(creature::yeek_payload& x) {
            if (x.yeek_call_counter <= 0.f)
                x.yeek_call_counter = 1.f;
        }

        // Yeek.Hop
        void hop(sys::game_state& state, dcon::creature_id c, room_view const& room, vec2 cur, vec2 goal, bool forced = false, bool allow_in_tunnel = false,
            bool from_jump = false) {
            auto& w = state.world;
            auto& x = pl(state, c);
            auto& rng = w.creature_get_random(c);
            if (!consious(state, c) || !(check_on_ground(state, c, room, pos(state, c, 0)) || forced || allow_in_tunnel || x.climbing) || x.tunnel
            || x.time_since_hop <= x.max_jump_counter)
                return;
            end_climb(x);
            x.time_since_hop = 0;
            float const health = w.creature_get_health(c);
            if (health < 1.f) {
                float const num = 1.f - health;
                creature::stun(state, c, int32_t(40.f * num));
                goal.x += float(rng.range(-25, 25)) * num;
            }
            vec2 v = custom::dir_vec(cur, goal);
            if (std::fabs((vel(state, c, 0) + v / 10.f).x) < 10.f)
                v.x *= 0.1f * std::fabs(cur.x - goal.x);
            v.y *= mathf::lerp(0.f, rw::distance(cur, goal) * 0.6f, grav(state, c));
            if (v.y < 3.f)
                v.y = 4.f;
            if (auto const pp = holding_slugcat(state, c); pp && w.player_get_animation(pp) == player::animation_index::climb_on_beam)
                v.y *= 0.75f;
            float const num2 = !forced && !from_jump ? frange(rng, 0.2f, 1.f) : 1.f;
            vec2 v2 = vec2{v.x * (1.f + 1.4f * x.fear_counter), v.y} * ((x.standard_mass ? 1.f : 0.85f) * num2);
            if (physics::submersion(state, room, chunk(state, c, 0)) > 0.2f)
                v2 *= 0.35f;
            float const cap = x.standard_mass ? 50.f : 40.f;
            if (rw::magnitude(v2) > cap)
                v2 = rw::normalized(v2) * cap;
            if (v2.y > x.ceiling_clearance * 5.f)
                v2.y = x.ceiling_clearance * 5.f;
            vel(state, c, 0) += v2;
            state.play_sound(sound::sound_id::slugcat_wall_jump, room.id, pos(state, c, 0), mathf::inverse_lerp(0.f, 10.f, v.y),
                2.5f + mathf::inverse_lerp(0.f, 100.f, v.y));
        }
        // Yeek.Jump
        void jump(sys::game_state& state, dcon::creature_id c, room_view const& room, vec2 cur, vec2 goal) {
            auto& x = pl(state, c);
            auto& rng = state.world.creature_get_random(c);
            if (x.time_since_jump <= 30)
                return;
            x.time_since_hop = 10000;
            x.time_since_jump = 0;
            vec2 b = custom::dir_vec(cur, goal);
            b.x *= 0.45f;
            b.y *= 15.f + frange(rng, 0.f, 10.f);
            behavior const bh = behavior(state.world.creature_get_behavior(c));
            if (bh == behavior::hungry || (bh == behavior::fear && x.fear_counter > 0.5f))
                b.y *= 2.f;
            if (bh == behavior::fear && x.fear_counter > 0.8f)
                b.x *= 16.f;
            hop(state, c, room, cur, cur + b, false, false, true);
        }

        //
        // YeekAI
        //
        // the tracker's settings: Tracker(10, 10, 250, 0.5, 5, 5, 20),
        // ThreatTracker(3) (no PreyTracker)
        tracker::params const& tracker_params() {
            static tracker::params const p = [] {
                tracker::params q;
                q.see_around_corners = 10;
                q.frames_to_remember = 250;
                q.ghost_dismissal_range = 20;
                q.threat_max = 3;
                return q;
            }();
            return p;
        }

        // CreatureTemplate.canFly of the ported kinds
        bool can_fly(creature::tmpl t) {
            using creature::tmpl;
            return t == tmpl::cicada_a || t == tmpl::cicada_b || t == tmpl::centiwing || t == tmpl::big_needle_worm || t == tmpl::small_needle_worm;
        }

        // ArtificialIntelligence.VisualScore * YeekAI's: from its head
        // (VisionPoint), out to 500 or so
        bool visual_contact(sys::game_state const& state, dcon::creature_id c, room_view const& room, vec2 look) {
            vec2 const eye = pos(state, c, 1);
            if (!custom::dist_less(eye, look, visual_radius))
                return false;
            float const dist = rw::distance(eye, look);
            float score = mathf::inverse_lerp(visual_radius, 0.f, dist);
            if (narrow(room, tile_of(look)))
                score -= 0.5f;
            score *= mathf::inverse_lerp(500.f, 190.f, dist);
            return score > 0.f && room.visual_contact(eye, look);
        }

        struct hook_ctx {
            sys::game_state& state;
            dcon::creature_id c;
            room_view const& room;
            // the field to where it is (CoordinateReachableAndGetbackable,
            // as the way back), built on the first question
            std::optional<pathing::field_id> from_here;
        };
        bool reachable(hook_ctx& x, ivec2 t) {
            if (!x.room.inside(t.x, t.y) || !pathing::accessible(x.room, t, movement))
                return false;
            if (!x.from_here)
                x.from_here = pathing::field_to(x.state, x.room.id, here(x.state, x.c), movement);
            return pathing::distance(x.state, *x.from_here, t) != pathing::unreachable;
        }

        void add_to_squad(sys::game_state& state, dcon::creature_id c, dcon::creature_id m) {
            auto& s = state.world.yeek_data_get_squad(data(state, c));
            for (uint32_t i = 0; i < s.count; ++i)
                if (s.who[i] == m)
                    return;
            if (s.count < max_squad)
                s.who[s.count++] = m;
        }
        // YeekAI.GetYeekSquadInRoom: the ones out in the room alive (the
        // rest forgotten)
        void squad_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room) {
            auto& s = state.world.yeek_data_get_squad(data(state, c));
            uint8_t n = 0;
            for (uint32_t i = 0; i < s.count; ++i) {
                auto const m = s.who[i];
                if (creature::live(state, m) && creature::is(state.world, m, creature::type::yeek) && creature::here(state, m, room) && !creature::dead(state, m))
                    s.who[n++] = m;
            }
            for (uint32_t i = n; i < s.count; ++i)
                s.who[i] = dcon::creature_id{};
            s.count = n;
        }

        // YeekAI.MakeCreatureLeaveRoom: a trip to a neighbour room (the
        // threat tracker's most attractive exit: here migrate.hpp's walk)
        void leave_room(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            if (!migrate::want_to(state, c))
                migrate::random_move(state, c, room, 1);
        }

        // YeekAI.SurprisedFearReaction: the squad panics and jumps away from
        // `t`, this yeek leaves the room
        relationship surprised(hook_ctx& x, creature::tracked const& r) {
            auto& state = x.state;
            auto& w = state.world;
            auto& me = pl(state, x.c);
            me.fear_counter = 1.f;
            auto const& s = w.yeek_data_get_squad(data(state, x.c));
            vec2 const danger = creature::main_pos(state, r.who);
            bool any = false;
            for (uint32_t i = 0; i < s.count; ++i) {
                auto const m = s.who[i];
                if (m == x.c || !creature::live(state, m) || !creature::is(w, m, creature::type::yeek) || !creature::here(state, m, x.room.id))
                    continue;
                auto& mp = pl(state, m);
                mp.fear_counter = 1.f;
                mp.yeek_call_counter = 0.f;
                // (its tracker takes the threat in on its own update)
                idle(state, m).alarm = r.who;
                any = true;
                vec2 a = custom::dir_vec(pos(state, m, 0), danger);
                a.x *= -1.f;
                a.y = 1.f;
                jump(state, m, x.room, pos(state, m, 0), pos(state, m, 0) + a * 600.f);
            }
            if (any) {
                leave_room(state, x.c, x.room);
                yeek_call(me);
                auto& g = w.yeek_data_get_graphics(data(state, x.c));
                g.eye_shudder = 3.f * w.yeek_data_get_personality(data(state, x.c)).nervous;
            }
            return relationship{creature::rel::afraid, 1.f};
        }

        // YeekAI.UpdateDynamicRelationship (TrackedCreatureState.alive: the
        // representation's `state`, 1 dead)
        relationship relationship_to(hook_ctx& x, creature::tracked& r) {
            auto& state = x.state;
            auto const& w = state.world;
            if (r.visual && creature::live(state, r.who))
                r.state = creature::dead(state, r.who) ? 1 : 0;
            relationship rel = creature::static_relationship(state, x.c, r.who);
            // another yeek: its pack
            if (creature::is(w, r.who, creature::type::yeek)) {
                add_to_squad(state, x.c, r.who);
                return relationship{creature::rel::pack, rel.intensity + 1.f};
            }
            if (rel.type == creature::rel::afraid) {
                if (r.state != 0) {
                    rel.intensity = 0.f;
                } else if (creature::here(state, r.who, x.room.id) && !can_fly(creature::template_of(state, r.who))) {
                    float const dist = rw::distance(pos(state, x.c, 0), mid(creature::main_pos(state, r.who)));
                    if (r.visual && ((pl(state, x.c).fear_counter < 0.4f && dist < 300.f) || dist < 30.f))
                        rel = surprised(x, r);
                    // (the rest: the intensity times 1, the way vanilla's
                    // height term works out)
                }
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
        // IUseARelationshipTracker.ModuleToTrackRelationship: Afraid a threat
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

        // Yeek.GroupLeaderPotential
        float group_leader_potential(personality const& p) {
            return (p.energy + p.sympathy + p.bravery) * mathf::clamp01(p.dominance);
        }

        // YeekAI.IdlePosScore
        float idle_pos_score(hook_ctx& x, ivec2 t) {
            auto& state = x.state;
            auto const& w = state.world;
            auto const& id = w.yeek_data_get_idle(data(state, x.c));
            float num = 1000.f;
            if (id.current_idle_pos.x >= 0)
                num -= 800.f * mathf::inverse_lerp(20.f, 0.f, rw::distance(rw::to_vec2(id.current_idle_pos), rw::to_vec2(t)));
            auto const& s = w.yeek_data_get_squad(data(state, x.c));
            for (uint32_t i = 0; i < s.count; ++i) {
                auto const m = s.who[i];
                if (!creature::live(state, m) || !creature::is(w, m, creature::type::yeek))
                    continue;
                float const glp = group_leader_potential(w.yeek_data_get_personality(data(state, m)));
                num += 150.f * mathf::inverse_lerp(12.f + 80.f * glp, 4.f, rw::distance(rw::to_vec2(here(state, m)), rw::to_vec2(t)));
            }
            // (where it can't get to scores nothing: vanilla only takes a
            // fifth off what it can't get back from)
            if (!reachable(x, t))
                return 0.f;
            auto const& ts = tracker::of(state, x.c);
            if (auto const* th = tracker::threat(ts); th && creature::here(state, th->who, x.room.id)) {
                float const k = mathf::inverse_lerp(400.f, 100.f, rw::distance(rw::to_vec2(tracker::best_guess(*th)), rw::to_vec2(t)));
                // (far from it the score is divided by nothing: the best)
                num = k > 0.f ? num / k : 3.4e38f;
            }
            if (x.room.get_tile(t).solid() || !x.room.get_tile(t - ivec2{0, 1}).solid() || (x.room.get_tile(t).flags & world::tile_worm_grass) != 0)
                num = 0.f;
            return num;
        }

        // YeekAI.getNewIdlePos
        void get_new_idle_pos(hook_ctx& x, int32_t chances) {
            auto& state = x.state;
            auto& w = state.world;
            auto& rng = w.creature_get_random(x.c);
            auto& id = idle(state, x.c);
            auto const& s = w.yeek_data_get_squad(data(state, x.c));
            if (id.room_interest < 0 || (id.room_interest < 2 && s.count < 2)) {
                leave_room(state, x.c, x.room);
                // (asks again in a while rather than every tick)
                id.idle_boredom = 200;
                return;
            }
            --id.room_interest;
            ivec2 best = id.current_idle_pos;
            float best_score = 0.f;
            for (int32_t i = 0; i < chances; ++i) {
                int32_t const tx = rng.range(0, x.room.width);
                ivec2 const t{tx, rng.range(0, x.room.height)};
                float const score = idle_pos_score(x, t);
                if (score > best_score) {
                    best = t;
                    best_score = score;
                    if (score == 1000.f)
                        break;
                }
            }
            float const energy = w.yeek_data_get_personality(data(state, x.c)).energy;
            auto& id2 = idle(state, x.c);
            id2.idle_boredom = int32_t(frange(rng, 400.f, 900.f) + mathf::lerp(800.f, 300.f, energy));
            id2.current_idle_pos = best;
        }

        // YeekAI.FeelsAlone
        bool feels_alone(sys::game_state& state, dcon::creature_id c) {
            auto& w = state.world;
            auto const& s = w.yeek_data_get_squad(data(state, c));
            if (s.count > 4 && w.creature_get_random(c).value() < 0.001f) {
                bool close = false;
                for (uint32_t i = 0; i < s.count; ++i)
                    if (creature::live(state, s.who[i]) && custom::dist_less(pos(state, c, 0), creature::main_pos(state, s.who[i]), 100.f))
                        close = true;
                if (!close)
                    yeek_call(pl(state, c));
                return !close;
            }
            return false;
        }

        // YeekAI.Update: the destination; true when it heads for a pipe (its
        // den, a room exit on a trip: it doesn't settle down short of it)
        bool ai_update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            ivec2 const at = here(state, c);
            hook_ctx hx{state, c, room, std::nullopt};
            tracker::hooks const h = make_hooks(hx);
            // a squad mate's alarm: the creature it panicked at
            if (auto const a = idle(state, c).alarm) {
                idle(state, c).alarm = dcon::creature_id{};
                if (creature::live(state, a) && creature::here(state, a, room.id))
                    tracker::see(state, c, room, a, h);
            }
            tracker::update(state, c, room, at, tracker_params(), h);
            squad_in_room(state, c, room.id);
            auto& x = pl(state, c);
            bool const trip = migrate::want_to(state, c);
            if (!trip)
                --idle(state, c).idle_boredom;
            if (idle(state, c).idle_boredom <= 0 || feels_alone(state, c))
                get_new_idle_pos(hx, 20);
            behavior b = behavior::idle;
            // UtilityComparer: threat (1), rain (1), to the power 1.1 (no
            // StuckTracker)
            auto const& ts = tracker::of(state, c);
            float const threat = mathf::pow(mathf::max(tracker::threat_utility(ts), 0.f), 1.1f);
            float const rain = mathf::pow(mathf::max(dens::rain_utility(state, c, room.id), 0.f), 1.1f);
            float const utility = mathf::max(threat, rain);
            if (utility > 0.01f)
                b = rain >= threat ? behavior::escape_rain : behavior::fear;
            w.creature_set_behavior(c, uint8_t(b));
            w.creature_set_current_utility(c, utility);

            ivec2& dest = w.creature_get_destination(c);
            auto const* th = tracker::threat(ts);
            if (b == behavior::idle || (b == behavior::fear && utility <= 0.2f)) {
                if (b == behavior::fear) {
                    x.fear_counter += 0.0001f;
                    if (x.fear_counter > 1.f) {
                        x.fear_counter = 1.f;
                        if (rng.value() < 0.5f) {
                            yeek_call(x);
                            leave_room(state, c, room);
                        }
                    }
                    idle(state, c).focus = th ? th->who : dcon::creature_id{};
                } else {
                    x.fear_counter = mathf::max(x.fear_counter - 0.005f, 0.f);
                }
                // on a trip: the way out
                if (migrate::want_to(state, c)) {
                    if (auto const e = migrate::travel_tile(state, c, room)) {
                        dest = *e;
                        return true;
                    }
                }
                if (idle(state, c).current_idle_pos.x >= 0)
                    dest = idle(state, c).current_idle_pos;
                return false;
            }
            if (b == behavior::fear) {
                x.fear_counter = mathf::min(x.fear_counter + 0.005f, 1.f);
                idle(state, c).idle_boredom = 40;
                idle(state, c).focus = dcon::creature_id{};
                if (check_on_ground(state, c, room, pos(state, c, 0)) || climbing_mode(state, c))
                    dest = tracker::flee_to(state, c, room, at, 3, 90, tracker_params(), h);
                // what it fears close by: a hop away along its way
                if (th && creature::here(state, th->who, room.id) && rw::distance(pos(state, c, 0), mid(tracker::best_guess(*th))) < 80.f && rng.value() < 0.6f) {
                    vec2 bv = rw::normalized(vel(state, c, 0));
                    bv.x *= 10.f;
                    bv.y *= 25.f;
                    hop(state, c, room, pos(state, c, 0), pos(state, c, 0) + bv);
                }
                return false;
            }
            // EscapeRain: the den (DenFinder, dens.hpp)
            x.fear_counter = mathf::min(x.fear_counter + 0.005f, 1.f);
            idle(state, c).focus = dcon::creature_id{};
            idle(state, c).idle_boredom = 40;
            if (auto const e = dens::home_tile(state, c, room)) {
                dest = *e;
                return true;
            }
            return false;
        }

        // YeekGraphics.AdvanceClimb: the head pulled along the climb
        void advance_climb(sys::game_state& state, dcon::creature_id c) {
            auto& x = pl(state, c);
            vec2 const p = mid(pos(state, c, 0) + rw::normalized(x.climbing_orientation) * 20.f);
            vel(state, c, 1) += custom::dir_vec(pos(state, c, 1), p) * state.world.creature_get_random(c).value();
            x.climb_step = climb_step_ticks;
        }

        // Yeek.Climb (the graphics module's way: a step when the legs are
        // through with the last one)
        void climb(sys::game_state& state, dcon::creature_id c, ivec2 t) {
            auto& x = pl(state, c);
            auto& rng = state.world.creature_get_random(c);
            vec2 const p0 = pos(state, c, 0);
            if (!x.climbing) {
                x.climbing = 1;
                x.climbing_orientation = rw::lerp(x.climbing_orientation, rw::normalized(custom::dir_vec(p0, mid(t))) * 10.f, 0.1f);
                vel(state, c, 0) *= 0.1f;
                vel(state, c, 1) *= 0.2f;
                // YeekGraphics.StartClimbing
                advance_climb(state, c);
                return;
            }
            if (x.climb_step == 0 || rng.value() < 0.05f) {
                x.climbing_orientation = custom::dir_vec(mid(p0), mid(t));
                advance_climb(state, c);
                vel(state, c, 0) += x.climbing_orientation * 0.7f;
            }
            vel(state, c, 0) *= 0.6f;
            vel(state, c, 1) *= 0.6f;
            vel(state, c, 0) += x.climbing_orientation * 0.1f;
            vel(state, c, 1) += x.climbing_orientation * 0.12f;
            float const num = mathf::lerp(1.12f, 1.04f, mathf::inverse_lerp(-1.f, 1.f, (p0 - mid(t)).y));
            vel(state, c, 0).y += grav(state, c) * num;
            x.climbing_orientation = rw::normalized(x.climbing_orientation);
        }

        // Yeek.Act (not player controlled)
        void act(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            bool const into_pipe = ai_update(state, c, room);
            auto& x = pl(state, c);
            ivec2 const at = here(state, c);
            vec2 const p0 = pos(state, c, 0);
            ivec2 const goal = w.creature_get_destination(c);
            vec2 const goal_mid = mid(goal);
            bool const on_grass = (room.get_tile(p0).flags & world::tile_worm_grass) != 0;
            // on worm grass: away in a panic (no AI.stranded)
            if (!x.climbing && !x.tunnel && on_grass && rng.value() < 0.2f) {
                vec2 b{mathf::lerp(frange(rng, -1.f, 1.f), mathf::sign(p0.x - goal_mid.x), 0.3f) * 30.f, 0.f};
                b.x *= 2.f;
                x.fear_counter = 1.f;
                yeek_call(x);
                hop(state, c, room, p0, p0 + b + vec2{0.f, 38.f + rng.value() * 20.f});
                return;
            }
            // StandardPather.FollowPath: the next tile of the way (none at
            // the goal, or with no way on)
            auto const f = pathing::field_to(state, room.id, goal, movement);
            auto const next = pathing::next_step(state, f, at);
            if (!next) {
                if (x.climbing) {
                    if (room.get_tile(mid(at)).any_beam())
                        idle(state, c).last_safe_climb = mid(at);
                    if (rw::distance(p0, idle(state, c).last_safe_climb) > 40.f)
                        end_climb(x);
                }
                return;
            }
            ivec2 const dt = *next;
            vec2 const dmid = mid(dt);
            set_go_through_floors(state, c, dt.y <= at.y);
            float const sub = physics::object_submersion(state, room, body(state, c));
            // the poles
            if (room.get_tile(mid(at)).any_beam() && !narrow(room, at)) {
                x.interest_in_climbing_poles += 0.15f;
                if (sub >= 0.5f)
                    x.interest_in_climbing_poles += 0.1f;
                if (x.interest_in_climbing_poles > 1.f)
                    x.interest_in_climbing_poles = 1.f;
            } else {
                if (x.interest_in_climbing_poles > 0.f)
                    x.interest_in_climbing_poles -= 0.1f;
                if (x.interest_in_climbing_poles < 0.f)
                    x.interest_in_climbing_poles += 0.01f;
            }
            bool const diagonal = dt.x != at.x && dt.y != at.y;
            bool const onto_slope = room.get_tile(dt).t == terrain::slope;
            if ((x.interest_in_climbing_poles >= 1.f && room.get_tile(p0).any_beam()) || x.climbing) {
                if (room.get_tile(p0).any_beam())
                    idle(state, c).last_safe_climb = mid(p0);
                set_go_through_floors(state, c, true);
                ivec2 t = dt;
                if (onto_slope)
                    t = at;
                if (sub > 0.5f)
                    vel(state, c, 0).y += 12.f;
                climb(state, c, t);
                // off the pole onto a slope or diagonally: a hop
                if (onto_slope || diagonal) {
                    x.interest_in_climbing_poles = 1.f;
                    vec2 const v = custom::dir_vec(pos(state, c, 0), goal_mid);
                    hop(state, c, room, pos(state, c, 0), dmid + v + vec2{0.f, 4.f});
                    vel(state, c, 0) += vec2{0.f, 1.f} + v * 3.f;
                    end_climb(x);
                }
                if (!room.get_tile(dt).any_beam())
                    end_climb(x);
                return;
            }
            // DropToFloor, DropToClimb, ReachDown: a hop off the edge
            if (dt.y < at.y - 1) {
                vec2 b2 = custom::dir_vec(p0, dmid);
                b2.y = 0.f;
                hop(state, c, room, p0, dmid + b2 + vec2{0.f, 5.f});
                end_climb(x);
                return;
            }
            // the corridors: crawling
            if (narrow(room, dt) || (x.tunnel && narrow(room, tile_of(w.body_chunk_get_last_pos(chunk(state, c, 0)))))
            || room.get_tile(p0).t == terrain::shortcut_entrance) {
                vec2 const v3 = custom::dir_vec(p0, dmid);
                x.climbing_orientation = rw::normalized(v3);
                if (x.climb_step == 0) {
                    vel(state, c, 0) = v3;
                    advance_climb(state, c);
                }
                vel(state, c, 0) += vec2{0.f, grav(state, c) * (creature::total_mass(state, c) * 3.35f)};
                vel(state, c, 1) += custom::dir_vec(pos(state, c, 1), pos(state, c, 0)) / 10.f;
                x.tunnel = 1;
                end_climb(x);
                if (!narrow(room, dt) && narrow(room, at)) {
                    x.tunnel = 0;
                    set_go_through_floors(state, c, false);
                    vel(state, c, 0) += rw::normalized(v3) * 4.f;
                }
                if (room.get_tile(dt).any_beam() && narrow(room, at) && !narrow(room, dt)) {
                    x.tunnel = 0;
                    x.interest_in_climbing_poles = 1.f;
                    set_go_through_floors(state, c, true);
                    x.climbing = 1;
                }
                return;
            }
            x.tunnel = 0;
            // swimming on the surface
            if (sub > 0.5f) {
                vec2 const a = custom::dir_vec(p0, dmid);
                vel(state, c, 0) = a * 0.8f + vec2{0.f, mathf::inverse_lerp(0.4f, 1.f, sub)};
                if (!x.climbing && room.get_tile(dt).any_beam() && !room.point_submerged(dmid)) {
                    x.interest_in_climbing_poles = 1.f;
                    climb(state, c, dt);
                    vel(state, c, 0) += custom::dir_vec(pos(state, c, 0), dmid) / 4.f;
                }
                return;
            }
            vec2 const v4 = custom::dir_vec(p0, goal_mid);
            if (behavior(w.creature_get_behavior(c)) == behavior::fear)
                jump(state, c, room, p0, dmid + v4 + vec2{0.f, 20.f});
            // close to where it wants to be: it sits (but on worm grass)
            if (!into_pipe && !x.climbing && rw::distance(rw::to_vec2(at), rw::to_vec2(goal)) < 8.f) {
                if (on_grass)
                    hop(state, c, room, p0, dmid + v4 + vec2{0.f, 9.f});
                return;
            }
            if (onto_slope) {
                hop(state, c, room, p0, dmid + v4 * 0.8f + vec2{0.f, 6.f});
                return;
            }
            if (!x.climbing && std::fabs(p0.x - goal_mid.x) < 10.f && std::fabs(p0.y - goal_mid.y) > 50.f && goal_mid.y > p0.y) {
                jump(state, c, room, p0, dmid + v4 + vec2{0.f, 40.f});
                return;
            }
            if (!x.climbing && p0.y + 15.f < goal_mid.y && std::fabs(p0.x - goal_mid.x) < 30.f) {
                hop(state, c, room, p0, dmid + v4 + vec2{0.f, 20.f});
                return;
            }
            if (!x.climbing)
                hop(state, c, room, p0, dmid + v4 + vec2{0.f, 10.f});
        }

        // Yeek.Update's grabbedBy a Player: it wriggles, and hops now and
        // then (or when the slugcat jumps), the slugcat along with it
        void held_by_player(sys::game_state& state, dcon::creature_id c, room_view const& room, dcon::player_id p) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            auto& x = pl(state, c);
            w.creature_set_stun(c, 0);
            x.body_direction = custom::rnv(rng);
            x.head_leading_counter = 0.f;
            vel(state, c, 0) += custom::rnv(rng) * 0.32f;
            vel(state, c, 1) += custom::rnv(rng) * 0.32f;
            set_connection_distance(state, c, 2.f);
            x.tunnel = 0;
            vec2 const p0 = pos(state, c, 0);
            if (narrow(room, tile_of(p0)) || room.get_tile(p0).t == terrain::shortcut_entrance)
                x.tunnel = 1;
            end_climb(x);
            weighted_push(state, c, 1, 0, x.climbing_orientation, 1.2f);
            auto const& in = w.player_get_input(p);
            bool const on_beam = w.player_get_body_mode(p) == player::body_mode_index::climbing_on_beam;
            bool const flag = in[0].jmp && (in[0].y > -1 || on_beam);
            if (((rng.value() > 0.95f && !on_beam) || flag) && x.time_since_hop > x.max_jump_counter) {
                if ((check_on_ground(state, c, room, p0) || on_beam) && !x.tunnel) {
                    x.max_jump_counter = int16_t(frange(rng, 30.f, 40.f));
                    x.yeek_call_counter = 0.f;
                    if (in[0].y > -1) {
                        vel(state, c, 0) *= 0.01f;
                        vec2 const up = vec2{frange(rng, -20.f, 20.f), frange(rng, 20.f, 30.f)} + vec2{0.f, flag ? 60.f : 0.f};
                        hop(state, c, room, pos(state, c, 0), pos(state, c, 0) + up, true);
                        vel(state, c, 0) *= 0.6f;
                        // the slugcat goes up with it
                        w.body_chunk_get_vel(player::body_chunk(state, p, 0)).y = vel(state, c, 0).y;
                        x.controlled_jump_velocity = vel(state, c, 0);
                    }
                }
                yeek_call(x);
            }
        }
    }

    //
    // The yeek
    //
    bool on_ground(sys::game_state const& state, dcon::creature_id c, room_view const& room) noexcept {
        return check_on_ground(state, c, room, pos(state, c, 0));
    }
    bool climbing_mode(sys::game_state const& state, dcon::creature_id c) noexcept {
        return pl(state, c).climbing && !creature::held(state, c);
    }
    bool tunnel_mode(sys::game_state const& state, dcon::creature_id c) noexcept {
        return pl(state, c).tunnel && !creature::held(state, c);
    }
    personality const& personality_of(sys::game_state const& state, dcon::creature_id c) noexcept {
        return state.world.yeek_data_get_personality(data(state, c));
    }
    looks const& looks_of(sys::game_state const& state, dcon::creature_id c) noexcept {
        return state.world.yeek_data_get_looks(data(state, c));
    }
    dcon::creature_id focus(sys::game_state const& state, dcon::creature_id c) noexcept {
        return state.world.yeek_data_get_idle(data(state, c)).focus;
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::yeek);
        auto const d = data(state, c);
        rw::rng rng{state.random.next() | 1u};
        int32_t seed = int32_t(rng.next() & 0x7FFFFFFFu);
        if (state.spawn_seed >= 0)
            seed = state.spawn_seed;
        physics::object_desc desc;
        desc.gravity = 0.9f;
        desc.air_friction = 0.99f;
        desc.water_friction = 0.98f;
        desc.buoyancy = 1.05f;
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
                        at = mid(t);
                    }
                }
        }
        // the body (7, 0.6) and the head (3, 0.05), pulled to 15 at most
        auto const c0 = physics::add_chunk(state, o, at, 7.f, 0.6f);
        auto const c1 = physics::add_chunk(state, o, at + vec2{0.f, 5.f}, 3.f, 0.05f);
        physics::add_connection(state, o, c0, c1, 15.f, physics::connection_type::pull, 1.f, 0.01f);
        w.try_create_creature_body(c, o);
        // AbstractCreature.personality (its own InitState(seed))
        personality p;
        {
            auto const push_from_half = [](float v, float e) {
                if (v == 0.5f)
                    return 0.5f;
                if (v < 0.5f)
                    return custom::lerp_map(v, 0.f, 0.5f, 0.f, 0.5f, e);
                return custom::lerp_map(v, 1.f, 0.5f, 1.f, 0.5f, e);
            };
            rw::unity_random r{seed};
            p.sympathy = push_from_half(r.value(), 1.5f);
            p.energy = push_from_half(r.value(), 1.5f);
            p.bravery = push_from_half(r.value(), 1.5f);
            float const n0 = r.value();
            float const n1 = mathf::pow(r.value(), 0.25f);
            p.nervous = mathf::lerp(n0, mathf::lerp(p.energy, 1.f - p.bravery, 0.5f), n1);
            float const a0 = r.value();
            float const a1 = mathf::pow(r.value(), 0.25f);
            p.aggression = mathf::lerp(a0, (p.energy + p.bravery) / 2.f * (1.f - p.sympathy), a1);
            float const d0 = r.value();
            float const d1 = mathf::pow(r.value(), 0.25f);
            p.dominance = mathf::lerp(d0, (p.energy + p.bravery + p.aggression) / 3.f, d1);
            p.nervous = push_from_half(p.nervous, 2.5f);
            p.aggression = push_from_half(p.aggression, 2.5f);
        }
        // YeekGraphics.CreateCosmeticAppearance (InitState(ID.RandomSeed);
        // no Winterized look)
        looks lk;
        {
            using rw::vec3;
            auto const lerp3 = [](vec3 a, vec3 b, float t) {
                t = mathf::clamp01(t);
                return a + (b - a) * t;
            };
            float const glp = group_leader_potential(p);
            rw::unity_random r{seed};
            float const fr = (1.f - glp) * r.range(0.4f, 0.9f);
            float const fg = mathf::clamp01(r.value() * glp);
            vec3 feather{fr, fg, mathf::clamp01(glp)};
            vec3 const highlight = lerp3(vec3{0.85490197f, 0.23137255f, 0.011764706f}, feather, 0.4f + p.bravery * 0.3f);
            vec3 fur = lerp3(feather, vec3{0.55f, 0.3f, 0.12f}, 0.33f + p.energy * 0.25f);
            vec3 a = lerp3(feather, vec3{0.38f, 0.37f, 0.35f}, 0.33f + p.aggression * 0.25f);
            if (p.nervous > p.bravery)
                a = lerp3(a, vec3{1.f, 1.f, 1.f}, p.nervous * 0.5f);
            else
                a = lerp3(a, vec3{0.f, 0.f, 0.f}, p.bravery * 0.5f);
            fur = lerp3(a, fur, p.sympathy);
            fur = lerp3(fur, vec3{1.f, 1.f, 1.f}, r.range(0.3f, 0.75f));
            lk.fur = fur;
            lk.highlight = highlight;
            int32_t plumage = 2;
            while (plumage == 2 || plumage == 1)
                plumage = r.range(0, 7);
            lk.plumage = uint8_t(plumage);
            (void)r.range(0.5f, mathf::clamp(glp * 1.5f, 0.6f, 1.2f));
            lk.feathers = uint8_t(r.range(3, 5));
        }
        auto& x = pl(state, c);
        x.body_direction = vec2{0.f, 0.f};
        x.climbing_orientation = vec2{0.f, 0.f};
        x.controlled_jump_velocity = vec2{0.f, 0.f};
        x.head_leading_counter = 0.f;
        x.yeek_call_counter = 0.f;
        x.grab_cooldown = 0.f;
        x.interest_in_climbing_poles = 0.f;
        x.ceiling_clearance = 10.f;
        x.fear_counter = 0.f;
        x.time_since_hop = 0;
        x.time_since_jump = 0;
        x.max_jump_counter = 20;
        x.hard_facing_dir = 1;
        x.climbing = 0;
        x.tunnel = 0;
        x.standard_mass = 1;
        x.climb_step = 0;
        x.meat_left = meat_points;
        x.eat_meat = 0;
        w.creature_set_random(c, rng);
        w.creature_set_graphics_random(c, rw::rng{(rng.state ^ 0x7EE4u) | 1u});
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_set_grasps(c, creature::grasp_pair{});
        w.creature_set_behavior(c, uint8_t(behavior::idle));
        w.creature_set_current_utility(c, 0.f);
        w.creature_set_destination(c, tile_of(at));
        w.yeek_data_set_seed(d, seed);
        w.yeek_data_set_personality(d, p);
        w.yeek_data_set_looks(d, lk);
        w.yeek_data_set_squad(d, squad{});
        idle_state id;
        id.current_idle_pos = tile_of(at);
        id.idle_boredom = 0;
        id.room_interest = w.creature_get_random(c).range(2, 7);
        id.last_safe_climb = at;
        w.yeek_data_set_idle(d, id);
        w.yeek_data_set_graphics(d, graphics_state{});
        tracker::clear(state, c);
        yeek_graphics::reset_positions(state, c);
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
                creature::stun(state, c, rng.range(1, rng.range(1, 27 - custom::int_clamp(int32_t(20.f * health), 0, 10))));
            if (health > 0.f && health < 1.f && rng.value() < 0.01f)
                health = mathf::min(1.f, health + 1.f / mathf::lerp(550.f, 70.f, health));
        }
        // Creature.Update + PhysicalObject.Update (no terrain impact sounds)
        creature::stun_countdown(state, c);
        physics::update_object(state, room, o, nullptr, nullptr);
        auto& x = pl(state, c);
        auto const holder = holding_slugcat(state, c);
        if (holder)
            set_player_holding_mass(state, c);
        else
            set_standard_mass(state, c);
        x.grab_cooldown -= 0.01f;
        if (x.climb_step > 0)
            --x.climb_step;
        vec2 const v0 = vel(state, c, 0);
        x.body_direction = rw::lerp(x.body_direction, rw::normalized(v0), 0.3f);
        if (std::fabs(x.body_direction.x) < 0.04f)
            x.body_direction.x = 0.f;
        x.hard_facing_dir = int8_t(mathf::sign(x.body_direction.x));
        x.head_leading_counter -= 1.f;
        x.yeek_call_counter -= 0.01f;
        if (std::fabs(v0.x) > 1.f)
            x.head_leading_counter = 60.f;
        if (std::fabs(pos(state, c, 0).x - w.body_chunk_get_last_pos(chunk(state, c, 0)).x) < 0.01f)
            x.head_leading_counter = 0.f;
        if (x.time_since_hop < 30000)
            ++x.time_since_hop;
        if (x.time_since_jump < 30000)
            ++x.time_since_jump;
        x.ceiling_clearance = 10.f;
        ivec2 const at = here(state, c);
        for (int32_t i = 0; i < 10; ++i)
            if (room.get_tile(at + ivec2{0, i}).solid()) {
                x.ceiling_clearance = float(i);
                break;
            }
        // the hop the slugcat holding it rides up on
        if (x.controlled_jump_velocity.y > 0.f) {
            x.controlled_jump_velocity.y -= grav(state, c);
            if (holder && !w.creature_get_dead(c)) {
                auto const pk = player::body_chunk(state, holder, 0);
                w.body_chunk_get_vel(pk).y = x.controlled_jump_velocity.y;
                if (room.get_tile(tile_of(w.body_chunk_get_pos(pk)) + ivec2{0, 1}).solid() || w.body_chunk_get_contact_point(pk).y != 0)
                    x.controlled_jump_velocity = vec2{0.f, 0.f};
            } else {
                x.controlled_jump_velocity = vec2{0.f, 0.f};
            }
        }
        // (the YeekTails are the graphics module's)
        if (holder && !w.creature_get_dead(c)) {
            held_by_player(state, c, room, holder);
            return;
        }
        x.max_jump_counter = int16_t(mathf::lerp(15.f, 6.f, x.fear_counter));
        if (!consious(state, c)) {
            set_connection_distance(state, c, 10.f);
            x.tunnel = 0;
            end_climb(x);
            return;
        }
        float const erg = room.gravity;
        weighted_push(state, c, 1, 0, custom::dir_vec(pos(state, c, 1), pos(state, c, 0)), 0.5f * erg);
        vec2 const b0 = vel(state, c, 0);
        if (climbing_mode(state, c))
            vel(state, c, 1) = b0 + vec2{x.climbing_orientation.x * 0.8f, 2.f} * erg;
        else if (tunnel_mode(state, c))
            vel(state, c, 1) = b0 + rw::normalized(x.climbing_orientation) * 0.4f * erg;
        else
            vel(state, c, 1) = b0 + vec2{mathf::lerp(0.f, mathf::sign(b0.x) * 0.2f, x.head_leading_counter / 60.f),
                                       mathf::lerp(2.f, 1.f, x.head_leading_counter / 60.f) * erg};
        // (no CarryObject: it never gets to hold fruit)
        act(state, c, room);
    }

    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) {
        // what holds it is kept apart by the carrying
        return !creature::holds(state.world, t, c) && !creature::holds(state.world, c, t);
    }

    void collide(sys::game_state&, dcon::creature_id, room_view const&, uint32_t, dcon::creature_id, uint32_t) {
        // Yeek.Collide grabs fruit only
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
        creature::stun(state, c, int32_t(st));
        health -= dmg;
        if (rng.value() < -health || health < -1.f || (health < 0.f && rng.value() < 0.33f) || dmg >= instant_death_damage_limit)
            creature::die(state, c);
    }

    void release(sys::game_state&, dcon::creature_id, dcon::creature_id) {
        // it holds nothing but fruit
    }

    void died(sys::game_state& state, dcon::creature_id c) {
        auto& x = pl(state, c);
        x.tunnel = 0;
        end_climb(x);
        x.controlled_jump_velocity = vec2{0.f, 0.f};
    }

    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto& rng = w.creature_get_random(c);
        auto const view = world::get_room_view(state, room);
        ivec2 const t = tile_of(at);
        vec2 const dir = rw::to_vec2(view.shortcut_entrance_hole_direction(t));
        vec2 const m = mid(t);
        for (uint32_t i = 0; i < 2; ++i) {
            auto const k = chunk(state, c, i);
            physics::hard_set_position(state, k, m - dir * (i != 1 ? 5.f : 10.f) + custom::deg_to_vec(rng.value() * 360.f));
            w.body_chunk_set_last_pos(k, m);
            w.body_chunk_set_vel(k, dir * 5.f);
        }
        auto& x = pl(state, c);
        hop(state, c, view, pos(state, c, 0), pos(state, c, 0) + vec2{dir.x * 10.f, dir.y});
        end_climb(x);
        x.tunnel = 1;
        x.interest_in_climbing_poles = 1.f;
        w.creature_set_shortcut_delay(c, 80);
        // YeekAI.NewRoom
        auto& id = idle(state, c);
        id.room_interest = rng.range(3, 7);
        id.idle_boredom = 0;
        id.current_idle_pos = t;
        id.focus = dcon::creature_id{};
        id.alarm = dcon::creature_id{};
        w.yeek_data_set_squad(data(state, c), squad{});
    }

    std::string describe(sys::game_state const& state, dcon::creature_id c) {
        static constexpr char const* names[] = {"idle", "get unstuck", "escape rain", "fear", "hungry", "return food"};
        auto const& w = state.world;
        auto const& x = pl(state, c);
        vec2 const p = pos(state, c, 0);
        uint32_t const b = w.creature_get_behavior(c);
        return "yeek at " + std::to_string(int(p.x)) + "," + std::to_string(int(p.y)) + " behavior " + (b < 6 ? names[b] : "?") + " fear "
            + std::to_string(x.fear_counter) + " squad " + std::to_string(int(w.yeek_data_get_squad(data(state, c)).count))
            + (x.climbing ? " climbing" : "") + (x.tunnel ? " crawling" : "") + (creature::held(state, c) ? " held" : "");
    }

    bool spawn_spot(room_view const& room, ivec2 tile) {
        return room.inside(tile.x, tile.y) && !room.get_tile(tile).solid() && room.get_tile(tile - ivec2{0, 1}).solid()
            && pathing::accessible(room, tile, movement);
    }

    void flying_weapon(sys::game_state& state, dcon::creature_id c, vec2 wp) {
        auto& w = state.world;
        if (!consious(state, c))
            return;
        vec2 const p0 = pos(state, c, 0);
        if (!(rw::distance(p0, wp) < 80.f && rw::distance(p0, wp) < rw::distance(w.body_chunk_get_last_pos(chunk(state, c, 0)), wp)))
            return;
        auto& x = pl(state, c);
        x.fear_counter = 1.f;
        yeek_call(x);
        auto const view = world::get_room_view(state, creature::room_of(state, c));
        leave_room(state, c, view);
        if (wp.y <= p0.y + 15.f)
            jump(state, c, view, p0, p0 - custom::dir_vec(p0, wp));
    }

    //
    // The player side
    //
    dcon::creature_id held_by(sys::game_state const& state, dcon::player_id p) noexcept {
        if (!p)
            return dcon::creature_id{};
        auto const c = creature::grasped(state.world, player::creature_of(state, p), 0);
        return c && creature::is(state.world, c, creature::type::yeek) ? c : dcon::creature_id{};
    }

    dcon::creature_id pickup_candidate(sys::game_state const& state, dcon::player_id p, room_view const& room, float& score) {
        auto const& w = state.world;
        vec2 const p0 = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
        int32_t const flip = w.player_get_flip_direction(p);
        dcon::creature_id result;
        score = 3.4e38f;
        rooms::for_each_creature(state, room.id, creature::type::yeek, [&](dcon::creature_id c) {
            // Player.Grabability: TwoHands; CanIPickThisUp: not held by a slugcat
            if (w.creature_get_away(c) != 0)
                return;
            if (auto const h = creature::holder(state, c); h && creature::is_slugcat(w, h))
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
        pl(state, c).eat_meat = 0;
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
        // (eatMeat moves it closer to the mouth: 5 + rad either way here)
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
            physics::hard_set_position(state, chunk(state, c, i), at + vec2{0.f, float(i) * 5.f});
            w.body_chunk_set_vel(chunk(state, c, i), w.body_chunk_get_vel(player::body_chunk(state, p, 0)));
        }
        // YeekAI.NewRoom
        auto& rng = w.creature_get_random(c);
        auto& id = idle(state, c);
        id.room_interest = rng.range(3, 7);
        id.idle_boredom = 0;
        id.current_idle_pos = tile_of(at);
        id.focus = dcon::creature_id{};
        id.alarm = dcon::creature_id{};
        w.yeek_data_set_squad(data(state, c), squad{});
        tracker::clear(state, c);
        yeek_graphics::reset_positions(state, c);
    }

    meat eat_meat_update(sys::game_state& state, dcon::player_id p, bool full, int32_t& quarters) {
        using ::player::slugcat_name;
        auto& w = state.world;
        quarters = 0;
        auto const c = held_by(state, p);
        if (!c)
            return meat::none;
        auto& x = pl(state, c);
        auto const& in = w.player_get_input(p);
        auto const name = w.player_get_stats(p).name;
        // Player.CanEatMeat: dead, and a carnivore (not the Saint nor
        // Spearmaster); the yeek isn't IPlayerEdible
        bool const carnivore = name == slugcat_name::red || name == slugcat_name::artificer || name == slugcat_name::gourmand || name == slugcat_name::inv;
        if (in[0].pckp && carnivore && creature::dead(state, c)) {
            if (x.eat_meat < 255)
                ++x.eat_meat;
            // EatMeatUpdate
            if (x.eat_meat > 20) {
                auto& rng = w.creature_get_random(c);
                auto const main = player::body_chunk(state, p, 0);
                auto const other = player::body_chunk(state, p, 1);
                if (x.eat_meat % 5 == 0) {
                    vec2 const b = custom::rnv(rng) * 3.f;
                    w.body_chunk_get_pos(main) += b;
                    w.body_chunk_get_vel(main) += b;
                }
                // the middle of the grabbed chunk and the one connected to it
                auto const k0 = chunk(state, c, 0), k1 = chunk(state, c, 1);
                float const m0 = w.body_chunk_get_mass(k0), m1 = w.body_chunk_get_mass(k1);
                vec2 const v = (w.body_chunk_get_pos(k0) * m0 + w.body_chunk_get_pos(k1) * m1) / (m0 + m1);
                vec2 const to = custom::dir_vec(w.body_chunk_get_pos(main), v);
                w.body_chunk_get_vel(main) += to * 0.5f;
                w.body_chunk_get_vel(other) -= to * 0.6f;
                if (x.meat_left > 0 && !full && x.eat_meat > 40 && x.eat_meat % 15 == 3) {
                    // a bite (no Slugcat_Eat_Meat sounds, no drips)
                    w.body_chunk_get_pos(main) += custom::deg_to_vec(mathf::lerp(-90.f, 90.f, rng.value())) * 4.f;
                    w.body_chunk_get_vel(k0) += custom::dir_vec(v, w.body_chunk_get_pos(main)) * 0.9f / m0;
                    --x.meat_left;
                    quarters = (name == slugcat_name::gourmand || name == slugcat_name::inv) ? 2 : 4;
                }
            }
            if (x.eat_meat % 80 == 0 && (x.meat_left == 0 || full)) {
                x.eat_meat = 0;
                return meat::done;
            }
            return meat::eating;
        }
        if (!in[0].pckp && x.eat_meat > 60) {
            x.eat_meat = 0;
            return meat::done;
        }
        x.eat_meat = uint8_t(custom::int_clamp(int32_t(x.eat_meat) - 1, 0, 50));
        return meat::none;
    }
}
