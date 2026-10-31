#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#include "lizard_ai.hpp"
#include "dens.hpp"
#include "migrate.hpp"
#include "rooms.hpp"
#include "tracker.hpp"
#include "lizard_graphics.hpp"
#include "items.hpp"
#include "physics.hpp"

// LizardAI: DetermineBehavior's utility comparison and the behaviours, over
// the modules it keeps: the shared Tracker, RelationshipTracker, PreyTracker,
// ThreatTracker and AgressionTracker (tracker.hpp) over every creature and
// slugcat of the room (vision with the breed's angles and the Black
// lizard's SuperHearing), the NoiseTracker, StuckTracker, LurkTracker,
// LizardSpitTracker, the Yellow pack's YellowAI and the lizards'
// communication (aggression and submission between rivals). It hunts its
// prey (the slugcat by how much it likes it, the rest by StaticWorld's
// relationships), flees what it is afraid of (Flee, ThreatTracker.FleeTo
// within the room), fights rivals (Fighting) and wanders to other rooms
// (AbstractBehavior's migration roll, Travelling: migrate.hpp). The
// RainTracker sends it home to its den as the rain approaches (EscapeRain),
// and it carries its prey there (ReturnPrey) or goes there hurt (Injured):
// dens.hpp, through other rooms when the den is elsewhere. No friends,
// missions (but LeaveRoom after submitting), masks or spears.

namespace lizard::ai {
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    using pathing::access;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        constexpr float float_max = std::numeric_limits<float>::max();

        vec2 chunk_pos(sys::game_state const& state, dcon::creature_id l, uint32_t i) {
            return state.world.body_chunk_get_pos(chunk(state, l, i));
        }

        float float_dist(ivec2 a, ivec2 b) {
            return rw::magnitude(rw::to_vec2(a - b));
        }

        bool lurker(breed b) {
            return b == breed::white || b == breed::salamander || b == breed::eel || b == breed::peach;
        }

        // LizardAI.LikeOfPlayer: the community's like (0, nothing changes it
        // here; a Cyan isn't mean outside the arena) leaning on the spawn
        // data's {Mean}, then on the temporary like
        float like_of_player(sys::game_state const& state, dcon::creature_id l) {
            float const evil = state.world.creature_get_payload(l).lizard.spawn_data_evil;
            float num = mathf::lerp(0.f, -evil, std::fabs(evil));
            float const temp = state.world.lizard_data_get_ai(creature::lizard_data(state.world, l)).temp_like;
            return mathf::lerp(num, temp, std::fabs(temp));
        }

        // LizardAI.VisualScore (+ ArtificialIntelligence.VisualScore): within
        // the visual radius, under or above the water, the breed's vision angles
        float visual_score(sys::game_state const& state, dcon::creature_id l, room_view const& room, vec2 look, float bonus) {
            auto const& p = params_of(state, l);
            vec2 const eye = chunk_pos(state, l, 0);
            float const r = p.visual_radius * (1.f + bonus);
            if (!custom::dist_less(eye, look, r))
                return 0.f;
            float num = mathf::inverse_lerp(r, 0.f, rw::distance(eye, look));
            if (room.water) {
                bool const deep_eye = room.get_tile(eye).water == 1, deep_look = room.get_tile(look).water == 1;
                if (deep_eye != deep_look)
                    num -= 1.f - p.through_surface_vision;
                if (deep_eye || deep_look)
                    num -= 1.f - p.water_vision;
            }
            if (pathing::narrow_space(room, room_view::get_tile_position(look)))
                num -= 0.5f;
            vec2 const c1 = chunk_pos(state, l, 1);
            return num - mathf::inverse_lerp(p.perfect_vision_angle, p.periferal_vision_angle, rw::dot(rw::normalized(eye - c1), rw::normalized(look - c1)));
        }

        // ArtificialIntelligence.VisualContact(BodyChunk): with the chunk's
        // VisibilityBonus from how fast it moves (the slugcat's own stealth isn't ported)
        bool sees_chunk(sys::game_state const& state, dcon::creature_id l, room_view const& room, dcon::body_chunk_id c) {
            auto const& w = state.world;
            float const mbv = params_of(state, l).movement_based_vision;
            vec2 const pos = w.body_chunk_get_pos(c);
            float const bonus = mathf::clamp(mathf::lerp(-mbv, 1.f, mathf::inverse_lerp(1.f, 10.f, rw::distance(w.body_chunk_get_last_pos(c), pos))) * mbv, -1.f, 1.f);
            return visual_score(state, l, room, pos, bonus) > 0.f && room.visual_contact(chunk_pos(state, l, 0), pos);
        }
        bool sees_point(sys::game_state const& state, dcon::creature_id l, room_view const& room, vec2 pos) {
            return visual_score(state, l, room, pos, 0.f) > 0.f && room.visual_contact(chunk_pos(state, l, 0), pos);
        }

        // the lizard's tracker modules (LizardAI's constructor)
        tracker::params const& tracker_params(breed b) {
            static tracker::params const normal = [] {
                tracker::params p;
                p.see_around_corners = 10;
                p.frames_to_remember = -1;
                p.ghost_dismissal_range = 10;
                p.prey_max = 5;
                p.persistance_bias = 2.f;
                p.sure_to_get = 3.f;
                p.sure_to_lose = 70.f;
                p.threat_max = 3;
                p.anger_up = 0.001f;
                p.anger_down = 0.001f;
                return p;
            }();
            static tracker::params const red = [] {
                tracker::params p = normal;
                p.give_up_unreachable = 1800;
                return p;
            }();
            return b == breed::red ? red : normal;
        }

        // the tracker's questions, answered by the lizard
        struct ai_ctx {
            sys::game_state& state;
            dcon::creature_id l;
            room_view const& room;
        };
        bool hook_sees(void* u, dcon::body_chunk_id k) {
            auto& x = *static_cast<ai_ctx*>(u);
            return sees_chunk(x.state, x.l, x.room, k);
        }
        bool hook_sees_point(void* u, vec2 pos) {
            auto& x = *static_cast<ai_ctx*>(u);
            return sees_point(x.state, x.l, x.room, pos);
        }
        relationship hook_relate(void* u, creature::tracked const& r) {
            auto& x = *static_cast<ai_ctx*>(u);
            return relationship_to(x.state, x.l, r.who);
        }
        // LizardAI.CreatureSpotted
        void hook_spotted(void* u, creature::tracked const& r, bool first) {
            auto& x = *static_cast<ai_ctx*>(u);
            if (x.state.viewed(x.room.id))
                lizard_graphics::creature_spotted(x.state, x.l, first, r.who);
            if (r.rel.go_for_kill() && r.rel.intensity > (first ? 0.2f : 0.45f) && grasped(x.state, x.l) != r.who)
                enter_animation(x.state, x.l, first ? animation::prey_spotted : animation::prey_re_spotted, false);
        }
        bool hook_reach(void* u, ivec2 t) {
            auto& x = *static_cast<ai_ctx*>(u);
            return reachable_and_getbackable(x.state, x.l, x.room, t);
        }
        tracker::hooks make_hooks(ai_ctx& x) {
            tracker::hooks h;
            h.user = &x;
            h.sees = hook_sees;
            h.sees_point = hook_sees_point;
            h.relate = hook_relate;
            h.module = tracker::default_module;
            h.spotted = hook_spotted;
            h.reach = hook_reach;
            return h;
        }

        // the representation of what it hunts, fights or flees, if any
        creature::tracked const* rep_of(sys::game_state const& state, dcon::creature_id l, dcon::creature_id t) {
            return t ? tracker::find(state, l, t) : nullptr;
        }
        creature::tracked const* prey_rep(sys::game_state const& state, dcon::creature_id l) {
            return tracker::prey(tracker::of(state, l));
        }
        float chance_of_finding(creature::tracked const& r) {
            return tracker::chance_of_finding(r, 10);
        }

