#include <cmath>
#include <vector>

#include "centipede.hpp"
#include "rooms.hpp"
#include "centipede_graphics.hpp"
#include "items.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "player.hpp"

namespace centipede {
    using rw::ivec2;
    using rw::vec2;
    using world::centipede_behavior;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // CreatureTemplate: Centipede
        constexpr float base_damage_resistance = 0.9f;
        constexpr float base_stun_resistance = 0.6f;
        constexpr float visual_radius = 900.f;

        float object_mass(sys::game_state const& state, dcon::physical_object_id o) {
            float m = 0.f;
            state.world.physical_object_for_each_chunk_ownership_as_owner(o, [&](dcon::chunk_ownership_id co) {
                m += state.world.body_chunk_get_mass(state.world.chunk_ownership_get_chunk(co));
            });
            return m;
        }

        using creature::consious;
        using creature::room_of;

        vec2 pos(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_pos(chunk(state, c, i));
        }

        dcon::player_id grasped(sys::game_state const& state, dcon::creature_id c, int g) {
            return creature::player_of(state, state.world.creature_get_grasp_player(c)[size_t(g)]);
        }

        // Centipede.VerticalSitSurface / HorizontalSitSurface
        int vertical_sit_surface(room_view const& room, vec2 p) {
            if (room.get_tile(p + vec2{0.f, -20.f}).solid())
                return -1;
            if (room.get_tile(p + vec2{0.f, 20.f}).solid())
                return 1;
            return 0;
        }
        int horizontal_sit_surface(room_view const& room, vec2 p) {
            bool const l = room.get_tile(p + vec2{-20.f, 0.f}).solid();
            bool const r = room.get_tile(p + vec2{20.f, 0.f}).solid();
            if (l && !r)
                return -1;
            if (r && !l)
                return 1;
            return 0;
        }

        bool accessible(room_view const& room, vec2 p) {
            return pathing::accessible(room, room_view::get_tile_position(p), pathing::climber);
        }

        void release(sys::game_state& state, dcon::creature_id c, int g) {
            auto& w = state.world;
            if (auto const p = grasped(state, c, g)) {
                uint8_t& n = w.physical_object_get_grabbed_by_count(player::body(state, p));
                if (n > 0)
                    --n;
            }
            w.creature_get_grasp_player(c)[size_t(g)] = 0;
        }

        void lose_all_grasps(sys::game_state& state, dcon::creature_id c) {
            release(state, c, 0);
            release(state, c, 1);
        }

        // Creature.Stun + Centipede.Stun
        void stun(sys::game_state& state, dcon::creature_id c, int32_t st) {
            auto& w = state.world;
            if (w.creature_get_random(c).value() < 0.5f)
                lose_all_grasps(state, c);
            w.creature_set_stun(c, std::max(w.creature_get_stun(c), st));
        }

        void die(sys::game_state& state, dcon::creature_id c) {
            state.world.creature_set_dead(c, 1);
            lose_all_grasps(state, c);
        }

        // CentipedeAI's relationship to a slugcat: prey if lighter, else feared
        struct relationship {
            bool eats = false;
            float intensity = 0.f;
        };
        relationship relationship_to(sys::game_state const& state, dcon::creature_id c, dcon::player_id p) {
            if (state.world.player_get_dead(p))
                return {};
            float const mine = total_mass(state, c);
            float const theirs = object_mass(state, player::body(state, p));
            if (theirs >= mine)
                return relationship{false, 0.2f + 0.8f * mathf::inverse_lerp(mine, mine * 1.5f, theirs)};
            return relationship{true, mathf::inverse_lerp(0.f, mine, theirs)};
        }

        // CentipedeAI.DoIWantToShockCreature
        bool want_to_shock(sys::game_state const& state, dcon::creature_id c, dcon::player_id p) {
            auto const& w = state.world;
            if (w.creature_get_payload(c).centipede.annoying_collisions < 150 && world::centipede_behavior(w.creature_get_behavior(c)) == centipede_behavior::flee
            && w.creature_get_current_utility(c) > 0.1f)
                return false;
            if (w.player_get_dead(p))
                return false;
            if (w.creature_get_payload(c).centipede.annoying_collisions > 150)
                return true;
            return relationship_to(state, c, p).eats;
        }

