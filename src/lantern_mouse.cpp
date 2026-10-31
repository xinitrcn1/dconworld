#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>

#include "lantern_mouse.hpp"
#include "dens.hpp"
#include "lantern_mouse_graphics.hpp"
#include "lantern_mouse_types.hpp"
#include "migrate.hpp"
#include "particles.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "rooms.hpp"
#include "tracker.hpp"

namespace lantern_mouse {
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
        // CreatureTemplate (LanternMouse): baseDamageResistance,
        // baseStunResistance, instantDeathDamageLimit, visualRadius,
        // lungCapacity (the default)
        constexpr float base_damage_resistance = 0.5f;
        constexpr float base_stun_resistance = 0.4f;
        constexpr float instant_death_damage_limit = 1.f;
        constexpr float visual_radius = 800.f;
        constexpr float lung_capacity = 520.f;
        constexpr float float_max = std::numeric_limits<float>::max();
        constexpr float float_min = std::numeric_limits<float>::lowest();

        creature::lantern_mouse_payload& pl(sys::game_state& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).lantern_mouse;
        }
        creature::lantern_mouse_payload const& pl(sys::game_state const& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).lantern_mouse;
        }
        dcon::lantern_mouse_data_id data(sys::game_state const& state, dcon::creature_id c) {
            return dcon::lantern_mouse_data_id{dcon::lantern_mouse_data_id::value_base_t(state.world.creature_get_payload(c).lantern_mouse.data)};
        }

        ivec2 tile_of(vec2 p) { return room_view::get_tile_position(p); }
        bool solid(room_view const& room, ivec2 t) { return room.get_tile(t).solid(); }
        float float_dist(ivec2 a, ivec2 b) { return rw::magnitude(rw::to_vec2(a - b)); }

        // AImap.TileAccessibleToCreature
        bool accessible(room_view const& room, ivec2 t) {
            return room.inside(t.x, t.y) && pathing::accessible(room, t, walk);
        }
        bool accessible(room_view const& room, vec2 p) { return accessible(room, tile_of(p)); }

        // CreatureTemplate.canFly of the ported templates
        bool can_fly(tmpl t) noexcept {
            return t == tmpl::cicada_a || t == tmpl::cicada_b || t == tmpl::centiwing || t == tmpl::big_needle_worm || t == tmpl::small_needle_worm;
        }

        // What one tick of the mouse works with
        struct ctx {
            sys::game_state& state;
            dcon::data_container& w;
            dcon::creature_id c;
            room_view const& room;
            creature::lantern_mouse_payload& x;
            dcon::lantern_mouse_data_id d;
            rw::rng rng;
            dcon::body_chunk_id ch[2];
            // `salt` tells apart the ctxs made for one mouse in one tick
            ctx(sys::game_state& s, dcon::creature_id cc, room_view const& r, uint32_t salt)
                : state(s), w(s.world), c(cc), room(r), x(pl(s, cc)), d(data(s, cc)), rng(rw::random(s.seed, s.tick, rw::ids(uint32_t(cc.value), salt))) {
                ch[0] = chunk(s, cc, 0);
                ch[1] = chunk(s, cc, 1);
            }
            vec2& pos(uint32_t i) { return w.body_chunk_get_pos(ch[i]); }
            vec2& vel(uint32_t i) { return w.body_chunk_get_vel(ch[i]); }
            vec2 last_pos(uint32_t i) { return w.body_chunk_get_last_pos(ch[i]); }
            rope_state& rope() { return w.lantern_mouse_data_get_rope(d); }
            ai_state& ai() { return w.lantern_mouse_data_get_ai(d); }
            individual_variations const& iv() { return w.lantern_mouse_data_get_ivars(d); }
            dcon::physical_object_id o() { return body(state, c); }
            // abstractCreature.pos: the main chunk's tile
            ivec2 at() { return tile_of(pos(0)); }
            bool footing() const { return x.footing_counter > 20; }
            bool has_rope() { return rope().bend_count > 0; }
            bool sleeping() { return has_rope() && x.fall_asleep >= 1.f; }
            behavior beh() { return behavior(w.creature_get_behavior(c)); }
            bool viewed() { return state.viewed(room.id); }
            bool solid_at(uint32_t i, int32_t rx, int32_t ry) { return physics::is_tile_solid(state, room, ch[i], rx, ry); }
        };

        //
        // MouseAI
        //

        // the tracker's settings (MouseAI's constructor: Tracker(10, 10,
        // 450, ..., 10), ThreatTracker(3))
        tracker::params const& tracker_params() {
            static tracker::params const p = [] {
                tracker::params q;
                q.see_around_corners = 10;
                q.frames_to_remember = 450;
                q.ghost_dismissal_range = 10;
                q.threat_max = 3;
                return q;
            }();
            return p;
        }

        // ArtificialIntelligence.VisualScore + MouseAI.VisualScore: asleep on
        // its rope it sees no further than 200
        bool visual_contact(sys::game_state const& state, dcon::creature_id c, room_view const& room, vec2 look) {
            auto const& w = state.world;
            vec2 const eye = w.body_chunk_get_pos(chunk(state, c, 0));
            auto const& x = pl(state, c);
            bool const asleep = w.lantern_mouse_data_get_rope(data(state, c)).bend_count > 0 && x.fall_asleep >= 1.f;
            if (asleep && !custom::dist_less(eye, look, 200.f))
                return false;
            if (!custom::dist_less(eye, look, visual_radius))
                return false;
            return room.visual_contact(eye, look);
        }

        // the reachability the AI's pathfinder answers: the tile can be stood
        // on and there is a way back from it to where the mouse is
        // (CoordinateReachableAndGetbackable)
        bool reachable(ctx& x, ivec2 t) {
            if (!accessible(x.room, t))
                return false;
            auto const f = pathing::field_to(x.state, x.room.id, x.at(), walk);
            return pathing::distance(x.state, f, t) != pathing::unreachable;
        }

        // MouseAI.TileAccessible: by the access map alone (AnyExitReachableFromTile)
        // or the pathfinder
        bool tile_accessible(ctx& x, ivec2 t, bool no_access_map) {
            return no_access_map ? accessible(x.room, t) : reachable(x, t);
        }

        // MouseAI.DangleTile: a tile with open air above it up to a ceiling or
        // a horizontal pole more than 4 tiles up, ground below to get back to,
        // and a walkable tile beside where the rope goes
        dangle dangle_tile(ctx& x, ivec2 tile, bool no_access_map) {
            auto const& room = x.room;
            if (!room.inside(tile.x, tile.y))
                return dangle{};
            if (pathing::ai_terrain_proximity(x.state, room.id, tile) < 5)
                return dangle{};
            if (pathing::tile_access(room, tile) == pathing::access::climb)
                return dangle{};
            bool ground = false;
            for (int32_t i = tile.y - 1; i >= 0; --i) {
                if (tile_accessible(x, ivec2{tile.x, i}, no_access_map)) {
                    ground = true;
                    break;
                }
                if (solid(room, ivec2{tile.x, i}))
                    break;
            }
            if (!ground)
                return dangle{};
            for (int32_t j = tile.y; j < room.height; ++j) {
                if (solid(room, ivec2{tile.x, j}))
                    return dangle{};
                if (j > tile.y + 4 && (solid(room, ivec2{tile.x, j + 1}) || room.get_tile(tile.x, j).horizontal_beam()))
                    for (int32_t k = -1; k < 2; ++k)
                        if (tile_accessible(x, ivec2{tile.x + k, j}, no_access_map) || tile_accessible(x, ivec2{tile.x + k, j + 1}, no_access_map)
                            || (!solid(room, ivec2{tile.x + k, j + 1}) && tile_accessible(x, ivec2{tile.x + k, j + 2}, no_access_map)))
                            return dangle{tile, ivec2{tile.x, j}, true};
            }
            return dangle{};
        }

        // MouseAI.DanglePosScore: room for the body, away from the spots of
        // the more dominant mice, the current spot strongly preferred
        float dangle_pos_score(ctx& x, dangle const& dg) {
            float num = float(pathing::ai_terrain_proximity(x.state, x.room.id, dg.body_pos)) * 2.f;
            float const mine = x.iv().dominance;
            bool too_close = false;
            rooms::for_each_creature(x.state, x.room.id, creature::type::lantern_mouse, [&](dcon::creature_id o) {
                if (o == x.c || too_close || x.w.creature_get_away(o) != 0)
                    return;
                auto const od = data(x.state, o);
                auto const& oa = x.w.lantern_mouse_data_get_ai(od);
                if (x.w.lantern_mouse_data_get_ivars(od).dominance <= mine || !oa.dangle_spot.valid)
                    return;
                float const dd = float_dist(dg.body_pos, oa.dangle_spot.body_pos);
                if (dd < 3.f)
                    too_close = true;
                else
                    num += dd;
            });
            if (too_close)
                return float_min;
            auto const& cur = x.ai().dangle_spot;
            if (cur.valid && dg.attached_pos == cur.attached_pos)
                num += 1000.f;
            return num;
        }

        // MouseAI.ReconsiderDanglePos: a random tile of the room tried each
        // tick, kept when it beats the current spot
        void reconsider_dangle_pos(ctx& x) {
            ivec2 const t{x.rng.range(0, x.room.width), x.rng.range(0, x.room.height)};
            dangle const dg = dangle_tile(x, t, false);
            auto& ai = x.ai();
            if (dg.valid)
                ai.dangle_checks = 0;
            else
                ++ai.dangle_checks;
            if (!dg.valid)
                return;
            if (!ai.dangle_spot.valid || dangle_pos_score(x, dg) > dangle_pos_score(x, ai.dangle_spot))
                ai.dangle_spot = dg;
        }

        // IUseARelationshipTracker.UpdateDynamicRelationship: what it fears
        // counts for nothing dead, for more above it, for less far from its
        // dangle spot. TrackedCreatureState.alive is the representation's
        // `state` (1 dead)
        relationship relationship_to(ctx& x, creature::tracked& r) {
            if (r.visual && creature::live(x.state, r.who))
                r.state = creature::dead(x.state, r.who) ? 1 : 0;
            relationship rl = creature::static_relationship(x.state, x.c, r.who);
            if (rl.type != rel::afraid)
                return rl;
            if (r.state != 0) {
                rl.intensity = 0.f;
                return rl;
            }
            if (!creature::live(x.state, r.who) || creature::room_of(x.state, r.who) != x.room.id || can_fly(creature::template_of(x.state, r.who)))
                return rl;
            float num = mathf::lerp(0.1f, 1.6f, mathf::inverse_lerp(-100.f, 200.f, room_view::middle_of_tile(tracker::best_guess(r)).y - x.pos(0).y));
            float value = float_max;
            auto const& dg = x.ai().dangle_spot;
            if (dg.valid)
                value = mathf::min(rw::distance(x.pos(0), room_view::middle_of_tile(dg.attached_pos)), rw::distance(x.pos(0), room_view::middle_of_tile(dg.body_pos)));
            num = mathf::lerp(num, 1.f, mathf::inverse_lerp(50.f, 500.f, value));
            rl.intensity *= num;
            return rl;
        }

        // the trackers' questions, answered by the mouse
        struct hook_ctx {
            ctx& x;
        };
        bool hook_sees(void* u, dcon::body_chunk_id k) {
            auto& h = *static_cast<hook_ctx*>(u);
            return visual_contact(h.x.state, h.x.c, h.x.room, h.x.w.body_chunk_get_pos(k));
        }
        bool hook_sees_point(void* u, vec2 p) {
            auto& h = *static_cast<hook_ctx*>(u);
            return visual_contact(h.x.state, h.x.c, h.x.room, p);
        }
        relationship hook_relate(void* u, creature::tracked const& r) {
            auto& h = *static_cast<hook_ctx*>(u);
            return relationship_to(h.x, const_cast<creature::tracked&>(r));
        }
        // ModuleToTrackRelationship: only Afraid is tracked (ThreatTracker)
        creature::track_module hook_module(relationship r) {
            return r.type == rel::afraid ? creature::track_module::threat : creature::track_module::none;
        }
        bool hook_reach(void* u, ivec2 t) {
            auto& h = *static_cast<hook_ctx*>(u);
            return reachable(h.x, t);
        }
        tracker::hooks make_hooks(hook_ctx& hx) {
            tracker::hooks h;
            h.user = &hx;
            h.sees = hook_sees;
            h.sees_point = hook_sees_point;
            h.relate = hook_relate;
            h.module = hook_module;
            h.reach = hook_reach;
            return h;
        }

        void attach_rope(ctx& x, ivec2 at);
        void detach_rope(ctx& x);

        // MouseAI.Update
        void ai_update(ctx& x) {
            auto& w = x.w;
            ivec2 const at = x.at();
            hook_ctx hx{x};
            tracker::hooks const h = make_hooks(hx);
            tracker::update(x.state, x.c, x.room, at, tracker_params(), h);
            auto const& ts = tracker::of(x.state, x.c);
            auto& ai = x.ai();
            // UtilityComparer: the threat and the rain (weights 1)
            float const threat = tracker::threat_utility(ts);
            float const rain = dens::rain_utility(x.state, x.c, x.room.id);
            behavior b = x.beh();
            float utility = 0.f;
            if (threat >= rain && threat > 0.f) {
                b = behavior::flee;
                utility = threat;
            } else if (rain > 0.f) {
                b = behavior::escape_rain;
                utility = rain;
            }
            if (utility < 0.2f)
                b = behavior::idle;
            w.creature_set_behavior(x.c, uint8_t(b));
            w.creature_set_current_utility(x.c, utility);

            // off the rope when the spot is gone or in a panic
            if (x.has_rope()) {
                auto const& dg = ai.dangle_spot;
                bool keep = dg.valid && dg.attached_pos == tile_of(x.rope().attached) && (b == behavior::idle || x.rng.value() >= tracker::panic(ts) * 0.5f);
                if (!keep)
                    detach_rope(x);
            }

            ivec2& dest = w.creature_get_destination(x.c);
            switch (b) {
            case behavior::idle: {
                x.x.run_speed = mathf::lerp(x.x.run_speed, 0.5f, 0.05f);
                reconsider_dangle_pos(x);
                if (!reachable(x, dest))
                    dest = at;
                if (ai.has_walk_with) {
                    dest = ai.walk_with;
                    if (x.rng.value() < 0.02f || custom::manhattan_distance(at, ai.walk_with) < 4)
                        ai.has_walk_with = 0;
                } else if (ai.dangle_spot.valid) {
                    // to the tile beside where the rope goes, then up it goes
                    ivec2 const ap = ai.dangle_spot.attached_pos;
                    ivec2 const t0 = x.at(), t1 = tile_of(x.pos(1));
                    for (auto const dd : custom::eight_directions_and_zero) {
                        if (reachable(x, ap + dd)) {
                            dest = ap + dd;
                            if (t0 == dest || t1 == dest)
                                attach_rope(x, ap);
                            break;
                        }
                        if (!solid(x.room, ap + dd) && reachable(x, ap + dd + ivec2{0, 1})) {
                            dest = ap + dd + ivec2{0, 1};
                            if (t0 == dest || t1 == dest)
                                attach_rope(x, ap);
                            break;
                        }
                    }
                } else {
                    // nowhere to hang here for a while: a neighbouring room
                    // (the abstract AI's roaming, migrate.hpp, stands in for
                    // the random exit node)
                    if (ai.dangle_checks > 200 && !migrate::want_to(x.state, x.c)) {
                        migrate::random_move(x.state, x.c, x.room, 1);
                        ai.idle_pos_counter = x.rng.range(200, 500);
                    } else {
                        migrate::idle_roll(x.state, x.c, x.room);
                    }
                    if (auto const e = migrate::travel_tile(x.state, x.c, x.room)) {
                        dest = *e;
                    } else if (x.rng.value() < 0.0045454544f || ai.idle_pos_counter <= 0) {
                        ivec2 const t{x.rng.range(0, x.room.width), x.rng.range(0, x.room.height)};
                        if (reachable(x, t)) {
                            dest = t;
                            ai.idle_pos_counter = x.rng.range(200, 1900);
                        }
                    }
                    if (!x.x.sitting)
                        --ai.idle_pos_counter;
                    --ai.idle_pos_counter;
                }
                break;
            }
            case behavior::flee:
                x.x.run_speed = mathf::lerp(x.x.run_speed, 1.f, 0.08f);
                dest = tracker::flee_to(x.state, x.c, x.room, at, 6, 20, tracker_params(), h);
                break;
            case behavior::escape_rain:
                x.x.run_speed = mathf::lerp(x.x.run_speed, 1.f, 0.08f);
                if (auto const e = dens::home_tile(x.state, x.c, x.room))
                    dest = *e;
                break;
            }
            if (b == behavior::flee)
                ai.fear = mathf::lerp(ai.fear, mathf::pow(tracker::panic(ts), 0.7f), 0.5f);
            else
                ai.fear = mathf::max(ai.fear - 0.0125f, 0.f);

            // asleep only with nothing to fear or that eats it close by; pull
            // the rope up while something that walks is in sight below
            ai.want_to_sleep = 1;
            float pull = 0.f;
            for (uint32_t i = 0; i < ts.count; ++i) {
                auto const& r = ts.reps[i];
                if (!creature::live(x.state, r.who))
                    continue;
                auto const tt = creature::template_of(x.state, r.who);
                auto const st = creature::static_relationship(x.state, x.c, r.who);
                if (st.type == rel::afraid) {
                    ai.want_to_sleep = 0;
                    if (!can_fly(tt) && r.visual && tracker::best_guess(r).y < at.y + 1)
                        pull = mathf::max(pull, mathf::pow(mathf::inverse_lerp(400.f, 30.f, rw::distance(x.pos(0), creature::main_pos(x.state, r.who))), 2.7f));
                } else if (st.type == rel::eats && custom::manhattan_distance(tracker::best_guess(r), at) < 10) {
                    ai.want_to_sleep = 0;
                }
            }
            ai.pull_up = mathf::lerp(ai.pull_up, pull, 0.05f);
        }

        //
        // LanternMouse
        //

        // LanternMouse.Squeak (Mouse_Squeak has no sound id: silent)
        void squeak(ctx& x, float stress) {
            if (x.w.creature_get_dead(x.c) || x.x.voice_counter > 0)
                return;
            x.x.voice_counter = int16_t(x.rng.range(5, 12));
            if (x.viewed()) {
                auto gr = rw::random(x.state.seed, x.state.tick, uint32_t(x.c.value));
                lantern_mouse_graphics::shove_head(x.state, x.c, custom::rnv(gr) * 4.f * gr.value());
                if (gr.value() > mathf::inverse_lerp(0.5f, 1.f, stress))
                    lantern_mouse_graphics::ouch(x.state, x.c, x.x.voice_counter);
            }
        }

        // LanternMouse.AttatchRope (Mouse_Rope_Attach: silent)
        void attach_rope(ctx& x, ivec2 at) {
            auto& r = x.rope();
            x.x.rope_invincible = 20;
            physics::set_go_through_floors(x.state, x.o(), true);
            r.attached = room_view::middle_of_tile(at);
            if (x.room.get_tile(r.attached + vec2{0.f, 20.f}).solid())
                r.attached.y += 9.f;
            x.x.rope_length = 20.f;
            r.bends[0] = r.attached;
            r.bend_count = 1;
            r.last_clear = x.pos(1);
        }

        // LanternMouse.DetatchRope (Mouse_Rope_Detach: silent)
        void detach_rope(ctx& x) {
            if (x.x.rope_invincible > 0)
                return;
            x.rope().bend_count = 0;
        }

        // SharedPhysics.RayTracedTilesArray: the tiles the segment crosses,
        // in order; returns how many (only the first `cap` are written)
        constexpr uint32_t ray_cap = 256;
        int32_t ray_traced_tiles(vec2 a, vec2 b, ivec2 (&out)[ray_cap]) {
            float const x0 = a.x / 20.f, y0 = a.y / 20.f, x1 = b.x / 20.f, y1 = b.y / 20.f;
            float const dx = std::fabs(x1 - x0), dy = std::fabs(y1 - y0);
            int32_t x = int32_t(std::floor(x0));
            int32_t y = int32_t(std::floor(y0));
            float const dt_dx = 1.f / dx;
            float const dt_dy = 1.f / dy;
            int32_t n = 1;
            int32_t x_inc, y_inc;
            float t_next_horizontal, t_next_vertical;
            if (dx == 0.f) {
                x_inc = 0;
                t_next_horizontal = dt_dx;
            } else if (x1 > x0) {
                x_inc = 1;
                n += int32_t(std::floor(x1)) - x;
                t_next_horizontal = (std::floor(x0) + 1.f - x0) * dt_dx;
            } else {
                x_inc = -1;
                n += x - int32_t(std::floor(x1));
                t_next_horizontal = (x0 - std::floor(x0)) * dt_dx;
            }
            if (dy == 0.f) {
                y_inc = 0;
                t_next_vertical = dt_dy;
            } else if (y1 > y0) {
                y_inc = 1;
                n += int32_t(std::floor(y1)) - y;
                t_next_vertical = (std::floor(y0) + 1.f - y0) * dt_dy;
            } else {
                y_inc = -1;
                n += y - int32_t(std::floor(y1));
                t_next_vertical = (y0 - std::floor(y0)) * dt_dy;
            }
            int32_t const total = n;
            for (int32_t i = 0; i < total && i < int32_t(ray_cap); ++i) {
                out[i] = ivec2{x, y};
                if (t_next_vertical < t_next_horizontal) {
                    y += y_inc;
                    t_next_vertical += dt_dy;
                } else {
                    x += x_inc;
                    t_next_horizontal += dt_dx;
                }
            }
            return total;
        }

        // Room.TileRect
        struct float_rect {
            float left, bottom, right, top;
        };
        float_rect tile_rect(ivec2 t) {
            return float_rect{float(t.x) * 20.f, float(t.y) * 20.f, float(t.x) * 20.f + 20.f, float(t.y) * 20.f + 20.f};
        }
        // FloatRect.GetCorner(0..3): top left, top right, bottom right, bottom left
        vec2 corner(float_rect const& r, int32_t i) {
            switch (i) {
            case 0: return vec2{r.left, r.top};
            case 1: return vec2{r.right, r.top};
            case 2: return vec2{r.right, r.bottom};
            default: return vec2{r.left, r.bottom};
            }
        }
        // Room.IsCornerFree(x, y, 0..3)
        bool corner_free(room_view const& room, ivec2 t, int32_t c) {
            auto const s = [&](int32_t dx, int32_t dy) { return room.get_tile(t + ivec2{dx, dy}).solid(); };
            switch (c) {
            case 0: return !s(-1, 0) && !s(-1, 1) && !s(0, 1);
            case 1: return !s(1, 0) && !s(1, 1) && !s(0, 1);
            case 2: return !s(1, 0) && !s(1, -1) && !s(0, -1);
            default: return !s(-1, 0) && !s(-1, -1) && !s(0, -1);
            }
        }
        // Custom.RectCollision(pos, lastPos, rect)'s lower left: where the
        // segment from `last` towards `pos` first crosses the rectangle's
        // edges (`pos` itself when it doesn't). Vertical/HorizontalCrossPoint
        // of an upright or level segment divide by zero in vanilla; those
        // crossings are skipped here
        vec2 rect_collision(vec2 pos, vec2 last, float_rect const& r) {
            bool const inside = last.x > r.left && last.x < r.right && last.y > r.bottom && last.y < r.top;
            float dst = inside ? float_max : rw::distance(last, pos);
            vec2 best = pos;
            auto const consider = [&](vec2 v, bool ok) {
                if (ok && custom::dist_less(last, v, dst)) {
                    best = v;
                    dst = rw::distance(last, v);
                }
            };
            float const slope = last.x != pos.x ? (last.y - pos.y) / (last.x - pos.x) : 0.f;
            float const offset = last.y - last.x * slope;
            auto const vertical = [&](float xx) -> std::optional<vec2> {
                if (last.y == pos.y)
                    return vec2{xx, last.y};
                if (last.x == pos.x)
                    return std::nullopt;
                return vec2{xx, offset + slope * xx};
            };
            auto const horizontal = [&](float yy) -> std::optional<vec2> {
                if (last.x == pos.x)
                    return vec2{last.x, yy};
                if (slope == 0.f)
                    return std::nullopt;
                return vec2{(yy - offset) / slope, yy};
            };
            if (auto const v = vertical(r.left))
                consider(*v, v->y >= r.bottom && v->y <= r.top);
            if (auto const v = vertical(r.right))
                consider(*v, v->y >= r.bottom && v->y <= r.top);
            if (auto const v = horizontal(r.bottom))
                consider(*v, v->x >= r.left && v->x <= r.right);
            if (auto const v = horizontal(r.top))
                consider(*v, v->x >= r.left && v->x <= r.right);
            return best;
        }

        // LanternMouse.UpdateRope: the rope from its last bend to the rear
        // chunk wraps around the corner of the terrain it passes, and
        // unwraps when the bend before is in sight again
        void update_rope(ctx& x) {
            auto& r = x.rope();
            auto const& room = x.room;
            vec2 const b1 = x.pos(1);
            static thread_local ivec2 ray[ray_cap];
            if (r.bend_count > 1) {
                int32_t const n = std::min(ray_traced_tiles(b1, r.bends[r.bend_count - 2], ray), int32_t(ray_cap));
                bool clear = true;
                for (int32_t j = 0; j < n - 1; ++j)
                    if (solid(room, ray[j])) {
                        clear = false;
                        break;
                    }
                if (clear) {
                    --r.bend_count;
                    r.last_clear = b1;
                    return;
                }
            }
            vec2 const last_bend = r.bends[r.bend_count - 1];
            ivec2 const bend_tile = tile_of(last_bend);
            ivec2 iv = bend_tile;
            int32_t const k = std::min(ray_traced_tiles(b1, last_bend, ray), int32_t(ray_cap));
            bool clear = true;
            for (int32_t num = 0; num < k && ray[num] != bend_tile; ++num) {
                if (!solid(room, ray[num]))
                    continue;
                clear = false;
                vec2 b = rect_collision(r.attached, b1, tile_rect(ray[num]));
                iv = ray[num];
                float dst = rw::distance(b1, b);
                for (auto const dd : custom::eight_directions) {
                    if (!solid(room, ray[num] + dd))
                        continue;
                    vec2 const b2 = rect_collision(r.attached, b1, tile_rect(ray[num] + dd));
                    if (custom::dist_less(b1, b2, dst)) {
                        b = b2;
                        iv = ray[num] + dd;
                        dst = rw::distance(b1, b);
                    }
                }
                break;
            }
            if (clear) {
                r.last_clear = b1;
                return;
            }
            // the free corner closest to the line from the knot to where the
            // rope was last clear, walking along the solid tiles
            vec2 v2 = r.attached;
            float best = float_max;
            for (int32_t m = 0; m < 100; ++m) {
                ivec2 iv2 = iv;
                for (auto const dd : custom::eight_directions_and_zero) {
                    ivec2 const t3 = iv + dd;
                    if (!solid(room, t3))
                        continue;
                    bool found = false;
                    float_rect const tr = tile_rect(t3);
                    for (int32_t cn = 0; cn < 4; ++cn) {
                        float const dl = std::fabs(custom::distance_to_line(corner(tr, cn), r.attached, r.last_clear));
                        if (corner_free(room, t3, cn) && dl < best) {
                            v2 = corner(tr, cn);
                            best = dl;
                            v2 += custom::dir_vec(room_view::middle_of_tile(t3), v2) * 0.4f;
                            iv2 = t3;
                            found = true;
                        }
                    }
                    if (!found) {
                        float const dl = std::fabs(custom::distance_to_line(room_view::middle_of_tile(t3), r.attached, r.last_clear));
                        if (dl < best) {
                            best = dl;
                            iv2 = t3;
                        }
                    }
                }
                if (iv2 == iv)
                    break;
                iv = iv2;
            }
            if (v2 != r.bends[r.bend_count - 1] && r.bend_count < max_bends)
                r.bends[r.bend_count++] = v2;
        }

        // LanternMouse.Hang: hanging from the rope by the rear end, the rope
        // reeled out to the dangle spot's length (pulled up while something
        // walks below), swinging, dozing off; the rope lets go when it runs
        // out or bends too much
        void hang(ctx& x) {
            auto& p = x.x;
            p.profile_fac *= 0.8f;
            if (consious(x.state, x.c)) {
                ai_update(x);
                if (!x.has_rope())
                    return;
                auto& ai = x.ai();
                if (ai.dangle_spot.valid) {
                    float num = rw::distance(x.rope().attached, room_view::middle_of_tile(ai.dangle_spot.body_pos));
                    if (p.wake_up == 1.f)
                        num = mathf::max(num * (1.f - mathf::clamp01(ai.pull_up)), 20.f);
                    if (p.rope_length < num) {
                        p.rope_length = mathf::min(mathf::lerp(p.rope_length, num, 0.002f) + 0.6f, num);
                        if (p.rope_length < num - 4.f)
                            p.fall_asleep -= 0.016666668f;
                    } else if (p.rope_length > num) {
                        if (p.rope_length > num + 1.5f) {
                            p.shrinking_rope = 1;
                            p.run_cycle += 0.05f;
                            p.fall_asleep -= 0.016666668f;
                        }
                        p.rope_length = mathf::max(mathf::lerp(p.rope_length, num, 0.002f) - 0.6f, num);
                    }
                }
                if (x.rng.value() < 0.025f)
                    p.swing_fac_get_to = mathf::pow(x.rng.value(), 1.5f);
                p.swing_fac = mathf::lerp(p.swing_fac, p.swing_fac_get_to, 0.05f);
                if (x.vel(0).x == 0.f)
                    x.vel(0).x = mathf::lerp(-0.5f, 0.5f, x.rng.value());
                else
                    x.vel(0).x += mathf::sign(x.vel(0).x) * 0.02f * p.swing_fac * (1.f - ai.fear);
                if (p.voice_counter > 0)
                    --p.voice_counter;
                if (ai.fear > 0.f)
                    x.vel(1) += custom::deg_to_vec(x.rng.value() * 360.f) * ai.fear;
                if (ai.want_to_sleep)
                    p.fall_asleep += 1.f / mathf::lerp(120.f, 700.f, x.rng.value());
                else
                    p.fall_asleep -= 0.033333335f;
                p.fall_asleep = mathf::clamp01(p.fall_asleep);
            }
            update_rope(x);
            p.footing_counter = 0;
            p.rope_length += 0.1f;
            auto& r = x.rope();
            float num2 = p.rope_length;
            for (uint32_t i = 1; i < r.bend_count; ++i)
                num2 -= rw::distance(r.bends[i - 1], r.bends[i]);
            if (num2 <= 0.f || r.bend_count > 3) {
                detach_rope(x);
                return;
            }
            vec2 const end = r.bends[r.bend_count - 1];
            if (!custom::dist_less(x.pos(1), end, num2)) {
                vec2 const a = custom::dir_vec(x.pos(1), end);
                float const num3 = rw::distance(x.pos(1), end);
                p.rope_stretch = num3 / num2 - 1.f;
                x.pos(1) += a * (num3 - num2) * 0.15f;
                x.vel(1) += a * (num3 - num2) * 0.15f;
            }
            // standing on something: off its edge
            for (uint32_t j = 0; j < 2; ++j) {
                if (x.w.body_chunk_get_contact_point(x.ch[j]).y >= 0)
                    continue;
                for (int32_t k = -1; k < 2; k += 2)
                    if (!x.solid_at(j, k, -1)) {
                        x.vel(0).y -= 1.f;
                        x.vel(1).y -= 1.f;
                        x.vel(0).x += float(k);
                        x.vel(1).x += float(k);
                        break;
                    }
            }
        }

        // LanternMouse.MoveTowards
        void move_towards(ctx& x, vec2 move_to) {
            auto& p = x.x;
            vec2 v = custom::dir_vec(x.pos(0), move_to);
            if (pathing::tile_access(x.room, tile_of(x.pos(1))) >= pathing::access::climb)
                v *= 0.5f;
            if (!x.footing())
                v *= 0.3f;
            if (x.solid_at(1, 0, -1) && ((v.x < -0.5f && x.pos(0).x > x.pos(1).x + 5.f) || (v.x > 0.5f && x.pos(0).x < x.pos(1).x - 5.f))) {
                // turning around on the floor: the head goes over the body
                float const s = v.x < 0.f ? -1.f : 1.f;
                x.vel(0).x -= s * 1.3f;
                x.vel(1).x += s * 0.5f;
                if (!x.solid_at(0, 0, 1))
                    x.vel(0).y += 3.2f;
            }
            x.vel(0) += v * 4.2f * p.run_speed;
            x.vel(1) -= v * 1.f * p.run_speed;
            physics::set_go_through_floors(x.state, x.o(), move_to.y < x.pos(0).y - 5.f);
        }

        // LanternMouse.Sit: on the floor, rear down, head up
        void sit(ctx& x) {
            if (pathing::tile_access(x.room, tile_of(x.pos(1))) == pathing::access::floor && !x.solid_at(0, 0, 1) && !x.solid_at(1, 0, 1)) {
                x.vel(0).y += 2.f;
                x.vel(1).y -= 4.f;
                x.x.profile_fac *= 0.6f;
                x.x.sitting = 1;
            }
        }

        // StandardPather.FollowPath from tile `from`
        movement_connection follow_path(sys::game_state const& state, pathing::field_id f, ivec2 from) {
            auto const step = pathing::next_step(state, f, from);
            if (!step)
                return movement_connection{};
            return movement_connection{from, *step, true};
        }

        // LanternMouse.Run (Mouse_Scurry: silent). The fields' steps are
        // Standard moves, OpenDiagonals (a special move) and drops
        // (DropToFloor); pipes are dens.hpp's
        void run(ctx& x, movement_connection const& fc) {
            auto& p = x.x;
            auto& ai = x.ai();
            p.run_cycle += p.run_speed / mathf::lerp(4.f, 12.f, x.rng.value());
            if (fc.dest.x != fc.start.x) {
                if (fc.dest.x > fc.start.x)
                    p.profile_fac = mathf::min(p.profile_fac + 0.14285715f, 1.f);
                else
                    p.profile_fac = mathf::max(p.profile_fac - 0.14285715f, -1.f);
            } else {
                p.profile_fac = mathf::lerp(p.profile_fac, -custom::dir_vec(x.pos(1), x.pos(0)).x, 0.6f);
            }
            if (fc.dest.y > fc.start.y && pathing::tile_access(x.room, fc.dest) != pathing::access::climb)
                p.climbing_corridor = 1;
            ivec2 const step = fc.dest - fc.start;
            if (step.x != 0 && step.y != 0) {
                p.special_move_counter = 30;
                ai.special_move_destination = fc.dest;
            } else {
                vec2 const v = room_view::middle_of_tile(fc.dest);
                if (x.footing())
                    for (uint32_t j = 0; j < 2; ++j) {
                        if (fc.start.x == fc.dest.x)
                            x.vel(j).x += mathf::min((v.x - x.pos(j).x) / 8.f, 1.2f);
                        else if (fc.start.y == fc.dest.y)
                            x.vel(j).y += mathf::min((v.y - x.pos(j).y) / 8.f, 1.2f);
                    }
                auto const& last = ai.last_followed;
                if (last.valid && (x.footing() || accessible(x.room, x.pos(0)))
                    && ((fc.start.x != fc.dest.x && last.start.x == last.dest.x) || (fc.start.y != fc.dest.y && last.start.y == last.dest.y))) {
                    x.vel(0) *= 0.7f;
                    x.vel(1) *= 0.5f;
                }
                if (step.y < -1)
                    p.footing_counter = 0;
                move_towards(x, v);
            }
            ai.last_followed = fc;
        }

        // LanternMouse.Act
        void act(ctx& x) {
            auto& p = x.x;
            auto& w = x.w;
            ai_update(x);
            if (p.special_move_counter > 0) {
                --p.special_move_counter;
                vec2 const to = room_view::middle_of_tile(x.ai().special_move_destination);
                move_towards(x, to);
                if (custom::dist_less(x.pos(0), to, 5.f))
                    p.special_move_counter = 0;
            } else {
                if (!accessible(x.room, x.pos(0)) && !accessible(x.room, x.pos(1)))
                    p.footing_counter = 0;
                ivec2 const dest = w.creature_get_destination(x.c);
                float const threat = tracker::threat_utility(tracker::of(x.state, x.c));
                if (x.at() == dest || (tile_of(x.pos(1)) == dest && threat < 0.5f)) {
                    sit(x);
                    physics::set_go_through_floors(x.state, x.o(), false);
                } else {
                    auto const f = pathing::field_to(x.state, x.room.id, dest, walk);
                    auto conn = follow_path(x.state, f, x.at());
                    if (!conn.valid)
                        conn = follow_path(x.state, f, tile_of(x.pos(1)));
                    if (conn.valid)
                        run(x, conn);
                    else
                        physics::set_go_through_floors(x.state, x.o(), false);
                }
            }
            if (consious(x.state, x.c))
                p.profile_fac *= 0.97f;
            if (!custom::dist_less(x.pos(0), x.last_pos(0), 5.f))
                p.run_cycle += p.run_speed / 40.f;
            if (p.voice_counter > 0) {
                --p.voice_counter;
                return;
            }
            float const threat = tracker::threat_utility(tracker::of(x.state, x.c));
            float const chance = (x.has_rope() ? 0.1f : 1.f) / (x.beh() == behavior::flee ? mathf::lerp(80.f, 20.f, threat) : 100.f);
            if (!x.sleeping() && x.rng.value() < chance)
                squeak(x, mathf::inverse_lerp(0.5f, 1.f, threat));
        }

        // LanternMouse.Carried: it wriggles now and then while the slugcat
        // holding it stands still on ground it could walk on
        void carried(ctx& x) {
            if (x.w.creature_get_dead(x.c))
                return;
            auto& ai = x.ai();
            bool flag = accessible(x.room, x.pos(0)) || accessible(x.room, x.pos(1));
            uint32_t g = 0;
            auto const h = creature::holder(x.state, x.c, &g);
            if (h && creature::is_slugcat(x.w, h)) {
                auto const pp = creature::player_of(x.state, h);
                if (pp) {
                    auto const& in = x.w.player_get_input(pp)[0];
                    if (in.x != 0 || in.y != 0)
                        flag = false;
                }
            }
            if (flag) {
                --ai.struggle_a;
                if (ai.struggle_a < 0) {
                    if (x.rng.value() < 0.008333334f)
                        ai.struggle_a = x.rng.range(20, 400);
                    for (uint32_t i = 0; i < 2; ++i)
                        x.vel(i) += custom::deg_to_vec(x.rng.value() * 360.f) * 6.f * x.rng.value();
                }
            }
            --ai.struggle_b;
            if (ai.struggle_b < 0 && x.rng.value() < 0.008333334f)
                ai.struggle_b = x.rng.range(10, 100);
            if (x.viewed() && (ai.struggle_a < 0 || ai.struggle_b < 0)) {
                auto gr = rw::random(x.state.seed, x.state.tick, uint32_t(x.c.value));
                if (gr.value() < 0.025f)
                    lantern_mouse_graphics::reset_unconsious_profile(x.state, x.c);
                lantern_mouse_graphics::jostle(x.state, x.c, gr);
            }
        }

        // AirBreatherCreature.Update: it drowns under the water
        void breathe(ctx& x) {
            auto& p = x.x;
            if (x.w.creature_get_dead(x.c))
                return;
            bool const under = x.room.water && physics::object_submersion(x.state, x.room, x.o()) == 1.f;
            if (p.lungs == 1.f) {
                if (x.rng.value() < 0.016666668f && under)
                    p.lungs = mathf::max(0.f, p.lungs - 1.f / lung_capacity);
                return;
            }
            if (!under) {
                p.lungs = mathf::clamp01(p.lungs + 0.033333335f);
                return;
            }
            p.lungs = mathf::max(-1.f, p.lungs - 1.f / lung_capacity);
            if (p.lungs >= 0.3f)
                return;
            float const k = std::sin(mathf::inverse_lerp(0.3f, -0.3f, p.lungs) * mathf::pi);
            if (x.rng.value() < k * 0.5f)
                particles::bubbles(x.state, x.room.id, x.pos(0), 1, 6.f);
            for (uint32_t i = 0; i < 2; ++i)
                x.vel(i) += custom::rnv(x.rng) * x.w.body_chunk_get_rad(x.ch[i]) * 0.4f * x.rng.value() * k
                    + custom::deg_to_vec(mathf::lerp(-30.f, 30.f, x.rng.value())) * x.rng.value() * (i == 0 ? 0.4f : 0.2f) * k * k;
            if (p.lungs <= 0.f && x.rng.value() < 0.1f)
                creature::stun(x.state, x.c, x.rng.range(0, 18));
            if (p.lungs < -0.5f && x.rng.value() < 1.f / custom::lerp_map(p.lungs, -0.5f, -1.f, 90.f, 30.f))
                creature::die(x.state, x.c);
        }

        // LanternMouse.TerrainImpact: a hard landing hurts its eyes and
        // throws sparks (MouseGraphics.TerrainImpact)
        struct impact_ctx {
            sys::game_state* state;
            dcon::creature_id c;
        };
        void on_impact(void* user, dcon::physical_object_id, uint32_t k, ivec2, float speed, bool) {
            auto& ic = *static_cast<impact_ctx*>(user);
            auto& state = *ic.state;
            if (speed <= 12.f)
                return;
            auto const room = creature::room_of(state, ic.c);
            if (!state.viewed(room))
                return;
            lantern_mouse_graphics::ouch(state, ic.c, custom::int_clamp(int32_t(speed * 0.5f), 0, 30));
            if (battery(state, ic.c) <= 0.7f)
                return;
            auto gr = rw::random(state.seed, state.tick, rw::ids(uint32_t(ic.c.value), k));
            auto const k0 = chunk(state, ic.c, 0);
            vec3 const col = custom::hsl2rgb(hue(state, ic.c), 1.f, 0.9f);
            int32_t const n = gr.range(2, 6);
            for (int32_t i = 0; i < n; ++i)
                particles::spawn(state, room, particles::kind::spark, state.world.body_chunk_get_pos(k0),
                    state.world.body_chunk_get_vel(k0) + custom::deg_to_vec(360.f * gr.value()) * speed * gr.value(), 1.5f, 1.f, col);
        }

        bool spot_for_hanging(sys::game_state& state, dcon::creature_id c, room_view const& room, ivec2 tile, dangle& out) {
            ctx x(state, c, room, 1);
            out = dangle_tile(x, tile, true);
            return out.valid;
        }
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::lantern_mouse);
        auto const d = data(state, c);
        auto rng = rw::random(state.seed, 0, uint32_t(c.value));
        int32_t seed = int32_t(rng.next() & 0x7FFFFFFFu);
        if (state.spawn_seed >= 0)
            seed = state.spawn_seed;
        auto const view = world::get_room_view(state, room);
        // never inside terrain
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
        physics::object_desc desc;
        desc.air_friction = 0.999f;
        desc.gravity = 0.9f;
        desc.bounce = 0.1f;
        desc.surface_friction = 0.4f;
        desc.water_friction = 0.96f;
        desc.buoyancy = 0.95f;
        auto const o = physics::create_object(state, room, desc);
        // two chunks (5.5 and 6, 0.2 each) 12 apart
        auto const c0 = physics::add_chunk(state, o, at, 5.5f, 0.2f);
        auto const c1 = physics::add_chunk(state, o, at + vec2{0.f, -1.f}, 6.f, 0.2f);
        physics::add_connection(state, o, c0, c1, 12.f, physics::connection_type::normal, 1.f, 0.5f);
        w.try_create_creature_body(c, o);
        // GenerateIVars, from the ID's seed
        individual_variations iv;
        {
            rw::unity_random r{seed};
            float h;
            if (r.value() < 0.01f)
                h = r.value();
            else if (r.value() < 0.5f)
                h = mathf::lerp(0.f, 0.1f, r.value());
            else
                h = mathf::lerp(0.5f, 0.65f, r.value());
            iv.hue = h;
            iv.dominance = r.value();
        }
        auto& x = pl(state, c);
        x = creature::lantern_mouse_payload{};
        x.data = uint16_t(d.index());
        x.run_speed = 0.f;
        x.rope_length = 0.f;
        x.lungs = 1.f;
        x.battery = full_battery;
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_set_grasps(c, creature::grasp_pair{});
        w.creature_set_behavior(c, uint8_t(behavior::idle));
        w.creature_set_current_utility(c, 0.f);
        w.creature_set_destination(c, tile_of(at));
        w.lantern_mouse_data_set_seed(d, seed);
        w.lantern_mouse_data_set_ivars(d, iv);
        w.lantern_mouse_data_set_rope(d, rope_state{});
        w.lantern_mouse_data_set_ai(d, ai_state{});
        w.lantern_mouse_data_set_graphics(d, graphics_state{});
        tracker::clear(state, c);
        // LanternMouse.PlaceInRoom (story): on a dangle tile it starts out
        // hanging there, else it drops to the ground below
        dangle dg;
        ivec2 const tile = tile_of(at);
        if (spot_for_hanging(state, c, view, tile, dg)) {
            ctx cx(state, c, view, 2);
            cx.ai().dangle_spot = dg;
            attach_rope(cx, dg.attached_pos);
            x.rope_length = float_dist(dg.attached_pos, dg.body_pos) * 20.f;
        } else if (!accessible(view, tile)) {
            for (int32_t j = tile.y; j >= 0; --j)
                if (accessible(view, ivec2{tile.x, j})) {
                    vec2 const p = room_view::middle_of_tile(ivec2{tile.x, j});
                    physics::hard_set_position(state, c0, p);
                    physics::hard_set_position(state, c1, p + vec2{0.f, -1.f});
                    w.creature_set_destination(c, ivec2{tile.x, j});
                    break;
                }
        }
        lantern_mouse_graphics::reset_positions(state, c);
        if (w.lantern_mouse_data_get_rope(d).bend_count > 0)
            lantern_mouse_graphics::spawn_as_hanging(state, c);
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        creature::release_target(state, c);
        creature::erase(state, c);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        ctx x(state, c, room, 0);
        auto& p = x.x;
        // LanternMouse.Update: hurt, it may die or faint now and then
        if (!w.creature_get_dead(c)) {
            float const health = w.creature_get_health(c);
            if (health < 0.f && x.rng.value() < -health && x.rng.value() < 0.025f)
                creature::die(state, c);
            if (!w.creature_get_dead(c) && x.rng.value() * 0.7f > health && x.rng.value() < 0.125f)
                creature::stun(state, c, x.rng.range(1, x.rng.range(1, 27 - custom::int_clamp(int32_t(20.f * health), 0, 10))));
        }
        // a dead mouse's battery runs down
        if (w.creature_get_dead(c) && p.battery > 0)
            --p.battery;
        p.carried = creature::held(state, c) ? 1 : 0;
        if (p.carried)
            carried(x);
        // Creature.Update + PhysicalObject.Update
        creature::stun_countdown(state, c);
        impact_ctx ic{&state, c};
        physics::update_object(state, room, x.o(), on_impact, &ic);
        if (!w.creature_get_dead(c) && w.creature_get_health(c) < 0.f && x.rng.value() < -w.creature_get_health(c) && x.rng.value() < 0.025f)
            creature::die(state, c);
        breathe(x);
        if (!w.creature_is_valid(c))
            return;
        p.rope_stretch = 0.f;
        if (p.rope_invincible > 0)
            --p.rope_invincible;
        p.sitting = 0;
        p.shrinking_rope = 0;
        p.climbing_corridor = 0;
        if (x.has_rope()) {
            hang(x);
        } else {
            if (consious(state, c)) {
                ++p.footing_counter;
                act(x);
            } else {
                p.footing_counter = 0;
            }
            if (x.footing()) {
                // clinging on: gravity cancelled (PhysicalObject.gravity)
                float const g = w.physical_object_get_g(x.o());
                for (uint32_t i = 0; i < 2; ++i) {
                    x.vel(i) *= 0.8f;
                    x.vel(i).y += g;
                }
            }
        }
        // fleeing in mid air: it flails
        if (consious(state, c) && !x.footing() && x.beh() == behavior::flee)
            for (uint32_t j = 0; j < 2; ++j)
                if (accessible(room, x.pos(j)))
                    x.vel(j) += custom::deg_to_vec(x.rng.value() * 360.f) * x.rng.value() * 5.f;
        if (x.sleeping())
            p.wake_up -= 0.0045454544f;
        else if (p.fall_asleep < 0.8f || !x.has_rope())
            p.wake_up += 1.f / mathf::lerp(100.f, 400.f, x.rng.value());
        p.wake_up += x.ai().fear / mathf::lerp(3.f, 10.f, x.rng.value());
        p.wake_up = mathf::clamp01(p.wake_up);
    }

    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) {
        // what holds it is kept apart by the carrying
        return !creature::holds(state.world, t, c);
    }

    void collide(sys::game_state& state, dcon::creature_id c, room_view const&, uint32_t, dcon::creature_id, uint32_t) {
        float& f = pl(state, c).fall_asleep;
        f = mathf::max(0.f, f - 0.25f);
    }

    void collide_kin(sys::game_state& state, dcon::creature_id c, uint32_t i, dcon::creature_id other, uint32_t j) {
        auto& w = state.world;
        // both wake up a bit, and each may follow the other
        for (auto const m : {c, other}) {
            float& f = pl(state, m).fall_asleep;
            f = mathf::max(0.f, f - 0.25f);
        }
        // MouseAI.CollideWithMouse
        auto const follow = [&](dcon::creature_id me, dcon::creature_id them) {
            ivec2 const my_at = tile_of(w.body_chunk_get_pos(chunk(state, me, 0)));
            ivec2 const their_dest = w.creature_get_destination(them);
            if (custom::manhattan_distance(my_at, their_dest) < 4)
                return;
            float const mine = w.lantern_mouse_data_get_ivars(data(state, me)).dominance;
            float const theirs = w.lantern_mouse_data_get_ivars(data(state, them)).dominance;
            if ((theirs > mine && !pl(state, them).sitting) || pl(state, me).sitting || migrate::want_to(state, them)) {
                auto& ai = w.lantern_mouse_data_get_ai(data(state, me));
                ai.walk_with = their_dest;
                ai.has_walk_with = 1;
            }
        };
        follow(c, other);
        follow(other, c);
        // the upper one climbs over
        auto const a = chunk(state, c, i), b = chunk(state, other, j);
        bool const above = w.body_chunk_get_pos(a).y > w.body_chunk_get_pos(b).y;
        w.body_chunk_get_vel(above ? a : b).y += 2.f;
        w.body_chunk_get_vel(above ? b : a).y -= 2.f;
    }

    void violence(sys::game_state& state, dcon::creature_id c, uint32_t k, vec2 momentum, float damage, float stun_bonus, creature::damage_type) {
        auto& w = state.world;
        auto rng = rw::random(state.seed, state.tick, rw::ids(uint32_t(c.value), k));
        // Creature.Violence (HealthState, quickDeath)
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
        // LanternMouse.Violence: a squeak
        auto const view = world::get_room_view(state, creature::room_of(state, c));
        ctx x(state, c, view, rw::ids(3, k));
        squeak(x, 1.f);
    }

    void release(sys::game_state&, dcon::creature_id, dcon::creature_id) {}

    void died(sys::game_state&, dcon::creature_id) {
        // (LanternMouse.Die is Creature.Die; its battery runs down in update)
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
        auto const d = data(state, c);
        // the rope was tied in the room it left
        w.lantern_mouse_data_get_rope(d).bend_count = 0;
        // MouseAI.NewRoom; the dangle spot and the mouse it walked with are
        // in the other room
        auto& ai = w.lantern_mouse_data_get_ai(d);
        ai.dangle_checks = 0;
        ai.dangle_spot = dangle{};
        ai.has_walk_with = 0;
        ai.last_followed = movement_connection{};
        auto& x = pl(state, c);
        x.special_move_counter = 0;
        x.footing_counter = 0;
        w.creature_set_destination(c, t);
    }

    std::string describe(sys::game_state const& state, dcon::creature_id c) {
        static constexpr char const* names[] = {"idle", "flee", "escape rain"};
        auto const& w = state.world;
        auto const& x = pl(state, c);
        vec2 const p = w.body_chunk_get_pos(chunk(state, c, 0));
        uint32_t const b = w.creature_get_behavior(c);
        auto const& r = w.lantern_mouse_data_get_rope(data(state, c));
        std::string s = "lantern mouse at " + std::to_string(int(p.x)) + "," + std::to_string(int(p.y)) + " behavior " + (b < 3 ? names[b] : "?");
        if (r.bend_count > 0)
            s += " hanging (rope " + std::to_string(int(x.rope_length)) + ", " + std::to_string(int(r.bend_count) - 1) + " bends)" + (x.fall_asleep >= 1.f ? " asleep" : "");
        if (x.carried)
            s += " carried";
        if (x.battery < full_battery)
            s += " battery " + std::to_string(x.battery);
        return s;
    }

    bool spawn_spot(room_view const& room, ivec2 tile) {
        return room.inside(tile.x, tile.y) && accessible(room, tile) && room.get_tile(tile).water == 0;
    }

    bool sleeping(sys::game_state const& state, dcon::creature_id c) noexcept {
        return hanging(state, c) && pl(state, c).fall_asleep >= 1.f;
    }
    bool hanging(sys::game_state const& state, dcon::creature_id c) noexcept {
        return state.world.lantern_mouse_data_get_rope(data(state, c)).bend_count > 0;
    }
    float battery(sys::game_state const& state, dcon::creature_id c) noexcept {
        return custom::s_curve(mathf::inverse_lerp(0.f, float(full_battery), float(pl(state, c).battery)), 0.2f);
    }
    float hue(sys::game_state const& state, dcon::creature_id c) noexcept {
        return state.world.lantern_mouse_data_get_ivars(data(state, c)).hue;
    }
    float fear(sys::game_state const& state, dcon::creature_id c) noexcept {
        return state.world.lantern_mouse_data_get_ai(data(state, c)).fear;
    }

    //
    // The player side
    //

    dcon::creature_id held_by(sys::game_state const& state, dcon::player_id p) noexcept {
        if (!p)
            return dcon::creature_id{};
        auto const c = creature::grasped(state.world, player::creature_of(state, p), 0);
        return c && creature::is(state.world, c, creature::type::lantern_mouse) ? c : dcon::creature_id{};
    }

    dcon::creature_id pickup_candidate(sys::game_state const& state, dcon::player_id p, room_view const& room, float& score) {
        auto const& w = state.world;
        vec2 const p0 = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
        int32_t const flip = w.player_get_flip_direction(p);
        dcon::creature_id result;
        score = float_max;
        rooms::for_each_creature(state, room.id, creature::type::lantern_mouse, [&](dcon::creature_id c) {
            // Player.Grabability: TwoHands, always
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
        auto const view = world::get_room_view(state, creature::room_of(state, c));
        ctx x(state, c, view, 4);
        // (the port's: a grabbed mouse is off its rope at once)
        x.x.rope_invincible = 0;
        detach_rope(x);
        // SlugcatGrab: chunk 0 (a TwoHands object isn't dragged by its closest)
        creature::grab(state, player::creature_of(state, p), 0, c, 0);
        // LanternMouse.Grabbed
        squeak(x, tracker::threat_utility(tracker::of(state, c)) * 0.5f + x.rng.value() * 0.5f);
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
        auto const d = data(state, c);
        w.lantern_mouse_data_get_rope(d).bend_count = 0;
        auto& ai = w.lantern_mouse_data_get_ai(d);
        ai.dangle_checks = 0;
        ai.dangle_spot = dangle{};
        ai.has_walk_with = 0;
        ai.last_followed = movement_connection{};
        tracker::clear(state, c);
        lantern_mouse_graphics::reset_positions(state, c);
    }

    void thrown(sys::game_state& state, dcon::creature_id c, float force) {
        if (force > 4.f)
            creature::stun(state, c, 20);
    }

    float carry_slowdown(sys::game_state const& state, dcon::creature_id c) noexcept {
        return 1.f + mathf::max(0.f, creature::total_mass(state, c) - 0.2f);
    }
}
