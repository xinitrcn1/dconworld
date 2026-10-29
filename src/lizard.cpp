#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

#include "lizard.hpp"
#include "rooms.hpp"
#include "lizard_ai.hpp"
#include "lizard_graphics.hpp"
#include "items.hpp"
#include "particles.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "player.hpp"

namespace lizard {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using world::room_view;
    using pathing::access;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // Player body chunk masses (the slugcat's TotalMass)
        constexpr float slugcat_mass = 0.7f;

        // One lizard during its update: the storage it works on
        struct ctx {
            sys::game_state& state;
            dcon::lizard_id id;
            room_view const& room;
            breed_params const& p;
            body_state& z;
            graphics_state& g;
            ai_state& a;
            rw::rng& rng;
            dcon::physical_object_id o;
            dcon::body_chunk_id c[3];
            // a camera looks at the room: it has a graphicsModule (LizardGraphics)
            bool viewed;

            ctx(sys::game_state& s, dcon::lizard_id l, room_view const& r)
                : state(s), id(l), room(r), p(params_of(s, l)), z(s.world.lizard_get_core(l)), g(s.world.lizard_get_graphics(l)),
                  a(s.world.lizard_get_ai(l)), rng(s.world.lizard_get_random(l)), o(body(s, l)), viewed(s.viewed(r.id)) {
                for (uint32_t i = 0; i < 3; ++i)
                    c[i] = physics::chunk(s, o, i);
            }
            vec2& pos(uint32_t i) { return state.world.body_chunk_get_pos(c[i]); }
            vec2& vel(uint32_t i) { return state.world.body_chunk_get_vel(c[i]); }
            vec2 last_pos(uint32_t i) { return state.world.body_chunk_get_last_pos(c[i]); }
            float rad(uint32_t i) { return state.world.body_chunk_get_rad(c[i]); }
            float mass(uint32_t i) { return state.world.body_chunk_get_mass(c[i]); }
            ivec2 contact(uint32_t i) { return state.world.body_chunk_get_contact_point(c[i]); }
            float submersion(uint32_t i) { return physics::submersion(state, room, c[i]); }
            float submersion() { return physics::object_submersion(state, room, o); }
            bool solid(uint32_t i, int32_t x, int32_t y) { return physics::is_tile_solid(state, room, c[i], x, y); }
            float total_mass() { return mass(0) + mass(1) + mass(2); }
            bool consious() { return lizard::consious(state, id); }
            bool is(breed b) { return state.world.lizard_get_breed(id) == b; }
            breed kind() { return state.world.lizard_get_breed(id); }
            float health() { return state.world.lizard_get_health(id); }
            void sound(sound::sound_id s, float vol = 1.f, float pitch = 1.f) { state.play_sound(s, room.id, pos(0), vol, pitch); }
            void sound_at(sound::sound_id s, vec2 at, float vol = 1.f, float pitch = 1.f) { state.play_sound(s, room.id, at, vol, pitch); }
            // AItile.acc / TileAccessibleToCreature
            access acc(ivec2 t) { return pathing::tile_access(room, t); }
            access acc(vec2 at) { return acc(room_view::get_tile_position(at)); }
            bool accessible(ivec2 t) { return room.inside(t.x, t.y) && pathing::accessible(room, t, p.movement); }
            bool accessible(vec2 at) { return accessible(room_view::get_tile_position(at)); }
            bool narrow(ivec2 t) { return pathing::narrow_space(room, t); }
            bool any_water(ivec2 t) { return room.get_tile(t).water != 0; }
            bool any_water(vec2 at) { return any_water(room_view::get_tile_position(at)); }
        };

        float float_dist(ivec2 a, ivec2 b) {
            return rw::magnitude(rw::to_vec2(a - b));
        }

        // Lizard.BodyForce
        float body_force(ctx& x) {
            return mathf::clamp(x.z.desperation_smoother * 0.025f, 1.f, x.p.max_muscle_power);
        }
        // Lizard.BodyDesperation
        float body_desperation(ctx& x) {
            return mathf::inverse_lerp(120.f, 400.f, x.z.desperation_smoother);
        }
        // Lizard.BodyWiggleFac
        float body_wiggle_fac(ctx& x) {
            return mathf::clamp((float(x.z.body_wiggle_counter) - float(x.p.wiggle_delay)) / (50.f + float(x.p.wiggle_delay)), 0.f, 1.f);
        }
        // Lizard.bodyWiggleCounter's setter
        void set_wiggle(ctx& x, int32_t v) {
            x.z.body_wiggle_counter = custom::int_clamp(v, 0, 100);
        }
        // Lizard.BodyDirection: the index into Custom.fourDirections the head points along
        int32_t body_direction(ctx& x) {
            return (int32_t(std::lround((-custom::aim(x.pos(1), x.pos(0)) + 180.f) / 90.f)) + 1) % 4;
        }
        // Lizard.SnakeCoil (the eel's coiling on land)
        float snake_coil(ctx& x) {
            if (!x.is(breed::eel))
                return 1.f;
            float const num = std::fabs(std::sin(float(x.z.snake_ticker) / 60.f * mathf::pi));
            // Custom.LerpElasticEaseIn/Out, from 0 to 1
            auto const ease_in = [](float t) {
                t = mathf::clamp01(t);
                return t == 0.f || t == 1.f ? t : -std::pow(2.f, 10.f * t - 10.f) * std::sin((t * 10.f - 10.75f) * (2.f * mathf::pi / 3.f));
            };
            auto const ease_out = [](float t) {
                t = mathf::clamp01(t);
                return t == 0.f || t == 1.f ? t : std::pow(2.f, -10.f * t) * std::sin((t * 10.f - 0.75f) * (2.f * mathf::pi / 3.f)) + 1.f;
            };
            if (num < 0.6f)
                return ease_in(num / 0.6f);
            return ease_out((num - 0.6f) / 0.4f);
        }

        // Creature.Die
        void die(sys::game_state& state, dcon::lizard_id l);
        // Lizard.LoseAllGrasps
        void lose_all_grasps(sys::game_state& state, dcon::lizard_id l) {
            auto& w = state.world;
            auto& z = w.lizard_get_core(l);
            if (auto const p = player_of(state, z.grasp_player)) {
                uint8_t& n = w.physical_object_get_grabbed_by_count(player::body(state, p));
                if (n > 0)
                    --n;
            }
            z.grasp_player = 0;
            z.danger_grasp_time = 0;
        }

        // LizardTongue.Retract
        void tongue_retract(sys::game_state& state, dcon::lizard_id l) {
            auto& w = state.world;
            auto& t = w.lizard_get_tongue(l);
            auto const room = room_of(state, l);
            if (t.state == tongue_state::stuck_in_terrain)
                state.play_sound(sound::sound_id::lizard_tongue_detatch_npc_or_terrain, room, t.pos);
            else if (t.attached_player)
                state.play_sound(sound::sound_id::lizard_tongue_detatch_player, room, t.pos);
            t.state = tongue_state::retracting;
            t.attached_player = 0;
        }

        // Lizard.Stun (+ Creature.Stun)
        void stun(sys::game_state& state, dcon::lizard_id l, int32_t st) {
            auto& w = state.world;
            float const health = w.lizard_get_health(l);
            if (health < 0.5f)
                st = int32_t(float(st) * (1.f + mathf::inverse_lerp(0.75f, 0.f, health) * 1.5f));
            if (st > 5 && w.lizard_get_stun(l) <= 0 && state.viewed(room_of(state, l)))
                lizard_graphics::stun(state, l, st);
            w.lizard_set_stun(l, std::max(w.lizard_get_stun(l), st));
            if (w.lizard_get_stun(l) > 0) {
                auto& z = w.lizard_get_core(l);
                z.has_grip_point = false;
                z.move_anim.active = false;
                lose_all_grasps(state, l);
            }
        }

        void die(sys::game_state& state, dcon::lizard_id l) {
            state.world.lizard_set_dead(l, 1);
            lose_all_grasps(state, l);
        }

        // the slugcat as the lizard hurts it (Player.Violence through
        // Creature.Violence: a stun, and death from a full point of damage);
        // held in a lizard's jaws (Player.dangerGrasp) it keeps what it holds
        void hurt_player(sys::game_state& state, dcon::player_id p, uint32_t k, vec2 momentum, float damage, float stun_bonus, rw::rng& rng) {
            auto& w = state.world;
            auto const c = player::body_chunk(state, p, k);
            w.body_chunk_get_vel(c) += momentum / w.body_chunk_get_mass(c);
            int32_t const st = int32_t(damage * 30.f + stun_bonus);
            player::stun(state, p, st);
            bool held = false;
            rooms::for_each_lizard(state, rooms::room_of(state, p), [&](dcon::lizard_id o) { held = held || grasped(state, o) == p; });
            if (!held && st > rng.range(5, 12))
                items::release_all(state, p);
            if (damage >= 1.f)
                player::die(state, p);
        }

        // Lizard.Bite on a slugcat's chunk (`p` none: the jaws shut on nothing)
        void bite(ctx& x, dcon::player_id p, uint32_t k) {
            auto& w = x.state.world;
            if (x.is(breed::train))
                for (int i = 0; i < 16; ++i) {
                    vec2 const a = custom::rnv(x.rng);
                    particles::spawn(x.state, x.room.id, particles::kind::spark, x.pos(0) + a * 40.f, a * mathf::lerp(4.f, 30.f, x.rng.value()), 2.f, 0.4f, vec3{1.f, 1.f, 1.f});
                }
            if (x.z.grasp_player)
                return; // (grasps[0] != null and nothing holds it)
            x.z.jaw_open = 0.f;
            x.z.last_jaw_open = 0.f;
            if (!p) {
                x.sound(sound::sound_id::lizard_jaws_shut_miss_creature);
                return;
            }
            auto const kc = player::body_chunk(x.state, p, k);
            w.body_chunk_get_vel(kc) += x.vel(0) * mathf::lerp(x.mass(0), 1.1f, 0.5f) / mathf::max(1.f, w.body_chunk_get_mass(kc));
            bool grabbed = false;
            if (ai::player_relationship(x.state, x.id, p).eats || (x.rng.value() < 0.5f && slugcat_mass < x.total_mass() * 1.2f)) {
                // Creature.Grab (CanOnlyShareWithNonExclusive, at biteDominance
                // * random): a weaker lizard's grip on it is broken, a
                // stronger one keeps it
                float const dominance = x.p.bite_dominance * x.rng.value();
                grabbed = true;
                rooms::for_each_lizard(x.state, x.room.id, [&](dcon::lizard_id other) {
                    if (other == x.id || grasped(x.state, other) != p)
                        return;
                    if (params_of(x.state, other).bite_dominance > dominance)
                        grabbed = false;
                    else
                        lose_all_grasps(x.state, other);
                });
                if (grabbed) {
                    x.z.grasp_player = player_ref(p);
                    x.z.grasp_chunk = uint8_t(k);
                    x.z.danger_grasp_time = 0;
                    ++w.physical_object_get_grabbed_by_count(player::body(x.state, p));
                }
            }
            if (grabbed) {
                // (the Indigo's mud isn't ported)
                if (x.is(breed::red))
                    items::release_all(x.state, p);
                ai::bit_creature(x.state, x.id, x.room);
                // no Remix: a killing bite at the breed's chance
                if (x.rng.value() < x.p.bite_damage_chance)
                    hurt_player(x.state, p, k, custom::dir_vec(x.pos(0), w.body_chunk_get_pos(kc)) * 0.1f, 1.5f, 0.f, x.rng);
                x.sound(sound::sound_id::lizard_jaws_grab_player);
                return;
            }
            // (only an Attacks relationship makes a DamageAttack of it)
            x.sound(sound::sound_id::lizard_jaws_shut_miss_creature);
            rooms::for_each_lizard(x.state, x.room.id, [&](dcon::lizard_id other) {
                if (grasped(x.state, other) == p)
                    lose_all_grasps(x.state, other);
            });
        }

        // Lizard.CarryObject: the slugcat held in front of the head
        void carry_object(ctx& x) {
            auto& w = x.state.world;
            auto const p = player_of(x.state, x.z.grasp_player);
            if (!p || !player_here(x.state, p, x.room.id)) {
                lose_all_grasps(x.state, x.id);
                return;
            }
            if (x.rng.value() < 0.025f && !ai::player_relationship(x.state, x.id, p).eats) {
                lose_all_grasps(x.state, x.id);
                return;
            }
            vec2 const hold = x.pos(0) + custom::dir_vec(x.pos(1), x.pos(0)) * 25.f * x.p.head_size;
            auto const gc = player::body_chunk(x.state, p, x.z.grasp_chunk);
            vec2 const rel = w.body_chunk_get_vel(gc) - x.vel(0);
            w.body_chunk_set_vel(gc, x.vel(0));
            if (rw::magnitude(rel) * w.body_chunk_get_mass(gc) > 30.f || !custom::dist_less(hold, w.body_chunk_get_pos(gc), 70.f + w.body_chunk_get_rad(gc))) {
                lose_all_grasps(x.state, x.id);
                return;
            }
            // BodyChunk.MoveFromOutsideMyUpdate
            w.body_chunk_set_pos(gc, hold);
            // PhysicalObject.PushOutOf: the slugcat's other chunk out of the head and middle
            for (uint32_t i = 0; i < 2; ++i) {
                auto const other = player::body_chunk(x.state, p, 1 - x.z.grasp_chunk);
                vec2& op = w.body_chunk_get_pos(other);
                float const r = x.rad(i) + w.body_chunk_get_rad(other);
                if (custom::dist_less(op, x.pos(i), r)) {
                    float const dist = rw::distance(op, x.pos(i));
                    vec2 const dir = custom::dir_vec(x.pos(i), op);
                    op += dir * (r - dist);
                    w.body_chunk_get_vel(other) += dir * (r - dist);
                }
            }
        }

        // Player.dangerGrasp: a slugcat in a lizard's jaws lets go of what it
        // holds after 30 ticks, and it is over for it at 60 (Player.Update,
        // RainWorldGame.GameOver)
        void danger_grasp(ctx& x) {
            auto const p = player_of(x.state, x.z.grasp_player);
            if (!p)
                return;
            ++x.z.danger_grasp_time;
            if (x.z.danger_grasp_time == 30)
                items::release_all(x.state, p);
            else if (x.z.danger_grasp_time == 60 && !x.state.world.player_get_dead(p))
                player::die(x.state, p);
        }

        //
        // LizardTongue
        //

        float tot_range(ctx& x, tongue const& t) { return x.p.tongue_p.tot_r * t.reel_in; }
        float el_range(ctx& x, tongue const& t) { return x.p.tongue_p.tot_r * x.p.tongue_p.elastic_range * t.reel_in * t.reel_in; }
        float drag(ctx& x, tongue const& t, float base) {
            auto const& tp = x.p.tongue_p;
            if (tp.base_drag_only)
                return base * t.dist;
            float const el = el_range(x, t), tot = tot_range(x, t);
            return mathf::max(base * t.dist, (t.dist < tot ? tp.drag_elasticity : 1.f) * (t.dist - el) * mathf::inverse_lerp(el, tot, t.dist));
        }

        // LizardTongue.Impact: drips, and the tongue's segments shaken
        void tongue_impact(ctx& x, tongue const& t) {
            float const speed = rw::magnitude(t.vel);
            for (int i = 0; i < 3; ++i)
                particles::spawn(x.state, x.room.id, particles::kind::drop, t.pos, (-t.vel + custom::rnv(x.rng) * speed * x.rng.value()) * x.rng.value(),
                    1.5f, 0.6f, vec3{0.75f, 0.8f, 0.85f}, 0.6f);
            // (graphicsModule != null; rolled either way, so the generator
            // doesn't depend on the camera)
            for (int32_t i = 0; i < x.p.tongue_segments && i < max_tongue; ++i) {
                vec2 const dp = custom::rnv(x.rng) * x.rng.value() * 0.5f * speed;
                vec2 const dv = custom::rnv(x.rng) * x.rng.value() * 0.5f * speed;
                if (x.viewed) {
                    x.g.tongue[i].pos += dp;
                    x.g.tongue[i].vel += dv;
                }
            }
        }

        // the first slugcat chunk the segment from `a` to `b` passes within
        // `rad` of (SharedPhysics.TraceProjectileAgainstBodyChunks)
        bool trace_players(sys::game_state const& state, dcon::room_id room, vec2 a, vec2 b, float rad, dcon::player_id& hit, uint32_t& k, vec2& at) {
            auto const& w = state.world;
            float best = 2.f;
            rooms::for_each_player(state, room, [&](dcon::player_id p) {
                if (!player_here(state, p, room))
                    return;
                for (uint32_t i = 0; i < 2; ++i) {
                    auto const c = player::body_chunk(state, p, i);
                    vec2 const cp = w.body_chunk_get_pos(c);
                    float const r = rad + w.body_chunk_get_rad(c);
                    vec2 const dv = b - a;
                    float const len2 = rw::sqr_magnitude(dv);
                    float const t = len2 > 0.f ? mathf::clamp01(rw::dot(cp - a, dv) / len2) : 0.f;
                    if (custom::dist_less(a + dv * t, cp, r) && t < best) {
                        best = t;
                        hit = p;
                        k = i;
                        at = a + dv * t;
                    }
                }
            });
            return best <= 1.f;
        }

