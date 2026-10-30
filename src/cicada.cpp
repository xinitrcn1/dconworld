#include <cmath>
#include <limits>
#include <optional>
#include <vector>

#include "cicada.hpp"
#include "dens.hpp"
#include "rooms.hpp"
#include "cicada_graphics.hpp"
#include "items.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "player.hpp"

namespace cicada {
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // CreatureTemplate: CicadaA (CicadaB inherits it)
        constexpr float base_damage_resistance = 0.8f;
        constexpr float base_stun_resistance = 1.f;
        constexpr float visual_radius = 1100.f;
        // Tracker(AI, 10, 10, 250, ...): ticks a creature out of sight is remembered
        constexpr int32_t forget_time = 250;
        constexpr float float_max = std::numeric_limits<float>::max();

        using creature::player_of;
        using creature::player_ref;
        using creature::room_of;

        vec2& pos(sys::game_state& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_pos(chunk(state, c, i));
        }
        vec2 pos(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_pos(chunk(state, c, i));
        }
        vec2& vel(sys::game_state& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_vel(chunk(state, c, i));
        }

        // abstractCreature.pos: the head's tile
        ivec2 here(sys::game_state const& state, dcon::creature_id c) {
            return room_view::get_tile_position(pos(state, c, 0));
        }

        dcon::creature_id cicada_of(sys::game_state const& state, uint16_t v) {
            if (v == 0)
                return dcon::creature_id{};
            dcon::creature_id const c{dcon::creature_id::value_base_t(v - 1)};
            return ::creature::is(state.world, c, ::creature::type::cicada) ? c : dcon::creature_id{};
        }

        float object_mass(sys::game_state const& state, dcon::physical_object_id o) {
            float m = 0.f;
            state.world.physical_object_for_each_chunk_ownership_as_owner(o, [&](dcon::chunk_ownership_id co) {
                m += state.world.body_chunk_get_mass(state.world.chunk_ownership_get_chunk(co));
            });
            return m;
        }

        bool solid(room_view const& room, ivec2 t) {
            return room.get_tile(t).solid();
        }

        ivec2 random_tile(rw::rng& rng, room_view const& room) {
            return ivec2{rng.range(0, room.width), rng.range(0, room.height)};
        }

        // a coordinate somewhere in the room (WorldCoordinates off the room's
        // edges aren't followed here)
        ivec2 clamp_to_room(room_view const& room, ivec2 t) {
            return ivec2{custom::int_clamp(t.x, 0, room.width - 1), custom::int_clamp(t.y, 0, room.height - 1)};
        }

        float float_dist(ivec2 a, ivec2 b) {
            return rw::magnitude(rw::to_vec2(a - b));
        }

        int32_t proximity(sys::game_state& state, room_view const& room, ivec2 t) {
            return pathing::ai_terrain_proximity(state, room.id, t);
        }
        int32_t proximity(sys::game_state& state, room_view const& room, vec2 p) {
            return proximity(state, room, room_view::get_tile_position(p));
        }

        bool holds_spear(sys::game_state const& state, dcon::player_id p) {
            for (uint32_t h = 0; h < 2; ++h) {
                auto const it = items::held(state, p, h);
                if (!it)
                    continue;
                auto const t = state.world.item_get_type(it);
                if (t == world::item_type::spear || t == world::item_type::explosive_spear || t == world::item_type::electric_spear)
                    return true;
            }
            return false;
        }

        bool player_here(sys::game_state const& state, dcon::player_id p, dcon::room_id room) {
            auto const& w = state.world;
            return p && !w.player_get_in_shortcut(p) && player::body(state, p)
                && w.physical_object_get_room_from_object_location(player::body(state, p)) == room;
        }

        dcon::player_id holder(sys::game_state const& state, dcon::creature_id c) {
            return player_of(state, state.world.creature_get_payload(c).cicada.held_by);
        }

        //
        // Cicada
        //

        // Cicada.Climbable: next to the terrain, or on a pole
        bool climbable(sys::game_state& state, room_view const& room, ivec2 t) {
            return proximity(state, room, t) == 1 || pathing::tile_access(room, t) == pathing::access::climb;
        }

        // Cicada.WantToSitDownAtDestination
        bool want_to_sit_down_at_destination(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            return ::cicada::behavior(state.world.creature_get_behavior(c)) == behavior::idle && climbable(state, room, state.world.creature_get_destination(c));
        }

        // Cicada.AtSitDestination
        bool at_sit_destination(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            ivec2 const dest = state.world.creature_get_destination(c);
            return want_to_sit_down_at_destination(state, c, room) && custom::manhattan_distance(here(state, c), dest) < 2 && climbable(state, room, dest);
        }

        // ArtificialIntelligence.VisualScore + CicadaAI.VisualScore: it sees
        // ahead of its head, not behind its body (no water terms)
        bool visual_contact(sys::game_state const& state, dcon::creature_id c, room_view const& room, vec2 look) {
            vec2 const eye = pos(state, c, 0);
            if (!custom::dist_less(eye, look, visual_radius))
                return false;
            float score = mathf::inverse_lerp(visual_radius, 0.f, rw::distance(eye, look));
            score -= mathf::inverse_lerp(1.f, -0.3f, rw::dot(rw::normalized(pos(state, c, 1) - eye), rw::normalized(pos(state, c, 1) - look)));
            return score > 0.f && room.visual_contact(eye, look);
        }

        // Cicada.Stun (+ Creature.Stun)
        void stun(sys::game_state& state, dcon::creature_id c, int32_t st) {
            auto& w = state.world;
            w.creature_get_payload(c).cicada.flying = 0;
            w.creature_get_payload(c).cicada.charge_counter = 0;
            // (LoseAllGrasps: it carries nothing here)
            w.creature_set_stun(c, std::max(w.creature_get_stun(c), st));
        }

        // Creature.Die
        void die(sys::game_state& state, dcon::creature_id c) {
            state.world.creature_set_dead(c, 1);
            state.world.creature_get_payload(c).cicada.sticky_player = 0;
        }

        // Cicada.Land
        void land(sys::game_state& state, dcon::creature_id c) {
            state.world.creature_get_payload(c).cicada.wait_to_fly_counter = 0;
            state.world.creature_get_payload(c).cicada.flying = 0;
            state.play_sound(sound::sound_id::cicada_landing, room_of(state, c), pos(state, c, 0));
        }