        // SuperHearing.Update (the Black lizard): what moves on the terrain
        // within 350 of its head is heard as if seen
        void super_hearing(sys::game_state& state, dcon::creature_id l, room_view const& room, tracker::hooks const& h) {
            auto& w = state.world;
            vec2 const head = chunk_pos(state, l, 0);
            auto const hear = [&](dcon::creature_id t) {
                uint32_t const n = creature::chunk_count_of(state, t);
                for (uint32_t i = 0; i < n; ++i) {
                    auto const c = creature::chunk_of(state, t, i);
                    if (w.body_chunk_get_contact_point(c) == ivec2{0, 0})
                        continue;
                    float const loud = mathf::min((rw::distance(w.body_chunk_get_last_pos(c), w.body_chunk_get_pos(c)) - 3.f) * 4.f, 10.f) * 0.1f * 350.f;
                    if (custom::dist_less(w.body_chunk_get_pos(c), head, loud)) {
                        tracker::see(state, l, room, t, h);
                        return;
                    }
                }
            };
            rooms::for_each_creature(state, room.id, [&](dcon::creature_id o) {
                if (o != l && creature::here(state, o, room.id))
                    hear(o);
            });
        }

        // LizardAI.LizardInjuryTracker.Utility
        float injury_utility(sys::game_state const& state, dcon::creature_id l) {
            return custom::s_curve(mathf::inverse_lerp(0.2f, 0.9f, 1.f - mathf::clamp(state.world.creature_get_health(l), 0.f, 1.f)), 0.01f);
        }

        // StuckTracker.Update (40 positions, from the 30th, within 5 tiles, more than 10)
        void stuck_update(sys::game_state& state, dcon::creature_id l) {
            auto& a = state.world.lizard_data_get_ai(creature::lizard_data(state.world, l));
            auto& s = a.stuck;
            ivec2 const at = here(state, l);
            int32_t close = 0;
            if (!s.satisfied_with_this_position && custom::manhattan_distance(at, state.world.creature_get_destination(l)) > 3) {
                s.head = (s.head + stuck_tracker::tracked - 1) % stuck_tracker::tracked;
                s.past[s.head] = at;
                s.count = std::min(s.count + 1, stuck_tracker::tracked);
                for (int32_t j = 30; j < s.count; ++j)
                    if (float_dist(at, s.past[(s.head + j) % stuck_tracker::tracked]) < 5.f)
                        ++close;
            }
            s.counter = custom::int_clamp(close > 10 ? s.counter + 1 : s.counter - 2, 0, 200);
        }
        float stuck_utility(sys::game_state const& state, dcon::creature_id l) {
            return mathf::inverse_lerp(100.f, 200.f, float(state.world.lizard_data_get_ai(creature::lizard_data(state.world, l)).stuck.counter));
        }

        // NoiseTracker.HeardNoise + Update, with LizardAI.ReactToNoise: the
        // noises of this tick it hears. Noises carry no source here: one close
        // to where it last knew the slugcat to be is the slugcat's, the rest
        // are mysterious and worth examining
        void noise_update(sys::game_state& state, dcon::creature_id l, room_view const& room) {
            auto& w = state.world;
            auto& a = w.lizard_data_get_ai(creature::lizard_data(w, l));
            auto& z = w.lizard_data_get_core(creature::lizard_data(w, l));
            breed const b = breed_of(state, l);
            vec2 const head = chunk_pos(state, l, 0);
            float const hearing = b == breed::black || b == breed::indigo ? 2.f : custom::lerp_map(a.run_speed, 0.f, 0.6f, 1.3f, 0.6f);
            for (auto const& n : state.noises(room.id)) {
                float const under = room.point_submerged(n.pos) || room.get_tile(head).water == 1 ? 0.2f : 1.f;
                if (!custom::dist_less(head, n.pos, n.strength * hearing * under))
                    continue;
                // ignoreSeenNoises
                if (sees_point(state, l, room, n.pos))
                    continue;
                // TheorizedSource: the noise of a creature it tracks when
                // close to where it was last known to be (noises carry
                // their source object here: its creature, if it tracks it)
                creature::tracked const* source = nullptr;
                auto const& ts = tracker::of(state, l);
                for (uint32_t i = 0; i < ts.count && !source; ++i) {
                    auto const& r = ts.reps[i];
                    if (n.source && creature::body_of(state, r.who) == n.source)
                        source = &r;
                    else if (!n.source && rw::distance(n.pos, r.last_seen) < custom::lerp_map(float(r.ticks_since_seen), 20.f, 600.f, 200.f, 1000.f))
                        source = &r;
                }
                auto const* pr = prey_rep(state, l);
                bool const from_prey = source && pr && source->who == pr->who;
                if (source)
                    // CreatureRepresentation.HeardThisCreature
                    if (auto* r = tracker::find(tracker::of(state, l), source->who); r && r->ticks_since_seen > 10 + 5)
                        r->ticks_since_seen = (r->ticks_since_seen - 10) / 2;
                if (!source) {
                    a.mysterious_noises += 1.f;
                    a.mysterious_noise_counter = 200;
                    a.examining = true;
                    a.examine_pos = room_view::get_tile_position(n.pos);
                }
                // LizardAI.ReactToNoise
                if (source && b == breed::black) {
                    z.bubble = std::max(z.bubble, 4);
                } else if (from_prey) {
                    enter_animation(state, l, animation::prey_re_spotted, false);
                } else if (!source && (behavior(state.world.creature_get_behavior(l)) == behavior::idle || behavior(state.world.creature_get_behavior(l)) == behavior::investigate_sound || behavior(state.world.creature_get_behavior(l)) == behavior::lurk)) {
                    enter_animation(state, l, animation::hear_sound, false);
                } else {
                    make_sound(state, l, emotion::general_small_noise);
                    z.bubble = std::max(z.bubble, 5);
                }
            }
            ivec2 const at = here(state, l);
            if (a.examining && (custom::manhattan_distance(at, a.examine_pos) < 3
            || (custom::manhattan_distance(at, a.examine_pos) < 10 && sees_point(state, l, room, room_view::middle_of_tile(a.examine_pos)))))
                a.examining = false;
            if (a.mysterious_noise_counter > 0)
                --a.mysterious_noise_counter;
            else if (a.mysterious_noises > 0.f)
                a.mysterious_noises = mathf::max(0.f, a.mysterious_noises - 0.0125f);
        }
        float noise_utility(sys::game_state const& state, dcon::creature_id l) {
            auto const& a = state.world.lizard_data_get_ai(creature::lizard_data(state.world, l));
            if (!a.examining)
                return 0.f;
            return mathf::pow(mathf::inverse_lerp(1.f, 20.f, a.mysterious_noises), 0.5f);
        }

        //
        // LizardAI.LurkTracker
        //

        // LurkPosScore (AItile.visibility, the tiles seen from a tile, isn't
        // mapped here: those terms are left out)
        float lurk_pos_score(sys::game_state& state, dcon::creature_id l, room_view const& room, ivec2 t) {
            auto const& p = params_of(state, l);
            if (!room.inside(t.x, t.y) || !pathing::accessible(room, t, p.movement) || room.get_tile(t).t == world::terrain::slope)
                return -100000.f;
            if (!reachable_and_getbackable(state, l, room, t))
                return -100000.f;
            float num = 0.f;
            if (breed_of(state, l) == breed::white) {
                num = mathf::clamp(float(pathing::floor_altitude(room, t)), 1.f, 5.f) / float(std::max(1, pathing::ai_terrain_proximity(state, room.id, t)));
            } else if (p.swimmer) {
                if (pathing::ai_terrain_proximity(state, room.id, t) > 2)
                    return -100000.f;
                if (pathing::tile_access(room, t) != access::floor && room.get_tile(t).water != 1)
                    return -100000.f;
                num = 40.f / mathf::max(1.f, float(std::abs(room.default_water_level - t.y) - 5));
                num = t.y < room.default_water_level ? num * 5.f : num / 5.f;
                if (t.y < room.default_water_level - 10)
                    num -= custom::lerp_map(float(t.y), float(room.default_water_level - 10), float(room.default_water_level - 40), 0.f, 100.f);
                if (pathing::tile_access(room, t) == access::floor)
                    num += 20.f;
            }
            if (pathing::narrow_space(room, t))
                num -= 10000.f;
            return num;
        }

