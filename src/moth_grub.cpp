// Watcher/MothGrub.cs and MothGrubAI.cs: the moth grub. See moth_grub.hpp.

#include <algorithm>
#include <cmath>
#include <string>

#include "moth_grub.hpp"
#include "dens.hpp"
#include "moth_grub_graphics.hpp"
#include "moth_grub_types.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "rooms.hpp"
#include "tracker.hpp"

namespace moth_grub {
    using creature::relationship;
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // CreatureTemplate (StaticWorld.InitMothGrub): baseDamageResistance,
        // baseStunResistance, instantDeathDamageLimit, visualRadius
        constexpr float base_damage_resistance = 2.3f;
        constexpr float base_stun_resistance = 1.f;
        constexpr float instant_death_damage_limit = 2.3f;
        constexpr float visual_radius = 450.f;
        // bodyChunkConnections[0].distance (head to neck)
        constexpr float head_distance = 20.f;
        constexpr uint32_t chunk_count = 5;

        creature::moth_grub_payload& pl(sys::game_state& state, dcon::creature_id c) { return state.world.creature_get_payload(c).moth_grub; }
        creature::moth_grub_payload const& pl(sys::game_state const& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).moth_grub;
        }
        dcon::moth_grub_data_id data(sys::game_state const& state, dcon::creature_id c) { return creature::moth_grub_data(state.world, c); }
        vec2& pos(sys::game_state& state, dcon::creature_id c, uint32_t i) { return state.world.body_chunk_get_pos(chunk(state, c, i)); }
        vec2& vel(sys::game_state& state, dcon::creature_id c, uint32_t i) { return state.world.body_chunk_get_vel(chunk(state, c, i)); }
        vec2 pos(sys::game_state const& state, dcon::creature_id c, uint32_t i) { return state.world.body_chunk_get_pos(chunk(state, c, i)); }
        ivec2 tile_of(vec2 p) { return room_view::get_tile_position(p); }

        // Vector2.Perpendicular (not normalized)
        vec2 perpendicular(vec2 v) { return vec2{-v.y, v.x}; }

        //
        // MothGrubAI
        //
        // Tracker(10, 10, 450, 0.5, 5, 5, 10), ThreatTracker(3)
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
        struct hook_ctx {
            sys::game_state& state;
            dcon::creature_id c;
            room_view const& room;
        };
        // ArtificialIntelligence.VisualScore/VisualContact from the head
        bool visual_contact(hook_ctx const& x, vec2 look) {
            vec2 const eye = pos(x.state, x.c, 0);
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
        bool hook_sees_point(void* u, vec2 p) { return visual_contact(*static_cast<hook_ctx*>(u), p); }
        // MothGrubAI.UpdateDynamicRelationship: the static one, nothing to
        // fear in a dead creature
        relationship hook_relate(void* u, creature::tracked const& r) {
            auto& x = *static_cast<hook_ctx*>(u);
            relationship rel = creature::static_relationship(x.state, x.c, r.who);
            if (rel.type == creature::rel::afraid && creature::live(x.state, r.who) && creature::dead(x.state, r.who))
                rel.intensity = 0.f;
            return rel;
        }
        // ModuleToTrackRelationship: the ThreatTracker, whatever it is
        creature::track_module hook_module(relationship) { return creature::track_module::threat; }

        // CreatureLooker.ReevaluateLookObject (with MothGrubAI's
        // CreatureInterestBonus: never a small creature)
        void reevaluate_look(sys::game_state& state, dcon::creature_id c, room_view const& room, ai_state& a) {
            auto const& w = state.world;
            auto& rng = state.world.creature_get_random(c);
            auto const& ts = tracker::of(state, c);
            a.look_creature = dcon::creature_id{};
            float best = 0.f;
            bool const flag = rng.value() < 0.5f;
            ivec2 const at = tile_of(pos(state, c, 0));
            auto const me = creature::template_of(state, c);
            for (uint32_t i = 0; i < ts.count; ++i) {
                auto const& r = ts.reps[i];
                if (!creature::live(state, r.who))
                    continue;
                auto const tt = creature::template_of(state, r.who);
                relationship const rel = creature::static_relationship(me, tt);
                float num2 = 1.f + mathf::lerp(creature::body_size(tt), 1.f, 0.5f) * rel.intensity * tracker::chance_of_finding(r, tracker_params().see_around_corners);
                if (flag)
                    num2 += rng.value();
                if (r.visual && creature::here(state, r.who, room.id))
                    num2 *= 1.f + rw::magnitude(w.body_chunk_get_vel(creature::chunk_of(state, r.who, creature::main_chunk_of(state, r.who))));
                num2 /= mathf::lerp(rw::magnitude(rw::to_vec2(tracker::best_guess(r) - at)), 1.f, 0.6f);
                if (rel.type == creature::rel::ignores)
                    num2 /= 2.f;
                if (creature::small_creature(tt))
                    num2 = -1.f;
                if (num2 > best) {
                    best = num2;
                    a.look_creature = r.who;
                }
            }
        }
        // CreatureLooker.Update (chanceToLookAtNothing 0.0025, 30 frames
        // between focus changes)
        void looker_update(sys::game_state& state, dcon::creature_id c, room_view const& room, ai_state& a) {
            auto& rng = state.world.creature_get_random(c);
            if (a.look_creature && !tracker::find(tracker::of(state, c), a.look_creature))
                a.look_creature = dcon::creature_id{};
            if (a.look_focus_delay > 0)
                --a.look_focus_delay;
            if (a.look_focus_delay < 1 && rng.value() < 1.f / 30.f) {
                a.look_focus_delay = 30;
                if (rng.value() > 0.0025f)
                    reevaluate_look(state, c, room, a);
                else
                    a.look_creature = dcon::creature_id{};
            }
        }

        // MothGrubAI.Update
        void ai_update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& a = w.moth_grub_data_get_ai(data(state, c));
            hook_ctx hx{state, c, room};
            tracker::hooks h;
            h.user = &hx;
            h.sees = hook_sees;
            h.sees_point = hook_sees_point;
            h.relate = hook_relate;
            h.module = hook_module;
            tracker::update(state, c, room, tile_of(pos(state, c, 0)), tracker_params(), h);
            looker_update(state, c, room, a);
            // UtilityComparer: the threat (smoothed in vanilla) and the rain,
            // each to the power 1.1
            float const threat = mathf::pow(mathf::max(tracker::threat_utility(tracker::of(state, c)), 0.f), 1.1f);
            float const rain = mathf::pow(mathf::max(dens::rain_utility(state, c, room.id), 0.f), 1.1f);
            behavior b = behavior(w.creature_get_behavior(c));
            if (threat > 0.f || rain > 0.f)
                b = threat >= rain ? behavior::flee : behavior::escape_rain;
            float const utility = mathf::max(threat, rain);
            if (utility < 0.4f)
                b = behavior::idle;
            w.creature_set_behavior(c, uint8_t(b));
            w.creature_set_current_utility(c, utility);
            // (it has no way to move: its destination only matters for the
            // pipe it lies at)
            if (b == behavior::escape_rain)
                dens::home_tile(state, c, room);
        }

        // MothGrub.JointBody
        void joint_body(sys::game_state& state, dcon::creature_id c) {
            vec2 const a = (pos(state, c, 1) + pos(state, c, 2)) * 0.5f;
            vec2 const a2 = (pos(state, c, 3) + pos(state, c, 4)) * 0.5f;
            vec2 const a3 = custom::dir_vec(pos(state, c, 2), pos(state, c, 1));
            vec2 const b = custom::perpendicular_vector(custom::dir_vec(pos(state, c, 3), pos(state, c, 4)));
            vec2 const n = rw::normalized(a3 + b);
            vec2 const a4 = custom::perpendicular_vector(n);
            vec2 b2 = ((a + n * 7.f - pos(state, c, 1)) * 0.5f - (a - n * 7.f - pos(state, c, 2)) * 0.5f) * 0.25f;
            vel(state, c, 1) += b2;
            pos(state, c, 1) += b2;
            vel(state, c, 2) -= b2;
            pos(state, c, 2) -= b2;
            b2 = ((a2 + a4 * 7.f - pos(state, c, 3)) * 0.5f - (a2 - a4 * 7.f - pos(state, c, 4)) * 0.5f) * 0.25f;
            vel(state, c, 3) += b2;
            pos(state, c, 3) += b2;
            vel(state, c, 4) -= b2;
            pos(state, c, 4) -= b2;
        }

        // MothGrub.Act
        void act(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            ai_update(state, c, room);
            auto const& a = w.moth_grub_data_get_ai(data(state, c));
            auto& p = pl(state, c);
            if (a.look_creature) {
                if (auto const* r = tracker::find(tracker::of(state, c), a.look_creature)) {
                    if (r->visual && creature::here(state, r->who, room.id))
                        p.look_at_point = creature::main_pos(state, r->who);
                    else
                        p.look_at_point = room_view::middle_of_tile(tracker::best_guess(*r));
                }
            }
            // (no path to follow: GoThroughFloors off, as at its destination)
            physics::set_go_through_floors(state, body(state, c), false);
        }
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::moth_grub);
        auto const d = data(state, c);
        rw::rng rng{state.random.next() | 1u};
        int32_t seed = int32_t(rng.next() & 0x7FFFFFFFu);
        if (state.spawn_seed >= 0)
            seed = state.spawn_seed;
        physics::object_desc desc;
        desc.gravity = 0.9f;
        desc.air_friction = 0.999f;
        desc.bounce = 0.47f;
        desc.surface_friction = 0.25f;
        desc.water_friction = 0.87f;
        desc.buoyancy = 1.3f;
        auto const o = physics::create_object(state, room, desc);
        // never inside terrain
        auto const view = world::get_room_view(state, room);
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
        // five chunks (8, 1.1 / 5); a head, then the body's four, a little
        // apart so the connections have a direction to work in
        constexpr vec2 offsets[chunk_count] = {{0.f, 12.f}, {0.f, 0.f}, {0.f, -8.f}, {-4.f, -4.f}, {4.f, -4.f}};
        dcon::body_chunk_id k[chunk_count];
        for (uint32_t i = 0; i < chunk_count; ++i) {
            k[i] = physics::add_chunk(state, o, at + offsets[i] * 0.5f, 8.f, 1.1f / 5.f);
            w.body_chunk_set_restrict_in_room_range(k[i], 2400.f);
        }
        using physics::connection_type;
        physics::add_connection(state, o, k[0], k[1], 20.f, connection_type::pull, 0.4f, 0.5f);
        physics::add_connection(state, o, k[1], k[2], 14.f, connection_type::push, 0.4f, 0.5f);
        physics::add_connection(state, o, k[3], k[4], 14.f, connection_type::push, 0.4f, 0.5f);
        physics::add_connection(state, o, k[0], k[2], 34.f, connection_type::push, 0.1f, 0.5f);
        physics::add_connection(state, o, k[1], k[4], 10.f, connection_type::normal, 0.7f, 0.5f);
        physics::add_connection(state, o, k[4], k[2], 10.f, connection_type::normal, 0.7f, 0.5f);
        physics::add_connection(state, o, k[2], k[3], 10.f, connection_type::normal, 0.7f, 0.5f);
        physics::add_connection(state, o, k[3], k[1], 10.f, connection_type::normal, 0.7f, 0.5f);
        physics::set_go_through_floors(state, o, true);
        w.try_create_creature_body(c, o);
        w.creature_set_random(c, rng);
        w.creature_set_graphics_random(c, rw::rng{rng.next() | 1u});
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_set_grasps(c, creature::grasp_pair{});
        w.creature_set_behavior(c, uint8_t(behavior::idle));
        w.creature_set_current_utility(c, 0.f);
        w.creature_set_destination(c, tile_of(at));
        auto& p = pl(state, c);
        uint16_t const keep = p.data;
        p = creature::moth_grub_payload{};
        p.data = keep;
        p.local_head_way = vec2{0.f, 1.f};
        p.look_at_point = at + vec2{0.f, 40.f};
        p.look_off_way = vec2{0.f, 0.f};
        // AbstractCreature.Personality(ID).energy (the graphics' flapping)
        {
            rw::unity_random r{seed};
            auto const push = [](float v, float e) {
                if (v == 0.5f)
                    return 0.5f;
                return v < 0.5f ? custom::lerp_map(v, 0.f, 0.5f, 0.f, 0.5f, e) : custom::lerp_map(v, 1.f, 0.5f, 1.f, 0.5f, e);
            };
            r.value();
            p.energy = push(r.value(), 1.5f);
        }
        w.moth_grub_data_set_seed(d, seed);
        w.moth_grub_data_set_ai(d, ai_state{});
        w.moth_grub_data_set_graphics(d, graphics_state{});
        tracker::clear(state, c);
        moth_grub_graphics::reset_positions(state, c);
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        creature::release_target(state, c);
        creature::erase(state, c);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto& rng = w.creature_get_random(c);
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
        // Creature.Update + PhysicalObject.Update
        creature::stun_countdown(state, c);
        physics::update_object(state, room, body(state, c), nullptr, nullptr);
        // (growthStage: a grub kept in a shelter over a cycle hatches into a
        // big moth; no cycles end here)
        joint_body(state, c);
        vec2 const vector = custom::dir_vec(pos(state, c, 2), pos(state, c, 1));
        vec2 const a = custom::perpendicular_vector(vector);
        bool const awake = consious(state, c);
        if (awake)
            act(state, c, room);
        auto& p = pl(state, c);
        ++p.look_wiggle;
        bool const on_floor = w.body_chunk_get_contact_point(chunk(state, c, 1)).y == -1 || w.body_chunk_get_contact_point(chunk(state, c, 0)).y == -1;
        float speed = 0.15f;
        vec2 v2 = rw::clamp_magnitude((p.look_at_point - pos(state, c, 1)) / 20.f, 1.f);
        if (p.look_off_time == 0 && rng.value() < 0.004f) {
            // Random.insideUnitCircle
            vec2 const in_circle = custom::rnv(rng) * std::sqrt(rng.value());
            p.look_off_way = rw::clamp_magnitude(vector * 0.2f + in_circle, 1.f);
            p.look_off_time = rng.range(20, 120 / (rng.value() < 0.5f ? 1 : 2));
        }
        if (p.look_off_time > 0) {
            --p.look_off_time;
            v2 = p.look_off_way;
            v2 = rw::clamp_magnitude(v2 + perpendicular(v2) * std::sin(float(p.look_wiggle) * 0.2f) * 0.2f, 1.f);
            speed = 0.25f;
        }
        bool const held = creature::held(state, c);
        if (!held && on_floor && rng.value() < 0.02f && p.self_right_state == 0) {
            p.self_right_state = 1;
            p.self_right_timer = 0;
        }
        if (p.self_right_state != 0) {
            int32_t const num = vector.x > 0.f ? 1 : -1;
            if (p.self_right_state == 1) {
                speed = 0.15f;
                v2 = vec2{float(num), 0.f};
                if (++p.self_right_timer > 30) {
                    p.self_right_was_tilted = int8_t(num);
                    ++p.self_right_state;
                    p.self_right_timer = 0;
                }
            } else if (p.self_right_state == 2) {
                speed = 0.7f;
                v2 = rw::normalized(vec2{float(-p.self_right_was_tilted) * (1.f - mathf::max(rw::dot(vector, vec2{0.f, 1.f}), 0.f) * 1.4f), 1.f});
                if (++p.self_right_timer > 70) {
                    p.self_right_state = 0;
                    p.self_right_timer = 0;
                }
            }
        }
        if (!awake)
            v2 = rw::clamp_magnitude((pos(state, c, 0) - pos(state, c, 1)) / head_distance, 1.f);
        vec2 v3{rw::dot(v2, -a), rw::dot(v2, vector)};
        if (v3.y < 0.01f) {
            v3.y = 0.01f;
            v3.x = v3.x > 0.f ? 1.f : -1.f;
        }
        p.local_head_way = custom::move_towards(p.local_head_way, v3, speed);
        vec2 v4 = pos(state, c, 1) + rw::clamp_magnitude(vector * p.local_head_way.y - a * p.local_head_way.x, 1.f) * head_distance - pos(state, c, 0);
        v4 *= 0.2f * (held ? 0.3f : 1.f);
        vel(state, c, 0) += v4;
        pos(state, c, 0) += v4;
        for (uint32_t i = 1; i < chunk_count; ++i) {
            vel(state, c, i) -= v4 * 0.25f;
            pos(state, c, i) -= v4 * 0.25f;
        }
    }

    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) {
        return !creature::holds(state.world, t, c) && !creature::holds(state.world, c, t);
    }

    void collide(sys::game_state&, dcon::creature_id, room_view const&, uint32_t, dcon::creature_id, uint32_t) {}

    void violence(sys::game_state& state, dcon::creature_id c, uint32_t k, vec2 momentum, float damage, float stun_bonus, creature::damage_type) {
        auto& w = state.world;
        auto& rng = w.creature_get_random(c);
        if (k >= chunk_count)
            k = 0;
        // MothGrub.Violence: the head takes double
        if (k == 0) {
            damage *= 2.f;
            stun_bonus *= 2.f;
        }
        // Creature.Violence (HealthState)
        auto const hit = chunk(state, c, k);
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

    void release(sys::game_state&, dcon::creature_id, dcon::creature_id) {}

    void died(sys::game_state&, dcon::creature_id) {}

    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const view = world::get_room_view(state, room);
        ivec2 const t = tile_of(at);
        vec2 const dir = rw::to_vec2(view.shortcut_entrance_hole_direction(t));
        vec2 const mid = room_view::middle_of_tile(t);
        // Creature.SpitOutOfShortCut: the head first, out of the hole
        for (uint32_t i = 0; i < chunk_count; ++i) {
            auto const k = chunk(state, c, i);
            physics::hard_set_position(state, k, mid + dir * (i == 0 ? 5.f : -float(i) * 2.f));
            w.body_chunk_set_last_pos(k, mid);
            w.body_chunk_set_vel(k, dir * 2.f);
        }
        auto& a = w.moth_grub_data_get_ai(data(state, c));
        a.look_creature = dcon::creature_id{};
        pl(state, c).look_at_point = mid + dir * 40.f;
    }

    std::string describe(sys::game_state const& state, dcon::creature_id c) {
        static constexpr char const* names[] = {"idle", "flee", "escape rain"};
        auto const& w = state.world;
        vec2 const h = pos(state, c, 0);
        uint32_t const b = w.creature_get_behavior(c);
        auto const& p = pl(state, c);
        return "moth grub at " + std::to_string(int(h.x)) + "," + std::to_string(int(h.y)) + " behavior " + (b < 3 ? names[b] : "?")
            + (p.self_right_state ? " rolling upright" : "") + (creature::held(state, c) ? " carried" : "");
    }

    bool spawn_spot(room_view const& room, ivec2 tile) {
        return room.inside(tile.x, tile.y) && !room.get_tile(tile).solid() && room.get_tile(tile).water == 0 && room.get_tile(tile + ivec2{0, -1}).solid();
    }

    //
    // The player side
    //
    dcon::creature_id held_by(sys::game_state const& state, dcon::player_id p) noexcept {
        if (!p)
            return dcon::creature_id{};
        auto const c = creature::grasped(state.world, player::creature_of(state, p), 0);
        return c && creature::is(state.world, c, creature::type::moth_grub) ? c : dcon::creature_id{};
    }

    namespace {
        // CanIPickThisUp's chunk of a Drag object: the closest to the
        // slugcat's main chunk
        uint32_t closest_chunk(sys::game_state const& state, dcon::creature_id c, vec2 to) {
            uint32_t best = 0;
            float dst = 3.4e38f;
            for (uint32_t i = 0; i < chunk_count; ++i) {
                vec2 const cp = pos(state, c, i);
                if (custom::dist_less(to, cp, dst)) {
                    dst = rw::distance(to, cp);
                    best = i;
                }
            }
            return best;
        }
    }

    dcon::creature_id pickup_candidate(sys::game_state const& state, dcon::player_id p, room_view const& room, float& score) {
        auto const& w = state.world;
        vec2 const p0 = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
        int32_t const flip = w.player_get_flip_direction(p);
        dcon::creature_id result;
        score = 3.4e38f;
        rooms::for_each_creature(state, room.id, creature::type::moth_grub, [&](dcon::creature_id c) {
            // Player.Grabability: Drag, alive or dead
            if (creature::held(state, c) || w.creature_get_away(c) != 0)
                return;
            auto const k = chunk(state, c, closest_chunk(state, c, p0));
            vec2 const cp = w.body_chunk_get_pos(k);
            float const rad = w.body_chunk_get_rad(k);
            if (!custom::dist_less(p0, cp, rad + 40.f))
                return;
            if (!custom::dist_less(p0, cp, rad + 20.f) && !room.visual_contact(p0, cp))
                return;
            float dist = rw::distance(p0, cp);
            if ((cp.x < p0.x) == (flip < 0))
                dist -= 10.f;
            if (dist < score) {
                result = c;
                score = dist;
            }
        });
        return result;
    }

    void grab(sys::game_state& state, dcon::player_id p, dcon::creature_id c) {
        // SlugcatGrab: a Drag object by its chunk closest to the main chunk
        vec2 const p0 = state.world.body_chunk_get_pos(player::body_chunk(state, p, 0));
        creature::grab(state, player::creature_of(state, p), 0, c, closest_chunk(state, c, p0));
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
        uint32_t const gi = creature::grasp_chunk(w, pc, 0);
        auto const gc = chunk(state, c, gi < chunk_count ? gi : 0);
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
        // a Drag object too far behind is let go of
        if (dist > want * 2.f + 30.f)
            creature::release_grasp(state, pc, 0);
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
        for (uint32_t i = 0; i < chunk_count; ++i) {
            physics::hard_set_position(state, chunk(state, c, i), at + vec2{0.f, float(i) * -3.f});
            w.body_chunk_set_vel(chunk(state, c, i), w.body_chunk_get_vel(player::body_chunk(state, p, 0)));
        }
        w.moth_grub_data_get_ai(data(state, c)).look_creature = dcon::creature_id{};
        tracker::clear(state, c);
        moth_grub_graphics::reset_positions(state, c);
    }
}