        // Cicada.TakeOff
        void take_off(sys::game_state& state, dcon::creature_id c, room_view const& room, vec2 dir) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            w.creature_get_payload(c).cicada.wait_to_fly_counter = 0;
            w.creature_get_payload(c).cicada.flying = 1;
            int32_t num = 0;
            vec2 v{0.f, 0.f};
            for (int i = 0; i < 8; ++i)
                for (int j = 0; j < 3; ++j) {
                    int32_t const prox = proximity(state, room, here(state, c) + custom::eight_directions[i] * j);
                    num += prox;
                    // (sic) vanilla assigns rather than adds: the last direction wins
                    v = rw::to_vec2(custom::eight_directions[i]) * float(prox);
                }
            v /= float(num);
            float const value = rng.value();
            vel(state, c, 0) += rw::normalized(rw::lerp(dir, v, 0.5f)) * 9.f * value;
            vel(state, c, 1) += rw::normalized(rw::lerp(dir, v, 0.8f)) * 7.f * value;
            w.creature_get_payload(c).cicada.flying_power = 0.5f;
            state.play_sound(sound::sound_id::cicada_wings_takeoff, room.id, pos(state, c, 0), 1.f, w.cicada_data_get_ivars(creature::cicada_data(w, c)).wing_sound_pitch);
        }

        // Cicada.Charge
        void charge(sys::game_state& state, dcon::creature_id c, vec2 at) {
            auto& w = state.world;
            w.creature_get_payload(c).cicada.sticky_player = 0;
            w.creature_get_payload(c).cicada.no_sticky_counter = 140;
            if (w.creature_get_payload(c).cicada.charge_counter > 0)
                return;
            w.creature_get_payload(c).cicada.charge_dir = custom::dir_vec(pos(state, c, 0), at);
            w.creature_get_payload(c).cicada.charge_counter = 1;
            state.play_sound(sound::sound_id::cicada_wings_prepare_bump_attack, room_of(state, c), pos(state, c, 0));
        }

        // Cicada.FindBodySitPos
        ivec2 find_body_sit_pos(sys::game_state& state, dcon::creature_id c, room_view const& room, ivec2 head) {
            int32_t const flip = state.world.creature_get_payload(c).cicada.flip_h;
            if (climbable(state, room, head + ivec2{0, -1}))
                return head + ivec2{0, -1};
            if (climbable(state, room, head + ivec2{flip, 0}))
                return head + ivec2{flip, 0};
            if (climbable(state, room, head + ivec2{-flip, 0}))
                return head + ivec2{-flip, 0};
            return head + ivec2{0, 1};
        }

        // Cicada.BodySitPosOffset: hugging the wall, ceiling or floor of the
        // tile, or one side of a pole (sets sitDirection)
        vec2 body_sit_pos_offset(sys::game_state& state, dcon::creature_id c, room_view const& room, ivec2 t) {
            auto& w = state.world;
            int32_t const flip = w.creature_get_payload(c).cicada.flip_h;
            vec2 const m = room_view::middle_of_tile(t);
            if (solid(room, t + ivec2{flip, 0})) {
                w.cicada_data_set_sit_direction(creature::cicada_data(w, c), ivec2{flip, 0});
                return m + vec2{float(flip) * -2.f, 0.f};
            }
            if (solid(room, t + ivec2{-flip, 0})) {
                w.cicada_data_set_sit_direction(creature::cicada_data(w, c), ivec2{-flip, 0});
                return m + vec2{float(-flip) * -2.f, 0.f};
            }
            if (solid(room, t + ivec2{0, 1})) {
                w.cicada_data_set_sit_direction(creature::cicada_data(w, c), ivec2{0, 1});
                return m + vec2{0.f, -2.f};
            }
            if (solid(room, t + ivec2{0, -1})) {
                w.cicada_data_set_sit_direction(creature::cicada_data(w, c), ivec2{0, -1});
                return m + vec2{0.f, 2.f};
            }
            if (room.get_tile(t).vertical_beam()) {
                if (!solid(room, t + ivec2{flip, 0})) {
                    w.cicada_data_set_sit_direction(creature::cicada_data(w, c), ivec2{-flip, 0});
                    return m + vec2{float(flip) * 7.f, 0.f};
                }
                if (!solid(room, t + ivec2{-flip, 0})) {
                    w.cicada_data_set_sit_direction(creature::cicada_data(w, c), ivec2{flip, 0});
                    return m + vec2{float(-flip) * 7.f, 0.f};
                }
            }
            return m;
        }

        //
        // CicadaAI
        //

        dcon::player_id tracked(sys::game_state const& state, dcon::creature_id c) {
            return player_of(state, state.world.creature_get_seen_player(c));
        }

        // Tracker: the closest slugcat it can see, remembered for a while
        void tracker_update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            dcon::player_id seen;
            float best = float_max;
            rooms::for_each_player(state, room.id, [&](dcon::player_id p) {
                if (!player_here(state, p, room.id))
                    return;
                for (uint32_t i = 0; i < 2; ++i) {
                    vec2 const pp = w.body_chunk_get_pos(player::body_chunk(state, p, i));
                    float const dist = rw::distance(pp, pos(state, c, 0));
                    if (dist < best && visual_contact(state, c, room, pp)) {
                        best = dist;
                        seen = p;
                    }
                }
            });
            int32_t& since = w.creature_get_ticks_since_seen(c);
            w.creature_set_sees_player(c, seen ? 1 : 0);
            if (seen) {
                if (tracked(state, c) != seen)
                    w.cicada_data_set_seen_age(creature::cicada_data(w, c), 0);
                w.creature_set_seen_player(c, player_ref(seen));
                w.creature_set_last_seen_pos(c, w.body_chunk_get_pos(player::body_chunk(state, seen, 0)));
                since = 0;
            } else if (w.creature_get_seen_player(c) && ++since > forget_time) {
                w.creature_set_seen_player(c, 0);
            }
            if (w.creature_get_seen_player(c))
                ++w.cicada_data_get_seen_age(creature::cicada_data(w, c));
        }

        // Tracker.CreatureRepresentation.EstimatedChanceOfFinding
        float chance_of_finding(sys::game_state const& state, dcon::creature_id c) {
            return custom::lerp_map(float(state.world.creature_get_ticks_since_seen(c)), 0.f, float(forget_time), 1.f, 0.f);
        }

        // CreatureTemplate.Relationship, as CicadaAI.UpdateDynamicRelationship
        // returns it towards a slugcat
        struct relationship {
            enum class kind : uint8_t { ignores, stay_out_of_way, antagonizes, afraid } type = kind::ignores;
            float intensity = 0.f;
        };

        // CicadaAI.LikeOfPlayer: the community's like (0, nothing changes it
        // here) leaning on the temporary like
        float like_of_player(sys::game_state const& state, dcon::creature_id c) {
            float const temp = state.world.cicada_data_get_temp_like(creature::cicada_data(state.world, c));
            return mathf::lerp(0.f, temp, std::fabs(temp));
        }

        // CicadaAI.ContextualDislikeOfRival (it never holds cicada food)
        float contextual_dislike_of_rival(sys::game_state const& state, dcon::creature_id c, dcon::player_id p, float intensity) {
            auto const& w = state.world;
            float num;
            if (w.creature_get_sees_player(c) && tracked(state, c) == p)
                num = mathf::inverse_lerp(mathf::lerp(100.f, 300.f, intensity), 40.f, rw::distance(pos(state, c, 0), w.body_chunk_get_pos(player::body_chunk(state, p, 0))));
            else
                num = mathf::inverse_lerp(mathf::lerp(80.f, 180.f, intensity), 40.f, rw::distance(pos(state, c, 0), room_view::middle_of_tile(w.creature_get_last_seen_pos(c)))) * 0.5f;
            return num * intensity;
        }

        // CicadaAI.UpdateDynamicRelationship for a slugcat (its CicadaTrackState
        // read fresh every tick: holding a cicada, armed with a spear)
        relationship player_relationship(sys::game_state const& state, dcon::creature_id c, dcon::player_id p) {
            auto const& w = state.world;
            float num = like_of_player(state, c);
            if (held_by(state, p))
                num -= mathf::inverse_lerp(0.9f, 0.1f, num) * 0.7f;
            // AgressiveRival: 0.6 for a CicadaA, 0.9 for a CicadaB
            num -= contextual_dislike_of_rival(state, c, p, w.creature_get_payload(c).cicada.gender ? 0.6f : 0.9f) * mathf::inverse_lerp(0.9f, 0.1f, num);
            if (num < -0.1f) {
                if (num < -0.5f && w.cicada_data_get_bravery(creature::cicada_data(w, c)) < 0.5f && holds_spear(state, p))
                    return relationship{relationship::kind::afraid, mathf::inverse_lerp(-0.5f, -1.f, num)};
                return relationship{relationship::kind::antagonizes, mathf::inverse_lerp(-0.1f, -1.f, num)};
            }
            if (num > 0.5f)
                return relationship{relationship::kind::ignores, 0.f};
            return relationship{relationship::kind::stay_out_of_way, custom::lerp_map(num, 0.5f, -0.1f, 0.f, 0.8f, 8.f)};
        }

        // PreyTracker.Utility (PreyTracker(5, 1, 5, 15)): distance estimated in tiles
        float prey_utility(sys::game_state const& state, dcon::creature_id c, float intensity) {
            float const dist = mathf::max(0.01f, float_dist(here(state, c), room_view::get_tile_position(state.world.creature_get_last_seen_pos(c))));
            float const num = mathf::lerp(1.f - mathf::inverse_lerp(5.f, 15.f, dist), mathf::lerp(5.f, 15.f, 0.25f) / dist, 0.5f);
            return mathf::min(num, chance_of_finding(state, c)) * mathf::pow(intensity, 0.75f);
        }

        // ThreatTracker.Utility with the one ThreatCreature's CurrentThreat
        // (no threat points or distance weighting)
        float threat_utility(sys::game_state const& state, dcon::creature_id c, float severity) {
            float const current = mathf::inverse_lerp(0.05f, 0.5f, mathf::pow(chance_of_finding(state, c), 0.75f) * severity);
            return mathf::clamp(mathf::lerp(-0.01f, 1.01f, current), 0.f, 1.f);
        }

        // StuckTracker.Update (trackPastPositions) with its
        // GetUnstuckPosCalculator; whether a tile is reachable is judged by
        // the field to its destination (flyers can go back the way they came)
        void stuck_update(sys::game_state& state, dcon::creature_id c, room_view const& room, bool satisfied) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            auto& s = w.cicada_data_get_stuck(creature::cicada_data(w, c));
            ivec2 const at = here(state, c);
            if (s.counter > 0) {
                ivec2 const test = clamp_to_room(room, room_view::get_tile_position(pos(state, c, 0) + custom::rnv(rng) * rng.value() * 15.f * 20.f));
                auto const f = pathing::field_to(state, room.id, w.creature_get_destination(c), pathing::cicada);
                auto const score = [&](ivec2 t) {
                    if (!pathing::accessible(room, t, pathing::cicada) || pathing::distance(state, f, t) == pathing::unreachable)
                        return float_max;
                    float num = std::fabs(10.f - float_dist(t, at));
                    if (float_dist(t, at) < 20.f && room.visual_contact(room_view::middle_of_tile(t), room_view::middle_of_tile(at)))
                        num -= 1000.f;
                    return num;
                };
                if (score(test) < score(s.unstuck_goal))
                    s.unstuck_goal = test;
            }
            int32_t close = 0;
            if (!satisfied && custom::manhattan_distance(at, w.creature_get_destination(c)) > 3) {
                s.head = (s.head + stuck_tracker::tracked - 1) % stuck_tracker::tracked;
                s.past[s.head] = at;
                s.count = std::min(s.count + 1, stuck_tracker::tracked);
                for (int32_t j = 40; j < s.count; ++j)
                    if (float_dist(at, s.past[(s.head + j) % stuck_tracker::tracked]) < 4.f)
                        ++close;
            }
            s.counter = custom::int_clamp(close > 30 ? s.counter + 1 : s.counter - 2, 0, 200);
        }
        float stuck_utility(sys::game_state const& state, dcon::creature_id c) {
            return mathf::inverse_lerp(100.f, 200.f, float(state.world.cicada_data_get_stuck(creature::cicada_data(state.world, c)).counter));
        }

        // ThreatTracker.FleeTo, simplified: of a few tiles up to 30 away, the
        // one furthest from the threat, kept while nothing better turns up
        ivec2 flee_to(sys::game_state& state, dcon::creature_id c, room_view const& room, vec2 threat) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            ivec2 const at = here(state, c);
            auto const score = [&](ivec2 t) {
                if (!pathing::accessible(room, t, pathing::cicada))
                    return -float_max;
                return rw::distance(room_view::middle_of_tile(t), threat) - mathf::max(0.f, float_dist(t, at) - 30.f) * 20.f;
            };
            ivec2 best = w.creature_get_destination(c);
            for (int i = 0; i < 4; ++i) {
                ivec2 const t = clamp_to_room(room, at + ivec2{rng.range(-30, 31), rng.range(-30, 31)});
                if (score(t) > score(best) + 20.f)
                    best = t;
            }
            return best;
        }

        //
        // CicadaAI.CircleGroup: the leader keeps the group's state
        //

        dcon::creature_id circle_leader(sys::game_state const& state, dcon::creature_id c) {
            return cicada_of(state, state.world.cicada_data_get_circle_leader(creature::cicada_data(state.world, c)));
        }

        template<typename F>
        void for_each_member(sys::game_state const& state, dcon::creature_id leader, F&& f) {
            // a circle group is of one room
            rooms::for_each_creature(state, room_of(state, leader), ::creature::type::cicada, [&](dcon::creature_id o) {
                if (circle_leader(state, o) == leader)
                    f(o);
            });
        }

        int32_t circle_count(sys::game_state const& state, dcon::creature_id leader) {
            int32_t n = 0;
            for_each_member(state, leader, [&](dcon::creature_id) { ++n; });
            return n;
        }

        // CicadaAI.AddToCircle
        void add_to_circle(sys::game_state& state, dcon::creature_id c, dcon::creature_id leader) {
            auto& w = state.world;
            uint32_t slot = 0;
            for_each_member(state, leader, [&](dcon::creature_id o) { slot = std::max(slot, w.cicada_data_get_circle_slot(creature::cicada_data(w, o)) + 1); });
            w.cicada_data_set_circle_leader(creature::cicada_data(w, c), uint16_t(leader.index() + 1));
            w.cicada_data_set_circle_slot(creature::cicada_data(w, c), slot);
        }

        // CicadaAI.RemoveFromCircle (CircleGroup.RemoveCicada): a leaving
        // leader hands the group to the next one in it
        void remove_from_circle(sys::game_state& state, dcon::creature_id c) {
            auto& w = state.world;
            auto const leader = circle_leader(state, c);
            if (!leader)
                return;
            w.cicada_data_set_circle_leader(creature::cicada_data(w, c), 0);
            if (leader == c) {
                dcon::creature_id next;
                for_each_member(state, c, [&](dcon::creature_id o) {
                    if (!next || w.cicada_data_get_circle_slot(creature::cicada_data(w, o)) < w.cicada_data_get_circle_slot(creature::cicada_data(w, next)))
                        next = o;
                });
                if (next) {
                    w.cicada_data_set_circle(creature::cicada_data(w, next), w.cicada_data_get_circle(creature::cicada_data(w, c)));
                    for_each_member(state, c, [&](dcon::creature_id o) { w.cicada_data_set_circle_leader(creature::cicada_data(w, o), uint16_t(next.index() + 1)); });
                }
            }
            w.cicada_data_set_no_circle_group_counter(creature::cicada_data(w, c), w.creature_get_random(c).range(10, 140));
        }

        // CircleGroup constructor
        void start_circle(sys::game_state& state, dcon::creature_id c, room_view const& room, vec2 center) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            circle_group g;
            g.center = center;
            g.radius = mathf::lerp(40.f, float(proximity(state, room, center) - 3) * 15.f, rng.value());
            g.rotation_dir = rng.value() < 0.5f ? -1.f : 1.f;
            g.rotation = rng.value();
            w.cicada_data_set_circle(creature::cicada_data(w, c), g);
            w.cicada_data_set_circle_leader(creature::cicada_data(w, c), uint16_t(c.index() + 1));
            w.cicada_data_set_circle_slot(creature::cicada_data(w, c), 0);
        }

        // CircleGroup.Update, run by the group's first cicada
        void circle_update(sys::game_state& state, dcon::creature_id leader, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(leader);
            auto& g = w.cicada_data_get_circle(creature::cicada_data(w, leader));
            if (rng.value() < 0.00066666666f)
                g.rotation_dir = -g.rotation_dir;
            ++g.life_time;
            if (g.life_time > 300 && rng.value() < 0.0023809525f)
                g.slated_for_deletion = 1;
            g.radius += mathf::lerp(-2.f, 2.f, rng.value());
            g.radius = mathf::clamp(g.radius, 40.f, float(proximity(state, room, g.center) - 3) * 15.f);
            g.rotation += 0.011111111f * g.rotation_dir * 120.f / g.radius;
            if (g.rotation < 0.f)
                g.rotation += 1.f;
            else if (g.rotation > 1.f)
                g.rotation -= 1.f;
            g.center += custom::deg_to_vec(rng.value() * 360.f) * rng.value() * 3.f;
        }

        // CircleGroup.MyPos
        vec2 circle_pos(sys::game_state const& state, dcon::creature_id c) {
            auto const& w = state.world;
            auto const leader = circle_leader(state, c);
            auto const& g = w.cicada_data_get_circle(creature::cicada_data(w, leader));
            int32_t index = 0, count = 0;
            for_each_member(state, leader, [&](dcon::creature_id o) {
                ++count;
                if (w.cicada_data_get_circle_slot(creature::cicada_data(w, o)) < w.cicada_data_get_circle_slot(creature::cicada_data(w, c)))
                    ++index;
            });
            return g.center + custom::deg_to_vec((g.rotation + float(index) / float(std::max(count, 1))) * 360.f) * g.radius;
        }

        // CicadaAI.InviteToDance
        bool invite_to_dance(sys::game_state const& state, dcon::creature_id c) {
            auto const& w = state.world;
            return w.cicada_data_get_no_circle_group_counter(creature::cicada_data(w, c)) == 0 && ::cicada::behavior(w.creature_get_behavior(c)) == behavior::idle && !circle_leader(state, c);
        }

        // the other cicadas of the room it can see (its tracker's cicadas)
        template<typename F>
        void for_each_seen_cicada(sys::game_state const& state, dcon::creature_id c, room_view const& room, F&& f) {
            rooms::for_each_creature(state, room.id, ::creature::type::cicada, [&](dcon::creature_id o) {
                if (o != c && visual_contact(state, c, room, pos(state, o, 0)))
                    f(o);
            });
        }

        // CicadaAI.Update
        void ai_update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            w.cicada_data_set_focus_player(creature::cicada_data(w, c), 0);
            if (w.cicada_data_get_no_circle_group_counter(creature::cicada_data(w, c)) > 0)
                --w.cicada_data_get_no_circle_group_counter(creature::cicada_data(w, c));
            tracker_update(state, c, room);
            vec2 const main = pos(state, c, 0);
            // the panic after being let go: away from the slugcat
            if (auto const panic = player_of(state, w.cicada_data_get_panic_flee_player(creature::cicada_data(w, c)))) {
                vec2 const theirs = w.body_chunk_get_pos(player::body_chunk(state, panic, 0));
                w.cicada_data_set_swoosh_to_pos(creature::cicada_data(w, c), main - custom::dir_vec(main, theirs) * 100.f);
                if (!custom::dist_less(main, theirs, 300.f) || w.body_chunk_get_contact_point(chunk(state, c, 0)) != ivec2{0, 0})
                    w.cicada_data_set_panic_flee_player(creature::cicada_data(w, c), 0);
                return;
            }
            w.cicada_data_set_panic_flee_player(creature::cicada_data(w, c), 0);

            // UtilityComparer: threat (weight 1), prey (0.5), rain (1), stuck
            // (1), none smoothed
            auto const prey = tracked(state, c);
            relationship rel;
            if (prey && player_here(state, prey, room.id))
                rel = player_relationship(state, c, prey);
            float const threat = rel.type == relationship::kind::afraid ? threat_utility(state, c, rel.intensity) : 0.f;
            float const hunt = rel.type == relationship::kind::antagonizes ? prey_utility(state, c, rel.intensity) * 0.5f : 0.f;
            float const stuck = stuck_utility(state, c);
            float utility = threat;
            behavior b = behavior::flee;
            if (hunt > utility) {
                utility = hunt;
                b = behavior::antagonize;
            }
            if (float const rain = dens::rain_utility(state, c, room.id); rain > utility) {
                utility = rain;
                b = behavior::escape_rain;
            }
            if (stuck > utility) {
                utility = stuck;
                b = behavior::get_unstuck;
            }
            if (utility < 0.1f)
                b = behavior::idle;
            w.creature_set_behavior(c, uint8_t(b));
            w.creature_set_current_utility(c, utility);
            w.cicada_data_set_swoosh_to_pos(creature::cicada_data(w, c), std::nullopt);
            if (circle_leader(state, c) && b != behavior::idle)
                remove_from_circle(state, c);
            stuck_update(state, c, room, at_sit_destination(state, c, room));

            ivec2& dest = w.creature_get_destination(c);
            switch (b) {
            case behavior::idle: {
                // (CicadaA heads for its den, CicadaB for a swarm room: there
                // are none here)
                if (auto const leader = circle_leader(state, c)) {
                    if (circle_count(state, leader) < 2) {
                        remove_from_circle(state, c);
                        return;
                    }
                    vec2 const swoosh = circle_pos(state, c);
                    w.cicada_data_set_swoosh_to_pos(creature::cicada_data(w, c), swoosh);
                    if (leader == c)
                        circle_update(state, c, room);
                    auto const& g = w.cicada_data_get_circle(creature::cicada_data(w, leader));
                    if ((g.slated_for_deletion && rng.value() < 0.00625f) || w.body_chunk_get_contact_point(chunk(state, c, 0)) != ivec2{0, 0}
                    || (float(circle_count(state, leader)) * 40.f > 6.2831855f * g.radius && rng.value() < 0.00625f) || rng.value() < 0.002173913f
                    || !visual_contact(state, c, room, swoosh)) {
                        remove_from_circle(state, c);
                        return;
                    }
                    break;
                }
                ivec2& spot = w.cicada_data_get_idle_sit_spot(creature::cicada_data(w, c));
                // (vanilla rolls one tile a tick, heading for each meanwhile:
                // here a few in one tick, so paths aren't asked for to
                // tiles it can't sit at)
                for (int k = 0; k < 16 && !climbable(state, room, spot); ++k)
                    spot = random_tile(rng, room);
                dest = spot;
                int32_t& counter = w.cicada_data_get_idle_sit_counter(creature::cicada_data(w, c));
                --counter;
                if (counter < 1 || pathing::narrow_space(room, spot)) {
                    counter = rng.range(0, rng.range(0, 650));
                    w.creature_set_forbidden_idle_pos(c, spot);
                }
                if (spot == w.creature_get_forbidden_idle_pos(c)) {
                    ivec2 const t = random_tile(rng, room);
                    if (climbable(state, room, t) && pathing::accessible(room, t, pathing::cicada)
                    && pathing::distance(state, pathing::field_to(state, room.id, dest, pathing::cicada), t) != pathing::unreachable
                    && (rng.value() < 0.3f || visual_contact(state, c, room, room_view::middle_of_tile(t))))
                        spot = t;
                }
                // asking another cicada to dance in a circle
                int32_t const floor = pathing::floor_altitude(room, here(state, c));
                if (rng.value() < 0.5f && w.cicada_data_get_no_circle_group_counter(creature::cicada_data(w, c)) == 0 && !circle_leader(state, c)
                && proximity(state, room, main) > 6 && floor < 12 && floor > 6) {
                    std::vector<dcon::creature_id> seen;
                    for_each_seen_cicada(state, c, room, [&](dcon::creature_id o) { seen.push_back(o); });
                    // Tracker.GetRep(Random.Range(0, CreaturesCount)): the slugcat is one of them
                    int32_t const pick = rng.range(0, int32_t(seen.size()) + (prey ? 1 : 0));
                    if (pick < int32_t(seen.size())) {
                        auto const other = seen[size_t(pick)];
                        if (invite_to_dance(state, other)) {
                            start_circle(state, c, room, main);
                            add_to_circle(state, other, c);
                            return;
                        }
                        if (auto const theirs = circle_leader(state, other)) {
                            add_to_circle(state, c, theirs);
                            return;
                        }
                    }
                }
                break;
            }
            case behavior::flee:
                dest = flee_to(state, c, room, w.body_chunk_get_pos(player::body_chunk(state, prey, 0)));
                w.cicada_data_set_focus_player(creature::cicada_data(w, c), player_ref(prey));
                return;
            case behavior::antagonize: {
                w.cicada_data_set_focus_player(creature::cicada_data(w, c), player_ref(prey));
                int32_t& method_counter = w.cicada_data_get_antagonize_method_counter(creature::cicada_data(w, c));
                ++method_counter;
                bool const sees = w.creature_get_sees_player(c) != 0;
                auto const focus_main = player::body_chunk(state, prey, 0);
                vec2 const focus_pos = w.body_chunk_get_pos(focus_main);
                if (w.cicada_data_get_antagonize_method(creature::cicada_data(w, c))) {
                    if ((rng.value() < 0.0016666667f && method_counter > 800)
                    || (rng.value() < 0.1f && sees && custom::dist_less(focus_pos, main, 50.f) && method_counter > 120)) {
                        method_counter = 0;
                        w.cicada_data_set_antagonize_method(creature::cicada_data(w, c), 0);
                    }
                } else if (rng.value() < 0.0125f && method_counter > 80) {
                    method_counter = 0;
                    w.cicada_data_set_antagonize_method(creature::cicada_data(w, c), 1);
                }
                if (sees && (w.cicada_data_get_antagonize_method(creature::cicada_data(w, c)) != 0) == (w.creature_get_payload(c).cicada.gender != 0)) {
                    // a spot near where it is heading, in sight of it, clear of the other cicadas
                    vec2 v = room_view::middle_of_tile(dest);
                    vec2 const v2 = focus_pos + w.body_chunk_get_vel(focus_main) * 4.5f;
                    float dst = float_max;
                    if (custom::dist_less(v2, v, 120.f) && visual_contact(state, c, room, v))
                        dst = rw::distance(main, v);
                    for (int i = 0; i < 5; ++i) {
                        vec2 v3 = v2 + custom::deg_to_vec(mathf::lerp(-75.f, 75.f, rng.value())) * 80.f;
                        v3 = rw::lerp(v3, v2 + custom::dir_vec(v2, main) * 80.f, 0.8f * mathf::inverse_lerp(400.f, 30.f, rw::distance(v3, main)));
                        rooms::for_each_creature(state, room.id, ::creature::type::cicada, [&](dcon::creature_id o) {
                            if (o == c)
                                return;
                            vec2 const theirs = room_view::middle_of_tile(pos(state, o, 0));
                            if (custom::dist_less(v3, theirs, 100.f))
                                v3 += custom::dir_vec(theirs, v3) * 50.f;
                        });
                        if (custom::dist_less(main, v3, dst) && room.visual_contact(v2, v3)) {
                            v = v3;
                            dst = rw::distance(main, v3);
                        }
                    }
                    dest = clamp_to_room(room, room_view::get_tile_position(v));
                    if (!charging(state, c) && rng.value() < 0.033333335f && custom::dist_less(main, v, 40.f) && custom::dist_less(v2, v, 120.f)
                    && proximity(state, room, main) > 1 && visual_contact(state, c, room, v) && room.visual_contact(v, v2))
                        charge(state, c, v2);
                } else {
                    dest = clamp_to_room(room, room_view::get_tile_position(w.creature_get_last_seen_pos(c)));
                }
                // the slugcat can't grab one that has been pestering it a while
                if (w.creature_get_payload(c).cicada.flying && w.cicada_data_get_seen_age(creature::cicada_data(w, c)) > 100 && visual_contact(state, c, room, focus_pos)) {
                    w.creature_get_payload(c).cicada.cant_pick_up_counter = 5;
                    w.creature_get_payload(c).cicada.cant_pick_up_player = player_ref(prey);
                    return;
                }
                break;
            }
            case behavior::get_unstuck:
                dest = w.cicada_data_get_stuck(creature::cicada_data(w, c)).unstuck_goal;
                break;
            case behavior::escape_rain:
                // the den (DenFinder, dens.hpp)
                if (auto const t = dens::home_tile(state, c, room))
                    dest = *t;
                break;
            default:
                break;
            }
        }

        // CicadaPather.FollowPath from tile `from` (clamped into the room).
        // Vanilla takes the cheapest connection out of the tile even at the
        // goal, so a flyer keeps steering about it: at the goal the step is
        // to the goal itself
        std::optional<ivec2> follow_path(sys::game_state const& state, room_view const& room, pathing::field_id f, ivec2 from) {
            from = clamp_to_room(room, from);
            if (!pathing::accessible(room, from, pathing::cicada))
                return std::nullopt;
            if (auto const step = pathing::next_step(state, f, from))
                return step;
            if (pathing::distance(state, f, from) == 0)
                return from;
            return std::nullopt;
        }

        // Cicada.Swim
        void swim(sys::game_state& state, dcon::creature_id c) {
            vel(state, c, 0).y += 0.5f;
        }

        // Cicada.GrabbedByPlayer: flapping to hold the slugcat up, tiring,
        // struggling when the slugcat stands still
        void grabbed_by_player(sys::game_state& state, dcon::creature_id c, dcon::player_id p) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            float& stamina = w.creature_get_payload(c).cicada.stamina;
            bool const lifting = w.creature_get_payload(c).cicada.currently_lifting_player != 0;
            w.creature_get_payload(c).cicada.flying = stamina > 0.33333334f ? 1 : 0;
            w.creature_get_payload(c).cicada.sticky_player = 0;
            if (lifting)
                stamina -= 1.f / (w.creature_get_payload(c).cicada.gender ? 190.f : 120.f);
            stamina = mathf::clamp(stamina, 0.f, 1.f);
            vel(state, c, 0) *= mathf::lerp(1.f, 0.98f, stamina);
            vel(state, c, 1) *= mathf::lerp(1.f, 0.94f, stamina);
            vel(state, c, 0).y += 1.2f * stamina;
            float& boost = w.creature_get_payload(c).cicada.player_jump_boost;
            vel(state, c, 1).y += boost + 1.8f * stamina;
            // (SocialMemory like dropping towards -1: it only matters next cycle)
            auto const& in = w.player_get_input(p);
            if (!lifting) {
                if (stamina < 0.33333334f)
                    stamina += 1.f / ((in[0].x == 0 && in[0].y == 0) ? 600.f : 900.f);
                else if (stamina < 0.6666667f)
                    stamina += 0.004f;
                else
                    stamina += 0.033333335f;
            }
            stamina = mathf::clamp(stamina, 0.f, 1.f);
            float& struggle = w.creature_get_payload(c).cicada.struggle_against_player;
            if (in[0].x == 0 && in[0].y == 0 && stamina == 1.f) {
                struggle = mathf::min(1.f, struggle + rng.value() / 30.f);
                vel(state, c, 0) += custom::deg_to_vec(rng.value() * 360.f) * rng.value() * struggle * 6.f;
            } else {
                struggle = 0.f;
            }
            if (in[0].jmp) {
                if (lifting)
                    boost = mathf::max(0.f, boost * 0.9f - 0.033333335f);
                else if (!in[1].jmp)
                    boost = 1.f;
            } else {
                boost = 0.f;
            }
            if (lifting) {
                vel(state, c, 0).x += float(in[0].x) * stamina * 0.3f;
                vel(state, c, 1).x += float(in[0].x) * stamina * 0.4f;
            }
            w.creature_get_payload(c).cicada.no_sticky_counter = 200;
            w.cicada_data_set_panic_flee_player(creature::cicada_data(w, c), player_ref(p));
        }

        // Cicada.Act
        void act(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            ai_update(state, c, room);
            float& stamina = w.creature_get_payload(c).cicada.stamina;
            if (!w.creature_get_payload(c).cicada.sticky_player)
                stamina = mathf::min(stamina + 0.014285714f, 1.f);
            ivec2 const dest = w.creature_get_destination(c);
            bool const sit = at_sit_destination(state, c, room);
            std::optional<ivec2> conn;
            ivec2 const start = clamp_to_room(room, here(state, c));
            // the path is only asked for when it is followed
            pathing::field_id f = 0;
            if ((w.creature_get_payload(c).cicada.flying || !sit) && w.creature_get_payload(c).cicada.charge_counter == 0 && !w.cicada_data_get_swoosh_to_pos(creature::cicada_data(w, c))) {
                f = pathing::field_to(state, room.id, dest, pathing::cicada);
                conn = follow_path(state, room, f, start);
            }
            float& power = w.creature_get_payload(c).cicada.flying_power;
            float const g = w.physical_object_get_g(body(state, c));

            if (w.creature_get_payload(c).cicada.flying) {
                // the flutter: a sine lift, damping, and holding itself up
                float& sin_counter = w.creature_get_payload(c).cicada.sin_counter;
                sin_counter += 1.f / mathf::lerp(45.f, 85.f, rng.value());
                if (sin_counter > 1.f)
                    sin_counter -= 1.f;
                float const fs = power * stamina;
                vel(state, c, 0).y += std::sin(sin_counter * mathf::pi * 2.f) * 0.05f * fs;
                vel(state, c, 1).y += std::sin(sin_counter * mathf::pi * 2.f) * 0.05f * fs;
                vel(state, c, 0) *= mathf::lerp(1.f, 0.98f, fs);
                vel(state, c, 1) *= mathf::lerp(1.f, 0.94f, fs);
                vel(state, c, 0).y += 0.8f * fs;
                vel(state, c, 1).y += (sit ? 0.5f : 1.2f) * fs;
                // somewhere to land: in a narrow space, against the terrain or at the sit spot
                bool want_land = false;
                if (!conn || climbable(state, room, *conn) || climbable(state, room, room_view::get_tile_position(pos(state, c, 0)))) {
                    if (pathing::narrow_space(room, room_view::get_tile_position(pos(state, c, 0))))
                        want_land = true;
                    else if (proximity(state, room, pos(state, c, 0)) == 1 && proximity(state, room, pos(state, c, 1)) == 1 && (!conn || proximity(state, room, *conn) == 1))
                        want_land = true;
                    else if (sit)
                        want_land = true;
                }
                bool keep_flying = true;
                if (want_land) {
                    ivec2 const d = custom::four_directions[rng.range(0, 4)];
                    if (solid(room, here(state, c) + d)) {
                        vel(state, c, 0) += rw::to_vec2(d) * 3.f;
                        vel(state, c, 1) += rw::to_vec2(d) * 3.f;
                        land(state, c);
                    } else if (room.get_tile(pos(state, c, 0)).vertical_beam()) {
                        land(state, c);
                    } else if (conn && conn->y < here(state, c).y) {
                        keep_flying = false;
                    }
                } else {
                    for (uint32_t l = 0; l < 2; ++l) {
                        ivec2 const cp = w.body_chunk_get_contact_point(chunk(state, c, l));
                        if (cp != ivec2{0, 0})
                            vel(state, c, l) -= rw::to_vec2(cp) * 8.f * fs * rng.value();
                    }
                }
                if (auto const sp = player_of(state, w.creature_get_payload(c).cicada.sticky_player)) {
                    // clinging to a slugcat with its tentacles, flapping away with it
                    auto const sc = player::body_chunk(state, sp, w.creature_get_payload(c).cicada.sticky_chunk);
                    vec2 const charge_dir = w.creature_get_payload(c).cicada.charge_dir;
                    vel(state, c, 1) += charge_dir * 0.4f;
                    vec2 const b = custom::deg_to_vec(rng.value() * 360.f) * 1.5f;
                    vel(state, c, 0) += b;
                    w.body_chunk_get_vel(sc) += b;
                    auto const main = chunk(state, c, 0);
                    vec2& mp = w.body_chunk_get_pos(main);
                    vec2& spp = w.body_chunk_get_pos(sc);
                    float const rads = w.body_chunk_get_rad(main) + w.body_chunk_get_rad(sc);
                    if (custom::dist_less(mp, spp, rads + 35.f) && !w.player_get_entering_shortcut(sp).has_value() && ::cicada::behavior(w.creature_get_behavior(c)) == behavior::antagonize
                    && player_here(state, sp, room.id) && spp.y > -20.f && w.creature_get_payload(c).cicada.flying && rng.value() > 0.004761905f && holder(state, c) != sp) {
                        float const dist = rw::distance(mp, spp);
                        vec2 const a = custom::dir_vec(mp, spp);
                        float const want = rads + 15.f;
                        constexpr float k = 0.65f;
                        float const ratio = w.body_chunk_get_mass(sc) / (w.body_chunk_get_mass(sc) + w.body_chunk_get_mass(main));
                        mp -= a * (want - dist) * ratio * k;
                        w.body_chunk_get_vel(main) -= a * (want - dist) * ratio * k;
                        spp += a * (want - dist) * (1.f - ratio) * k;
                        w.body_chunk_get_vel(sc) += a * (want - dist) * (1.f - ratio) * k;
                        stamina = mathf::clamp(stamina - 0.008333334f, 0.f, 1.f);
                        if (stamina < 0.2f)
                            w.creature_get_payload(c).cicada.sticky_player = 0;
                    } else {
                        if (custom::dist_less(mp, spp, rads + 45.f))
                            for (uint32_t m = 0; m < 2; ++m)
                                vel(state, c, m) += custom::dir_vec(spp, pos(state, c, m)) * 4.f;
                        w.creature_get_payload(c).cicada.sticky_player = 0;
                    }
                    if (!w.creature_get_payload(c).cicada.sticky_player)
                        state.play_sound(sound::sound_id::cicada_tentacles_detatch, room.id, pos(state, c, 0));
                } else {
                    power = mathf::lerp(power, keep_flying ? 1.f : 0.f, 0.1f);
                }
            } else {
                power = mathf::lerp(power, 0.f, 0.05f);
                if (climbable(state, room, room_view::get_tile_position(pos(state, c, 0)))) {
                    // holding on with its tentacles
                    for (uint32_t i = 0; i < 2; ++i) {
                        vel(state, c, i) *= 0.8f;
                        vel(state, c, i).y += g;
                    }
                } else {
                    w.creature_get_payload(c).cicada.flying = 1;
                }
            }
            if (sit) {
                vec2 const head = body_sit_pos_offset(state, c, room, dest);
                vel(state, c, 1) += rw::clamp_magnitude(body_sit_pos_offset(state, c, room, find_body_sit_pos(state, c, room, dest)) - pos(state, c, 1), 10.f) / 10.f * 0.5f;
                vel(state, c, 0) += rw::clamp_magnitude(head - pos(state, c, 0), 10.f) / 10.f * 0.5f;
            }
            if (conn) {
                if (conn->x < start.x)
                    w.creature_get_payload(c).cicada.flip_h = -1;
                else if (conn->x > start.x)
                    w.creature_get_payload(c).cicada.flip_h = 1;
                physics::set_go_through_floors(state, body(state, c), conn->y < start.y);
                // (shortcuts and NPC transportation aren't steps of the fields)
                if (w.creature_get_payload(c).cicada.flying) {
                    // towards the next steps, slower where it would be
                    // moving away from the terrain. (sic) vanilla asks for the
                    // step after the next one three times over, not three
                    // steps further
                    vec2 v = room_view::middle_of_tile(*conn);
                    int32_t n = 1;
                    for (int k = 0; k < 3; ++k) {
                        auto const next = follow_path(state, room, f, *conn);
                        if (!next)
                            break;
                        v += room_view::middle_of_tile(*next);
                        ++n;
                    }
                    v /= float(n);
                    vec2 const main = pos(state, c, 0);
                    float k = float(proximity(state, room, main))
                        / mathf::max(float(proximity(state, room, main + custom::dir_vec(main, v) * mathf::clamp(rw::magnitude(vel(state, c, 0)) * 5.f, 5.f, 15.f))), 1.f);
                    // off the room AImap.getTerrainProximity is -1, which in
                    // vanilla turns the steering around and sends it further
                    // out: here it steers back in at full strength
                    if (proximity(state, room, main) < 0)
                        k = 1.f;
                    k = mathf::pow(mathf::min(k, 1.f), 3.f);
                    vec2 const goal = room_view::middle_of_tile(dest);
                    if (want_to_sit_down_at_destination(state, c, room) && custom::dist_less(goal, main, 200.f) && visual_contact(state, c, room, goal))
                        k *= mathf::lerp(0.2f, 1.f, mathf::inverse_lerp(0.f, 300.f, rw::distance(goal, main)));
                    float const fs = power * stamina;
                    vel(state, c, 0) += rw::clamp_magnitude(v - main, 40.f) / 40.f * 1.1f * k * fs;
                    vel(state, c, 1) += rw::clamp_magnitude(v - main, 40.f) / 40.f * 0.65f * k * fs;
                    return;
                }
                if (room.get_tile(*conn).t == world::terrain::slope)
                    take_off(state, c, room, custom::deg_to_vec(rng.value() * 360.f));
                if (climbable(state, room, *conn)) {
                    // crawling along the terrain
                    vel(state, c, 0) += custom::dir_vec(pos(state, c, 0), room_view::middle_of_tile(*conn)) * mathf::lerp(0.4f, 1.8f, stuck_utility(state, c));
                    return;
                }
                if (++w.creature_get_payload(c).cicada.wait_to_fly_counter > 30)
                    take_off(state, c, room, custom::dir_vec(pos(state, c, 0), room_view::middle_of_tile(*conn)));
                return;
            }
            int32_t& counter = w.creature_get_payload(c).cicada.charge_counter;
            if (counter > 0) {
                // the bump attack: a wind up, backing off, then a dash
                ++counter;
                vec2 const charge_dir = w.creature_get_payload(c).cicada.charge_dir;
                if (counter < 21) {
                    vel(state, c, 0) *= 0.8f;
                    vel(state, c, 1) *= 0.8f;
                    vel(state, c, 1) -= charge_dir * 0.8f;
                } else if (counter == 21) {
                    state.play_sound(sound::sound_id::cicada_wings_start_bump_attack, room.id, pos(state, c, 0));
                } else if (counter > 38) {
                    counter = 0;
                    vel(state, c, 0) *= 0.5f;
                    vel(state, c, 1) *= 0.5f;
                    state.play_sound(sound::sound_id::cicada_wings_exit_bump_attack, room.id, pos(state, c, 0));
                } else {
                    vel(state, c, 0) += charge_dir * 4.f;
                    vel(state, c, 1) *= rw::magnitude(vel(state, c, 0)) > 15.f ? 0.8f : 0.98f;
                }
                if (pathing::narrow_space(room, room_view::get_tile_position(pos(state, c, 0))))
                    counter = 0;
                w.creature_get_payload(c).cicada.flying = 1;
                return;
            }
            if (auto const swoosh = w.cicada_data_get_swoosh_to_pos(creature::cicada_data(w, c))) {
                vec2 const main = pos(state, c, 0);
                float const fs = power * stamina;
                vel(state, c, 0) += rw::clamp_magnitude(*swoosh - main, 20.f) / 20.f * 1.8f * fs;
                vel(state, c, 1) += rw::clamp_magnitude(*swoosh - main, 20.f) / 20.f * 0.8f * fs;
                vel(state, c, 1) *= 0.9f;
                vel(state, c, 1).y -= 0.2f;
                w.creature_get_payload(c).cicada.flying = 1;
            }
        }

        struct impact_ctx {
            sys::game_state* state;
            dcon::creature_id c;
        };

        // Cicada.TerrainImpact: the bump attack bounces off walls (or knocks
        // itself out on a hard hit); landings make a sound
        void terrain_impact(void* user, dcon::physical_object_id, uint32_t, ivec2 direction, float speed, bool first_contact) {
            auto& x = *static_cast<impact_ctx*>(user);
            auto& state = *x.state;
            auto& w = state.world;
            auto const c = x.c;
            vec2 const main = pos(state, c, 0);
            if (charging(state, c)) {
                if (first_contact)
                    state.play_sound(sound::sound_id::cicada_bump_attack_hit_terrain, room_of(state, c), main);
                if (speed < 20.f) {
                    vec2& v = vel(state, c, 0);
                    if (direction.y < 0)
                        v.y = std::fabs(v.y);
                    else if (direction.y > 0)
                        v.y = -std::fabs(v.y);
                    if (direction.x < 0)
                        v.x = std::fabs(v.x);
                    else if (direction.x > 0)
                        v.x = -std::fabs(v.x);
                    if (first_contact) {
                        w.creature_get_payload(c).cicada.charge_counter = 25;
                        // (Room.ScreenMovement: no screen shake here)
                        v -= rw::to_vec2(direction);
                    }
                    w.creature_get_payload(c).cicada.charge_dir = rw::normalized(v);
                    return;
                }
                stun(state, c, 20);
            } else if (speed > 1.5f && first_contact) {
                state.play_sound(speed < 8.f ? sound::sound_id::cicada_light_terrain_impact : sound::sound_id::cicada_heavy_terrain_impact, room_of(state, c), main);
            }
        }



        // Cicada.Collide with a slugcat: myChunk `i`, their chunk `j`
        void collide_with_player(sys::game_state& state, dcon::creature_id c, uint32_t i, dcon::player_id p, uint32_t j) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            auto const pc = player::body_chunk(state, p, j);
            if (charging(state, c)) {
                vec2 v = rw::lerp(w.creature_get_payload(c).cicada.charge_dir * 6.f, vel(state, c, i) * 0.5f, 0.5f);
                if (v.y < 0.f)
                    v.y *= 0.5f;
                w.body_chunk_get_vel(pc) += v / w.body_chunk_get_mass(pc);
                w.creature_get_payload(c).cicada.charge_counter = 0;
                stun(state, c, 10);
                player::stun(state, p, 4);
                state.play_sound(sound::sound_id::cicada_bump_attack_hit_player, room_of(state, c), pos(state, c, 0));
                return;
            }
            if (i == 0 && w.creature_get_payload(c).cicada.no_sticky_counter == 0 && rng.value() < 0.5f && !w.creature_get_payload(c).cicada.sticky_player
            && ::cicada::behavior(w.creature_get_behavior(c)) == behavior::antagonize && tracked(state, c) == p) {
                w.creature_get_payload(c).cicada.sticky_player = player_ref(p);
                w.creature_get_payload(c).cicada.sticky_chunk = uint8_t(j);
                w.creature_get_payload(c).cicada.charge_dir = custom::deg_to_vec(mathf::lerp(-70.f, 70.f, rng.value()));
                state.play_sound(sound::sound_id::cicada_tentacles_grab_player, room_of(state, c), pos(state, c, 0));
            }
        }

        // Cicada.Collide with another cicada, while charging
        void collide_cicada(sys::game_state& state, dcon::creature_id c, uint32_t i, dcon::creature_id other, uint32_t j) {
            auto& w = state.world;
            if (!consious(state, c) || !charging(state, c))
                return;
            auto const oc = chunk(state, other, j);
            vec2 v = rw::lerp(w.creature_get_payload(c).cicada.charge_dir * 6.f, vel(state, c, i) * 0.5f, 0.5f);
            if (v.y < 0.f)
                v.y *= 0.5f;
            w.body_chunk_get_vel(oc) += v / w.body_chunk_get_mass(oc);
            w.creature_get_payload(c).cicada.charge_counter = 25;
            w.creature_get_payload(c).cicada.charge_dir = custom::dir_vec(w.body_chunk_get_pos(oc), pos(state, c, i));
            if (!charging(state, other))
                stun(state, other, 20);
            state.play_sound(sound::sound_id::cicada_bump_attack_hit_npc, room_of(state, c), pos(state, c, 0));
        }
    }

    // Cicada.Update
    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto& rng = w.creature_get_random(c);
        // the slugcat holding it: let go when it can't (it carries it
        // through shortcuts, see follow_player)
        if (auto const p = holder(state, c); w.creature_get_payload(c).cicada.held_by && (!p || !player_here(state, p, room.id)) && !(p && w.player_get_in_shortcut(p)))
            release(state, c);
        // Creature.Update + PhysicalObject.Update
        creature::stun_countdown(state, c);
        impact_ctx ctx{&state, c};
        physics::update_object(state, room, body(state, c), &terrain_impact, &ctx);

        if (w.creature_get_payload(c).cicada.no_sticky_counter > 0)
            --w.creature_get_payload(c).cicada.no_sticky_counter;
        if (w.creature_get_payload(c).cicada.charge_counter == 0 && w.creature_get_payload(c).cicada.cant_pick_up_counter > 0)
            --w.creature_get_payload(c).cicada.cant_pick_up_counter;
        float const health = w.creature_get_health(c);
        if (health < 0.5f && rng.value() > health && rng.value() < 0.33333334f) {
            stun(state, c, 4);
            if (health <= 0.f && rng.value() < 0.25f)
                die(state, c);
        }
        if (consious(state, c)) {
            if (auto const p = holder(state, c))
                grabbed_by_player(state, c, p);
            else if (physics::object_submersion(state, room, body(state, c)) > 0.5f)
                swim(state, c);
            else
                act(state, c, room);
        } else {
            w.creature_get_payload(c).cicada.sticky_player = 0;
            w.creature_get_payload(c).cicada.cant_pick_up_counter = 0;
            w.creature_get_payload(c).cicada.stamina = 0.f;
        }
        if (!w.creature_get_payload(c).cicada.flying)
            w.creature_get_payload(c).cicada.cant_pick_up_counter = 0;
        // (no grasps of its own: the flies and leeches it would carry don't exist here)
    }

    bool charging(sys::game_state const& state, dcon::creature_id c) noexcept {
        return state.world.creature_get_payload(c).cicada.charge_counter > 21;
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 at, bool gender) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::cicada);
        rw::rng rng{state.random.next() | 1u};
        auto const view = world::get_room_view(state, room);
        physics::object_desc desc;
        desc.air_friction = 0.999f;
        desc.gravity = 0.9f;
        desc.bounce = 0.1f;
        desc.surface_friction = 0.4f;
        desc.water_friction = 0.96f;
        desc.buoyancy = 0.95f;
        auto const o = physics::create_object(state, room, desc);
        // never inside terrain
        if (view.get_tile(at).solid()) {
            ivec2 const t0 = room_view::get_tile_position(at);
            for (int r = 1; r < 8 && view.get_tile(at).solid(); ++r)
                for (int dy = -r; dy <= r; ++dy)
                    for (int dx = -r; dx <= r; ++dx)
                        if (view.get_tile(at).solid() && !view.get_tile(t0 + ivec2{dx, dy}).solid())
                            at = room_view::middle_of_tile(t0 + ivec2{dx, dy});
        }
        float const mass = gender ? 0.65f : 0.55f;
        auto const c0 = physics::add_chunk(state, o, at, 7.5f, mass / 2.f);
        auto const c1 = physics::add_chunk(state, o, at + vec2{0.f, -1.f}, 7.f, mass / 2.f);
        physics::add_connection(state, o, c0, c1, 14.f, physics::connection_type::normal, 1.f, 0.5f);
        w.try_create_creature_body(c, o);
        // Cicada.GenerateIVars (vanilla seeds these from the creature's ID)
        individual_variations iv;
        iv.hue = custom::clamped_random_variation(0.55f, 0.1f, 0.5f, rng);
        float const fat = custom::clamped_random_variation(gender ? 0.6f : 0.4f, 0.1f, 0.5f, rng) * 2.f;
        iv.busted_wing = rng.value() < 0.125f ? rng.range(0, 4) : -1;
        iv.fatness = fat;
        iv.wing_sound_pitch = 1.f / fat + mathf::lerp(-0.1f, 0.1f, rng.value());
        iv.default_wing_deployment = rng.value();
        iv.tentacle_length = mathf::lerp(0.6f, 1.4f, rng.value());
        iv.tentacle_thickness = mathf::lerp(0.6f, 1.4f, rng.value());
        float const thin = rng.value();
        iv.wing_thickness = mathf::lerp(1.f, 0.4f, thin * rng.value());
        iv.wing_length = custom::clamped_random_variation(0.66667f, 0.3f, 0.2f, rng) * 1.5f;
        w.cicada_data_set_ivars(creature::cicada_data(w, c), iv);
        w.creature_get_payload(c).cicada.gender = gender ? 1 : 0;
        w.cicada_data_set_bravery(creature::cicada_data(w, c), rng.value());
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_get_payload(c).cicada.sin_counter = rng.value();
        w.creature_get_payload(c).cicada.flip_h = rng.value() < 0.5f ? -1 : 1;
        w.creature_get_payload(c).cicada.flying = 1;
        w.creature_get_payload(c).cicada.wait_to_fly_counter = 0;
        w.creature_get_payload(c).cicada.flying_power = 0.f;
        w.creature_get_payload(c).cicada.charge_counter = 0;
        w.creature_get_payload(c).cicada.charge_dir = vec2{0.f, 0.f};
        w.creature_get_payload(c).cicada.sticky_player = 0;
        w.creature_get_payload(c).cicada.sticky_chunk = 0;
        w.creature_get_payload(c).cicada.no_sticky_counter = 0;
        w.creature_get_payload(c).cicada.cant_pick_up_counter = 0;
        w.creature_get_payload(c).cicada.cant_pick_up_player = 0;
        w.cicada_data_set_sit_direction(creature::cicada_data(w, c), ivec2{0, 0});
        w.creature_get_payload(c).cicada.stamina = 1.f;
        w.creature_get_payload(c).cicada.struggle_against_player = 0.f;
        w.creature_get_payload(c).cicada.currently_lifting_player = 0;
        w.creature_get_payload(c).cicada.player_jump_boost = 0.f;
        w.creature_get_payload(c).cicada.held_by = 0;
        w.creature_get_payload(c).cicada.held_chunk = 0;
        w.cicada_data_set_temp_like(creature::cicada_data(w, c), 0.f);
        w.creature_set_behavior(c, uint8_t(behavior::idle));
        w.creature_set_current_utility(c, 0.f);
        w.cicada_data_set_idle_sit_counter(creature::cicada_data(w, c), 0);
        w.cicada_data_set_no_circle_group_counter(creature::cicada_data(w, c), 0);
        w.cicada_data_set_circle_leader(creature::cicada_data(w, c), 0);
        w.cicada_data_set_circle_slot(creature::cicada_data(w, c), 0);
        w.cicada_data_set_circle(creature::cicada_data(w, c), circle_group{});
        w.cicada_data_set_swoosh_to_pos(creature::cicada_data(w, c), std::nullopt);
        w.cicada_data_set_antagonize_method(creature::cicada_data(w, c), 0);
        w.cicada_data_set_antagonize_method_counter(creature::cicada_data(w, c), 0);
        w.cicada_data_set_panic_flee_player(creature::cicada_data(w, c), 0);
        w.cicada_data_set_focus_player(creature::cicada_data(w, c), 0);
        w.creature_set_seen_player(c, 0);
        w.creature_set_sees_player(c, 0);
        w.creature_set_ticks_since_seen(c, 0);
        w.cicada_data_set_seen_age(creature::cicada_data(w, c), 0);
        w.creature_set_last_seen_pos(c, at);
        stuck_tracker st;
        st.unstuck_goal = room_view::get_tile_position(at);
        w.cicada_data_set_stuck(creature::cicada_data(w, c), st);
        w.creature_set_random(c, rng);
        // CicadaAI.NewRoom: a random idle sit spot
        auto& r = w.creature_get_random(c);
        ivec2 const spot = random_tile(r, view);
        w.cicada_data_set_idle_sit_spot(creature::cicada_data(w, c), spot);
        w.creature_set_forbidden_idle_pos(c, ivec2{-1, -1});
        w.creature_set_destination(c, room_view::get_tile_position(at));
        cicada_graphics::reset(state, c);
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        release(state, c);
        remove_from_circle(state, c);
        creature::erase(state, c);
    }

    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::player_id p, dcon::room_id room) {
        // what holds it is kept apart by the carry
        return player_here(state, p, room) && holder(state, c) != p;
    }

    void collide_player(sys::game_state& state, dcon::creature_id c, uint32_t i, dcon::player_id p, uint32_t j) {
        if (consious(state, c))
            collide_with_player(state, c, i, p, j);
    }

    void collide_kin(sys::game_state& state, dcon::creature_id c, uint32_t i, dcon::creature_id other, uint32_t j) {
        collide_cicada(state, c, i, other, j);
        collide_cicada(state, other, j, c, i);
    }

    void violence(sys::game_state& state, dcon::creature_id c, uint32_t k, vec2 momentum, float damage, float stun_bonus) {
        auto& w = state.world;
        auto& rng = w.creature_get_random(c);
        // Creature.Violence
        auto const hit = chunk(state, c, k);
        w.body_chunk_get_vel(hit) += rw::clamp_magnitude(momentum / w.body_chunk_get_mass(hit), 10.f);
        float const dmg = damage / base_damage_resistance;
        float st = (damage * 30.f + stun_bonus) / base_stun_resistance;
        float& health = w.creature_get_health(c);
        st *= 1.5f + mathf::inverse_lerp(0.5f, 0.f, health) * rng.value();
        stun(state, c, int32_t(st));
        health -= dmg;
        // quickDeath
        if (rng.value() < -health || health < -1.f || (health < 0.f && rng.value() < 0.33f))
            die(state, c);
        // (SocialEventRecognizer: a hurt cicada doesn't hold it against the slugcat here)
    }

    dcon::creature_id held_by(sys::game_state const& state, dcon::player_id p) noexcept {
        dcon::creature_id found;
        if (!p)
            return found;
        auto const look = [&](dcon::creature_id c) {
            if (state.world.creature_get_payload(c).cicada.held_by == p.index() + 1)
                found = c;
        };
        // in the slugcat's room while the rooms update; anywhere otherwise
        // (a pipe moves the slugcat first, then what it carries)
        if (state.index.valid)
            rooms::for_each_creature(state, rooms::room_of(state, p), ::creature::type::cicada, look);
        else
            creature::for_each_of(state.world, creature::type::cicada, look);
        return found;
    }

    float lift_player_power(sys::game_state const& state, dcon::creature_id c) noexcept {
        auto const& w = state.world;
        return custom::s_curve(w.creature_get_payload(c).cicada.stamina, 0.15f) * (0.4f + w.creature_get_payload(c).cicada.player_jump_boost * 0.6f);
    }

    void set_currently_lifting_player(sys::game_state& state, dcon::creature_id c, bool lifting) noexcept {
        state.world.creature_get_payload(c).cicada.currently_lifting_player = lifting ? 1 : 0;
    }

    dcon::creature_id pickup_candidate(sys::game_state const& state, dcon::player_id p, room_view const& room, float& score) {
        auto const& w = state.world;
        vec2 const p0 = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
        int32_t const flip = w.player_get_flip_direction(p);
        dcon::creature_id result;
        score = float_max;
        rooms::for_each_creature(state, room.id, ::creature::type::cicada, [&](dcon::creature_id c) {
            if (w.creature_get_payload(c).cicada.held_by)
                return;
            // Player.Grabability: not while it charges, nor while it is
            // keeping its distance from this slugcat
            if (charging(state, c) || (w.creature_get_payload(c).cicada.cant_pick_up_counter != 0 && w.creature_get_payload(c).cicada.cant_pick_up_player == p.index() + 1))
                return;
            auto const c0 = chunk(state, c, 0);
            vec2 const cp = w.body_chunk_get_pos(c0);
            float const rad = w.body_chunk_get_rad(c0);
            if (!custom::dist_less(p0, cp, rad + 40.f))
                return;
            if (!custom::dist_less(p0, cp, rad + 20.f) && !room.visual_contact(p0, cp))
                return;
            float d = rw::distance(p0, cp);
            if ((cp.x < p0.x) == (flip < 0))
                d -= 10.f;
            if (d < score) {
                result = c;
                score = d;
            }
        });
        return result;
    }

    void grab(sys::game_state& state, dcon::player_id p, dcon::creature_id c) {
        auto& w = state.world;
        // SlugcatGrab: chunk 0 (TwoHands isn't dragged by its closest chunk)
        w.creature_get_payload(c).cicada.held_by = player_ref(p);
        w.creature_get_payload(c).cicada.held_chunk = 0;
        ++w.physical_object_get_grabbed_by_count(body(state, c));
        w.creature_get_payload(c).cicada.sticky_player = 0;
        remove_from_circle(state, c);
    }

    void release(sys::game_state& state, dcon::creature_id c) {
        auto& w = state.world;
        if (!w.creature_get_payload(c).cicada.held_by)
            return;
        uint8_t& n = w.physical_object_get_grabbed_by_count(body(state, c));
        if (n > 0)
            --n;
        w.creature_get_payload(c).cicada.held_by = 0;
        w.creature_get_payload(c).cicada.currently_lifting_player = 0;
    }

    void release_player(sys::game_state& state, dcon::creature_id c, dcon::player_id p) {
        auto& w = state.world;
        if (w.creature_get_payload(c).cicada.held_by == p.index() + 1)
            release(state, c);
        if (w.creature_get_payload(c).cicada.sticky_player == p.index() + 1)
            w.creature_get_payload(c).cicada.sticky_player = 0;
    }

    void release_player(sys::game_state& state, dcon::player_id p) {
        // whatever holds the slugcat or clings to it is in its room
        rooms::for_each_creature(state, rooms::room_of(state, p), creature::type::cicada, [&](dcon::creature_id c) { release_player(state, c, p); });
    }

    void carry_update(sys::game_state& state, dcon::player_id p) {
        auto& w = state.world;
        auto const c = held_by(state, p);
        if (!c)
            return;
        auto const main = player::body_chunk(state, p, 0);
        auto const gc = chunk(state, c, w.creature_get_payload(c).cicada.held_chunk);
        vec2& mp = w.body_chunk_get_pos(main);
        vec2& gp = w.body_chunk_get_pos(gc);
        vec2 const a = custom::dir_vec(mp, gp);
        float const dist = rw::distance(mp, gp);
        // a cicada hangs 30 above the hands
        constexpr float want = 30.f;
        float ratio = w.body_chunk_get_mass(gc) / (w.body_chunk_get_mass(main) + w.body_chunk_get_mass(gc));
        bool const entering = w.player_get_entering_shortcut(p).has_value();
        if (entering)
            ratio = 0.f;
        else if (object_mass(state, body(state, c)) < object_mass(state, player::body(state, p)))
            ratio /= 2.f;
        if (!entering || dist > want) {
            mp += a * (dist - want) * ratio;
            w.body_chunk_get_vel(main) += a * (dist - want) * ratio;
            gp -= a * (dist - want) * (1.f - ratio);
            w.body_chunk_get_vel(gc) -= a * (dist - want) * (1.f - ratio);
        }
        if (w.player_get_body_mode(p) == player::body_mode_index::climbing_on_beam && w.player_get_animation(p) != player::animation_index::beam_tip
        && w.player_get_animation(p) != player::animation_index::stand_on_beam) {
            auto const room = world::get_room_view(state, room_of(state, c));
            w.body_chunk_get_vel(gc).y += w.physical_object_get_g(body(state, c)) * (1.f - physics::submersion(state, room, gc)) * 0.75f;
        }
    }

    void follow_player(sys::game_state& state, dcon::player_id p, dcon::room_id room, bool spit_out) {
        auto& w = state.world;
        auto const c = held_by(state, p);
        if (!c)
            return;
        w.physical_object_set_room_from_object_location(body(state, c), room);
        if (spit_out) {
            // Cicada.SpitOutOfShortCut: out after the slugcat
            vec2 const at = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
            for (uint32_t i = 0; i < 2; ++i) {
                physics::hard_set_position(state, chunk(state, c, i), at + vec2{0.f, float(i) * -5.f});
                w.body_chunk_set_vel(chunk(state, c, i), w.body_chunk_get_vel(player::body_chunk(state, p, 0)));
            }
            cicada_graphics::reset(state, c);
        }
    }
}