        // LurkTracker.Update
        void lurk_update(sys::game_state& state, dcon::creature_id l, room_view const& room) {
            auto& w = state.world;
            auto& a = w.lizard_data_get_ai(creature::lizard_data(w, l));
            auto rng = rw::random(state.seed, state.tick, uint32_t(l.value));
            auto& lk = a.lurk;
            ivec2 const look{rng.range(0, room.width), rng.range(0, room.height)};
            if (room.visual_contact(room_view::middle_of_tile(lk.lurk_position), room_view::middle_of_tile(look)) && rng.value() < 0.1f)
                lk.look_position = look;
            ivec2 const at = here(state, l);
            if (custom::manhattan_distance(at, lk.lurk_position) > 10 || !reachable_and_getbackable(state, l, room, lk.lurk_position)) {
                ivec2 const t = at + ivec2{rng.range(-15, 16), rng.range(-15, 16)};
                if (lurk_pos_score(state, l, room, t) > lurk_pos_score(state, l, room, lk.lurk_position)) {
                    lk.lurk_position = t;
                    lk.look_position = t;
                    lk.best_vis_look = 0;
                }
            }
            if (rng.value() < 0.00083333335f)
                lk.lurk_position = ivec2{rng.range(0, room.width), rng.range(0, room.height)};
        }

        // LurkTracker.Utility
        float lurk_utility(sys::game_state& state, dcon::creature_id l, room_view const& room) {
            if (breed_of(state, l) == breed::white)
                return 0.5f;
            if (!params_of(state, l).swimmer)
                return 0.f;
            auto const& lk = state.world.lizard_data_get_ai(creature::lizard_data(state.world, l)).lurk;
            if (lurk_pos_score(state, l, room, lk.lurk_position) <= 0.f)
                return 0.f;
            return room.get_tile(lk.lurk_position).water == 0 ? 0.2f : 0.5f;
        }

        //
        // LizardAI.LizardSpitTracker (Red, Caramel)
        //

        // SpitFromPosScore
        float spit_from_pos_score(sys::game_state& state, dcon::creature_id l, room_view const& room, ivec2 t) {
            if (!reachable_and_getbackable(state, l, room, t))
                return std::numeric_limits<float>::lowest();
            auto const& s = state.world.lizard_data_get_ai(creature::lizard_data(state.world, l)).spit;
            float num = -float_dist(t, s.want_to_spit_at_pos);
            if (room.visual_contact(room_view::middle_of_tile(t), room_view::middle_of_tile(s.want_to_spit_at_pos)))
                num += 1000.f;
            return num;
        }

        void spit_update(sys::game_state& state, dcon::creature_id l, room_view const& room) {
            auto& w = state.world;
            auto& a = w.lizard_data_get_ai(creature::lizard_data(w, l));
            auto& s = a.spit;
            auto rng = rw::random(state.seed, state.tick, uint32_t(l.value));
            if (!s.target_reachable && s.want_to_spit && !at_spit_pos(state, l))
                s.go_to_spit_pos_counter = 200;
            else if (s.go_to_spit_pos_counter > 0)
                --s.go_to_spit_pos_counter;
            if (--s.random_cycle < -100)
                s.random_cycle = rng.range(100, 800);
            if (s.delay > 0)
                --s.delay;
            s.spitting = false;
            s.want_to_spit = false;
            if (behavior(state.world.creature_get_behavior(l)) == behavior::flee || behavior(state.world.creature_get_behavior(l)) == behavior::travelling || behavior(state.world.creature_get_behavior(l)) == behavior::escape_rain)
                return;
            auto const* pr = prey_rep(state, l);
            if (!pr)
                return;
            auto const p = pr->who;
            bool const sees = pr->visual != 0;
            ivec2 const g = tracker::best_guess(*pr);
            ivec2 const at = here(state, l);
            if (float_dist(g, at) < 6.f)
                return;
            if (at_spit_pos(state, l)) {
                if (behavior(state.world.creature_get_behavior(l)) == behavior::go_to_spit_pos)
                    s.spitting = true;
                if (!sees && creature::here(state, p, room.id)) {
                    uint32_t const n = creature::chunk_count_of(state, p);
                    if (room.visual_contact(chunk_pos(state, l, 0), w.body_chunk_get_pos(creature::chunk_of(state, p, uint32_t(rng.range(0, int32_t(n)))))))
                        see(state, l, p);
                }
            }
            if (s.random_cycle < 0)
                s.want_to_spit = true;
            s.target_reachable = false;
            for (int32_t n = 1; n < 3 && !s.target_reachable; ++n)
                for (auto const d : custom::eight_directions_and_zero) {
                    if (reachable_and_getbackable(state, l, room, g + d * n)) {
                        s.target_reachable = true;
                        break;
                    }
                }
            if (!s.want_to_spit)
                s.want_to_spit = !s.target_reachable;
            if (s.target_reachable)
                s.go_to_spit_pos_counter = 0;
            int32_t const alt = pathing::floor_altitude(room, g);
            if ((s.want_to_spit && s.random_cycle >= 0) || (alt > 4 && float_dist(at, g) > 20.f - float(alt))) {
                s.want_to_spit = false;
                if (pathing::tile_access(room, g) >= access::climb) {
                    vec2 const danger = creature::main_pos(state, p);
                    float const side = mathf::sign(danger.x - chunk_pos(state, l, 0).x);
                    for (int k = 0; k < 2 && !s.want_to_spit; ++k) {
                        vec2 d = danger;
                        for (int i = 0; i < 40; ++i) {
                            d.y -= 20.f;
                            if (k == 1)
                                d.x += side * 5.f;
                            ivec2 const t = room_view::get_tile_position(d);
                            if (pathing::tile_access(room, t) == access::floor && reachable_and_getbackable(state, l, room, t)) {
                                s.want_to_spit = true;
                                break;
                            }
                        }
                    }
                }
            }
            if (!s.want_to_spit) {
                ++s.dont_wanna_spit_counter;
                s.wanna_spit_counter = custom::int_clamp(s.wanna_spit_counter - 1, 0, 200);
                if (s.dont_wanna_spit_counter > 100)
                    s.spit_from_pos = at;
                return;
            }
            s.dont_wanna_spit_counter = 0;
            s.wanna_spit_counter = custom::int_clamp(s.wanna_spit_counter + 1, 0, 200);
            s.want_to_spit_at_pos = g;
            for (int j = 0; j < 10; ++j) {
                ivec2 t{rng.range(0, room.width), rng.range(0, room.height)};
                if (rng.value() < 0.5f)
                    t = room_view::get_tile_position(room_view::middle_of_tile(s.spit_from_pos) + custom::rnv(rng) * 200.f * rng.value());
                if (spit_from_pos_score(state, l, room, t) > spit_from_pos_score(state, l, room, s.temp_spit_from_pos))
                    s.temp_spit_from_pos = t;
            }
            if (spit_from_pos_score(state, l, room, s.temp_spit_from_pos) > spit_from_pos_score(state, l, room, s.spit_from_pos) + 10.f)
                s.spit_from_pos = s.temp_spit_from_pos;
            if (!creature::here(state, p, room.id))
                return;
            if (s.wanna_spit_counter > 100 && !at_spit_pos(state, l))
                return;
            if (s.wanna_spit_counter <= 100 && !sees)
                return;
            s.spitting = true;
            s.general_aim_up = mathf::lerp(s.general_aim_up, mathf::lerp(rng.value(), 0.5f, rng.value()), 0.003f);
        }

        //
        // YellowAI: the pack (every conscious yellow lizard in the room) tells
        // each other of the slugcat any of them sees, flashing as they do
        //

