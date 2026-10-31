// GarbageWorm.cs, GarbageWormAI.cs and GarbageWormState.cs: the garbage
// worm. See garbage_worm.hpp for what it does; the tentacle (Tentacle.cs
// with the worm's TentacleProps(false, false, true, 0.5f, 0f, 1.4f, 0f, 0f,
// 1.2f, 10f, ...)) is ported without its tile segments: its alignToSegment
// and goalAttraction (but the tip's) speeds are 0, so the segments only ever
// matter for the backtracking around corners, which is left out (the
// tentacle's backtrackFrom stays -1): a chain of TentacleChunks from the
// root, each kept within its share of idealLength (shortened by
// retractFac) of the one before, the tip drawn to the grab destination, the
// chunks colliding with the terrain.

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "garbage_worm.hpp"
#include "garbage_worm_graphics.hpp"
#include "garbage_worm_types.hpp"
#include "items.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "rain.hpp"
#include "rooms.hpp"
#include "tracker.hpp"

namespace garbage_worm {
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // Tentacle.TentacleProps: massDeteriorationPerChunk,
        // pullAtConnectionChunk, chunkVelocityCap
        constexpr float mass_deterioration_per_chunk = 0.5f;
        constexpr float pull_at_connection_chunk = 0.f;
        constexpr float chunk_velocity_cap = 10.f;
        // the template (StaticWorld): visualRadius
        constexpr float visual_radius = 800.f;
        // GarbageWorm: gravity
        constexpr float own_gravity = 0.9f;