        // Centipede.Shock
        void shock(sys::game_state& state, dcon::creature_id c, dcon::player_id p) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            auto const room = room_of(state, c);
            state.play_sound(sound::sound_id::centipede_shock, room, pos(state, c, chunk_count(state, c) / 2));
            w.centipede_data_set_light_flash(creature::centipede_data(w, c), 1.f);
            auto const jolt = [&](dcon::body_chunk_id k) {
                w.body_chunk_get_vel(k) += custom::deg_to_vec(rng.value() * 360.f) * 6.f * rng.value();
                w.body_chunk_get_pos(k) += custom::deg_to_vec(rng.value() * 360.f) * 6.f * rng.value();
            };
            for (uint32_t i = 0; i < chunk_count(state, c); ++i)
                jolt(chunk(state, c, i));
            for (uint32_t i = 0; i < 2; ++i)
                jolt(player::body_chunk(state, p, i));
            float const theirs = object_mass(state, player::body(state, p));
            float const mine = total_mass(state, c);
            if (theirs < mine) {
                player::die(state, p);
                items::release_all(state, p);
            } else {
                player::stun(state, p, int32_t(custom::lerp_map(theirs, 0.f, mine * 2.f, 300.f, 30.f)));
                items::release_all(state, p);
                stun(state, c, 6);
                w.creature_get_payload(c).centipede.shock_give_up_counter = std::max(w.creature_get_payload(c).centipede.shock_give_up_counter, 30);
                w.creature_get_payload(c).centipede.annoying_collisions = std::min(w.creature_get_payload(c).centipede.annoying_collisions / 2, 150);
            }
        }

        void grab(sys::game_state& state, dcon::creature_id c, int g, dcon::player_id p, uint32_t player_chunk) {
            auto& w = state.world;
            w.creature_get_grasp_player(c)[size_t(g)] = uint8_t(p.index() + 1);
            w.creature_get_grasp_chunk(c)[size_t(g)] = uint8_t(player_chunk);
            ++w.physical_object_get_grabbed_by_count(player::body(state, p));
            uint32_t const end = g == 0 ? 0 : chunk_count(state, c) - 1;
            state.play_sound(sound::sound_id::centipede_attach, room_of(state, c), pos(state, c, end));
        }

        // Centipede.UpdateGrasp
        void update_grasp(sys::game_state& state, dcon::creature_id c, int g) {
            auto& w = state.world;
            auto const p = grasped(state, c, g);
            if (!p) {
                w.creature_get_grasp_player(c)[size_t(g)] = 0;
                return;
            }
            if (w.creature_get_random(c).value() < 0.025f && (!want_to_shock(state, c, p) || !consious(state, c) || grasped(state, c, 1 - g))) {
                release(state, c, g);
                return;
            }
            uint32_t const n = chunk_count(state, c);
            auto const end = chunk(state, c, g == 0 ? 0 : n - 1);
            auto const held = player::body_chunk(state, p, w.creature_get_grasp_chunk(c)[size_t(g)]);
            vec2& ep = w.body_chunk_get_pos(end);
            vec2& hp = w.body_chunk_get_pos(held);
            float const num = rw::distance(ep, hp);
            if (num > 50.f + w.body_chunk_get_rad(held)) {
                release(state, c, g);
                return;
            }
            vec2 a = custom::dir_vec(ep, hp);
            float rad = w.body_chunk_get_rad(held);
            float ratio = w.body_chunk_get_mass(held) / (w.body_chunk_get_mass(held) + w.body_chunk_get_mass(end));
            constexpr float d = 0.95f;
            ep -= a * ((rad - num) * ratio * d);
            w.body_chunk_get_vel(end) -= a * ((rad - num) * ratio * d);
            hp += a * ((rad - num) * (1.f - ratio) * d);
            w.body_chunk_get_vel(held) += a * ((rad - num) * (1.f - ratio) * d);
            if (grasped(state, c, 1 - g))
                return;
            // the free end reaching the prey charges the shock
            auto const other = chunk(state, c, g != 0 ? 0 : n - 1);
            for (uint32_t i = 0; i < 2; ++i) {
                auto const k = player::body_chunk(state, p, i);
                vec2& op = w.body_chunk_get_pos(other);
                vec2& kp = w.body_chunk_get_pos(k);
                if (!custom::dist_less(op, kp, w.body_chunk_get_rad(other) + w.body_chunk_get_rad(k) + 10.f))
                    continue;
                a = custom::dir_vec(op, kp);
                rad = w.body_chunk_get_rad(k);
                ratio = w.body_chunk_get_mass(k) / (w.body_chunk_get_mass(held) + w.body_chunk_get_mass(other));
                op -= a * ((rad - num) * ratio * d);
                w.body_chunk_get_vel(other) -= a * ((rad - num) * ratio * d);
                kp += a * ((rad - num) * (1.f - ratio) * d);
                w.body_chunk_get_vel(k) += a * ((rad - num) * (1.f - ratio) * d);
                float& charge = w.creature_get_payload(c).centipede.shock_charge;
                charge += 1.f / mathf::lerp(100.f, 5.f, w.creature_get_payload(c).centipede.size);
                if (charge >= 1.f) {
                    shock(state, c, p);
                    charge = 0.f;
                }
                return;
            }
        }

        // Rope between neighbouring chunks, as a straight distance constraint
        void rope(sys::game_state& state, dcon::creature_id c, uint32_t l) {
            auto& w = state.world;
            auto const a = chunk(state, c, l);
            auto const b = chunk(state, c, l + 1);
            vec2& pa = w.body_chunk_get_pos(a);
            vec2& pb = w.body_chunk_get_pos(b);
            float const len = rw::distance(pa, pb);
            float const want = w.body_chunk_get_rad(a) + w.body_chunk_get_rad(b);
            if (len <= want)
                return;
            float const ratio = w.body_chunk_get_mass(a) / (w.body_chunk_get_mass(a) + w.body_chunk_get_mass(b));
            vec2 const dir_a = custom::dir_vec(pa, pb);
            pa += dir_a * ((len - want) * ratio);
            w.body_chunk_get_vel(a) += dir_a * ((len - want) * ratio);
            pb -= dir_a * ((len - want) * (1.f - ratio));
            w.body_chunk_get_vel(b) -= dir_a * ((len - want) * (1.f - ratio));
        }

        // ArtificialIntelligence.VisualScore, roughly: distance, facing and
        // the centipede's poor sight while moving
        bool sees(sys::game_state const& state, dcon::creature_id c, room_view const& room, vec2 target) {
            uint32_t const n = chunk_count(state, c);
            bool const front = (state.tick & 1u) != 0; // CentipedeAI alternates ends
            vec2 const p1 = front ? pos(state, c, 1) : pos(state, c, n - 2);
            vec2 const p2 = front ? pos(state, c, 0) : pos(state, c, n - 1);
            float const d = rw::distance(p2, target);
            if (d > visual_radius || !room.visual_contact(p2, target))
                return false;
            float score = 1.f - d / visual_radius;
            score -= mathf::inverse_lerp(1.f, 0.7f, rw::dot(rw::normalized(p1 - p2), rw::normalized(p2 - target)));
            if (state.world.creature_get_payload(c).centipede.moving)
                score -= 0.75f;
            return score > 0.05f;
        }

        // CentipedeAI.IdleScore (no attractors, no other centipedes yet)
        float idle_score(sys::game_state const& state, dcon::creature_id c, room_view const& room, ivec2 t) {
            if (!room.inside(t.x, t.y) || !pathing::accessible(room, t, pathing::climber))
                return -1e9f;
            float score = 1000.f / mathf::max(1.f, float(pathing::terrain_proximity(room, t)) - 1.f);
            ivec2 const f = state.world.creature_get_forbidden_idle_pos(c);
            score -= custom::lerp_map(rw::magnitude(rw::to_vec2(t - f)), 0.f, 10.f, 1000.f, 0.f);
            return score;
        }

        // CentipedeAI.Update
        void ai_update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            if (w.creature_get_payload(c).centipede.annoying_collisions > 0)
                --w.creature_get_payload(c).centipede.annoying_collisions;
            ivec2 const here = room_view::get_tile_position(pos(state, c, head_index(state, c)));

            // tracker: the closest slugcat it can see
            dcon::player_id prey;
            float best = 1e9f;
            rooms::for_each_player(state, room.id, [&](dcon::player_id p) {
                if (w.player_get_in_shortcut(p))
                    return;
                vec2 const pp = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
                float const d = rw::distance(pp, pos(state, c, head_index(state, c)));
                if (d < best && sees(state, c, room, pp)) {
                    best = d;
                    prey = p;
                }
            });
            int32_t& since = w.creature_get_ticks_since_seen(c);
            if (prey) {
                w.creature_set_seen_player(c, uint8_t(prey.index() + 1));
                w.creature_set_last_seen_pos(c, w.body_chunk_get_pos(player::body_chunk(state, prey, 0)));
                since = 0;
            } else if (w.creature_get_seen_player(c)) {
                if (++since > 1200)
                    w.creature_set_seen_player(c, 0);
            }
            dcon::player_id tracked;
            if (uint8_t const sp = w.creature_get_seen_player(c)) {
                tracked = dcon::player_id{dcon::player_id::value_base_t(sp - 1)};
                if (!w.player_is_valid(tracked) || !rooms::reachable(state, tracked))
                    tracked = dcon::player_id{};
            }

            // UtilityComparer: prey (0.9), threat (1), injury (0.7)
            float prey_utility = 0.f, threat_utility = 0.f;
            if (tracked) {
                auto const rel = relationship_to(state, c, tracked);
                float const d = rw::distance(w.creature_get_last_seen_pos(c), pos(state, c, head_index(state, c)));
                if (rel.eats)
                    prey_utility = rel.intensity * 0.9f * mathf::inverse_lerp(50.f, 10.f, float(since)) * custom::lerp_map(d, 100.f, 1200.f, 1.f, 0.3f);
                else
                    threat_utility = rel.intensity * mathf::inverse_lerp(500.f, 100.f, float(since)) * custom::lerp_map(d, 60.f, 500.f, 1.f, 0.f);
            }
            float const health = w.creature_get_health(c);
            float const injury_utility = health < 0.6f ? (0.6f - health) / 0.6f * 0.7f : 0.f;
            float utility = prey_utility;
            centipede_behavior behavior = centipede_behavior::hunt;
            if (threat_utility > utility) {
                utility = threat_utility;
                behavior = centipede_behavior::flee;
            }
            if (injury_utility > utility) {
                utility = injury_utility;
                behavior = centipede_behavior::injured;
            }
            if (utility < 0.1f)
                behavior = centipede_behavior::idle;
            w.creature_set_current_utility(c, utility);
            w.creature_set_behavior(c, uint8_t(behavior));

            float excitement_goal = 0.f;
            ivec2& dest = w.creature_get_destination(c);
            switch (behavior) {
            case centipede_behavior::idle:
            case centipede_behavior::injured: {
                excitement_goal = behavior == centipede_behavior::injured ? 1.f : 0.f;
                ivec2 test = here + ivec2{rng.range(-10, 11), rng.range(-10, 11)};
                if (rng.value() < 1.f / 100.f)
                    test = ivec2{rng.range(0, room.width), rng.range(0, room.height)};
                ivec2& idle = w.creature_get_payload(c).centipede.temp_idle_pos;
                if (idle_score(state, c, room, test) > idle_score(state, c, room, idle)) {
                    idle = test;
                    w.creature_get_payload(c).centipede.idle_counter = 0;
                } else if (++w.creature_get_payload(c).centipede.idle_counter > 1400) {
                    w.creature_get_payload(c).centipede.idle_counter = 0;
                    w.creature_set_forbidden_idle_pos(c, idle);
                }
                if (idle != dest && idle_score(state, c, room, idle) > idle_score(state, c, room, dest) + 100.f)
                    dest = idle;
                break;
            }
            case centipede_behavior::flee: {
                excitement_goal = 1.f;
                // ThreatTracker.FleeTo: the walkable tile furthest from the threat
                vec2 const threat = w.creature_get_last_seen_pos(c);
                float best_d = rw::distance(room_view::middle_of_tile(dest), threat);
                for (int k = 0; k < 6; ++k) {
                    ivec2 const t = here + ivec2{rng.range(-30, 31), rng.range(-30, 31)};
                    if (!room.inside(t.x, t.y) || !pathing::accessible(room, t, pathing::climber))
                        continue;
                    float const d = rw::distance(room_view::middle_of_tile(t), threat);
                    if (d > best_d + 40.f) {
                        best_d = d;
                        dest = t;
                    }
                }
                break;
            }
            case centipede_behavior::hunt:
                excitement_goal = relationship_to(state, c, tracked).intensity;
                dest = room_view::get_tile_position(w.creature_get_last_seen_pos(c));
                break;
            }
            float& excitement = w.creature_get_payload(c).centipede.excitement;
            excitement = mathf::lerp(excitement, excitement_goal, 0.1f);
            // bursts of running and resting
            float& run = w.creature_get_payload(c).centipede.run;
            run -= 1.f;
            if (run < mathf::lerp(-50.f, -5.f, excitement))
                run = mathf::lerp(30.f, 50.f, excitement);
        }

        // Centipede.Crawl
        void crawl(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            uint32_t const n = chunk_count(state, c);
            float const size = w.creature_get_payload(c).centipede.size;
            float const health = mathf::clamp01(w.creature_get_health(c));
            float const g = physics::gravity(state, body(state, c), room);
            bool const moving = w.creature_get_payload(c).centipede.moving;
            int const fwd = w.creature_get_payload(c).centipede.body_direction ? -1 : 1;
            auto const hold = [&]() { return g * mathf::pow(mathf::max(0.f, mathf::lerp(health, 1.f, rng.value())), 0.25f); };
            uint32_t grip = 0;
            for (uint32_t i = 0; i < n; ++i) {
                auto const k = chunk(state, c, i);
                vec2& v = w.body_chunk_get_vel(k);
                vec2 const p = w.body_chunk_get_pos(k);
                if (!accessible(room, p))
                    continue;
                ++grip;
                v *= 0.7f;
                v.y += hold();
                if (i > 0 && !accessible(room, pos(state, c, i - 1))) {
                    v *= 0.3f;
                    v.y += hold();
                }
                if (i < n - 1 && !accessible(room, pos(state, c, i + 1))) {
                    v *= 0.3f;
                    v.y += hold();
                }
                if (i == 0 || i == n - 1)
                    continue;
                if (moving) {
                    vec2 const ahead = pos(state, c, uint32_t(int(i) + fwd));
                    if (accessible(room, ahead))
                        v += custom::dir_vec(p, ahead) * 1.5f * mathf::lerp(0.5f, 1.5f, size * health);
                    v -= custom::dir_vec(p, pos(state, c, uint32_t(int(i) - fwd))) * 0.8f * mathf::lerp(0.7f, 1.3f, size * health);
                } else {
                    // settle onto the surface it sits on
                    vec2 const along = (rw::normalized(p - pos(state, c, i - 1)) + rw::normalized(pos(state, c, i + 1) - p)) / 2.f;
                    float const k_fac = mathf::lerp(0.01f, 0.6f, mathf::pow(size * health, 1.2f));
                    float const rad = w.body_chunk_get_rad(k);
                    if (std::fabs(along.x) > 0.5f)
                        v.y -= (p.y - (room_view::middle_of_tile(p).y + float(vertical_sit_surface(room, p)) * (10.f - rad))) * k_fac;
                    if (std::fabs(along.y) > 0.5f)
                        v.x -= (p.x - (room_view::middle_of_tile(p).x + float(horizontal_sit_surface(room, p)) * (10.f - rad))) * k_fac;
                }
            }
            auto const head = chunk(state, c, head_index(state, c));
            vec2 const target = w.creature_get_payload(c).centipede.move_to_pos;
            if (grip > 0 && !custom::dist_less(w.body_chunk_get_pos(head), target, 10.f))
                w.body_chunk_get_vel(head) += custom::dir_vec(w.body_chunk_get_pos(head), target)
                    * custom::lerp_map(float(grip), 0.f, float(n), 6.f, 3.f) * mathf::lerp(0.7f, 1.3f, size * w.creature_get_health(c));
        }

        // Centipede.Act
        void act(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            uint32_t const n = chunk_count(state, c);
            float const size = w.creature_get_payload(c).centipede.size;
            ivec2 const dest = w.creature_get_destination(c);
            auto const field = pathing::field_to(state, room.id, dest, pathing::climber);

            // turn around when the tail is the end closer to the goal
            int32_t& block = w.creature_get_payload(c).centipede.direction_change_block;
            if (block > 0) {
                block -= w.creature_get_payload(c).centipede.moving ? 1 : 0;
            } else {
                bool const dir = w.creature_get_payload(c).centipede.body_direction;
                ivec2 const head_t = room_view::get_tile_position(pos(state, c, dir ? 0 : n - 1));
                ivec2 const tail_t = room_view::get_tile_position(pos(state, c, dir ? n - 1 : 0));
                if (pathing::distance(state, field, tail_t) < pathing::distance(state, field, head_t)) {
                    bool const open = pathing::terrain_proximity(room, head_t) > 1 || pathing::terrain_proximity(room, tail_t) > 1;
                    if (++w.creature_get_payload(c).centipede.change_dir_counter > (open ? 10 : 2)) {
                        w.creature_get_payload(c).centipede.body_direction = dir ? 0 : 1;
                        block = 40;
                        w.creature_get_payload(c).centipede.change_dir_counter = 0;
                    }
                } else {
                    w.creature_get_payload(c).centipede.change_dir_counter = 0;
                }
            }

            ai_update(state, c, room);
            auto const head = chunk(state, c, head_index(state, c));
            ivec2 const head_t = room_view::get_tile_position(w.body_chunk_get_pos(head));
            ivec2 const goal = w.creature_get_destination(c);
            bool moving = w.creature_get_payload(c).centipede.run > 0.f && std::abs(head_t.x - goal.x) + std::abs(head_t.y - goal.y) > 2;
            if (moving) {
                auto const f = goal == dest ? field : pathing::field_to(state, room.id, goal, pathing::climber);
                if (pathing::distance(state, f, head_t) == pathing::unreachable && world::centipede_behavior(w.creature_get_behavior(c)) == centipede_behavior::idle) {
                    // nowhere to go from here: try somewhere else
                    w.creature_set_forbidden_idle_pos(c, goal);
                    w.creature_get_payload(c).centipede.temp_idle_pos = head_t;
                }
                auto const step = pathing::next_step(state, f, head_t);
                moving = step.has_value();
                if (step) {
                    physics::set_go_through_floors(state, body(state, c), step->y < head_t.y);
                    vec2 to = room_view::middle_of_tile(*step);
                    if (step->x != head_t.x)
                        to.y += float(vertical_sit_surface(room, to)) * 5.f;
                    if (step->y != head_t.y)
                        to.x += float(horizontal_sit_surface(room, to)) * 5.f;
                    w.creature_get_payload(c).centipede.move_to_pos = to;
                    w.creature_get_payload(c).centipede.no_follow_con_counter = 0;
                } else if (++w.creature_get_payload(c).centipede.no_follow_con_counter > 40) {
                    uint32_t const k = uint32_t(w.creature_get_random(c).range(0, int32_t(n)));
                    if (accessible(room, pos(state, c, k)))
                        w.creature_get_payload(c).centipede.move_to_pos = pos(state, c, k);
                }
            }
            w.creature_get_payload(c).centipede.moving = moving;
            // close enough to the prey it is hunting: reach for it (the path ends
            // two tiles short of the prey's tile)
            if (world::centipede_behavior(w.creature_get_behavior(c)) == centipede_behavior::hunt && w.creature_get_ticks_since_seen(c) < 10) {
                vec2 const prey = w.creature_get_last_seen_pos(c);
                if (custom::dist_less(w.body_chunk_get_pos(head), prey, 70.f) && room.visual_contact(w.body_chunk_get_pos(head), prey))
                    w.creature_get_payload(c).centipede.move_to_pos = prey;
            }

            if (!grasped(state, c, 0) && !grasped(state, c, 1)) {
                crawl(state, c, room);
                w.creature_get_payload(c).centipede.double_grab_charge = mathf::max(0.f, w.creature_get_payload(c).centipede.double_grab_charge - 0.025f);
                w.creature_get_payload(c).centipede.shock_give_up_counter = std::max(0, w.creature_get_payload(c).centipede.shock_give_up_counter - 2);
                return;
            }
            // holding on with one end: bring the other one around
            for (int g = 0; g < 2; ++g) {
                auto const p = grasped(state, c, g);
                if (p && !grasped(state, c, 1 - g) && want_to_shock(state, c, p))
                    w.creature_get_payload(c).centipede.move_to_pos = w.body_chunk_get_pos(player::body_chunk(state, p, w.creature_get_grasp_chunk(c)[size_t(g)]));
            }
            float& charge = w.creature_get_payload(c).centipede.double_grab_charge;
            w.body_chunk_get_vel(head) += custom::dir_vec(w.body_chunk_get_pos(head), w.creature_get_payload(c).centipede.move_to_pos) * mathf::pow(charge, 2.f) * 6.f * mathf::lerp(0.7f, 1.3f, size);
            charge = mathf::min(1.f, charge + 0.0125f);
            if (charge > 0.9f) {
                int32_t& give_up = w.creature_get_payload(c).centipede.shock_give_up_counter;
                give_up = std::min(110, give_up + 1);
                if (give_up >= 110) {
                    stun(state, c, 12);
                    give_up = 30;
                    lose_all_grasps(state, c);
                }
            }
        }

    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto const o = body(state, c);
        uint32_t const n = chunk_count(state, c);
        float const size = w.creature_get_payload(c).centipede.size;
        auto& rng = w.creature_get_random(c);
        // Creature.Update + PhysicalObject.Update
        creature::stun_countdown(state, c);
        physics::update_object(state, room, o, nullptr, nullptr);
        for (int g = 0; g < 2; ++g)
            update_grasp(state, c, g);
        for (uint32_t l = 0; l + 1 < n; ++l)
            rope(state, c, l);
        for (uint32_t l = n - 1; l-- > 0;)
            rope(state, c, l);
        // every chunk pushes away from the one two steps along: a stiff body
        float const stiff = mathf::lerp(1.f, 6.f, w.creature_get_payload(c).centipede.shock_charge) * mathf::lerp(1.f, 2.f, size);
        for (uint32_t i = 0; i + 2 < n; ++i) {
            vec2 const a = pos(state, c, i), b = pos(state, c, i + 2);
            w.body_chunk_get_vel(chunk(state, c, i)) += custom::dir_vec(b, a) * stiff;
            w.body_chunk_get_vel(chunk(state, c, i + 2)) += custom::dir_vec(a, b) * stiff;
        }
        for (int g = 0; g < 2; ++g)
            update_grasp(state, c, g);

        float const health = w.creature_get_health(c);
        if (w.creature_get_dead(c)) {
            for (uint32_t i = 0; i < n; ++i)
                if (w.body_chunk_get_contact_point(chunk(state, c, i)).y != 0)
                    w.body_chunk_get_vel(chunk(state, c, i)).x *= 0.1f;
        } else if (health < 0.75f) {
            if (rng.value() * 0.75f > health && w.creature_get_stun(c) > 0)
                --w.creature_get_stun(c);
            if (rng.value() > health && rng.value() < 1.f / 3.f) {
                stun(state, c, 4);
                if (health <= 0.f && rng.value() < 1.f / mathf::lerp(500.f, 10.f, -health))
                    die(state, c);
            }
            if (!w.creature_get_dead(c))
                for (uint32_t i = 0; i < n; ++i)
                    if (rng.value() > health * 2.f)
                        w.body_chunk_get_vel(chunk(state, c, i)) += custom::deg_to_vec(rng.value() * 360.f)
                            * mathf::pow(rng.value(), custom::lerp_map(health, 0.75f, 0.f, 3.f, 0.1f, 2.f)) * 4.f * mathf::inverse_lerp(0.75f, 0.f, health);
        }
        if (consious(state, c))
            act(state, c, room);
        w.creature_get_payload(c).centipede.shock_charge = mathf::max(0.f, w.creature_get_payload(c).centipede.shock_charge - 0.008333334f);
        w.centipede_data_set_light_flash(creature::centipede_data(w, c), mathf::max(0.f, w.centipede_data_get_light_flash(creature::centipede_data(w, c)) - 0.1f));
        if (w.creature_get_payload(c).centipede.moving)
            w.centipede_data_get_walk_cycle(creature::centipede_data(w, c)) += 0.25f;
    }

    // Centipede.HeadIndex: the free end when holding on with one, else the leading one
    uint32_t head_index(sys::game_state const& state, dcon::creature_id c) noexcept {
        uint32_t const last = chunk_count(state, c) - 1;
        auto const& g = state.world.creature_get_grasp_player(c);
        if (g[0] != 0 && g[1] == 0)
            return last;
        if (g[0] == 0 && g[1] != 0)
            return 0;
        return state.world.creature_get_payload(c).centipede.body_direction ? 0 : last;
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 at, float size) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::centipede);
        rw::rng rng{state.random.next() | 1u};
        if (size < 0.f)
            size = mathf::lerp(0.f, 1.f, mathf::pow(rng.value(), 1.5f));
        size = mathf::clamp01(size);
        physics::object_desc d;
        d.air_friction = 0.999f;
        d.gravity = 0.9f;
        d.bounce = 0.1f;
        d.surface_friction = 0.4f;
        d.water_friction = 0.96f;
        d.buoyancy = 1.05f;
        auto const o = physics::create_object(state, room, d);
        int const n = int(mathf::lerp(7.f, 17.f, size));
        auto const view = world::get_room_view(state, room);
        // never inside terrain: the closest tile it can stand on, else any open one
        {
            ivec2 const t0 = room_view::get_tile_position(at);
            ivec2 best = t0;
            int best_score = 1 << 30;
            for (int dy = -6; dy <= 6; ++dy)
                for (int dx = -6; dx <= 6; ++dx) {
                    ivec2 const t = t0 + ivec2{dx, dy};
                    if (!view.inside(t.x, t.y) || view.get_tile(t).solid())
                        continue;
                    int const score = dx * dx + dy * dy + (pathing::accessible(view, t, pathing::climber) ? 0 : 50);
                    if (score < best_score) {
                        best_score = score;
                        best = t;
                    }
                }
            if (best != t0 || view.get_tile(t0).solid())
                at = room_view::middle_of_tile(best);
        }
        vec2 p = at;
        std::vector<dcon::body_chunk_id> chunks;
        for (int i = 0; i < n; ++i) {
            float const t = float(i) / float(n - 1);
            float const rad = mathf::lerp(mathf::lerp(2.f, 3.5f, size), mathf::lerp(4.f, 6.5f, size),
                mathf::pow(mathf::clamp01(std::sin(mathf::pi * t)), mathf::lerp(0.7f, 0.3f, size)));
            float const mass = mathf::lerp(0.042857144f, 0.32352942f, mathf::pow(size, 1.4f));
            // laid out sideways from the spawn point, folding back at walls
            if (i > 0) {
                vec2 const next = p + vec2{8.f, 0.f};
                if (!view.get_tile(next).solid())
                    p = next;
            }
            chunks.push_back(physics::add_chunk(state, o, p, rad, mass));
        }
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j) {
                float const mi = w.body_chunk_get_mass(chunks[size_t(i)]), mj = w.body_chunk_get_mass(chunks[size_t(j)]);
                physics::add_connection(state, o, chunks[size_t(i)], chunks[size_t(j)],
                    w.body_chunk_get_rad(chunks[size_t(i)]) + w.body_chunk_get_rad(chunks[size_t(j)]),
                    physics::connection_type::push, 1.f, mj / (mi + mj));
            }
        w.try_create_creature_body(c, o);
        w.creature_get_payload(c).centipede.size = size;
        w.creature_set_random(c, rng);
        w.centipede_data_set_hue(creature::centipede_data(w, c), mathf::lerp(0.04f, 0.1f, rng.value()));
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_get_payload(c).centipede.body_direction = rng.value() < 0.5f;
        w.creature_get_payload(c).centipede.moving = 0;
        w.creature_get_payload(c).centipede.move_to_pos = at;
        w.creature_get_payload(c).centipede.change_dir_counter = 0;
        w.creature_get_payload(c).centipede.direction_change_block = 0;
        w.creature_get_payload(c).centipede.no_follow_con_counter = 0;
        w.creature_set_grasp_player(c, world::byte_pair{});
        w.creature_set_grasp_chunk(c, world::byte_pair{});
        w.creature_get_payload(c).centipede.shock_charge = 0.f;
        w.creature_get_payload(c).centipede.double_grab_charge = 0.f;
        w.creature_get_payload(c).centipede.shock_give_up_counter = 0;
        w.creature_set_behavior(c, uint8_t(centipede_behavior::idle));
        w.creature_get_payload(c).centipede.run = 0.f;
        w.creature_get_payload(c).centipede.excitement = 0.f;
        w.creature_set_current_utility(c, 0.f);
        ivec2 const tile = room_view::get_tile_position(at);
        w.creature_set_destination(c, tile);
        w.creature_get_payload(c).centipede.temp_idle_pos = tile;
        w.creature_set_forbidden_idle_pos(c, ivec2{-1000, -1000});
        w.creature_get_payload(c).centipede.idle_counter = 0;
        w.creature_get_payload(c).centipede.annoying_collisions = 0;
        w.creature_set_seen_player(c, 0);
        w.creature_set_ticks_since_seen(c, 0);
        w.creature_set_last_seen_pos(c, at);
        w.centipede_data_set_light_flash(creature::centipede_data(w, c), 0.f);
        w.centipede_data_set_walk_cycle(creature::centipede_data(w, c), 0.f);
        centipede_graphics::reset(state, c);
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        lose_all_grasps(state, c);
        creature::erase(state, c);
    }

    void collide_player(sys::game_state& state, dcon::creature_id c, uint32_t i, dcon::player_id p, uint32_t j) {
        auto& w = state.world;
        if (!consious(state, c))
            return;
        // Centipede.Collide: noticed, annoyed, and grabbed with an end
        uint32_t const n = chunk_count(state, c);
        w.creature_set_seen_player(c, uint8_t(p.index() + 1));
        w.creature_set_ticks_since_seen(c, 0);
        w.creature_set_last_seen_pos(c, w.body_chunk_get_pos(player::body_chunk(state, p, 0)));
        w.creature_get_payload(c).centipede.annoying_collisions += 10;
        if (!want_to_shock(state, c, p) || w.creature_get_payload(c).centipede.shock_give_up_counter > 0)
            return;
        for (int g = 0; g < 2; ++g)
            if (i == (g == 0 ? 0 : n - 1) && !grasped(state, c, g))
                grab(state, c, g, p, j);
    }

    void violence(sys::game_state& state, dcon::creature_id c, uint32_t k, vec2 momentum, float damage, float stun_bonus) {
        auto& w = state.world;
        auto& rng = w.creature_get_random(c);
        float const health = w.creature_get_health(c);
        // Centipede.Violence: big centipedes shrug hits off
        stun_bonus *= 1.f - health;
        damage *= mathf::lerp(1.3f, 0.075f, std::sqrt(w.creature_get_payload(c).centipede.size));
        // Creature.Violence
        auto const hit = chunk(state, c, k);
        w.body_chunk_get_vel(hit) += rw::clamp_magnitude(momentum / w.body_chunk_get_mass(hit), 10.f);
        float const dmg = damage / base_damage_resistance;
        float st = (damage * 30.f + stun_bonus) / base_stun_resistance;
        if (health < 0.5f)
            st *= 1.5f + mathf::inverse_lerp(0.5f, 0.f, health) * rng.value();
        stun(state, c, int32_t(st));
        w.creature_set_health(c, health - dmg);
    }

    void release_player(sys::game_state& state, dcon::creature_id c, dcon::player_id p) {
        for (int g = 0; g < 2; ++g)
            if (grasped(state, c, g) == p)
                release(state, c, g);
    }
}