        // LizardTongue.PullAtChunk
        void pull_at_chunk(ctx& x, tongue& t, dcon::body_chunk_id att) {
            auto& w = x.state.world;
            vec2 const a = custom::dir_vec(x.pos(0), w.body_chunk_get_pos(att));
            float const am = w.body_chunk_get_mass(att);
            float const num = x.mass(0) / (x.mass(0) + am);
            float const d = drag(x, t, x.p.tongue_p.chunk_drag);
            x.vel(0) += a * d * (1.f - num);
            x.pos(0) += a * d * (1.f - num);
            w.body_chunk_get_vel(att) -= a * d * num * x.p.tongue_p.pull_at_chunk_ratio;
            w.body_chunk_get_pos(att) -= a * d * num * x.p.tongue_p.pull_at_chunk_ratio;
        }

        // LizardTongue.Update
        void tongue_update(ctx& x) {
            auto& w = x.state.world;
            auto& t = w.lizard_get_tongue(x.id);
            auto const& tp = x.p.tongue_p;
            t.graph_pos[1] = t.graph_pos[0];
            if (!x.consious() && t.state != tongue_state::hidden)
                t.state = tongue_state::retracting;
            auto const attached = [&]() -> std::optional<dcon::body_chunk_id> {
                auto const pl = player_of(x.state, t.attached_player);
                if (!pl)
                    return std::nullopt;
                return player::body_chunk(x.state, pl, t.attached_chunk);
            };
            bool const stuck = t.state == tongue_state::attatched || t.state == tongue_state::stuck_in_terrain;
            if (t.state != tongue_state::hidden) {
                if (t.state != tongue_state::retracting && !x.room.visual_contact(x.pos(0), t.pos))
                    t.state = tongue_state::retracting;
                t.dist = rw::distance(t.pos, x.pos(0));
                x.vel(0) += custom::dir_vec(x.pos(0), t.pos) * 4.f;
                x.vel(1) -= custom::dir_vec(x.pos(0), t.pos) * 4.f;
                if (stuck && (t.reel_in < 0.f || rw::dot(custom::dir_vec(x.pos(0), t.pos), custom::dir_vec(x.pos(1), x.pos(0))) < -0.5f || t.dist > tp.tot_r + tp.tot_r_extra_limit))
                    tongue_retract(x.state, x.id);
                if (t.state == tongue_state::stuck_in_terrain && t.dist < tp.detatch_min_distance_terrain)
                    tongue_retract(x.state, x.id);
                if (t.state == tongue_state::attatched && t.dist < tp.detatch_min_distance_creature)
                    tongue_retract(x.state, x.id);
                if (t.state == tongue_state::attatched || t.state == tongue_state::stuck_in_terrain) {
                    ++t.stuck_counter;
                    auto const pl = player_of(x.state, t.attached_player);
                    if (pl && ai::want_to_hold_with_tongue(x.state, x.id, x.room, pl, t.attached_chunk)) {
                        if (x.rng.value() < tp.involuntary_release_chance && t.dist > tp.elastic_range)
                            tongue_retract(x.state, x.id);
                    } else if (x.rng.value() < tp.voluntary_release_chance) {
                        tongue_retract(x.state, x.id);
                    }
                    if (x.is(breed::blue) && ((jaw_open(x.state, x.id) < 0.1f && x.rng.value() < 0.125f) || x.rng.value() < mathf::inverse_lerp(20.f, 120.f, float(t.stuck_counter))))
                        tongue_retract(x.state, x.id);
                } else {
                    t.stuck_counter = 0;
                }
            }
            switch (t.state) {
            case tongue_state::hidden:
                if (t.delay > 0)
                    --t.delay;
                t.last_pos = x.last_pos(0);
                t.pos = x.pos(0);
                t.graph_pos[0] = t.pos;
                t.vel = vec2{0.f, 0.f};
                return;
            case tongue_state::lashing_out: {
                t.last_pos = t.pos;
                t.pos += t.vel;
                if (x.room.point_submerged(t.pos))
                    t.vel *= 0.9f;
                else
                    t.vel.y -= 0.1f;
                t.graph_pos[0] = t.pos;
                float const tot = tot_range(x, t), el = el_range(x, t);
                if (!custom::dist_less(t.pos, x.pos(0), tot)) {
                    t.vel += custom::dir_vec(t.pos, x.pos(0)) * (t.dist - tot);
                    t.pos += custom::dir_vec(t.pos, x.pos(0)) * (t.dist - tot);
                } else if (!custom::dist_less(t.pos, x.pos(0), el)) {
                    t.vel += custom::dir_vec(t.pos, x.pos(0)) * (t.dist - el) * tp.empty_elasticity;
                }
                vec2 const fwd = custom::dir_vec(x.pos(1), x.pos(0));
                dcon::player_id hit;
                uint32_t k = 0;
                vec2 at;
                if (trace_players(x.state, x.room.id, t.last_pos, t.pos, 3.f, hit, k, at)) {
                    // (a slugcat is heavier than 0.2: no AttachedInSmallObject)
                    t.pos = at;
                    t.attached_player = player_ref(hit);
                    t.attached_chunk = uint8_t(k);
                    tongue_impact(x, t);
                    t.state = tongue_state::attatched;
                    auto const c = player::body_chunk(x.state, hit, k);
                    w.body_chunk_get_vel(c) += t.vel * (0.15f / w.body_chunk_get_mass(c));
                    x.sound_at(sound::sound_id::lizard_tongue_attatch_player, t.pos);
                } else if (x.room.get_tile(t.pos).t == world::terrain::solid) {
                    vec2 v = t.last_pos;
                    for (float f = 0.f; f < 1.f && x.room.get_tile(rw::lerp(t.last_pos, t.pos, f)).t != world::terrain::solid; f += 0.05f)
                        v = rw::lerp(t.last_pos, t.pos, f);
                    t.pos = v;
                    x.sound_at(sound::sound_id::lizard_tongue_attatch_terrain, v);
                    t.state = x.rng.value() < tp.attach_terrain_chance ? tongue_state::stuck_in_terrain : tongue_state::retracting;
                    tongue_impact(x, t);
                    t.vel = vec2{0.f, 0.f};
                } else if (rw::dot(rw::normalized(t.vel), fwd) < 0.f) {
                    vec2 v = t.last_pos;
                    bool behind = false;
                    if (tp.attaches_background_walls)
                        for (float f = 0.f; f < 1.f; f += 0.05f) {
                            if (x.room.get_tile(rw::lerp(t.last_pos, t.pos, f)).flags & world::tile_wall_behind) {
                                behind = true;
                                break;
                            }
                            v = rw::lerp(t.last_pos, t.pos, f);
                        }
                    if (tp.attaches_background_walls && ((x.room.get_tile(t.pos).flags & world::tile_wall_behind) || behind)) {
                        t.pos = v;
                        x.sound_at(sound::sound_id::lizard_tongue_attatch_terrain, v);
                        t.state = x.rng.value() < tp.attach_terrain_chance ? tongue_state::stuck_in_terrain : tongue_state::retracting;
                        tongue_impact(x, t);
                        t.vel = vec2{0.f, 0.f};
                    } else {
                        t.state = tongue_state::retracting;
                    }
                }
                set_jaw_open(x.state, x.id, mathf::clamp(jaw_open(x.state, x.id) + 0.4f, 0.f, 1.f));
                return;
            }
            case tongue_state::attatched: {
                auto const att = attached();
                auto const pl = player_of(x.state, t.attached_player);
                if (!att || !player_here(x.state, pl, x.room.id)) {
                    tongue_retract(x.state, x.id);
                } else {
                    t.pos = w.body_chunk_get_pos(*att);
                    t.vel = w.body_chunk_get_vel(*att);
                    t.last_pos = w.body_chunk_get_last_pos(*att);
                    t.graph_pos[0] = t.pos + custom::dir_vec(t.pos, x.pos(0)) * mathf::max(w.body_chunk_get_rad(*att) - 7.5f, 0.f);
                    t.reel_in -= tp.reel_in_speed;
                    if (x.rng.value() < 0.5f && !custom::dist_less(x.pos(0), t.pos, mathf::lerp(el_range(x, t), tot_range(x, t), 0.75f)))
                        t.reel_in = mathf::min(t.reel_in + 0.01f, 1.f);
                    pull_at_chunk(x, t, *att);
                }
                // (the slugcat's GraspWiggle isn't ported: it doesn't struggle free)
                return;
            }
            case tongue_state::retracting:
            case tongue_state::attached_in_small_object: {
                t.last_pos = t.pos;
                t.pos += t.vel;
                t.graph_pos[0] = t.pos;
                vec2 const back = x.pos(0) + custom::dir_vec(x.pos(1), x.pos(0)) * t.dist * 0.5f;
                t.vel = rw::lerp(t.vel, custom::dir_vec(t.pos, back) * (rw::distance(t.pos, back) + 5.f) * 0.5f, 0.5f);
                if (t.state == tongue_state::attached_in_small_object)
                    t.state = tongue_state::retracting;
                if (custom::dist_less(t.pos, x.pos(0), 10.f)) {
                    x.sound(sound::sound_id::lizard_tongue_go_back_in_mouth);
                    t.state = tongue_state::hidden;
                    t.attached_player = 0;
                }
                return;
            }
            case tongue_state::stuck_in_terrain: {
                t.graph_pos[0] = t.pos;
                t.vel = vec2{0.f, 0.f};
                vec2 const a = custom::dir_vec(x.pos(0), t.pos);
                float const d = drag(x, t, tp.terrain_drag);
                x.vel(0) += a * d;
                x.pos(0) += a * d;
                t.reel_in -= tp.reel_in_speed;
                return;
            }
            }
        }

        // LizardTongue.LashOut
        void tongue_lash_out(ctx& x, vec2 target) {
            auto& t = x.state.world.lizard_get_tongue(x.id);
            auto const& tp = x.p.tongue_p;
            target.y += rw::distance(x.pos(0), target) * 0.05f;
            vec2 const fwd = custom::dir_vec(x.pos(1), x.pos(0));
            vec2 const dir = custom::dir_vec(x.pos(0), target);
            if (rw::dot(fwd, dir) <= 0.3f)
                return;
            float const d = mathf::lerp(mathf::inverse_lerp(tp.tot_r * tp.elastic_range * 0.5f, tp.tot_r, rw::distance(x.pos(0), target)), 1.f, 0.75f);
            t.vel = dir * tp.lash_out_speed * d;
            t.state = tongue_state::lashing_out;
            t.reel_in = 1.f;
            t.delay = 20;
            t.bite_inanimate = false;
            set_jaw_open(x.state, x.id, 1.f);
            x.vel(1) -= dir * (x.is(breed::blue) ? 5.f : tp.lash_out_speed);
            int32_t const n = std::min(x.p.tongue_segments, max_tongue);
            for (int32_t i = 0; i < n; ++i) {
                vec2 v = dir * tp.lash_out_speed * d + custom::rnv(x.rng) * x.rng.value() * tp.lash_out_speed * d * 0.75f;
                if (x.is(breed::indigo)) {
                    for (int j = 0; j < 3; ++j)
                        particles::spawn(x.state, x.room.id, particles::kind::spark, x.pos(0) + v * x.rng.value() * 3.f, v * 0.25f * x.rng.value(), 2.f, 0.6f,
                            x.state.world.lizard_get_effect_color(x.id));
                } else {
                    particles::spawn(x.state, x.room.id, particles::kind::drop, x.pos(0), v * 0.5f * x.rng.value(), 1.5f, 0.6f, vec3{0.75f, 0.8f, 0.85f}, 0.6f);
                }
                // the tongue's segments are LizardGraphics' (graphicsModule != null)
                if (!x.viewed)
                    continue;
                if (!x.is(breed::blue))
                    v = rw::slerp(v, custom::dir_vec(x.pos(0), x.g.head.pos) * tp.lash_out_speed * d, float(i) / float(n));
                else
                    v = x.vel(0);
                x.g.tongue[i].vel = v;
            }
            x.sound(x.is(breed::blue) ? sound::sound_id::blue_lizard_tongue_shoot_out : sound::sound_id::white_lizard_tongue_shoot_out);
        }

        //
        // LizardPather.FollowPath over the path field
        //

        // MovementConnection from `from` along the field: Standard steps,
        // OpenDiagonals and drops (DropToFloor, DropToClimb onto a pole). With
        // a body direction, a step back along a floor isn't wanted: it becomes
        // the LizardTurn connection (AImapper offers one wherever there is
        // headroom over both tiles)
        movement_connection follow_path(ctx& x, pathing::field_id f, ivec2 from, std::optional<int32_t> body_dir) {
            auto const step = pathing::next_step(x.state, f, from);
            if (!step)
                return movement_connection{};
            movement_connection m{move_type::standard, from, *step, true};
            ivec2 const d = *step - from;
            if (d.x != 0 && d.y != 0)
                m.type = move_type::open_diagonal;
            else if (d.y < -1)
                m.type = x.acc(*step) == access::climb ? move_type::drop_to_climb : move_type::drop_to_floor;
            bool const falling = !x.narrow(from) && ai::fall_risk_tile(x.room, from).y < from.y - 5;
            if (!falling && body_dir && m.type == move_type::standard && x.acc(from) == access::floor) {
                ivec2 const s = custom::clamp_at_one(d);
                ivec2 const bd = custom::four_directions[*body_dir];
                if ((s.x != 0 || x.narrow(from)) && s.x == -bd.x && s.y == -bd.y && s.y == 0 && x.acc(*step) == access::floor
                && !x.room.get_tile(from + ivec2{0, 1}).solid() && !x.room.get_tile(*step + ivec2{0, 1}).solid())
                    m.type = move_type::lizard_turn;
            }
            return m;
        }

        // PathFinder.RayTraceInAccessibleTiles
        bool ray_trace_in_accessible_tiles(ctx& x, ivec2 a, ivec2 b) {
            int32_t const n = std::max(std::abs(b.x - a.x), std::abs(b.y - a.y));
            for (int32_t i = 0; i <= n; ++i) {
                vec2 const f = rw::lerp(rw::to_vec2(a), rw::to_vec2(b), n == 0 ? 0.f : float(i) / float(n));
                if (!x.accessible(ivec2{int32_t(std::lround(f.x)), int32_t(std::lround(f.y))}))
                    return false;
            }
            return true;
        }

        //
        // Lizard.MovementAnimation
        //

        movement_animation make_movement_animation(ctx& x, movement_connection const& conn, int32_t step, float friction, float speed) {
            movement_animation m;
            m.active = true;
            m.connection = conn;
            m.step = step;
            m.friction = friction;
            m.speed = speed;
            vec2 const s = room_view::middle_of_tile(conn.start), d = room_view::middle_of_tile(conn.dest);
            auto const add = [&](uint8_t part, vec2 at, float finished) {
                m.destinations[m.count++] = movement_animation::destination{part, at, finished, rw::distance(x.pos(part), at) + 10.f};
            };
            switch (conn.type) {
            case move_type::standard:
                m.total_steps = 1;
                add(0, d, 5.f);
                add(2, s, 5.f);
                break;
            case move_type::reach_up:
                m.total_steps = 2;
                if (step == 1) {
                    add(0, (s + d) * 0.5f, 10.f);
                } else {
                    add(0, d, 5.f);
                    add(1, (s + d) * 0.5f, 100.f);
                    add(2, s, 12.f);
                }
                break;
            case move_type::drop_to_climb:
                m.total_steps = 1;
                add(0, d + vec2{0.f, 5.f}, 5.f);
                add(1, s, 12.f);
                break;
            case move_type::lizard_turn:
                m.total_steps = 2;
                if (step == 1) {
                    add(0, s, 8.f);
                    add(1, room_view::middle_of_tile(conn.start + ivec2{0, 1}), 9.f);
                    add(2, room_view::middle_of_tile(conn.dest + ivec2{0, 1}), 10.f);
                } else {
                    add(0, d, 5.f);
                    add(1, s, 5.f);
                    add(2, room_view::middle_of_tile(conn.start + ivec2{0, 1}), 8.f);
                }
                break;
            default:
                break;
            }
            return m;
        }