        creature::garbage_worm_payload& pl(sys::game_state& state, dcon::creature_id c) { return state.world.creature_get_payload(c).garbage_worm; }
        creature::garbage_worm_payload const& pl(sys::game_state const& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).garbage_worm;
        }
        dcon::garbage_worm_data_id data(sys::game_state const& state, dcon::creature_id c) { return creature::garbage_worm_data(state.world, c); }

        ivec2 tile_of(vec2 p) { return room_view::get_tile_position(p); }

        // Room.garbageHoles: the tiles marked as garbage holes, in the level
        // file's order (column by column, each from the top)
        std::vector<ivec2>& holes_buffer() {
            static thread_local std::vector<ivec2> buf;
            return buf;
        }
        std::vector<ivec2> const& garbage_holes(room_view const& room) {
            auto& out = holes_buffer();
            out.clear();
            if (!room.flags)
                return out;
            for (int32_t x = 0; x < room.width; ++x)
                for (int32_t y = room.height - 1; y >= 0; --y)
                    if (room.flags[size_t(y) * size_t(room.width) + size_t(x)] & world::tile_garbage_hole)
                        out.push_back(ivec2{x, y});
            return out;
        }

        bool is_spear(sys::game_state const& state, dcon::item_id it) {
            if (!it || !state.world.item_is_valid(it))
                return false;
            auto const t = state.world.item_get_type(it);
            return t == world::item_type::spear || t == world::item_type::explosive_spear || t == world::item_type::electric_spear;
        }

        // the worm while it updates
        struct ctx {
            sys::game_state& state;
            dcon::creature_id c;
            room_view const& room;
            rw::rng& rng;
            dcon::garbage_worm_data_id d;
            creature::garbage_worm_payload& p;
            tentacle_state& t;
            ai_state& a;
            dcon::body_chunk_id b[2];

            ctx(sys::game_state& s, dcon::creature_id id, room_view const& r)
                : state(s), c(id), room(r), rng(s.world.creature_get_random(id)), d(data(s, id)), p(pl(s, id)), t(s.world.garbage_worm_data_get_tentacle(d)),
                  a(s.world.garbage_worm_data_get_ai(d)) {
                b[0] = creature::chunk(s, id, 0);
                b[1] = creature::chunk(s, id, 1);
            }
            vec2& pos(uint32_t i) { return state.world.body_chunk_get_pos(b[i]); }
            vec2& vel(uint32_t i) { return state.world.body_chunk_get_vel(b[i]); }
            tentacle_chunk& tip() { return t.c[t.count - 1]; }
            bool awake() const { return creature::consious(state, c); }
        };

        //
        // SharedPhysics.TerrainCollisionData: VerticalCollision,
        // SlopesVertically, HorizontalCollision (as tentacle_plant.cpp's)
        //
        struct terrain_collision {
            vec2 pos;
            vec2 last_pos;
            vec2 vel;
            float rad;
            ivec2 contact{0, 0};
        };
        void vertical_collision(room_view const& room, terrain_collision& cd) {
            ivec2 const tp = room_view::get_tile_position(cd.last_pos);
            bool const last_solid = room.get_tile(cd.last_pos).solid();
            if (cd.vel.y > 0.f) {
                int32_t const y = room_view::get_tile_position(vec2{0.f, cd.pos.y + cd.rad + 0.01f}).y;
                int32_t const y2 = room_view::get_tile_position(vec2{0.f, cd.last_pos.y + cd.rad}).y;
                int32_t const x = room_view::get_tile_position(vec2{cd.pos.x - cd.rad + 1.f, 0.f}).x;
                int32_t const x2 = room_view::get_tile_position(vec2{cd.pos.x + cd.rad - 1.f, 0.f}).x;
                for (int32_t i = y2; i <= y; ++i)
                    for (int32_t j = x; j <= x2; ++j)
                        if (room.get_tile(j, i).solid() && !room.get_tile(j, i - 1).solid() && (tp.y < i || last_solid)) {
                            cd.pos.y = float(i) * 20.f - cd.rad;
                            cd.vel.y = 0.f;
                            cd.contact.y = 1;
                            return;
                        }
            } else if (cd.vel.y < 0.f) {
                int32_t const y = room_view::get_tile_position(vec2{0.f, cd.pos.y - cd.rad - 0.01f}).y;
                int32_t const y2 = room_view::get_tile_position(vec2{0.f, cd.last_pos.y - cd.rad}).y;
                int32_t const x = room_view::get_tile_position(vec2{cd.pos.x - cd.rad + 1.f, 0.f}).x;
                int32_t const x2 = room_view::get_tile_position(vec2{cd.pos.x + cd.rad - 1.f, 0.f}).x;
                for (int32_t i = y2; i >= y; --i)
                    for (int32_t j = x; j <= x2; ++j)
                        if (room.get_tile(j, i).solid() && !room.get_tile(j, i + 1).solid() && (tp.y > i || last_solid)) {
                            cd.pos.y = (float(i) + 1.f) * 20.f + cd.rad;
                            cd.vel.y = 0.f;
                            cd.contact.y = -1;
                            return;
                        }
            }
        }
        void slopes_vertically(room_view const& room, terrain_collision& cd) {
            using world::slope_direction;
            ivec2 const tp = room_view::get_tile_position(cd.pos);
            ivec2 b{0, 0};
            slope_direction a = room.identify_slope(cd.pos);
            vec2 const mid = room_view::middle_of_tile(cd.pos);
            if (room.get_tile(cd.pos).t != world::terrain::slope) {
                if (room.identify_slope(tp.x - 1, tp.y) != slope_direction::broken && cd.pos.x - cd.rad <= mid.x - 10.f) {
                    a = room.identify_slope(tp.x - 1, tp.y);
                    b.x = -1;
                } else if (room.identify_slope(tp.x + 1, tp.y) != slope_direction::broken && cd.pos.x + cd.rad >= mid.x + 10.f) {
                    a = room.identify_slope(tp.x + 1, tp.y);
                    b.x = 1;
                } else if (cd.pos.y - cd.rad < mid.y - 10.f) {
                    if (room.identify_slope(tp.x, tp.y - 1) != slope_direction::broken) {
                        a = room.identify_slope(tp.x, tp.y - 1);
                        b.y = -1;
                    }
                } else if (cd.pos.y + cd.rad > mid.y + 10.f && room.identify_slope(tp.x, tp.y + 1) != slope_direction::broken) {
                    a = room.identify_slope(tp.x, tp.y + 1);
                    b.y = 1;
                }
            }
            if (a == slope_direction::broken)
                return;
            vec2 const v = room_view::middle_of_tile(tp + b);
            float num;
            int32_t num2;
            if (a == slope_direction::up_left) {
                num = cd.pos.x - (v.x - 10.f) + (v.y - 10.f);
                num2 = -1;
            } else if (a == slope_direction::up_right) {
                num = 20.f - (cd.pos.x - (v.x - 10.f)) + (v.y - 10.f);
                num2 = -1;
            } else if (a == slope_direction::down_left) {
                num = 20.f - (cd.pos.x - (v.x - 10.f)) + (v.y - 10.f);
                num2 = 1;
            } else {
                num = cd.pos.x - (v.x - 10.f) + (v.y - 10.f);
                num2 = 1;
            }
            if (num2 == -1 && cd.pos.y <= num + cd.rad + cd.rad)
                cd.pos.y = num + cd.rad + cd.rad;
            else if (num2 == 1 && cd.pos.y >= num - cd.rad - cd.rad)
                cd.pos.y = num - cd.rad - cd.rad;
        }
        void horizontal_collision(room_view const& room, terrain_collision& cd) {
            ivec2 const tp = room_view::get_tile_position(cd.last_pos);
            bool const last_solid = room.get_tile(cd.last_pos).solid();
            if (cd.vel.x > 0.f) {
                int32_t const x = room_view::get_tile_position(vec2{cd.pos.x + cd.rad + 0.01f, 0.f}).x;
                int32_t const x2 = room_view::get_tile_position(vec2{cd.last_pos.x + cd.rad, 0.f}).x;
                int32_t const y = room_view::get_tile_position(vec2{0.f, cd.pos.y + cd.rad - 1.f}).y;
                int32_t const y2 = room_view::get_tile_position(vec2{0.f, cd.pos.y - cd.rad + 1.f}).y;
                for (int32_t i = x2; i <= x; ++i)
                    for (int32_t j = y2; j <= y; ++j)
                        if (room.get_tile(i, j).solid() && !room.get_tile(i - 1, j).solid() && (tp.x < i || last_solid)) {
                            cd.pos.x = float(i) * 20.f - cd.rad;
                            cd.vel.x = 0.f;
                            cd.contact.x = 1;
                            return;
                        }
            } else if (cd.vel.x < 0.f) {
                int32_t const x = room_view::get_tile_position(vec2{cd.pos.x - cd.rad - 0.01f, 0.f}).x;
                int32_t const x2 = room_view::get_tile_position(vec2{cd.last_pos.x - cd.rad, 0.f}).x;
                int32_t const y = room_view::get_tile_position(vec2{0.f, cd.pos.y + cd.rad - 1.f}).y;
                int32_t const y2 = room_view::get_tile_position(vec2{0.f, cd.pos.y - cd.rad + 1.f}).y;
                for (int32_t i = x2; i >= x; --i)
                    for (int32_t j = y2; j <= y; ++j)
                        if (room.get_tile(i, j).solid() && !room.get_tile(i + 1, j).solid() && (tp.x > i || last_solid)) {
                            cd.pos.x = (float(i) + 1.f) * 20.f + cd.rad;
                            cd.vel.x = 0.f;
                            cd.contact.x = -1;
                            return;
                        }
            }
        }

        //
        // Tentacle
        //
        // TentacleChunk.Update (no rope, no phasing, backtrackFrom -1)
        void chunk_update(ctx& x, uint32_t i) {
            auto& t = x.t;
            auto& ch = t.c[i];
            ch.last_pos = ch.pos;
            float const share = t.ideal_length / float(t.count);
            float const ideal = share * mathf::lerp(1.f, 0.1f, t.retract_fac);
            if (i == 0) {
                // connectedChunk: the root
                vec2 const base = x.pos(1);
                vec2 const a = custom::dir_vec(ch.pos, base);
                float const num = rw::distance(ch.pos, base);
                if (num > ideal) {
                    ch.pos -= a * (ideal - num) * (1.f - pull_at_connection_chunk);
                    ch.vel -= a * (ideal - num) * (1.f - pull_at_connection_chunk);
                }
                ch.stretched_fac = share / mathf::max(1.f, num);
            } else {
                auto& prev = t.c[i - 1];
                vec2 const a = custom::dir_vec(ch.pos, prev.pos);
                float const num = rw::distance(ch.pos, prev.pos);
                if (num > ideal) {
                    ch.pos -= a * (ideal - num) * (1.f - mass_deterioration_per_chunk);
                    ch.vel -= a * (ideal - num) * (1.f - mass_deterioration_per_chunk);
                    prev.pos += a * (ideal - num) * mass_deterioration_per_chunk;
                    prev.vel += a * (ideal - num) * mass_deterioration_per_chunk;
                }
                ch.stretched_fac = share / mathf::max(1.f, num);
            }
            // only the tip has a goal attraction speed (the rest 0, and no
            // alignment to the segments); a limp tentacle has neither
            if (!t.limp && t.has_grab_dest && i + 1 == t.count)
                ch.vel += rw::clamp_magnitude(t.float_grab_dest - ch.pos, 20.f) * t.goal_attraction_speed_tip / 20.f;
            ch.vel = rw::clamp_magnitude(ch.vel, chunk_velocity_cap);
            ch.pos += ch.vel;
            terrain_collision cd{ch.pos, ch.pos - ch.vel, ch.vel, ch.rad};
            vertical_collision(x.room, cd);
            slopes_vertically(x.room, cd);
            horizontal_collision(x.room, cd);
            ch.contact = cd.contact;
            ch.pos = cd.pos;
            ch.vel = cd.vel;
        }
        // Tentacle.Update
        void tentacle_update(ctx& x) {
            for (uint32_t i = 0; i < x.t.count; ++i)
                chunk_update(x, i);
        }
        // Tentacle.MoveGrabDest (a new destination only in another tile)
        void move_grab_dest(ctx& x, vec2 to) {
            if (x.t.has_grab_dest && tile_of(x.t.float_grab_dest) == tile_of(to))
                return;
            x.t.float_grab_dest = to;
            x.t.has_grab_dest = 1;
        }
        // Tentacle.Reset: every chunk at `at`
        void tentacle_reset(ctx& x, vec2 at) {
            for (uint32_t i = 0; i < x.t.count; ++i) {
                auto& ch = x.t.c[i];
                ch.pos = at;
                ch.last_pos = at;
                ch.vel = vec2{0.f, 0.f};
                ch.contact = ivec2{0, 0};
            }
        }

        //
        // GarbageWormAI
        //
        tracker::params const& tracker_params() {
            // Tracker(10, 10, 200, 0.5, 10, 5, 20)
            static tracker::params const p = [] {
                tracker::params q;
                q.see_around_corners = 10;
                q.frames_to_remember = 200;
                q.ghost_dismissal_range = 20;
                return q;
            }();
            return p;
        }
        struct hook_ctx {
            sys::game_state& state;
            dcon::creature_id c;
            room_view const& room;
        };
        // ArtificialIntelligence.VisualScore/VisualContact from the head
        // (VisionPoint: the main chunk)
        bool visual_contact(hook_ctx const& x, vec2 look) {
            vec2 const eye = x.state.world.body_chunk_get_pos(creature::chunk(x.state, x.c, 0));
            if (!custom::dist_less(eye, look, visual_radius))
                return false;
            float score = mathf::inverse_lerp(visual_radius, 0.f, rw::distance(eye, look));
            if (pathing::narrow_space(x.room, tile_of(look)))
                score -= 0.5f;
            return score > 0.f && x.room.visual_contact(eye, look);
        }
        bool hook_sees(void* u, dcon::body_chunk_id k) {
            auto& x = *static_cast<hook_ctx*>(u);
            return visual_contact(x, x.state.world.body_chunk_get_pos(k));
        }
        bool hook_sees_point(void* u, vec2 p) {
            return visual_contact(*static_cast<hook_ctx*>(u), p);
        }
        // no relationship modules: only the Tracker
        creature::track_module hook_module(creature::relationship) { return creature::track_module::none; }

        // GarbageWormState.angryAt
        bool angry_contains(angry_list const& l, dcon::creature_id who) {
            for (uint32_t i = 0; i < l.count; ++i)
                if (l.who[i] == who)
                    return true;
            return false;
        }
        void angry_add(angry_list& l, dcon::creature_id who) {
            if (!who || angry_contains(l, who))
                return;
            if (l.count < max_angry_at) {
                l.who[l.count++] = who;
                return;
            }
            // full: the oldest goes
            for (uint32_t i = 0; i + 1 < max_angry_at; ++i)
                l.who[i] = l.who[i + 1];
            l.who[max_angry_at - 1] = who;
        }
        // GarbageWormAI.AngryAtCreature
        bool angry_at(ctx const& x, dcon::creature_id t) {
            return !creature::dead(x.state, t) && angry_contains(x.state.world.garbage_worm_data_get_angry(x.d), t);
        }

        // the interests follow the Tracker's representations
        // (CreateTrackerRepresentationForCreature adds one, a deleted
        // representation's goes); none for garbage worms and leeches
        void sync_interests(ctx& x) {
            auto& a = x.a;
            auto const& ts = tracker::of(x.state, x.c);
            uint32_t n = 0;
            for (uint32_t i = 0; i < a.interest_count; ++i)
                if (tracker::find(ts, a.interests[i].who))
                    a.interests[n++] = a.interests[i];
            a.interest_count = uint8_t(n);
            if (a.current && !tracker::find(ts, a.current))
                a.current = dcon::creature_id{};
            for (uint32_t i = 0; i < ts.count; ++i) {
                auto const who = ts.reps[i].who;
                if (!creature::live(x.state, who))
                    continue;
                auto const k = creature::type_of(x.state.world, who);
                if (k == creature::type::garbage_worm || k == creature::type::leech)
                    continue;
                bool found = false;
                for (uint32_t j = 0; j < a.interest_count && !found; ++j)
                    found = a.interests[j].who == who;
                if (found || a.interest_count >= max_interests)
                    continue;
                // CreatureInterest(ai, rep, bodySize)
                creature_interest ci;
                ci.who = who;
                float const size = creature::body_size(creature::template_of(x.state, who));
                ci.base_interest = size;
                ci.interest = size;
                ci.danger = size >= 1.f ? size : 0.f;
                a.interests[a.interest_count++] = ci;
            }
        }

        // the creature's main chunk, if it is out in the room
        dcon::body_chunk_id main_chunk_here(ctx const& x, dcon::creature_id t) {
            if (!creature::here(x.state, t, x.room.id))
                return dcon::body_chunk_id{};
            return creature::chunk_of(x.state, t, creature::main_chunk_of(x.state, t));
        }

        // CreatureInterest.Update
        void interest_update(ctx& x, creature_interest& ci) {
            auto const& w = x.state.world;
            auto const* r = tracker::find(tracker::of(x.state, x.c), ci.who);
            if (!r)
                return;
            bool const visual = r->visual != 0;
            ++ci.ticks_since_discovered;
            ci.interest = ci.base_interest;
            if (visual && !ci.last_visual && ci.ticks_since_visual > 40)
                ci.ticks_since_visual = 0;
            else
                ci.ticks_since_visual += x.rng.range(0, 3);
            ci.last_visual = visual ? 1 : 0;
            float const num = rw::distance(x.pos(1), room_view::middle_of_tile(tracker::best_guess(*r)));
            if (ci.danger == 0.f) {
                ci.interest /= float(800 + ci.ticks_since_discovered);
                ci.interest /= float(80 + ci.ticks_since_visual);
            } else {
                ci.interest /= 800.f + float(ci.ticks_since_discovered) * mathf::inverse_lerp(40.f, 660.f, num);
                ci.interest /= 80.f + float(ci.ticks_since_discovered) * mathf::inverse_lerp(40.f, 220.f, num);
            }
            ci.interest /= mathf::lerp(400.f, num, 0.3f);
            ci.interest *= 25600000.f;
            if (visual)
                if (auto const k = main_chunk_here(x, ci.who))
                    ci.interest *= mathf::lerp(1.f + rw::magnitude(w.body_chunk_get_vel(k)), 3.f, 0.75f);
        }

        // a random creature of the room (Room.creatures[Random.Range])
        dcon::creature_id random_room_creature(ctx& x) {
            uint32_t n = 0;
            rooms::for_each_creature(x.state, x.room.id, [&](dcon::creature_id) { ++n; });
            if (n == 0)
                return dcon::creature_id{};
            uint32_t const pick = uint32_t(x.rng.range(0, int32_t(n)));
            uint32_t i = 0;
            dcon::creature_id out;
            rooms::for_each_creature(x.state, x.room.id, [&](dcon::creature_id o) {
                if (i++ == pick)
                    out = o;
            });
            return out;
        }

        // Weapon.AllGraspsLetGoOfThisObject, the holder's ReleaseGrasp and
        // the worm's Grab (Garbage_Worm_Snatch_Spear has no sound id)
        void snatch(ctx& x, dcon::item_id it) {
            auto& w = x.state.world;
            if (auto const by = w.item_get_grabbed_by(it); by && w.creature_is_valid(by))
                creature::release_grasp(x.state, by, w.item_get_grabbed_hand(it));
            // the Hunter's spear on its back
            rooms::for_each_player(x.state, x.room.id, [&](dcon::player_id pp) {
                if (w.player_get_back_spear(pp) == it)
                    w.player_set_back_spear(pp, dcon::item_id{});
            });
            items::creature_grab(x.state, x.c, 0, it);
            // Weapon.Forbid
            w.item_set_forbidden_to_player(it, 40);
        }

        // the spear `t` holds (its hands, else a slugcat's back)
        dcon::item_id held_spear(ctx const& x, dcon::creature_id t) {
            auto const& w = x.state.world;
            for (uint32_t j = 0; j < 2; ++j)
                if (auto const it = creature::grasped_item(w, t, j); is_spear(x.state, it))
                    return it;
            if (auto const pp = creature::player_of(x.state, t)) {
                auto const s = w.player_get_back_spear(pp);
                if (is_spear(x.state, s) && w.item_get_mode(s) == world::weapon_mode::on_back)
                    return s;
            }
            return dcon::item_id{};
        }

        void set_charge(ctx& x, vec2 at) {
            x.p.charge_pos = at;
            x.p.has_charge = 1;
        }

        // GarbageWorm.Retract, Extend, NewHole
        void retract(ctx& x) { x.p.retract_speed = -0.033333335f; }
        void map_floor(ctx& x);
        void new_hole(ctx& x, bool burrowed);
        void extend(ctx& x) {
            new_hole(x, true);
            x.p.grab_spears = 1;
            x.p.retract_speed = 0.005f;
        }

        // GarbageWormAI.Update
        void ai_update(ctx& x) {
            auto& state = x.state;
            auto& w = state.world;
            auto& a = x.a;
            auto& p = x.p;
            hook_ctx hx{state, x.c, x.room};
            tracker::hooks h;
            h.user = &hx;
            h.sees = hook_sees;
            h.sees_point = hook_sees_point;
            h.module = hook_module;
            tracker::update(state, x.c, x.room, tile_of(p.root_pos), tracker_params(), h);
            sync_interests(x);
            a.show_as_angry = 0;
            if (a.attack_counter > 0) {
                ++a.attack_counter;
                if (a.attack_counter > 220)
                    a.attack_counter = 0;
                return;
            }
            float best = std::numeric_limits<float>::lowest();
            int32_t pick = -1;
            for (int32_t i = int32_t(a.interest_count) - 1; i >= 0; --i) {
                auto& ci = a.interests[i];
                interest_update(x, ci);
                float const v = ci.interest * (ci.who == a.current ? 1.2f : 1.f);
                if (v > best) {
                    best = v;
                    pick = i;
                }
            }
            a.stress = mathf::clamp(a.stress - 0.005f, 0.f, 1.f);
            // the anger of a worm of the room spreads
            if (auto const o = random_room_creature(x); o && o != x.c && creature::is(w, o, creature::type::garbage_worm)) {
                auto const& other = w.garbage_worm_data_get_angry(creature::garbage_worm_data(w, o));
                if (other.count > 0)
                    angry_add(w.garbage_worm_data_get_angry(x.d), other.who[x.rng.range(0, other.count)]);
            }
            creature_interest const* ci = pick >= 0 ? &a.interests[pick] : nullptr;
            auto const* rep = ci ? tracker::find(tracker::of(state, x.c), ci->who) : nullptr;
            if (ci && !rep)
                ci = nullptr;
            if (ci && best > 0.05f) {
                a.current = ci->who;
                auto const crit = ci->who;
                auto const mk = main_chunk_here(x, crit);
                bool const visual = rep->visual != 0 && mk;
                vec2 const guess = room_view::middle_of_tile(tracker::best_guess(*rep));
                p.look_point = visual ? w.body_chunk_get_pos(mk) : guess;
                float num3 = ci->danger;
                if (visual) {
                    num3 *= 7.f + rw::magnitude(w.body_chunk_get_vel(mk));
                    num3 /= mathf::max(rw::distance(x.pos(0), w.body_chunk_get_pos(mk)), 1.f);
                } else {
                    num3 /= mathf::max(rw::distance(x.pos(0), guess), 1.f);
                }
                a.stress = mathf::clamp(a.stress + num3 / 5.f, 0.f, 1.f);
                a.search_counter = 0;
                if (p.grab_spears && p.extended > 0.5f && !creature::grasped_object(w, x.c, 0) && visual) {
                    auto const spear = held_spear(x, crit);
                    if (spear && custom::dist_less(x.pos(0), w.body_chunk_get_pos(items::chunk(state, spear)), x.t.ideal_length * 0.8f)) {
                        vec2 const sp = w.body_chunk_get_pos(items::chunk(state, spear));
                        set_charge(x, sp);
                        if (custom::dist_less(x.pos(0), sp, 10.f)) {
                            snatch(x, spear);
                            p.grab_spears = 0;
                        }
                    } else if (!spear && angry_at(x, crit)) {
                        set_charge(x, w.body_chunk_get_pos(mk));
                        uint32_t const n = creature::chunk_count_of(state, crit);
                        for (uint32_t l = 0; l < n; ++l) {
                            auto const k = creature::chunk_of(state, crit, l);
                            if (custom::dist_less(x.pos(0), w.body_chunk_get_pos(k), 10.f + w.body_chunk_get_rad(k))) {
                                // (no Garbage_Worm_Grab_Creature sound)
                                creature::grab(state, x.c, 0, crit, l);
                                p.grab_spears = 0;
                                break;
                            }
                        }
                    }
                }
                if (auto const held = creature::grasped(w, x.c, 0); held && creature::live(state, held)) {
                    a.retract_counter = 0;
                    float num5 = creature::dead(state, held) ? 40.f : 400.f;
                    if (a.floor_count > 0) {
                        if (a.look_at_floor.y > x.room.default_water_level - 3) {
                            a.look_at_floor = a.floor_tiles[x.rng.range(0, a.floor_count)];
                            if (x.rng.value() < 0.14285715f)
                                a.shake_dir = custom::deg_to_vec(mathf::lerp(-140.f, 140.f, x.rng.value()));
                            set_charge(x, x.pos(0) + a.shake_dir * 200.f);
                            x.vel(0) += a.shake_dir * x.rng.value() * 25.f;
                            auto const gk = creature::chunk_of(state, held, creature::grasp_chunk(w, x.c, 0));
                            num5 = (gk && rw::magnitude(w.body_chunk_get_vel(gk)) > 15.f) ? 20.f : 120.f;
                        } else {
                            if (x.rng.value() < 0.008333334f)
                                a.look_at_floor = a.floor_tiles[x.rng.range(0, a.floor_count)];
                            set_charge(x, room_view::middle_of_tile(a.look_at_floor) + custom::deg_to_vec(x.rng.value() * 360.f) * x.rng.value() * 10.f
                                    + vec2{0.f, -200.f});
                        }
                    }
                    if (x.rng.value() < 1.f / num5)
                        creature::release_grasp(state, x.c, 0);
                }
            } else {
                a.current = dcon::creature_id{};
                if ((x.rng.value() < 0.025f || a.search_counter < 5) && a.floor_count > 0) {
                    a.look_at_floor = a.floor_tiles[x.rng.range(0, a.floor_count)];
                    p.look_point = room_view::middle_of_tile(a.look_at_floor) + vec2{mathf::lerp(-10.f, 10.f, x.rng.value()), -10.f};
                }
                ++a.search_counter;
                if (a.search_counter > 100 && x.rng.value() < 0.003030303f && custom::dist_less(x.pos(0), p.look_point, 100.f)) {
                    a.attack_counter = 1;
                    a.search_counter = 0;
                }
            }
            // (a vulture or a leviathan scares it from 1000 px: neither is ported)
            constexpr float num6 = 340.f;
            vec2 const guess = rep ? room_view::middle_of_tile(tracker::best_guess(*rep)) : vec2{0.f, 0.f};
            if (a.stress > 0.9f && rep && rw::distance(x.pos(1), guess) < num6) {
                ++a.retract_counter;
                if (rw::distance(x.pos(1), guess) < 40.f)
                    a.retract_counter += 10;
                if (rw::distance(x.pos(0), guess) < 40.f)
                    ++a.retract_counter;
                if (a.retract_counter > 40) {
                    retract(x);
                    a.retract_counter = 0;
                }
            } else if (a.retract_counter > 0) {
                --a.retract_counter;
            }
            auto const* cy = rain::of_room(state, x.room.id);
            int32_t const until_rain = cy ? rain::time_until_rain(*cy) : std::numeric_limits<int32_t>::max();
            if (p.extended == 0.f && until_rain > 60 && a.retract_counter == 0) {
                a.come_back_out_counter += x.rng.range(0, 3);
                if (a.come_back_out_counter > 200)
                    extend(x);
            } else {
                a.come_back_out_counter = 0;
            }
            if (rep && angry_at(x, rep->who))
                a.show_as_angry = 1;
        }

        // GarbageWormAI.MapFloor
        void map_floor(ctx& x) {
            auto& a = x.a;
            a.floor_count = 0;
            int32_t const steps = int32_t(12.f * x.p.body_size);
            for (int32_t i = -1; i < 2; i += 2) {
                ivec2 v = tile_of(x.p.root_pos);
                for (int32_t j = 0; j < steps; ++j) {
                    bool found = false;
                    for (int32_t k = -1; k < 2; ++k)
                        if (!x.room.get_tile(v + ivec2{i, k}).solid() && x.room.get_tile(v + ivec2{i, k - 1}).solid()) {
                            v += ivec2{i, k};
                            found = true;
                            break;
                        }
                    if (!found)
                        break;
                    if (a.floor_count < max_floor_tiles)
                        a.floor_tiles[a.floor_count++] = v;
                }
            }
            if (a.floor_count == 0) {
                auto const& holes = garbage_holes(x.room);
                a.floor_tiles[a.floor_count++] = x.p.hole >= 0 && size_t(x.p.hole) < holes.size() ? holes[size_t(x.p.hole)] : tile_of(x.p.root_pos);
            }
            a.look_at_floor = a.floor_tiles[x.rng.range(0, a.floor_count)];
        }

        // GarbageWorm.NewHole: a hole of the room no other worm of it has
        void new_hole(ctx& x, bool burrowed) {
            auto& state = x.state;
            auto& w = state.world;
            auto& p = x.p;
            auto& a = x.a;
            auto const& holes = garbage_holes(x.room);
            std::vector<int32_t> free_holes;
            for (int32_t i = 0; i < int32_t(holes.size()); ++i)
                free_holes.push_back(i);
            auto const taken = [&](dcon::creature_id o) {
                if (o == x.c)
                    return;
                int32_t const h = w.creature_get_payload(o).garbage_worm.hole;
                free_holes.erase(std::remove(free_holes.begin(), free_holes.end(), h), free_holes.end());
            };
            // (while the rooms update, the room's own worms; serially, when
            // the room index may not have the worms just made yet, all of
            // them)
            if (state.parallel) {
                rooms::for_each_creature(state, x.room.id, creature::type::garbage_worm, taken);
            } else {
                creature::for_each_of(w, creature::type::garbage_worm, [&](dcon::creature_id o) {
                    if (creature::room_of(state, o) == x.room.id)
                        taken(o);
                });
            }
            if (free_holes.empty()) {
                a.come_back_out_counter = 0;
                p.retract_speed = -0.033333335f;
                a.floor_count = 0;
                return;
            }
            p.hole = int16_t(free_holes[size_t(x.rng.range(0, int32_t(free_holes.size())))]);
            ivec2 const tile = holes[size_t(p.hole)] + ivec2{0, 1};
            p.root_pos = room_view::middle_of_tile(tile) + vec2{0.f, -10.f + w.body_chunk_get_rad(x.b[1])};
            tentacle_reset(x, p.root_pos);
            if (burrowed) {
                physics::hard_set_position(state, x.b[0], p.root_pos);
            } else {
                // the tentacle stretched up out of the hole, as far as it
                // reaches or the terrain lets it
                std::vector<ivec2> segs{tile};
                ivec2 t = tile;
                for (int32_t y = tile.y + 1; float(y) < float(tile.y) + x.t.ideal_length / 20.f && !x.room.get_tile(t).solid(); ++y) {
                    segs.push_back(t);
                    t.y = y;
                }
                for (uint32_t k = 0; k < x.t.count; ++k) {
                    float const t_pos = float(k + 1) / float(x.t.count);
                    int32_t const s = custom::int_clamp(int32_t(t_pos * float(segs.size() - 1)), 0, int32_t(segs.size()) - 1);
                    x.t.c[k].pos = room_view::middle_of_tile(segs[size_t(s)]);
                    x.t.c[k].last_pos = x.t.c[k].pos;
                }
                physics::hard_set_position(state, x.b[0], room_view::middle_of_tile(t));
                x.t.retract_fac = 0.f;
                p.extended = 1.f;
            }
            physics::hard_set_position(state, x.b[1], p.root_pos);
            map_floor(x);
        }

        // Creature.Stun + GarbageWorm.Stun: it lets go
        void stun(sys::game_state& state, dcon::creature_id c, int32_t st) {
            creature::stun(state, c, st);
            creature::release_grasp(state, c, 0);
        }

        // GarbageWorm.Carry
        void carry(ctx& x) {
            auto& state = x.state;
            auto& w = state.world;
            auto const n = x.t.count;
            vec2 const dir = n >= 2 ? rw::normalized(x.t.c[n - 1].pos - x.t.c[n - 2].pos) : vec2{0.f, 1.f};
            auto const obj = creature::grasped_object(w, x.c, 0);
            auto const it = creature::grasped_item(w, x.c, 0);
            if (it) {
                // grabbed by another as well
                if (w.physical_object_get_grabbed_by_count(obj) > 1) {
                    creature::release_grasp(state, x.c, 0);
                    return;
                }
                w.item_set_rotation(it, custom::perpendicular_vector(dir));
                auto const k = items::chunk(state, it);
                w.body_chunk_set_last_pos(k, w.body_chunk_get_pos(k));
                w.body_chunk_set_pos(k, x.pos(0) + dir * 6.f);
                w.body_chunk_set_vel(k, vec2{0.f, 0.f});
            } else {
                auto const t = creature::grasped(w, x.c, 0);
                uint32_t const gi = creature::grasp_chunk(w, x.c, 0);
                if (!t || !creature::here(state, t, x.room.id) || gi >= creature::chunk_count_of(state, t)) {
                    creature::release_grasp(state, x.c, 0);
                    return;
                }
                auto const gk = creature::chunk_of(state, t, gi);
                if (!custom::dist_less(x.pos(0), w.body_chunk_get_pos(gk), 100.f)) {
                    creature::release_grasp(state, x.c, 0);
                    return;
                }
                x.p.extended = 1.f;
                uint32_t const m = creature::chunk_count_of(state, t);
                for (uint32_t i = 0; i < m; ++i) {
                    auto const k = creature::chunk_of(state, t, i);
                    // Tile.WaterSurface
                    if (x.room.get_tile(w.body_chunk_get_pos(k)).water == 2)
                        w.body_chunk_get_vel(k).y -= 4.f;
                }
                // SharedPhysics.ConnectChunks(main, grabbed, rad * 0.2, 1, 0.45, 1)
                vec2& ap = x.pos(0);
                vec2& av = x.vel(0);
                vec2& bp = w.body_chunk_get_pos(gk);
                vec2& bv = w.body_chunk_get_vel(gk);
                float const ma = w.body_chunk_get_mass(x.b[0]), mb = w.body_chunk_get_mass(gk);
                float const dist = w.body_chunk_get_rad(gk) * 0.2f;
                vec2 const a = custom::dir_vec(ap, bp);
                float const num = rw::distance(ap, bp);
                float const num2 = mathf::lerp(ma / (ma + mb), 1.f, 0.45f);
                ap -= a * (dist - num) * (1.f - num2);
                av -= a * (dist - num) * (1.f - num2);
                bp += a * (dist - num) * num2;
                bv += a * (dist - num) * num2;
            }
            if (x.p.extended == 0.f && x.rng.value() < 0.025f) {
                creature::release_grasp(state, x.c, 0);
                // a spear drawn into the hole is gone
                if (it)
                    state.defer(x.room.id, [it](sys::game_state& s) {
                        if (s.world.item_is_valid(it))
                            items::destroy(s, it);
                    });
            }
        }
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 pos) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::garbage_worm);
        auto const d = data(state, c);
        rw::rng rng{state.random.next() | 1u};
        int32_t seed = int32_t(rng.next() & 0x7FFFFFFFu);
        if (state.spawn_seed >= 0)
            seed = state.spawn_seed;
        // GarbageWormState(bodySize: Lerp(0.35, 1, SeededRandom(seed) ^ 0.5))
        float body_size = 1.f;
        {
            rw::unity_random r{seed};
            body_size = mathf::lerp(0.35f, 1.f, mathf::pow(r.value(), 0.5f));
        }
        physics::object_desc desc;
        desc.gravity = own_gravity;
        desc.air_friction = 0.99f;
        desc.bounce = 0.1f;
        desc.surface_friction = 0.47f;
        desc.water_friction = 0.92f;
        desc.buoyancy = 0.95f;
        auto const o = physics::create_object(state, room, desc);
        // two chunks (8, 0.05), no connection; the root doesn't collide
        physics::add_chunk(state, o, pos, 8.f, 0.05f);
        auto const root = physics::add_chunk(state, o, pos, 8.f, 0.05f);
        w.body_chunk_set_collide_with_terrain(root, 0);
        physics::set_go_through_floors(state, o, true);
        w.try_create_creature_body(c, o);
        w.creature_set_random(c, rng);
        w.creature_set_graphics_random(c, rw::rng{rng.next() | 1u});
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_set_grasps(c, creature::grasp_pair{});
        w.creature_set_behavior(c, uint8_t(behavior::search));
        w.creature_set_current_utility(c, 0.f);
        w.creature_set_destination(c, room_view::get_tile_position(pos));
        auto& p = pl(state, c);
        uint16_t const keep = p.data;
        p = creature::garbage_worm_payload{};
        p.data = keep;
        p.body_size = body_size;
        p.extended = 1.f;
        p.retract_speed = 1.f;
        p.last_extended = 1;
        p.grab_spears = 1;
        p.hole = -1;
        p.can_be_hit = 1;
        p.root_pos = pos;
        p.look_point = pos + vec2{0.f, 100.f};
        w.garbage_worm_data_set_seed(d, seed);
        // the tentacle: 400 * bodySize long, (int)(15 * Lerp(bodySize, 1,
        // 0.5)) chunks of radius 2 * Lerp(bodySize, 1, 0.5)
        tentacle_state t;
        float const f = mathf::lerp(body_size, 1.f, 0.5f);
        t.count = uint8_t(std::clamp(int32_t(15.f * f), 2, int32_t(max_tentacle_chunks)));
        t.ideal_length = 400.f * body_size;
        for (uint32_t i = 0; i < t.count; ++i) {
            t.c[i] = tentacle_chunk{};
            t.c[i].rad = 2.f * f;
            t.c[i].pos = pos;
            t.c[i].last_pos = pos;
        }
        w.garbage_worm_data_set_tentacle(d, t);
        ai_state a;
        a.shake_dir = custom::deg_to_vec(mathf::lerp(-140.f, 140.f, w.creature_get_random(c).value()));
        w.garbage_worm_data_set_ai(d, a);
        // GarbageWormAI's constructor: the anger of every live garbage worm
        // of the world
        angry_list angry;
        creature::for_each_of(w, creature::type::garbage_worm, [&](dcon::creature_id o) {
            if (o == c || w.creature_get_dead(o))
                return;
            auto const& other = w.garbage_worm_data_get_angry(creature::garbage_worm_data(w, o));
            for (uint32_t i = 0; i < other.count; ++i)
                angry_add(angry, other.who[i]);
        });
        w.garbage_worm_data_set_angry(d, angry);
        w.garbage_worm_data_set_graphics(d, graphics_state{});
        tracker::clear(state, c);
        place_in_room(state, c, room, pos);
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        creature::release_target(state, c);
        creature::erase(state, c);
    }

    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, vec2) {
        auto const view = world::get_room_view(state, room);
        ctx x(state, c, view);
        // Tentacle.NewRoom, NewHole(false)
        x.t.has_grab_dest = 0;
        new_hole(x, false);
        x.p.last_extended = x.p.extended > 0.f ? 1 : 0;
        garbage_worm_graphics::reset_positions(state, c);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        ctx x(state, c, room);
        auto& p = x.p;
        auto& t = x.t;
        // Creature.Update + PhysicalObject.Update
        creature::stun_countdown(state, c);
        physics::update_object(state, room, body(state, c), nullptr, nullptr);
        bool const awake = x.awake();
        tentacle_update(x);
        t.limp = awake ? 0 : 1;
        t.retract_fac = 1.f - p.extended;
        // (the Garbage_Worm sound loops, Withdraw and Emerge have no sound ids)
        p.last_extended = p.extended > 0.f ? 1 : 0;
        w.body_chunk_set_collide_with_terrain(x.b[0], p.extended == 1.f ? 1 : 0);
        p.can_be_hit = p.extended > 0.f ? 1 : 0;
        bool const last_extended = p.last_extended != 0;
        {
            float const reach = t.ideal_length * 2.f * p.extended;
            if (!custom::dist_less(x.pos(1), x.pos(0), reach))
                x.pos(0) = x.pos(1) + custom::dir_vec(x.pos(1), x.pos(0)) * reach;
        }
        auto& a = x.a;
        if (awake) {
            if (p.has_charge) {
                p.look_point = p.charge_pos + custom::dir_vec(x.pos(0), p.charge_pos) * 100.f;
                x.vel(0) += rw::clamp_magnitude(p.charge_pos - x.pos(0), 20.f) / 5.f;
                p.has_charge = 0;
            }
            p.extended = mathf::clamp(p.extended + p.retract_speed, 0.f, 1.f);
            ai_update(x);
            if (p.retract_speed < 0.f)
                p.look_point = p.root_pos + vec2{0.f, 1000.f};
            move_grab_dest(x, p.look_point);
            float const value = rw::distance(x.tip().pos, p.look_point);
            if (a.attack_counter == 0) {
                t.goal_attraction_speed_tip = mathf::lerp(0.15f, 1.9f, mathf::inverse_lerp(40.f, a.search_counter > 0 ? 90.f : 290.f, value));
                if (pathing::ai_terrain_proximity(state, room.id, tile_of(x.pos(0))) < 2) {
                    for (uint32_t k = 0; k < 8; ++k)
                        if (pathing::ai_terrain_proximity(state, room.id, tile_of(x.pos(0)) + custom::eight_directions[k]) > 1) {
                            x.tip().vel += rw::to_vec2(custom::eight_directions[k]) * 2.f;
                            break;
                        }
                }
            } else if (a.attack_counter < 20) {
                t.goal_attraction_speed_tip = 0.1f;
            } else if (a.attack_counter < 40) {
                t.goal_attraction_speed_tip = 40.f;
                x.vel(0) += rw::clamp_magnitude(p.look_point - x.pos(0), 30.f);
            } else if (a.attack_counter < 190) {
                x.pos(0) = p.look_point;
            } else {
                p.look_point.y += 20.f;
                t.goal_attraction_speed_tip = 0.01f;
            }
            // CurrentlyLookingAtScaryCreature
            bool scary = false;
            for (uint32_t i = 0; i < a.interest_count && !scary; ++i)
                scary = a.current && a.interests[i].who == a.current && a.interests[i].danger > 0.f;
            for (uint32_t k = 0; k < t.count; ++k) {
                auto& ch = t.c[k];
                float const num8 = (float(k) + 0.5f) / float(t.count);
                ch.vel *= mathf::lerp(0.9f, 0.99f, num8);
                if (a.attack_counter > 20 || p.extended < 1.f)
                    ch.vel.y += 0.5f;
                else
                    ch.vel.y += (1.f - num8) * 0.5f;
                float num9 = std::sin(mathf::pi * mathf::pow(num8, 2.f)) * 0.1f;
                if (scary)
                    num9 *= 3.f * mathf::inverse_lerp(220.f, 20.f, value);
                else if (a.attack_counter > 0 && a.attack_counter < 20)
                    num9 *= 3.f;
                ch.vel += custom::dir_vec(p.look_point, ch.pos) * num9;
                if (k > 1) {
                    vec2 const dv = custom::dir_vec(t.c[k - 2].pos, ch.pos);
                    ch.vel += dv * 0.2f;
                    t.c[k - 2].vel -= dv * 0.2f;
                }
            }
        } else {
            for (uint32_t k = 0; k < t.count; ++k) {
                t.c[k].vel *= 0.95f;
                t.c[k].vel.y -= 0.5f;
            }
        }
        if (p.extended == 0.f) {
            // down in its hole
            if (!last_extended) {
                for (uint32_t i = 0; i < 2; ++i) {
                    w.body_chunk_set_collide_with_terrain(x.b[i], 0);
                    x.pos(i) = p.root_pos + vec2{0.f, -50.f};
                    x.vel(i) = vec2{0.f, 0.f};
                }
            } else {
                w.body_chunk_set_collide_with_terrain(x.b[0], 0);
                x.pos(0) = p.root_pos + vec2{0.f, -50.f};
                x.vel(0) = vec2{0.f, 0.f};
            }
            // (no spears stick in creatures here: nothing stuck to destroy)
        } else {
            x.pos(1) = p.root_pos;
            x.vel(1) = vec2{0.f, 0.f};
            for (uint32_t i = 0; i < 2; ++i)
                w.body_chunk_set_collide_with_terrain(x.b[i], 1);
            // the head on the tentacle's tip (backtrackFrom is always -1:
            // halfway each)
            float const num14 = 0.5f;
            vec2 const dir = custom::dir_vec(x.pos(0), x.tip().pos);
            float const num15 = rw::distance(x.pos(0), x.tip().pos);
            x.pos(0) -= (0.f - num15) * dir * (1.f - num14);
            x.vel(0) -= (0.f - num15) * dir * (1.f - num14);
            x.tip().pos += (0.f - num15) * dir * num14;
            x.tip().vel += (0.f - num15) * dir * num14;
            if (awake)
                x.vel(0).y += own_gravity;
        }
        if (creature::grasped_object(w, c, 0))
            carry(x);
        // what the log shows
        behavior b = behavior::search;
        if (p.extended == 0.f)
            b = behavior::hidden;
        else if (a.attack_counter > 0)
            b = behavior::dig;
        else if (a.current)
            b = behavior::watch;
        w.creature_set_behavior(c, uint8_t(b));
        w.creature_set_current_utility(c, a.stress);
        w.creature_set_destination(c, tile_of(p.root_pos));
    }

    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) {
        if (pl(state, c).extended <= 0.f)
            return false;
        return !creature::holds(state.world, t, c) && !creature::holds(state.world, c, t);
    }

    void collide(sys::game_state&, dcon::creature_id, room_view const&, uint32_t, dcon::creature_id, uint32_t) {}

    void violence(sys::game_state& state, dcon::creature_id c, uint32_t k, vec2 momentum, float damage, float stun_bonus, creature::damage_type type) {
        auto& w = state.world;
        if (pl(state, c).extended <= 0.f)
            return;
        // GarbageWorm.Violence: a weapon's hit (anything but a bite here)
        // makes it angry at every slugcat of the room
        if (type != creature::damage_type::bite) {
            auto& angry = w.garbage_worm_data_get_angry(data(state, c));
            rooms::for_each_creature(state, creature::room_of(state, c), creature::type::slugcat, [&](dcon::creature_id s) { angry_add(angry, s); });
        }
        // Creature.Violence (a CreatureState: no health, no instant death)
        auto const hit = chunk(state, c, k < 2 ? k : 0);
        w.body_chunk_get_vel(hit) += rw::clamp_magnitude(momentum / w.body_chunk_get_mass(hit), 10.f);
        // baseStunResistance 1
        stun(state, c, int32_t(damage * 30.f + stun_bonus));
    }

    void release(sys::game_state&, dcon::creature_id, dcon::creature_id) {
        // only its grasp holds on: the generic grasp, let go of by
        // creature::release
    }

    void died(sys::game_state&, dcon::creature_id) {}

    std::string describe(sys::game_state const& state, dcon::creature_id c) {
        static constexpr char const* names[] = {"searching", "watching", "digging", "hidden"};
        auto const& w = state.world;
        auto const& p = pl(state, c);
        auto const& a = w.garbage_worm_data_get_ai(data(state, c));
        auto const& angry = w.garbage_worm_data_get_angry(data(state, c));
        vec2 const h = w.body_chunk_get_pos(chunk(state, c, 0));
        uint32_t const b = w.creature_get_behavior(c);
        std::string held;
        if (creature::grasped_item(w, c, 0))
            held = " holding a spear";
        else if (auto const t = creature::grasped(w, c, 0))
            held = std::string{" holding a "} + creature::to_string(w.creature_get_type(t));
        return "garbage worm at " + std::to_string(int(h.x)) + "," + std::to_string(int(h.y)) + " hole " + std::to_string(p.hole) + " " + (b < 4 ? names[b] : "?")
            + " extended " + std::to_string(p.extended) + " stress " + std::to_string(a.stress) + (angry.count ? " angry at " + std::to_string(angry.count) : "") + held;
    }

    bool spawn_spot(room_view const& room, ivec2 tile) {
        if (!room.inside(tile.x, tile.y) || !room.flags)
            return false;
        return (room.flags[size_t(tile.y) * size_t(room.width) + size_t(tile.x)] & world::tile_garbage_hole) != 0;
    }

    bool can_be_hit(sys::game_state const& state, dcon::creature_id c) noexcept { return pl(state, c).can_be_hit != 0; }

    bool show_as_angry(sys::game_state const& state, dcon::creature_id c) noexcept {
        return state.world.garbage_worm_data_get_ai(data(state, c)).show_as_angry != 0;
    }
    float stress(sys::game_state const& state, dcon::creature_id c) noexcept { return state.world.garbage_worm_data_get_ai(data(state, c)).stress; }
    int32_t attack_counter(sys::game_state const& state, dcon::creature_id c) noexcept {
        return state.world.garbage_worm_data_get_ai(data(state, c)).attack_counter;
    }

    uint32_t hole_count(room_view const& room) noexcept { return uint32_t(garbage_holes(room).size()); }
}
