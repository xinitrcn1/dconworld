#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#include "lizard_ai.hpp"
#include "lizard_graphics.hpp"
#include "items.hpp"
#include "physics.hpp"
#include "player.hpp"

// LizardAI: DetermineBehavior's utility comparison and the behaviours, over
// the modules it keeps: a Tracker of the slugcat (vision with the breed's
// angles and the Black lizard's SuperHearing), the PreyTracker, NoiseTracker,
// StuckTracker, LurkTracker, LizardSpitTracker and the Yellow pack's YellowAI.
// Nothing threatens a lizard here (no vultures, no masks), there is no rain
// or migration, and lizards don't fight each other, so the Flee, EscapeRain,
// Travelling, Fighting, ActingOutMission and FollowFriend behaviours don't
// come up. The den is a room exit.

namespace lizard::ai {
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    using pathing::access;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        constexpr float float_max = std::numeric_limits<float>::max();

        vec2 chunk_pos(sys::game_state const& state, dcon::lizard_id l, uint32_t i) {
            return state.world.body_chunk_get_pos(chunk(state, l, i));
        }

        float float_dist(ivec2 a, ivec2 b) {
            return rw::magnitude(rw::to_vec2(a - b));
        }

        bool lurker(breed b) {
            return b == breed::white || b == breed::salamander || b == breed::eel || b == breed::peach;
        }

        // the first room exit's entrance: where the den would be (no dens,
        // no creatures in shortcuts)
        std::optional<ivec2> den(sys::game_state const& state, dcon::room_id room) {
            std::optional<ivec2> e;
            state.world.room_for_each_shortcut_location(room, [&](dcon::shortcut_location_id sl) {
                auto const s = state.world.shortcut_location_get_shortcut(sl);
                if (!e && state.world.shortcut_get_type(s) == world::shortcut_type::room_exit)
                    e = state.world.shortcut_get_start(s);
            });
            return e;
        }

        // LizardAI.LikeOfPlayer: the community's like (0, nothing changes it
        // here; a Cyan isn't mean outside the arena) leaning on the temporary like
        float like_of_player(sys::game_state const& state, dcon::lizard_id l) {
            float const temp = state.world.lizard_get_ai(l).temp_like;
            return mathf::lerp(0.f, temp, std::fabs(temp));
        }