        // MovementAnimation.Update
        void movement_animation_update(ctx& x, float speed_fac) {
            auto& m = x.z.move_anim;
            bool finished = true;
            bool failed = false;
            for (uint8_t i = 0; i < m.count; ++i) {
                auto const& d = m.destinations[i];
                vec2 const at = x.pos(d.part);
                if (finished && !custom::dist_less(at, d.pos, d.finished_dist))
                    finished = false;
                x.vel(d.part) *= m.friction;
                float num = m.speed * (d.part == 0 ? body_force(x) : 1.f) * mathf::pow(x.a.run_speed, 0.2f);
                if (custom::dist_less(at, d.pos, num))
                    num = rw::distance(at, d.pos);
                num *= body_desperation(x) * x.p.max_muscle_power * 2.f * x.rng.value() + 1.f;
                x.vel(d.part) += custom::dir_vec(at, d.pos) * num * speed_fac;
                x.vel(d.part) += custom::rnv(x.rng) * body_desperation(x) * x.p.max_muscle_power * 2.f * x.rng.value() * m.speed;
                if (!custom::dist_less(x.pos(d.part), d.pos, d.max_distance)) {
                    finished = true;
                    failed = true;
                    break;
                }
            }
            if (!finished)
                return;
            if (m.step < m.total_steps && !failed) {
                m = make_movement_animation(x, m.connection, m.step + 1, m.friction, m.speed);
                return;
            }
            // Lizard.MovementAnimationEnded
            m.active = false;
        }

        //
        // Lizard.Act and its movement
        //

        // Lizard.GetFrameSpeed
        float get_frame_speed(ctx& x, float run_speed) {
            float num = float(legs_gripping(x.state, x.id)) / 4.f;
            int32_t const no_grip = no_grip_counter(x.state, x.id);
            num = num * (1.f - x.p.no_grip_speed) + x.p.no_grip_speed;
            if (no_grip < 1)
                num = mathf::lerp(num, 1.f + body_desperation(x), body_force(x) - 1.f);
            if (num == 0.f && body_wiggle_fac(x) > 0.f)
                for (uint32_t i = 0; i < 3; ++i)
                    if (x.contact(i) != ivec2{0, 0}) {
                        num = body_wiggle_fac(x) * (0.5f + 0.5f * std::sin(x.z.body_wiggle * 2.f * mathf::pi)) * 0.2f;
                        break;
                    }
            if (num < 0.1f || body_force(x) > 1.2f)
                set_wiggle(x, x.z.body_wiggle_counter + 1);
            else
                set_wiggle(x, x.z.body_wiggle_counter - 1);
            float speed = x.p.base_speed * num * mathf::lerp(run_speed, 1.f, body_force(x) - 1.f);
            speed *= mathf::lerp(x.rng.value(), 1.f, (x.health() - 0.5f) * 2.f);
            speed_multiplier sm{0.f, 0.f, 0.f, 0.f};
            for (uint32_t i = 0; i < 3; ++i) {
                auto const& t = x.p.terrain_speeds[uint32_t(x.acc(x.pos(i)))];
                sm.speed += t.speed;
                sm.horizontal += t.horizontal;
                sm.up += t.up;
                sm.down += t.down;
            }
            if (x.is(breed::eel) && (x.narrow(room_view::get_tile_position(x.pos(0))) || x.room.get_tile(x.pos(0)).t == world::terrain::shortcut_entrance))
                speed *= 1.75f;
            speed *= sm.speed / 3.f;
            ivec2 const d = x.z.following.dest - x.z.following.start;
            if (d.x != 0)
                speed *= sm.horizontal / 3.f;
            else if (d.y > 0)
                speed *= sm.up / 3.f;
            else
                speed *= sm.down / 3.f;
            return speed;
        }

        // Lizard.FollowConnection
        void follow_connection(ctx& x, float run_speed) {
            auto& z = x.z;
            auto const& fc = z.following;
            float const frame_speed = get_frame_speed(x, run_speed);
            if (z.time_spent_trying_this_move < 20 && z.narrow_upcoming && frame_speed > 0.2f && z.upcoming_count > 1 && x.narrow(z.upcoming[1].dest)) {
                ivec2 const a = custom::clamp_at_one(fc.dest - fc.start);
                ivec2 const b = custom::clamp_at_one(z.upcoming[1].dest - z.upcoming[1].start);
                if (a.x != b.x && a.y != b.y) {
                    x.vel(0) *= 0.75f;
                    x.vel(0) += rw::to_vec2(b) * 2.f;
                    run_speed = mathf::min(run_speed, 0.2f);
                    x.vel(1) *= 0.75f;
                    x.vel(2) *= 0.5f;
                }
            }
            ivec2 const here_tile = here(x.state, x.id);
            if (fc.type == move_type::standard || fc.type == move_type::open_diagonal) {
                vec2 b{0.f, 0.f};
                if (x.acc(fc.dest) == access::climb) {
                    ivec2 s = custom::clamp_at_one((z.upcoming_count > 0 ? z.upcoming[std::min(2, z.upcoming_count - 1)].dest : fc.dest) - fc.start);
                    if (fc.dest.y == fc.start.y) {
                        b.y = float(s.y) * 8.f;
                    } else if (fc.dest.x == fc.start.x) {
                        if (x.acc(fc.start) != access::climb)
                            s.x = x.pos(2).x < room_view::middle_of_tile(x.pos(0)).x ? -1 : 1;
                        b.x = float(s.x) * 8.f;
                    }
                    if (x.solid(0, s.x, 0)) {
                        s.x *= -1;
                        b.x = 0.f;
                    }
                    if (x.solid(0, 0, s.y)) {
                        s.y *= -1;
                        b.y = 0.f;
                    }
                } else if (no_grip_counter(x.state, x.id) == 0 && fc.dest.y == fc.start.y && !x.solid(0, 0, 1)
                && !x.solid(0, (fc.dest.x < here_tile.x ? -1 : 0) + (fc.dest.x > here_tile.x ? 1 : 0), 1)) {
                    b.y = x.p.floor_leverage;
                }
                vec2 const mid = room_view::middle_of_tile(x.pos(0));
                if (fc.start.x != fc.dest.x && fc.start.y == fc.dest.y) {
                    if ((x.solid(0, 0, -1) && x.pos(0).y < mid.y) || (x.solid(0, 0, 1) && x.pos(0).y > mid.y))
                        x.vel(0).y -= (x.pos(0).y - mid.y) * 0.5f;
                } else if (fc.start.y != fc.dest.y && fc.start.x == fc.dest.x
                && ((x.solid(0, -1, 0) && x.pos(0).x < mid.x) || (x.solid(0, 1, 0) && x.pos(0).x > mid.x))) {
                    x.vel(0).x -= (x.pos(0).x - mid.x) * 0.5f;
                }
                ivec2 const step = fc.dest - fc.start;
                if (step.y != 0) {
                    for (uint32_t i = 0; i < 3; ++i)
                        if (x.contact(i).y != 0 && !x.solid(i, 0, x.contact(i).y)) {
                            x.vel(i).x -= (x.pos(i).x - room_view::middle_of_tile(x.pos(i)).x) * 0.25f;
                            b.x *= 0.f;
                        }
                } else if (step.x != 0) {
                    for (uint32_t i = 0; i < 3; ++i)
                        if (x.contact(i).x != 0 && !x.solid(i, x.contact(i).x, 0)) {
                            x.vel(i).y -= (x.pos(i).y - room_view::middle_of_tile(x.pos(i)).y) * 0.25f;
                            b.y *= 0.f;
                        }
                }
                vec2 goal = room_view::middle_of_tile(fc.dest);
                if (z.upcoming_count > 0 && x.acc(z.upcoming[z.upcoming_count - 1].dest) < access::climb) {
                    ivec2 const at = room_view::get_tile_position(x.pos(0));
                    for (int32_t k = z.upcoming_count - 1; k >= 0; --k)
                        if ((float_dist(at, z.upcoming[k].dest) < 2.f && x.room.visual_contact(x.pos(0), room_view::middle_of_tile(z.upcoming[k].dest)))
                        || ray_trace_in_accessible_tiles(x, at, z.upcoming[k].dest)) {
                            goal = room_view::middle_of_tile(z.upcoming[k].dest);
                            break;
                        }
                }
                goal += b;
                float const link = 17.f * x.p.body_length_fac * ((x.p.body_size_fac + 1.f) / 2.f);
                vec2 const pp = goal + custom::dir_vec(goal, x.pos(0)) * link;
                float const force = body_force(x);
                x.vel(0) += custom::dir_vec(x.pos(0), goal) * frame_speed * force;
                x.vel(1) += custom::dir_vec(x.pos(1), pp) * frame_speed * force
                    * custom::lerp_map(rw::dot(custom::dir_vec(x.pos(1), goal), custom::dir_vec(x.pos(1), pp)), -1.f, 1.f, 0.5f, 1.f);
                if (z.anim == animation::standard && x.a.run_speed > 0.1f) {
                    float num = mathf::inverse_lerp(0.f, -1.f, rw::dot(custom::dir_vec(x.pos(0), goal), custom::dir_vec(x.pos(1), x.pos(0))));
                    if (num > 0.5f)
                        z.straighten_out_needed = custom::lerp_and_tick(z.straighten_out_needed, 1.f, 0.1f, 0.1f);
                    num = mathf::lerp(num, 1.f, mathf::inverse_lerp(20.f, 40.f, float(z.time_spent_trying_this_move)));
                    num *= mathf::max(mathf::max((z.narrow_upcoming || z.shortcut_upcoming) ? 1.f : 0.2f, z.climb_upcoming ? 0.5f : 0.2f),
                        mathf::max(mathf::inverse_lerp(5.f, 20.f, float(z.time_spent_trying_this_move)), z.straighten_out_needed));
                    float const push = num * 2.f * force + body_desperation(x) * 2.f;
                    float const back = push * 0.5f * custom::lerp_map(float(z.time_spent_trying_this_move), 20.f, 60.f, 1.f, 0.5f) * frame_speed;
                    vec2 const dir = custom::dir_vec(x.pos(1), goal);
                    x.vel(0) += dir * push * frame_speed;
                    x.vel(1) -= dir * back;
                    x.vel(2) -= dir * back;
                }
                return;
            }
            if (fc.type != move_type::drop_to_floor && fc.type != move_type::drop_to_climb)
                return;
            bool commit = !(std::abs(here_tile.x - fc.start.x) > 4 && std::abs(here_tile.x - fc.dest.x) > 4);
            for (uint32_t i = 0; i < 3; ++i)
                if (x.contact(i).y < 0)
                    commit = false;
            if (commit)
                z.commited_to_drop = fc;
            x.vel(0).y -= 0.01f;
            x.vel(2).y += 0.01f;
            vec2 const dm = room_view::middle_of_tile(fc.dest);
            if (std::fabs(x.pos(0).x - dm.x) < 5.f) {
                if (std::fabs(x.vel(0).x) < 5.f)
                    x.vel(0).x *= 0.8f;
            } else if (x.pos(0).x < dm.x) {
                x.vel(0).x += 0.5f;
            } else {
                x.vel(0).x -= 0.5f;
            }
            bool landed = false, over_start = false, above = false;
            float const force = body_force(x);
            for (uint32_t i = 0; i < 3; ++i) {
                if (x.contact(i).y < 0)
                    landed = true;
                ivec2 const t = room_view::get_tile_position(x.pos(i));
                if (t.y >= fc.start.y) {
                    above = true;
                    if (t.x == fc.start.x) {
                        x.vel(i) += custom::dir_vec(x.pos(i), dm) * frame_speed * (i == 0 ? force : 1.f);
                        over_start = true;
                    } else {
                        x.vel(i) += custom::dir_vec(x.pos(i), room_view::middle_of_tile(fc.start)) * frame_speed * (i == 0 ? force : 1.f);
                    }
                }
                if (t.x == fc.start.x)
                    x.vel(i).y -= 1.f;
            }
            if (over_start)
                return;
            if (z.upcoming_count > 0) {
                float const dx = float(fc.dest.x - z.upcoming[z.upcoming_count - 1].dest.x);
                x.vel(0).x -= 0.3f * dx;
                x.vel(2).x += 0.3f * dx;
            }
            if (x.pos(0).y <= x.last_pos(0).y) {
                int32_t const from = room_view::get_tile_position(x.last_pos(0)).y + 1;
                int32_t const to = room_view::get_tile_position(x.pos(0)).y - 1;
                int32_t const cx = room_view::get_tile_position(x.pos(0)).x;
                if (fc.type == move_type::drop_to_climb) {
                    for (int32_t n = from; n >= to; --n)
                        if (n <= fc.dest.y && x.acc(ivec2{cx, n}) == access::climb) {
                            x.sound(sound::sound_id::lizard_grab_pole);
                            z.has_grip_point = true;
                            z.grip_point = room_view::middle_of_tile(cx, n);
                            x.vel(0).y = 0.f;
                            break;
                        }
                } else if (fc.dest.x != fc.start.x && std::abs(cx - fc.dest.x) < 2) {
                    for (int32_t n = from; n >= to; --n)
                        if (n <= fc.dest.y && x.acc(ivec2{fc.dest.x, n}) == access::floor) {
                            z.has_grip_point = true;
                            z.grip_point = room_view::middle_of_tile(fc.dest.x, n);
                            x.vel(0).y = 0.f;
                            break;
                        }
                }
            }
            if (landed && !above && !z.has_grip_point)
                z.following = movement_connection{};
        }

        // Lizard.GripPointBehavior
        void grip_point_behavior(ctx& x, pathing::field_id f) {
            auto& z = x.z;
            if (rw::magnitude(x.vel(0)) > 10.f && z.in_allowed_terrain_counter > 5) {
                z.has_grip_point = false;
                z.following = movement_connection{};
            } else if (!z.apply_gravity) {
                movement_connection m = follow_path(x, f, room_view::get_tile_position(x.pos(0)), 0);
                z.move_anim.active = false;
                if (z.following.valid && z.following.type == move_type::drop_to_climb && m.valid && m.dest.y - m.start.y == 0 && std::abs(m.dest.x - m.start.x) == 1) {
                    m.type = move_type::drop_to_climb;
                    z.move_anim = make_movement_animation(x, m, 1, 0.99f, 0.75f);
                }
                z.has_grip_point = false;
                z.following = movement_connection{};
            } else {
                if (x.pos(1).y < x.pos(0).y)
                    x.vel(1).y += x.rng.value() * 2.f;
                if (z.upcoming_count > 0) {
                    float const dx = float(custom::clamp_at_one(room_view::get_tile_position(x.pos(0)) - z.upcoming[z.upcoming_count - 1].dest).x);
                    x.vel(1).x += 3.f * dx;
                    x.vel(2).x += x.rng.value() * 3.f * dx;
                } else {
                    x.vel(1).x += -2.f + x.rng.value() * 4.f;
                }
            }
            x.vel(0) *= 0.f;
            if (z.has_grip_point)
                x.vel(0) -= x.pos(0) - z.grip_point;
        }

        // Lizard.SwimBehavior
        void swim_behavior(ctx& x, pathing::field_id f) {
            auto& z = x.z;
            z.swim = mathf::clamp(z.swim + 0.06666667f, 0.f, 1.f);
            z.desperation_smoother = mathf::lerp(z.desperation_smoother, 30.f, 0.1f);
            // (no Remix: every lizard bobs at the surface)
            if (x.submersion(0) > 0.f && x.submersion(0) < 1.f && (!z.following.valid || z.following.dest.y == x.room.default_water_level)) {
                x.vel(0).y *= 0.8f;
                x.vel(0).y += mathf::clamp(x.room.float_water_level - x.pos(0).y, -10.f, 10.f) * 0.1f;
            }
            ivec2 wc = room_view::get_tile_position(x.pos(0));
            ivec2 const dest = x.a.destination;
            if (x.p.swimmer) {
                if (x.a.current == behavior::lurk && custom::manhattan_distance(dest, room_view::get_tile_position(x.pos(2))) < 3) {
                    x.vel(2) += rw::clamp_magnitude(room_view::middle_of_tile(dest) - x.pos(2), 10.f) / 7.f;
                    x.vel(0) += custom::dir_vec(x.pos(2), room_view::middle_of_tile(x.a.lurk.look_position)) * 0.2f;
                    z.salamander_lurk = true;
                } else {
                    z.salamander_lurk = false;
                    vec2 a{0.f, 0.f};
                    if (x.any_water(dest) && x.room.visual_contact(x.pos(0), room_view::middle_of_tile(dest))) {
                        // (the destination is never a node here: straight at it in open water)
                        a = custom::dir_vec(x.pos(0), room_view::middle_of_tile(dest));
                    } else {
                        if (!z.following.valid)
                            z.following = follow_path(x, f, wc, std::nullopt);
                        if (z.following.valid)
                            a = custom::dir_vec(x.pos(0), room_view::middle_of_tile(z.following.dest));
                    }
                    if (x.is(breed::eel))
                        a *= 1.4f;
                    bool const surface = x.room.get_tile(x.pos(0)).water == 2;
                    x.vel(0) += a * 1.3f * mathf::lerp(0.5f, 1.f, x.a.run_speed) * (surface ? 0.7f : 1.f);
                    x.vel(1) -= a * 0.3f * mathf::lerp(0.5f, 1.f, x.a.run_speed) * (surface ? 0.7f : 1.f);
                }
            } else {
                wc.y = custom::int_clamp(wc.y, x.room.default_water_level, x.room.height);
                if (!z.following.valid) {
                    z.following = follow_path(x, f, wc, std::nullopt);
                    z.time_spent_trying_this_move = 0;
                }
                if (z.swim > 0.5f)
                    z.in_allowed_terrain_counter = 0;
                if (z.following.valid) {
                    x.vel(0).x += mathf::clamp(room_view::middle_of_tile(z.following.dest).x - x.pos(0).x, -1.f, 1.f) * x.p.swim_speed * body_force(x);
                    if (x.any_water(z.following.dest) && room_view::get_tile_position(x.pos(0)) == z.following.dest)
                        z.following = movement_connection{};
                }
            }
            if (z.following.valid) {
                if (wc.x != z.following.start.x && wc.x != z.following.dest.x)
                    z.following = movement_connection{};
                else if (wc == z.following.dest)
                    z.following = movement_connection{};
            }
            if (z.following.valid && !x.any_water(z.following.dest)) {
                // climbing out onto the bank
                z.in_allowed_terrain_counter = x.p.regain_footing_counter + 2;
                z.swim = 0.f;
                if (x.accessible(z.following.dest + ivec2{0, 1}))
                    z.move_anim = make_movement_animation(x, movement_connection{move_type::reach_up, z.following.start, z.following.dest + ivec2{0, 1}, true}, 1, 0.99f, 0.75f);
            }
        }