        void yellow_update(sys::game_state& state, dcon::creature_id l, room_view const& room) {
            auto& w = state.world;
            auto& a = w.lizard_data_get_ai(creature::lizard_data(w, l));
            auto rng = rw::random(state.seed, state.tick, uint32_t(l.value));
            if (a.communicating > 0)
                --a.communicating;
            float const num = mathf::inverse_lerp(0.f, 14.f, float(a.communicating));
            if (a.comm_flicker < num)
                a.comm_flicker = mathf::min(num, a.comm_flicker + 0.25f);
            else
                a.comm_flicker = mathf::max(num, a.comm_flicker - 0.025f);
            rooms::for_each_creature(state, room.id, ::creature::type::lizard, [&](dcon::creature_id o) {
                if (o == l || breed_of(state, o) != breed_of(state, l) || !consious(state, o))
                    return;
                // PackMemberIsSeeingCreature: what a pack member hunts and
                // sees and this one doesn't
                auto& oa = w.lizard_data_get_ai(creature::lizard_data(w, o));
                auto const* orep = prey_rep(state, o);
                if (!orep || !orep->visual || !creature::here(state, orep->who, room.id))
                    return;
                auto const p = orep->who;
                if (auto const* mine = rep_of(state, l, p); mine && mine->visual)
                    return;
                // RecieveInfoOnCritter
                see(state, l, p);
                float const intensity = tracker::dynamic_relationship(state, l, p).intensity;
                a.communicating = std::max(a.communicating, rng.range(3, int32_t(mathf::lerp(3.f, 50.f, intensity))));
                oa.communicating = std::max(oa.communicating, rng.range(3, int32_t(mathf::lerp(3.f, 50.f, intensity))));
            });
        }

        //
        // LizardAI
        //

        // LizardAI.IdleSpotScore (AItile.visibility, which divides the base
        // score, taken as constant)
        float idle_spot_score(sys::game_state& state, dcon::creature_id l, room_view const& room, ivec2 t) {
            auto const& p = params_of(state, l);
            if (!room.inside(t.x, t.y))
                return 50.f;
            if (!pathing::accessible(room, t, p.movement) || !reachable_and_getbackable(state, l, room, t) || t == state.world.lizard_data_get_forbidden_idle_pos(creature::lizard_data(state.world, l)))
                return float_max;
            float num = 10.f;
            bool const narrow = pathing::narrow_space(room, t);
            bool const water = room.get_tile(t).water != 0;
            if (narrow)
                num += 10.f;
            if (water)
                num += 100.f;
            auto const acc = pathing::tile_access(room, t);
            switch (breed_of(state, l)) {
            case breed::green:
                if (acc == access::floor)
                    num *= 0.5f;
                if (narrow)
                    num += 20.f;
                break;
            case breed::blue:
                num += mathf::clamp(10.f - float(pathing::floor_altitude(room, t)), 0.f, 10.f);
                num += mathf::clamp(3.f - float(pathing::ai_terrain_proximity(state, room.id, t)), 0.f, 3.f);
                if (acc == access::climb || acc == access::wall)
                    num *= 0.5f;
                break;
            case breed::pink:
                num += float(pathing::floor_altitude(room, t)) * 2.f;
                if (narrow)
                    num -= 15.f;
                break;
            case breed::black:
                if (narrow)
                    num -= 20.f;
                break;
            default:
                if (p.swimmer) {
                    if (!water)
                        num += 20.f;
                    num += mathf::max(0.f, float_dist(t, here(state, l)) - 30.f) * 1.5f;
                    num += float(std::abs(t.y - room.default_water_level)) * 10.f;
                    num += float(pathing::ai_terrain_proximity(state, room.id, t)) * 10.f;
                }
                break;
            }
            return num;
        }

        // LizardAI.ComfortableIdlePosition
        bool comfortable_idle_position(sys::game_state const& state, dcon::creature_id l, room_view const& room) {
            vec2 const p0 = chunk_pos(state, l, 0), p1 = chunk_pos(state, l, 1), p2 = chunk_pos(state, l, 2);
            if (room.get_tile(p0).water != 0)
                return params_of(state, l).swimmer;
            ivec2 const t0 = room_view::get_tile_position(p0), t2 = room_view::get_tile_position(p2);
            return room.get_tile(p0 + custom::dir_vec(p1, p0) * 30.f).t != world::terrain::solid
                && (t0.y == t2.y || (breed_of(state, l) == breed::blue && t0.x == t2.x));
        }

        // LizardAI.FallRisk
        bool fall_risk(sys::game_state const& state, dcon::creature_id l, room_view const& room, ivec2 tile) {
            return fall_risk_tile(room, tile).y < here(state, l).y - 5;
        }

        // LizardAI.AggressiveBehavior towards the slugcat
        void aggressive_behavior(sys::game_state& state, dcon::creature_id l, room_view const& room, creature::tracked const& target, float tongue_chance) {
            auto& w = state.world;
            auto& a = w.lizard_data_get_ai(creature::lizard_data(w, l));
            auto& z = w.lizard_data_get_core(creature::lizard_data(w, l));
            auto rng = rw::random(state.seed, state.tick, uint32_t(l.value));
            auto const& par = params_of(state, l);
            auto const p = target.who;
            a.casual_aggression = dcon::creature_id{};
            state.world.creature_get_destination(l) = tracker::best_guess(target);
            vec2 const head = chunk_pos(state, l, 0);
            bool const seen = target.visual && creature::here(state, p, room.id);
            vec2 const prey = seen ? creature::main_pos(state, p) : target.last_seen;
            if (z.anim == animation::prepare_to_lounge)
                set_jaw_open(state, l, mathf::lerp(jaw_open(state, l), 0.2f, 0.2f));
            else if (z.anim == animation::lounge)
                set_jaw_open(state, l, jaw_open(state, l) + 0.2f);
            else if (z.anim == animation::shoot_tongue)
                set_jaw_open(state, l, tongue_out(state, l) ? jaw_open(state, l) + 0.1f : mathf::lerp(jaw_open(state, l), rng.value(), 0.2f));
            else if (seen)
                set_jaw_open(state, l, mathf::inverse_lerp(400.f, 90.f + par.bite_in_front, rw::distance(head, prey)));
            else
                set_jaw_open(state, l, mathf::lerp(jaw_open(state, l), mathf::inverse_lerp(0.6f, 0.9f, a.hunger * a.hunger) * 0.7f, 0.2f));
            breed const b = breed_of(state, l);
            if (b == breed::red && seen)
                set_jaw_open(state, l, mathf::clamp(jaw_open(state, l) + 0.1f, 0.f, 1.f));
            if (b == breed::caramel && seen)
                set_jaw_open(state, l, mathf::clamp(jaw_open(state, l) + 0.01f, 0.f, 0.5f));
            a.focus = p;
            if (!seen)
                return;
            ivec2 const at = here(state, l);
            ivec2 const pt = room_view::get_tile_position(prey);
            if (z.lounge_delay < 1 && rng.value() < par.lounge_tendensy && !unpleasant_fall_risk(state, l, room, at) && !fall_risk(state, l, room, at)
            && !unpleasant_fall_risk(state, l, room, pt) && custom::dist_less(head, prey, par.lounge_distance) && !pathing::narrow_space(room, pt)
            && !pathing::narrow_space(room, at) && (b != breed::green || room.get_tile(head + vec2{0.f, -20.f}).solid()) && legs_gripping(state, l) > 2) {
                enter_animation(state, l, animation::prepare_to_lounge, false);
            } else if (par.tongue && tongue_ready(state, l) && !grasped(state, l) && custom::dist_less(head, prey, par.tongue_attack_range)
            && !custom::dist_less(head, prey, mathf::lerp(par.attempt_bite_radius, par.tongue_attack_range, 0.2f))
            && (physics::object_submersion(state, room, body(state, l)) < 0.5f || physics::object_submersion(state, room, creature::body_of(state, p)) < 0.5f)
            && rng.value() < par.tongue_chance * tongue_chance) {
                enter_animation(state, l, animation::shoot_tongue, false);
            }
            if (target.age > 120 && jaw_ready_for_bite(state, l)) {
                ++a.attempt_bite_frames;
                uint32_t const n = creature::chunk_count_of(state, p);
                for (uint32_t i = 0; i < n; ++i)
                    if (custom::dist_less(w.body_chunk_get_pos(creature::chunk_of(state, p, i)), head, par.attempt_bite_radius)) {
                        attempt_bite(state, l, p);
                        return;
                    }
                return;
            }
            if (a.attempt_bite_frames > 5)
                jaws_snap_shut(state, l, prey);
            a.attempt_bite_frames = 0;
        }

