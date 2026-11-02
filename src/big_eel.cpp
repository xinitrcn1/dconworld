#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "big_eel.hpp"
#include "big_eel_graphics.hpp"
#include "borders.hpp"
#include "dens.hpp"
#include "items.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "rooms.hpp"
#include "rot.hpp"
#include "spawns.hpp"
#include "tracker.hpp"

namespace big_eel {
    using creature::relationship;
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // CreatureTemplate (BigEel): baseDamageResistance, baseStunResistance,
        // visualRadius, throughSurfaceVision (waterVision 1 takes nothing off)
        constexpr float base_damage_resistance = 1000.f;
        constexpr float base_stun_resistance = 1000.f;
        constexpr float visual_radius = 1450.f;
        constexpr float through_surface_vision = 0.65f;
        // where it waits while off at sea: far out beside its room (no
        // physics runs; out of every creature's sight)
        constexpr float park_distance = 8000.f;
        // the fewest rows of open deep water at a side that make a sea exit
        constexpr int32_t min_sea_exit = 8;

        creature::big_eel_payload& pl(sys::game_state& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).big_eel;
        }
        creature::big_eel_payload const& pl(sys::game_state const& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).big_eel;
        }
        dcon::big_eel_data_id data(sys::game_state const& state, dcon::creature_id c) {
            return dcon::big_eel_data_id{dcon::big_eel_data_id::value_base_t(pl(state, c).data)};
        }
        ai_state& ai(sys::game_state& state, dcon::creature_id c) {
            return state.world.big_eel_data_get_ai(data(state, c));
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
        float rad(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_rad(chunk(state, c, i));
        }
        float mass(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_mass(chunk(state, c, i));
        }
        float sub(sys::game_state const& state, room_view const& room, dcon::creature_id c, uint32_t i) {
            return physics::submersion(state, room, chunk(state, c, i));
        }
        ivec2 tile_of(vec2 p) {
            return room_view::get_tile_position(p);
        }
        // abstractCreature.pos: the head's tile
        ivec2 here(sys::game_state const& state, dcon::creature_id c) {
            return tile_of(pos(state, c, 0));
        }
        float tile_dist(ivec2 a, ivec2 b) {
            return rw::magnitude(rw::to_vec2(a - b));
        }
        ivec2 clamp_into(room_view const& room, ivec2 t) {
            return ivec2{custom::int_clamp(t.x, 0, room.width - 1), custom::int_clamp(t.y, 0, room.height - 1)};
        }
        bool deep_water(room_view const& room, vec2 p) {
            return room.get_tile(p).water == 1;
        }
        // Room.IsPositionInsideBoundries
        bool inside(room_view const& room, ivec2 t) {
            return room.inside(t.x, t.y);
        }
        // bodyChunkConnections[i].distance
        float connection_distance(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return mathf::max(rad(state, c, i), rad(state, c, i + 1));
        }

        //
        // The sea: rooms with deep water open at a side
        //
        bool sea_room(sys::game_state const& state, dcon::room_id r) {
            auto const& w = state.world;
            if (!r || w.room_get_is_shelter(r) || w.room_get_is_gate(r))
                return false;
            auto const view = world::get_room_view(state, r);
            int32_t lo = 0, hi = 0;
            return sea_exit(state, view, -1, lo, hi) || sea_exit(state, view, 1, lo, hi);
        }
        // the sea rooms a room's exits lead to (BigEelAbstractAI's room
        // cluster), serial only (it looks at other rooms)
        std::vector<dcon::room_id> neighbour_sea_rooms(sys::game_state const& state, dcon::room_id room) {
            auto const& w = state.world;
            std::vector<dcon::room_id> out;
            w.room_for_each_exit_location(room, [&](dcon::exit_location_id el) {
                auto const t = w.room_exit_get_target_from_exit_connection(w.exit_location_get_exit(el));
                if (t && t != room && std::find(out.begin(), out.end(), t) == out.end() && sea_room(state, t))
                    out.push_back(t);
            });
            return out;
        }
        // every sea room of a region (world.seaAccessNodes' rooms)
        std::vector<dcon::room_id> region_sea_rooms(sys::game_state const& state, dcon::region_id region) {
            std::vector<dcon::room_id> out;
            if (!region)
                return out;
            state.world.region_for_each_region_membership(region, [&](dcon::region_membership_id m) {
                auto const r = state.world.region_membership_get_room(m);
                if (sea_room(state, r))
                    out.push_back(r);
            });
            return out;
        }

        //
        // BigEelAI
        //
        // Tracker(10, 5, 250, 0.5, 5, 5, 20), PreyTracker(5, 1, 5, 150, 0.05)
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
                return q;
            }();
            return p;
        }

        // ArtificialIntelligence.VisualContact (VisualScore, BigEelAI's is
        // the base one): from the head
        bool visual_contact(sys::game_state const& state, dcon::creature_id c, room_view const& room, vec2 look) {
            vec2 const eye = pos(state, c, 0);
            if (!custom::dist_less(eye, look, visual_radius))
                return false;
            float score = mathf::inverse_lerp(visual_radius, 0.f, rw::distance(eye, look));
            if (room.water && deep_water(room, eye) != deep_water(room, look))
                score -= 1.f - through_surface_vision;
            if (pathing::narrow_space(room, tile_of(look)))
                score -= 0.5f;
            return score > 0.f && room.visual_contact(eye, look);
        }

        // what is in its jaws (AmIHoldingCreature)
        bool holding(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) {
            auto const& cl = state.world.big_eel_data_get_clamped(data(state, c));
            for (uint32_t i = 0; i < cl.count; ++i)
                if (cl.o[i].who == t)
                    return true;
            return false;
        }

        struct hook_ctx {
            sys::game_state& state;
            dcon::creature_id c;
            room_view const& room;
            // the field to where it is (CoordinateReachableAndGetbackable:
            // swimming is the same both ways), built on the first question
            std::optional<pathing::field_id> from_here;
        };
        bool reachable(hook_ctx& x, ivec2 t) {
            if (!x.room.inside(t.x, t.y) || !pathing::accessible(x.room, t, swim))
                return false;
            if (!x.from_here)
                x.from_here = pathing::field_to(x.state, x.room.id, clamp_into(x.room, here(x.state, x.c)), swim);
            return pathing::distance(x.state, *x.from_here, t) != pathing::unreachable;
        }
        // BigEelAI.UpdateDynamicRelationship (CreateTrackerRepresentation-
        // ForCreature: what it holds and the small creatures aren't tracked)
        relationship relationship_to(hook_ctx& x, creature::tracked const& r) {
            auto& state = x.state;
            auto const tt = creature::template_of(state, r.who);
            if (holding(state, x.c, r.who) || creature::small_creature(tt))
                return relationship{creature::rel::ignores, 0.f};
            relationship rel = creature::static_relationship(state, x.c, r.who);
            if (rel.type == creature::rel::eats) {
                float num = rel.intensity;
                float const wl = float(x.room.default_water_level);
                num *= mathf::inverse_lerp(wl + 6.f, wl - 40.f, float(tracker::best_guess(r).y));
                num *= mathf::inverse_lerp(0.f, 5.f, creature::body_size(tt));
                rel.intensity = mathf::pow(num, 0.2f);
            }
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
            return relationship_to(*static_cast<hook_ctx*>(u), r);
        }
        bool hook_reach(void* u, ivec2 t) {
            return reachable(*static_cast<hook_ctx*>(u), t);
        }
        // IUseARelationshipTracker.ModuleToTrackRelationship: Eats is prey
        creature::track_module hook_module(relationship r) {
            return r.type == creature::rel::eats ? creature::track_module::prey : creature::track_module::none;
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

        // BigEel.InBiteArea
        bool in_bite_area(sys::game_state const& state, dcon::creature_id c, room_view const& room, vec2 p, float margin) {
            vec2 const h = pos(state, c, 0);
            vec2 const v = custom::dir_vec(pos(state, c, 1), h);
            if (!custom::dist_less(h + v * 60.f, p, 300.f + margin))
                return false;
            vec2 const b = custom::perpendicular_vector(v);
            if (std::fabs(custom::distance_to_line(p, h + v * 60.f - b, h + v * 60.f + b)) > 60.f + margin)
                return false;
            if (std::fabs(custom::distance_to_line(p, h - v, h + v)) > 50.f + margin)
                return false;
            return room.visual_contact(h, p);
        }

        // BigEelAI.WantToSnapJaw: something it eats, in sight, in the bite
        // area (a random chunk of it), the area wider the more tired the jaws
        bool want_to_snap_jaw(sys::game_state& state, dcon::creature_id c, room_view const& room, rw::rng& rng) {
            auto const& ts = tracker::of(state, c);
            float const margin = mathf::lerp(-10.f, 100.f, mathf::pow(pl(state, c).jaw_charge_fatigue, 5.f));
            for (uint32_t i = 0; i < ts.count; ++i) {
                auto const& r = ts.reps[i];
                if (r.rel.type != creature::rel::eats || !r.visual || !creature::here(state, r.who, room.id))
                    continue;
                uint32_t const n = creature::chunk_count_of(state, r.who);
                if (n == 0)
                    continue;
                auto const k = creature::chunk_of(state, r.who, uint32_t(rng.range(0, int32_t(n))));
                if (in_bite_area(state, c, room, state.world.body_chunk_get_pos(k), margin))
                    return true;
            }
            return false;
        }

        // BigEelAI.WantToChargeJaw: hunting, the prey within 200 px
        bool want_to_charge_jaw(sys::game_state& state, dcon::creature_id c) {
            if (behavior(state.world.creature_get_behavior(c)) != behavior::hunt)
                return false;
            auto const f = ai(state, c).focus;
            if (!f)
                return false;
            auto const* r = tracker::find(state, c, f);
            return r && custom::dist_less(pos(state, c, 0), room_view::middle_of_tile(tracker::best_guess(*r)), 200.f);
        }

        // on the way out to sea over a side (the pathfinder's OutsideRoom
        // connection to a sea exit): the border tile it heads for (false:
        // the room has no sea exit)
        bool start_leaving(sys::game_state& state, dcon::creature_id c, room_view const& room, bool escape) {
            auto& a = ai(state, c);
            if (a.leave_side != 0) {
                a.escape = a.escape || escape;
                return true;
            }
            vec2 const h = pos(state, c, 0);
            int32_t best_side = 0, best_y = 0;
            float best = 0.f;
            for (int32_t side : {-1, 1}) {
                int32_t lo = 0, hi = 0;
                if (!sea_exit(state, room, side, lo, hi))
                    continue;
                int32_t const y = custom::int_clamp(tile_of(h).y, lo + 1, std::max(lo + 1, hi - 1));
                float const d = rw::distance(h, room_view::middle_of_tile(side < 0 ? 0 : room.width - 1, y));
                if (best_side == 0 || d < best) {
                    best_side = side;
                    best_y = y;
                    best = d;
                }
            }
            if (best_side == 0)
                return false;
            a.leave_side = int8_t(best_side);
            a.exit_tile = ivec2{best_side < 0 ? 0 : room.width - 1, best_y};
            a.leave_counter = 0;
            a.escape = escape ? 1 : 0;
            return true;
        }

        // BigEelAI.Update
        void ai_update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& x = pl(state, c);
            auto& a = ai(state, c);
            auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
            a.focus = dcon::creature_id{};
            ivec2 const at = here(state, c);
            hook_ctx hx{state, c, room, std::nullopt};
            tracker::hooks const h = make_hooks(hx);
            tracker::update(state, c, room, at, tracker_params(), h);
            bool const in_room = inside(room, at);
            if (in_room)
                x.time_in_this_room += 1.f;
            // UtilityComparer: the prey (its weight by hunger and the time
            // spent in the room, 0.9), the rain (1); the first of the highest
            float const prey_weight = mathf::inverse_lerp(300.f, 0.f, float(x.hunger_delay)) * mathf::inverse_lerp(1400.f, 1000.f, x.time_in_this_room) * 0.9f;
            float const prey_u = tracker::prey_utility(state, c, room, at, tracker_params()) * prey_weight;
            float const rain_u = dens::rain_utility(state, c, room.id);
            behavior b = rain_u > prey_u ? behavior::escape_rain : behavior::hunt;
            float const utility = mathf::max(rain_u, prey_u);
            if (utility < 0.1f)
                b = behavior::idle;
            // to the den off at sea; one in a pool with no way out idles on
            if (b == behavior::escape_rain && !start_leaving(state, c, room, true))
                b = behavior::idle;
            if (x.hunger_delay > 0 && in_room)
                --x.hunger_delay;
            w.creature_set_behavior(c, uint8_t(b));
            w.creature_set_current_utility(c, utility);

            ivec2& dest = w.creature_get_destination(c);
            switch (b) {
            case behavior::idle: {
                // BigEelAbstractAI.AbstractBehavior: every 1200 ticks in the
                // room it may set out for a neighbouring sea room
                if (in_room)
                    ++a.roam_timer;
                if (a.roam_timer > 1200) {
                    a.roam_timer -= 1200;
                    if (a.can_roam && rng.value() < 0.85f)
                        start_leaving(state, c, room, false);
                }
                if (a.leave_side != 0) {
                    dest = a.exit_tile;
                    break;
                }
                // a random spot under the water
                if (!inside(room, dest) || tile_dist(at, dest) < 10.f || !reachable(hx, dest)) {
                    int32_t const tx = rng.range(0, room.width);
                    int32_t const ty = rng.range(0, rng.range(0, room.default_water_level));
                    ivec2 const t{tx, ty};
                    if (reachable(hx, t) && !(tile_dist(at, t) < 10.f))
                        dest = t;
                }
                break;
            }
            case behavior::escape_rain:
                dest = a.exit_tile;
                break;
            case behavior::hunt: {
                auto const* r = tracker::prey(tracker::of(state, c));
                if (!r || !creature::live(state, r->who))
                    break;
                a.focus = r->who;
                dest = tracker::best_guess(*r);
                if (r->visual && creature::here(state, r->who, room.id)) {
                    vec2 const pp = creature::main_pos(state, r->who);
                    if (custom::dist_less(pos(state, c, 0), pp, 400.f)) {
                        x.attack_pos = pp;
                        x.has_attack_pos = 1;
                    }
                }
                break;
            }
            default:
                break;
            }
            if (rng.value() < 0.0033333334f && a.leave_side == 0) {
                ivec2 const d = custom::eight_directions[rng.range(0, 8)];
                dest += d;
            }
        }

        //
        // BigEel
        //
        // the room's NoLeviathanStrandingZones push the body off the shore
        void anti_stranding(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            vec2 const h = pos(state, c, 0);
            w.room_for_each_thing_location(room.id, [&](dcon::thing_location_id l) {
                auto const t = w.thing_location_get_thing(l);
                if (w.placed_thing_get_kind(t) != world::thing_kind::leviathan_zone)
                    return;
                vec2 const zp = w.placed_thing_get_pos(t);
                if (!custom::dist_less(h, zp, 1000.f))
                    return;
                vec2 const handle = w.placed_thing_get_handle(t);
                float const zr = rw::magnitude(handle);
                vec2 const n = rw::normalized(handle);
                for (uint32_t k = 0; k + 2 < chunk_count; ++k) {
                    vec2 const kp = pos(state, c, k);
                    if (custom::dist_less(kp, zp, zr))
                        vel(state, c, k) += n * 1.4f * mathf::inverse_lerp(zr, zr / 5.f, rw::distance(kp, zp));
                }
            });
        }

        // what it caught: gone into it (Destroy). Everything lets go of a
        // creature; a slugcat is eaten (Player.PermaDie), a creature is gone
        // at the end of the tick, an item too
        void finish(sys::game_state& state, dcon::creature_id c, room_view const& room, dcon::creature_id t) {
            auto& w = state.world;
            tracker::forget(state, c, t);
            creature::release_target(state, t, room.id);
            rot::release(state, t);
            ++dens::stats().prey_taken;
            if (auto const pp = creature::player_of(state, t)) {
                items::release_all(state, pp);
                creature::kill(state, t, creature::tmpl::big_eel);
                w.player_set_eaten(pp, 1);
                return;
            }
            creature::kill(state, t, creature::tmpl::big_eel);
            ++dens::stats().creatures_eaten;
            state.defer(room.id, [t](sys::game_state& st) {
                if (st.world.creature_is_valid(t))
                    creature::destroy(st, t);
            });
        }

        // BigEel.Swallow
        void swallow(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& x = pl(state, c);
            auto& cl = w.big_eel_data_get_clamped(data(state, c));
            // (letting go of each empties the list: work on a copy)
            clamped_list const list = cl;
            cl.count = 0;
            for (uint32_t i = 0; i < list.count; ++i) {
                auto const& e = list.o[i];
                if (e.who) {
                    bool seen = false;
                    for (uint32_t j = 0; j < i; ++j)
                        seen = seen || list.o[j].who == e.who;
                    if (seen || !creature::here(state, e.who, room.id))
                        continue;
                    if (!creature::small_creature(creature::template_of(state, e.who)))
                        x.hunger_delay = std::max(x.hunger_delay, 600);
                    finish(state, c, room, e.who);
                } else if (e.item && w.item_is_valid(e.item)) {
                    dcon::item_id const it = e.item;
                    state.defer(room.id, [it](sys::game_state& st) {
                        if (st.world.item_is_valid(it))
                            items::destroy(st, it);
                    });
                }
            }
        }

        // BigEel.Crush: the caught creature's joints go slack
        void crush(sys::game_state& state, dcon::creature_id t) {
            auto& w = state.world;
            auto const o = creature::body_of(state, t);
            if (!o)
                return;
            w.physical_object_for_each_chunk_connection_as_owner(o, [&](dcon::chunk_connection_id k) {
                w.chunk_connection_set_type(k, physics::connection_type::pull);
            });
        }

        void add_clamped(clamped_list& cl, dcon::creature_id who, dcon::item_id item, uint32_t k, float distance) {
            for (uint32_t i = 0; i < cl.count; ++i)
                if (cl.o[i].who == who && cl.o[i].item == item && cl.o[i].chunk == k)
                    return;
            if (cl.count >= max_clamped)
                return;
            clamped_object& o = cl.o[cl.count++];
            o.who = who;
            o.item = item;
            o.chunk = uint8_t(k);
            o.distance = distance;
        }

        // BigEel.JawsSnap
        void jaws_snap(sys::game_state& state, dcon::creature_id c, room_view const& room, rw::rng& rng) {
            auto& w = state.world;
            auto& x = pl(state, c);
            x.snap_frame = 1;
            // (Leviathan_Bite and the screen shake: no sound, no camera)
            for (uint32_t i = 1; i < chunk_count; ++i)
                vel(state, c, i) += (custom::rnv(rng) + custom::dir_vec(pos(state, c, i - 1), pos(state, c, i)))
                    * std::sin(mathf::inverse_lerp(1.f, 11.f, float(i)) * mathf::pi) * 8.f;
            vec2 const p = pos(state, c, 0);
            vec2 const v = custom::dir_vec(pos(state, c, 1), p);
            x.beak_gap = 0.f;
            auto& cl = w.big_eel_data_get_clamped(data(state, c));
            std::vector<dcon::creature_id> caught;
            // every creature but the leviathans (no Leviathan_Crush sounds)
            rooms::for_each_creature(state, room.id, [&](dcon::creature_id t) {
                if (t == c || w.creature_get_type(t) == creature::type::big_eel || !creature::here(state, t, room.id))
                    return;
                uint32_t const n = creature::chunk_count_of(state, t);
                bool hit = false;
                for (uint32_t l = 0; l < n; ++l) {
                    auto const k = creature::chunk_of(state, t, l);
                    vec2 const kp = w.body_chunk_get_pos(k);
                    float const kr = w.body_chunk_get_rad(k);
                    if (!in_bite_area(state, c, room, kp, kr / 2.f))
                        continue;
                    vec2 const b = custom::closest_point_on_line(p, p + v, kp);
                    add_clamped(cl, t, dcon::item_id{}, l, rw::distance(p, b));
                    x.beak_gap = mathf::max(x.beak_gap, kr);
                    hit = true;
                }
                if (hit)
                    caught.push_back(t);
            });
            // the loose items (not one held, stuck in a wall or on its stalk)
            rooms::for_each_item(state, room.id, [&](dcon::item_id it) {
                if (w.item_get_grabbed_by(it) || w.item_get_has_stuck(it) || w.item_get_stalk(it))
                    return;
                auto const k = items::chunk(state, it);
                if (!k)
                    return;
                vec2 const kp = w.body_chunk_get_pos(k);
                float const kr = w.body_chunk_get_rad(k);
                if (!in_bite_area(state, c, room, kp, kr / 2.f))
                    return;
                vec2 const b = custom::closest_point_on_line(p, p + v, kp);
                add_clamped(cl, dcon::creature_id{}, it, 0, rw::distance(p, b));
                x.beak_gap = mathf::max(x.beak_gap, kr);
            });
            // (no Leviathan_Clamper_Hit_Terrain sound)
            for (auto const t : caught) {
                creature::kill(state, t, creature::tmpl::big_eel);
                crush(state, t);
            }
        }

        // BigEel.Act's clampedObjects: held in the jaws, pulled in as the
        // charge runs on (MoveFromOutsideMyUpdate; the slugcat's graphics
        // body parts aren't pulled in with it)
        void hold_clamped(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto const& x = pl(state, c);
            auto& cl = w.big_eel_data_get_clamped(data(state, c));
            vec2 const p = pos(state, c, 0);
            vec2 const a = custom::dir_vec(pos(state, c, 1), p);
            float const jc = x.jaw_charge;
            float const f = mathf::inverse_lerp(0.6f, 0.8f, jc);
            for (uint32_t i = 0; i < cl.count;) {
                auto const& e = cl.o[i];
                bool ok = false;
                if (e.who)
                    ok = creature::here(state, e.who, room.id) && e.chunk < creature::chunk_count_of(state, e.who);
                else
                    ok = e.item && w.item_is_valid(e.item) && rooms::room_of(state, e.item) == room.id && !w.item_get_grabbed_by(e.item);
                if (!ok) {
                    cl.o[i] = cl.o[--cl.count];
                    continue;
                }
                vec2 const target = p + a * (e.distance - mathf::inverse_lerp(0.4f, 0.7f, jc) * 60.f - mathf::inverse_lerp(0.7f, 1.f, jc) * 40.f);
                if (e.who) {
                    w.body_chunk_set_pos(creature::chunk_of(state, e.who, e.chunk), target);
                    if (jc > 0.6f) {
                        uint32_t const n = creature::chunk_count_of(state, e.who);
                        for (uint32_t k = 0; k < n; ++k) {
                            auto const ck = creature::chunk_of(state, e.who, k);
                            w.body_chunk_get_vel(ck) *= 1.f - f;
                            w.body_chunk_set_pos(ck, rw::lerp(w.body_chunk_get_pos(ck), target, f));
                        }
                    }
                } else {
                    auto const k = items::chunk(state, e.item);
                    w.body_chunk_set_pos(k, target);
                    if (jc > 0.6f) {
                        w.body_chunk_get_vel(k) *= 1.f - f;
                        w.body_chunk_set_pos(k, rw::lerp(w.body_chunk_get_pos(k), target, f));
                    }
                }
                ++i;
            }
        }

        // BigEel.Swim (BigEelPather.FollowPath: the field to the
        // destination; on the way out, past the border, straight out)
        void swim_update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& x = pl(state, c);
            auto const& a = ai(state, c);
            ivec2 const at = here(state, c);
            bool const leaving = a.leave_side != 0 && behavior(w.creature_get_behavior(c)) != behavior::hunt;
            if (behavior(w.creature_get_behavior(c)) == behavior::idle && inside(room, at) && !leaving)
                x.swim_speed = mathf::max(x.swim_speed - 0.00625f, 0.5f);
            else
                x.swim_speed = mathf::min(x.swim_speed + 0.00625f, 1.f);
            float const ss = x.swim_speed;
            x.swim_motion -= 1.f / mathf::lerp(220.f, 60.f, ss);
            vec2 const h = pos(state, c, 0);
            std::optional<vec2> step_to;
            if (leaving && (a.leave_side < 0 ? at.x <= 1 : at.x >= room.width - 2)) {
                // (the OutsideRoom connection: 10000 tiles out)
                step_to = h + vec2{float(a.leave_side) * 200000.f, 0.f};
            } else {
                auto const f = pathing::field_to(state, room.id, w.creature_get_destination(c), swim);
                if (auto const n = pathing::next_step(state, f, clamp_into(room, at)))
                    step_to = room_view::middle_of_tile(*n);
            }
            if (step_to)
                x.swim_dir = rw::slerp(x.swim_dir, custom::dir_vec(h, *step_to), 0.3f);
            for (uint32_t i = 0; i < chunk_count; ++i) {
                float const num = float(i) / float(chunk_count - 1);
                vel(state, c, i) += x.swim_dir * mathf::lerp(1.f, -1.f, mathf::pow(num, 0.5f)) * 0.125f * ss;
                if (i < chunk_count - 1) {
                    vec2 const v = custom::dir_vec(pos(state, c, i + 1), pos(state, c, i));
                    vel(state, c, i) += v * 0.125f * mathf::lerp(0.5f, 1.f, ss);
                    vel(state, c, i) += custom::perpendicular_vector(v) * std::sin((x.swim_motion + num * 2.f) * mathf::pi * 2.f) * 0.5f * num
                        * mathf::lerp(0.5f, 1.f, ss);
                } else {
                    vel(state, c, i) -= x.swim_dir * ss;
                }
            }
            vel(state, c, 0) += x.swim_dir * 0.15f;
            vel(state, c, 1) -= x.swim_dir * 0.05f * ss;
            ivec2 const cp = w.body_chunk_get_contact_point(chunk(state, c, 0));
            if (cp.x != 0 || cp.y != 0) {
                vel(state, c, 0) += x.swim_dir * 3.f * ss;
                vel(state, c, 1) -= x.swim_dir * 1.5f * ss;
            }
            if (x.has_attack_pos && x.jaw_charge > 0.f) {
                vec2 const ap = x.attack_pos;
                vec2 const c1 = pos(state, c, 1);
                float const num2 = mathf::inverse_lerp(400.f, 150.f, rw::distance(h, ap)) * (1.f - x.jaw_charge_fatigue)
                    * mathf::inverse_lerp(-1.f, 1.f, rw::dot(rw::normalized(c1 - h), rw::normalized(c1 - ap)));
                vel(state, c, 0) *= mathf::lerp(1.f, 0.9f, sub(state, room, c, 0) * num2);
                vel(state, c, 1) *= mathf::lerp(1.f, 0.9f, sub(state, room, c, 1) * num2);
                vec2 const d = custom::dir_vec(ap, h);
                vel(state, c, 0) += rw::clamp_magnitude(ap + d * 60.f - h, 25.f) / 15.f * num2;
                vel(state, c, 1) += rw::clamp_magnitude(ap + d * (60.f + connection_distance(state, c, 0)) - c1, 10.f) / 15.f * num2;
                for (uint32_t j = 2; j < chunk_count; ++j)
                    vel(state, c, j) += d * num2 * sub(state, room, c, j) * 0.5f / mathf::lerp(float(j), 3.f, 0.3f);
            }
            x.has_attack_pos = 0;
        }

        // BigEel.Act
        void act(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& x = pl(state, c);
            auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
            ai_update(state, c, room);
            float const num = x.jaw_charge;
            if (x.jaw_charge == 0.f && x.jaw_charge_fatigue > 0.f)
                x.jaw_charge_fatigue = mathf::max(x.jaw_charge_fatigue - 0.008333334f, 0.f);
            else if (x.jaw_charge > 0.3f || (x.jaw_charge_fatigue < 1.f && want_to_charge_jaw(state, c)))
                x.jaw_charge += 0.008333334f;
            else
                x.jaw_charge = mathf::max(x.jaw_charge - 0.008333334f, 0.f);
            // (Leviathan_Deploy_Jaws, Leviathan_Jaws_Armed: no samples)
            x.snap_frame = 0;
            if (x.jaw_charge > 0.3f && num <= 0.3f && !want_to_snap_jaw(state, c, room, rng)) {
                x.jaw_charge = 0.3f;
                x.jaw_charge_fatigue += 0.025f;
            }
            if (num <= 0.35f && x.jaw_charge > 0.35f) {
                jaws_snap(state, c, room, rng);
                x.jaw_charge_fatigue = 1.f;
            }
            if (x.jaw_charge >= 1.f) {
                x.jaw_charge = 0.f;
                swallow(state, c, room);
            }
            hold_clamped(state, c, room);
            swim_update(state, c, room);
        }

        //
        // Off at sea
        //
        void park(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            float const y = room.pixel_height() * 0.5f + float(c.index() % 16) * 300.f;
            for (uint32_t i = 0; i < chunk_count; ++i) {
                auto const k = chunk(state, c, i);
                physics::hard_set_position(state, k, vec2{-park_distance - float(i) * 40.f, y});
                state.world.body_chunk_set_vel(k, vec2{0.f, 0.f});
            }
        }

        void go_hidden(sys::game_state& state, dcon::creature_id c, room_view const& room, int32_t wait) {
            auto& x = pl(state, c);
            auto& a = ai(state, c);
            // what it still holds goes down with it
            if (state.world.big_eel_data_get_clamped(data(state, c)).count > 0)
                swallow(state, c, room);
            creature::release_all(state, c, room.id);
            x.hidden = 1;
            x.hidden_timer = int16_t(std::min(wait, 30000));
            x.jaw_charge = 0.f;
            x.has_attack_pos = 0;
            a.leave_side = 0;
            a.leave_counter = 0;
            a.escape = 0;
            a.focus = dcon::creature_id{};
            tracker::clear(state, c);
            park(state, c, room);
        }

        // BigEel.NewRoom and BigEelAI.NewRoom (outside the rooms' parallel
        // update: it looks at the neighbouring rooms)
        void new_room(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& x = pl(state, c);
            auto& a = ai(state, c);
            x.jaw_charge = 0.f;
            x.has_attack_pos = 0;
            x.time_in_this_room = 0.f;
            w.big_eel_data_get_clamped(data(state, c)).count = 0;
            w.physical_object_set_impact_threshold(body(state, c), 500.f);
            a.focus = dcon::creature_id{};
            a.roam_timer = 0;
            a.leave_side = 0;
            a.leave_counter = 0;
            a.escape = 0;
            a.can_roam = neighbour_sea_rooms(state, room.id).empty() ? 0 : 1;
            tracker::clear(state, c);
        }

        // back in over a sea side of `room` (serial: the deferred room change)
        void enter_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room) {
            auto& w = state.world;
            auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
            auto& x = pl(state, c);
            auto const from = creature::room_of(state, c);
            auto const view = world::get_room_view(state, room);
            int32_t sides[2];
            int32_t n = 0;
            int32_t lo[2] = {0, 0}, hi[2] = {0, 0};
            for (int32_t side : {-1, 1}) {
                int32_t l = 0, h = 0;
                if (sea_exit(state, view, side, l, h)) {
                    sides[n] = side;
                    lo[n] = l;
                    hi[n] = h;
                    ++n;
                }
            }
            w.physical_object_set_room_from_object_location(body(state, c), room);
            int32_t side = -1, y = std::max(1, view.default_water_level / 2);
            if (n > 0) {
                int32_t const k = rng.range(0, n);
                side = sides[k];
                y = rng.range(lo[k] + 1, std::max(lo[k] + 2, hi[k]));
            }
            float const fs = float(side);
            vec2 const head{side < 0 ? -40.f : view.pixel_width() + 40.f, room_view::middle_of_tile(0, y).y};
            float along = 0.f;
            for (uint32_t i = 0; i < chunk_count; ++i) {
                auto const k = chunk(state, c, i);
                if (i > 0)
                    along += connection_distance(state, c, i - 1);
                physics::hard_set_position(state, k, head + vec2{fs * along, 0.f});
                w.body_chunk_set_vel(k, vec2{-fs * 3.f, 0.f});
            }
            x.swim_dir = vec2{-fs, 0.f};
            x.hidden = 0;
            x.hidden_timer = 0;
            new_room(state, c, view);
            w.creature_set_destination(c, ivec2{side < 0 ? std::min(15, view.width - 1) : std::max(0, view.width - 16), y});
            if (from && from != room)
                dens::changed_rooms(state, c, from, room, false);
            creature::reset_graphics(state, c);
        }

        // BigEelAbstractAI.AddRandomCheckRoom: a neighbouring sea room
        // without a leviathan (now and then one of the region's), else back
        // where it was
        void swim_in(sys::game_state& state, dcon::creature_id c) {
            auto& w = state.world;
            auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
            auto const from = creature::room_of(state, c);
            if (!from)
                return;
            // the rooms with a leviathan out in them
            std::vector<dcon::room_id> taken;
            creature::for_each_of(w, creature::type::big_eel, [&](dcon::creature_id o) {
                if (o != c && !pl(state, o).hidden && w.creature_get_away(o) == 0)
                    taken.push_back(creature::room_of(state, o));
            });
            auto const free = [&](std::vector<dcon::room_id> v) {
                v.erase(std::remove_if(v.begin(), v.end(), [&](dcon::room_id r) { return std::find(taken.begin(), taken.end(), r) != taken.end(); }), v.end());
                return v;
            };
            std::vector<dcon::room_id> picks;
            if (rng.value() > 0.2f)
                picks = free(neighbour_sea_rooms(state, from));
            if (picks.empty()) {
                picks = free(region_sea_rooms(state, w.room_get_region_from_region_membership(from)));
                if (sea_room(state, from) && std::find(picks.begin(), picks.end(), from) == picks.end())
                    picks.push_back(from);
            }
            dcon::room_id target = from;
            if (!picks.empty())
                target = picks[size_t(rng.range(0, int32_t(picks.size())))];
            enter_room(state, c, target);
        }

        void offscreen_update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& x = pl(state, c);
            park(state, c, room);
            // out of the rain and of everything else
            w.creature_set_rain_death(c, 0.f);
            w.creature_set_stun(c, 0);
            if (x.hidden_timer > 0) {
                --x.hidden_timer;
                return;
            }
            // WantToStayInDenUntilEndOfCycle (rainTracker.Utility() > 0.01)
            if (w.creature_get_dead(c) || dens::rain_utility(state, c, room.id) > 0.01f) {
                x.hidden_timer = 200;
                return;
            }
            x.hidden_timer = 1000;
            state.defer(room.id, [c](sys::game_state& st) {
                if (st.world.creature_is_valid(c) && st.world.creature_get_type(c) == creature::type::big_eel && pl(st, c).hidden)
                    swim_in(st, c);
            });
        }

        // GenerateIVars
        individual_variations generate_ivars(int32_t seed, bool albino) {
            individual_variations iv;
            rw::unity_random r{seed};
            float const num = custom::wrapped_random_variation(0.65f, 0.2f, 0.8f, r);
            iv.pattern_displacement = r.value();
            iv.fins_seed = r.range(0, 2147483647);
            float const s = mathf::lerp(0.5f, 0.95f, r.value());
            float const l = mathf::lerp(0.12f, 0.18f, r.value());
            iv.pattern_a = rw::vec3{num, s, l};
            iv.pattern_b = rw::vec3{num + (r.value() < 0.5f ? -0.15f : 0.15f), 1.f, 0.2f};
            if (albino) {
                iv.pattern_b = rw::vec3{0.f, 0.6f, 0.75f};
                iv.pattern_a.x = 0.5f;
                iv.pattern_a = hsl_lerp(iv.pattern_a, rw::vec3{0.97f, 0.8f, 0.75f}, 0.9f);
            }
            return iv;
        }
    }

    bool sea_exit(sys::game_state const& state, room_view const& room, int32_t side, int32_t& lo, int32_t& hi) {
        // ShortcutMapper.MapRoomBorders' SeaExits (borders.hpp): the rows of
        // the longest run of sea exit tiles on that side's edge column
        int32_t const x = side < 0 ? 0 : room.width - 1;
        std::vector<int32_t> ys;
        for (auto const& e : borders::map(state, room)) {
            if (e.type != borders::kind::sea)
                continue;
            for (auto const t : e.tiles)
                if (t.x == x)
                    ys.push_back(t.y);
        }
        std::sort(ys.begin(), ys.end());
        int32_t best_lo = 0, best_len = 0;
        for (size_t i = 0; i < ys.size();) {
            size_t j = i + 1;
            while (j < ys.size() && ys[j] <= ys[j - 1] + 1)
                ++j;
            int32_t const len = ys[j - 1] - ys[i] + 1;
            if (len > best_len) {
                best_len = len;
                best_lo = ys[i];
            }
            i = j;
        }
        if (best_len < min_sea_exit)
            return false;
        lo = best_lo;
        hi = best_lo + best_len - 1;
        return true;
    }

    individual_variations const& ivars(sys::game_state const& state, dcon::creature_id c) noexcept {
        return state.world.big_eel_data_get_ivars(data(state, c));
    }
    creature::big_eel_payload const& state_of(sys::game_state const& state, dcon::creature_id c) noexcept {
        return pl(state, c);
    }
    bool hidden(sys::game_state const& state, dcon::creature_id c) noexcept {
        return pl(state, c).hidden != 0;
    }
    bool voided(sys::game_state const& state, dcon::creature_id c) noexcept {
        return (state.world.creature_get_spawn_flags(c) & dens::flag_void_sea) != 0;
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::big_eel);
        auto const d = data(state, c);
        auto rng = rw::random(state.seed, 0, uint32_t(c.value));
        int32_t seed = int32_t(rng.next() & 0x7FFFFFFFu);
        if (state.spawn_seed >= 0)
            seed = state.spawn_seed;
        // the region's albinos (RegionParams.albinos; no arena here)
        bool albino = false;
        if (auto const region = w.room_get_region_from_region_membership(room))
            albino = w.region_get_albinos(region);
        auto const iv = generate_ivars(seed, albino);
        physics::object_desc desc;
        desc.gravity = 0.9f;
        desc.air_friction = 0.999f;
        desc.bounce = 0.1f;
        desc.surface_friction = 0.4f;
        desc.water_friction = 0.98f;
        desc.buoyancy = 0.9f;
        auto const o = physics::create_object(state, room, desc);
        w.physical_object_set_water_retardation_immunity(o, 0.1f);
        // never inside terrain
        auto const view = world::get_room_view(state, room);
        ivec2 const t0 = tile_of(at);
        if (view.get_tile(t0).solid()) {
            int best = 1 << 30;
            for (int dy = -8; dy <= 8; ++dy)
                for (int dx = -8; dx <= 8; ++dx) {
                    ivec2 const t = t0 + ivec2{dx, dy};
                    if (view.inside(t.x, t.y) && !view.get_tile(t).solid() && dx * dx + dy * dy < best) {
                        best = dx * dx + dy * dy;
                        at = room_view::middle_of_tile(t);
                    }
                }
        }
        // twenty chunks, the head first, laid out along x
        dcon::body_chunk_id ks[chunk_count];
        float rads[chunk_count];
        float along = 0.f;
        for (uint32_t i = 0; i < chunk_count; ++i) {
            float num = float(i) / float(chunk_count - 1);
            num = (1.f - num) * 0.5f + std::sin(mathf::pow(num, 0.5f) * mathf::pi) * 0.5f;
            rads[i] = mathf::lerp(10.f, 60.f, num);
            if (i > 0)
                along += mathf::max(rads[i - 1], rads[i]);
            ks[i] = physics::add_chunk(state, o, at + vec2{-along * 0.25f, 0.f}, rads[i], mathf::lerp(0.5f, 20.f, num));
            w.body_chunk_set_restrict_in_room_range(ks[i], 2000.f);
        }
        for (uint32_t i = 0; i + 1 < chunk_count; ++i)
            physics::add_connection(state, o, ks[i], ks[i + 1], mathf::max(rads[i], rads[i + 1]), physics::connection_type::normal, 1.f, -1.f);
        physics::set_go_through_floors(state, o, true);
        w.try_create_creature_body(c, o);
        auto& x = pl(state, c);
        x.swim_dir = vec2{0.f, 0.f};
        x.attack_pos = vec2{0.f, 0.f};
        x.swim_motion = 0.f;
        x.jaw_charge = 0.f;
        x.jaw_charge_fatigue = 0.f;
        x.beak_gap = 0.f;
        x.swim_speed = 0.f;
        x.time_in_this_room = 0.f;
        // BigEelAI: hungerDelay 300
        x.hunger_delay = 300;
        x.hidden_timer = 0;
        x.has_attack_pos = 0;
        x.snap_frame = 0;
        x.albino = albino ? 1 : 0;
        x.hidden = 0;
        x.data = uint16_t(d.index());
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_set_grasps(c, creature::grasp_pair{});
        w.creature_set_behavior(c, uint8_t(behavior::idle));
        w.creature_set_current_utility(c, 0.f);
        w.creature_set_destination(c, tile_of(at));
        w.big_eel_data_set_seed(d, seed);
        w.big_eel_data_set_ivars(d, iv);
        w.big_eel_data_set_clamped(d, clamped_list{});
        w.big_eel_data_set_ai(d, ai_state{});
        w.big_eel_data_set_layout(d, big_eel_graphics::make_layout(iv.fins_seed));
        w.big_eel_data_set_graphics(d, graphics_state{});
        new_room(state, c, view);
        big_eel_graphics::reset_positions(state, c);
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        creature::release_target(state, c);
        creature::erase(state, c);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto& x = pl(state, c);
        if (x.hidden) {
            offscreen_update(state, c, room);
            return;
        }
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        auto const o = body(state, c);
        float& impact = w.physical_object_get_impact_threshold(o);
        if (impact > 1.f)
            impact -= 1.f;
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
        creature::stun_countdown(state, c);
        // PhysicalObject.Update (no Leviathan terrain impact sounds)
        physics::update_object(state, room, o, nullptr, nullptr);
        anti_stranding(state, c, room);
        // every chunk kept from the one two along
        for (uint32_t l = 0; l + 2 < chunk_count; ++l) {
            vec2 const a = custom::dir_vec(pos(state, c, l), pos(state, c, l + 2));
            float const num = mass(state, c, l + 2) / (mass(state, c, l) + mass(state, c, l + 2));
            vel(state, c, l) -= a * 0.15f * num;
            vel(state, c, l + 2) += a * 0.15f * (1.f - num);
        }
        bool const awake = consious(state, c);
        if (awake)
            act(state, c, room);
        // (CarryObject is empty: the jaws hold what they caught)
        // on the way out: off at sea once the whole body is past the border
        auto& a = ai(state, c);
        if (awake && a.leave_side != 0 && behavior(w.creature_get_behavior(c)) != behavior::hunt) {
            ++a.leave_counter;
            bool out = true;
            for (uint32_t i = 0; i < chunk_count && out; ++i) {
                float const px = pos(state, c, i).x;
                float const r = rad(state, c, i);
                out = a.leave_side < 0 ? px + r < -20.f : px - r > room.pixel_width() + 20.f;
            }
            // (not vanilla: one that can't find its way out goes all the same)
            if (out || a.leave_counter > 2400) {
                bool const escape = a.escape != 0;
                go_hidden(state, c, room, escape ? rng.range(400, 1200) : rng.range(200, 800));
            }
        }
    }

    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) {
        // (what is in the jaws is in collision layer 0: ChangeCollisionLayer)
        return !hidden(state, c) && !holding(state, c, t);
    }

    void collide(sys::game_state&, dcon::creature_id, room_view const&, uint32_t, dcon::creature_id, uint32_t) {
        // BigEel.Collide: Creature.Collide, nothing of its own
    }

    void violence(sys::game_state& state, dcon::creature_id c, uint32_t k, vec2 momentum, float damage, float stun_bonus, creature::damage_type) {
        auto& w = state.world;
        if (hidden(state, c))
            return;
        // the hit chunk in the key: two hits of a tick roll apart
        auto rng = rw::random(state.seed, state.tick, rw::ids(uint32_t(c.value), k));
        k = k < chunk_count ? k : 0u;
        auto const hit = chunk(state, c, k);
        w.body_chunk_get_vel(hit) += rw::clamp_magnitude(momentum / w.body_chunk_get_mass(hit), 10.f);
        float const dmg = damage / base_damage_resistance;
        float st = (damage * 30.f + stun_bonus) / base_stun_resistance;
        float& health = w.creature_get_health(c);
        st *= 1.5f + mathf::inverse_lerp(0.5f, 0.f, health) * rng.value();
        // (BigEel.Stun's LoseAllGrasps lets go of nothing: its grasps hold
        // nothing, the jaws aren't grasps)
        creature::stun(state, c, int32_t(st));
        health -= dmg;
        if (rng.value() < -health || health < -1.f || (health < 0.f && rng.value() < 0.33f))
            creature::die(state, c);
    }

    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t) {
        auto& cl = state.world.big_eel_data_get_clamped(data(state, c));
        for (uint32_t i = 0; i < cl.count;) {
            if (cl.o[i].who == t)
                cl.o[i] = cl.o[--cl.count];
            else
                ++i;
        }
        auto& a = ai(state, c);
        if (a.focus == t)
            a.focus = dcon::creature_id{};
    }

    void died(sys::game_state&, dcon::creature_id) {
        // (BigEel.Die's swallowing and jaw reset are Remix's)
    }

    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const view = world::get_room_view(state, room);
        ivec2 const t = tile_of(at);
        vec2 const a = rw::to_vec2(view.shortcut_entrance_hole_direction(t));
        vec2 const mid = room_view::middle_of_tile(t);
        for (uint32_t i = 0; i < chunk_count; ++i) {
            auto const k = chunk(state, c, i);
            physics::hard_set_position(state, k, mid - a * (-1.5f + float(i)) * 5.f);
            w.body_chunk_set_last_pos(k, mid);
            w.body_chunk_set_vel(k, a * 2.f);
        }
        auto& x = pl(state, c);
        x.hidden = 0;
        x.hidden_timer = 0;
        x.swim_dir = a;
        new_room(state, c, view);
    }

    std::string describe(sys::game_state const& state, dcon::creature_id c) {
        static constexpr char const* names[] = {"idle", "hunt", "escape rain", "return prey"};
        auto const& w = state.world;
        auto const& x = pl(state, c);
        if (x.hidden)
            return "leviathan off at sea (back in " + std::to_string(int(x.hidden_timer)) + " ticks)";
        vec2 const p = pos(state, c, 0);
        uint32_t const b = w.creature_get_behavior(c);
        auto const& a = w.big_eel_data_get_ai(data(state, c));
        auto const& cl = w.big_eel_data_get_clamped(data(state, c));
        std::string s = "leviathan at " + std::to_string(int(p.x)) + "," + std::to_string(int(p.y)) + " behavior " + (b < 4 ? names[b] : "?");
        if (x.jaw_charge > 0.f)
            s += " jaws " + std::to_string(int(x.jaw_charge * 100.f)) + "%";
        if (cl.count > 0)
            s += " holding " + std::to_string(int(cl.count));
        if (a.leave_side != 0)
            s += a.leave_side < 0 ? " leaving left" : " leaving right";
        return s;
    }

    bool spawn_spot(room_view const& room, ivec2 tile) {
        if (!room.inside(tile.x, tile.y) || room.get_tile(tile).solid() || room.default_water_level < 10)
            return false;
        if (tile.y > room.default_water_level - 4)
            return false;
        return pathing::terrain_proximity(room, tile, 4) >= 4;
    }

    uint32_t spawn_offscreen(sys::game_state& state, dcon::region_id region, spawns::spawn const& s, spawns::report& r) {
        auto& w = state.world;
        auto const rooms = region_sea_rooms(state, region);
        if (rooms.empty()) {
            ++r.no_room;
            if (r.missing.size() < 8)
                r.missing.push_back(s.room + ":" + std::to_string(s.den) + " " + s.type);
            return 0;
        }
        auto const sd = spawns::parse_data(s.data);
        uint8_t flags = sd.flags;
        if (s.night)
            flags = uint8_t((flags | dens::flag_night) & ~dens::flag_ignore_cycle);
        uint32_t made = 0;
        int32_t const n = std::max(1, s.count);
        for (int32_t i = 0; i < n; ++i) {
            // RainWorldGame.GetNewID: the next number, the seed unless {Seed:N}
            int32_t const number = ++state.next_issued_id;
            int32_t const seed = sd.seed >= 0 ? sd.seed : number;
            auto const room = rooms[size_t(uint32_t(seed) * 2654435761u % uint32_t(rooms.size()))];
            state.spawn_seed = seed;
            auto const view = world::get_room_view(state, room);
            auto const c = create(state, room, vec2{view.pixel_width() * 0.5f, view.pixel_height() * 0.25f});
            state.spawn_seed = -1;
            if (!c)
                continue;
            w.creature_set_spawn_flags(c, flags);
            // AbstractCreature's constructor: out of the den in 100 to 1000 ticks
            go_hidden(state, c, view, rw::random(state.seed, 0, uint32_t(c.value)).range(100, 1000));
            ++made;
            ++r.made["leviathan"];
            ++r.creatures;
            r.night += (flags & dens::flag_night) ? 1 : 0;
            r.pre_cycle += (flags & dens::flag_pre_cycle) ? 1 : 0;
            r.ignore_cycle += (flags & dens::flag_ignore_cycle) ? 1 : 0;
        }
        return made;
    }
}