        // LizardJumpModule.NoRunBehavior
        bool no_run_behavior(sys::game_state const& state, dcon::lizard_id l) {
            auto const& j = state.world.lizard_get_jump(l);
            return j.has_act_on && state.world.lizard_get_core(l).anim == animation::jumping && j.jump_counter < j.act_on.best.tick;
        }

        void jump_now(ctx& x);
        float act_animation(ctx& x);
        void spit_now(ctx& x, vec2 at, vec2 dir);

        // Lizard.Act
        void act(ctx& x) {
            auto& z = x.z;
            ai::update(x.state, x.id, x.room);
            bool head_ok = false, rest_ok = false;
            for (uint32_t i = 0; i < 3; ++i)
                if (x.accessible(x.pos(i))) {
                    head_ok = head_ok || i == 0;
                    rest_ok = rest_ok || i > 0;
                }
            if ((head_ok || rest_ok || z.move_anim.active) && z.in_allowed_terrain_counter < 100) {
                ++z.in_allowed_terrain_counter;
            } else {
                bool near = false;
                for (uint32_t i = 0; i < 3 && !near; ++i)
                    for (auto const d : custom::four_directions) {
                        ivec2 const t = room_view::get_tile_position(x.pos(i)) + d;
                        if (x.accessible(t) && !x.any_water(t)) {
                            near = true;
                            break;
                        }
                    }
                z.in_allowed_terrain_counter = near ? std::max(0, z.in_allowed_terrain_counter - 10) : 0;
            }
            z.apply_gravity = z.in_allowed_terrain_counter < x.p.regain_footing_counter || no_grip_counter(x.state, x.id) > 10 || z.commited_to_drop.valid;
            z.swim = mathf::clamp(z.swim - 0.05f, 0.f, 1.f);
            x.a.stuck.satisfied_with_this_position = z.swim > 0.5f;
            float num4 = act_animation(x);
            if (x.is(breed::eel) && x.submersion() == 0.f) {
                ivec2 const t = room_view::get_tile_position(x.pos(0));
                if (!x.narrow(t) && x.room.get_tile(t).t != world::terrain::shortcut_entrance) {
                    ++z.snake_ticker;
                    num4 *= mathf::lerp(0.25f, 1.f, snake_coil(x));
                }
            }
            auto const f = ai::field(x.state, x.id, x.room);
            ivec2 wc = room_view::get_tile_position(x.pos(0));
            if (!z.move_anim.active && x.room.inside(wc.x, wc.y) && !ai::reachable_and_getbackable(x.state, x.id, x.room, wc))
                for (auto const d : custom::eight_directions_diagonals_last)
                    if (ai::reachable_and_getbackable(x.state, x.id, x.room, wc + d)) {
                        wc += d;
                        break;
                    }
            z.following = follow_path(x, f, wc, body_direction(x));
            if (z.commited_to_drop.valid) {
                z.following = z.commited_to_drop;
                z.commited_to_drop = movement_connection{};
            }
            // the chunk connections' types (Update sets both to Normal again
            // before the physics, as vanilla does)
            for (uint32_t i = 0; i < 3; ++i) {
                x.state.world.body_chunk_set_terrain_squeeze(x.c[i], custom::lerp_map(float(z.time_spent_trying_this_move), 10.f, 30.f, 1.f, 0.05f));
                x.state.world.body_chunk_set_collide_with_slopes(x.c[i], z.time_spent_trying_this_move < 30);
            }
            auto& st = x.a.stuck;
            auto const in_log = [&](movement_connection const& m, int32_t back) {
                if (!m.valid)
                    return false;
                for (int32_t i = 0; i < std::min(back, std::min(st.backlog_head, int32_t(stuck_tracker::backlog_size))); ++i)
                    if (st.backlog[i] == m)
                        return true;
                return false;
            };
            if (num4 > 0.1f && (!z.following.valid || in_log(z.following, int32_t(mathf::lerp(1.f, 20.f, num4)))))
                z.time_spent_trying_this_move = custom::int_clamp(z.time_spent_trying_this_move + 1, 0, 600);
            else
                z.time_spent_trying_this_move = 0;
            float const stuck_utility = mathf::inverse_lerp(100.f, 200.f, float(st.counter));
            z.desperation_smoother = custom::lerp_and_tick(z.desperation_smoother,
                mathf::max(float(z.time_spent_trying_this_move) + mathf::lerp(-300.f, 0.f, num4), stuck_utility * 100.f), 0.05f, 0.5f);
            if (z.desperation_smoother > mathf::lerp(40.f, 10.f, num4))
                set_wiggle(x, z.body_wiggle_counter + 2);
            else
                set_wiggle(x, z.body_wiggle_counter - 1);
            // MoveBacklog.ReportNewMove (backlog_head counts the entries, newest first)
            if (z.following.valid && (st.backlog_head == 0 || !(st.backlog[0] == z.following))) {
                for (int32_t i = stuck_tracker::backlog_size - 1; i > 0; --i)
                    st.backlog[i] = st.backlog[i - 1];
                st.backlog[0] = z.following;
                st.backlog_head = std::min(st.backlog_head + 1, int32_t(stuck_tracker::backlog_size));
            }
            z.upcoming_count = 0;
            if (z.following.valid) {
                z.limbs_aim_for = room_view::middle_of_tile(z.following.dest);
                // (no shortcuts for lizards: ShortCut connections don't come up)
                if (z.following.type == move_type::lizard_turn)
                    z.move_anim = make_movement_animation(x, z.following, 1, 0.99f, 0.75f);
                movement_connection m = z.following;
                z.narrow_upcoming = false;
                z.climb_upcoming = false;
                z.shortcut_upcoming = false;
                for (int k = 0; k < 5; ++k) {
                    auto const solid = [&](ivec2 d) { return x.room.get_tile(m.dest + d).solid(); };
                    if (!z.narrow_upcoming && ((solid(ivec2{0, 1}) && solid(ivec2{0, -1})) || (solid(ivec2{1, 0}) && solid(ivec2{-1, 0}))))
                        z.narrow_upcoming = true;
                    if (!z.climb_upcoming && x.acc(m.dest) == access::climb)
                        z.climb_upcoming = true;
                    z.upcoming[z.upcoming_count++] = m;
                    m = follow_path(x, f, m.dest, std::nullopt);
                    for (int32_t i = 0; i < z.upcoming_count && m.valid; ++i)
                        if (z.upcoming[i].dest == m.dest)
                            m = movement_connection{};
                    if (!m.valid)
                        break;
                }
            }
            ++z.time_in_animation;
            if (z.time_to_remain_in_animation > -1 && z.time_in_animation > z.time_to_remain_in_animation)
                enter_animation(x.state, x.id, animation::standard, true);
            if (z.bubble == 0 && x.rng.value() < 0.05f && x.rng.value() < x.a.excitement && x.rng.value() < x.a.excitement) {
                z.bubble_intensity = x.a.excitement * x.a.excitement * x.rng.value();
                z.bubble = 1;
            }
            if (x.is(breed::cyan) && !z.has_grip_point && no_run_behavior(x.state, x.id))
                return;
            if (z.has_grip_point) {
                grip_point_behavior(x, f);
            } else if (z.move_anim.active) {
                movement_animation_update(x, num4 * x.p.base_speed * 0.5f);
            } else if (z.following.valid && !x.narrow(here(x.state, x.id)) && !x.any_water(z.following.dest) && x.submersion() > 0.5f) {
                swim_behavior(x, f);
            } else if ((z.following.valid && x.any_water(z.following.dest)) || (!z.following.valid && x.submersion() > 0.5f)) {
                swim_behavior(x, f);
            } else if (z.following.valid) {
                follow_connection(x, num4);
            }
            if (!z.following.valid && !z.move_anim.active) {
                if (!z.apply_gravity && !x.accessible(x.pos(0))) {
                    ivec2 const t = room_view::get_tile_position(x.pos(0)) + custom::four_directions[x.rng.range(0, 4)];
                    if (x.accessible(t))
                        z.following = movement_connection{move_type::standard, room_view::get_tile_position(x.pos(0)), t, true};
                } else if (head_ok && !rest_ok && z.time_spent_trying_this_move > 20) {
                    for (auto const d : custom::four_directions) {
                        ivec2 const t = room_view::get_tile_position(x.pos(1)) + d;
                        if (x.accessible(t)) {
                            x.vel(1) += custom::dir_vec(x.pos(1), room_view::middle_of_tile(t));
                            break;
                        }
                    }
                }
            }
            if (body_desperation(x) > 0.f && z.swim == 0.f) {
                vec2 const v = custom::rnv(x.rng) * body_desperation(x) * x.p.max_muscle_power * 2.f * x.rng.value();
                x.vel(0) += v * 0.5f;
                x.vel(1) -= v;
                x.vel(2) += v * 0.5f;
            }
            // bodyChunkConnections[2].elasticity
            x.state.world.physical_object_for_each_chunk_connection_as_owner(x.o, [&](dcon::chunk_connection_id k) {
                if (x.state.world.chunk_connection_get_type(k) == physics::connection_type::push)
                    x.state.world.chunk_connection_set_elasticity(k, 0.7f + 0.3f / body_force(x));
            });
            if (z.anim == animation::shake_prey) {
                set_wiggle(x, custom::int_clamp(50 - z.time_in_animation, 0, 100));
                vec2 v = custom::rnv(x.rng) * 12.f * x.rng.value();
                if (!z.grasp_player) {
                    v *= 0.5f;
                } else {
                    // the slugcat's chunk weighs 0.315/0.385 against the head
                    auto const p = player_of(x.state, z.grasp_player);
                    float const m = p ? x.state.world.body_chunk_get_mass(player::body_chunk(x.state, p, z.grasp_chunk)) : 0.f;
                    float n = std::fabs(m - x.mass(0) * 0.5f);
                    n = mathf::pow(mathf::inverse_lerp(x.mass(0) * 0.5f, 0.f, n), 1.5f);
                    v *= n;
                    set_wiggle(x, custom::int_clamp(int32_t(float(z.body_wiggle_counter) * mathf::lerp(n, 1.f, 0.5f)), 0, 100));
                    if (n == 0.f && x.rng.value() < 0.025f) {
                        set_wiggle(x, 0);
                        enter_animation(x.state, x.id, animation::standard, true);
                    }
                }
                x.vel(0) -= v * 0.5f;
                x.vel(1) += v;
                x.vel(2) -= v * 0.5f;
            }
            if (body_wiggle_fac(x) > 0.f && z.swim < 0.5f) {
                z.body_wiggle += mathf::lerp(0.05f, 0.15f, body_wiggle_fac(x)) * mathf::lerp(0.5f, 0.8f, x.p.wiggle_speed);
                float const d = std::sin(z.body_wiggle * 2.f * mathf::pi);
                vec2 const perp = custom::perpendicular_vector(rw::slerp(custom::dir_vec(x.pos(1), x.pos(0)), custom::dir_vec(x.pos(2), x.pos(1)), 0.5f));
                float const d2 = (body_wiggle_fac(x) + 2.f) / (mathf::pow(1.f - x.p.wiggle_speed, 2.f) + 2.f);
                x.vel(0) += perp * d * d2;
                x.vel(1) -= perp * d * d2 * 2.f;
                x.vel(2) += perp * d * d2;
            }
            if (z.following.valid)
                z.last_following = z.following;
            z.straighten_out_needed = custom::lerp_and_tick(z.straighten_out_needed, 0.f, 0.1f, 0.05f);
        }

        // Lizard.ActAnimation: the run speed this animation allows
        float act_animation(ctx& x) {
            auto& z = x.z;
            auto& w = x.state.world;
            auto const focus = ai::focus(x.state, x.id);
            bool const focus_seen = focus && x.a.rep.visual_contact;
            vec2 const focus_pos = focus ? w.body_chunk_get_pos(player::body_chunk(x.state, focus, 0)) : vec2{0.f, 0.f};
            float num = 0.f;
            int32_t const grip = legs_gripping(x.state, x.id);
            switch (z.anim) {
            case animation::standard:
                num = x.a.run_speed;
                break;
            case animation::hear_sound:
                set_jaw_open(x.state, x.id, 0.2f);
                break;
            case animation::prey_spotted:
                set_jaw_open(x.state, x.id, 1.f);
                break;
            case animation::prey_re_spotted:
                num = x.a.run_speed * 0.3f;
                set_jaw_open(x.state, x.id, mathf::lerp(jaw_open(x.state, x.id), 1.f, x.rng.value() * x.rng.value()));
                break;
            case animation::threat_spotted:
                // (nothing it is afraid of exists here)
                num = 0.5f;
                set_jaw_open(x.state, x.id, x.rng.value());
                break;
            case animation::fighting_stance:
                set_jaw_open(x.state, x.id, 1.f);
                if (focus && !ai::unpleasant_fall_risk(x.state, x.id, x.room, room_view::get_tile_position(x.pos(0))) && focus_seen) {
                    vec2 const d = custom::dir_vec(x.pos(0), focus_pos);
                    x.vel(0) += d * 4.f * float(grip);
                    x.vel(1) -= d * 2.2f * float(grip);
                    x.vel(2) -= d * (is_wall_climber(x.state, x.id) ? 2.2f : 2.6f) * float(grip);
                }
                break;
            case animation::threat_re_spotted:
                num = x.a.run_speed * 0.8f;
                set_jaw_open(x.state, x.id, mathf::lerp(jaw_open(x.state, x.id), x.rng.value(), x.rng.value()));
                break;
            case animation::shoot_tongue:
                set_wiggle(x, 0);
                num = 0.2f;
                if (x.a.current != behavior::hunt || !focus_seen || rw::dot(custom::dir_vec(x.pos(1), x.pos(0)), custom::dir_vec(x.pos(0), focus_pos)) < 0.3f)
                    enter_animation(x.state, x.id, animation::standard, true);
                if (z.time_in_animation == z.time_to_remain_in_animation / 2 && focus) {
                    set_jaw_open(x.state, x.id, 1.f);
                    tongue_lash_out(x, w.body_chunk_get_pos(player::body_chunk(x.state, focus, uint32_t(x.rng.range(0, 2)))));
                }
                break;
            case animation::spit: {
                set_wiggle(x, 0);
                set_jaw_open(x.state, x.id, mathf::clamp(jaw_open(x.state, x.id) + 0.2f, 0.f, 1.f));
                if (!x.a.spit.spitting) {
                    enter_animation(x.state, x.id, animation::standard, true);
                    break;
                }
                vec2 aim;
                if (!ai::spit_aim_pos(x.state, x.id, aim))
                    break;
                if (ai::at_spit_pos(x.state, x.id)) {
                    vec2 const sp = room_view::middle_of_tile(x.a.spit.spit_from_pos);
                    float const link = 17.f * x.p.body_length_fac * ((x.p.body_size_fac + 1.f) / 2.f);
                    x.vel(0) += rw::clamp_magnitude(sp - custom::dir_vec(sp, aim) * link - x.pos(0), 10.f) / 5.f;
                    x.vel(1) += rw::clamp_magnitude(sp - x.pos(1), 10.f) / 5.f;
                }
                if (!ai::unpleasant_fall_risk(x.state, x.id, x.room, room_view::get_tile_position(x.pos(0)))) {
                    vec2 const d = custom::dir_vec(x.pos(0), aim);
                    x.vel(0) += d * 4.f * float(grip);
                    x.vel(1) -= d * 2.f * float(grip);
                    x.vel(2) -= d * 2.f * float(grip);
                }
                if (x.a.spit.delay < 1) {
                    vec2 const from = x.pos(0) + custom::dir_vec(x.pos(1), x.pos(0)) * 10.f;
                    vec2 const dir = custom::dir_vec(from, aim);
                    if (rw::dot(dir, custom::dir_vec(x.pos(1), x.pos(0))) > 0.3f)
                        spit_now(x, from, dir);
                }
                break;
            }
            case animation::prepare_to_jump: {
                auto const& j = w.lizard_get_jump(x.id);
                if (j.has_act_on && x.consious() && custom::dist_less(x.pos(1), room_view::middle_of_tile(j.act_on.start), 50.f)) {
                    vec2 const iv = rw::normalized(j.act_on.best.init_vel);
                    x.vel(1) *= 0.5f;
                    x.pos(1) = rw::lerp(x.pos(1), room_view::middle_of_tile(j.act_on.start) - iv * float(z.time_in_animation) * 0.5f, 0.2f);
                    x.vel(0) += iv * 1.5f;
                    x.vel(2) -= iv * 2.f;
                    if (x.viewed) {
                        x.g.head.vel += custom::dir_vec(x.pos(0), room_view::middle_of_tile(j.act_on.best.goal)) * 10.f;
                        vec2 const perp = custom::perpendicular_vector(x.pos(0), x.pos(2));
                        for (int32_t i = 0; i < x.p.tail_segments && i < max_tail; ++i)
                            x.g.tail[i].bp.vel -= iv * float(i) + perp * (z.time_in_animation % 6 < 3 ? -5.f : 5.f);
                    }
                    if (z.time_in_animation > z.time_to_remain_in_animation - 5)
                        enter_animation(x.state, x.id, animation::jumping, false);
                    z.in_allowed_terrain_counter = x.p.regain_footing_counter + 10;
                } else {
                    enter_animation(x.state, x.id, animation::standard, true);
                }
                break;
            }
            case animation::jumping:
                break;
            case animation::prepare_to_lounge:
                set_wiggle(x, 0);
                if (!x.p.can_exit_lounge_warm_up || (x.a.current == behavior::hunt && focus_seen)) {
                    if (focus) {
                        if (grip > 0)
                            for (uint32_t i = 0; i < 3; ++i)
                                x.vel(i) += custom::dir_vec(x.pos(i), focus_pos) * x.p.pre_lounge_crouch_movement;
                        if (z.time_in_animation >= x.p.pre_lounge_crouch)
                            enter_animation(x.state, x.id, animation::lounge, false);
                    } else {
                        enter_animation(x.state, x.id, animation::standard, true);
                    }
                } else {
                    enter_animation(x.state, x.id, animation::standard, true);
                }
                break;
            case animation::lounge:
                if (z.time_in_animation < x.p.lounge_maximum_frames && (!x.p.can_exit_lounge || (x.a.current == behavior::hunt && focus_seen))) {
                    set_jaw_open(x.state, x.id, jaw_open(x.state, x.id) + 0.1f);
                    if (z.time_in_animation < x.p.lounge_propulsion_frames && grip > 0)
                        for (uint32_t k = 0; k < 3; ++k)
                            x.vel(k) += z.lounge_dir * x.p.lounge_speed / float(k + 1);
                } else {
                    jaws_snap_shut(x.state, x.id, x.pos(0) + custom::dir_vec(x.pos(1), x.pos(0)) * 10.f);
                    enter_animation(x.state, x.id, animation::standard, true);
                    z.post_lounge_stun = 40;
                    z.in_allowed_terrain_counter = 0;
                }
                break;
            case animation::shake_prey:
                num = 0.75f;
                if (!z.grasp_player && x.rng.value() < 0.025f)
                    enter_animation(x.state, x.id, animation::standard, true);
                break;
            }
            if (tongue_stuck(x.state, x.id)) {
                if (x.is(breed::white)) {
                    set_wiggle(x, z.body_wiggle_counter + 2);
                    num = 0.f;
                    if (!z.apply_gravity) {
                        float const d = 1.f - float(grip) / 4.f;
                        for (uint32_t i = 0; i < 3; ++i)
                            x.vel(i) *= d;
                        if (grip > 0)
                            x.vel(2) += custom::dir_vec(w.lizard_get_tongue(x.id).pos, x.pos(2)) * 1.2f;
                    }
                } else {
                    num *= 0.6f;
                }
            }
            if (z.post_lounge_stun > 0)
                num *= 0.1f;
            if (x.is(breed::red) && x.a.run_speed > 0.1f && z.anim != animation::spit)
                num = mathf::lerp(num, 1.f, mathf::lerp(0.2f, 0.7f, x.a.hunger));
            return num;
        }

