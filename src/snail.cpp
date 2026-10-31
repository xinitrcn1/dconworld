#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <string>

#include "snail.hpp"
#include "barnacle.hpp"
#include "dens.hpp"
#include "items.hpp"
#include "migrate.hpp"
#include "particles.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "rooms.hpp"
#include "snail_graphics.hpp"
#include "tracker.hpp"
#include "water.hpp"

namespace snail {
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    using world::terrain;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // CreatureTemplate (Snail): baseDamageResistance, baseStunResistance,
        // instantDeathDamageLimit, visualRadius, throughSurfaceVision
        // (waterVision 1 takes nothing off)
        constexpr float base_damage_resistance = 0.4f;
        constexpr float base_stun_resistance = 0.8f;
        constexpr float instant_death_damage_limit = 1.f;
        constexpr float visual_radius = 300.f;
        constexpr float through_surface_vision = 0.5f;
        // Tracker(10, 5, -1, 0.5, 5, 5, 10): seeAroundCorners
        constexpr int32_t see_around_corners = 10;
        // PhysicalObject.gravity, buoyancy (the Act's anti-gravity uses them)
        constexpr float own_gravity = 0.9f;
        constexpr float own_buoyancy = 0.8f;

        creature::snail_payload& pl(sys::game_state& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).snail;
        }
        creature::snail_payload const& pl(sys::game_state const& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).snail;
        }
        dcon::snail_data_id data(sys::game_state const& state, dcon::creature_id c) {
            return creature::snail_data(state.world, c);
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
        ivec2 contact(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_contact_point(chunk(state, c, i));
        }
        bool solid_at(sys::game_state const& state, room_view const& room, dcon::creature_id c, uint32_t i, int32_t dx, int32_t dy) {
            return physics::is_tile_solid(state, room, chunk(state, c, i), dx, dy);
        }
        ivec2 tile_of(vec2 p) {
            return room_view::get_tile_position(p);
        }
        // abstractCreature.pos: the main chunk's tile
        ivec2 here(sys::game_state const& state, dcon::creature_id c) {
            return tile_of(pos(state, c, 0));
        }
        // AImap.TileAccessibleToCreature(mainBodyChunk.pos, Template)
        bool accessible_at(room_view const& room, vec2 p) {
            ivec2 const t = tile_of(p);
            return room.inside(t.x, t.y) && pathing::accessible(room, t, crawl);
        }

        //
        // SnailAI
        //
        tracker::params const& tracker_params() {
            static tracker::params const p = [] {
                tracker::params q;
                q.see_around_corners = see_around_corners;
                q.frames_to_remember = -1;
                q.ghost_dismissal_range = 10;
                return q;
            }();
            return p;
        }

        // ArtificialIntelligence.VisualScore (> 0) + Room.VisualContact
        bool visual_contact(sys::game_state const& state, dcon::creature_id c, room_view const& room, vec2 look) {
            vec2 const eye = pos(state, c, 0);
            if (!custom::dist_less(eye, look, visual_radius))
                return false;
            float score = mathf::inverse_lerp(visual_radius, 0.f, rw::distance(eye, look));
            if (room.water && (room.get_tile(eye).water == 1) != (room.get_tile(look).water == 1))
                score -= 1.f - through_surface_vision;
            if (pathing::narrow_space(room, tile_of(look)))
                score -= 0.5f;
            return score > 0.f && room.visual_contact(eye, look);
        }

        struct hook_ctx {
            sys::game_state& state;
            dcon::creature_id c;
            room_view const& room;
            // the field to where it is (CoordinateReachable and
            // CoordinatePossibleToGetBackFrom in one), built on first use
            std::optional<pathing::field_id> from_here;
        };
        bool reachable(hook_ctx& x, ivec2 t) {
            if (!x.room.inside(t.x, t.y) || !pathing::accessible(x.room, t, crawl))
                return false;
            if (!x.from_here)
                x.from_here = pathing::field_to(x.state, x.room.id, here(x.state, x.c), crawl);
            return pathing::distance(x.state, *x.from_here, t) != pathing::unreachable;
        }
        bool hook_sees(void* u, dcon::body_chunk_id k) {
            auto& x = *static_cast<hook_ctx*>(u);
            return visual_contact(x.state, x.c, x.room, x.state.world.body_chunk_get_pos(k));
        }
        bool hook_sees_point(void* u, vec2 p) {
            auto& x = *static_cast<hook_ctx*>(u);
            return visual_contact(x.state, x.c, x.room, p);
        }
        bool hook_reach(void* u, ivec2 t) {
            return reachable(*static_cast<hook_ctx*>(u), t);
        }
        // ModuleToTrackRelationship: null for everything
        creature::track_module hook_module(creature::relationship) {
            return creature::track_module::none;
        }
        tracker::hooks make_hooks(hook_ctx& x) {
            tracker::hooks h;
            h.user = &x;
            h.sees = hook_sees;
            h.sees_point = hook_sees_point;
            // UpdateDynamicRelationship: the static one (relate left null)
            h.module = hook_module;
            h.reach = hook_reach;
            return h;
        }

    }

    // SnailAI.CreatureUnease (public for the graphics' CreatureLooker)
    float creature_unease(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) {
        auto const& w = state.world;
        if (!creature::live(state, t) || creature::dead(state, t))
            return 0.f;
        auto const rel = creature::static_relationship(state, c, t);
        if (rel.type == creature::rel::ignores)
            return 0.f;
        float num = 1.f;
        if (creature::is(w, t, creature::type::leech)) {
            // a leech on the ground is harmless
            if (w.body_chunk_get_contact_point(creature::chunk_of(state, t, creature::main_chunk_of(state, t))).y < 0)
                return 0.f;
            num = 10.f - pl(state, c).scared * 7.f;
        }
        num *= creature::body_size(creature::template_of(state, t));
        num *= mathf::lerp(rel.intensity, 1.f, 0.5f);
        if (!creature::consious(state, t))
            num /= 20.f;
        // (the AirOnly/WaterOnly thirds aren't ported: no water relationships here)
        return num;
    }

    namespace {
        // SnailAI.TileIdleScore
        float tile_idle_score(hook_ctx& x, ivec2 t) {
            auto& state = x.state;
            auto const& w = state.world;
            auto const c = x.c;
            if (!reachable(x, t))
                return -3.4e38f;
            float num = 0.f;
            if (pathing::narrow_space(x.room, t))
                num -= 100.f;
            // (ExitDistanceForCreature to its den, as tiles to the den's pipe)
            if (w.creature_get_home_room(c) == x.room.id && w.creature_get_home_den(c) != dens::no_den)
                if (auto const e = dens::den_entrance(state, x.room.id, w.creature_get_home_den(c)))
                    num += mathf::min(float(custom::manhattan_distance(t, *e)), 15.f) * 0.01f;
            bool leech = false;
            auto const& ts = tracker::of(state, c);
            for (uint32_t i = 0; i < ts.count; ++i) {
                auto const& r = ts.reps[i];
                if (!creature::live(state, r.who))
                    continue;
                ivec2 const g = tracker::best_guess(r);
                auto const rel = creature::static_relationship(state, c, r.who);
                if (rel.type != creature::rel::ignores) {
                    if (r.visual)
                        num -= 200.f * rel.intensity / float(custom::manhattan_distance(t, g) + 1);
                    else
                        num += float(custom::manhattan_distance(t, g));
                    num *= creature_unease(state, c, r.who);
                    if (creature::is(w, r.who, creature::type::leech))
                        leech = true;
                } else if (creature::is(w, r.who, creature::type::snail) && r.who != c && custom::manhattan_distance(t, g) < 1) {
                    num -= 20.f;
                }
            }
            if (leech)
                num += x.room.get_tile(t).water != 0 ? -100.f : 100.f;
            // (aimap.Visibility isn't ported: the factor is 1)
            return num;
        }

        // a random tile of the room (SnailAI.NewRoom, MMF's shuffle)
        ivec2 random_tile(rw::rng& rng, room_view const& room) {
            int32_t const x = rng.range(0, room.width);
            return ivec2{x, rng.range(0, room.height)};
        }

        // SnailAI.Update
        void ai_update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
            auto& x = pl(state, c);
            ivec2 const at = here(state, c);
            hook_ctx hx{state, c, room, std::nullopt};
            tracker::hooks const h = make_hooks(hx);
            tracker::update(state, c, room, at, tracker_params(), h);
            ivec2& dest = w.creature_get_destination(c);
            // home before the rain (DenFinder), sooner the less scared it is
            bool flag = false;
            if (mathf::lerp(0.01f, 0.9f, x.scared) < dens::rain_utility(state, c, room.id)) {
                if (auto const e = dens::home_tile(state, c, room)) {
                    flag = true;
                    x.move = 1;
                    dest = *e;
                }
            }
            bool travelling = false;
            if (!flag) {
                // (the abstract AI's migration, now and then; the way out
                // its destination while it has a trip)
                migrate::idle_roll(state, c, room);
                if (auto const e = migrate::travel_tile(state, c, room)) {
                    travelling = true;
                    dest = *e;
                } else {
                    ivec2 const t{at.x + rng.range(-10, 11), at.y + rng.range(-10, 11)};
                    if (room.inside(t.x, t.y) && room.get_tile(t).t == terrain::air && tile_idle_score(hx, t) > tile_idle_score(hx, dest)
                    && room.visual_contact(pos(state, c, 0), room_view::middle_of_tile(t)))
                        dest = t;
                }
            }
            if (!flag && !travelling && at == dest)
                x.move = 0;
            else
                x.move = 1;
            // scared: of the Uncomfortable creatures close by
            float num = 0.f;
            auto const& ts = tracker::of(state, c);
            vec2 const m = pos(state, c, 0);
            for (uint32_t i = 0; i < ts.count; ++i) {
                auto const& r = ts.reps[i];
                if (!creature::live(state, r.who) || creature::static_relationship(state, c, r.who).type != creature::rel::uncomfortable)
                    continue;
                float const u = creature_unease(state, c, r.who);
                vec2 const g = room_view::middle_of_tile(tracker::best_guess(r));
                if (custom::dist_less(m, g, 300.f * u))
                    num += mathf::inverse_lerp(300.f * u, 100.f * u, rw::distance(m, g)) * mathf::lerp(u, 1.f, 0.5f)
                         * tracker::chance_of_finding(r, see_around_corners);
            }
            // FloatTweenUpAndDown(Lerp 0.1, Tick 1/120)
            if (num != x.scared) {
                float const target = mathf::min(num, 5.f);
                float s = x.scared;
                if (s < target)
                    s = mathf::lerp(s, target, 0.1f);
                else if (s != target)
                    s = mathf::max(s - 0.008333334f, target);
                x.scared = mathf::clamp(s, 0.f, 1.f);
            }
            if (x.scared > 0.2f && !flag)
                x.move = 0;
        }

        // SnailAI.CollideWithSnail
        void collide_with_snail(sys::game_state& state, dcon::creature_id c, room_view const& room, dcon::creature_id t) {
            auto& w = state.world;
            // the other snail in the key: each collision of a tick rolls apart
            auto rng = rw::random(state.seed, state.tick, rw::ids(uint32_t(c.value), uint32_t(t.value)));
            if (!pl(state, c).move || rng.value() >= 0.1f)
                return;
            ivec2 const at = here(state, c);
            int32_t const dx = rng.range(-10, 11);
            int32_t const dy = rng.range(-10, 11);
            w.creature_set_destination(c, ivec2{custom::int_clamp(at.x + dx, 0, room.width - 1), custom::int_clamp(at.y + dy, 0, room.height - 1)});
        }

        //
        // Snail
        //
        // Creature.Stun
        void stun(sys::game_state& state, dcon::creature_id c, int32_t st) {
            creature::stun(state, c, st);
        }

        // the drips and bubbles of the clicks (WaterDrip, Bubble: shared particles)
        void spray(sys::game_state& state, dcon::creature_id c, room_view const& room, int n, float lo, float hi, bool bubbles) {
            if (!state.viewed(room.id))
                return;
            // (the count and kind in the key: a mini click and a click of a tick spray apart)
            auto rng = rw::random(state.seed, state.tick, rw::ids(uint32_t(c.value), uint32_t(n) * 2u + (bubbles ? 1u : 0u)));
            vec2 const at = pos(state, c, 1);
            for (int i = 0; i < n; ++i) {
                vec2 const v = custom::deg_to_vec(rng.value() * 360.f) * mathf::lerp(lo, hi, rng.value());
                if (bubbles)
                    particles::spawn(state, room.id, particles::kind::bubble, at, v * 0.25f, mathf::lerp(1.2f, 3.2f, rng.value()), 2.f,
                        rw::vec3{0.62f, 0.72f, 0.85f}, 0.8f);
                else
                    particles::spawn(state, room.id, particles::kind::drop, at, v * 0.35f, mathf::lerp(0.8f, 1.6f, rng.value()), 2.f,
                        rw::vec3{0.62f, 0.72f, 0.85f}, 0.85f);
            }
        }

        // Snail.VibrateLeeches + Leech.HeardSnailClick (the push away; the
        // leeches' afraidOfPositions aren't ported)
        void vibrate_leeches(sys::game_state& state, dcon::creature_id c, room_view const& room, float rad) {
            if (sub(state, room, c, 1) <= 0.5f)
                return;
            auto& w = state.world;
            vec2 const m = pos(state, c, 0);
            rooms::for_each_creature(state, room.id, creature::type::leech, [&](dcon::creature_id l) {
                if (!creature::here(state, l, room.id))
                    return;
                auto const lk = creature::chunk(state, l, creature::main_chunk(state, l));
                vec2 const lp = w.body_chunk_get_pos(lk);
                if (!custom::dist_less(m, lp, rad) || !(custom::dist_less(m, lp, rad / 4.f) || room.visual_contact(m, lp)))
                    return;
                if (physics::submersion(state, room, lk) < 0.5f)
                    return;
                vec2& lv = w.body_chunk_get_vel(lk);
                lv = rw::lerp(lv, custom::dir_vec(m, lp) * 6.f, creature::consious(state, l) ? 0.5f : 0.2f);
            });
        }

        // Snail.MiniClick: a warning click
        void mini_click(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
            // (no Snail_Warning_Click sound)
            vel(state, c, 1) += custom::deg_to_vec(rng.value() * 360.f) * 3.f;
            pl(state, c).bloated = 1;
            spray(state, c, room, 3, 4.f, 21.f, sub(state, room, c, 1) >= 0.5f);
            vibrate_leeches(state, c, room, 250.f);
        }

        // Snail.FindSuckPoint: falling onto something to hold on to
        void find_suck_point(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& x = pl(state, c);
            vel(state, c, 0).y -= 0.3f;
            if (contact(state, c, 0).y < 0 && accessible_at(room, pos(state, c, 0))) {
                // (no Snail_Attatch_To_Surface sound)
                x.suck_point = room_view::middle_of_tile(pos(state, c, 0));
                x.has_suck_point = 1;
            }
        }

        // Snail.Drop: over the tile it drops to, until it lands
        void drop(sys::game_state& state, dcon::creature_id c) {
            auto& x = pl(state, c);
            vec2 const m = pos(state, c, 0);
            vel(state, c, 0).x += mathf::clamp(room_view::middle_of_tile(x.drop_to_tile).x - m.x, -5.f, 5.f) * 0.1f;
            x.has_suck_point = 0;
            for (uint32_t i = 0; i < 2; ++i)
                if (contact(state, c, i).y < 0) {
                    x.has_drop_to_tile = 0;
                    return;
                }
        }

        // the bodyChunkConnections[0] (Pull) distance: out of the shell or in
        void set_pull_distance(sys::game_state& state, dcon::creature_id c, float d) {
            auto& w = state.world;
            w.physical_object_for_each_chunk_connection_as_owner(body(state, c), [&](dcon::chunk_connection_id k) {
                if (w.chunk_connection_get_type(k) == physics::connection_type::pull)
                    w.chunk_connection_set_distance(k, d);
            });
        }

        // PathFinder.FollowPath's MovementConnection, from the flow field:
        // the step's kind and its tiles
        enum class conn_kind : uint8_t { none, standard, open_diagonal, slope, ceiling_slope, drop_to_floor };
        struct connection {
            conn_kind kind = conn_kind::none;
            ivec2 start{0, 0};
            ivec2 dest{0, 0};
        };
        connection follow_path(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            connection out;
            ivec2 const at = here(state, c);
            auto const f = pathing::field_to(state, room.id, state.world.creature_get_destination(c), crawl);
            auto const n = pathing::next_step(state, f, at);
            if (!n)
                return out;
            out.start = at;
            out.dest = *n;
            ivec2 const d = *n - at;
            if (d.x == 0 && d.y < -1) {
                out.kind = conn_kind::drop_to_floor;
            } else if (d.x != 0 && d.y != 0) {
                out.kind = conn_kind::open_diagonal;
                // a slope tile at either end: along the slope
                for (ivec2 const t : {at, *n})
                    if (room.get_tile(t).t == terrain::slope) {
                        auto const sl = room.identify_slope(t);
                        if (sl == world::slope_direction::up_left || sl == world::slope_direction::up_right)
                            out.kind = conn_kind::slope;
                        else if (sl == world::slope_direction::down_left || sl == world::slope_direction::down_right)
                            out.kind = conn_kind::ceiling_slope;
                        break;
                    }
            } else {
                out.kind = conn_kind::standard;
            }
            return out;
        }

        // Snail.Act
        void act(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
            auto& x = pl(state, c);
            if (!x.has_suck_point) {
                find_suck_point(state, c, room);
                return;
            }
            set_pull_distance(state, c, (x.move ? 12.f : 7.f) * mathf::lerp(x.size, 1.f, 0.5f));
            x.just_clicked = 0;
            float const before = x.click_counter;
            if (x.scared > 0.3f)
                x.click_counter += mathf::inverse_lerp(0.3f, 0.9f, x.scared) / 200.f;
            else if (x.click_counter > 0.92f)
                x.click_counter += 0.0033333334f;
            else
                x.click_counter -= 0.01f;
            x.click_counter = mathf::clamp(x.click_counter, 0.f, 1.f);
            if (before < x.click_counter) {
                auto const& lk = w.snail_data_get_looks(data(state, c));
                for (uint32_t i = 0; i < mini_click_count; ++i)
                    if (before < lk.mini_clicks[i] && x.click_counter >= lk.mini_clicks[i]) {
                        mini_click(state, c, room);
                        break;
                    }
            }
            if (x.click_counter == 1.f && rng.value() < 0.6666667f) {
                click(state, c, room);
                return;
            }
            // held on to its surface: gravity cancelled, damped
            float const s0 = sub(state, room, c, 0);
            vel(state, c, 0).y += mathf::lerp(own_gravity, own_gravity - own_buoyancy, s0);
            vel(state, c, 0) *= 0.5f;
            vel(state, c, 1).y += mathf::lerp(own_gravity, own_gravity - own_buoyancy, s0) * 0.5f;
            vel(state, c, 1) *= 0.9f;
            x.breathing_force *= 0.96f;
            x.breathing_force += custom::deg_to_vec(rng.value() * 360.f) / 60.f;
            x.breathing_force = rw::clamp_magnitude(x.breathing_force, 1.f);
            vel(state, c, 1) += x.breathing_force * 0.1f;
            ai_update(state, c, room);
            if (!x.move || !x.has_suck_point)
                return;
            auto const conn = follow_path(state, c, room);
            if (conn.kind == conn_kind::none)
                return;
            if (conn.kind == conn_kind::slope || conn.kind == conn_kind::ceiling_slope || conn.kind == conn_kind::open_diagonal)
                x.allowed_in_non_accessible_tile = 20;
            else if (conn.kind == conn_kind::drop_to_floor) {
                x.drop_to_tile = conn.dest;
                x.has_drop_to_tile = 1;
            }
            vec2 shell{0.f, 0.f};
            if (conn.kind == conn_kind::standard) {
                if (conn.start.x != conn.dest.x) {
                    if (solid_at(state, room, c, 0, 0, -1))
                        shell.y = 1.f;
                    else if (solid_at(state, room, c, 0, 0, 1))
                        shell.y = -1.f;
                } else if (solid_at(state, room, c, 0, -1, 0) && !solid_at(state, room, c, 0, 1, 0)) {
                    shell.x = 1.f;
                } else if (solid_at(state, room, c, 0, 1, 0) && !solid_at(state, room, c, 0, -1, 0)) {
                    shell.x = -1.f;
                }
            } else if (conn.kind == conn_kind::slope || conn.kind == conn_kind::ceiling_slope) {
                shell = custom::perpendicular_vector(rw::normalized(rw::to_vec2(conn.dest - conn.start)));
                if (conn.kind == conn_kind::ceiling_slope)
                    shell = -shell;
                if (conn.dest.x < conn.start.x)
                    shell *= -1.f;
            }
            x.shell_direction = shell;
            vec2 const dest_mid = room_view::middle_of_tile(conn.dest);
            if (contact(state, c, 1).x != 0 && conn.start.x != conn.dest.x && conn.start.y == conn.dest.y)
                vel(state, c, 1).y -= (pos(state, c, 1).y - dest_mid.y) * 0.2f;
            else if (contact(state, c, 1).y != 0 && conn.start.x == conn.dest.x && conn.start.y != conn.dest.y)
                vel(state, c, 1).x -= (pos(state, c, 1).x - dest_mid.x) * 0.2f;
            vel(state, c, 1) += x.shell_direction * 0.75f;
            // (shortcuts: dens.hpp takes it into its pipe when it goes home)
            physics::set_go_through_floors(state, body(state, c), conn.dest.y < conn.start.y);
            ivec2 const mt = here(state, c);
            if (pathing::tile_access(room, mt) == pathing::access::climb && !room.get_tile(mt).vertical_beam())
                x.shell_direction = vec2{0.f, 1.8f};
            vec2 const v = custom::dir_vec(pos(state, c, 0), dest_mid - x.shell_direction * 5.f) * 1.1f;
            vel(state, c, 1) -= v * 0.2f;
            if (custom::dist_less(pos(state, c, 0), x.suck_point + v, 10.f))
                x.suck_point += v;
            else
                x.suck_point += custom::dir_vec(x.suck_point, pos(state, c, 0));
        }

        struct impact_ctx {
            sys::game_state* state;
            dcon::creature_id c;
        };
        // Snail.TerrainImpact: a hard landing sets it off, a lighter one
        // nearly (no Snail_Light/Heavy_Terrain_Impact sounds)
        void terrain_impact(void* user, dcon::physical_object_id, uint32_t, ivec2, float speed, bool first_contact) {
            auto& ix = *static_cast<impact_ctx*>(user);
            auto& x = pl(*ix.state, ix.c);
            if (!x.just_clicked && speed > 20.f && first_contact)
                x.triggered = 1;
            else if (!x.just_clicked && speed > 5.f && x.click_counter < 0.92f)
                x.click_counter = 0.92f;
        }

        // Snail.Collide's side for the creature it touched
        void collide_with(sys::game_state& state, dcon::creature_id c, room_view const& room, dcon::creature_id t) {
            auto& x = pl(state, c);
            bool const is_snail = creature::is(state.world, t, creature::type::snail);
            if (x.triggered) {
                click(state, c, room);
            } else if (consious(state, c)) {
                if (is_snail) {
                    collide_with_snail(state, c, room, t);
                    if (x.move && pathing::narrow_space(room, tile_of(pos(state, t, 0))))
                        x.click_counter += 0.005f;
                } else {
                    x.click_counter += 0.025f;
                }
            }
            if (x.has_suck_point && !is_snail && !custom::dist_less(pos(state, c, 0), x.suck_point, 6.f))
                x.has_suck_point = 0;
        }
    }

    void click(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto& x = pl(state, c);
        if (x.trigger_ticker > 0)
            return;
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        auto const shell = chunk(state, c, 1);
        float const shell_sub = physics::submersion(state, room, shell);
        // (no ShockWave; the WaterDrips out of the water)
        if (shell_sub < 1.f)
            spray(state, c, room, 10, 4.f, 21.f, false);
        else
            spray(state, c, room, 10, 4.f, 11.f, true);
        stun(state, c, 60);
        // (the Inv's exploding snails in DS aren't ported)
        x.click_counter = 0.f;
        // (no Snail_Pop sound)
        float const num = 60.f * x.size;
        vec2 const sp = w.body_chunk_get_pos(shell);
        float const srad = w.body_chunk_get_rad(shell);
        // the push is from a bit under the shell on the ground
        vec2 const from = sp + vec2{0.f, solid_at(state, room, c, 1, 0, -1) ? -20.f : 0.f};
        // the blast on a body chunk: pushed away, and how hard (0 out of
        // reach or out of sight); under the water both ways it reaches
        // (and pushes) up to 5.5 times as far
        auto const fling = [&](dcon::body_chunk_id k) {
            vec2 const kp = w.body_chunk_get_pos(k);
            float const num2 = 1.f + physics::submersion(state, room, k) * shell_sub * 4.5f;
            float const reach = num * num2 + w.body_chunk_get_rad(k) + srad;
            if (!custom::dist_less(kp, sp, reach) || !room.visual_contact(kp, sp))
                return 0.f;
            float const num3 = mathf::inverse_lerp(reach, reach / 2.f, rw::distance(kp, sp));
            w.body_chunk_get_vel(k) += custom::dir_vec(from, kp) * num3 * num2 * 3.f / w.body_chunk_get_mass(k);
            return num3;
        };
        // every creature of the room (the slugcats and the leeches included)
        rooms::for_each_creature(state, room.id, [&](dcon::creature_id t) {
            if (t == c || !creature::here(state, t, room.id))
                return;
            bool const leech = creature::is(w, t, creature::type::leech);
            uint32_t const n = creature::chunk_count(state, t);
            for (uint32_t i = 0; i < n; ++i) {
                auto const k = creature::chunk(state, t, i);
                float const num3 = fling(k);
                if (num3 <= 0.f)
                    continue;
                // Player.Stun, Creature.Stun (the Saint's SaintStagger is a
                // plain stun here; no Remix stun increase)
                creature::stun(state, t, int32_t(60.f * num3));
                if (leech && !creature::dead(state, t)) {
                    if (rng.value() < 0.033333335f || custom::dist_less(sp, w.body_chunk_get_pos(k), srad + w.body_chunk_get_rad(k) + 5.f))
                        creature::kill(state, t, creature::tmpl::snail);
                    else
                        creature::stun(state, t, int32_t(num3 * physics::submersion(state, room, k) * mathf::lerp(800.f, 900.f, rng.value())));
                }
                // Barnacle.LoseShell
                if (creature::is(w, t, creature::type::barnacle) && barnacle::has_shell(state, t))
                    barnacle::lose_shell(state, t);
            }
        });
        // and the items (pushed only)
        rooms::for_each_item(state, room.id, [&](dcon::item_id it) { fling(items::chunk(state, it)); });
        // waterObject.Explosion: a wave on the surface
        if (room.water) {
            float const num4 = 1.f + shell_sub * 1.5f;
            water::disturb(state, room.id, sp.x, num4 * 3.f, num * num4 * 1.2f);
        }
        x.has_suck_point = 0;
        for (uint32_t i = 0; i < 2; ++i) {
            if (solid_at(state, room, c, i, 0, -1))
                vel(state, c, i) += custom::deg_to_vec(-50.f + 100.f * rng.value()) * 10.f;
            else
                vel(state, c, i) += custom::deg_to_vec(rng.value() * 360.f) * 10.f;
        }
        vibrate_leeches(state, c, room, 1000.f);
        x.just_clicked = 1;
        x.bloated = 1;
        x.triggered = 0;
    }

    looks const& looks_of(sys::game_state const& state, dcon::creature_id c) noexcept {
        return state.world.snail_data_get_looks(data(state, c));
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::snail);
        auto const d = data(state, c);
        auto rng = rw::random(state.seed, 0, uint32_t(c.value));
        int32_t seed = int32_t(rng.next() & 0x7FFFFFFFu);
        if (state.spawn_seed >= 0)
            seed = state.spawn_seed;
        // the constructor's rolls (Random.InitState(ID.RandomSeed)); the size
        // is game.SeededRandom(ID.RandomSeed), its own generator
        looks lk;
        float size = 1.f;
        {
            rw::unity_random r{seed};
            float const num = mathf::lerp(0.2f, 0.4f, r.value());
            rw::unity_random sr{seed};
            size = mathf::lerp(1.f - num, 1.f + num, sr.value());
            float num2 = mathf::lerp(0.85f, 1.1f, r.value());
            if (num2 > 1.f)
                num2 -= 1.f;
            float num3 = mathf::lerp(0.85f, 1.1f, r.value());
            if (num3 > 1.f)
                num3 -= 1.f;
            // (C# evaluates the arguments left to right)
            float const s0 = mathf::lerp(0.7f, 1.f, 1.f - mathf::pow(r.value(), 3.f));
            float const l0 = mathf::lerp(0.f, 0.3f, mathf::pow(r.value(), 2.f));
            lk.shell_color[0] = custom::hsl2rgb(num2, s0, l0);
            float const h1 = mathf::lerp(num3, num2, mathf::pow(r.value(), 3.f));
            float const s1 = mathf::lerp(0.7f, 1.f, 1.f - mathf::pow(r.value(), 3.f));
            float const l1 = mathf::lerp(0.05f, 1.f, mathf::pow(r.value(), 3.f));
            lk.shell_color[1] = custom::hsl2rgb(h1, s1, l1);
            float const pick = r.value();
            float const t = pick < 0.8f ? mathf::pow(r.value(), 3.f) : 1.f;
            lk.shell_color[0] = lk.shell_color[0] + (lk.shell_color[1] - lk.shell_color[0]) * mathf::clamp01(t);
            for (uint32_t i = 0; i < 2; ++i)
                lk.shell_color[i] = rw::vec3{mathf::max(lk.shell_color[i].x, 0.007843138f), mathf::max(lk.shell_color[i].y, 0.007843138f),
                    mathf::max(lk.shell_color[i].z, 0.007843138f)};
            float const n = float(mini_click_count);
            for (uint32_t j = 0; j < mini_click_count; ++j)
                lk.mini_clicks[j] = mathf::lerp(1.f / n * float(j), 1.f / n * (float(j) + 1.f), r.value());
        }
        physics::object_desc desc;
        desc.gravity = own_gravity;
        desc.air_friction = 0.999f;
        desc.bounce = 0.1f;
        desc.surface_friction = 0.4f;
        desc.water_friction = 0.96f;
        desc.buoyancy = own_buoyancy;
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
        // the body (4 * size) and the shell (9 * min(1, size)), 0.16 each;
        // a Pull connection (0, then Act's 7 or 12) and a Push one (2 * size)
        float const mass = 0.32f / 2.f;
        auto const c0 = physics::add_chunk(state, o, at, 4.f * size, mass);
        auto const c1 = physics::add_chunk(state, o, at + vec2{0.f, -1.f}, 9.f * mathf::min(1.f, size), mass);
        physics::add_connection(state, o, c0, c1, 0.f, physics::connection_type::pull, 1.f, 0.5f);
        physics::add_connection(state, o, c0, c1, 2.f * size, physics::connection_type::push, 1.f, 0.5f);
        w.try_create_creature_body(c, o);
        auto& x = pl(state, c);
        x = creature::snail_payload{};
        x.data = uint16_t(d.index());
        x.size = size;
        // Snail.PlaceInRoom: holding on where it is put, if it can
        if (accessible_at(view, at)) {
            x.suck_point = at;
            x.has_suck_point = 1;
        }
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_set_grasps(c, creature::grasp_pair{});
        w.creature_set_behavior(c, 0);
        w.creature_set_current_utility(c, 0.f);
        w.creature_set_destination(c, tile_of(at));
        w.snail_data_set_seed(d, seed);
        w.snail_data_set_looks(d, lk);
        w.snail_data_set_graphics(d, graphics_state{});
        tracker::clear(state, c);
        snail_graphics::reset_positions(state, c);
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
        // Creature.Update + PhysicalObject.Update (TerrainImpact on landings)
        creature::stun_countdown(state, c);
        impact_ctx ictx{&state, c};
        physics::update_object(state, room, o, &terrain_impact, &ictx);
        auto& x = pl(state, c);
        // (no Snail_Emerge_From_Shell / Withdraw_In_Shell sounds)
        x.last_out_of_shell = x.move;
        x.bloated = 0;
        if (x.trigger_ticker > 0) {
            --x.trigger_ticker;
            if (x.trigger_ticker == 0)
                x.triggered = 1;
        }
        bool const awake = consious(state, c);
        if (awake && x.has_drop_to_tile)
            drop(state, c);
        else if (awake && accessible_at(room, pos(state, c, 0)))
            act(state, c, room);
        else if (x.allowed_in_non_accessible_tile < 1)
            x.has_suck_point = 0;
        if (!w.creature_is_valid(c))
            return;
        if (x.triggered && !w.creature_get_dead(c) && (contact(state, c, 1).y < 0 || (sub(state, room, c, 0) > 0.f && rng.value() < 0.05f)))
            click(state, c, room);
        if (x.allowed_in_non_accessible_tile > 0)
            --x.allowed_in_non_accessible_tile;
        if (x.has_suck_point) {
            vec2 const m = pos(state, c, 0);
            if (custom::dist_less(m, x.suck_point, 15.f) && rw::magnitude(vel(state, c, 0)) < 6.f)
                vel(state, c, 0) += (x.suck_point - m) * 0.5f;
            else
                x.has_suck_point = 0;
        }
    }

    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) {
        // what holds it is kept apart by the carrying
        return !creature::holds(state.world, t, c);
    }

    void collide(sys::game_state& state, dcon::creature_id c, room_view const& room, uint32_t, dcon::creature_id t, uint32_t) {
        if (!creature::live(state, t))
            return;
        collide_with(state, c, room, t);
    }

    void collide_kin(sys::game_state& state, dcon::creature_id c, uint32_t, dcon::creature_id other, uint32_t) {
        auto const room = world::get_room_view(state, creature::room_of(state, c));
        collide_with(state, c, room, other);
        if (state.world.creature_is_valid(other))
            collide_with(state, other, room, c);
    }

    void violence(sys::game_state& state, dcon::creature_id c, uint32_t k, vec2 momentum, float damage, float stun_bonus, creature::damage_type) {
        auto& w = state.world;
        // the hit chunk in the key: two hits of a tick roll apart
        auto rng = rw::random(state.seed, state.tick, rw::ids(uint32_t(c.value), k));
        auto& x = pl(state, c);
        // Snail.Violence: a real hit sets it off, a light one nearly
        if (!w.creature_get_dead(c) && (damage >= 0.1f || stun_bonus > 10.f))
            x.triggered = 1;
        else if (!x.just_clicked && x.click_counter < 0.92f)
            x.click_counter = 0.92f;
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
        // grasps 0: it holds nothing
    }

    void died(sys::game_state&, dcon::creature_id) {
        // Snail.Die is Creature.Die's (a dead snail never clicks)
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
        auto& x = pl(state, c);
        x.has_suck_point = 0;
        x.has_drop_to_tile = 0;
        x.allowed_in_non_accessible_tile = 0;
        // SnailAI.NewRoom: heading for the den it came out of, it picks a
        // random tile of the new room instead
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        w.creature_set_destination(c, random_tile(rng, view));
    }

    std::string describe(sys::game_state const& state, dcon::creature_id c) {
        auto const& x = pl(state, c);
        vec2 const p = pos(state, c, 0);
        char buf[96];
        std::snprintf(buf, sizeof buf, " click %.2f scared %.2f", x.click_counter, x.scared);
        return "snail at " + std::to_string(int(p.x)) + "," + std::to_string(int(p.y)) + (x.move ? " crawling" : " in its shell") + buf
            + (x.triggered ? " triggered" : "") + (x.trigger_ticker > 0 ? " thrown" : "") + (x.has_suck_point ? "" : " loose")
            + (creature::held(state, c) ? " carried" : "");
    }

    bool spawn_spot(room_view const& room, ivec2 tile) {
        return room.inside(tile.x, tile.y) && !room.get_tile(tile).solid() && pathing::accessible(room, tile, crawl);
    }

    //
    // The player side
    //
    dcon::creature_id held_by(sys::game_state const& state, dcon::player_id p) noexcept {
        if (!p)
            return dcon::creature_id{};
        auto const c = creature::grasped(state.world, player::creature_of(state, p), 0);
        return c && creature::is(state.world, c, creature::type::snail) ? c : dcon::creature_id{};
    }

    dcon::creature_id pickup_candidate(sys::game_state const& state, dcon::player_id p, room_view const& room, float& score) {
        auto const& w = state.world;
        vec2 const p0 = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
        int32_t const flip = w.player_get_flip_direction(p);
        dcon::creature_id result;
        score = 3.4e38f;
        rooms::for_each_creature(state, room.id, creature::type::snail, [&](dcon::creature_id c) {
            // Player.Grabability: TwoHands; IsCreatureLegalToHoldWithoutStun
            if (creature::held(state, c) || w.creature_get_away(c) != 0)
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
        // SlugcatGrab: chunk 0 (a TwoHands object isn't held by its closest)
        creature::grab(state, player::creature_of(state, p), 0, c, 0);
        // picked off its surface
        pl(state, c).has_suck_point = 0;
    }

    void release(sys::game_state& state, dcon::creature_id c) {
        uint32_t g = 0;
        auto const h = creature::holder(state, c, &g);
        if (h && creature::is_slugcat(state.world, h))
            creature::release_grasp(state, h, g);
    }

    void thrown(sys::game_state& state, dcon::creature_id c) {
        release(state, c);
        // Player.ThrowObject: it goes off 40 ticks later, on its landing
        pl(state, c).trigger_ticker = 40;
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
        auto& x = pl(state, c);
        x.has_suck_point = 0;
        x.has_drop_to_tile = 0;
        w.creature_set_destination(c, tile_of(at));
        tracker::clear(state, c);
        snail_graphics::reset_positions(state, c);
    }

    bool jumped_on(sys::game_state& state, dcon::creature_id c) {
        auto& x = pl(state, c);
        x.click_counter += 0.8f;
        if (x.click_counter <= 1.f)
            return false;
        auto const room = world::get_room_view(state, creature::room_of(state, c));
        click(state, c, room);
        return true;
    }
}