        // LizardAI.TravelUtility
        float travel_utility(sys::game_state const& state, dcon::creature_id l, room_view const& room) {
            if (!migrate::want_to(state, l))
                return 0.f;
            float const num = migrate::attraction(state, l, room.id);
            float const num2 = migrate::attraction(state, l, migrate::destination(state, l));
            if (num > num2)
                return 0.f;
            float const num3 = mathf::lerp(0.99f, 0.1f, mathf::pow(num, 0.25f));
            return num == num2 ? num3 * 0.5f : num3;
        }

        // LizardAI.DetermineBehavior
        behavior determine_behavior(sys::game_state& state, dcon::creature_id l, room_view const& room) {
            auto& a = state.world.lizard_data_get_ai(creature::lizard_data(state.world, l));
            auto const& ts = tracker::of(state, l);
            behavior b = behavior::idle;
            state.world.creature_get_current_utility(l) = a.u.best_utility;
            switch (a.u.best) {
            case module::threat: b = behavior::flee; break;
            case module::rain: b = behavior::escape_rain; break;
            case module::prey: b = behavior::hunt; break;
            case module::injury: b = behavior::injured; break;
            case module::aggression: b = behavior::fighting; break;
            case module::mission: b = behavior::acting_out_mission; break;
            case module::lurk: b = behavior::lurk; break;
            case module::noise: b = behavior::investigate_sound; break;
            case module::friend_tracker: b = behavior::follow_friend; break;
            default: break;
            }
            if (state.world.creature_get_current_utility(l) < 0.05f) {
                state.world.creature_get_current_utility(l) = 0.05f;
                b = behavior::idle;
            }
            if (auto const g = grasped(state, l); b != behavior::flee && g && tracker::dynamic_relationship(state, l, g).type == creature::rel::eats) {
                b = behavior::return_prey;
                state.world.creature_get_current_utility(l) = 1.f;
            }
            auto const* pr = tracker::prey(ts);
            if (b == behavior::hunt && pr && ts.frustration > 0.75f && breed_of(state, l) != breed::red
            && chance_of_finding(*pr) > 0.4f && custom::dist_less(chunk_pos(state, l, 0), room_view::middle_of_tile(tracker::best_guess(*pr)), 350.f))
                b = behavior::frustrated;
            float const travel = travel_utility(state, l, room);
            if (b != behavior::return_prey && b != behavior::injured && b != behavior::escape_rain && b != behavior::flee && b != behavior::acting_out_mission
            && state.world.creature_get_current_utility(l) < travel) {
                state.world.creature_get_current_utility(l) = travel;
                b = behavior::travelling;
            }
            if ((breed_of(state, l) == breed::red || breed_of(state, l) == breed::caramel) && a.spit.go_to_spit_pos_counter > 0)
                b = behavior::go_to_spit_pos;
            return b;
        }

        // UtilityComparer.Update over LizardAI's modules (the ones that can
        // have a utility here)
        void compare_utilities(sys::game_state& state, dcon::creature_id l, room_view const& room) {
            auto& w = state.world;
            auto& a = w.lizard_data_get_ai(creature::lizard_data(w, l));
            auto const& ts = tracker::of(state, l);
            breed const b = breed_of(state, l);
            // threat: FloatTweenUpAndDown(lerp 0.5 up, tick 1 / ((lastDistressLength + 20) * 3) down), weight 1
            float const threat = tracker::threat_utility(ts);
            float const down = 1.f / ((float(a.last_distress_length) + 20.f) * 3.f);
            a.u.threat = threat > a.u.threat ? mathf::lerp(a.u.threat, threat, 0.5f) : mathf::max(a.u.threat - down, threat);
            // prey: lerp 0.15, weight 0.6
            a.u.prey = mathf::lerp(a.u.prey, tracker::prey_utility(state, l, room, here(state, l), tracker_params(b)) * 0.6f, 0.15f);
            float const injury = injury_utility(state, l) * (b == breed::red ? 0.4f : 0.9f);
            // aggression: weight 0.25 * health in a story session
            float const aggression = tracker::aggression_utility(ts) * 0.25f * w.creature_get_health(l);
            // MissionTracker.LeaveRoom: 0.4 while it lasts, weight 0.8
            float const mission = a.leave_room > 0 ? 0.4f * 0.8f : 0.f;
            float const noise = noise_utility(state, l) * 0.2f;
            float const lurk = lurker(b) ? lurk_utility(state, l, room) * mathf::lerp(0.4f, 0.3f, w.creature_get_payload(l).lizard.energy) : 0.f;
            // rain: weight 0.9 (FriendTracker.CareAboutRain: no friend to stay with)
            a.u.rain = dens::rain_utility(state, l, room.id) * 0.9f;
            float const values[] = {a.u.threat, a.u.prey, a.u.rain, injury, aggression, mission, noise, 0.f, lurk};
            float best = std::numeric_limits<float>::lowest();
            a.u.best = module::none;
            for (uint32_t i = 0; i < 9; ++i) {
                if (i == uint32_t(module::lurk) && !lurker(b))
                    continue;
                if (values[i] > best) {
                    best = values[i];
                    a.u.best = module(i);
                }
            }
            a.u.best_utility = best;
        }

        // LizardAI.SendCommunication / RecieveCommunication between two
        // lizards of the room: aggression makes the other angrier (or makes
        // it submit, and leave the room), submission calms the sender
        enum class communication : uint8_t { aggression, submission };
        void receive_communication(sys::game_state& state, dcon::creature_id l, room_view const& room, dcon::creature_id other, communication signal);
        void send_communication(sys::game_state& state, dcon::creature_id l, room_view const& room, dcon::creature_id other, communication signal) {
            auto& w = state.world;
            if (!w.creature_is_valid(other) || w.creature_get_dead(other) || rooms::room_of(state, other) != room.id)
                return;
            auto& a = w.lizard_data_get_ai(creature::lizard_data(w, l));
            auto rng = rw::random(state.seed, state.tick, rw::ids(uint32_t(l.value), uint32_t(other.value)));
            a.focus = other;
            auto& g = w.lizard_data_get_graphics(creature::lizard_data(w, l));
            // (graphicsModule != null: rolled either way)
            float const r = rng.value();
            if (signal == communication::aggression) {
                if (state.viewed(room.id))
                    g.show_dominance += r;
                make_sound(state, l, emotion::dominance);
            } else {
                if (state.viewed(room.id))
                    g.show_dominance -= r;
                make_sound(state, l, emotion::submission);
            }
            receive_communication(state, other, room, l, signal);
        }
        void receive_communication(sys::game_state& state, dcon::creature_id l, room_view const& room, dcon::creature_id other, communication signal) {
            auto& w = state.world;
            if (!sees_chunk(state, l, room, chunk(state, other, 0)))
                return;
            auto& a = w.lizard_data_get_ai(creature::lizard_data(w, l));
            auto rng = rw::random(state.seed, state.tick, rw::ids(uint32_t(l.value), uint32_t(other.value)));
            auto const ot = other;
            a.focus = ot;
            if (signal == communication::aggression) {
                auto const& oa = w.lizard_data_get_ai(creature::lizard_data(w, other));
                if (oa.submitted_to == l)
                    return;
                if (a.submitted_to != ot)
                    tracker::increment_anger(state, l, ot, 0.1f);
                if (rng.value() < 0.041666668f) {
                    send_communication(state, l, room, other, communication::submission);
                    tracker::set_anger(state, l, ot, 0.f, 0.f);
                    a.submitted_to = ot;
                    // MissionTracker.LeaveRoom(0.4, 1000)
                    a.leave_room = 1000;
                }
            } else {
                tracker::set_anger(state, l, ot, 0.f, -1.f);
            }
        }