        //
        // LizardSpit (Red, Caramel)
        //

        void spit_now(ctx& x, vec2 from, vec2 dir) {
            auto& w = x.state.world;
            x.sound_at(sound::sound_id::red_lizard_spit, from);
            // LizardSpit constructor
            spit_state s;
            vec2 const v = dir * 40.f;
            s.last_pos = from;
            s.vel = v;
            s.pos = from + v;
            s.fall_off = x.rng.range(2, 100);
            s.mass_left = 1.f;
            s.dissapear_speed = x.rng.value();
            s.my_aim_up = x.a.spit.aim_up;
            s.my_aim_player = x.a.spit.aim_player;
            s.my_aim_chunk = x.a.spit.aim_chunk;
            s.slime_count = int32_t(mathf::lerp(8.f, 15.f, x.rng.value()));
            for (int32_t i = 0; i < s.slime_count; ++i) {
                auto& sl = s.slime[i];
                sl.pos = from + custom::rnv(x.rng) * 4.f * x.rng.value();
                sl.last_pos = sl.pos;
                sl.vel = v + custom::rnv(x.rng) * 4.f * x.rng.value();
                if (i == 0 || x.rng.value() < 0.3f)
                    sl.connected = -1;
                else if (x.rng.value() < 0.7f)
                    sl.connected = i - 1;
                else
                    sl.connected = x.rng.range(0, s.slime_count);
                sl.length = mathf::lerp(3.f, 8.f, x.rng.value());
                s.slime_alpha[i] = x.rng.value();
            }
            s.jagged_alpha = x.rng.value() * 0.5f;
            s.dot_alpha = x.rng.value() * 0.5f;
            s.color = x.is(breed::red) ? vec3{1.f, 0.f, 0.f} : w.lizard_get_effect_color(x.id);
            // room.AddObject: a new object, made once the rooms are done updating
            x.state.defer(x.room.id, [s, room = x.room.id, owner = x.id](sys::game_state& st) {
                auto& sw = st.world;
                auto const sp = sw.create_lizard_spit();
                sw.lizard_spit_set_room(sp, uint32_t(room.index()));
                sw.lizard_spit_set_owner(sp, sw.lizard_is_valid(owner) ? uint32_t(owner.index() + 1) : 0u);
                sw.lizard_spit_set_spit(sp, s);
            });
            x.a.spit.delay = 12;
            x.pos(2) -= dir * 8.f;
            x.pos(1) -= dir * 4.f;
            x.vel(2) -= dir * 2.f;
            x.vel(1) -= dir * 1.f;
            set_jaw_open(x.state, x.id, 1.f);
        }

        // LizardSpit.Update
        void spit_update(sys::game_state& state, dcon::lizard_spit_id sp, room_view const& room) {
            auto& w = state.world;
            auto& s = w.lizard_spit_get_spit(sp);
            dcon::lizard_id const owner{dcon::lizard_id::value_base_t(w.lizard_spit_get_owner(sp) - 1)};
            bool const has_owner = w.lizard_spit_get_owner(sp) != 0 && w.lizard_is_valid(owner);
            rw::rng& rng = state.room_random(room.id);
            s.last_pos = s.pos;
            s.pos += s.vel;
            s.vel.y -= 0.9f;
            for (int32_t i = 0; i < s.slime_count; ++i) {
                auto& sl = s.slime[i];
                sl.last_pos = sl.pos;
                sl.pos += sl.vel;
                sl.vel *= 0.99f;
                sl.vel.y -= 0.9f;
                if (sl.connected < 0 || sl.connected >= s.slime_count) {
                    vec2 const a = custom::dir_vec(sl.pos, s.pos);
                    float const d = rw::distance(sl.pos, s.pos);
                    sl.pos -= a * (sl.length * s.mass_left - d) * 0.9f;
                    sl.vel -= a * (sl.length * s.mass_left - d) * 0.9f;
                    s.pos += a * (sl.length - d) * 0.1f;
                    s.vel += a * (sl.length - d) * 0.1f;
                } else {
                    auto& o = s.slime[sl.connected];
                    vec2 const a = custom::dir_vec(sl.pos, o.pos);
                    float const d = rw::distance(sl.pos, o.pos);
                    sl.pos -= a * (sl.length * s.mass_left - d) * 0.5f;
                    sl.vel -= a * (sl.length * s.mass_left - d) * 0.5f;
                    o.pos += a * (sl.length * s.mass_left - d) * 0.5f;
                    o.vel += a * (sl.length * s.mass_left - d) * 0.5f;
                }
            }
            float const rad = 4.f * s.mass_left;
            auto const stick = player_of(state, s.stick_player);
            dcon::body_chunk_id sc;
            if (stick && player_here(state, stick, room.id))
                sc = player::body_chunk(state, stick, s.stick_chunk);
            bool hit = false;
            if (sc && custom::dist_less(w.body_chunk_get_pos(sc), s.pos, w.body_chunk_get_rad(sc) + 40.f) && s.fall_off > 0) {
                float const r = rad + w.body_chunk_get_rad(sc);
                vec2 const a = custom::dir_vec(s.pos, w.body_chunk_get_pos(sc));
                float const d = rw::distance(s.pos, w.body_chunk_get_pos(sc));
                float const k = w.body_chunk_get_mass(sc) / (0.1f * s.mass_left + w.body_chunk_get_mass(sc));
                s.pos += a * (d - r) * k;
                s.vel += a * (d - r) * k;
                w.body_chunk_get_pos(sc) -= a * (d - r) * (1.f - k);
                w.body_chunk_get_vel(sc) -= a * (d - r) * (1.f - k);
                --s.fall_off;
            } else {
                s.stick_player = 0;
                // the terrain: bouncing off what it flew into (the tile
                // collisions simplified to the tile it ended up in)
                ivec2 const t = room_view::get_tile_position(s.pos);
                if (room.get_tile(t).solid()) {
                    ivec2 const lt = room_view::get_tile_position(s.last_pos);
                    if (lt.x != t.x && !room.get_tile(ivec2{lt.x, t.y}).solid()) {
                        s.vel.x = -s.vel.x * 0.2f;
                        s.vel.y *= 0.8f;
                        s.pos.x = s.last_pos.x;
                    } else {
                        s.vel.y = -s.vel.y * 0.2f;
                        s.vel.x *= 0.8f;
                        s.pos.y = s.last_pos.y;
                    }
                    hit = true;
                }
                dcon::player_id p;
                uint32_t k = 0;
                vec2 at;
                if (trace_players(state, room.id, s.last_pos, s.pos, rad, p, k, at)) {
                    s.pos = at;
                    s.stick_player = player_ref(p);
                    s.stick_chunk = uint8_t(k);
                    s.vel *= 0.25f;
                    if (has_owner && s.my_aim_player == player_ref(p)) {
                        s.my_aim_player = 0;
                        ai::spit_set_aim(state, owner, s.my_aim_up);
                    }
                    // Creature.Violence(Blunt, 0 damage, random * 7 stun)
                    auto const c = player::body_chunk(state, p, k);
                    w.body_chunk_get_vel(c) += s.vel * 0.6f / w.body_chunk_get_mass(c);
                    player::stun(state, p, int32_t(rng.value() * 7.f));
                    state.play_sound(sound::sound_id::red_lizard_spit_hit_player, room.id, s.pos);
                    hit = true;
                }
            }
            auto const aim = player_of(state, s.my_aim_player);
            vec2 const aim_pos = aim ? w.body_chunk_get_pos(player::body_chunk(state, aim, s.my_aim_chunk)) : vec2{0.f, 0.f};
            if (hit) {
                if (aim && has_owner) {
                    if (s.pos.y < aim_pos.y)
                        ai::spit_aim_a_bit_up(state, owner, s.my_aim_up);
                    else if (s.pos.y > aim_pos.y)
                        ai::spit_aim_a_bit_down(state, owner, s.my_aim_up);
                }
                s.my_aim_player = 0;
                if (s.mass_left == 1.f) {
                    s.mass_left = 0.99f;
                    if (!s.stick_player)
                        state.play_sound(sound::sound_id::red_lizard_spit_hit_wall, room.id, s.pos);
                }
            }
            if (aim && (s.last_pos.x < aim_pos.x) != (s.pos.x < aim_pos.x)) {
                if (has_owner) {
                    if (s.pos.y < aim_pos.y)
                        ai::spit_aim_a_bit_up(state, owner, s.my_aim_up);
                    else if (s.pos.y > aim_pos.y)
                        ai::spit_aim_a_bit_down(state, owner, s.my_aim_up);
                }
                s.my_aim_player = 0;
            }
            if (s.mass_left < 1.f)
                s.mass_left -= mathf::lerp(0.5f, 1.5f, s.dissapear_speed) / (s.stick_player == 0 ? 30.f : 120.f);
            if (s.mass_left <= 0.f || s.pos.y < -300.f || !std::isfinite(s.pos.x) || !std::isfinite(s.pos.y))
                s.dead = true;
        }

        //
        // LizardJumpModule (Cyan)
        //

        // the path cost at a tile (PathingCell.costToGoal), unreachable for none
        uint16_t cell(ctx& x, pathing::field_id f, ivec2 t) {
            if (!x.room.inside(t.x, t.y))
                return pathing::unreachable;
            return pathing::distance(x.state, f, t);
        }
        // LizardJumpModule.PathWeightComparison: B better than A
        bool path_weight_comparison(uint16_t a, uint16_t b) {
            if (a == pathing::unreachable)
                return b != pathing::unreachable;
            if (b == pathing::unreachable)
                return false;
            return b < a;
        }

        // JumpFinder.NewTest
        void jump_new_test(ctx& x, jump_finder& jf) {
            float num = x.rng.value() * mathf::pow(x.a.run_speed, 0.5f);
            if (pathing::terrain_proximity(x.room, jf.start) > 1)
                num *= 0.5f;
            if (jf.chain_jump)
                num *= 0.5f;
            float speed = mathf::lerp(14.f, 50.f, num);
            if (x.z.grasp_player)
                speed *= 0.75f;
            speed *= mathf::lerp(1.f, mathf::clamp01(x.health()), 0.5f + 0.5f * x.rng.value());
            float const ang = (45.f * mathf::pow(x.rng.value(), 0.75f) + 135.f * mathf::pow(x.rng.value(), 2.f)) * (x.rng.value() < 0.5f ? -1.f : 1.f);
            jf.current = jump_instruction{};
            jf.current.start_pos = room_view::middle_of_tile(jf.start);
            jf.current.init_vel = custom::deg_to_vec(ang) * speed;
            jf.current.power = num;
            jf.current.set = true;
            jf.pos = jf.current.start_pos;
            jf.last_pos = jf.pos;
            jf.vel = jf.current.init_vel;
            jf.ventured_away_from_terrain = false;
            if (!jf.best.set)
                jf.best = jf.current;
        }

        void jump_finder_start(ctx& x, jump_finder& jf, ivec2 start, bool chain) {
            jf = jump_finder{};
            jf.active = true;
            jf.start = start;
            jf.chain_jump = chain;
            jump_new_test(x, jf);
        }

        // JumpFinder.BeneficialMovement
        bool beneficial(ctx& x, pathing::field_id f, jump_finder const& jf) {
            auto const& j = x.state.world.lizard_get_jump(x.id);
            if (!jf.best.set || !jf.best.has_goal)
                return false;
            uint16_t const goal = cell(x, f, jf.best.goal);
            return float_dist(jf.start, jf.best.goal) > 5.f && float_dist(x.a.destination, jf.best.goal) > 3.f
                && !path_weight_comparison(goal, j.lizard_cell) && !path_weight_comparison(goal, cell(x, f, jf.start));
        }

        // JumpFinder.Iterate: one tick of the test jump's flight
        void jump_iterate(ctx& x, pathing::field_id f, jump_finder& jf) {
            jf.last_pos = jf.pos;
            jf.pos += jf.vel;
            jf.vel *= 0.999f;
            jf.vel.y -= 0.9f;
            vec2 const perp = custom::perpendicular_vector(jf.last_pos, jf.pos);
            // SharedPhysics.RayTracedTilesArray
            ivec2 const a = room_view::get_tile_position(jf.last_pos), b = room_view::get_tile_position(jf.pos);
            int32_t const n = std::max(std::abs(b.x - a.x), std::abs(b.y - a.y));
            for (int32_t i = 0; i <= n; ++i) {
                vec2 const fv = rw::lerp(rw::to_vec2(a), rw::to_vec2(b), n == 0 ? 0.f : float(i) / float(n));
                ivec2 const t{int32_t(std::lround(fv.x)), int32_t(std::lround(fv.y))};
                auto const tile = x.room.get_tile(t);
                if (tile.solid() || t.y < 0 || t.y < x.room.default_water_level || x.narrow(t) || !x.room.inside(t.x, t.y)) {
                    jump_new_test(x, jf);
                    return;
                }
                int32_t const prox = pathing::terrain_proximity(x.room, t);
                bool const beam = tile.vertical_beam() || tile.horizontal_beam();
                if (!jf.ventured_away_from_terrain && prox > 1 && !beam)
                    jf.ventured_away_from_terrain = true;
                if (jf.ventured_away_from_terrain && x.accessible(t) && (prox == 1 || beam)
                && float_dist(jf.start, t) > float(custom::int_clamp(int32_t(rw::magnitude(jf.current.init_vel) / 3.f), 5, 20)) && float_dist(x.a.destination, t) > 3.f) {
                    uint16_t const c = cell(x, f, t);
                    if (path_weight_comparison(jf.best.has_goal ? cell(x, f, jf.best.goal) : pathing::unreachable, c)) {
                        jf.best = jf.current;
                        jf.best.goal = t;
                        jf.best.has_goal = true;
                        vec2 const gm = room_view::middle_of_tile(t);
                        vec2 const d = custom::dir_vec(jf.last_pos, jf.pos);
                        jf.best.grab_when_landing = !x.room.get_tile(gm + custom::perpendicular_vector(d) * 15.f + d * 20.f).solid();
                    }
                }
                float const side = custom::lerp_map(float(jf.current.tick), 5.f, 20.f, 10.f, 20.f);
                if ((!x.room.get_tile(jf.start + ivec2{0, 1}).solid() && x.room.get_tile(t + ivec2{0, 1}).solid())
                || x.room.get_tile(room_view::middle_of_tile(t) + perp * side).solid() || x.room.get_tile(room_view::middle_of_tile(t) - perp * side).solid()) {
                    jump_new_test(x, jf);
                    return;
                }
            }
            if (++jf.current.tick > 700)
                jump_new_test(x, jf);
        }