        // LizardAI.VisualScore (+ ArtificialIntelligence.VisualScore): within
        // the visual radius, under or above the water, the breed's vision angles
        float visual_score(sys::game_state const& state, dcon::lizard_id l, room_view const& room, vec2 look, float bonus) {
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
        bool sees_chunk(sys::game_state const& state, dcon::lizard_id l, room_view const& room, dcon::body_chunk_id c) {
            auto const& w = state.world;
            float const mbv = params_of(state, l).movement_based_vision;
            vec2 const pos = w.body_chunk_get_pos(c);
            float const bonus = mathf::clamp(mathf::lerp(-mbv, 1.f, mathf::inverse_lerp(1.f, 10.f, rw::distance(w.body_chunk_get_last_pos(c), pos))) * mbv, -1.f, 1.f);
            return visual_score(state, l, room, pos, bonus) > 0.f && room.visual_contact(chunk_pos(state, l, 0), pos);
        }
        bool sees_point(sys::game_state const& state, dcon::lizard_id l, room_view const& room, vec2 pos) {
            return visual_score(state, l, room, pos, 0.f) > 0.f && room.visual_contact(chunk_pos(state, l, 0), pos);
        }

        // LizardAI.CreatureSpotted
        void creature_spotted(sys::game_state& state, dcon::lizard_id l, bool first, dcon::player_id p) {
            lizard_graphics::creature_spotted(state, l, first, p);
            auto const rel = player_relationship(state, l, p);
            if (rel.go_for_kill() && rel.intensity > (first ? 0.2f : 0.45f) && grasped(state, l) != p)
                enter_animation(state, l, first ? animation::prey_spotted : animation::prey_re_spotted, false);
        }

        // Tracker.CreatureRepresentation.EstimatedChanceOfFinding (no ghosts)
        float chance_of_finding(tracked_player const& r) {
            if (r.visual_contact)
                return 1.f;
            float const num = float(r.ticks_since_seen) / 4.f;
            if (num < 45.f)
                return mathf::clamp(1.f / -(1.f + std::exp(-(num / 12.f - 5.f))) + 1.007f, 0.f, 1.f);
            return 1.f / (num - 7.f) * 30.f;
        }

        // Tracker.Update + SuperHearing.Update: the slugcats it sees (or, the
        // Black lizard, hears moving on the terrain close by)
        void tracker_update(sys::game_state& state, dcon::lizard_id l, room_view const& room) {
            auto& w = state.world;
            auto& a = w.lizard_get_ai(l);
            auto& r = a.rep;
            bool const black = breed_of(state, l) == breed::black;
            vec2 const head = chunk_pos(state, l, 0);
            dcon::player_id seen;
            float best = float_max;
            w.for_each_player([&](dcon::player_id p) {
                if (!player_here(state, p, room.id))
                    return;
                bool sees = false;
                for (uint32_t i = 0; i < 2; ++i) {
                    auto const c = player::body_chunk(state, p, i);
                    sees = sees || sees_chunk(state, l, room, c);
                    if (black && w.body_chunk_get_contact_point(c) != ivec2{0, 0}) {
                        float const loud = mathf::min((rw::distance(w.body_chunk_get_last_pos(c), w.body_chunk_get_pos(c)) - 3.f) * 4.f, 10.f) * 0.1f * 350.f;
                        sees = sees || custom::dist_less(w.body_chunk_get_pos(c), head, loud);
                    }
                }
                float const d = rw::distance(head, w.body_chunk_get_pos(player::body_chunk(state, p, 0)));
                if (sees && d < best) {
                    best = d;
                    seen = p;
                }
            });
            if (r.player && !player_of(state, r.player))
                r = tracked_player{};
            if (seen)
                see_player(state, l, seen);
            else
                r.visual_contact = false;
            auto const p = player_of(state, r.player);
            if (!p)
                return;
            if (!r.visual_contact)
                ++r.ticks_since_seen;
            ++r.age;
            if (!player_here(state, p, room.id))
                r.visual_contact = false;
        }

        // the slugcat's tile it hunts (BestGuessForPosition)
        ivec2 best_guess(tracked_player const& r) {
            return room_view::get_tile_position(r.last_seen_pos);
        }

        // PreyTracker.TrackedPrey.PathFinderCanGetToPrey
        bool can_get_to_prey(sys::game_state& state, dcon::lizard_id l, room_view const& room, ivec2 t) {
            for (auto const d : custom::eight_directions_and_zero)
                if (reachable_and_getbackable(state, l, room, t + d))
                    return true;
            for (auto const d : custom::four_directions)
                if (reachable_and_getbackable(state, l, room, t + d * 2))
                    return true;
            return false;
        }

        // PreyTracker.TrackedPrey.Reachable
        float prey_reachable(sys::game_state const& state, dcon::lizard_id l, room_view const& room) {
            auto const& r = state.world.lizard_get_ai(l).rep;
            if (room.get_tile(best_guess(r)).solid())
                return 0.f;
            int32_t const give_up = breed_of(state, l) == breed::red ? 1800 : 400;
            return mathf::inverse_lerp(float(give_up), 0.f, float(r.unreachable_counter)) * mathf::inverse_lerp(200.f, 100.f, float(r.at_position_but_cant_see_counter));
        }

        // PreyTracker.Update with the one prey it can have (TrackedPrey.Update)
        void prey_tracker_update(sys::game_state& state, dcon::lizard_id l, room_view const& room) {
            auto& w = state.world;
            auto& a = w.lizard_get_ai(l);
            auto& r = a.rep;
            auto const p = player_of(state, r.player);
            if (!p || !player_relationship(state, l, p).eats) {
                a.frustration = mathf::clamp(a.frustration - 0.0125f * 4.f, 0.f, 1.f);
                return;
            }
            ivec2 const g = best_guess(r);
            bool ok = reachable_and_getbackable(state, l, room, g);
            for (int i = 0; i < 4 && !ok; ++i)
                ok = reachable_and_getbackable(state, l, room, g + custom::four_directions[i]);
            r.unreachable_counter = ok ? 0 : r.unreachable_counter + 1;
            ivec2 const at = here(state, l);
            if (r.last_best_guess == g && float_dist(at, g) < 5.f && float_dist(a.destination, g) < 5.f
            && room.visual_contact(room_view::middle_of_tile(at), room_view::middle_of_tile(g)))
                r.at_position_but_cant_see_counter += 5;
            else
                --r.at_position_but_cant_see_counter;
            r.at_position_but_cant_see_counter = custom::int_clamp(r.at_position_but_cant_see_counter, 0, 200);
            r.last_best_guess = g;
            if (!can_get_to_prey(state, l, room, g))
                a.frustration = mathf::clamp(a.frustration + 0.0125f, 0.f, 1.f);
            else
                a.frustration = mathf::clamp(a.frustration - 0.0125f * 4.f, 0.f, 1.f);
        }

        // PreyTracker.Utility (PreyTracker(5, 2, 3, 70, 0.5))
        float prey_utility(sys::game_state const& state, dcon::lizard_id l, room_view const& room) {
            auto const& a = state.world.lizard_get_ai(l);
            auto const p = player_of(state, a.rep.player);
            if (!p)
                return 0.f;
            auto const rel = player_relationship(state, l, p);
            if (!rel.eats)
                return 0.f;
            float const dist = mathf::max(0.01f, float_dist(here(state, l), best_guess(a.rep)));
            float const num = mathf::lerp(1.f - mathf::inverse_lerp(3.f, 70.f, dist), mathf::lerp(3.f, 70.f, 0.25f) / dist, 0.5f);
            return mathf::min(num, chance_of_finding(a.rep)) * mathf::pow(rel.intensity, 0.75f) * prey_reachable(state, l, room);
        }

        // LizardAI.LizardInjuryTracker.Utility
        float injury_utility(sys::game_state const& state, dcon::lizard_id l) {
            return custom::s_curve(mathf::inverse_lerp(0.2f, 0.9f, 1.f - mathf::clamp(state.world.lizard_get_health(l), 0.f, 1.f)), 0.01f);
        }

        // StuckTracker.Update (40 positions, from the 30th, within 5 tiles, more than 10)
        void stuck_update(sys::game_state& state, dcon::lizard_id l) {
            auto& a = state.world.lizard_get_ai(l);
            auto& s = a.stuck;
            ivec2 const at = here(state, l);
            int32_t close = 0;
            if (!s.satisfied_with_this_position && custom::manhattan_distance(at, a.destination) > 3) {
                s.head = (s.head + stuck_tracker::tracked - 1) % stuck_tracker::tracked;
                s.past[s.head] = at;
                s.count = std::min(s.count + 1, stuck_tracker::tracked);
                for (int32_t j = 30; j < s.count; ++j)
                    if (float_dist(at, s.past[(s.head + j) % stuck_tracker::tracked]) < 5.f)
                        ++close;
            }
            s.counter = custom::int_clamp(close > 10 ? s.counter + 1 : s.counter - 2, 0, 200);
        }
        float stuck_utility(sys::game_state const& state, dcon::lizard_id l) {
            return mathf::inverse_lerp(100.f, 200.f, float(state.world.lizard_get_ai(l).stuck.counter));
        }

        // NoiseTracker.HeardNoise + Update, with LizardAI.ReactToNoise: the
        // noises of this tick it hears. Noises carry no source here: one close
        // to where it last knew the slugcat to be is the slugcat's, the rest
        // are mysterious and worth examining
        void noise_update(sys::game_state& state, dcon::lizard_id l, room_view const& room) {
            auto& w = state.world;
            auto& a = w.lizard_get_ai(l);
            auto& z = w.lizard_get_core(l);
            breed const b = breed_of(state, l);
            vec2 const head = chunk_pos(state, l, 0);
            float const hearing = b == breed::black || b == breed::indigo ? 2.f : custom::lerp_map(a.run_speed, 0.f, 0.6f, 1.3f, 0.6f);
            for (auto const& n : state.noises) {
                if (n.room != room.id)
                    continue;
                float const under = room.point_submerged(n.pos) || room.get_tile(head).water == 1 ? 0.2f : 1.f;
                if (!custom::dist_less(head, n.pos, n.strength * hearing * under))
                    continue;
                // ignoreSeenNoises
                if (sees_point(state, l, room, n.pos))
                    continue;
                bool const from_prey = a.rep.player && rw::distance(n.pos, a.rep.last_seen_pos) < custom::lerp_map(float(a.rep.ticks_since_seen), 20.f, 600.f, 200.f, 1000.f);
                if (!from_prey) {
                    a.mysterious_noises += 1.f;
                    a.mysterious_noise_counter = 200;
                    a.examining = true;
                    a.examine_pos = room_view::get_tile_position(n.pos);
                }
                // LizardAI.ReactToNoise
                if (from_prey && b == breed::black) {
                    z.bubble = std::max(z.bubble, 4);
                } else if (from_prey && player_relationship(state, l, player_of(state, a.rep.player)).eats) {
                    enter_animation(state, l, animation::prey_re_spotted, false);
                } else if (!from_prey && (a.current == behavior::idle || a.current == behavior::investigate_sound || a.current == behavior::lurk)) {
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
        float noise_utility(sys::game_state const& state, dcon::lizard_id l) {
            auto const& a = state.world.lizard_get_ai(l);
            if (!a.examining)
                return 0.f;
            return mathf::pow(mathf::inverse_lerp(1.f, 20.f, a.mysterious_noises), 0.5f);
        }

        //
        // LizardAI.LurkTracker
        //

        // LurkPosScore (AItile.visibility, the tiles seen from a tile, isn't
        // mapped here: those terms are left out)
        float lurk_pos_score(sys::game_state& state, dcon::lizard_id l, room_view const& room, ivec2 t) {
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
        void lurk_update(sys::game_state& state, dcon::lizard_id l, room_view const& room) {
            auto& w = state.world;
            auto& a = w.lizard_get_ai(l);
            auto& rng = w.lizard_get_random(l);
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
        float lurk_utility(sys::game_state& state, dcon::lizard_id l, room_view const& room) {
            if (breed_of(state, l) == breed::white)
                return 0.5f;
            if (!params_of(state, l).swimmer)
                return 0.f;
            auto const& lk = state.world.lizard_get_ai(l).lurk;
            if (lurk_pos_score(state, l, room, lk.lurk_position) <= 0.f)
                return 0.f;
            return room.get_tile(lk.lurk_position).water == 0 ? 0.2f : 0.5f;
        }

        //
        // LizardAI.LizardSpitTracker (Red, Caramel)
        //

        // SpitFromPosScore
        float spit_from_pos_score(sys::game_state& state, dcon::lizard_id l, room_view const& room, ivec2 t) {
            if (!reachable_and_getbackable(state, l, room, t))
                return std::numeric_limits<float>::lowest();
            auto const& s = state.world.lizard_get_ai(l).spit;
            float num = -float_dist(t, s.want_to_spit_at_pos);
            if (room.visual_contact(room_view::middle_of_tile(t), room_view::middle_of_tile(s.want_to_spit_at_pos)))
                num += 1000.f;
            return num;
        }

        void spit_update(sys::game_state& state, dcon::lizard_id l, room_view const& room) {
            auto& w = state.world;
            auto& a = w.lizard_get_ai(l);
            auto& s = a.spit;
            auto& rng = w.lizard_get_random(l);
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
            if (a.current == behavior::flee || a.current == behavior::travelling || a.current == behavior::escape_rain)
                return;
            auto const p = player_of(state, a.rep.player);
            if (!p || !player_relationship(state, l, p).eats)
                return;
            ivec2 const g = best_guess(a.rep);
            ivec2 const at = here(state, l);
            if (float_dist(g, at) < 6.f)
                return;
            if (at_spit_pos(state, l)) {
                if (a.current == behavior::go_to_spit_pos)
                    s.spitting = true;
                if (!a.rep.visual_contact && player_here(state, p, room.id)
                && room.visual_contact(chunk_pos(state, l, 0), w.body_chunk_get_pos(player::body_chunk(state, p, uint32_t(rng.range(0, 2))))))
                    see_player(state, l, p);
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
                    vec2 const danger = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
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
            if (!player_here(state, p, room.id))
                return;
            if (s.wanna_spit_counter > 100 && !at_spit_pos(state, l))
                return;
            if (s.wanna_spit_counter <= 100 && !a.rep.visual_contact)
                return;
            s.spitting = true;
            s.general_aim_up = mathf::lerp(s.general_aim_up, mathf::lerp(rng.value(), 0.5f, rng.value()), 0.003f);
        }

        //
        // YellowAI: the pack (every conscious yellow lizard in the room) tells
        // each other of the slugcat any of them sees, flashing as they do
        //

        void yellow_update(sys::game_state& state, dcon::lizard_id l, room_view const& room) {
            auto& w = state.world;
            auto& a = w.lizard_get_ai(l);
            auto& rng = w.lizard_get_random(l);
            if (a.communicating > 0)
                --a.communicating;
            float const num = mathf::inverse_lerp(0.f, 14.f, float(a.communicating));
            if (a.comm_flicker < num)
                a.comm_flicker = mathf::min(num, a.comm_flicker + 0.25f);
            else
                a.comm_flicker = mathf::max(num, a.comm_flicker - 0.025f);
            w.for_each_lizard([&](dcon::lizard_id o) {
                if (o == l || breed_of(state, o) != breed_of(state, l) || room_of(state, o) != room.id || !consious(state, o))
                    return;
                // PackMemberIsSeeingCreature: what a pack member sees and this one doesn't
                auto& oa = w.lizard_get_ai(o);
                auto const p = player_of(state, oa.rep.player);
                if (!p || !oa.rep.visual_contact || (a.rep.player == oa.rep.player && a.rep.visual_contact))
                    return;
                // RecieveInfoOnCritter
                see_player(state, l, p);
                float const intensity = player_relationship(state, l, p).intensity;
                a.communicating = std::max(a.communicating, rng.range(3, int32_t(mathf::lerp(3.f, 50.f, intensity))));
                oa.communicating = std::max(oa.communicating, rng.range(3, int32_t(mathf::lerp(3.f, 50.f, intensity))));
            });
        }

        //
        // LizardAI
        //

        // LizardAI.IdleSpotScore (AItile.visibility, which divides the base
        // score, taken as constant)
        float idle_spot_score(sys::game_state& state, dcon::lizard_id l, room_view const& room, ivec2 t) {
            auto const& a = state.world.lizard_get_ai(l);
            auto const& p = params_of(state, l);
            if (!room.inside(t.x, t.y))
                return 50.f;
            if (!pathing::accessible(room, t, p.movement) || !reachable_and_getbackable(state, l, room, t) || t == a.forbidden_idle_spot)
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
        bool comfortable_idle_position(sys::game_state const& state, dcon::lizard_id l, room_view const& room) {
            vec2 const p0 = chunk_pos(state, l, 0), p1 = chunk_pos(state, l, 1), p2 = chunk_pos(state, l, 2);
            if (room.get_tile(p0).water != 0)
                return params_of(state, l).swimmer;
            ivec2 const t0 = room_view::get_tile_position(p0), t2 = room_view::get_tile_position(p2);
            return room.get_tile(p0 + custom::dir_vec(p1, p0) * 30.f).t != world::terrain::solid
                && (t0.y == t2.y || (breed_of(state, l) == breed::blue && t0.x == t2.x));
        }

        // LizardAI.FallRisk
        bool fall_risk(sys::game_state const& state, dcon::lizard_id l, room_view const& room, ivec2 tile) {
            return fall_risk_tile(room, tile).y < here(state, l).y - 5;
        }

        // LizardAI.AggressiveBehavior towards the slugcat
        void aggressive_behavior(sys::game_state& state, dcon::lizard_id l, room_view const& room, dcon::player_id p, float tongue_chance) {
            auto& w = state.world;
            auto& a = w.lizard_get_ai(l);
            auto& z = w.lizard_get_core(l);
            auto& rng = w.lizard_get_random(l);
            auto const& par = params_of(state, l);
            a.casual_aggression = false;
            a.destination = best_guess(a.rep);
            vec2 const head = chunk_pos(state, l, 0);
            vec2 const prey = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
            bool const seen = a.rep.visual_contact && player_here(state, p, room.id);
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
            a.focus = true;
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
            && (physics::object_submersion(state, room, body(state, l)) < 0.5f || physics::object_submersion(state, room, player::body(state, p)) < 0.5f)
            && rng.value() < par.tongue_chance * tongue_chance) {
                enter_animation(state, l, animation::shoot_tongue, false);
            }
            if (a.rep.age > 120 && jaw_ready_for_bite(state, l)) {
                ++a.attempt_bite_frames;
                for (uint32_t i = 0; i < 2; ++i)
                    if (custom::dist_less(w.body_chunk_get_pos(player::body_chunk(state, p, i)), head, par.attempt_bite_radius)) {
                        attempt_bite(state, l, p);
                        return;
                    }
                return;
            }
            if (a.attempt_bite_frames > 5)
                jaws_snap_shut(state, l, prey);
            a.attempt_bite_frames = 0;
        }

        // LizardAI.DetermineBehavior
        behavior determine_behavior(sys::game_state& state, dcon::lizard_id l) {
            auto& a = state.world.lizard_get_ai(l);
            behavior b = behavior::idle;
            a.current_utility = a.u.best_utility;
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
            if (a.current_utility < 0.05f) {
                a.current_utility = 0.05f;
                b = behavior::idle;
            }
            if (b != behavior::flee && grasped(state, l) && player_relationship(state, l, grasped(state, l)).eats) {
                b = behavior::return_prey;
                a.current_utility = 1.f;
            }
            if (b == behavior::hunt && a.rep.player && a.frustration > 0.75f && breed_of(state, l) != breed::red
            && chance_of_finding(a.rep) > 0.4f && custom::dist_less(chunk_pos(state, l, 0), room_view::middle_of_tile(best_guess(a.rep)), 350.f))
                b = behavior::frustrated;
            // (no migration: TravelUtility is 0)
            if ((breed_of(state, l) == breed::red || breed_of(state, l) == breed::caramel) && a.spit.go_to_spit_pos_counter > 0)
                b = behavior::go_to_spit_pos;
            return b;
        }

        // UtilityComparer.Update over LizardAI's modules (the ones that can
        // have a utility here)
        void compare_utilities(sys::game_state& state, dcon::lizard_id l, room_view const& room) {
            auto& w = state.world;
            auto& a = w.lizard_get_ai(l);
            breed const b = breed_of(state, l);
            // threat: FloatTweenUpAndDown(lerp 0.5, tick), nothing threatens it here
            float const down = 1.f / ((float(a.last_distress_length) + 20.f) * 3.f);
            a.u.threat = a.u.threat < 0.f ? mathf::lerp(a.u.threat, 0.f, 0.5f) : mathf::max(a.u.threat - down, 0.f);
            // prey: lerp 0.15, weight 0.6
            a.u.prey = mathf::lerp(a.u.prey, prey_utility(state, l, room) * 0.6f, 0.15f);
            float const injury = injury_utility(state, l) * (b == breed::red ? 0.4f : 0.9f);
            float const noise = noise_utility(state, l) * 0.2f;
            float const lurk = lurker(b) ? lurk_utility(state, l, room) * mathf::lerp(0.4f, 0.3f, w.lizard_get_energy(l)) : 0.f;
            float const values[] = {a.u.threat, a.u.prey, 0.f, injury, 0.f, 0.f, noise, 0.f, lurk};
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
    }

    dcon::player_id tracked(sys::game_state const& state, dcon::lizard_id l) noexcept {
        return player_of(state, state.world.lizard_get_ai(l).rep.player);
    }

    dcon::player_id focus(sys::game_state const& state, dcon::lizard_id l) noexcept {
        auto const& a = state.world.lizard_get_ai(l);
        return a.focus ? player_of(state, a.rep.player) : dcon::player_id{};
    }

    pathing::field_id field(sys::game_state& state, dcon::lizard_id l, room_view const& room) {
        return pathing::field_to(state, room.id, state.world.lizard_get_ai(l).destination, params_of(state, l).movement);
    }

    bool reachable_and_getbackable(sys::game_state& state, dcon::lizard_id l, room_view const& room, ivec2 t) {
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

    bool unpleasant_fall_risk(sys::game_state& state, dcon::lizard_id l, room_view const& room, ivec2 tile) {
        ivec2 const f = fall_risk_tile(room, tile);
        return room.get_tile(f).water != 0 || f.y < 0 || f.y < tile.y - 20 || (f.y < tile.y - 10 && !reachable_and_getbackable(state, l, room, f));
    }

    bool want_to_hold_with_tongue(sys::game_state& state, dcon::lizard_id l, room_view const& room, dcon::player_id p, uint32_t k) {
        auto const c = player::body_chunk(state, p, k);
        return player_here(state, p, room.id) && physics::submersion(state, room, c) <= 0.5f
            && (!unpleasant_fall_risk(state, l, room, here(state, l)) || !unpleasant_fall_risk(state, l, room, room_view::get_tile_position(state.world.body_chunk_get_pos(c))))
            && player_relationship(state, l, p).go_for_kill();
    }

    relationship player_relationship(sys::game_state const& state, dcon::lizard_id l, dcon::player_id p) {
        (void)p;
        // (spears and vulture masks the slugcat carries: masks don't exist here)
        float const num = like_of_player(state, l);
        if (num < 0.5f)
            return relationship{true, mathf::pow(mathf::inverse_lerp(0.5f, -1.f, num), params_of(state, l).aggression_curve_exponent)};
        return relationship{false, 0.f};
    }

    void see_player(sys::game_state& state, dcon::lizard_id l, dcon::player_id p) {
        auto& w = state.world;
        auto& r = w.lizard_get_ai(l).rep;
        bool first = false, again = false;
        if (r.player != player_ref(p)) {
            r = tracked_player{};
            r.player = player_ref(p);
            first = true;
        } else if (!r.visual_contact) {
            again = true;
        }
        r.visual_contact = true;
        r.ticks_since_seen = 0;
        r.last_seen_pos = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
        if (first || again)
            creature_spotted(state, l, first, p);
    }

    void bit_creature(sys::game_state& state, dcon::lizard_id l, room_view const& room) {
        if (!unpleasant_fall_risk(state, l, room, here(state, l)))
            enter_animation(state, l, animation::shake_prey, true);
    }

    bool at_spit_pos(sys::game_state const& state, dcon::lizard_id l) noexcept {
        return custom::dist_less(chunk_pos(state, l, 0), room_view::middle_of_tile(state.world.lizard_get_ai(l).spit.spit_from_pos), 50.f);
    }

    bool spit_aim_pos(sys::game_state& state, dcon::lizard_id l, vec2& out) {
        auto& w = state.world;
        auto& a = w.lizard_get_ai(l);
        auto& s = a.spit;
        auto& rng = w.lizard_get_random(l);
        auto const p = player_of(state, a.rep.player);
        if (!p || !a.rep.visual_contact)
            return false;
        uint32_t const k = uint32_t(rng.range(0, 2));
        auto const c = player::body_chunk(state, p, k);
        vec2 v = w.body_chunk_get_pos(c);
        vec2 const lead = v - w.body_chunk_get_last_pos(c);
        s.aim_player = player_ref(p);
        s.aim_chunk = uint8_t(k);
        s.aim_up = s.general_aim_up + mathf::lerp(-0.15f, 0.15f, rng.value());
        vec2 const head = chunk_pos(state, l, 0);
        v.y += mathf::pow(rw::distance(v, head), mathf::lerp(1.2f, 1.55f, s.aim_up)) * 0.015f * s.aim_up;
        v += lead * rw::distance(v, head) * 0.01f * rng.value();
        out = v;
        return true;
    }

    void spit_set_aim(sys::game_state& state, dcon::lizard_id l, float am) {
        state.world.lizard_get_ai(l).spit.general_aim_up = am;
    }

    void spit_aim_a_bit_up(sys::game_state& state, dcon::lizard_id l, float am) {
        auto& s = state.world.lizard_get_ai(l).spit;
        if (am < s.general_aim_up)
            s.general_aim_up = mathf::clamp(s.general_aim_up + 0.3f * mathf::inverse_lerp(1.f, 0.5f, s.general_aim_up) * state.world.lizard_get_random(l).value(), am, 1.f);
    }

    void spit_aim_a_bit_down(sys::game_state& state, dcon::lizard_id l, float am) {
        auto& s = state.world.lizard_get_ai(l).spit;
        if (am > s.general_aim_up)
            s.general_aim_up = mathf::clamp(s.general_aim_up - 0.3f * mathf::inverse_lerp(0.f, 0.5f, s.general_aim_up) * state.world.lizard_get_random(l).value(), 0.f, am);
    }

    void reset(sys::game_state& state, dcon::lizard_id l) {
        auto& w = state.world;
        auto& rng = w.lizard_get_random(l);
        ai_state a;
        ivec2 const at = here(state, l);
        a.destination = at;
        a.lurk.lurk_position = at;
        a.lurk.look_position = at;
        a.spit.random_cycle = rng.range(100, 800);
        a.spit.spit_from_pos = at;
        a.spit.temp_spit_from_pos = at;
        w.lizard_set_ai(l, a);
    }

    void update(sys::game_state& state, dcon::lizard_id l, room_view const& room) {
        auto& w = state.world;
        auto& a = w.lizard_get_ai(l);
        auto& z = w.lizard_get_core(l);
        auto& rng = w.lizard_get_random(l);
        auto const& par = params_of(state, l);
        breed const b = breed_of(state, l);
        if (a.panic > 0)
            --a.panic;
        ++a.time_in_room;
        // SocialMemory.EvenOutAllTemps
        a.temp_like = a.temp_like > 0.f ? mathf::max(0.f, a.temp_like - 0.0005f) : mathf::min(0.f, a.temp_like + 0.0005f);
        // the utilities as the comparer smoothed them last tick
        a.fear = a.u.threat;
        a.hunger = a.u.prey / 0.6f;
        a.rain_fear = 0.f;
        a.excitement = mathf::lerp(a.excitement, mathf::max(a.hunger, mathf::max(a.fear, mathf::pow(a.rain_fear, 3.f))), 0.1f);
        if (a.fear > 0.8f)
            a.last_distress_length = custom::int_clamp(a.last_distress_length + 1, 0, 500);
        else if (a.fear < 0.2f)
            a.last_distress_length = 0;
        a.current = determine_behavior(state, l);
        set_jaw_open(state, l, jaw_open(state, l) - 0.01f);
        auto const prey = player_of(state, a.rep.player);
        if (lurker(b) && a.current == behavior::hunt && prey && a.run_speed < 0.1f && float_dist(here(state, l), best_guess(a.rep)) > 17.f)
            a.current = behavior::lurk;
        ivec2 const at = here(state, l);
        auto const den_tile = den(state, room.id);
        switch (a.current) {
        case behavior::hunt:
            if (prey && player_relationship(state, l, prey).eats)
                aggressive_behavior(state, l, room, prey, 1.f);
            a.run_speed = mathf::lerp(a.run_speed, mathf::max(a.hunger, 0.3f), 0.1f);
            if (rng.value() < 0.0125f && b != breed::black)
                make_sound(state, l, emotion::blood_lust);
            break;
        case behavior::idle: {
            bool restless = true;
            int32_t const before = a.idle_counter--;
            if (custom::manhattan_distance(at, a.destination) < 5) {
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
            if (idle_spot_score(state, l, room, a.destination) == float_max)
                a.idle_counter = 0;
            if (a.idle_counter < 1) {
                if (before > 0)
                    a.forbidden_idle_spot = a.destination;
                ivec2 const t{rng.range(0, room.width), rng.range(0, room.height)};
                if (idle_spot_score(state, l, room, t) < idle_spot_score(state, l, room, a.destination)) {
                    a.destination = t;
                    a.idle_spot_win_streak = 0;
                } else if (++a.idle_spot_win_streak > 50) {
                    a.idle_counter = rng.range(1000, 4000);
                    a.idle_spot_win_streak = 0;
                }
            }
            if (restless) {
                if (a.forbidden_idle_spot != a.destination)
                    a.run_speed = mathf::lerp(a.run_speed, 0.25f, 0.5f);
            } else {
                a.run_speed = mathf::lerp(a.run_speed, 0.f, 0.5f);
            }
            if (rng.value() < 0.00125f)
                make_sound(state, l, emotion::boredom);
            a.focus = false;
            break;
        }
        case behavior::return_prey:
            if (den_tile)
                a.destination = *den_tile;
            a.run_speed = mathf::lerp(a.run_speed, mathf::pow(mathf::max(a.fear, 0.5f), 0.5f), 0.1f);
            // carried into the den: eaten (the slugcat carried into a shortcut)
            if (den_tile && custom::manhattan_distance(at, *den_tile) < 2)
                if (auto const p = grasped(state, l)) {
                    items::release_all(state, p);
                    release_player(state, p);
                    if (!w.player_get_dead(p))
                        player::die(state, p);
                    w.player_set_eaten(p, 1);
                }
            break;
        case behavior::injured:
            if (den_tile)
                a.destination = *den_tile;
            a.run_speed = mathf::lerp(a.run_speed, 1.f, 0.01f);
            if (rng.value() < 0.0125f)
                make_sound(state, l, emotion::pain_idle, 1.f - w.lizard_get_health(l));
            break;
        case behavior::frustrated:
            a.focus = prey != dcon::player_id{};
            a.run_speed = mathf::lerp(a.run_speed, 1.f, 0.5f);
            if (rng.value() < 0.1f || custom::manhattan_distance(at, a.destination) < 10) {
                ivec2 const t{rng.range(0, room.width), rng.range(0, room.height)};
                if (reachable_and_getbackable(state, l, room, t)) {
                    a.destination = t;
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
            if (custom::manhattan_distance(at, a.destination) > 5 && lurk_pos_score(state, l, room, at) * 1.2f < lurk_pos_score(state, l, room, lk.lurk_position)) {
                a.run_speed = mathf::lerp(a.run_speed, 0.5f, 0.5f);
                a.destination = lk.lurk_position;
            } else if (sees_point(state, l, room, room_view::middle_of_tile(lk.look_position))) {
                a.destination = lk.lurk_position;
                a.run_speed = mathf::lerp(a.run_speed, 0.f, 0.5f);
            } else {
                a.destination = lk.lurk_position + custom::four_directions[rng.range(0, 4)];
                a.run_speed = mathf::lerp(a.run_speed, 0.7f, 0.2f);
            }
            break;
        }
        case behavior::investigate_sound:
            a.destination = a.examine_pos;
            a.run_speed = mathf::lerp(a.run_speed, 0.6f, 0.2f);
            break;
        case behavior::go_to_spit_pos:
            a.destination = a.spit.spit_from_pos;
            a.run_speed = mathf::lerp(a.run_speed, 1.f, 0.2f);
            break;
        default:
            break;
        }
        if (a.current != behavior::idle && b == breed::red)
            a.run_speed = mathf::pow(a.run_speed, 0.7f);
        a.run_speed = mathf::max(a.run_speed, stuck_utility(state, l));
        // casualAggressionTarget: biting at the slugcat close by, whatever it is doing
        if (!grasped(state, l) && prey) {
            bool const want = player_relationship(state, l, prey).go_for_kill() || (rng.value() < 0.1f && rng.value() > w.lizard_get_health(l))
                || (rng.value() > w.lizard_get_health(l) && rng.value() < 0.05f);
            if (want && a.rep.visual_contact)
                a.casual_aggression = true;
        }
        if (a.casual_aggression && prey && rng.value() < 0.5f && a.rep.visual_contact && player_here(state, prey, room.id)
        && custom::dist_less(w.body_chunk_get_pos(player::body_chunk(state, prey, 0)), chunk_pos(state, l, 0), par.attempt_bite_radius))
            attempt_bite(state, l, prey);
        if ((b == breed::red || b == breed::caramel) && a.spit.spitting)
            enter_animation(state, l, animation::spit, false);
        // (the Blizzard's beam isn't ported)

        // ArtificialIntelligence.Update: the modules, in the order LizardAI added them
        tracker_update(state, l, room);
        noise_update(state, l, room);
        prey_tracker_update(state, l, room);
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