        // LizardAI.DoIWantToBiteThisCreature
        bool want_to_bite(sys::game_state& state, dcon::creature_id l, creature::tracked const& r) {
            auto rng = rw::random(state.seed, state.tick, rw::ids(uint32_t(l.value), uint32_t(r.who.value)));
            if (rng.value() < 0.1f && rng.value() > state.world.creature_get_health(l))
                return true;
            return r.rel.go_for_kill() || (rng.value() < 0.5f && r.rel.type == creature::rel::agressive_rival);
        }
    }

    dcon::creature_id tracked(sys::game_state const& state, dcon::creature_id l) noexcept {
        auto const* r = prey_rep(state, l);
        return r ? r->who : dcon::creature_id{};
    }

    dcon::creature_id focus(sys::game_state const& state, dcon::creature_id l) noexcept {
        auto const t = state.world.lizard_data_get_ai(creature::lizard_data(state.world, l)).focus;
        return t && tracker::find(state, l, t) ? t : dcon::creature_id{};
    }

    pathing::field_id field(sys::game_state& state, dcon::creature_id l, room_view const& room) {
        return pathing::field_to(state, room.id, state.world.creature_get_destination(l), params_of(state, l).movement);
    }

    bool reachable_and_getbackable(sys::game_state& state, dcon::creature_id l, room_view const& room, ivec2 t) {
        if (!room.inside(t.x, t.y))
            return false;
        auto const f = field(state, l, room);
        if (pathing::distance(state, f, t) == pathing::unreachable)
            return false;
        ivec2 const at = here(state, l);
        if (pathing::distance(state, f, at) != pathing::unreachable)
            return true;
        // the lizard off its walkable tiles (in mid air): one beside it then
        for (auto const d : custom::eight_directions)
            if (room.inside(at.x + d.x, at.y + d.y) && pathing::distance(state, f, at + d) != pathing::unreachable)
                return true;
        // a destination it can't get to from where it is: whether it could
        // get back here from the tile, by the field to its own tile
        auto const home = pathing::field_to(state, room.id, at, params_of(state, l).movement);
        return pathing::distance(state, home, t) != pathing::unreachable;
    }

    // AItile.fallRiskTile: the lowest tile something falling from `t` (or
    // from the columns beside it) lands on, (-1, -1) over a pit
    ivec2 fall_risk_tile(room_view const& room, ivec2 t) {
        if (room.get_tile(t).solid())
            return t;
        auto const landing = [&](int32_t x, int32_t y) -> std::optional<ivec2> {
            if (x < 0 || x >= room.width || room.get_tile(ivec2{x, y}).solid())
                return std::nullopt;
            for (; y > 0 && !room.get_tile(ivec2{x, y - 1}).solid(); --y) {
            }
            if (y == 0 && !room.get_tile(ivec2{x, -1}).solid())
                return ivec2{-1, -1};
            return ivec2{x, y};
        };
        auto const own = landing(t.x, t.y);
        ivec2 best = own ? *own : t;
        for (int32_t i = -1; i < 2; i += 2)
            if (auto const l = landing(t.x + i, t.y); l && l->y < best.y)
                best = *l;
        return best;
    }

    bool unpleasant_fall_risk(sys::game_state& state, dcon::creature_id l, room_view const& room, ivec2 tile) {
        ivec2 const f = fall_risk_tile(room, tile);
        return room.get_tile(f).water != 0 || f.y < 0 || f.y < tile.y - 20 || (f.y < tile.y - 10 && !reachable_and_getbackable(state, l, room, f));
    }

    bool want_to_hold_with_tongue(sys::game_state& state, dcon::creature_id l, room_view const& room, dcon::creature_id p, uint32_t k) {
        if (!creature::here(state, p, room.id))
            return false;
        auto const c = creature::chunk_of(state, p, k);
        return physics::submersion(state, room, c) <= 0.5f
            && (!unpleasant_fall_risk(state, l, room, here(state, l)) || !unpleasant_fall_risk(state, l, room, room_view::get_tile_position(state.world.body_chunk_get_pos(c))))
            && tracker::dynamic_relationship(state, l, p).go_for_kill();
    }

    relationship relationship_to(sys::game_state const& state, dcon::creature_id l, dcon::creature_id t) {
        // UpdateDynamicRelationship: the slugcat by how much it is liked
        // (spears and vulture masks the slugcat carries: masks don't exist
        // here), the rest by the static relationship (no friends)
        if (creature::is_slugcat(state.world, t)) {
            float const num = like_of_player(state, l);
            if (num < 0.5f)
                return relationship{creature::rel::eats, mathf::pow(mathf::inverse_lerp(0.5f, -1.f, num), params_of(state, l).aggression_curve_exponent)};
            return relationship{creature::rel::ignores, 0.f};
        }
        return creature::static_relationship(state, l, t);
    }

    void see(sys::game_state& state, dcon::creature_id l, dcon::creature_id t) {
        auto const room = world::get_room_view(state, room_of(state, l));
        ai_ctx x{state, l, room};
        tracker::see(state, l, room, t, make_hooks(x));
    }

    void bit_creature(sys::game_state& state, dcon::creature_id l, room_view const& room) {
        if (!unpleasant_fall_risk(state, l, room, here(state, l)))
            enter_animation(state, l, animation::shake_prey, true);
    }

    bool at_spit_pos(sys::game_state const& state, dcon::creature_id l) noexcept {
        return custom::dist_less(chunk_pos(state, l, 0), room_view::middle_of_tile(state.world.lizard_data_get_ai(creature::lizard_data(state.world, l)).spit.spit_from_pos), 50.f);
    }

    bool spit_aim_pos(sys::game_state& state, dcon::creature_id l, vec2& out) {
        auto& w = state.world;
        auto& a = w.lizard_data_get_ai(creature::lizard_data(w, l));
        auto& s = a.spit;
        auto rng = rw::random(state.seed, state.tick, uint32_t(l.value));
        auto const* pr = prey_rep(state, l);
        if (!pr || !pr->visual || !creature::live(state, pr->who))
            return false;
        auto const p = pr->who;
        uint32_t const k = uint32_t(rng.range(0, int32_t(creature::chunk_count_of(state, p))));
        auto const c = creature::chunk_of(state, p, k);
        vec2 v = w.body_chunk_get_pos(c);
        vec2 const lead = v - w.body_chunk_get_last_pos(c);
        s.aim = p;
        s.aim_chunk = uint8_t(k);
        s.aim_up = s.general_aim_up + mathf::lerp(-0.15f, 0.15f, rng.value());
        vec2 const head = chunk_pos(state, l, 0);
        v.y += mathf::pow(rw::distance(v, head), mathf::lerp(1.2f, 1.55f, s.aim_up)) * 0.015f * s.aim_up;
        v += lead * rw::distance(v, head) * 0.01f * rng.value();
        out = v;
        return true;
    }

    void spit_set_aim(sys::game_state& state, dcon::creature_id l, float am) {
        state.world.lizard_data_get_ai(creature::lizard_data(state.world, l)).spit.general_aim_up = am;
    }

    void spit_aim_a_bit_up(sys::game_state& state, dcon::creature_id l, float am) {
        auto& s = state.world.lizard_data_get_ai(creature::lizard_data(state.world, l)).spit;
        if (am < s.general_aim_up)
            s.general_aim_up = mathf::clamp(s.general_aim_up + 0.3f * mathf::inverse_lerp(1.f, 0.5f, s.general_aim_up) * rw::random(state.seed, state.tick, uint32_t(l.value)).value(), am, 1.f);
    }

    void spit_aim_a_bit_down(sys::game_state& state, dcon::creature_id l, float am) {
        auto& s = state.world.lizard_data_get_ai(creature::lizard_data(state.world, l)).spit;
        if (am > s.general_aim_up)
            s.general_aim_up = mathf::clamp(s.general_aim_up - 0.3f * mathf::inverse_lerp(0.f, 0.5f, s.general_aim_up) * rw::random(state.seed, state.tick, uint32_t(l.value)).value(), 0.f, am);
    }

    void collided(sys::game_state& state, dcon::creature_id l, dcon::creature_id t) {
        // Lizard.Collide: seen, and a rival it hasn't submitted to angers it
        see(state, l, t);
        auto& a = state.world.lizard_data_get_ai(creature::lizard_data(state.world, l));
        auto const r = tracker::dynamic_relationship(state, l, t);
        if (r.type == creature::rel::agressive_rival && a.submitted_to != t)
            tracker::increment_anger(state, l, t, 0.05f * r.intensity);
    }

    void reset(sys::game_state& state, dcon::creature_id l) {
        auto& w = state.world;
        auto rng = rw::random(state.seed, state.tick, uint32_t(l.value));
        ai_state a;
        ivec2 const at = here(state, l);
        state.world.creature_get_destination(l) = at;
        a.lurk.lurk_position = at;
        a.lurk.look_position = at;
        a.spit.random_cycle = rng.range(100, 800);
        a.spit.spit_from_pos = at;
        a.spit.temp_spit_from_pos = at;
        w.lizard_data_set_ai(creature::lizard_data(w, l), a);
        tracker::clear(state, l);
    }

    void update(sys::game_state& state, dcon::creature_id l, room_view const& room) {
        auto& w = state.world;
        auto& a = w.lizard_data_get_ai(creature::lizard_data(w, l));
        auto& z = w.lizard_data_get_core(creature::lizard_data(w, l));
        auto rng = rw::random(state.seed, state.tick, uint32_t(l.value));
        auto const& par = params_of(state, l);
        breed const b = breed_of(state, l);
        if (a.panic > 0)
            --a.panic;
        ++a.time_in_room;
        if (a.leave_room > 0)
            --a.leave_room;
        // AbstractBehavior(1): a chance to plan a trip to another room, more
        // the less it likes this one (a Red lizard: 1 in 50)
        {
            float like = migrate::size_dependent_attraction(state, l, room.id);
            rooms::for_each_creature(state, room.id, ::creature::type::lizard, [&](dcon::creature_id o) {
                if (o != l)
                    like *= 0.9f;
            });
            if (rng.value() < 1.f / (b == breed::red ? 50.f : custom::lerp_map(like, 0.35f, 1.f, 10.f, 1000.f)))
                migrate::abstract_behavior(state, l, room);
        }
        // SocialMemory.EvenOutAllTemps
        a.temp_like = a.temp_like > 0.f ? mathf::max(0.f, a.temp_like - 0.0005f) : mathf::min(0.f, a.temp_like + 0.0005f);
        // the utilities as the comparer smoothed them last tick
        a.fear = a.u.threat;
        a.hunger = a.u.prey / 0.6f;
        // UtilityComparer.GetSmoothedNonWeightedUtility(rainTracker)
        a.rain_fear = a.u.rain / 0.9f;
        a.excitement = mathf::lerp(a.excitement, mathf::max(a.hunger, mathf::max(a.fear, mathf::pow(a.rain_fear, 3.f))), 0.1f);
        if (a.fear > 0.8f)
            a.last_distress_length = custom::int_clamp(a.last_distress_length + 1, 0, 500);
        else if (a.fear < 0.2f)
            a.last_distress_length = 0;
        state.world.creature_set_behavior(l, uint8_t(determine_behavior(state, l, room)));
        set_jaw_open(state, l, jaw_open(state, l) - 0.01f);
        auto const& ts = tracker::of(state, l);
        auto const* prey = tracker::prey(ts);
        if (lurker(b) && behavior(state.world.creature_get_behavior(l)) == behavior::hunt && prey && a.run_speed < 0.1f && float_dist(here(state, l), tracker::best_guess(*prey)) > 17.f)
            state.world.creature_set_behavior(l, uint8_t(behavior::lurk));
        ivec2 const at = here(state, l);
        ai_ctx hx{state, l, room};
        tracker::hooks const h = make_hooks(hx);
        // DenFinder: the pipe home (only asked for by the behaviours heading there)
        auto const den_tile = [&] { return dens::home_tile(state, l, room); };
        switch (behavior(state.world.creature_get_behavior(l))) {
        case behavior::flee: {
            // ThreatTracker.FleeTo (within the room)
            state.world.creature_get_destination(l) = tracker::flee_to(state, l, room, at, 5, 20, tracker_params(b), h);
            if (tracker::threat_of_tile(ts, at, 10) > 1.f) {
                a.panic = a.panic < 40 ? 40 : a.panic + 2;
                if (rng.value() < 0.05f)
                    if (auto const* th = tracker::threat(ts); th && float_dist(tracker::best_guess(*th), at) < 15.f)
                        enter_animation(state, l, animation::threat_spotted, false);
            }
            // CombinedFear: the fear and the rain's
            a.run_speed = mathf::lerp(a.run_speed, mathf::pow(mathf::max(a.fear, mathf::pow(a.rain_fear, 3.f)), 0.1f), 0.5f);
            a.focus = ts.threat;
            break;
        }
        case behavior::travelling:
            a.run_speed = mathf::lerp(a.run_speed, 0.5f + 0.5f * travel_utility(state, l, room), 0.01f);
            if (auto const t = migrate::travel_tile(state, l, room))
                state.world.creature_get_destination(l) = *t;
            break;
        case behavior::acting_out_mission:
            // MissionTracker.LeaveRoom: the way out of the room
            if (!migrate::want_to(state, l))
                migrate::move_to_more_attractive_neighbor(state, l, room);
            if (auto const t = migrate::travel_tile(state, l, room))
                state.world.creature_get_destination(l) = *t;
            a.run_speed = mathf::lerp(a.run_speed, 0.8f, 0.1f);
            break;
        case behavior::fighting:
            a.focus = ts.aggression;
            if (auto const* f = tracker::aggression(ts)) {
                vec2 const head = chunk_pos(state, l, 0);
                auto const other = creature::live(state, f->who) ? f->who : dcon::creature_id{};
                bool const lizard = other && creature::type_of(w, other) == ::creature::type::lizard;
                vec2 const fp = creature::main_pos(state, f->who);
                if (f->visual && creature::here(state, f->who, room.id) && custom::dist_less(head, fp, 160.f)
                && (!lizard || rw::distance(head, fp) < rw::distance(head, w.body_chunk_get_pos(chunk(state, other, 2))))) {
                    if (rng.value() < 0.05f)
                        enter_animation(state, l, animation::fighting_stance, false);
                    a.run_speed = mathf::lerp(a.run_speed, 0.2f, 0.15f);
                } else {
                    a.run_speed = mathf::lerp(a.run_speed, 0.8f, 0.15f);
                    if (rng.value() < 0.0055555557f)
                        enter_animation(state, l, animation::fighting_stance, false);
                }
                if (lizard && rng.value() < 0.05f && rng.value() < f->rel.intensity)
                    send_communication(state, l, room, other, communication::aggression);
                if (auto const* f2 = tracker::aggression(tracker::of(state, l)))
                    aggressive_behavior(state, l, room, *f2, 0.025f);
            }
            break;
        case behavior::hunt:
            if (prey)
                aggressive_behavior(state, l, room, *prey, 1.f);
            a.run_speed = mathf::lerp(a.run_speed, mathf::max(a.hunger, 0.3f), 0.1f);
            if (rng.value() < 0.0125f && b != breed::black)
                make_sound(state, l, emotion::blood_lust);
            break;
        case behavior::idle: {
            bool restless = true;
            int32_t const before = a.idle_counter--;
            if (custom::manhattan_distance(at, state.world.creature_get_destination(l)) < 5) {
                a.idle_counter -= par.idle_counter_subtract_when_close_to_idle_pos;
                --a.idle_restlessness;
                restless = a.idle_counter < 1 || a.idle_restlessness > 0;
                vec2 const v0 = w.body_chunk_get_vel(chunk(state, l, 0)), v1 = w.body_chunk_get_vel(chunk(state, l, 1)), v2 = w.body_chunk_get_vel(chunk(state, l, 2));
                if (!restless && !comfortable_idle_position(state, l, room)) {
                    ++a.unable_to_find_comfortable_position;
                    restless = true;
                } else if (z.swim < 0.5f && (rw::magnitude(v0) > 2.f || rw::magnitude(v1) > 2.f || rw::magnitude(v2) > 2.f)) {
                    ++a.unable_to_find_comfortable_position;
                } else {
                    a.unable_to_find_comfortable_position = 0;
                }
                if (a.unable_to_find_comfortable_position > 40) {
                    a.idle_counter = 0;
                    a.unable_to_find_comfortable_position = 0;
                } else {
                    bool settled = true;
                    for (uint32_t i = 0; i < 3; ++i)
                        settled = settled && physics::is_tile_solid(state, room, chunk(state, l, i), 0, -1);
                    if (settled)
                        for (uint32_t i = 0; i < 3; ++i)
                            w.body_chunk_get_vel(chunk(state, l, i)).y -= 0.1f;
                }
                if (a.idle_restlessness < 40 && rng.value() < 0.0016666667f)
                    a.idle_restlessness = rng.range(1, 40);
            }
            if (idle_spot_score(state, l, room, state.world.creature_get_destination(l)) == float_max)
                a.idle_counter = 0;
            if (a.idle_counter < 1) {
                if (before > 0)
                    state.world.lizard_data_get_forbidden_idle_pos(creature::lizard_data(state.world, l)) = state.world.creature_get_destination(l);
                ivec2 const t{rng.range(0, room.width), rng.range(0, room.height)};
                if (idle_spot_score(state, l, room, t) < idle_spot_score(state, l, room, state.world.creature_get_destination(l))) {
                    state.world.creature_get_destination(l) = t;
                    a.idle_spot_win_streak = 0;
                } else if (++a.idle_spot_win_streak > 50) {
                    a.idle_counter = rng.range(1000, 4000);
                    a.idle_spot_win_streak = 0;
                }
            }
            if (restless) {
                if (state.world.lizard_data_get_forbidden_idle_pos(creature::lizard_data(state.world, l)) != state.world.creature_get_destination(l))
                    a.run_speed = mathf::lerp(a.run_speed, 0.25f, 0.5f);
            } else {
                a.run_speed = mathf::lerp(a.run_speed, 0.f, 0.5f);
            }
            if (rng.value() < 0.00125f)
                make_sound(state, l, emotion::boredom);
            a.focus = dcon::creature_id{};
            break;
        }
        case behavior::return_prey:
            // (carried into the den, the prey is eaten: dens.hpp)
            if (auto const t = den_tile())
                state.world.creature_get_destination(l) = *t;
            a.run_speed = mathf::lerp(a.run_speed, mathf::pow(mathf::max(a.fear, 0.5f), 0.5f), 0.1f);
            break;
        case behavior::escape_rain:
            // LizardAI: the den, at full speed
            if (auto const t = den_tile())
                state.world.creature_get_destination(l) = *t;
            a.run_speed = mathf::lerp(a.run_speed, 1.f, 0.1f);
            break;
        case behavior::injured:
            if (auto const t = den_tile())
                state.world.creature_get_destination(l) = *t;
            a.run_speed = mathf::lerp(a.run_speed, 1.f, 0.01f);
            if (rng.value() < 0.0125f)
                make_sound(state, l, emotion::pain_idle, 1.f - w.creature_get_health(l));
            break;
        case behavior::frustrated:
            a.focus = prey ? prey->who : dcon::creature_id{};
            a.run_speed = mathf::lerp(a.run_speed, 1.f, 0.5f);
            if (rng.value() < 0.1f || custom::manhattan_distance(at, state.world.creature_get_destination(l)) < 10) {
                ivec2 const t{rng.range(0, room.width), rng.range(0, room.height)};
                if (reachable_and_getbackable(state, l, room, t)) {
                    state.world.creature_get_destination(l) = t;
                    a.idle_restlessness = rng.range(30, 100);
                }
            }
            if (rng.value() < 0.0125f)
                enter_animation(state, l, animation::prey_re_spotted, false);
            if (rng.value() < 0.0125f)
                z.body_wiggle_counter = custom::int_clamp(std::max(z.body_wiggle_counter, int32_t(rng.value() * 100.f)), 0, 100);
            if (rng.value() < 0.0125f && b != breed::black)
                make_sound(state, l, emotion::frustration);
            break;
        case behavior::lurk: {
            auto const& lk = a.lurk;
            if (custom::manhattan_distance(at, state.world.creature_get_destination(l)) > 5 && lurk_pos_score(state, l, room, at) * 1.2f < lurk_pos_score(state, l, room, lk.lurk_position)) {
                a.run_speed = mathf::lerp(a.run_speed, 0.5f, 0.5f);
                state.world.creature_get_destination(l) = lk.lurk_position;
            } else if (sees_point(state, l, room, room_view::middle_of_tile(lk.look_position))) {
                state.world.creature_get_destination(l) = lk.lurk_position;
                a.run_speed = mathf::lerp(a.run_speed, 0.f, 0.5f);
            } else {
                state.world.creature_get_destination(l) = lk.lurk_position + custom::four_directions[rng.range(0, 4)];
                a.run_speed = mathf::lerp(a.run_speed, 0.7f, 0.2f);
            }
            break;
        }
        case behavior::investigate_sound:
            state.world.creature_get_destination(l) = a.examine_pos;
            a.run_speed = mathf::lerp(a.run_speed, 0.6f, 0.2f);
            break;
        case behavior::go_to_spit_pos:
            state.world.creature_get_destination(l) = a.spit.spit_from_pos;
            a.run_speed = mathf::lerp(a.run_speed, 1.f, 0.2f);
            break;
        default:
            break;
        }
        if (behavior(state.world.creature_get_behavior(l)) != behavior::idle && b == breed::red)
            a.run_speed = mathf::pow(a.run_speed, 0.7f);
        a.run_speed = mathf::max(a.run_speed, stuck_utility(state, l));
        // casualAggressionTarget: biting at what is close by, whatever it is doing
        if (!grasped(state, l) && ts.count > 0) {
            auto const& r = ts.reps[rng.range(0, ts.count)];
            vec2 const head = chunk_pos(state, l, 0);
            auto const* cur = rep_of(state, l, a.casual_aggression);
            if ((want_to_bite(state, l, r) || (rng.value() > w.creature_get_health(l) && rng.value() < 0.05f)) && r.visual && creature::here(state, r.who, room.id)
            && (!cur || !creature::here(state, cur->who, room.id) || rw::distance(head, creature::main_pos(state, r.who)) < rw::distance(head, creature::main_pos(state, cur->who))))
                a.casual_aggression = r.who;
        }
        if (auto const* cur = rep_of(state, l, a.casual_aggression); cur && rng.value() < 0.5f && cur->visual && creature::here(state, cur->who, room.id)
        && custom::dist_less(creature::main_pos(state, cur->who), chunk_pos(state, l, 0), par.attempt_bite_radius))
            attempt_bite(state, l, cur->who);
        if ((b == breed::red || b == breed::caramel) && a.spit.spitting)
            enter_animation(state, l, animation::spit, false);
        // (the Blizzard's beam isn't ported)

        // ArtificialIntelligence.Update: the modules, in the order LizardAI
        // added them (the tracker's together: Tracker, SuperHearing,
        // PreyTracker, ThreatTracker, AgressionTracker, RelationshipTracker)
        tracker::update(state, l, room, at, tracker_params(b), h);
        if (b == breed::black)
            super_hearing(state, l, room, h);
        noise_update(state, l, room);
        compare_utilities(state, l, room);
        stuck_update(state, l);
        if (lurker(b))
            lurk_update(state, l, room);
        if (b == breed::yellow || b == breed::peach)
            yellow_update(state, l, room);
        if (b == breed::red || b == breed::caramel)
            spit_update(state, l, room);
    }
}