        // LizardJumpModule.InitiateJump
        void initiate_jump(ctx& x, jump_finder const& jf, bool chain) {
            auto& j = x.state.world.lizard_get_jump(x.id);
            if (!jf.best.set || !jf.best.has_goal)
                return;
            j.act_on = jf;
            j.has_act_on = true;
            enter_animation(x.state, x.id, animation::prepare_to_jump, true);
            if (chain) {
                x.z.time_to_remain_in_animation = 10;
            } else if (jf.best.power > 0.75f) {
                x.z.time_to_remain_in_animation = 45;
                x.sound(sound::sound_id::cyan_lizard_prepare_powerful_jump);
            } else if (jf.best.power > 0.25f) {
                x.z.time_to_remain_in_animation = 30;
                x.sound(sound::sound_id::cyan_lizard_prepare_medium_jump);
            } else {
                x.z.time_to_remain_in_animation = 15;
                x.sound(sound::sound_id::cyan_lizard_prepare_small_jump);
            }
            x.z.lounge_dir = custom::dir_vec(x.pos(0), room_view::middle_of_tile(jf.best.goal));
            j.jump_counter = 0;
            for (auto& f : j.finders)
                f.active = false;
            // canChainJump: a Cyan with nothing in its jaws
            if (!x.z.grasp_player && (j.act_on.chain_jump || j.act_on.best.power > 0.25f))
                jump_finder_start(x, j.finders[0], jf.best.goal, true);
        }

        // LizardJumpModule.Jump (entering the Jumping animation)
        void jump_now(ctx& x) {
            auto& j = x.state.world.lizard_get_jump(x.id);
            if (!j.has_act_on || !j.act_on.best.set)
                return;
            auto const& b = j.act_on.best;
            x.sound(b.power > 0.75f ? sound::sound_id::cyan_lizard_powerful_jump : b.power > 0.25f ? sound::sound_id::cyan_lizard_medium_jump : sound::sound_id::cyan_lizard_small_jump);
            vec2 const start = room_view::middle_of_tile(j.act_on.start);
            for (uint32_t i = 0; i < 3; ++i)
                x.pos(i) = start + rw::normalized(b.init_vel) * float(1 - int32_t(i)) * 8.f;
            vec2 const shift = start + b.init_vel - x.pos(1);
            for (uint32_t i = 0; i < 3; ++i) {
                x.pos(i) += shift;
                x.vel(i) = b.init_vel;
            }
            x.z.move_anim.active = false;
            x.z.in_allowed_terrain_counter = 0;
            j.jump_counter = 0;
            j.add_delay = 20;
            x.z.has_grip_point = false;
            if (b.tick > 25 && rw::dot(rw::normalized(x.pos(1) - x.pos(0)), rw::normalized(b.init_vel)) < mathf::lerp(-0.9f, -0.1f, mathf::pow((x.a.excitement + x.a.run_speed) / 2.f, 0.5f)))
                j.spin = int32_t(mathf::sign(b.init_vel.x));
            else
                j.spin = 0;
            // (the CyanLizardSmoke, JumpLight and ShockWave are cosmetics left out)
            for (int i = 0; i < 6; ++i)
                particles::spawn(x.state, x.room.id, particles::kind::dust, start, -b.init_vel * x.rng.value() * 0.3f + custom::rnv(x.rng) * mathf::lerp(2.f, 6.f, b.power),
                    mathf::lerp(4.f, 10.f, b.power), 0.6f, x.state.world.lizard_get_effect_color(x.id), 0.5f);
        }

        // LizardJumpModule.TryChainJump
        bool try_chain_jump(ctx& x, pathing::field_id f) {
            auto& j = x.state.world.lizard_get_jump(x.id);
            if (x.z.grasp_player)
                return false;
            for (auto& jf : j.finders) {
                if (!jf.active || !jf.chain_jump || !beneficial(x, f, jf))
                    continue;
                for (uint32_t i = 0; i < 3; ++i)
                    if (custom::dist_less(room_view::middle_of_tile(jf.start), x.pos(i), 40.f)) {
                        for (uint32_t k = 0; k < 3; ++k)
                            x.vel(k) *= 0.f;
                        x.z.in_allowed_terrain_counter = 1000;
                        x.pos(1) = room_view::middle_of_tile(jf.start);
                        jump_finder const copy = jf;
                        initiate_jump(x, copy, true);
                        return true;
                    }
            }
            return false;
        }

        // LizardJumpModule.Update
        void jump_module_update(ctx& x) {
            auto& w = x.state.world;
            auto& j = w.lizard_get_jump(x.id);
            if (w.lizard_get_dead(x.id))
                return;
            auto const f = ai::field(x.state, x.id, x.room);
            if (!x.consious() && (x.z.anim == animation::prepare_to_jump || x.z.anim == animation::jumping))
                enter_animation(x.state, x.id, animation::standard, true);
            // (the gas leaking out of a spear stuck in it isn't ported)
            int32_t active = 0;
            for (auto const& jf : j.finders)
                active += jf.active ? 1 : 0;
            bool const run_mode = !j.has_act_on && x.z.anim != animation::prepare_to_jump && x.z.anim != animation::jumping;
            for (auto& jf : j.finders) {
                if (!jf.active)
                    continue;
                // JumpFinder.Update
                if (run_mode) {
                    ++jf.fade;
                    if (!path_weight_comparison(j.lizard_cell, cell(x, f, jf.start)))
                        jf.fade += 10;
                    if (jf.fade > 40) {
                        jf.active = false;
                        continue;
                    }
                }
                for (int32_t i = std::max(1, 100 / std::max(1, active)); i >= 0; --i)
                    jump_iterate(x, f, jf);
            }
            if (j.has_act_on) {
                // the landing direction: down the path from the goal
                uint16_t best = cell(x, f, j.act_on.best.goal);
                ivec2 bt = j.act_on.best.goal;
                for (auto const d : custom::eight_directions) {
                    uint16_t const c = cell(x, f, j.act_on.best.goal + d);
                    if (path_weight_comparison(best, c)) {
                        best = c;
                        bt = j.act_on.best.goal + d;
                    }
                }
                if (bt != j.act_on.best.goal) {
                    j.act_on.has_landing_direction = true;
                    j.act_on.landing_direction = custom::dir_vec(room_view::middle_of_tile(j.act_on.best.goal), room_view::middle_of_tile(bt));
                }
            }
            if (x.z.anim == animation::jumping) {
                // JumpingUpdate
                ++j.jump_counter;
                bool done = !j.has_act_on;
                if (j.has_act_on) {
                    auto const& b = j.act_on.best;
                    vec2 const to = room_view::middle_of_tile(b.goal);
                    if (j.jump_counter > b.tick + 5) {
                        for (uint32_t i = 0; i < 3; ++i)
                            if (x.accessible(x.pos(i))) {
                                x.vel(0) *= 0.5f;
                                ++x.z.in_allowed_terrain_counter;
                            }
                    } else {
                        x.z.in_allowed_terrain_counter = 0;
                        if (j.jump_counter > b.tick - 30 && j.act_on.has_landing_direction) {
                            x.vel(0) += j.act_on.landing_direction * 3.f;
                            x.vel(2) -= j.act_on.landing_direction * 3.f;
                        } else if (j.spin == 0) {
                            float const k = custom::lerp_map(float(j.jump_counter), 10.f, 60.f, 2.f, 0.f);
                            x.vel(0) += custom::dir_vec(x.pos(1), to) * k;
                            x.vel(2) -= custom::dir_vec(x.pos(1), to) * k;
                        } else if (pathing::terrain_proximity(x.room, room_view::get_tile_position(x.pos(1))) > 2) {
                            vec2 const n = rw::normalized(custom::perpendicular_vector(x.pos(2), x.pos(1)) + custom::perpendicular_vector(x.pos(1), x.pos(0)));
                            float const k = custom::lerp_map(float(j.jump_counter), 20.f, 60.f, 3.f, 0.f);
                            x.vel(0) += n * float(j.spin) * k;
                            x.vel(2) -= n * float(j.spin) * k;
                            if (x.viewed) {
                                x.g.head.vel += n * float(j.spin) * 50.f;
                                for (int32_t i = 0; i < x.p.tail_segments && i < max_tail; ++i)
                                    x.g.tail[i].bp.vel += n * float(j.spin) * std::sin(mathf::inverse_lerp(0.f, float(x.p.tail_segments - 1), float(i)) * mathf::pi) * 15.f;
                            }
                        }
                    }
                    for (uint32_t k = 0; k < 3; ++k)
                        if (custom::dist_less(x.pos(k), to, 40.f)) {
                            if (try_chain_jump(x, f))
                                return;
                            if (b.grab_when_landing) {
                                x.z.has_grip_point = true;
                                x.z.grip_point = to;
                            }
                            done = true;
                            break;
                        }
                }
                if (done || j.jump_counter >= j.act_on.best.tick + 20) {
                    x.z.in_allowed_terrain_counter = x.p.regain_footing_counter + 5;
                    if (j.has_act_on && j.act_on.best.grab_when_landing)
                        for (uint32_t i = 0; i < 3; ++i)
                            x.vel(i) *= 0.5f;
                    j.has_act_on = false;
                    enter_animation(x.state, x.id, animation::standard, true);
                }
            } else if (run_mode) {
                // RunningUpdate
                ivec2 const at = here(x.state, x.id);
                for (auto& jf : j.finders)
                    if (jf.active && at == jf.start && beneficial(x, f, jf)) {
                        jump_finder const copy = jf;
                        initiate_jump(x, copy, false);
                        break;
                    }
                ivec2 future[10];
                int32_t future_count = 0;
                bool any = false;
                ivec2 wc = at;
                for (int k = 0; k < 10; ++k) {
                    auto const m = follow_path(x, f, wc, std::nullopt);
                    if (!m.valid)
                        break;
                    wc = m.dest;
                    future[future_count++] = wc;
                    for (auto& jf : j.finders)
                        if (jf.active && jf.start == wc) {
                            jf.fade = 0;
                            any = true;
                        }
                }
                if (cell(x, f, at) != pathing::unreachable)
                    j.lizard_cell = cell(x, f, at);
                if (j.add_delay > 0) {
                    --j.add_delay;
                } else {
                    int32_t count = 0;
                    jump_finder* slot = nullptr;
                    for (auto& jf : j.finders) {
                        count += jf.active ? 1 : 0;
                        if (!jf.active && !slot)
                            slot = &jf;
                    }
                    if (slot && count < (any ? 1 : 4) && future_count > 1) {
                        ivec2 const t = future[x.rng.range(0, future_count)];
                        if (float_dist(t, at) > 2.f && !x.narrow(t) && path_weight_comparison(j.lizard_cell, cell(x, f, t)))
                            jump_finder_start(x, *slot, t, false);
                    }
                }
            }
            if (j.has_act_on && x.z.anim != animation::prepare_to_jump && x.z.anim != animation::jumping) {
                ++j.act_on.fade;
                if (j.act_on.fade > 40)
                    j.has_act_on = false;
            }
        }

        //
        // LizardVoice
        //

        // the breed's voice triggers (GetMyVoiceTrigger, no Remix: only the
        // Pink, Green and Blue lizards have one)
        sound::sound_id voice_sound(breed b, uint8_t trigger) {
            using sound::sound_id;
            switch (b) {
            case breed::pink: {
                static constexpr sound_id pinks[5] = {sound_id::lizard_voice_pink_a, sound_id::lizard_voice_pink_b, sound_id::lizard_voice_pink_c,
                    sound_id::lizard_voice_pink_d, sound_id::lizard_voice_pink_e};
                return pinks[std::min<uint8_t>(trigger, 4)];
            }
            case breed::green:
                return sound_id::lizard_voice_green_a;
            default:
                return sound_id::lizard_voice_blue_a;
            }
        }
        bool has_voice(breed b) {
            return b == breed::pink || b == breed::green || b == breed::blue;
        }

        // LizardVoice.Update: the articulation's progress (its volume curve,
        // vanilla's random table per emotion, taken as a sine)
        void voice_update(ctx& x) {
            auto& z = x.z;
            if (z.voice_index < 0)
                return;
            z.voice_progression += 1.f / (z.voice_length * custom::lerp_map(z.voice_intensity, 0.f, 2.f, 0.5f, 1.5f));
            if (z.voice_progression >= 1.f)
                z.voice_index = -1;
        }

        //
        // Lizard.Update
        //

        struct impact_ctx {
            ctx* x;
        };

        // Lizard.TerrainImpact (+ LizardGraphics.TerrainImpact's bubbles)
        void terrain_impact(void* user, dcon::physical_object_id, uint32_t, ivec2, float speed, bool first_contact) {
            auto& x = *static_cast<impact_ctx*>(user)->x;
            if (speed > 1.5f && first_contact) {
                float const num = mathf::inverse_lerp(6.f, 14.f, speed);
                float const pitch = mathf::lerp(1.f / x.p.body_size_fac, 1.f, 0.8f);
                if (num < 1.f)
                    x.sound(sound::sound_id::lizard_light_terrain_impact, 1.f - num, pitch);
                if (num > 0.f)
                    x.sound(sound::sound_id::lizard_heavy_terrain_impact, num, pitch);
            }
        }

        void update(sys::game_state& state, dcon::lizard_id l, room_view const& room) {
            auto& w = state.world;
            ctx x{state, l, room};
            auto& z = x.z;
            if (x.p.swimmer)
                w.physical_object_set_buoyancy(x.o, 0.92f);
            z.jaw_open = jaw_open(state, l);
            z.last_jaw_open = z.jaw_open;
            voice_update(x);
            if (jaw_open(state, l) > 0.9f) {
                if (z.bite_delay > 0)
                    --z.bite_delay;
            } else if (z.bite_delay < x.p.bite_delay) {
                ++z.bite_delay;
            }
            z.jaw_forced_shut = mathf::max(0.f, z.jaw_forced_shut - 0.016666668f);
            bool const dead = w.lizard_get_dead(l) != 0;
            float const health = x.health();
            if (!dead && x.rng.value() * 0.7f > health && x.rng.value() < 0.125f)
                stun(state, l, x.rng.range(1, x.rng.range(1, 27 - custom::int_clamp(int32_t(20.f * health), 0, 10))));
            if (!dead && x.rng.value() * 0.2f > health && x.rng.value() < 0.025f) {
                int32_t const flicker = x.rng.range(1, 5);
                if (x.viewed)
                    lizard_graphics::white_flicker(state, l, flicker);
            }
            if (!dead && x.rng.value() * 0.5f > health && x.rng.value() < 0.05f)
                set_jaw_open(state, l, x.rng.value());
            // bodyChunkConnections[2].active = BodyStiff()
            bool const stiff = !x.consious() || !z.move_anim.active;
            w.physical_object_for_each_chunk_connection_as_owner(x.o, [&](dcon::chunk_connection_id k) {
                if (w.chunk_connection_get_type(k) == physics::connection_type::push)
                    w.chunk_connection_set_active(k, stiff);
            });
            if (z.turned_by_rock_counter > 0) {
                --z.turned_by_rock_counter;
                // PhysicalObject.WeightedPush(0, 2, dir, frc)
                auto const push = [&](vec2 dir, float frc) {
                    float const r = x.mass(2) / (x.mass(0) + x.mass(2));
                    x.vel(0) += dir * frc * r;
                    x.vel(2) -= dir * frc * (1.f - r);
                };
                push(vec2{float(z.turned_by_rock_direction), 0.f}, 6.f);
                if (std::fabs(x.pos(0).y - x.pos(2).y) < 5.f)
                    push(vec2{0.f, 1.f}, 2.f);
            }
            if (z.bubble > 0)
                --z.bubble;
            --z.lounge_delay;
            if (x.p.tongue)
                tongue_update(x);
            if (x.consious())
                act(x);
            else
                z.apply_gravity = true;
            // (slugcats can't grab lizards: no grabbedAttackCounter bites)
            // the chunk connections are Normal while no Standard
            // MovementAnimation runs (and one never does here)
            w.physical_object_for_each_chunk_connection_as_owner(x.o, [&](dcon::chunk_connection_id k) {
                if (w.chunk_connection_get_type(k) != physics::connection_type::push)
                    w.chunk_connection_set_type(k, physics::connection_type::normal);
            });
            if (x.p.tongue && (z.anim == animation::shoot_tongue || (!x.is(breed::blue) && tongue_out(state, l))))
                z.jaw_open += 0.1f;
            if (x.viewed && x.is(breed::caramel) && !x.narrow(here(state, l)) && z.anim == animation::prepare_to_lounge) {
                // the Caramel rears up for its lunge (all of it under
                // graphicsModule != null in vanilla, the body's pull too)
                x.g.show_dominance = 1.f;
                vec2 v{x.pos(2).x + z.lounge_dir.x * 10.f, x.pos(2).y + 15.f};
                x.vel(1) += custom::dir_vec(x.pos(1), v);
                x.vel(0) += custom::dir_vec(x.pos(0), x.pos(1) + z.lounge_dir * 10.f);
                x.g.head.pos = rw::lerp(x.g.head.pos, x.pos(0) + z.lounge_dir * 20.f, 0.25f);
                x.g.head.vel = z.lounge_dir * 20.f;
                // (limbs[0..3] only: the middle pair keeps stepping)
                for (int32_t i = 0; i < 4; ++i) {
                    float const ahead = i < 2 ? 10.f : 20.f;
                    x.g.limbs[i].h.mode = player::limb_mode::hunt_absolute_position;
                    x.g.limbs[i].h.absolute_hunt_pos = vec2{x.pos(2).x + z.lounge_dir.x * ahead, x.pos(2).y - 15.f};
                }
            }
            // the body's physics settings for this tick
            float g = 0.9f, air = 0.999f, surface = 0.5f;
            bool through_floors = true;
            if (z.anim == animation::lounge || z.post_lounge_stun > 0 || (x.is(breed::cyan) && no_run_behavior(state, l))) {
                surface = z.anim == animation::lounge ? 0.5f : 0.4f;
                through_floors = z.lounge_dir.y < 0.f;
                --z.post_lounge_stun;
            } else if (z.apply_gravity) {
                surface = 0.3f;
                through_floors = z.swim > 0.f;
            } else {
                air = 0.8f;
                g = 0.f;
            }
            w.physical_object_set_g(x.o, g);
            w.physical_object_set_air_friction(x.o, air);
            w.physical_object_set_surface_friction(x.o, surface);
            w.physical_object_set_bounce(x.o, 0.1f);
            physics::set_go_through_floors(state, x.o, through_floors);
            if (z.grasp_player) {
                carry_object(x);
                danger_grasp(x);
            }
            // (LizardBreedParams.pullDownFac is 1 for every breed)
            if (x.is(breed::cyan))
                jump_module_update(x);
            // (the Blizzard's beam, the Basilisk's poison field and the rot
            // modules aren't ported)
            // Creature.Update + PhysicalObject.Update
            if (w.lizard_get_stun(l) > 0)
                --w.lizard_get_stun(l);
            if (!w.lizard_get_dead(l) && w.lizard_get_health(l) < 0.f && x.rng.value() < -w.lizard_get_health(l) && x.rng.value() < 0.025f)
                die(state, l);
            impact_ctx ic{&x};
            physics::update_object(state, room, x.o, &terrain_impact, &ic);
        }

        // Room collision response between two chunks, weighted by mass
        bool push_apart(sys::game_state& state, dcon::body_chunk_id a, dcon::body_chunk_id b) {
            auto& w = state.world;
            vec2& pa = w.body_chunk_get_pos(a);
            vec2& pb = w.body_chunk_get_pos(b);
            float const r = w.body_chunk_get_rad(a) + w.body_chunk_get_rad(b);
            if (!custom::dist_less(pa, pb, r))
                return false;
            float const dist = rw::distance(pa, pb);
            vec2 const dir = custom::dir_vec(pa, pb);
            float const ratio = w.body_chunk_get_mass(b) / (w.body_chunk_get_mass(a) + w.body_chunk_get_mass(b));
            pa -= dir * ((r - dist) * ratio);
            w.body_chunk_get_vel(a) -= dir * ((r - dist) * ratio);
            pb += dir * ((r - dist) * (1.f - ratio));
            w.body_chunk_get_vel(b) += dir * ((r - dist) * (1.f - ratio));
            return true;
        }

        // Lizard.Collide with a slugcat: myChunk `i`, their chunk `j`
        void collide_player(sys::game_state& state, dcon::lizard_id l, room_view const& room, uint32_t i, dcon::player_id p, uint32_t j) {
            auto& w = state.world;
            ai::see_player(state, l, p);
            auto& a = w.lizard_get_ai(l);
            // SocialMemory: bumping into a slugcat it doesn't like much
            if (ai::player_relationship(state, l, p).eats && a.temp_like > -0.5f)
                a.temp_like = mathf::clamp(a.temp_like - 0.05f, -1.f, 1.f);
            if (!consious(state, l) || grasped(state, l) || !ai::player_relationship(state, l, p).go_for_kill())
                return;
            ctx x{state, l, room};
            if (x.z.anim == animation::lounge && i == 0)
                bite(x, p, j);
            else
                attempt_bite(state, l, p);
        }
    }

    //
    // What lizard_ai.cpp and lizard_graphics.cpp call
    //

    dcon::room_id room_of(sys::game_state const& state, dcon::lizard_id l) noexcept {
        return state.world.physical_object_get_room_from_object_location(body(state, l));
    }

    dcon::player_id player_of(sys::game_state const& state, uint8_t v) noexcept {
        if (v == 0)
            return dcon::player_id{};
        dcon::player_id const p{dcon::player_id::value_base_t(v - 1)};
        return state.world.player_is_valid(p) && rooms::reachable(state, p) ? p : dcon::player_id{};
    }

    breed breed_of(sys::game_state const& state, dcon::lizard_id l) noexcept {
        return state.world.lizard_get_breed(l);
    }

    breed_params const& params_of(sys::game_state const& state, dcon::lizard_id l) noexcept {
        return params(state.world.lizard_get_breed(l));
    }

    ivec2 here(sys::game_state const& state, dcon::lizard_id l) noexcept {
        return room_view::get_tile_position(state.world.body_chunk_get_pos(chunk(state, l, 0)));
    }

    bool player_here(sys::game_state const& state, dcon::player_id p, dcon::room_id room) noexcept {
        auto const& w = state.world;
        return p && !w.player_get_in_shortcut(p) && player::body(state, p) && w.physical_object_get_room_from_object_location(player::body(state, p)) == room;
    }

    void enter_animation(sys::game_state& state, dcon::lizard_id l, animation a, bool force) {
        auto& w = state.world;
        auto& z = w.lizard_get_core(l);
        if ((!force && uint8_t(a) < uint8_t(z.anim)) || z.anim == a)
            return;
        auto const& p = params_of(state, l);
        auto& rng = w.lizard_get_random(l);
        auto const room = room_of(state, l);
        vec2 const head = w.body_chunk_get_pos(chunk(state, l, 0));
        z.time_in_animation = 0;
        z.anim = a;
        breed const b = w.lizard_get_breed(l);
        switch (a) {
        case animation::standard:
            z.time_to_remain_in_animation = -1;
            break;
        case animation::hear_sound:
            z.time_to_remain_in_animation = 40;
            z.bubble = 40;
            z.bubble_intensity = 0.1f;
            if (b != breed::black)
                make_sound(state, l, emotion::curious);
            break;
        case animation::prey_spotted:
            z.bubble = 30;
            z.bubble_intensity = 1.f;
            z.time_to_remain_in_animation = 30;
            make_sound(state, l, emotion::spotted_prey_first_time);
            break;
        case animation::prey_re_spotted:
            z.bubble = 10;
            z.bubble_intensity = 0.4f;
            z.time_to_remain_in_animation = 10;
            if (b == breed::yellow) {
                // the pack leader calls the others: the most dominant yellow in the room
                bool leader = true;
                float const dom = w.lizard_get_dominance(l);
                rooms::for_each_lizard(state, room, [&](dcon::lizard_id o) {
                    if (o != l && w.lizard_get_breed(o) == breed::yellow && w.lizard_get_dominance(o) > dom)
                        leader = false;
                });
                if (leader)
                    make_sound(state, l, emotion::spotted_prey_first_time);
                else
                    make_sound(state, l, emotion::re_spotted_prey, mathf::lerp(0.1f, 0.25f, rng.value()));
            } else if (b != breed::black) {
                make_sound(state, l, emotion::re_spotted_prey);
            }
            break;
        case animation::threat_spotted:
            z.bubble_intensity = 1.f;
            z.time_to_remain_in_animation = rng.range(10, 50);
            make_sound(state, l, emotion::fear);
            break;
        case animation::threat_re_spotted:
            z.bubble_intensity = 1.f;
            z.time_to_remain_in_animation = 10;
            break;
        case animation::shoot_tongue:
            z.time_to_remain_in_animation = p.tongue_warm_up;
            state.play_sound(sound::sound_id::lizard_prepare_to_shoot_tongue, room, head);
            break;
        case animation::spit:
            z.time_to_remain_in_animation = 200;
            break;
        case animation::prepare_to_jump:
            z.bubble = 3;
            z.bubble_intensity = 0.1f;
            break;
        case animation::jumping: {
            z.time_to_remain_in_animation = -1;
            auto const view = world::get_room_view(state, room);
            ctx x{state, l, view};
            jump_now(x);
            physics::set_go_through_floors(state, body(state, l), true);
            break;
        }
        case animation::prepare_to_lounge: {
            z.bubble = 3;
            z.bubble_intensity = 0.1f;
            z.time_to_remain_in_animation = p.pre_lounge_crouch + 5;
            if (auto const f = ai::focus(state, l))
                z.lounge_dir = custom::dir_vec(head, w.body_chunk_get_pos(player::body_chunk(state, f, 0)));
            state.play_sound(sound::sound_id::lizard_prepare_lunge_attack_init, room, head);
            break;
        }
        case animation::lounge: {
            z.lounge_delay = p.lounge_delay * (rng.value() < p.risk_of_double_lounge_delay ? 2 : 1) + p.lounge_maximum_frames;
            if (auto const f = ai::focus(state, l))
                z.lounge_dir = rw::slerp(z.lounge_dir, custom::dir_vec(head, w.body_chunk_get_pos(player::body_chunk(state, f, 0))), p.find_lounge_direction);
            vec2 const fwd = custom::dir_vec(w.body_chunk_get_pos(chunk(state, l, 1)), head);
            if (rw::dot(fwd, z.lounge_dir) < 0.f) {
                if (p.can_exit_lounge || p.can_exit_lounge_warm_up) {
                    enter_animation(state, l, animation::standard, true);
                    // (Lizard_Lunge_Attack_Init has no sound in vanilla's sounds.txt)
                    return;
                }
                z.lounge_dir = fwd;
            }
            if (z.lounge_dir.y > 0.f)
                z.lounge_dir.y += p.lounge_jumpyness;
            z.lounge_dir.y *= 0.5f;
            z.time_to_remain_in_animation = p.lounge_maximum_frames + 5;
            break;
        }
        case animation::shake_prey: {
            z.bubble_intensity = 0.f;
            z.time_to_remain_in_animation = rng.range(p.shake_prey, p.shake_prey * 2);
            auto const prey = player_of(state, z.grasp_player);
            float const intensity = prey ? ai::player_relationship(state, l, prey).intensity : 0.f;
            z.time_to_remain_in_animation = int32_t(float(z.time_to_remain_in_animation) * intensity);
            break;
        }
        case animation::fighting_stance:
            z.bubble = 10;
            z.bubble_intensity = 1.f;
            z.time_to_remain_in_animation = rng.range(20, 50);
            break;
        }
    }

    float jaw_open(sys::game_state const& state, dcon::lizard_id l) noexcept {
        // Lizard.JawOpen's getter (vanilla writes these clamps back: Update
        // does so once a tick here)
        auto const& z = state.world.lizard_get_core(l);
        float v = z.grasp_player ? 0.f : z.jaw_open;
        if (params_of(state, l).tongue && tongue_out(state, l))
            v = mathf::max(v, 0.5f);
        if (z.jaw_forced_shut > 0.f)
            v = mathf::min(mathf::pow(1.f - z.jaw_forced_shut, 4.f), v);
        return v;
    }

    void set_jaw_open(sys::game_state& state, dcon::lizard_id l, float v) {
        auto& z = state.world.lizard_get_core(l);
        z.jaw_open = z.grasp_player ? 0.f : mathf::clamp(v, 0.f, 1.f);
    }

    bool jaw_ready_for_bite(sys::game_state const& state, dcon::lizard_id l) noexcept {
        return jaw_open(state, l) > 0.9f && state.world.lizard_get_core(l).bite_delay == 0;
    }

    void attempt_bite(sys::game_state& state, dcon::lizard_id l, dcon::player_id p) {
        auto& w = state.world;
        if (grasped(state, l) || !consious(state, l))
            return;
        auto const room = world::get_room_view(state, room_of(state, l));
        ctx x{state, l, room};
        if (!jaw_ready_for_bite(state, l)) {
            set_jaw_open(state, l, jaw_open(state, l) + 0.05f);
            return;
        }
        bool bit = false;
        if (x.rng.value() < x.p.bite_chance && p) {
            vec2 const front = x.pos(0) + custom::dir_vec(x.pos(1), x.pos(0)) * x.p.bite_in_front;
            for (uint32_t i = 0; i < 2; ++i) {
                auto const c = player::body_chunk(state, p, i);
                // (no Remix: the chunk's own radius)
                if (custom::dist_less(front, w.body_chunk_get_pos(c), w.body_chunk_get_rad(c) + x.p.bite_rad_bonus)) {
                    if (tongue_out(state, l)) {
                        if (w.lizard_get_tongue(l).state != tongue_state::stuck_in_terrain)
                            tongue_retract(state, l);
                        return;
                    }
                    bit = true;
                    bite(x, p, i);
                    break;
                }
            }
        } else if (!p) {
            bite(x, dcon::player_id{}, 0);
        }
        if (legs_gripping(state, l) > 0) {
            if (bit) {
                for (uint32_t i = 0; i < 3; ++i)
                    x.vel(i) += custom::rnv(x.rng) * 7.f;
                return;
            }
            if (p && !tongue_out(state, l)) {
                vec2 const d = custom::dir_vec(x.pos(0), w.body_chunk_get_pos(player::body_chunk(state, p, 0)));
                x.vel(0) += d * 3.f * x.p.bite_homing_speed;
                x.vel(1) -= d * x.p.bite_homing_speed;
                x.vel(2) -= d * x.p.bite_homing_speed;
            }
        }
    }

    void jaws_snap_shut(sys::game_state& state, dcon::lizard_id l, vec2 at) {
        auto const room = world::get_room_view(state, room_of(state, l));
        ctx x{state, l, room};
        x.sound(sound::sound_id::lizard_jaws_shut_miss_creature);
        x.z.jaw_open = 0.f;
        x.z.last_jaw_open = 0.f;
        vec2 const d = custom::dir_vec(x.pos(0), at);
        x.vel(0) += d * 8.f;
        x.vel(1) -= d * 6.f;
        x.vel(2) -= d * 6.f;
        x.z.bite_delay = x.p.bite_delay;
        x.z.jaw_forced_shut = 1.f;
    }

    int32_t legs_gripping(sys::game_state const& state, dcon::lizard_id l) noexcept {
        // Lizard.LegsGripping: the limbs' grip of LizardGraphics, 2 without
        // a graphicsModule (off screen)
        if (!state.viewed(room_of(state, l)))
            return 2;
        return state.world.lizard_get_graphics(l).legs_grabbing;
    }

    int32_t no_grip_counter(sys::game_state const& state, dcon::lizard_id l) noexcept {
        // Lizard.NoGripCounter: 0 without a graphicsModule
        if (!state.viewed(room_of(state, l)))
            return 0;
        return state.world.lizard_get_graphics(l).no_grip_counter;
    }

    void make_sound(sys::game_state& state, dcon::lizard_id l, emotion e) {
        make_sound(state, l, e, mathf::lerp(0.8f, 1.2f, state.world.lizard_get_random(l).value()));
    }

    void make_sound(sys::game_state& state, dcon::lizard_id l, emotion e, float intensity) {
        auto& w = state.world;
        auto& z = w.lizard_get_core(l);
        if (!consious(state, l))
            return;
        if (intensity < (z.voice_index >= 0 ? z.voice_intensity : 0.f) / 2.f || z.grasp_player)
            return;
        breed const b = w.lizard_get_breed(l);
        if (!has_voice(b))
            return;
        // the articulation's length (VoiceArticulation, times the lizard's 0.5..1.5)
        float len = 40.f;
        switch (e) {
        case emotion::spotted_prey_first_time:
        case emotion::pain_idle:
            len = 50.f;
            break;
        case emotion::general_small_noise:
        case emotion::re_spotted_prey:
            len = 12.f;
            break;
        case emotion::submission:
        case emotion::pain_impact:
            len = 20.f;
            break;
        case emotion::boredom:
        case emotion::out_of_shortcut:
            len = 30.f;
            break;
        case emotion::blood_lust:
            len = 35.f;
            break;
        default:
            break;
        }
        z.voice_index = int32_t(e);
        z.voice_progression = 0.f;
        z.voice_intensity = intensity;
        z.voice_length = len * mathf::lerp(0.5f, 1.5f, w.lizard_get_energy(l));
        // (the trigger is a loop shaped by the articulation in vanilla; here
        // the sample plays once at the lizard's pitch)
        state.play_sound(voice_sound(b, z.voice_trigger), room_of(state, l), w.body_chunk_get_pos(chunk(state, l, 0)),
            mathf::min(1.f, custom::lerp_map(intensity, 0.f, 2.f, 0.4f, 3.f) * 0.5f), z.voice_pitch);
    }

    bool is_wall_climber(sys::game_state const& state, dcon::lizard_id l) noexcept {
        return params_of(state, l).wall_climber || world::get_room_view(state, room_of(state, l)).gravity <= 0.3f;
    }

    bool tongue_out(sys::game_state const& state, dcon::lizard_id l) noexcept {
        return params_of(state, l).tongue && state.world.lizard_get_tongue(l).state != tongue_state::hidden;
    }

    bool tongue_ready(sys::game_state const& state, dcon::lizard_id l) noexcept {
        return state.world.lizard_get_tongue(l).delay < 1;
    }

    bool tongue_stuck(sys::game_state const& state, dcon::lizard_id l) noexcept {
        auto const s = state.world.lizard_get_tongue(l).state;
        return params_of(state, l).tongue && (s == tongue_state::attatched || s == tongue_state::stuck_in_terrain);
    }

    //
    // Public
    //

    dcon::physical_object_id body(sys::game_state const& state, dcon::lizard_id l) noexcept {
        return state.world.lizard_get_body_from_lizard_body(l);
    }

    dcon::body_chunk_id chunk(sys::game_state const& state, dcon::lizard_id l, uint32_t i) noexcept {
        return physics::chunk(state, body(state, l), i);
    }

    bool consious(sys::game_state const& state, dcon::lizard_id l) noexcept {
        return state.world.lizard_get_stun(l) <= 0 && !state.world.lizard_get_dead(l);
    }

    dcon::player_id grasped(sys::game_state const& state, dcon::lizard_id l) noexcept {
        return player_of(state, state.world.lizard_get_core(l).grasp_player);
    }

    dcon::lizard_id create(sys::game_state& state, dcon::room_id room, vec2 at, breed b) {
        auto& w = state.world;
        auto const l = w.create_lizard();
        rw::rng rng{state.random.next() | 1u};
        auto const view = world::get_room_view(state, room);
        auto const& p = params(b);
        // never inside terrain
        if (view.get_tile(at).solid()) {
            ivec2 const t0 = room_view::get_tile_position(at);
            for (int r = 1; r < 8 && view.get_tile(at).solid(); ++r)
                for (int dy = -r; dy <= r; ++dy)
                    for (int dx = -r; dx <= r; ++dx)
                        if (view.get_tile(at).solid() && !view.get_tile(t0 + ivec2{dx, dy}).solid())
                            at = room_view::middle_of_tile(t0 + ivec2{dx, dy});
        }
        physics::object_desc desc;
        desc.air_friction = 0.999f;
        desc.gravity = 0.9f;
        desc.bounce = 0.1f;
        desc.surface_friction = 0.5f;
        desc.water_friction = 0.92f;
        desc.buoyancy = p.swimmer ? 0.92f : 1.06f;
        auto const o = physics::create_object(state, room, desc);
        float const rad = 8.f * p.body_size_fac * p.body_rad_fac;
        float const link = 17.f * p.body_length_fac * ((p.body_size_fac + 1.f) / 2.f);
        auto const c0 = physics::add_chunk(state, o, at, rad, p.body_mass / 3.f);
        auto const c1 = physics::add_chunk(state, o, at + vec2{-1.f, 0.f}, rad, p.body_mass / 3.f);
        auto const c2 = physics::add_chunk(state, o, at + vec2{-2.f, 0.f}, rad, p.body_mass / 3.f);
        physics::add_connection(state, o, c0, c1, link, physics::connection_type::normal, 0.95f, 0.5f);
        physics::add_connection(state, o, c1, c2, link, physics::connection_type::normal, 0.95f, 0.5f);
        physics::add_connection(state, o, c0, c2, link * (1.f + p.body_stiffnes), physics::connection_type::push, 1.f - mathf::lerp(0.9f, 0.5f, p.body_stiffnes), 0.5f);
        physics::set_go_through_floors(state, o, true);
        w.try_create_lizard_body(l, o);
        w.lizard_set_breed(l, b);
        w.lizard_set_stun(l, 0);
        w.lizard_set_dead(l, 0);
        w.lizard_set_health(l, 1.f);
        w.lizard_set_throat_health(l, 1.f);
        w.lizard_set_limb_health(l, limb_health{});
        // AbstractCreature.personality
        w.lizard_set_dominance(l, rng.value());
        w.lizard_set_energy(l, rng.value());
        body_state z;
        z.bite_delay = p.bite_delay;
        // LizardVoice: the trigger and the pitch
        z.voice_trigger = uint8_t(b == breed::pink ? rng.range(0, 5) : 0);
        z.voice_pitch = custom::clamped_random_variation(0.5f, 0.2f, 0.8f, rng) * 2.f;
        w.lizard_set_core(l, z);
        w.lizard_set_tongue(l, tongue{});
        w.lizard_set_jump(l, jump_module{});
        // Lizard constructor: effectColor (vanilla seeds these from the creature's ID)
        vec3 color = p.standard_color;
        switch (b) {
        case breed::pink:
            color = custom::hsl2rgb(custom::wrapped_random_variation(0.87f, 0.1f, 0.6f, rng), 1.f, custom::clamped_random_variation(0.5f, 0.15f, 0.1f, rng));
            break;
        case breed::green:
            color = custom::hsl2rgb(custom::wrapped_random_variation(0.32f, 0.1f, 0.6f, rng), 1.f, custom::clamped_random_variation(0.5f, 0.15f, 0.1f, rng));
            break;
        case breed::blue:
            color = custom::hsl2rgb(custom::wrapped_random_variation(0.57f, 0.08f, 0.6f, rng), 1.f, custom::clamped_random_variation(0.5f, 0.15f, 0.1f, rng));
            break;
        case breed::yellow:
            color = custom::hsl2rgb(custom::wrapped_random_variation(0.1f, 0.05f, 0.6f, rng), 1.f, custom::clamped_random_variation(0.5f, 0.15f, 0.1f, rng));
            break;
        case breed::salamander:
            color = custom::hsl2rgb(custom::wrapped_random_variation(0.9f, 0.15f, 0.6f, rng), 1.f, custom::clamped_random_variation(0.4f, 0.15f, 0.2f, rng));
            break;
        case breed::red:
            color = custom::hsl2rgb(custom::wrapped_random_variation(0.0025f, 0.02f, 0.6f, rng), 1.f, custom::clamped_random_variation(0.5f, 0.15f, 0.1f, rng));
            break;
        case breed::cyan:
            color = custom::hsl2rgb(custom::wrapped_random_variation(0.49f, 0.04f, 0.6f, rng), 1.f, custom::clamped_random_variation(0.5f, 0.15f, 0.1f, rng));
            break;
        case breed::caramel:
            color = custom::hsl2rgb(custom::wrapped_random_variation(0.1f, 0.03f, 0.2f, rng), 0.55f, custom::clamped_random_variation(0.55f, 0.36f, 0.2f, rng));
            break;
        case breed::blizzard:
            color = custom::hsl2rgb(custom::wrapped_random_variation(0.6f, 0.05f, 0.44f, rng), custom::clamped_random_variation(0.3f, 0.05f, 0.85f, rng),
                custom::clamped_random_variation(0.58f, 0.05f, 0.85f, rng));
            break;
        case breed::basilisk:
            color = custom::hsl2rgb(custom::wrapped_random_variation(0.11f, 0.06f, 0.44f, rng), 1.f, custom::clamped_random_variation(0.5f, 0.15f, 0.1f, rng));
            color = color + (vec3{0.f, 0.f, 0.f} - color) * 0.85f;
            break;
        case breed::indigo:
            color = custom::hsl2rgb(custom::wrapped_random_variation(0.7f, 0.08f, 0.6f, rng), 1.f, custom::clamped_random_variation(0.52f, 0.15f, 0.1f, rng));
            break;
        case breed::peach:
            color = custom::hsl2rgb(custom::wrapped_random_variation(1.f, 0.07f, 0.5f, rng), 1.f, custom::clamped_random_variation(0.7f, 0.07f, 0.1f, rng));
            break;
        default:
            break;
        }
        w.lizard_set_effect_color(l, color);
        w.lizard_set_random(l, rng);
        ai::reset(state, l);
        lizard_graphics::reset(state, l);
        return l;
    }

    void destroy(sys::game_state& state, dcon::lizard_id l) {
        auto& w = state.world;
        lose_all_grasps(state, l);
        std::vector<dcon::lizard_spit_id> spits;
        w.for_each_lizard_spit([&](dcon::lizard_spit_id s) {
            if (w.lizard_spit_get_owner(s) == uint32_t(l.index() + 1))
                spits.push_back(s);
        });
        for (auto const s : spits)
            w.delete_lizard_spit(s);
        auto const o = body(state, l);
        w.delete_lizard(l);
        if (o)
            physics::delete_object(state, o);
    }

    void update_room(sys::game_state& state, room_view const& room) {
        auto& w = state.world;
        bool const viewed = state.viewed(room.id);
        rooms::for_each_lizard(state, room.id, [&](dcon::lizard_id l) {
            update(state, l, room);
            // the GraphicsModule only exists where a camera looks (the legs'
            // grip the body runs on falls back to Lizard.LegsGripping's 2)
            if (viewed)
                lizard_graphics::update(state, l, room);
        });
        rooms::for_each_lizard_spit(state, room.id, [&](dcon::lizard_spit_id s) {
            if (w.lizard_spit_get_spit(s).dead)
                return;
            spit_update(state, s, room);
            // Destroy: gone at the end of the tick
            if (w.lizard_spit_get_spit(s).dead)
                state.defer(room.id, [s](sys::game_state& st) {
                    if (st.world.lizard_spit_is_valid(s))
                        st.world.delete_lizard_spit(s);
                });
        });
    }

    void collide(sys::game_state& state, room_view const& room) {
        rooms::for_each_lizard(state, room.id, [&](dcon::lizard_id l) {
            rooms::for_each_player(state, room.id, [&](dcon::player_id p) {
                // what it holds is kept out of it by CarryObject
                if (!player_here(state, p, room.id) || grasped(state, l) == p)
                    return;
                for (uint32_t i = 0; i < 3; ++i)
                    for (uint32_t j = 0; j < 2; ++j)
                        if (push_apart(state, chunk(state, l, i), player::body_chunk(state, p, j)))
                            collide_player(state, l, room, i, p, j);
            });
            // lizards push each other about (their fights and communication
            // aren't ported, the Yellow pack's aside)
            rooms::for_each_lizard(state, room.id, [&](dcon::lizard_id other) {
                if (other.index() <= l.index())
                    return;
                for (uint32_t i = 0; i < 3; ++i)
                    for (uint32_t j = 0; j < 3; ++j)
                        push_apart(state, chunk(state, l, i), chunk(state, other, j));
            });
        });
    }

    bool trace(sys::game_state const& state, dcon::room_id room, vec2 from, vec2 to, float rad, dcon::lizard_id& hit, uint32_t& hit_chunk) {
        auto const& w = state.world;
        float best = 2.f;
        rooms::for_each_lizard(state, room, [&](dcon::lizard_id l) {
            for (uint32_t i = 0; i < 3; ++i) {
                auto const c = chunk(state, l, i);
                vec2 const p = w.body_chunk_get_pos(c);
                float const r = rad + w.body_chunk_get_rad(c);
                vec2 const dv = to - from;
                float const len2 = rw::sqr_magnitude(dv);
                float const t = len2 > 0.f ? mathf::clamp01(rw::dot(p - from, dv) / len2) : 0.f;
                if (custom::dist_less(from + dv * t, p, r) && t < best) {
                    best = t;
                    hit = l;
                    hit_chunk = i;
                }
            }
        });
        return best <= 1.f;
    }

    void violence(sys::game_state& state, dcon::lizard_id l, uint32_t k, vec2 momentum, float damage, float stun_bonus, bool rock, bool explosion) {
        auto& w = state.world;
        auto const& p = params_of(state, l);
        auto& rng = w.lizard_get_random(l);
        auto const room = room_of(state, l);
        auto const hit = chunk(state, l, k);
        float num = damage / p.base_damage_resistance;
        float st = (damage * 30.f + stun_bonus) / p.base_stun_resistance;
        if (explosion && p.explosion_resistance > 0.f)
            num /= p.explosion_resistance;
        make_sound(state, l, emotion::pain_impact, mathf::max(num * 2.f, st / 40.f));
        vec2 const head = w.body_chunk_get_pos(chunk(state, l, 0));
        // bodyChunks[0].Rotation: away from its rotationChunk, the hips
        vec2 const rot = rw::normalized(head - w.body_chunk_get_pos(chunk(state, l, 2)));
        float const jaw = jaw_open(state, l);
        // Lizard.HitInMouth / HitHeadShield
        auto const in_mouth = [&](vec2 dir) {
            if (dir.y > 0.f)
                return false;
            dir = rw::slerp(dir, vec2{0.f, 1.f}, 0.1f);
            return std::fabs(rw::angle(dir, -rot)) < mathf::lerp(-15.f, 11.f, jaw);
        };
        if (k == 0 && momentum != vec2{0.f, 0.f}) {
            if (!in_mouth(momentum) && rw::angle(momentum, -rot) < p.head_shield_angle + 20.f * jaw) {
                state.play_sound(sound::sound_id::lizard_head_shield_deflect, room, head);
                int32_t const sparks = int32_t(std::min(std::max(st / 2.f, float(int32_t(num * 30.f))), 25.f));
                for (int32_t i = 0; i < sparks; ++i)
                    particles::spawn(state, room, particles::kind::spark, head + custom::rnv(rng) * 5.f * rng.value(),
                        custom::rnv(rng) * mathf::lerp(0.2f, 0.4f, rng.value()) * rw::magnitude(momentum), 2.f, 0.3f, vec3{1.f, 1.f, 1.f});
                if (state.viewed(room)) {
                    lizard_graphics::white_flicker(state, l, sparks);
                    w.lizard_get_graphics(l).head.vel += momentum * 2.f;
                }
                num *= 0.1f;
                st = (damage * 0.5f * 30.f + stun_bonus * 0.6666667f) / p.base_stun_resistance;
                if (w.lizard_get_breed(l) == breed::cyan)
                    st *= 1.2f;
                momentum /= 3.f;
            } else if (in_mouth(momentum)) {
                num *= 1.5f;
                st = mathf::min(st * 2.f, 120.f);
                w.lizard_get_throat_health(l) -= num;
            }
        }
        w.body_chunk_get_vel(hit) += momentum / w.body_chunk_get_mass(hit);
        auto& z = w.lizard_get_core(l);
        if (rock && w.lizard_get_breed(l) != breed::red && momentum.x != 0.f) {
            z.turned_by_rock_direction = int32_t(mathf::sign(momentum.x));
            z.turned_by_rock_counter = 20;
        }
        st *= 1.f + mathf::inverse_lerp(0.75f, 0.f, w.lizard_get_health(l)) * rng.value();
        stun(state, l, int32_t(st));
        w.lizard_get_health(l) -= num;
        // SocialEventRecognizer: the slugcat's attack (lethal or not) on it
        // lowers its like of the slugcat (LizardPlayerRelationChange)
        auto& a = w.lizard_get_ai(l);
        float const change = damage >= 0.5f ? 1.f : 0.15f;
        a.temp_like = mathf::clamp(a.temp_like - change * 1.5f, -1.f, 1.f);
    }

    void release_player(sys::game_state& state, dcon::player_id p) {
        auto& w = state.world;
        // whatever holds the slugcat or has its tongue on it is in its room
        rooms::for_each_lizard(state, rooms::room_of(state, p), [&](dcon::lizard_id l) {
            if (grasped(state, l) == p)
                lose_all_grasps(state, l);
            auto& t = w.lizard_get_tongue(l);
            if (t.attached_player == player_ref(p)) {
                t.attached_player = 0;
                t.state = tongue_state::retracting;
            }
        });
    }
}
