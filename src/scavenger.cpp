#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

#include "scavenger.hpp"
#include "body_parts.hpp"
#include "dens.hpp"
#include "items.hpp"
#include "migrate.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "rooms.hpp"
#include "scavenger_graphics.hpp"
#include "tracker.hpp"

namespace scavenger {
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;
    using creature::relationship;
    using creature::rel;

    namespace {
        // CreatureTemplate: Scavenger (ScavengerElite and ScavengerDisciple:
        // baseDamageResistance 2.2, instantDeathDamageLimit 3.5; the
        // Disciple's visualRadius 2500)
        constexpr float base_stun_resistance = 1.3f;
        // Weapon.exitThrownModeSpeed
        constexpr float exit_thrown_mode_speed = 30.f;
        // how long a scavenger threatens a hostile slugcat (Warning) before
        // it throws to kill (the community's reputation steps, simplified)
        constexpr int32_t warning_ticks = 160;
        constexpr float float_max = std::numeric_limits<float>::max();
        constexpr float float_min = std::numeric_limits<float>::lowest();

        // Custom.fourDirectionsAndZero
        constexpr ivec2 four_directions_and_zero[5] = {{-1, 0}, {0, -1}, {1, 0}, {0, 1}, {0, 0}};

        creature::scavenger_payload& pl(sys::game_state& state, dcon::creature_id c) { return state.world.creature_get_payload(c).scavenger; }
        creature::scavenger_payload const& pl(sys::game_state const& state, dcon::creature_id c) { return state.world.creature_get_payload(c).scavenger; }
        dcon::scavenger_data_id data(sys::game_state const& state, dcon::creature_id c) { return creature::scavenger_data(state.world, c); }
        body_state& bs(sys::game_state& state, dcon::creature_id c) { return state.world.scavenger_data_get_body(data(state, c)); }
        body_state const& bs(sys::game_state const& state, dcon::creature_id c) { return state.world.scavenger_data_get_body(data(state, c)); }
        ai_state& ai(sys::game_state& state, dcon::creature_id c) { return state.world.scavenger_data_get_ai(data(state, c)); }
        ai_state const& ai(sys::game_state const& state, dcon::creature_id c) { return state.world.scavenger_data_get_ai(data(state, c)); }
        personality const& pers(sys::game_state const& state, dcon::creature_id c) { return state.world.scavenger_data_get_personality(data(state, c)); }
        combat_skills const& skills(sys::game_state const& state, dcon::creature_id c) { return state.world.scavenger_data_get_skills(data(state, c)); }

        vec2& pos(sys::game_state& state, dcon::creature_id c, uint32_t i) { return state.world.body_chunk_get_pos(chunk(state, c, i)); }
        vec2 pos(sys::game_state const& state, dcon::creature_id c, uint32_t i) { return state.world.body_chunk_get_pos(chunk(state, c, i)); }
        vec2& vel(sys::game_state& state, dcon::creature_id c, uint32_t i) { return state.world.body_chunk_get_vel(chunk(state, c, i)); }
        vec2 last_pos(sys::game_state const& state, dcon::creature_id c, uint32_t i) { return state.world.body_chunk_get_last_pos(chunk(state, c, i)); }

        // Scavenger.Elite, Templar (only the Disciple is ported), Disciple
        bool elite(sys::game_state const& state, dcon::creature_id c) { return pl(state, c).variant == variant::elite; }
        bool templar(sys::game_state const& state, dcon::creature_id c) { return pl(state, c).variant == variant::disciple; }
        float base_damage_resistance(sys::game_state const& state, dcon::creature_id c) { return pl(state, c).variant == variant::scavenger ? 1.1f : 2.2f; }
        float instant_death_damage_limit(sys::game_state const& state, dcon::creature_id c) { return pl(state, c).variant == variant::scavenger ? 1.5f : 3.5f; }
        float visual_radius(sys::game_state const& state, dcon::creature_id c) { return templar(state, c) ? 2500.f : 1200.f; }
        // ScavengerAI.bloodLustAggressionThreshold
        constexpr int32_t blood_lust_aggression_threshold = 25;

        // abstractCreature.pos: the main chunk's tile
        ivec2 here(sys::game_state const& state, dcon::creature_id c) { return room_view::get_tile_position(pos(state, c, 0)); }

        bool solid(room_view const& room, ivec2 t) { return room.get_tile(t).solid(); }
        bool accessible(room_view const& room, ivec2 t) { return room.inside(t.x, t.y) && pathing::accessible(room, t, movement); }
        bool accessible(room_view const& room, vec2 p) { return accessible(room, room_view::get_tile_position(p)); }
        pathing::access acc(room_view const& room, ivec2 t) { return pathing::tile_access(room, t); }
        pathing::access acc(room_view const& room, vec2 p) { return acc(room, room_view::get_tile_position(p)); }
        float float_dist(ivec2 a, ivec2 b) { return rw::magnitude(rw::to_vec2(a - b)); }

        // Room.Tile.DeepWater / AnyWater
        bool deep_water(room_view const& room, ivec2 t) { return room.get_tile(t).water == 1; }
        bool any_water(room_view const& room, ivec2 t) { return room.get_tile(t).water != 0; }

        // Custom.PushFromHalf
        float push_from_half(float v, float e) {
            if (v == 0.5f)
                return 0.5f;
            if (v < 0.5f)
                return custom::lerp_map(v, 0.f, 0.5f, 0.f, 0.5f, e);
            return custom::lerp_map(v, 1.f, 0.5f, 1.f, 0.5f, e);
        }
        // Custom.Screen
        float screen(float a, float b) { return 1.f - (1.f - mathf::clamp01(a)) * (1.f - mathf::clamp01(b)); }

        // PhysicalObject.WeightedPush
        void weighted_push(sys::game_state& state, dcon::creature_id c, uint32_t a, uint32_t b, vec2 dir, float frc) {
            auto const& w = state.world;
            float const ma = w.body_chunk_get_mass(chunk(state, c, a)), mb = w.body_chunk_get_mass(chunk(state, c, b));
            float const num = mb / (ma + mb);
            vel(state, c, a) += dir * frc * num;
            vel(state, c, b) -= dir * frc * (1.f - num);
        }

        // Scavenger.Injured, LittleStuck, ReallyStuck
        float injured(sys::game_state const& state, dcon::creature_id c) {
            return mathf::inverse_lerp(elite(state, c) ? 0.25f : 0.5f, 0.f, state.world.creature_get_health(c));
        }
        float little_stuck(sys::game_state const& state, dcon::creature_id c) { return mathf::inverse_lerp(5.f, 20.f, float(pl(state, c).stuck_counter)); }
        float really_stuck(sys::game_state const& state, dcon::creature_id c) { return mathf::inverse_lerp(40.f, 200.f, float(pl(state, c).stuck_counter)); }

        // Scavenger.MovementSpeed
        float movement_speed(sys::game_state const& state, dcon::creature_id c) {
            auto const& a = ai(state, c);
            float const e = pers(state, c).energy;
            if (a.anim == animation::jumping)
                return std::fabs(e - 0.5f) * mathf::lerp(1.f, 0.1f, injured(state, c));
            if (a.anim == animation::throw_)
                return 0.f;
            return mathf::max(little_stuck(state, c), mathf::lerp(a.run_speed_goal, 0.3f + 0.7f * e, std::fabs(e - 0.5f)) * mathf::lerp(1.f, 0.1f, injured(state, c)));
        }

        // the creature's chunk `k` (none out of reach)
        std::optional<dcon::body_chunk_id> chunk_of(sys::game_state const& state, dcon::creature_id t, uint32_t k) {
            if (!creature::live(state, t) || k >= creature::chunk_count_of(state, t))
                return std::nullopt;
            return creature::chunk_of(state, t, k);
        }

        //
        // Items: what it holds
        //
        dcon::item_id grasp_item(sys::game_state const& state, dcon::creature_id c, uint32_t i) { return creature::grasped_item(state.world, c, i); }

        bool is_spear_type(world::item_type t) {
            return t == world::item_type::spear || t == world::item_type::explosive_spear || t == world::item_type::electric_spear;
        }
        bool is_spear(sys::game_state const& state, dcon::item_id it) { return it && is_spear_type(state.world.item_get_type(it)); }

        // ScavengerAI.WeaponScore (pickupDropInsteadOfWeaponSelection), for
        // the items ported
        int32_t weapon_score(sys::game_state const& state, dcon::creature_id c, dcon::item_id it, bool pickup) {
            if (!it)
                return 0;
            auto const& w = state.world;
            auto const lethal = ai(state, c).violence == violence_type::lethal;
            // the Templar with its blood lust up wants spears most (vanilla
            // scores even one stuck in a wall 6; not here)
            bool const lust = templar(state, c) && ai(state, c).blood_lust >= blood_lust_aggression_threshold;
            switch (w.item_get_type(it)) {
            case world::item_type::spear:
            case world::item_type::electric_spear:
                return w.item_get_mode(it) == world::weapon_mode::stuck_in_wall ? 0 : (lust ? 6 : 3);
            case world::item_type::explosive_spear:
                if (w.item_get_mode(it) == world::weapon_mode::stuck_in_wall)
                    return 0;
                if (lust)
                    return 6;
                return (!lethal && !pickup) ? 1 : 4;
            case world::item_type::rock: return 2;
            case world::item_type::bomb:
                if (pickup)
                    return 3;
                return lethal ? 3 : 0;
            case world::item_type::flare_bomb: return 1;
            case world::item_type::lilly_puck: return 1;
            default: return 0;
            }
        }
        // ScavengerAI.RealWeapon
        bool real_weapon(sys::game_state const& state, dcon::item_id it) {
            return it && (is_spear(state, it) || state.world.item_get_type(it) == world::item_type::bomb);
        }
        // ScavengerAI.CollectScore (weaponFiltered), the weapons only: the
        // pearls, food and the other valuables aren't collected here
        int32_t collect_score(sys::game_state const& state, dcon::creature_id c, dcon::item_id it) {
            return weapon_score(state, c, it, true);
        }

        // ScavengerAI.HoldWeapon
        bool hold_weapon(sys::game_state const& state, dcon::creature_id c);

        // Scavenger.ReleaseGrasp: what isn't thrown is tossed off
        void release_grasp(sys::game_state& state, dcon::creature_id c, uint32_t i) {
            auto& w = state.world;
            auto const it = grasp_item(state, c, i);
            if (it && w.item_get_mode(it) != world::weapon_mode::thrown) {
                auto& rng = w.creature_get_random(c);
                w.body_chunk_set_vel(items::chunk(state, it), vel(state, c, 0) + custom::deg_to_vec(mathf::lerp(-70.f, 70.f, rng.value())) * 8.2f);
            }
            creature::release_grasp(state, c, i);
        }

        // Creature.SwitchGrasps
        void switch_grasps(sys::game_state& state, dcon::creature_id c, uint32_t a, uint32_t b) {
            auto const ia = grasp_item(state, c, a), ib = grasp_item(state, c, b);
            if (ia)
                creature::release_grasp(state, c, a);
            if (ib)
                creature::release_grasp(state, c, b);
            if (ib)
                creature::grab_item(state, c, a, ib);
            if (ia)
                creature::grab_item(state, c, b, ia);
        }

        // Scavenger.PickUpAndPlaceInInventory
        void pick_up(sys::game_state& state, dcon::creature_id c, dcon::item_id it) {
            auto& w = state.world;
            if (!it || !w.item_is_valid(it) || items::chunk(state, it) == dcon::body_chunk_id{})
                return;
            if (w.physical_object_get_room_from_object_location(items::body(state, it)) != creature::room_of(state, c))
                return;
            int32_t g = -1;
            for (uint32_t i = 0; i < 2 && g < 0; ++i)
                if (!creature::grasped_object(w, c, i))
                    g = int32_t(i);
            if (g < 0) {
                int32_t lowest = std::numeric_limits<int32_t>::max();
                for (uint32_t i = 0; i < 2; ++i) {
                    int32_t const s = collect_score(state, c, grasp_item(state, c, i));
                    if (s < lowest) {
                        g = int32_t(i);
                        lowest = s;
                    }
                }
                if (lowest >= collect_score(state, c, it))
                    return;
                release_grasp(state, c, uint32_t(g));
            }
            auto& a = ai(state, c);
            a.arrange_inventory_counter = std::min(a.arrange_inventory_counter, w.creature_get_random(c).range(20, 60));
            items::creature_grab(state, c, uint32_t(g), it);
        }

        // Scavenger.ArrangeInventory (two grasps: grasps[0] the hand it
        // throws with, grasps[1] the other one)
        bool arrange_inventory(sys::game_state& state, dcon::creature_id c) {
            auto const& a = ai(state, c);
            if (a.anim == animation::throw_)
                return false;
            for (uint32_t i = 0; i < 2; ++i)
                if (auto const it = grasp_item(state, c, i); it && collect_score(state, c, it) < 1) {
                    release_grasp(state, c, i);
                    return true;
                }
            // a creature in its hands (none: it grabs only items)
            if (creature::grasped(state.world, c, 0) || creature::grasped(state.world, c, 1))
                return false;
            auto const first = grasp_item(state, c, 0);
            if (hold_weapon(state, c)) {
                int32_t const mine = weapon_score(state, c, first, false);
                if (auto const other = grasp_item(state, c, 1); other && weapon_score(state, c, other, false) > mine) {
                    switch_grasps(state, c, 0, 1);
                    return true;
                }
            } else if (first && !creature::grasped_object(state.world, c, 1)) {
                switch_grasps(state, c, 0, 1);
                return true;
            }
            return false;
        }

        // the items it holds go where it is (Scavenger.GraphicsModuleUpdated
        // without a graphics module: to the main chunk)
        void carry_items_unviewed(sys::game_state& state, dcon::creature_id c) {
            auto& w = state.world;
            for (uint32_t i = 0; i < 2; ++i) {
                auto const it = grasp_item(state, c, i);
                if (!it)
                    continue;
                auto const k = items::chunk(state, it);
                w.body_chunk_set_last_pos(k, w.body_chunk_get_pos(k));
                w.body_chunk_set_pos(k, pos(state, c, 0));
                w.body_chunk_set_vel(k, vec2{0.f, 0.f});
            }
        }

        //
        // The tracker (ArtificialIntelligence's VisualContact, the
        // RelationshipTracker)
        //
        // (the Elite's and the Templar's PreyTracker: sureToGetPreyDistance
        // 40, sureToLosePreyDistance 450)
        tracker::params const& tracker_params(sys::game_state const& state, dcon::creature_id c) {
            static tracker::params const plain = [] {
                tracker::params q;
                q.see_around_corners = 10;
                q.frames_to_remember = -1;
                q.ghost_dismissal_range = 10;
                q.prey_max = 10;
                q.persistance_bias = 1.f;
                q.sure_to_get = 5.f;
                q.sure_to_lose = 150.f;
                q.threat_max = 10;
                return q;
            }();
            static tracker::params const strong = [] {
                tracker::params q = plain;
                q.sure_to_get = 40.f;
                q.sure_to_lose = 450.f;
                return q;
            }();
            return pl(state, c).variant == variant::scavenger ? plain : strong;
        }

        // ArtificialIntelligence.VisualScore + ScavengerAI.VisualScore: it
        // sees from its head, best where it looks
        bool visual_contact(sys::game_state const& state, dcon::creature_id c, room_view const& room, vec2 look) {
            vec2 const eye = pos(state, c, 2);
            float const vr = visual_radius(state, c);
            if (!custom::dist_less(eye, look, vr))
                return false;
            float score = mathf::inverse_lerp(vr, 0.f, rw::distance(eye, look));
            auto const& a = ai(state, c);
            score -= mathf::lerp(0.8f, 0.2f, a.agitation) * mathf::inverse_lerp(1.f, mathf::lerp(-1.f, 0.8f, 1.f), rw::dot(head_look_dir(state, c), rw::normalized(look - eye)));
            score -= 1.f - 0.75f;
            return score > 0.f && room.visual_contact(eye, look);
        }

        // the like of the slugcat (LikeOfPlayer) leading to a relationship;
        // PlayerRelationship simplified: neutral (Ignores) or hostile
        // (Attacks while armed, else Afraid)
        relationship relationship_to(sys::game_state const& state, dcon::creature_id c, creature::tracked const& r) {
            auto const& w = state.world;
            if (!creature::live(state, r.who) || creature::dead(state, r.who))
                return relationship{};
            if (creature::is_slugcat(w, r.who)) {
                float const like = ai(state, c).like;
                if (pl(state, c).variant != variant::scavenger) {
                    // PlayerRelationship of the Elite and the Templar: their
                    // like threshold is 0.7, above any like here, so the
                    // slugcat is always below it. Attacks when
                    // aggression * the courage (num3) beats LerpMap(like, 0,
                    // 0.7, 0.08, 0.18), else Afraid; both at full intensity.
                    // The courage leaves out the pack and the slugcat being
                    // armed; the karmic armour adds 0.5 to both
                    auto const& p = pers(state, c);
                    bool const armoured = karmic_armor_protected(state, c);
                    float num3 = p.bravery * (1.f - 0.5f * ai(state, c).scared * (1.f - p.bravery));
                    if (armoured)
                        num3 += 0.5f;
                    float num7 = p.aggression * num3;
                    if (armoured)
                        num7 += 0.5f;
                    float const intensity = 0.5f + mathf::inverse_lerp(0.7f, 0.f, like) * 0.5f;
                    if (num7 > custom::lerp_map(like, 0.f, 0.7f, 0.08f, 0.18f))
                        return relationship{rel::attacks, intensity};
                    return relationship{rel::afraid, intensity};
                }
                if (like >= 0.f)
                    return relationship{};
                float const hate = mathf::inverse_lerp(0.f, -1.f, like);
                bool armed = false;
                for (uint32_t i = 0; i < 2; ++i)
                    armed = armed || weapon_score(state, c, grasp_item(state, c, i), false) > 0;
                if (armed)
                    return relationship{rel::attacks, mathf::lerp(0.6f, 1.f, hate)};
                return relationship{rel::afraid, mathf::lerp(0.3f, 0.7f, hate)};
            }
            return creature::static_relationship(state, c, r.who);
        }

        struct hook_ctx {
            sys::game_state& state;
            dcon::creature_id c;
            room_view const& room;
        };
        bool hook_sees(void* u, dcon::body_chunk_id k) {
            auto& x = *static_cast<hook_ctx*>(u);
            return visual_contact(x.state, x.c, x.room, x.state.world.body_chunk_get_pos(k));
        }
        bool hook_sees_point(void* u, vec2 p) {
            auto& x = *static_cast<hook_ctx*>(u);
            return visual_contact(x.state, x.c, x.room, p);
        }
        relationship hook_relate(void* u, creature::tracked const& r) {
            auto& x = *static_cast<hook_ctx*>(u);
            return relationship_to(x.state, x.c, r);
        }
        bool hook_reach(void* u, ivec2 t) {
            auto& x = *static_cast<hook_ctx*>(u);
            if (!accessible(x.room, t))
                return false;
            auto const f = pathing::field_to(x.state, x.room.id, here(x.state, x.c), movement);
            return pathing::distance(x.state, f, t) != pathing::unreachable;
        }
        tracker::hooks make_hooks(hook_ctx& x) {
            tracker::hooks h;
            h.user = &x;
            h.sees = hook_sees;
            h.sees_point = hook_sees_point;
            h.relate = hook_relate;
            h.module = tracker::module_without_aggression;
            h.reach = hook_reach;
            return h;
        }

        // ScavengerTrackState.taggedViolenceType: a hostile slugcat is
        // warned first, the threats are fought to kill
        violence_type violence_against(sys::game_state const& state, dcon::creature_id c, creature::tracked const& r) {
            if (r.rel.type != rel::attacks && r.rel.type != rel::afraid)
                return violence_type::none;
            if (creature::is_slugcat(state.world, r.who)) {
                auto const& a = ai(state, c);
                // the Elite always tags the slugcat Lethal; the Templar
                // NonLethal until its blood lust is up
                if (elite(state, c))
                    return violence_type::lethal;
                if (templar(state, c))
                    return a.blood_lust > blood_lust_aggression_threshold ? violence_type::lethal : violence_type::non_lethal;
                bool const close = custom::dist_less(pos(state, c, 0), r.last_seen, 200.f);
                return (a.warned < warning_ticks && !close && a.like > -0.9f) ? violence_type::warning : violence_type::lethal;
            }
            return violence_type::lethal;
        }

        bool hold_weapon(sys::game_state const& state, dcon::creature_id c) {
            auto const b = behavior(state.world.creature_get_behavior(c));
            return b == behavior::attack || b == behavior::flee || ai(state, c).scared > 0.3f || act_nervous(state, c) > 0.8f;
        }

        //
        // Path following (StandardPather.FollowPath over the distance field)
        //
        movement_connection follow_path(sys::game_state const& state, room_view const& room, pathing::field_id f, ivec2 from) {
            movement_connection m;
            if (!room.inside(from.x, from.y))
                return m;
            auto const step = pathing::next_step(state, f, from);
            if (!step) {
                // at the destination: the step that stays (vanilla's pather
                // hands back a connection there too)
                if (accessible(room, from) && pathing::distance(state, f, from) != pathing::unreachable) {
                    m.start = m.dest = from;
                    m.valid = true;
                }
                return m;
            }
            m.start = from;
            m.dest = *step;
            m.valid = true;
            ivec2 const d = *step - from;
            if (d.y < -1 || std::abs(d.x) > 1)
                m.type = movement_connection::drop;
            else if (d.x != 0 && d.y != 0)
                m.type = movement_connection::diagonal;
            return m;
        }

        //
        // The animations (ThrowCharge, Throw)
        //
        void stop_animation(sys::game_state& state, dcon::creature_id c) {
            auto& a = ai(state, c);
            a.anim = animation::none;
            a.anim_age = 0;
            a.charge_target = dcon::creature_id{};
            a.has_aim_target = 0;
        }
        // ThrowChargeAnimation's constructor
        void start_charge(sys::game_state& state, dcon::creature_id c, dcon::creature_id target, uint32_t k) {
            auto& a = ai(state, c);
            a.anim = animation::throw_charge;
            a.anim_age = 0;
            a.discontinue = 0;
            a.charge_cycle = 0.f;
            a.charge_shake = 0.f;
            a.charge_target = target;
            a.charge_chunk = uint8_t(k);
            a.has_aim_target = 0;
        }
        bool charging(sys::game_state const& state, dcon::creature_id c) {
            auto const& a = ai(state, c);
            return a.anim == animation::throw_charge && a.discontinue < 20;
        }
        // ThrowChargeAnimation.UseTarget
        vec2 charge_use_target(sys::game_state const& state, dcon::creature_id c) {
            auto const& a = ai(state, c);
            if (auto const k = chunk_of(state, a.charge_target, a.charge_chunk))
                return state.world.body_chunk_get_pos(*k);
            if (a.has_aim_target)
                return a.aim_target;
            return vec2{0.f, 0.f};
        }
        void animation_update(sys::game_state& state, dcon::creature_id c) {
            auto& a = ai(state, c);
            switch (a.anim) {
            case animation::none: return;
            // (the jumps: JumpLogicUpdate and Act's PrepareToJump branch)
            case animation::prepare_to_jump: return;
            case animation::jumping:
                // JumpingAnimation: Continue while it acts on a jump; Update
                // looks for a throw (in Act, after the AI) and goes through
                // floors
                if (!bs(state, c).has_act)
                    stop_animation(state, c);
                return;
            case animation::throw_charge:
                // Continue: discontinue < 60
                if (a.discontinue >= 60) {
                    stop_animation(state, c);
                    return;
                }
                ++a.anim_age;
                ++a.discontinue;
                a.charge_cycle += 1.f;
                a.charge_shake *= 0.95f;
                return;
            case animation::throw_:
                if (a.anim_age >= 20) {
                    stop_animation(state, c);
                    return;
                }
                ++a.anim_age;
                pl(state, c).flip = a.throw_flip;
                if (a.anim_age < 10 && a.thrown_item && state.world.item_is_valid(a.thrown_item))
                    vel(state, c, 2) += custom::dir_vec(pos(state, c, 2), state.world.body_chunk_get_pos(items::chunk(state, a.thrown_item))) * 3.f;
                if (a.anim_age == 18)
                    a.arrange_inventory_counter = std::min(a.arrange_inventory_counter, 10);
                return;
            }
        }

        // Scavenger.Throw (without DLCShared: frc 1.3, the speed set to
        // max(20, exitThrownModeSpeed + 5) along the throw direction, which
        // DLCShared's last line sets for every variant too)
        void throw_weapon(sys::game_state& state, dcon::creature_id c, vec2 dir) {
            auto& w = state.world;
            auto const it = grasp_item(state, c, 0);
            if (!it)
                return;
            auto& a = ai(state, c);
            // the Templar holds its spear back three times in four without
            // its blood lust
            if (templar(state, c) && is_spear(state, it) && a.blood_lust < blood_lust_aggression_threshold && w.creature_get_random(c).value() < 0.75f)
                return;
            a.anim = animation::throw_;
            a.anim_age = 0;
            a.throw_flip = mathf::sign(dir.x);
            a.thrown_item = it;
            // (the Templar watches where it lands: HitAnObjectWithWeapon)
            if (templar(state, c) && items::is_weapon(w.item_get_type(it)))
                a.watched_throw = it;
            // ThrowAnimation's constructor: the head swings forward
            pos(state, c, 2) = rw::lerp(pos(state, c, 2), pos(state, c, 0) + vec2{a.throw_flip * 30.f, 0.f}, 0.8f);
            a.reflex_build_up = mathf::lerp(a.reflex_build_up, 1.f, 0.5f);
            vec2 const main = pos(state, c, 0);
            // thrown from main + dir * 50 (thrownPos); the first frame's trace
            // runs from main - dir * 5 (firstFrameTraceFromPos): the item starts there
            if (items::is_weapon(w.item_get_type(it)))
                items::creature_throw(state, it, c, main - dir * 5.f, main + dir * 50.f, dir, mathf::max(20.f, exit_thrown_mode_speed + 5.f));
            else
                items::creature_throw(state, it, c, rw::lerp(w.body_chunk_get_pos(items::chunk(state, it)), main + dir * 50.f, 0.5f), main + dir * 50.f, dir, 20.f);
        }

        // ScavengerAI.IsThrowPathClearFromFriends
        bool throw_path_clear(sys::game_state const& state, dcon::creature_id c, vec2 throw_pos, float margin) {
            auto const& ts = tracker::of(state, c);
            vec2 const main = pos(state, c, 0);
            for (uint32_t i = 0; i < ts.count; ++i) {
                auto const& r = ts.reps[i];
                if (!r.visual || r.rel.type == rel::afraid || r.rel.type == rel::attacks || !creature::live(state, r.who))
                    continue;
                vec2 const p = creature::main_pos(state, r.who);
                if (rw::dot(rw::normalized(main - p), rw::normalized(main - throw_pos)) > 0.f && std::fabs(custom::distance_to_line(p, main, throw_pos)) < 50.f + margin
                    && custom::dist_less(main, p, rw::distance(main, throw_pos) + margin))
                    return false;
            }
            return true;
        }

        // Scavenger.TryThrow (the safari's parts aside; the Elite's flatter,
        // longer and quicker throws and the Templar's blood lust in)
        void try_throw(sys::game_state& state, dcon::creature_id c, room_view const& room, dcon::creature_id target, uint32_t k, violence_type vt) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            auto& a = ai(state, c);
            bool const el = elite(state, c);
            auto const held = grasp_item(state, c, 0);
            auto const aim = chunk_of(state, target, k);
            if (!held || !aim || a.anim == animation::throw_)
                return;
            vec2 const main = pos(state, c, 0);
            vec2 const ap = w.body_chunk_get_pos(*aim), alp = w.body_chunk_get_last_pos(*aim);
            if (std::fabs(custom::dir_vec(main, ap).x) < (el ? 0.f : 0.75f))
                return;
            bool const spear = is_spear(state, held);
            bool const nonlethal_real = real_weapon(state, held) && vt != violence_type::lethal;
            if (templar(state, c) && spear)
                a.blood_lust /= 3;
            vec2 v = ap + (alp - ap) * rw::distance(main, ap) * (0.02f + 0.02f * rng.value());
            if (!custom::dist_less(main, v, 200.f)) {
                float const r = rng.value();
                v.y += mathf::pow(rw::distance(main, v), el ? 1.f : 1.5f) * (el ? 0.35f : 0.05f) * (spear ? (0.08f + r * 0.1f) : (0.15f + r * 0.2f));
            }
            if (nonlethal_real) {
                // a warning: over its head
                v.y += 50.f * mathf::max(1.f, rw::distance(main, ap) / 400.f);
                uint32_t const n = creature::chunk_count_of(state, target);
                for (uint32_t i = 0; i < n; ++i) {
                    auto const kk = creature::chunk_of(state, target, i);
                    v.y = mathf::max(v.y, w.body_chunk_get_pos(kk).y + w.body_chunk_get_rad(kk) + 50.f);
                }
            } else if (auto const hit = exact_terrain_ray_trace_pos(room, ap, v)) {
                v = rw::lerp(*hit, v, 0.1f);
            }
            float const num3 = el ? 0.95f - mathf::inverse_lerp(150.f, 600.f, rw::distance(main, v)) * 0.05f : 0.95f;
            if (std::fabs(custom::dir_vec(main, v).x) < num3 || !room.visual_contact(main, v))
                return;
            vec2 mv = alp - ap;
            if (vt != violence_type::warning || !spear)
                mv.x *= 0.1f;
            auto const& p = pers(state, c);
            float const ra = el ? 400.f : 200.f, rb = el ? 1800.f : 900.f;
            float const num4 = el ? 40.f + mathf::inverse_lerp(150.f, 600.f, rw::distance(main, v)) * 60.f : 40.f;
            float num5;
            if (nonlethal_real) {
                num5 = rw::magnitude(mv) * mathf::max(20.f, rw::distance(main, v) - mathf::lerp(ra, rb, p.aggression)) * 0.05f;
            } else {
                num5 = rw::magnitude(mv) * mathf::max(10.f, rw::distance(main, v) - mathf::lerp(ra, rb, p.bravery)) * 0.1f;
                if (rw::distance(main, ap) < (el ? 240.f : 120.f) && rw::distance(main, ap) > 40.f && std::fabs(main.y - ap.y) < num4)
                    num5 = 0.f;
            }
            float num6 = 0.f;
            int32_t reals = 0;
            for (uint32_t i = 0; i < 2; ++i)
                if (auto const it = grasp_item(state, c, i)) {
                    num6 += float(weapon_score(state, c, it, false));
                    if (real_weapon(state, it))
                        ++reals;
                }
            if (w.item_get_type(held) == world::item_type::rock && vt != violence_type::warning) {
                num6 = num6 * 4.f + 20.f;
            } else if (real_weapon(state, held) && reals < 2) {
                num6 *= 0.2f + 0.8f * p.aggression;
            }
            num6 *= 1.f + a.agitation;
            num6 *= 1.f + p.aggression * 2.f;
            if (vt == violence_type::warning) {
                num6 *= 1.f - 0.5f * p.dominance;
                num5 = 0.f;
                if (a.anim == animation::throw_charge) {
                    a.charge_cycle += a.charge_shake * 0.3f;
                    a.charge_shake = mathf::lerp(a.charge_shake, 0.2f + 0.8f * mathf::inverse_lerp(1.f, 4.f, rw::magnitude(mv)), 0.5f);
                }
            }
            float const health = w.creature_get_health(c);
            num5 *= screen(health, 1.f - a.agitation);
            num6 *= custom::lerp_map(screen(health, 1.f - a.agitation), 0.f, 1.f, 10.f, 1.f);
            if (a.anim == animation::throw_charge || a.anim == animation::jumping) {
                if (a.anim == animation::throw_charge) {
                    a.charge_target = target;
                    a.charge_chunk = uint8_t(k);
                    if (a.discontinue < 30)
                        a.discontinue = 0;
                }
                if (nonlethal_real) {
                    v += custom::dir_vec(main, v) * 400.f;
                    if (auto const hit = exact_terrain_ray_trace_pos(room, main, v))
                        v = *hit;
                }
                float const quick = w.item_get_type(held) == world::item_type::rock ? 10.f : (el ? 20.f : 40.f);
                bool const charged = a.anim == animation::jumping || float(a.anim_age) > mathf::lerp(quick, 2.f, mathf::pow(mathf::max(p.aggression, 1.f - health), 0.93f));
                bool const go = charged
                    && (vt != violence_type::warning || !spear
                        || rng.value() < rw::magnitude(mv) / custom::lerp_map(num6, 2.f, 50.f, 100.f, 0.15f, mathf::lerp(0.8f, 0.2f, p.aggression * 0.5f)))
                    && (std::fabs(custom::dir_vec(main, v).x) > 0.975f || el) && num6 > num5 && throw_path_clear(state, c, v, nonlethal_real ? 40.f : 0.f);
                if (go) {
                    float const num8 = rw::distance(main, v);
                    if (num8 > 50.f && el) {
                        // the Elite's aim lifted for the drop
                        float const y = custom::dir_vec(main, v).y;
                        if (y >= 0.f && y <= 0.05f)
                            v.y += 17.f * (num8 / 180.f);
                        else if (y >= -0.1f && y <= 0.f)
                            v.y += 10.f * (num8 / 200.f);
                        else if (y <= 0.f)
                            v.y += 25.f * (num8 / 200.f);
                        else
                            v.y += 15.f * (num8 / 200.f);
                    }
                    throw_weapon(state, c, custom::dir_vec(main, v));
                }
                return;
            }
            if (num6 < num5 / 2.f)
                return;
            start_charge(state, c, target, k);
        }

        // ScavengerAI.WantToThrowSpearAtCreature
        float want_to_throw_at(sys::game_state& state, dcon::creature_id c, creature::tracked const& r) {
            auto& rng = state.world.creature_get_random(c);
            float const d = float_dist(here(state, c), tracker::best_guess(r));
            if ((r.rel.type != rel::afraid || d > mathf::lerp(5.f, 500.f, rng.value())) && r.rel.type != rel::attacks)
                return 0.f;
            return mathf::pow(r.rel.intensity * custom::lerp_map(d, 5.f, 50.f, 1.f, 0.5f), r.who == ai(state, c).focus ? 0.2f : 1.f);
        }

        // ScavengerAI.CheckThrow
        void check_throw(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            if (weapon_score(state, c, grasp_item(state, c, 0), false) < 1 || rng.value() < 0.5f)
                return;
            vec2 const main = pos(state, c, 0);
            float best = 0.f;
            dcon::creature_id target;
            violence_type vt = violence_type::none;
            auto const& ts = tracker::of(state, c);
            for (uint32_t i = 0; i < ts.count; ++i) {
                auto const& r = ts.reps[i];
                if (!r.visual || !creature::here(state, r.who, room.id))
                    continue;
                auto const t = violence_against(state, c, r);
                if (t == violence_type::none)
                    continue;
                float s = want_to_throw_at(state, c, r);
                if (s <= 0.f)
                    continue;
                s *= 0.2f + 0.8f * mathf::inverse_lerp(0.5f, 0.7f, std::fabs(custom::dir_vec(main, creature::main_pos(state, r.who)).x));
                if (s > best) {
                    best = s;
                    target = r.who;
                    vt = t;
                }
            }
            if (!target)
                return;
            float const flip = pl(state, c).flip;
            if (rng.value() < 0.7f && std::fabs(flip) > 0.5f && (creature::main_pos(state, target).x < main.x) != (flip < 0.f))
                return;
            int32_t k = -1;
            float top = 0.f;
            uint32_t const n = creature::chunk_count_of(state, target);
            for (uint32_t j = 0; j < n; ++j) {
                vec2 const cp = w.body_chunk_get_pos(creature::chunk_of(state, target, j));
                float const x = std::fabs(custom::dir_vec(main, cp).x);
                float const num6 = elite(state, c) ? 40.f + mathf::inverse_lerp(150.f, 600.f, rw::distance(main, cp)) * 60.f : 40.f;
                if (x > top && (x > 0.5f || std::fabs(main.y - cp.y) < num6) && custom::dist_less(main, cp, 1000.f) && !exact_terrain_ray_trace_pos(room, main, cp)) {
                    top = x;
                    k = int32_t(j);
                }
            }
            if (k < 0)
                return;
            try_throw(state, c, room, target, uint32_t(k), vt);
        }

        // Scavenger.WantToLethallyAttack
        bool want_to_lethally_attack(sys::game_state const& state, dcon::creature_id c, creature::tracked const* r) {
            if (!r || creature::dead(state, r->who) || violence_against(state, c, *r) != violence_type::lethal)
                return false;
            if (r->rel.type != rel::afraid && r->rel.type != rel::attacks)
                return false;
            auto const& a = ai(state, c);
            if (r->rel.intensity < mathf::pow(a.agitation * pers(state, c).aggression, 0.5f))
                return false;
            auto const b = behavior(state.world.creature_get_behavior(c));
            auto const& ts = tracker::of(state, c);
            return (b == behavior::flee && r->who == ts.threat) || (b == behavior::attack && r->who == ts.prey);
        }

        // Scavenger.TryToMeleeCreature (spear stabs: a throw from up close;
        // the Elite reaches 125 px, the Templar 35 px and skips the skill
        // and reaction check; a jump throws at once)
        void try_to_melee(sys::game_state& state, dcon::creature_id c) {
            auto& w = state.world;
            auto& a = ai(state, c);
            auto& rng = w.creature_get_random(c);
            bool const tp = templar(state, c);
            if (!tp && (rng.value() > skills(state, c).melee || !(a.reflex_build_up >= 1.f || rng.value() < 0.5f)))
                return;
            auto const k = chunk_of(state, a.melee_target, a.melee_chunk);
            if (!k)
                return;
            float const num2 = tp ? 35.f : (elite(state, c) ? 125.f : 50.f);
            vec2 const main = pos(state, c, 0);
            float const side = pos(state, c, 2).x < main.x ? -num2 : num2;
            vec2 const reach = main + vec2{side, 0.f};
            uint32_t const n = creature::chunk_count_of(state, a.melee_target);
            bool const ready = (charging(state, c) && a.anim_age > 5) || a.anim == animation::jumping;
            for (uint32_t i = 0; i < n + 1; ++i) {
                auto const kk = i == 0 ? *k : creature::chunk_of(state, a.melee_target, i - 1);
                vec2 const cp = w.body_chunk_get_pos(kk);
                if (!custom::dist_less(cp, reach, num2))
                    continue;
                if (ready) {
                    throw_weapon(state, c, custom::dir_vec(main, cp));
                    vel(state, c, 0) += custom::dir_vec(cp, main) * 3.f;
                    a.melee_target = dcon::creature_id{};
                    return;
                }
                if (a.anim == animation::none)
                    start_charge(state, c, a.melee_target, a.melee_chunk);
                return;
            }
        }

        // Scavenger.CombatUpdate (the mid range throws: MidRangeUpdate)
        void combat_update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& a = ai(state, c);
            auto& rng = w.creature_get_random(c);
            auto const& sk = skills(state, c);
            bool const el = elite(state, c);
            // the Templar's blood lust: at the creature its spear stunned
            if (a.blood_lust >= blood_lust_aggression_threshold && is_spear(state, grasp_item(state, c, 0)) && chunk_of(state, a.targeted, a.targeted_chunk)) {
                a.melee_target = a.targeted;
                a.melee_chunk = a.targeted_chunk;
                try_to_melee(state, c);
            }
            float const low = a.agitation * 0.25f;
            a.reflex_build_up = mathf::max(low, a.reflex_build_up - mathf::lerp(0.033333335f, 0.002f, sk.reaction));
            // MeleeRange
            float const melee_range = el ? 80.f + 110.f * sk.melee : 40.f + 100.f * sk.melee;
            if (a.melee_target) {
                auto const k = chunk_of(state, a.melee_target, a.melee_chunk);
                auto const held = grasp_item(state, c, 0);
                if (!k || !custom::dist_less(w.body_chunk_get_pos(*k), pos(state, c, 0), w.body_chunk_get_rad(*k) + melee_range)
                    || a.violence != violence_type::lethal || !held || w.item_get_type(held) == world::item_type::bomb
                    || w.item_get_type(held) == world::item_type::explosive_spear)
                    a.melee_target = dcon::creature_id{};
                else
                    try_to_melee(state, c);
            }
            if (rng.value() >= sk.mid_range * (0.2f + 0.8f * a.agitation))
                return;
            // MidRangeUpdate
            if (!is_spear(state, grasp_item(state, c, 0)) || rng.value() > sk.mid_range)
                return;
            auto const* f = tracker::find(tracker::of(state, c), a.focus);
            if (!f || !f->visual || !want_to_lethally_attack(state, c, f) || !creature::here(state, f->who, room.id))
                return;
            vec2 const main = pos(state, c, 0);
            vec2 const fp = creature::main_pos(state, f->who);
            // (MidRange)
            if (std::fabs(fp.y - main.y) > mathf::lerp(el ? 60.f : 30.f, el ? 100.f : 60.f, sk.mid_range)
                || !custom::dist_less(fp, main, (el ? 350.f : 200.f) + 200.f * sk.mid_range))
                return;
            uint32_t const k = uint32_t(rng.range(0, int32_t(creature::chunk_count_of(state, f->who))));
            auto const kc = creature::chunk_of(state, f->who, k);
            vec2 const cp = w.body_chunk_get_pos(kc), clp = w.body_chunk_get_last_pos(kc);
            if (std::fabs(cp.y - clp.y) > 60.f * sk.mid_range || custom::dir_vec(main, cp).y > 0.2f || !room.visual_contact(main, cp))
                return;
            // AimPosForChunk(chunk, 0.45, 0)
            float const t = mathf::max(0.f, std::fabs(cp.x - main.x) - 50.f) / (mathf::max(20.f, exit_thrown_mode_speed + 5.f) + std::fabs(cp.x - clp.x));
            vec2 aim{cp.x + (cp.x - clp.x) * t, cp.y};
            aim.y += 0.45f * t * t;
            aim.y += (cp.y - clp.y) * t;
            if (custom::dir_vec(main, aim).y > 0.25f)
                return;
            if (a.anim == animation::jumping || (charging(state, c) && a.anim_age > 5 && room.visual_contact(main, cp))) {
                // ReactionCheck (none mid-jump)
                if (a.anim != animation::jumping) {
                    a.reflex_build_up = mathf::min(1.f, a.reflex_build_up + mathf::lerp(0.008333334f, 0.5f, mathf::pow(sk.reaction, 3.5f)));
                    if (a.reflex_build_up < 1.f)
                        return;
                }
                if (!throw_path_clear(state, c, aim, 0.f))
                    return;
                throw_weapon(state, c, custom::dir_vec(main, aim));
                vel(state, c, 0) += custom::dir_vec(aim, main) * 3.f;
                return;
            }
            if (a.anim == animation::none)
                start_charge(state, c, f->who, k);
        }

        //
        // The Templar (the Disciple): karmic armour, its shockwave, the
        // DisciplePing, HitAnObjectWithWeapon
        //
        // whether `t` is already in a hit list (else it goes in; a full
        // list counts as hit)
        bool already_hit(dcon::creature_id* list, uint8_t& count, dcon::creature_id t) {
            for (uint8_t i = 0; i < count; ++i)
                if (list[i] == t)
                    return true;
            if (count >= ai_state::max_hit)
                return true;
            list[count++] = t;
            return false;
        }

        // Scavenger.DisciplePing: a ring from where it stands (KarmicArmor.rad
        // 40 while the armour is up)
        void start_ping(sys::game_state& state, dcon::creature_id c) {
            auto& a = ai(state, c);
            a.ping_frame = 0;
            a.ping_pos = pos(state, c, 0);
            a.ping_rad = a.armor_alive ? 40.f : 0.f;
            a.ping_inc = 0.2f;
            a.ping_hit_count = 0;
        }

        // DisciplePing.Update (TemplarCircle: rad += radIncrement,
        // radIncrement += 0.2, 100 frames): a creature it passes that is
        // moving (2.5 px a tick) is seen (PingReturnInfo). The weapons it
        // passes aren't (no ItemTracker)
        void ping_update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& a = ai(state, c);
            if (a.ping_frame < 0)
                return;
            a.ping_rad += a.ping_inc;
            a.ping_inc += 0.2f;
            ++a.ping_frame;
            hook_ctx hx{state, c, room};
            tracker::hooks const h = make_hooks(hx);
            rooms::for_each_creature(state, room.id, [&](dcon::creature_id t) {
                if (t == c || !creature::live(state, t))
                    return;
                auto const k = creature::chunk_of(state, t, 0);
                if (!custom::dist_less(a.ping_pos, state.world.body_chunk_get_pos(k), a.ping_rad) || already_hit(a.ping_hit, a.ping_hit_count, t))
                    return;
                if (rw::magnitude(state.world.body_chunk_get_vel(k)) >= 2.5f)
                    tracker::see(state, c, room, t, h);
            });
            if (a.ping_frame >= 100)
                a.ping_frame = -1;
        }

        // NoiseTracker (hearingSkill: 0.8 moving, else Lerp(1.4, 0.9,
        // agitation); the noises it sees are ignored) with
        // ScavengerAI.ReactToNoise, the Disciple's part: a ping when it has
        // no focus, else three times in ten
        void hear_noises(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& a = ai(state, c);
            auto& rng = state.world.creature_get_random(c);
            float const hearing = pl(state, c).moving ? 0.8f : mathf::lerp(1.4f, 0.9f, a.agitation);
            vec2 const head = pos(state, c, 2);
            auto const me = body(state, c);
            for (auto const& n : state.noises(room.id)) {
                if (a.ping_cd != 0)
                    return;
                if ((n.source && n.source == me) || !custom::dist_less(head, n.pos, n.strength * hearing) || visual_contact(state, c, room, n.pos))
                    continue;
                if (!a.focus || rng.value() > 0.7f) {
                    a.ping_cd = 100;
                    start_ping(state, c);
                }
            }
        }

        // KarmicArmor.Explode: a KarmicShockwave from the scavenger (its
        // sparks, strands, light and sounds aren't ported)
        void explode_armor(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& a = ai(state, c);
            a.armor_alive = 0;
            a.shock_frame = 0;
            a.shock_pos = pos(state, c, 0);
            a.shock_hit_count = 0;
            state.add_noise(room.id, a.shock_pos, 800.f, body(state, c));
        }

        // Scavenger.Update's karmic armour (a level lost every
        // karmicArmorResetTime ticks once triggered) and KarmicArmor.Update
        // (made while there is karma; shaking, then bursting once it's gone)
        void armor_update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& a = ai(state, c);
            if (a.karma_levels > 0 && a.armor_triggered && --a.armor_timer <= 0) {
                a.armor_timer = a.armor_reset_time;
                --a.karma_levels;
            }
            if (!a.armor_alive && a.karma_levels > 0) {
                a.armor_alive = 1;
                a.armor_level = a.karma_levels;
                a.armor_shake = 0.f;
            }
            if (!a.armor_alive)
                return;
            bool const imminent = a.armor_level == 0;
            if (a.armor_shake > 0.f && !imminent) {
                a.armor_shake = mathf::max(a.armor_shake - 0.01f, 0.f);
            } else if (imminent) {
                a.armor_shake = mathf::min(a.armor_shake + 0.015f, 1.f);
                if (a.armor_shake >= 1.f)
                    explode_armor(state, c, room);
            }
            if (a.armor_level != a.karma_levels) {
                a.armor_level = a.karma_levels;
                a.armor_shake = mathf::clamp01(a.armor_shake + (a.armor_level == 0 ? 0.5f : 0.6f));
            }
        }

        // KarmicShockwave.Update/StunCreatures: a ring from 15 to 300 px
        // over 20 frames, following the scavenger; every creature it reaches
        // (0.7 of its radius, plus the nearest chunk's) is thrown off,
        // stunned 50 ticks and dealt Violence(Explosion, 0.01, 2)
        void shock_update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& a = ai(state, c);
            if (a.shock_frame < 0)
                return;
            ++a.shock_frame;
            if (creature::here(state, c, room.id))
                a.shock_pos = pos(state, c, 0);
            float const rad = custom::lerp_map(float(a.shock_frame), 0.f, 20.f, 15.f, 300.f, 0.5f);
            float const num = rad * 0.7f;
            constexpr float max_rad = 300.f;
            constexpr float num2 = 8.f;
            auto const source = creature::template_of(state, c);
            std::vector<dcon::creature_id> hit;
            rooms::for_each_creature(state, room.id, [&](dcon::creature_id t) {
                if (t == c || !creature::live(state, t))
                    return;
                uint32_t const n = creature::chunk_count_of(state, t);
                float closest = float_max, crad = 0.f;
                for (uint32_t i = 0; i < n; ++i) {
                    auto const k = creature::chunk_of(state, t, i);
                    float const d = rw::distance(w.body_chunk_get_pos(k), a.shock_pos) - w.body_chunk_get_rad(k);
                    if (d < closest) {
                        closest = d;
                        crad = w.body_chunk_get_rad(k);
                    }
                }
                if (n == 0 || !custom::dist_less(a.shock_pos, creature::main_pos(state, t), num + crad) || already_hit(a.shock_hit, a.shock_hit_count, t))
                    return;
                hit.push_back(t);
            });
            for (auto const t : hit) {
                uint32_t const n = creature::chunk_count_of(state, t);
                for (uint32_t i = 0; i < n; ++i) {
                    auto const k = creature::chunk_of(state, t, i);
                    vec2 const kp = w.body_chunk_get_pos(k);
                    float const d = mathf::inverse_lerp(max_rad, max_rad * 0.75f, rw::distance(kp, a.shock_pos));
                    // PushAngle: away, a fifth of the way up
                    vec2 const dir = rw::slerp(rw::normalized(kp - a.shock_pos), vec2{0.f, 1.f}, 0.2f);
                    float const m = mathf::max(w.body_chunk_get_mass(k), 0.0001f);
                    w.body_chunk_get_vel(k) += dir * (num2 / m) * d;
                    w.body_chunk_get_pos(k) += dir * (num2 / m) * d * 0.1f;
                }
                creature::violence_on(state, t, creature::main_chunk_of(state, t), vec2{0.f, 0.f}, 0.01f, 2.f, creature::damage_type::explosion, source);
                creature::stun(state, t, 50);
            }
            if (a.shock_frame >= 20)
                a.shock_frame = -1;
        }

        // ScavengerAI.HitAnObjectWithWeapon, the Templar's: its thrown
        // weapon stopped being its throw without being picked up (a
        // creature hit clears thrownBy): the creature closest to it is the
        // stunnedCreature, its first chunk the targetedBodyChunk, and a
        // pounce is due (boomerangCharge)
        void watch_throw(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& a = ai(state, c);
            auto const it = a.watched_throw;
            if (!it)
                return;
            if (!w.item_is_valid(it)) {
                a.watched_throw = dcon::item_id{};
                return;
            }
            if (w.item_get_thrown_by(it) == c)
                return;
            a.watched_throw = dcon::item_id{};
            if (w.item_get_mode(it) == world::weapon_mode::carried || w.physical_object_get_room_from_object_location(items::body(state, it)) != room.id)
                return;
            vec2 const ip = w.body_chunk_get_pos(items::chunk(state, it));
            float best = 60.f;
            dcon::creature_id found;
            rooms::for_each_creature(state, room.id, [&](dcon::creature_id t) {
                if (t == c || !creature::live(state, t))
                    return;
                uint32_t const n = creature::chunk_count_of(state, t);
                for (uint32_t i = 0; i < n; ++i) {
                    auto const k = creature::chunk_of(state, t, i);
                    float const d = rw::distance(w.body_chunk_get_pos(k), ip) - w.body_chunk_get_rad(k);
                    if (d < best) {
                        best = d;
                        found = t;
                    }
                }
            });
            if (!found)
                return;
            a.stunned_creature = found;
            a.boomerang_charge = 1;
            a.targeted = found;
            a.targeted_chunk = 0;
        }

        //
        // ScavengerAI
        //
        // ScavengerAI.IdleScore (the tiles' visibility, the exit distances,
        // the discomfort and the places it idled at aren't ported)
        float idle_score(sys::game_state& state, dcon::creature_id c, room_view const& room, ivec2 t) {
            if (!accessible(room, t))
                return float_min;
            if (pathing::distance(state, pathing::field_to(state, room.id, here(state, c), movement), t) == pathing::unreachable)
                return float_min;
            auto const& w = state.world;
            float const dom = pers(state, c).dominance;
            float num = 0.f, top = 0.f;
            ivec2 leader = here(state, c);
            rooms::for_each_creature(state, room.id, ::creature::type::scavenger, [&](dcon::creature_id o) {
                if (o == c || !creature::here(state, o, room.id) || w.creature_get_dead(o))
                    return;
                float const od = pers(state, o).dominance;
                if (od <= dom)
                    return;
                num -= custom::lerp_map(float_dist(t, ai(state, o).test_idle_pos), 0.f, 4.f, 250.f, 0.f);
                if (od > top) {
                    top = od;
                    leader = w.creature_get_destination(o);
                }
            });
            if (top > 0.f && leader != here(state, c)) {
                auto const& p = pers(state, c);
                num += custom::lerp_map(float_dist(t, leader), 10.f + 40.f * mathf::pow(p.bravery, 0.5f), 60.f, mathf::lerp(500.f, 0.f, mathf::pow(p.dominance, 0.3f)), 0.f);
                if (float_dist(leader, t) < 3.f)
                    num -= 1000.f;
            }
            for (int32_t k = -1; k < 2; ++k) {
                if (acc(room, t + ivec2{k, 0}) != pathing::access::floor)
                    num -= 10.f;
                if (pathing::narrow_space(room, t + ivec2{k, 0}))
                    num -= 10.f;
            }
            if (!solid(room, t + ivec2{0, -1}))
                num -= 10.f;
            if (any_water(room, t))
                num -= 500.f;
            num -= 1000.f * tracker::threat_of_area(tracker::of(state, c), t, tracker_params(state, c).see_around_corners);
            return num;
        }

        // ScavengerAI.IdleBehavior
        void idle_behavior(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            auto& a = ai(state, c);
            ivec2& dest = w.creature_get_destination(c);
            vec2 const off = custom::rnv(rng);
            float const r = rng.value();
            ivec2 test = room_view::get_tile_position(pos(state, c, 0) + off * r * 800.f);
            if (rng.value() < 0.01f) {
                int32_t const x = rng.range(0, room.width);
                int32_t const y = rng.range(0, room.height);
                test = ivec2{x, y};
            }
            if (idle_score(state, c, room, test) > idle_score(state, c, room, a.test_idle_pos))
                a.test_idle_pos = test;
            float margin = float(200 + a.idle_counter);
            if (elite(state, c) && !pl(state, c).moving)
                margin = 0.f;
            if (a.test_idle_pos != dest && idle_score(state, c, room, a.test_idle_pos) > idle_score(state, c, room, dest) + margin)
                dest = a.test_idle_pos;
            --a.idle_counter;
            if (!pl(state, c).moving)
                a.idle_counter -= 3;
            if (a.idle_counter < 1) {
                a.idle_counter = rng.range(100, 200 + int32_t(1900.f * (1.f - pers(state, c).nervous))) * 4;
                dest = a.test_idle_pos;
            }
            ivec2 const step = dest + custom::four_directions[rng.range(0, 4)];
            if (idle_score(state, c, room, step) > idle_score(state, c, room, dest))
                dest = step;
        }

        // the throwing position: ScavengerAI.SpearThrowPositionScore
        // simplified (on the target's row, nothing solid between, not too
        // close nor too far, away from the threats)
        float throw_position_score(sys::game_state& state, dcon::creature_id c, room_view const& room, ivec2 t, ivec2 target) {
            if (!accessible(room, t))
                return float_min;
            if (pathing::distance(state, pathing::field_to(state, room.id, here(state, c), movement), t) == pathing::unreachable)
                return float_min;
            float num = 1.f;
            if (std::abs(t.y - target.y) < 2) {
                bool clear = true;
                for (int32_t x = std::min(t.x, target.x); x <= std::max(t.x, target.x) && clear; ++x)
                    clear = !solid(room, ivec2{x, t.y});
                if (clear)
                    num += 100.f;
            }
            if (float_dist(t, here(state, c)) < 3.f)
                num *= 2.f;
            float const d = float_dist(t, target);
            num *= custom::lerp_map(d, 30.f, 60.f, 1.f, 0.f);
            num *= custom::lerp_map(d, 5.f, 0.f, 1.f, 0.1f);
            num *= 1.f - mathf::clamp01(tracker::threat_of_area(tracker::of(state, c), t, tracker_params(state, c).see_around_corners));
            num *= std::abs(t.y - target.y) < 3 ? 1.f : mathf::inverse_lerp(5.f, 10.f, d);
            if (t == bs(state, c).occupy_tile)
                num *= 0.1f;
            return num;
        }

        // ScavengerAI.CheckForScavangeItems over the weapons it sees
        void check_for_scavenge_items(sys::game_state& state, dcon::creature_id c, room_view const& room, bool conservative) {
            auto& w = state.world;
            auto& a = ai(state, c);
            vec2 const main = pos(state, c, 0);
            float best = -10000.f;
            dcon::item_id found;
            rooms::for_each_item(state, room.id, [&](dcon::item_id it) {
                if (w.item_get_grabbed_by(it) || w.item_get_mode(it) != world::weapon_mode::free)
                    return;
                int32_t const score = collect_score(state, c, it);
                if (score < 1)
                    return;
                vec2 const ip = w.body_chunk_get_pos(items::chunk(state, it));
                if (!visual_contact(state, c, room, ip) || !accessible(room, room_view::get_tile_position(ip) ))
                    return;
                // ScavengerAI.PickUpItemScore (the Templar wants spears)
                float s = float(score) * 100.f - rw::distance(main, ip) * 0.1f;
                if (templar(state, c) && is_spear(state, it))
                    s += 100.f;
                s -= tracker::threat_of_area(tracker::of(state, c), room_view::get_tile_position(ip), tracker_params(state, c).see_around_corners) * 300.f;
                if (conservative && it == a.scavenge_candidate)
                    s += 100.f;
                if (s > best) {
                    best = s;
                    found = it;
                }
            });
            if (found)
                a.scavenge_candidate = found;
        }

        // Scavenger.LookForItemsToPickUp: a weapon close by
        void look_for_items_to_pick_up(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto const& a = ai(state, c);
            bool const has_free = !creature::grasped_object(w, c, 0) || !creature::grasped_object(w, c, 1);
            vec2 const main = pos(state, c, 0);
            dcon::item_id take;
            rooms::for_each_item(state, room.id, [&](dcon::item_id it) {
                if (take || w.item_get_grabbed_by(it) || w.physical_object_get_grabbed_by_count(items::body(state, it)) > 0)
                    return;
                auto const mode = w.item_get_mode(it);
                if (mode == world::weapon_mode::thrown || mode == world::weapon_mode::stuck_in_wall)
                    return;
                vec2 const ip = w.body_chunk_get_pos(items::chunk(state, it));
                if (!custom::dist_less(main, ip, 50.f) || !room.visual_contact(main, ip))
                    return;
                int32_t const s = collect_score(state, c, it);
                if (s <= 0)
                    return;
                if (!has_free && a.scavenge_candidate && w.item_is_valid(a.scavenge_candidate) && s < collect_score(state, c, a.scavenge_candidate))
                    return;
                take = it;
            });
            if (take)
                pick_up(state, c, take);
        }

        // ScavengerAI.UpdateLookPoint (LookPointScore without the tiles'
        // visibility)
        float look_point_score(sys::game_state const& state, dcon::creature_id c, room_view const& room, vec2 tst, rw::rng& rng) {
            if (room.get_tile(tst).solid())
                return float_min;
            auto const& a = ai(state, c);
            vec2 const main = pos(state, c, 0);
            float num = 100.f;
            num -= std::fabs(500.f - rw::distance(main, tst)) * 0.25f;
            num += std::fabs(custom::dir_vec(main, tst).x) * 100.f;
            num += mathf::min(500.f, rw::distance(tst, a.already_looked_at_pos));
            if ((tst.x < main.x) == (pl(state, c).flip < 0.f))
                num += 70.f;
            if ((tst.x < main.x) != (a.test_look_pos.x < main.x))
                num -= 70.f;
            vec2 const dest = room_view::middle_of_tile(state.world.creature_get_destination(c));
            num += mathf::inverse_lerp(1000.f, 0.f, rw::distance(tst, dest)) * mathf::inverse_lerp(0.f, 500.f, rw::distance(main, dest)) * 500.f;
            return num + rng.value() * mathf::pow(pers(state, c).nervous, 2.5f) * 100.f;
        }
        void update_look_point(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            auto& a = ai(state, c);
            auto& b = bs(state, c);
            float const nerv = act_nervous(state, c);
            a.already_looked_at_pos += custom::dir_vec(a.already_looked_at_pos, b.look_point);
            a.already_looked_at_pos = rw::lerp(a.already_looked_at_pos, b.look_point, mathf::lerp(0.01f, 0.12f, mathf::pow(nerv, 1.2f)));
            vec2 const rv = custom::rnv(rng);
            float const rr = rng.value();
            vec2 v = a.test_look_pos + rv * mathf::lerp(10.f * pers(state, c).energy, 20.f + 30.f * nerv, rr);
            if (rng.value() < 1.f / mathf::lerp(22.f, 6.f, mathf::pow(nerv, 1.5f))) {
                vec2 const main = pos(state, c, 0);
                if (rng.value() < 0.5f) {
                    float const ang = 45.f + 90.f * rng.value();
                    float const side = rng.value() < 0.5f ? -1.f : 1.f;
                    float const len = mathf::lerp(200.f, 700.f, rng.value());
                    v = main + custom::deg_to_vec(ang) * side * len;
                } else {
                    vec2 const d = custom::rnv(rng);
                    v = main + d * mathf::lerp(200.f, 700.f, rng.value());
                }
            }
            if (!room.get_tile(v).solid() && look_point_score(state, c, room, v, rng) > look_point_score(state, c, room, a.test_look_pos, rng))
                a.test_look_pos = v;
            float const diff = look_point_score(state, c, room, a.test_look_pos, rng) - look_point_score(state, c, room, b.look_point, rng);
            b.look_point = rw::lerp(b.look_point, a.test_look_pos,
                custom::lerp_map(diff, mathf::lerp(200.f, 0.f, nerv), mathf::lerp(600.f, 200.f, mathf::pow(nerv, 1.5f)), 0.f, mathf::lerp(0.1f, 0.6f, pers(state, c).energy)));
        }

        // ScavengerAI.AttackBehavior (simplified throwing positions)
        void attack_behavior(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            auto& a = ai(state, c);
            auto const* prey = tracker::prey(tracker::of(state, c));
            if (!prey)
                return;
            a.focus = prey->who;
            ivec2& dest = w.creature_get_destination(c);
            // the Templar's blood lust with a spear in hand: straight at it
            // (the stab comes from CombatUpdate and the collisions)
            if (a.blood_lust >= blood_lust_aggression_threshold && is_spear(state, grasp_item(state, c, 0))) {
                dest = tracker::best_guess(*prey);
                return;
            }
            int32_t num = 0;
            for (uint32_t i = 0; i < 2; ++i)
                num = std::max(num, weapon_score(state, c, grasp_item(state, c, i), false));
            check_throw(state, c, room);
            // the Templar fights with spears only: else it fetches one
            if (templar(state, c) && !is_spear(state, grasp_item(state, c, 0)) && !is_spear(state, grasp_item(state, c, 1)))
                num = 0;
            if (num == 0) {
                // RetrieveWeapon: to the weapon it saw, else away from it
                if (a.scavenge_candidate && w.item_is_valid(a.scavenge_candidate))
                    dest = room_view::get_tile_position(w.body_chunk_get_pos(items::chunk(state, a.scavenge_candidate)));
                return;
            }
            ivec2 const target = tracker::best_guess(*prey);
            ivec2 const at = here(state, c);
            ivec2 test;
            if (rng.value() < 0.5f) {
                int32_t const side = rng.value() < 0.5f ? -1 : 1;
                test = target;
                for (int32_t k = 0; k < 40; ++k) {
                    ivec2 const t = target + ivec2{side * k, 0};
                    if (solid(room, t))
                        break;
                    if (k > 5 && accessible(room, t)) {
                        test = t;
                        break;
                    }
                }
            } else {
                int32_t const dx = rng.range(1, 10) * (rng.value() < 0.5f ? -1 : 1);
                int32_t const dy = rng.range(1, 10) * (rng.value() < 0.5f ? -1 : 1);
                test = at + ivec2{dx, dy};
            }
            if (throw_position_score(state, c, room, test, target) > throw_position_score(state, c, room, a.test_throw_pos, target))
                a.test_throw_pos = test;
            if (a.test_throw_pos != dest
                && (custom::manhattan_distance(at, dest) < 3
                    || throw_position_score(state, c, room, a.test_throw_pos, target) > throw_position_score(state, c, room, dest, target) + float(a.change_attack_position_delay))) {
                dest = a.test_throw_pos;
                a.change_attack_position_delay = 400;
            }
            if (a.change_attack_position_delay > 0)
                --a.change_attack_position_delay;
        }

        // ScavengerAI.DecideBehavior (the threat, prey, rain, injury,
        // travelling and scavenging; no noise, outpost, pack or trading)
        void decide_behavior(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& a = ai(state, c);
            auto const& ts = tracker::of(state, c);
            float utility = 0.f;
            behavior b = behavior::idle;
            auto const consider = [&](float v, behavior k) {
                if (v > utility) {
                    utility = v;
                    b = k;
                }
            };
            // (the Templar Attacks its threat while it holds a spear)
            bool const spear_in_hand = is_spear(state, grasp_item(state, c, 0)) || is_spear(state, grasp_item(state, c, 1));
            consider(tracker::threat_utility(ts), templar(state, c) && spear_in_hand ? behavior::attack : behavior::flee);
            consider(tracker::prey_utility(state, c, room, here(state, c), tracker_params(state, c)) * 0.5f, behavior::attack);
            consider(dens::rain_utility(state, c, room.id), behavior::escape_rain);
            // InjuryTracker(0.01)
            consider(custom::s_curve(mathf::inverse_lerp(0.2f, 0.9f, 1.f - mathf::clamp01(w.creature_get_health(c))), 0.01f), behavior::injured);
            float const current = utility;
            float num = utility;
            if (current < 0.05f) {
                b = behavior::idle;
                num = 0.f;
            }
            float const travel = 0.08f + mathf::inverse_lerp(0.5f, 1.f, a.scared) * 0.2f;
            if (num < travel && migrate::want_to(state, c)) {
                b = behavior::travel;
                num = travel;
            }
            if (a.scavenge_candidate && w.item_is_valid(a.scavenge_candidate)) {
                float const num3 = mathf::min(0.3f, float(collect_score(state, c, a.scavenge_candidate)) * 0.05f);
                if (num < num3) {
                    int32_t lowest = std::numeric_limits<int32_t>::max();
                    for (uint32_t i = 0; i < 2 && lowest > 0; ++i) {
                        if (!creature::grasped_object(w, c, i))
                            lowest = 0;
                        else
                            lowest = std::min(lowest, collect_score(state, c, grasp_item(state, c, i)));
                    }
                    ivec2 const it = room_view::get_tile_position(w.body_chunk_get_pos(items::chunk(state, a.scavenge_candidate)));
                    int32_t const sac = tracker_params(state, c).see_around_corners;
                    if (collect_score(state, c, a.scavenge_candidate) > lowest && tracker::threat_of_area(ts, it, sac) <= tracker::threat_of_area(ts, bs(state, c).occupy_tile, sac))
                        b = behavior::scavenge;
                }
            }
            w.creature_set_behavior(c, uint8_t(b));
            w.creature_set_current_utility(c, current);
        }

        // ScavengerAI.Update
        void ai_update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            ivec2 const at = here(state, c);
            hook_ctx hx{state, c, room};
            tracker::hooks const h = make_hooks(hx);
            tracker::update(state, c, room, at, tracker_params(state, c), h);
            auto& a = ai(state, c);
            auto const& p = pers(state, c);
            ++a.age;
            // the Templar's: its blood lust (two spears in hand, the creature
            // it stunned still stunned), the ping's cool down, the noises
            if (templar(state, c)) {
                int32_t spears = 0;
                for (uint32_t i = 0; i < 2; ++i)
                    if (is_spear(state, grasp_item(state, c, i)))
                        ++spears;
                if (a.blood_lust < blood_lust_aggression_threshold && spears > 1)
                    a.blood_lust += 2;
                if (creature::live(state, a.stunned_creature) && w.creature_get_stun(a.stunned_creature) > 0)
                    a.blood_lust += 4;
                else
                    a.stunned_creature = dcon::creature_id{};
                if (a.blood_lust > 20)
                    --a.blood_lust;
                if (a.ping_cd > 0) {
                    --a.ping_cd;
                    if (behavior(w.creature_get_behavior(c)) != behavior::attack && a.ping_cd > 0)
                        --a.ping_cd;
                }
                hear_noises(state, c, room);
            }
            // the abstract AI's roll for a trip to another room
            if (rng.value() < 0.002f)
                migrate::abstract_behavior(state, c, room);
            if (--a.arrange_inventory_counter < 0) {
                bool const f = arrange_inventory(state, c);
                a.arrange_inventory_counter = rng.range(10, (!pl(state, c).moving || f) ? 20 : 400);
            }
            decide_behavior(state, c, room);
            update_look_point(state, c, room);
            if (--a.scavage_item_check < 1) {
                check_for_scavenge_items(state, c, room, true);
                a.scavage_item_check = rng.range(40, 200);
            }
            if (a.scavenge_candidate) {
                bool const gone = !w.item_is_valid(a.scavenge_candidate) || w.item_get_grabbed_by(a.scavenge_candidate)
                    || w.physical_object_get_room_from_object_location(items::body(state, a.scavenge_candidate)) != room.id;
                if (!gone && float_dist(room_view::get_tile_position(w.body_chunk_get_pos(items::chunk(state, a.scavenge_candidate))), at) < 4.f)
                    look_for_items_to_pick_up(state, c, room);
                if (gone || !w.item_is_valid(a.scavenge_candidate) || w.item_get_grabbed_by(a.scavenge_candidate) || collect_score(state, c, a.scavenge_candidate) < 1) {
                    a.scavenge_candidate = dcon::item_id{};
                    check_for_scavenge_items(state, c, room, false);
                }
            }
            // UpdateCurrentViolenceType, and the warning running out
            if (auto const* f = tracker::find(tracker::of(state, c), a.focus)) {
                auto const v = violence_against(state, c, *f);
                if (v != a.violence)
                    a.arrange_inventory_counter = std::min(5, a.arrange_inventory_counter);
                a.violence = v;
                if (v == violence_type::warning && f->visual)
                    ++a.warned;
            }
            float num = 0.f;
            ivec2& dest = w.creature_get_destination(c);
            auto const b = behavior(w.creature_get_behavior(c));
            switch (b) {
            case behavior::idle:
                a.run_speed_goal = p.energy * 0.3f + p.nervous * 0.7f;
                idle_behavior(state, c, room);
                break;
            case behavior::travel:
                a.run_speed_goal = 0.2f + p.energy * 0.6f + a.agitation * 0.2f;
                if (auto const e = migrate::travel_tile(state, c, room))
                    dest = *e;
                break;
            case behavior::attack:
                a.run_speed_goal = 1.f;
                attack_behavior(state, c, room);
                num = 1.f;
                break;
            case behavior::flee:
                a.run_speed_goal = 1.f;
                a.focus = tracker::of(state, c).threat;
                dest = tracker::flee_to(state, c, room, bs(state, c).occupy_tile.x >= 0 ? bs(state, c).occupy_tile : at, 10, 30, tracker_params(state, c), h);
                if (rng.value() < 0.25f)
                    check_throw(state, c, room);
                break;
            case behavior::escape_rain:
            case behavior::injured:
                a.run_speed_goal = 0.7f;
                if (auto const e = dens::home_tile(state, c, room))
                    dest = *e;
                num = 0.5f;
                if (rng.value() < 0.25f)
                    check_throw(state, c, room);
                break;
            case behavior::scavenge:
                a.run_speed_goal = 0.7f;
                if (a.scavenge_candidate && w.item_is_valid(a.scavenge_candidate)) {
                    vec2 const ip = w.body_chunk_get_pos(items::chunk(state, a.scavenge_candidate));
                    dest = room_view::get_tile_position(ip);
                    if (custom::dist_less(pos(state, c, 0), ip, 50.f)) {
                        pick_up(state, c, a.scavenge_candidate);
                        a.scavenge_candidate = dcon::item_id{};
                    }
                } else {
                    a.scavenge_candidate = dcon::item_id{};
                }
                break;
            }
            a.run_speed_goal = mathf::lerp(a.run_speed_goal, mathf::max(a.run_speed_goal, a.scared), 0.5f);
            num = mathf::max(num, a.scared);
            float const num2 = mathf::pow(tracker::threat_utility(tracker::of(state, c)), mathf::lerp(0.2f, 1.8f, p.bravery));
            a.scared = mathf::lerp(a.scared, num2, 0.01f);
            if (a.scared < num2)
                a.scared = mathf::min(num2, a.scared + 0.033333335f);
            else
                a.scared = mathf::max(num2, a.scared - 1.f / mathf::lerp(900.f, 30.f, mathf::pow(p.bravery, 0.7f)));
            if (a.agitation < num)
                a.agitation = mathf::min(num, a.agitation + 1.f / (180.f * (1.f + a.agitation * 2.f)));
            else
                a.agitation = mathf::max(num, a.agitation - 1.f / (mathf::lerp(280.f, 600.f, p.nervous) * (1.f + a.agitation * 2.f)));
        }

        //
        // Scavenger.cs's movement
        //
        // Scavenger.SwimTile
        bool swim_tile(room_view const& room, ivec2 t) {
            return deep_water(room, t) || (any_water(room, t) && !solid(room, t + ivec2{0, -1}));
        }
        // Scavenger.TileLegalForSwinging
        bool tile_legal_for_swinging(sys::game_state& state, dcon::creature_id c, room_view const& room, ivec2 t) {
            if (acc(room, t) != pathing::access::climb || pathing::ai_terrain_proximity(state, room.id, t) < 3)
                return false;
            int32_t const alt = pathing::floor_altitude(room, t);
            return (alt <= -1 || alt >= 4) && room.visual_contact(pos(state, c, 0), room_view::middle_of_tile(t));
        }
        // Scavenger.TileViableForNextSwingPos
        bool tile_viable_for_next_swing_pos(sys::game_state const& state, dcon::creature_id c, room_view const& room, ivec2 t) {
            auto const& b = bs(state, c);
            if (float(t.y) > b.swing_pos.y + 1.f)
                return false;
            float const num = rw::distance(room_view::middle_of_tile(b.swing_pos), room_view::middle_of_tile(t));
            return num >= 5.f && num <= 60.f && room.visual_contact(pos(state, c, 0), room_view::middle_of_tile(t));
        }
        // Scavenger.KnucklePosLegal
        bool knuckle_pos_legal(sys::game_state const& state, dcon::creature_id c, std::optional<vec2> t) {
            if (!t)
                return false;
            vec2 const main = pos(state, c, 0);
            return custom::dist_less(main, *t, custom::lerp_map((t->x - main.x) * pl(state, c).flip, 0.f, 40.f, 20.f, 150.f));
        }

        // Scavenger.Swing
        void swing(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& b = bs(state, c);
            auto& x = pl(state, c);
            if (!b.has_next_swing_pos) {
                for (int32_t i = int32_t(b.connection_count) - 1; i >= 1; --i)
                    if (tile_viable_for_next_swing_pos(state, c, room, b.connections[i].dest)) {
                        b.next_swing_pos = room_view::middle_of_tile(b.connections[i].dest);
                        b.has_next_swing_pos = 1;
                        break;
                    }
            } else if (!tile_viable_for_next_swing_pos(state, c, room, room_view::get_tile_position(b.next_swing_pos))) {
                b.has_next_swing_pos = 0;
            } else {
                bool found = false;
                for (uint8_t i = 0; i < b.connection_count && !found; ++i)
                    found = room_view::get_tile_position(b.next_swing_pos) == b.connections[i].dest;
                if (!found)
                    b.has_next_swing_pos = 0;
            }
            if (b.swing_radius < 50.f)
                b.swing_radius = mathf::min(b.swing_radius + 0.5f + 4.f * mathf::inverse_lerp(0.5f, 1.f, b.swing_progress), 50.f);
            else
                b.swing_radius = mathf::max(b.swing_radius - 0.5f - 4.f * mathf::inverse_lerp(0.5f, 1.f, b.swing_progress), 50.f);
            vec2 const main = pos(state, c, 0);
            if (b.has_next_swing_pos) {
                b.swing_progress = mathf::clamp(b.swing_progress + 0.0125f, 0.f, 1.f);
                vel(state, c, 0) += custom::perpendicular_vector(main, b.swing_pos) * 1.2f * mathf::sign(b.next_swing_pos.x - main.x)
                    * std::sin(mathf::inverse_lerp(0.25f, 1.f, b.swing_progress) * mathf::pi);
                vel(state, c, 0) += custom::dir_vec(main, b.next_swing_pos) * 1.2f * mathf::pow(mathf::inverse_lerp(0.75f, 1.f, b.swing_progress), 2.f);
                if (x.moving)
                    x.flip = mathf::lerp(x.flip, mathf::clamp(b.next_swing_pos.x - main.x, -1.f, 1.f), 0.1f);
                if (b.swing_progress > 0.1f && custom::dist_less(main, b.next_swing_pos, 50.f)) {
                    b.swing_arm = uint8_t(1 - b.swing_arm);
                    b.swing_radius = rw::distance(main, b.next_swing_pos);
                    b.swing_pos = b.next_swing_pos;
                    b.has_next_swing_pos = 0;
                    b.swing_progress = 0.f;
                    // (Scavenger_Grab_Pole: no sound id)
                }
            }
            if (!x.moving)
                x.flip = mathf::lerp(x.flip, mathf::clamp(head_look_dir(state, c).x * 10.f, -1.f, 1.f), 0.1f);
            if (std::fabs(w.body_chunk_get_vel(chunk(state, c, 0)).x) < 3.f)
                for (uint8_t j = 0; j < std::min<uint8_t>(b.connection_count, 3); ++j)
                    if (b.connections[j].is_drop()) {
                        b.commited_to_move = b.connections[j];
                        x.commit_to_move_counter = 20;
                        x.drop = 1;
                        b.has_swing_pos = 0;
                        b.has_next_swing_pos = 0;
                        break;
                    }
            if (b.has_swing_pos && !custom::dist_less(pos(state, c, 0), b.swing_pos, 100.f))
                b.has_swing_pos = 0;
        }

        // Scavenger.TightClimbing
        void tight_climbing(sys::game_state& state, dcon::creature_id c, movement_connection const& follow, bool any_narrow) {
            auto& w = state.world;
            auto& b = bs(state, c);
            auto& x = pl(state, c);
            vel(state, c, 0) *= 0.8f;
            auto const& last = b.connections[b.connection_count - 1];
            if (last.dest.x != follow.start.x)
                b.climb_orientation.x = custom::int_clamp(last.dest.x - follow.start.x, -1, 1);
            if (last.dest.y != follow.start.y)
                b.climb_orientation.y = custom::int_clamp(last.dest.y - follow.start.y, -1, 1);
            vec2 v = rw::to_vec2(b.climb_orientation) * 8.f;
            if (follow.start.x == follow.dest.x)
                v.y *= 0.f;
            if (follow.start.y == follow.dest.y)
                v.x *= 0.f;
            float const speed = movement_speed(state, c);
            if (!x.moving) {
                vel(state, c, 0) += rw::clamp_magnitude(room_view::middle_of_tile(w.creature_get_destination(c)) - pos(state, c, 0), 10.f) / 10.f;
            } else {
                if (any_narrow)
                    v *= 0.f;
                vel(state, c, 0) += rw::clamp_magnitude(room_view::middle_of_tile(follow.dest) + v * 2.f - pos(state, c, 0), 9.f) / 10.f * speed;
                vel(state, c, 1) += rw::clamp_magnitude(room_view::middle_of_tile(follow.start) + v * 0.8f - pos(state, c, 1), 9.f) / 10.f * speed;
            }
            vec2 const md = room_view::middle_of_tile(follow.dest);
            for (uint32_t i = 0; i < 2; ++i) {
                vec2& cv = vel(state, c, i);
                if (follow.start.x == follow.dest.x && std::fabs(cv.x) < 4.f && pos(state, c, 0).y > pos(state, c, 1).y)
                    cv.x += mathf::clamp(md.x + v.x * (i == 0 ? 2.f : 0.8f) - pos(state, c, i).x, -5.f, 5.f) * 0.05f;
                else if (follow.start.y == follow.dest.y && std::fabs(cv.y) < 4.f)
                    cv.y += mathf::clamp(md.y + v.y * (i == 0 ? 2.f : 0.8f) - pos(state, c, i).y, -5.f, 5.f) * 0.05f;
            }
            if (follow.start.x == follow.dest.x)
                x.flip = mathf::lerp(x.flip, custom::lerp_map(md.x - pos(state, c, 0).x, -20.f, 20.f, -1.f, 1.f) * float(custom::int_clamp(follow.dest.y - follow.start.y, -1, 1)), 0.1f);
            if (follow.start.y == follow.dest.y)
                x.flip = mathf::lerp(x.flip, custom::lerp_map(md.y - pos(state, c, 0).y, -6.f, 6.f, -1.f, 1.f) * float(custom::int_clamp(follow.start.x - follow.dest.x, -1, 1)), 0.1f);
        }

        // Scavenger.LookForAndMoveToUnstuckTile
        void look_for_unstuck_tile(sys::game_state& state, dcon::creature_id c, room_view const& room, pathing::field_id f) {
            auto& rng = state.world.creature_get_random(c);
            ivec2 best = here(state, c);
            int32_t most = 0;
            for (uint32_t i = 0; i < 2; ++i)
                for (auto const d : custom::eight_directions) {
                    ivec2 const t = room_view::get_tile_position(pos(state, c, i)) + d;
                    if (!accessible(room, t))
                        continue;
                    int32_t n = 0;
                    ivec2 at = t;
                    for (int32_t k = 0; k < 15; ++k) {
                        auto const m = follow_path(state, room, f, at);
                        if (!m.valid || m.dest == at)
                            break;
                        ++n;
                        at = m.dest;
                    }
                    if (n > most) {
                        most = n;
                        best = t;
                    }
                }
            if (most > 2)
                for (uint32_t l = 0; l < 3; ++l)
                    vel(state, c, l) += custom::dir_vec(pos(state, c, 0), room_view::middle_of_tile(best)) * (1.f + really_stuck(state, c) * rng.value());
        }

        // Scavenger.GetUnstuckRoutine (no pathing with exits)
        void get_unstuck_routine(sys::game_state& state, dcon::creature_id c, room_view const& room, pathing::field_id f) {
            auto& rng = state.world.creature_get_random(c);
            auto& b = bs(state, c);
            float const rs = really_stuck(state, c);
            bool moved = false;
            if (rs > 0.1f && rs < 0.9f && b.connection_count < 3) {
                vec2 const v0 = vel(state, c, 0);
                look_for_unstuck_tile(state, c, room, f);
                moved = vel(state, c, 0) != v0;
            }
            if (rs > 0.2f && !moved) {
                std::optional<ivec2> entrance;
                for (uint32_t i = 0; i < 3 && !entrance; ++i)
                    for (auto const d : custom::eight_directions_and_zero) {
                        ivec2 const t = room_view::get_tile_position(pos(state, c, i)) + d;
                        if (room.get_terrain(t.x, t.y) == world::terrain::shortcut_entrance) {
                            entrance = t;
                            break;
                        }
                    }
                if (entrance) {
                    b.stuck_on_shortcut_counter = std::min(b.stuck_on_shortcut_counter + 1, 100);
                    if (b.stuck_on_shortcut_counter > 25)
                        for (uint32_t i = 0; i < 3; ++i) {
                            vec2 const r = custom::rnv(rng);
                            vel(state, c, i) += rw::to_vec2(room.shortcut_entrance_hole_direction(*entrance)) * 7.f + r * rng.value() * 5.f;
                        }
                }
            } else if (b.stuck_on_shortcut_counter > 0) {
                --b.stuck_on_shortcut_counter;
            }
            if (b.stuck_on_shortcut_counter > 25)
                for (uint32_t j = 0; j < 3; ++j) {
                    vec2 const r = custom::rnv(rng);
                    vel(state, c, j) += r * 6.f * rng.value();
                }
        }

        // the swing catch: a pole on its way down
        void catch_pole(sys::game_state& state, dcon::creature_id c, vec2 at) {
            auto& b = bs(state, c);
            auto& x = pl(state, c);
            b.swing_pos = at;
            b.has_swing_pos = 1;
            b.swing_radius = 50.f;
            x.mov_mode = move_mode::climb;
            x.drop = 0;
        }

        //
        // DLCShared's jumps: the Elite's JumpFinders (Scavenger.JumpFinder,
        // RunningUpdate, InitiateJump, Jump, JumpingUpdate) and the
        // Templar's pounce. The pathing cells' costToGoal are the path
        // field's costs to the destination (no generations)
        //
        // SharedPhysics.RayTracedTilesArray, the tiles from `a` to `b` in
        // order: `f` returns false to stop
        template<typename F>
        void for_each_ray_tile(vec2 a, vec2 b, F&& f) {
            float const ax = a.x / 20.f, ay = a.y / 20.f, bx = b.x / 20.f, by = b.y / 20.f;
            float const dx = std::fabs(bx - ax), dy = std::fabs(by - ay);
            int32_t x = int32_t(std::floor(ax)), y = int32_t(std::floor(ay));
            float const dt_dx = 1.f / dx, dt_dy = 1.f / dy;
            int32_t n = 1, x_inc = 0, y_inc = 0;
            float t_next_h, t_next_v;
            if (dx == 0.f) {
                t_next_h = dt_dx;
            } else if (bx > ax) {
                x_inc = 1;
                n += int32_t(std::floor(bx)) - x;
                t_next_h = (std::floor(ax) + 1.f - ax) * dt_dx;
            } else {
                x_inc = -1;
                n += x - int32_t(std::floor(bx));
                t_next_h = (ax - std::floor(ax)) * dt_dx;
            }
            if (dy == 0.f) {
                t_next_v = dt_dy;
            } else if (by > ay) {
                y_inc = 1;
                n += int32_t(std::floor(by)) - y;
                t_next_v = (std::floor(ay) + 1.f - ay) * dt_dy;
            } else {
                y_inc = -1;
                n += y - int32_t(std::floor(by));
                t_next_v = (ay - std::floor(ay)) * dt_dy;
            }
            n = std::min(n, 4096);
            for (; n > 0; --n) {
                if (!f(ivec2{x, y}))
                    return;
                if (t_next_v < t_next_h) {
                    y += y_inc;
                    t_next_v += dt_dy;
                } else {
                    x += x_inc;
                    t_next_h += dt_dx;
                }
            }
        }

        // the path field to the destination
        pathing::field_id dest_field(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            return pathing::field_to(state, room.id, state.world.creature_get_destination(c), movement);
        }
        // the field's cost from `t` to its goal
        uint16_t path_cost(sys::game_state const& state, room_view const& room, pathing::field_id f, ivec2 t) {
            if (!room.inside(t.x, t.y) || !accessible(room, t))
                return pathing::unreachable;
            return pathing::distance(state, f, t);
        }
        // JumpFinder.PathWeightComparison(A, B): B is the better cell (A
        // none: any B)
        bool path_weight_comparison(bool a_none, uint16_t a, uint16_t b) {
            if (a_none)
                return true;
            if (b == pathing::unreachable)
                return false;
            return a == pathing::unreachable || b < a;
        }

        // Scavenger.InStandardRunMode
        bool in_standard_run_mode(sys::game_state const& state, dcon::creature_id c) {
            auto const anim = ai(state, c).anim;
            return !bs(state, c).has_act && anim != animation::prepare_to_jump && anim != animation::jumping;
        }

        // Scavenger.InitiateJump: the PrepareToJumpAnimation, every finder gone
        void initiate_jump(sys::game_state& state, dcon::creature_id c, jump_instruction const& j, ivec2 start, int32_t length) {
            if (!j.has_goal)
                return;
            auto& b = bs(state, c);
            auto& a = ai(state, c);
            b.has_act = 1;
            b.act = j;
            b.act_start = start;
            b.act_fade = 0;
            b.jump_counter = 0;
            b.jump_anim_length = length;
            b.jump_cycle = 0.f;
            b.finder_count = 0;
            stop_animation(state, c);
            a.anim = animation::prepare_to_jump;
        }

        // JumpFinder.NewTest
        void new_test(sys::game_state& state, dcon::creature_id c, room_view const& room, jump_finder& f) {
            auto& rng = state.world.creature_get_random(c);
            float num = rng.value() * mathf::pow(ai(state, c).run_speed_goal, 0.5f);
            if (pathing::ai_terrain_proximity(state, room.id, f.start) > 1)
                num *= 0.5f;
            float num2 = mathf::lerp(14.f, 50.f, num);
            if (creature::grasped_object(state.world, c, 0))
                num2 *= 0.75f;
            {
                float const r = rng.value();
                num2 *= mathf::lerp(1.f, 1.f - injured(state, c), 0.5f + 0.5f * r);
            }
            float const r1 = rng.value();
            float const r2 = rng.value();
            float const r3 = rng.value();
            vec2 const init = custom::deg_to_vec((45.f * mathf::pow(r1, 0.75f) + 135.f * mathf::pow(r2, 2.f)) * (r3 >= 0.5f ? 1.f : -1.f)) * num2;
            f.current = jump_instruction{};
            f.current.start_pos = room_view::middle_of_tile(f.start);
            f.current.init_vel = init;
            f.current.power = num;
            f.best_shares = 0;
            f.pos = f.last_pos = room_view::middle_of_tile(f.start);
            f.vel = init;
            f.ventured = 0;
            if (!f.has_best) {
                f.best = f.current;
                f.has_best = 1;
                f.best_shares = 1;
            }
        }

        // JumpFinder.Iterate: one tick of the test jump's flight
        void iterate(sys::game_state& state, dcon::creature_id c, room_view const& room, pathing::field_id pf, jump_finder& f) {
            f.last_pos = f.pos;
            f.pos += f.vel;
            f.vel *= 0.999f;
            f.vel.y -= 0.9f;
            vec2 const a = custom::perpendicular_vector(f.last_pos, f.pos);
            ivec2 const dest = state.world.creature_get_destination(c);
            bool abort = false;
            for_each_ray_tile(f.last_pos, f.pos, [&](ivec2 t) {
                if (solid(room, t) || t.y < 0 || t.y < room.default_water_level || pathing::narrow_space(room, t)) {
                    abort = true;
                    return false;
                }
                int32_t const prox = pathing::ai_terrain_proximity(state, room.id, t);
                bool const beam = room.get_tile(t).any_beam();
                if (!f.ventured && prox > 1 && !beam)
                    f.ventured = 1;
                if (f.ventured && accessible(room, t) && (prox == 1 || beam)
                    && float_dist(f.start, t) > float(custom::int_clamp(int32_t(rw::magnitude(f.current.init_vel) / 3.f), 5, 20)) && float_dist(dest, t) > 3.f) {
                    uint16_t const best = f.best.has_goal ? path_cost(state, room, pf, f.best.goal) : pathing::unreachable;
                    if (path_weight_comparison(!f.best.has_goal, best, path_cost(state, room, pf, t))) {
                        f.best = f.current;
                        f.best_shares = 1;
                        f.best.goal = t;
                        f.best.has_goal = 1;
                        vec2 const a2 = room_view::middle_of_tile(t);
                        vec2 const v = custom::dir_vec(f.last_pos, f.pos);
                        f.best.grab_when_landing = !room.get_tile(a2 + custom::perpendicular_vector(v) * 15.f + v * 20.f).solid();
                    }
                }
                float const side = custom::lerp_map(float(f.current.tick), 5.f, 20.f, 10.f, 20.f);
                if ((!solid(room, f.start + ivec2{0, 1}) && solid(room, t + ivec2{0, 1})) || room.get_tile(room_view::middle_of_tile(t) + a * side).solid()
                    || room.get_tile(room_view::middle_of_tile(t) - a * side).solid()) {
                    abort = true;
                    return false;
                }
                return true;
            });
            if (abort) {
                new_test(state, c, room, f);
                return;
            }
            ++f.current.tick;
            if (f.best_shares)
                f.best.tick = f.current.tick;
            if (f.current.tick > 700)
                new_test(state, c, room, f);
        }

        // JumpFinder.BeneficialMovement
        bool beneficial_movement(sys::game_state& state, dcon::creature_id c, room_view const& room, pathing::field_id pf, jump_finder const& f) {
            if (!f.has_best || !f.best.has_goal || float_dist(f.start, f.best.goal) <= 5.f
                || float_dist(state.world.creature_get_destination(c), f.best.goal) <= 3.f)
                return false;
            auto const& b = bs(state, c);
            uint16_t const goal = path_cost(state, room, pf, f.best.goal);
            if (b.has_jump_cell && path_weight_comparison(false, goal, path_cost(state, room, pf, b.jump_cell)))
                return false;
            return !path_weight_comparison(false, goal, path_cost(state, room, pf, f.start));
        }

        // a new JumpFinder at `t`
        void add_finder(sys::game_state& state, dcon::creature_id c, room_view const& room, ivec2 t) {
            auto& b = bs(state, c);
            if (b.finder_count >= body_state::max_jump_finders)
                return;
            jump_finder& f = b.finders[b.finder_count++];
            f = jump_finder{};
            f.start = t;
            new_test(state, c, room, f);
        }

        // JumpFinder.Update for all of them (the destroyed ones go)
        void update_finders(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& b = bs(state, c);
            bool const standard = in_standard_run_mode(state, c);
            int32_t const count = b.finder_count;
            if (count == 0)
                return;
            auto const pf = dest_field(state, c, room);
            uint8_t kept = 0;
            for (int32_t i = 0; i < count; ++i) {
                jump_finder& f = b.finders[i];
                if (standard) {
                    ++f.fade;
                    uint16_t const jc = b.has_jump_cell ? path_cost(state, room, pf, b.jump_cell) : pathing::unreachable;
                    if (!path_weight_comparison(!b.has_jump_cell, jc, path_cost(state, room, pf, f.start)))
                        f.fade += 10;
                    if (f.fade > 40)
                        continue;
                }
                for (int32_t k = std::max(1, 100 / std::max(1, count)); k >= 0; --k)
                    iterate(state, c, room, pf, f);
                if (kept != i)
                    b.finders[kept] = f;
                ++kept;
            }
            b.finder_count = kept;
        }

        // Scavenger.RunningUpdate (the Templar's pounce at what it stunned,
        // else the Elite's jump finders along the path ahead)
        void running_update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            auto& a = ai(state, c);
            auto& b = bs(state, c);
            auto const& x = pl(state, c);
            ivec2 const at = here(state, c);
            auto const pf = dest_field(state, c, room);
            if (templar(state, c)) {
                if (!a.boomerang_charge || a.blood_lust < blood_lust_aggression_threshold)
                    return;
                a.boomerang_charge = 0;
                if (!creature::live(state, a.stunned_creature) || !creature::here(state, a.stunned_creature, room.id))
                    return;
                vec2 const sp = creature::main_pos(state, a.stunned_creature);
                float const num = rw::distance(pos(state, c, 0), sp);
                if (num <= 151.f)
                    return;
                ivec2 const st = room_view::get_tile_position(sp);
                bool const left = at.x > st.x;
                float const t = num / 300.f;
                float const r1 = rng.value();
                float const r2 = rng.value();
                jump_instruction j;
                j.start_pos = pos(state, c, 0);
                j.init_vel = vec2{left ? -11.5f : 11.5f, 13.5f};
                j.power = mathf::lerp(0.05f + mathf::lerp(-0.05f, 0.05f, r1), 0.5f + mathf::lerp(-0.1f, 0.1f, r2), t);
                j.goal = st;
                j.has_goal = 1;
                initiate_jump(state, c, j, at, 12);
                return;
            }
            if (x.mov_mode == move_mode::run || (x.mov_mode == move_mode::climb && !b.has_swing_pos))
                for (int32_t i = int32_t(b.finder_count) - 1; i >= 0; --i)
                    if (b.finders[i].start == at && beneficial_movement(state, c, room, pf, b.finders[i])) {
                        jump_finder const f = b.finders[i];
                        initiate_jump(state, c, f.best, f.start, 30);
                        return;
                    }
            ivec2 list[5];
            int32_t listed = 0;
            bool on_path = false;
            ivec2 wc = at;
            for (int32_t j = 0; j < 5; ++j) {
                auto const m = follow_path(state, room, pf, wc);
                if (!m.valid || m.dest == m.start)
                    break;
                wc = m.dest;
                list[listed++] = wc;
                for (uint8_t k = 0; k < b.finder_count; ++k)
                    if (b.finders[k].start == wc) {
                        b.finders[k].fade = 0;
                        on_path = true;
                    }
            }
            if (path_cost(state, room, pf, at) != pathing::unreachable) {
                b.jump_cell = at;
                b.has_jump_cell = 1;
            }
            if (b.add_delay > 0) {
                --b.add_delay;
                return;
            }
            if (int32_t(b.finder_count) < (on_path ? 1 : 4) && listed > 1) {
                ivec2 const t = list[rng.range(0, listed)];
                uint16_t const jc = b.has_jump_cell ? path_cost(state, room, pf, b.jump_cell) : pathing::unreachable;
                if (float_dist(t, at) > 2.f && !pathing::narrow_space(room, t) && path_weight_comparison(!b.has_jump_cell, jc, path_cost(state, room, pf, t)))
                    add_finder(state, c, room, t);
            }
        }

        // Scavenger.Jump: off the start tile with the jump's velocity
        void jump(sys::game_state& state, dcon::creature_id c) {
            auto& b = bs(state, c);
            auto& x = pl(state, c);
            x.mov_mode = move_mode::run;
            b.move_mode_change_counter = 0;
            x.drop = 0;
            b.has_swing_pos = 0;
            b.has_next_swing_pos = 0;
            x.climbing_up_coming = 0;
            x.commit_to_move_counter = 0;
            vec2 const a0 = room_view::middle_of_tile(b.act_start);
            vec2 const init = b.act.init_vel;
            vec2 const n = rw::normalized(init);
            for (uint32_t i = 0; i < 3; ++i)
                pos(state, c, i) = a0 + n * float(1 - int32_t(i)) * 8.f;
            vec2 const d = a0 + init - pos(state, c, 0);
            for (uint32_t j = 0; j < 3; ++j) {
                pos(state, c, j) += d;
                vel(state, c, j) = vec2{mathf::clamp(init.x, -22.f, 22.f), mathf::min(init.y, 17.f)};
            }
            b.jump_counter = 0;
            b.add_delay = 20;
            b.spin = 0;
            if (b.act.tick > 25 && rw::dot(rw::normalized(pos(state, c, 0) - pos(state, c, 2)), n) < -0.1f)
                b.spin = int8_t(mathf::sign(init.x));
        }

        // PrepareToJumpAnimation.Update: crouching on the start tile, then
        // the JumpingAnimation (which jumps)
        void prepare_to_jump_update(sys::game_state& state, dcon::creature_id c) {
            auto& a = ai(state, c);
            auto& b = bs(state, c);
            ++a.anim_age;
            vec2 const start = room_view::middle_of_tile(b.act_start);
            if (!b.has_act || !consious(state, c) || !custom::dist_less(pos(state, c, 0), start, 50.f)) {
                stop_animation(state, c);
                return;
            }
            vec2 const n = rw::normalized(b.act.init_vel);
            vel(state, c, 0) *= 0.5f;
            pos(state, c, 0) = rw::lerp(pos(state, c, 0), start - n * float(a.anim_age) * 0.5f, 0.2f);
            vel(state, c, 2) += n * 1.5f;
            vel(state, c, 1) -= n * 2.f;
            b.jump_cycle += 1.f;
            pos(state, c, 1) += custom::perpendicular_vector(custom::dir_vec(pos(state, c, 0), pos(state, c, 1))) * std::sin(b.jump_cycle)
                * (float(a.anim_age) / float(b.jump_anim_length)) * 10.f;
            if (a.anim_age > b.jump_anim_length - 5) {
                a.anim = animation::jumping;
                a.anim_age = 0;
                jump(state, c);
            }
        }

        // Scavenger.JumpingUpdate
        void jumping_update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& b = bs(state, c);
            ++b.jump_counter;
            bool end = false;
            if (!b.has_act) {
                end = true;
            } else {
                if (b.jump_counter > b.act.tick + 5) {
                    for (uint32_t i = 0; i < 3; ++i)
                        if (accessible(room, pos(state, c, i)))
                            vel(state, c, i) *= 0.5f;
                } else if (b.jump_counter >= 5) {
                    b.has_act = 0;
                }
                if (b.has_act) {
                    vec2 const to = room_view::middle_of_tile(b.act.goal);
                    for (uint32_t j = 0; j < 3 && !end; ++j)
                        end = custom::dist_less(pos(state, c, j), to, 40.f);
                }
            }
            if (end || (b.has_act && b.jump_counter >= b.act.tick + 20)) {
                if (b.has_act && b.act.grab_when_landing)
                    for (uint32_t k = 0; k < 3; ++k)
                        vel(state, c, k) *= 0.5f;
                b.has_act = 0;
                stop_animation(state, c);
            }
        }

        // Scavenger.JumpLogicUpdate (the Elite and the Templar)
        void jump_logic_update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            if (state.world.creature_get_dead(c))
                return;
            auto& a = ai(state, c);
            auto& b = bs(state, c);
            if (!consious(state, c) && (a.anim == animation::prepare_to_jump || a.anim == animation::jumping))
                stop_animation(state, c);
            update_finders(state, c, room);
            if (a.anim == animation::jumping)
                jumping_update(state, c, room);
            else if (in_standard_run_mode(state, c))
                running_update(state, c, room);
            if (b.has_act && a.anim != animation::prepare_to_jump && a.anim != animation::jumping && ++b.act_fade > 40)
                b.has_act = 0;
        }

        // Scavenger.Act, the movement (the AI and the combat first)
        void act(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto& rng = w.creature_get_random(c);
            auto& x = pl(state, c);
            // PrepareToJump: only the animation runs (no AI, no moving)
            if (ai(state, c).anim == animation::prepare_to_jump) {
                prepare_to_jump_update(state, c);
                return;
            }
            ai_update(state, c, room);
            animation_update(state, c);
            if (ai(state, c).anim == animation::jumping) {
                // JumpingAnimation.Update: a throw on the way, through floors
                check_throw(state, c, room);
                physics::set_go_through_floors(state, body(state, c), true);
            }
            combat_update(state, c, room);
            if (ai(state, c).anim == animation::jumping) {
                // mid-jump the body flies on (vanilla's AllowIdleMoves: no
                // path following; the head and standing pushes are left out
                // too)
                x.mov_mode = move_mode::run;
                x.moving = 1;
                return;
            }
            auto& b = bs(state, c);
            float const g = w.physical_object_get_g(body(state, c));
            vec2 const main = pos(state, c, 0), main_last = last_pos(state, c, 0);
            vec2 const main_vel = w.body_chunk_get_vel(chunk(state, c, 0));
            ivec2 const main_tile = room_view::get_tile_position(main);
            // a pole on the way down: it grabs it
            if (x.drop && b.commited_to_move.valid) {
                vec2 const m = room_view::middle_of_tile(b.commited_to_move.dest);
                if (main.y <= m.y && main_last.y > m.y && acc(room, b.commited_to_move.dest) == pathing::access::climb) {
                    catch_pole(state, c, m);
                    x.commit_to_move_counter = 0;
                }
            }
            bool const commit_drop = b.commited_to_move.is_drop();
            if (x.commit_to_move_counter < 1 && (!commit_drop || main_tile.y < b.commited_to_move.dest.y) && !b.has_swing_pos && main_vel.y < -5.f && main_last.y > main.y) {
                for (int32_t y = room_view::get_tile_position(main_last).y; y >= main_tile.y; --y)
                    if (acc(room, ivec2{main_tile.x, y}) == pathing::access::climb) {
                        catch_pole(state, c, room_view::middle_of_tile(ivec2{main_tile.x, y}));
                        break;
                    }
            }
            float const ls = little_stuck(state, c), rs = really_stuck(state, c), inj = injured(state, c);
            if (x.mov_mode == move_mode::run || x.mov_mode == move_mode::stand_still)
                weighted_push(state, c, 0, 1, vec2{0.f, 1.f},
                    custom::lerp_map(rw::dot(rw::normalized(pos(state, c, 0) - pos(state, c, 1)), vec2{0.f, 1.f}), -1.f, 1.f, 5.5f, 0.3f) * (1.f - ls) * mathf::lerp(1.f, 0.1f, inj * inj));
            vec2 const hld = head_look_dir(state, c);
            weighted_push(state, c, 0, 1, hld, 0.05f * (1.f - ls));
            {
                // the head goes where it looks (bodyChunkConnections[1].distance 22)
                bool const pointing = false;
                vec2 const a = pos(state, c, 0) + hld * 22.f * (pointing ? 0.4f : 1.f);
                float const num = 5.f;
                weighted_push(state, c, 2, 0, rw::clamp_magnitude(a - pos(state, c, 2), num), 0.8f / num);
            }
            auto const f = pathing::field_to(state, room.id, w.creature_get_destination(c), movement);
            get_unstuck_routine(state, c, room, f);

            movement_connection conn;
            int32_t follow_chunk = -1;
            b.occupy_tile = ivec2{-1, -1};
            bool narrow = false;
            if (x.mov_mode != move_mode::climb) {
                b.has_swing_pos = 0;
                b.swing_climb_counter = 0;
            }
            if (x.commit_to_move_counter > 0) {
                --x.commit_to_move_counter;
                if (!x.drop) {
                    ivec2 const ft = room_view::get_tile_position(pos(state, c, uint32_t(b.commited_move_follow_chunk)));
                    if (x.commit_to_move_counter > 0 && ft != b.commited_to_move.dest) {
                        b.occupy_tile = b.commited_to_move.start;
                        conn = b.commited_to_move;
                        follow_chunk = b.commited_move_follow_chunk;
                    } else {
                        x.commit_to_move_counter = -5;
                    }
                }
            } else {
                if (x.commit_to_move_counter < 0)
                    ++x.commit_to_move_counter;
                if (b.has_swing_pos) {
                    b.occupy_tile = room_view::get_tile_position(b.swing_pos);
                    conn = follow_path(state, room, f, b.occupy_tile);
                    follow_chunk = 0;
                }
                for (uint32_t i = 0; i < 2 && follow_chunk < 0; ++i)
                    for (uint32_t k = 0; k < 5 && follow_chunk < 0; ++k) {
                        float const rad = w.body_chunk_get_rad(chunk(state, c, i));
                        vec2 const p = pos(state, c, i) + rw::to_vec2(four_directions_and_zero[k]) * rad;
                        if (accessible(room, p)) {
                            b.occupy_tile = room_view::get_tile_position(p);
                            conn = follow_path(state, room, f, b.occupy_tile);
                            follow_chunk = int32_t(i);
                        }
                    }
                if (physics::object_submersion(state, room, body(state, c)) > 0.f && (b.occupy_tile.x < 0 || swim_tile(room, b.occupy_tile) || swim_tile(room, main_tile))) {
                    x.mov_mode = move_mode::swim;
                    b.move_mode_change_counter = 0;
                }
                b.connection_count = 0;
            }
            ivec2 const dest = w.creature_get_destination(c);
            auto const& a = ai(state, c);
            x.moving = float_dist(main_tile, dest) >= 3.f || float_dist(b.occupy_tile, dest) >= 3.f;
            if (!x.moving && pathing::distance(state, f, main_tile) == pathing::unreachable)
                x.moving = 1;
            if (x.moving && float_dist(b.occupy_tile, dest) < 3.f && a.agitation < 0.5f)
                x.moving = 0;
            if (a.scared > 0.8f)
                x.moving = 1;
            if (x.drop) {
                if (conn.valid) {
                    ++b.footing_counter;
                    if (b.footing_counter > 10 && x.commit_to_move_counter == 0)
                        x.drop = 0;
                } else {
                    b.footing_counter = 0;
                    if (main_tile.y < b.commited_to_move.dest.y + 2)
                        x.commit_to_move_counter = 0;
                }
                conn = movement_connection{};
                follow_chunk = -1;
                b.occupy_tile = ivec2{-1, -1};
            }
            if (rs > 0.f)
                for (uint32_t j = 0; j < 3; ++j) {
                    vec2 const r = custom::rnv(rng);
                    vel(state, c, j) += r * rng.value() * 3.f * mathf::pow(rs, 3.f);
                }
            if (conn.valid && follow_chunk >= 0) {
                uint32_t const fc = uint32_t(follow_chunk);
                if (x.commit_to_move_counter < 1) {
                    movement_connection m = conn;
                    x.climbing_up_coming = 0;
                    for (int32_t k = 0; k < body_state::max_connections && m.valid; ++k) {
                        ivec2 const d = m.dest;
                        if (!narrow && ((solid(room, d + ivec2{0, 1}) && solid(room, d + ivec2{0, -1})) || (solid(room, d + ivec2{1, 0}) && solid(room, d + ivec2{-1, 0}))))
                            narrow = true;
                        if (!x.climbing_up_coming && x.moving && acc(room, d) == pathing::access::climb)
                            x.climbing_up_coming = 1;
                        b.connections[b.connection_count++] = m;
                        if (m.dest == m.start)
                            break;
                        m = follow_path(state, room, f, m.dest);
                        for (uint8_t q = 0; q < b.connection_count && m.valid; ++q)
                            if (b.connections[q].dest == m.dest)
                                m.valid = false;
                    }
                }
                if (b.connection_count == 0)
                    b.connections[b.connection_count++] = conn;
                if (x.commit_to_move_counter < 1)
                    for (uint32_t l = 0; l < 3; ++l)
                        vel(state, c, l) += custom::dir_vec(pos(state, c, l), room_view::middle_of_tile(conn.dest)) * ls * (1.f - rs);
                auto const& last_conn = b.connections[b.connection_count - 1];
                // charging a throw the other way: it stops
                if (a.anim == animation::throw_charge && charging(state, c)) {
                    vec2 const ut = charge_use_target(state, c);
                    if ((room_view::middle_of_tile(last_conn.dest).x < main.x) != (ut.x < main.x)
                        || (conn.dest.x != conn.start.x && (conn.dest.x < conn.start.x) != (ut.x < main.x)))
                        x.moving = 0;
                }
                // (the bodyChunkConnections[0] type: Pull crawling, Normal else)
                move_mode want = narrow ? move_mode::crawl : move_mode::run;
                if (acc(room, b.occupy_tile) == pathing::access::climb && acc(room, conn.dest) == pathing::access::climb)
                    want = move_mode::climb;
                if (want == move_mode::run && !x.moving)
                    want = move_mode::stand_still;
                if (x.mov_mode == move_mode::swim) {
                    if (!swim_tile(room, conn.dest)) {
                        b.move_mode_change_counter = 10;
                    } else {
                        for (uint8_t q = 0; q < std::min<uint8_t>(5, b.connection_count); ++q)
                            if (custom::dist_less(main, room_view::middle_of_tile(b.connections[q].dest), 50.f) && !swim_tile(room, b.connections[q].dest)) {
                                b.move_mode_change_counter = 10;
                                break;
                            }
                    }
                }
                if (want != x.mov_mode) {
                    if (++b.move_mode_change_counter > 10)
                        x.mov_mode = want;
                } else {
                    b.move_mode_change_counter = 0;
                }
                // standing: gravity cancelled, the chunks slowed
                if (!b.has_swing_pos && x.mov_mode != move_mode::swim) {
                    for (uint32_t n = 0; n < 3; ++n)
                        vel(state, c, n).y += g;
                    vel(state, c, 0) *= 0.9f;
                    vel(state, c, 1) *= 0.8f;
                    vel(state, c, 2) *= 0.8f;
                } else {
                    vel(state, c, 2) *= 0.8f;
                    vel(state, c, 2).y += g;
                }
                if (rs < 0.5f && (x.mov_mode == move_mode::run || x.mov_mode == move_mode::stand_still) && acc(room, conn.start) == pathing::access::floor
                    && (conn.start.y == conn.dest.y || x.mov_mode == move_mode::stand_still)) {
                    vec2& v1 = vel(state, c, 1);
                    v1.y *= 0.5f;
                    v1.y += (room_view::middle_of_tile(b.occupy_tile).y - pos(state, c, 1).y) * 0.1f;
                }
                if (conn.is_drop()) {
                    b.commited_to_move = conn;
                    x.commit_to_move_counter = 20;
                    b.commited_move_follow_chunk = int32_t(fc);
                    x.drop = 1;
                    b.has_swing_pos = 0;
                    b.has_next_swing_pos = 0;
                } else if (x.commit_to_move_counter == 0 && conn.type != movement_connection::standard) {
                    b.commited_to_move = conn;
                    x.commit_to_move_counter = 20;
                    b.commited_move_follow_chunk = int32_t(fc);
                }
                if (conn.type != movement_connection::standard) {
                    ivec2 const t1 = room_view::get_tile_position(pos(state, c, 1));
                    ivec2 const back = conn.dest + custom::clamp_at_one(conn.start - conn.dest);
                    if (t1 != conn.dest && float_dist(t1, back) < 3.f)
                        vel(state, c, 1) += custom::dir_vec(pos(state, c, 1), room_view::middle_of_tile(back));
                }
                float const speed = movement_speed(state, c);
                switch (x.mov_mode) {
                case move_mode::run: {
                    bool go_through = false;
                    for (uint32_t n = 0; n < 2; ++n) {
                        vec2 v = custom::dir_vec(room_view::middle_of_tile(conn.start), room_view::middle_of_tile(conn.dest));
                        bool found = false;
                        ivec2 const ct = room_view::get_tile_position(pos(state, c, n));
                        if (n == 0)
                            for (int32_t q = int32_t(b.connection_count) - 1; q >= 0 && !found; --q) {
                                ivec2 const up = b.connections[q].dest + ivec2{0, 1};
                                if (float_dist(ct, up) < 5.f && room.visual_contact(pos(state, c, n), room_view::middle_of_tile(up))) {
                                    v = rw::clamp_magnitude(room_view::middle_of_tile(up) - pos(state, c, n), 5.f) / 5.f;
                                    found = true;
                                }
                            }
                        for (int32_t q = int32_t(b.connection_count) - 1; q >= 0 && !found; --q) {
                            vec2 const m = room_view::middle_of_tile(b.connections[q].dest);
                            if (float_dist(ct, b.connections[q].dest) < 5.f && room.visual_contact(pos(state, c, 0), m) && room.visual_contact(pos(state, c, 1), m)) {
                                v = rw::clamp_magnitude(m - pos(state, c, n), 5.f) / 5.f;
                                found = true;
                            }
                        }
                        go_through = go_through || v.y < 0.f;
                        if ((v.y < -0.35f && n == 0) || (v.y > 0.35f && n == 1))
                            v.y *= 0.5f;
                        vel(state, c, n) += v * 0.8f * speed;
                    }
                    physics::set_go_through_floors(state, body(state, c), go_through);
                    if (b.has_knuckle_pos) {
                        auto const k0 = chunk(state, c, 0), k1 = chunk(state, c, 1);
                        ivec2 const cp0 = w.body_chunk_get_contact_point(k0), cp1 = w.body_chunk_get_contact_point(k1);
                        if (cp0.x == 0 && cp0.y == 0 && cp1.x == 0 && cp1.y > -1) {
                            float const t = mathf::inverse_lerp(40.f, 10.f, (b.knuckle_pos.x - pos(state, c, 0).x) * mathf::sign(x.flip));
                            weighted_push(state, c, 1, 0, vec2{x.flip * mathf::lerp(-1.f, 1.f, t), 0.f},
                                custom::lerp_map((w.body_chunk_get_vel(k0).x + w.body_chunk_get_vel(k1).x) / 2.f * mathf::sign(x.flip), 0.f, 5.f, 0.5f, 3.5f));
                        }
                    }
                    break;
                }
                case move_mode::crawl: {
                    vec2 v = custom::dir_vec(pos(state, c, fc), room_view::middle_of_tile(conn.dest));
                    if (ls < 0.5f) {
                        ivec2 const ct = room_view::get_tile_position(pos(state, c, fc));
                        for (int32_t q = int32_t(b.connection_count) - 1; q >= 0; --q) {
                            vec2 const m = room_view::middle_of_tile(b.connections[q].dest);
                            if (float_dist(ct, b.connections[q].dest) < 2.f && room.visual_contact(pos(state, c, fc), m)) {
                                v = rw::clamp_magnitude(m - pos(state, c, fc), 5.f) / 5.f;
                                break;
                            }
                        }
                    }
                    vel(state, c, fc) += v * 1.5f * speed;
                    physics::set_go_through_floors(state, body(state, c), v.y < 0.f);
                    b.knuckle_pos = room_view::middle_of_tile(b.connections[std::min<int32_t>(b.connection_count - 1, 3)].dest);
                    b.has_knuckle_pos = 1;
                    x.flip = mathf::lerp(x.flip, mathf::clamp(float(last_conn.dest.x - conn.start.x), -0.5f, 0.5f), 0.1f);
                    break;
                }
                case move_mode::climb: {
                    if (b.swinging_forbidden < 1 && tile_legal_for_swinging(state, c, room, b.occupy_tile) && tile_legal_for_swinging(state, c, room, last_conn.dest)
                        && conn.start.y == conn.dest.y && std::abs(b.occupy_tile.y - last_conn.dest.y) < 2)
                        b.swing_climb_counter = std::min(b.swing_climb_counter + 1, 40);
                    else
                        b.swing_climb_counter = std::max(b.swing_climb_counter - 1, 0);
                    if (b.has_swing_pos) {
                        if (rs > 0.1f)
                            b.swinging_forbidden = 200;
                        if (b.swing_climb_counter > 10) {
                            swing(state, c, room);
                        } else {
                            b.swing_radius = mathf::max(b.swing_radius - 4.f, 10.f);
                            vel(state, c, 0) *= 0.8f;
                            vel(state, c, 0) += custom::dir_vec(pos(state, c, 0), b.swing_pos) * 1.4f;
                            if (custom::dist_less(pos(state, c, 0), b.swing_pos, 5.f) || acc(room, pos(state, c, 0)) == pathing::access::climb) {
                                b.has_swing_pos = 0;
                                b.has_next_swing_pos = 0;
                                b.swing_climb_counter = 0;
                            }
                        }
                    } else if (b.swing_climb_counter < 30) {
                        tight_climbing(state, c, conn, narrow);
                    } else {
                        b.swing_pos = room_view::middle_of_tile(b.occupy_tile);
                        b.has_swing_pos = 1;
                        b.swing_radius = 50.f;
                        b.swing_climb_counter = 40;
                    }
                    if (b.swinging_forbidden > 0)
                        --b.swinging_forbidden;
                    break;
                }
                case move_mode::stand_still:
                    if (float_dist(b.occupy_tile, dest) < 5.f) {
                        vec2 const dm = room_view::middle_of_tile(dest);
                        if (acc(room, b.occupy_tile) == pathing::access::climb && acc(room, pos(state, c, 1)) != pathing::access::floor)
                            vel(state, c, 0) += rw::clamp_magnitude(dm - pos(state, c, 0), 10.f) / 10.f;
                        else
                            vel(state, c, 1) += rw::clamp_magnitude(dm - pos(state, c, 1), 10.f) / 10.f;
                    }
                    break;
                case move_mode::swim:
                    if (x.moving) {
                        vec2 v3 = room_view::middle_of_tile(conn.dest);
                        vec2& mv = vel(state, c, 0);
                        if (std::fabs(mv.x) > 5.f)
                            mv.x *= 0.8f;
                        if (swim_tile(room, conn.dest)) {
                            v3.y = room.float_water_level;
                            mv += custom::dir_vec(pos(state, c, 0), v3) * 0.25f;
                        } else {
                            mv += custom::dir_vec(pos(state, c, 0), v3) * 1.25f;
                        }
                        if (std::fabs(pos(state, c, 0).x - v3.x) < 5.f)
                            x.flip *= 0.9f;
                        else if (pos(state, c, 0).x < v3.x)
                            x.flip = mathf::min(x.flip + 0.07f, 1.f);
                        else
                            x.flip = mathf::max(x.flip - 0.07f, -1.f);
                        vel(state, c, 2) *= 0.9f;
                        vel(state, c, 2) += rw::lerp(custom::dir_vec(pos(state, c, 0), room_view::middle_of_tile(last_conn.dest)), vec2{0.f, 1.f},
                                                physics::submersion(state, room, chunk(state, c, 2))) * 2.f;
                    } else {
                        vel(state, c, 0) += rw::clamp_magnitude(room_view::middle_of_tile(dest) - pos(state, c, 0), 10.f) / 60.f;
                        x.flip = mathf::lerp(x.flip, mathf::clamp(hld.x * 1.5f, -1.f, 1.f), 0.1f);
                    }
                    break;
                }
                if (rs < 0.5f && x.moving && x.mov_mode != move_mode::stand_still && x.mov_mode != move_mode::swim) {
                    vec2 v4 = custom::dir_vec(room_view::middle_of_tile(conn.start), room_view::middle_of_tile(conn.dest + ivec2{0, 1}));
                    for (int32_t q = int32_t(b.connection_count) - 1; q >= 0; --q) {
                        float const d = float_dist(b.connections[q].dest, b.occupy_tile);
                        if (d > 3.f && d < 7.f) {
                            v4 = custom::dir_vec(pos(state, c, 2), room_view::middle_of_tile(b.connections[q].dest + ivec2{0, 1}));
                            break;
                        }
                    }
                    vel(state, c, 2) += v4 * custom::lerp_map(rw::dot(v4, custom::dir_vec(pos(state, c, 0), pos(state, c, 2))), 1.f, -1.f, 1.f, 4.f, 1.5f);
                    for (uint32_t n = 0; n < 3; ++n) {
                        ivec2 const cp = w.body_chunk_get_contact_point(chunk(state, c, n));
                        vec2& cv = vel(state, c, n);
                        if (conn.start.x != conn.dest.x && cp.x == custom::int_clamp(conn.dest.x - conn.start.x, -1, 1))
                            cv.y += mathf::clamp(room_view::middle_of_tile(conn.start).y - pos(state, c, n).y, -5.f, 5.f) * 0.1f;
                        else if (conn.start.y != conn.dest.y && cp.y == custom::int_clamp(conn.dest.y - conn.start.y, -1, 1))
                            cv.x += mathf::clamp(room_view::middle_of_tile(conn.start).x - pos(state, c, n).x, -5.f, 5.f) * 0.1f;
                    }
                }
            } else if (x.drop) {
                vec2& mv = vel(state, c, 0);
                if (std::fabs(mv.x) < 5.f) {
                    mv.x *= 0.8f;
                    mv.x += mathf::clamp((room_view::middle_of_tile(b.commited_to_move.dest).x - pos(state, c, 0).x) / 3.f, -4.f, 4.f);
                }
                ivec2 const t1 = room_view::get_tile_position(pos(state, c, 1));
                if (t1.y >= b.commited_to_move.start.y && std::abs(t1.x - b.commited_to_move.start.x) == 1)
                    vel(state, c, 1).x += mathf::clamp((room_view::middle_of_tile(b.commited_to_move.start).x - pos(state, c, 1).x) / 3.f, -4.f, 4.f);
            } else if (x.mov_mode == move_mode::swim) {
                // in the water without a way: to the surface, towards the goal
                vel(state, c, 0) += rw::clamp_magnitude(room_view::middle_of_tile(dest) - pos(state, c, 0), 10.f) / 60.f;
            }
            // the flip and the knuckles
            switch (x.mov_mode) {
            case move_mode::run:
                if (b.connection_count > 2) {
                    if (room_view::middle_of_tile(b.connections[b.connection_count - 1].dest).x > pos(state, c, 0).x)
                        x.flip = mathf::min(mathf::lerp(x.flip, 1.f, 0.1f) + 0.05f, 1.f);
                    else
                        x.flip = mathf::max(mathf::lerp(x.flip, -1.f, 0.1f) - 0.05f, -1.f);
                } else {
                    x.flip *= 0.8f;
                }
                if (!b.has_knuckle_pos) {
                    for (int32_t q = std::min<int32_t>(4, int32_t(b.connection_count) - 1); q >= 0 && !b.has_knuckle_pos; --q) {
                        vec2 v5 = room_view::middle_of_tile(b.connections[q].dest + ivec2{0, -1});
                        v5 += custom::dir_vec(pos(state, c, 0), v5) * 40.f;
                        auto const t = exact_terrain_ray_trace_pos(room, pos(state, c, 0), v5);
                        if (knuckle_pos_legal(state, c, t)) {
                            // (Scavenger_Knuckle_Hit_Ground and the rest: no sound ids)
                            b.knuckle_pos = *t;
                            b.has_knuckle_pos = 1;
                        }
                    }
                } else if (!knuckle_pos_legal(state, c, b.knuckle_pos)) {
                    b.has_knuckle_pos = 0;
                }
                break;
            case move_mode::climb:
                if (x.drop)
                    b.has_swing_pos = 0;
                break;
            case move_mode::stand_still:
                x.flip = mathf::lerp(x.flip, mathf::clamp(hld.x * 10.f, -1.f, 1.f), 0.1f);
                break;
            case move_mode::swim: {
                float const level = room.float_water_level;
                vel(state, c, 1).y -= g * (1.f - physics::submersion(state, room, chunk(state, c, 2)));
                vel(state, c, 2) *= mathf::lerp(1.f, 0.9f, physics::submersion(state, room, chunk(state, c, 0)));
                vel(state, c, 0).y *= mathf::lerp(1.f, 0.95f, physics::submersion(state, room, chunk(state, c, 0)));
                vel(state, c, 1).y *= mathf::lerp(1.f, 0.95f, physics::submersion(state, room, chunk(state, c, 1)));
                vel(state, c, 0).y += mathf::clamp((level - pos(state, c, 0).y) / 14.f, -0.5f, 0.5f);
                vel(state, c, 1).y += mathf::clamp((level - 18.f - pos(state, c, 1).y) / 14.f, -0.5f, 0.5f);
                // out of the water: back on its feet
                if (physics::object_submersion(state, room, body(state, c)) <= 0.f && b.occupy_tile.x >= 0)
                    x.mov_mode = move_mode::run;
                break;
            }
            case move_mode::crawl:
                break;
            }
            if (x.mov_mode != move_mode::run && x.mov_mode != move_mode::crawl)
                b.has_knuckle_pos = 0;
            // stuck: the past positions all close by
            if (b.not_following_path_counter < 200 && pathing::distance(state, f, main_tile) == pathing::unreachable)
                ++b.not_following_path_counter;
            else if (b.not_following_path_counter > 0)
                --b.not_following_path_counter;
            int32_t close = 0;
            if (x.moving || b.not_following_path_counter > 100) {
                b.past_head = (b.past_head + body_state::max_past - 1) % body_state::max_past;
                b.past[b.past_head] = main_tile;
                b.past_count = std::min(b.past_count + 1, body_state::max_past);
                for (int32_t q = 30; q < b.past_count; ++q)
                    if (float_dist(main_tile, b.past[(b.past_head + q) % body_state::max_past]) < 4.f)
                        ++close;
            }
            x.stuck_counter = custom::int_clamp(close > 20 ? x.stuck_counter + 1 : x.stuck_counter - 4, 0, 200);
        }

        // Scavenger.Stun / HeavyStun
        void stun(sys::game_state& state, dcon::creature_id c, int32_t st) {
            auto& w = state.world;
            if (w.creature_get_random(c).value() < 0.5f && st > 15)
                release_grasp(state, c, 0);
            w.creature_set_stun(c, std::max(w.creature_get_stun(c), st));
        }

        // a slugcat hurt `c`: it and the scavengers of its room turn on the
        // slugcats (the community's like, simplified)
        void slugcat_attacked(sys::game_state& state, dcon::creature_id c, dcon::room_id room) {
            rooms::for_each_creature(state, room, ::creature::type::scavenger, [&](dcon::creature_id o) {
                if (state.world.creature_get_dead(o))
                    return;
                auto& a = ai(state, o);
                a.like = mathf::max(-1.f, mathf::min(a.like, 0.f) - (o == c ? 0.5f : 0.25f));
            });
        }
    }

    //
    // What the graphics module reads
    //
    vec2 look_point(sys::game_state const& state, dcon::creature_id c) {
        if (!consious(state, c))
            return pos(state, c, 0);
        auto const& a = ai(state, c);
        if (auto const* f = tracker::find(tracker::of(state, c), a.focus); f && f->visual && creature::live(state, f->who))
            return creature::main_pos(state, f->who);
        if (a.anim == animation::throw_charge)
            return charge_use_target(state, c);
        return bs(state, c).look_point;
    }

    vec2 head_look_dir(sys::game_state const& state, dcon::creature_id c) {
        return rw::clamp_magnitude(look_point(state, c) - pos(state, c, 0), 300.f) / 300.f;
    }

    float act_nervous(sys::game_state const& state, dcon::creature_id c) {
        auto const& a = ai(state, c);
        return mathf::lerp(pers(state, c).nervous, a.agitation, a.agitation * 0.5f);
    }

    dcon::item_id held_item(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
        return grasp_item(state, c, i);
    }

    variant variant_of(sys::game_state const& state, dcon::creature_id c) {
        return pl(state, c).variant;
    }

    bool karmic_armor_protected(sys::game_state const& state, dcon::creature_id c) {
        return templar(state, c) && ai(state, c).karma_levels > 0;
    }

    // SharedPhysics.ExactTerrainRayTracePos: where the segment from `a` to
    // `b` enters the first solid tile on it
    std::optional<vec2> exact_terrain_ray_trace_pos(room_view const& room, vec2 a, vec2 b) {
        // RayTracedTilesArray's walk to the first solid tile
        float const ax = a.x / 20.f, ay = a.y / 20.f, bx = b.x / 20.f, by = b.y / 20.f;
        float const dx = std::fabs(bx - ax), dy = std::fabs(by - ay);
        int32_t x = int32_t(std::floor(ax)), y = int32_t(std::floor(ay));
        float const dt_dx = 1.f / dx, dt_dy = 1.f / dy;
        int32_t n = 1, x_inc = 0, y_inc = 0;
        float t_next_h, t_next_v;
        if (dx == 0.f) {
            t_next_h = dt_dx;
        } else if (bx > ax) {
            x_inc = 1;
            n += int32_t(std::floor(bx)) - x;
            t_next_h = (std::floor(ax) + 1.f - ax) * dt_dx;
        } else {
            x_inc = -1;
            n += x - int32_t(std::floor(bx));
            t_next_h = (ax - std::floor(ax)) * dt_dx;
        }
        if (dy == 0.f) {
            t_next_v = dt_dy;
        } else if (by > ay) {
            y_inc = 1;
            n += int32_t(std::floor(by)) - y;
            t_next_v = (std::floor(ay) + 1.f - ay) * dt_dy;
        } else {
            y_inc = -1;
            n += y - int32_t(std::floor(by));
            t_next_v = (ay - std::floor(ay)) * dt_dy;
        }
        n = std::min(n, 4096);
        std::optional<ivec2> hit;
        for (; n > 0; --n) {
            if (room.get_tile(ivec2{x, y}).solid()) {
                hit = ivec2{x, y};
                break;
            }
            if (t_next_v < t_next_h) {
                y += y_inc;
                t_next_v += dt_dy;
            } else {
                x += x_inc;
                t_next_h += dt_dx;
            }
        }
        if (!hit)
            return std::nullopt;
        // Custom.RectCollision of the segment with the tile
        vec2 const m = room_view::middle_of_tile(*hit);
        float const left = m.x - 10.f, bottom = m.y - 10.f, right = m.x + 10.f, top = m.y + 10.f;
        auto const vcross = [](vec2 p, vec2 q, float xx) {
            if (p.x == q.x)
                return vec2{xx, std::numeric_limits<float>::quiet_NaN()};
            float const k = (p.y - q.y) / (p.x - q.x);
            return vec2{xx, p.y - p.x * k + k * xx};
        };
        auto const hcross = [](vec2 p, vec2 q, float yy) {
            if (p.y == q.y)
                return vec2{std::numeric_limits<float>::quiet_NaN(), yy};
            if (p.x == q.x)
                return vec2{p.x, yy};
            float const k = (p.y - q.y) / (p.x - q.x);
            return vec2{(yy - (p.y - p.x * k)) / k, yy};
        };
        vec2 const v1 = vcross(a, b, left), v2 = vcross(a, b, right);
        vec2 v3 = hcross(a, b, bottom), v4 = hcross(a, b, top);
        if (a.x == b.x) {
            v3 = vec2{a.x, bottom};
            v4 = vec2{a.x, top};
        }
        bool const inside = a.x > left && a.x < right && a.y > bottom && a.y < top;
        float dst = inside ? float_max : rw::distance(a, b);
        vec2 out = b;
        auto const take = [&](vec2 v) {
            out = v;
            dst = rw::distance(a, v);
        };
        if (v1.y >= bottom && v1.y <= top && custom::dist_less(a, v1, dst))
            take(v1);
        if (v2.y >= bottom && v2.y <= top && custom::dist_less(a, v2, dst))
            take(v2);
        if (v3.x >= left && v3.x <= right && custom::dist_less(a, v3, dst))
            take(v3);
        if (v4.x >= left && v4.x <= right && custom::dist_less(a, v4, dst))
            take(v4);
        return out;
    }

    //
    // The creature
    //
    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 at, variant v) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::scavenger);
        auto const d = data(state, c);
        // (first: everything below reads the template)
        pl(state, c).variant = uint32_t(v) < variant_count ? v : variant::scavenger;
        rw::rng rng{state.random.next() | 1u};
        int32_t seed = int32_t(rng.next() & 0x7FFFFFFFu);
        if (state.spawn_seed >= 0)
            seed = state.spawn_seed;
        auto const view = world::get_room_view(state, room);
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
        desc.surface_friction = 0.4f;
        desc.water_friction = 0.96f;
        desc.buoyancy = 0.95f;
        auto const o = physics::create_object(state, room, desc);
        auto const c0 = physics::add_chunk(state, o, at, 9.5f, 0.5f);
        auto const c1 = physics::add_chunk(state, o, at + vec2{0.f, -1.f}, 7.f, 0.3f);
        auto const c2 = physics::add_chunk(state, o, at + vec2{0.f, 1.f}, 5.f, 0.05f);
        physics::add_connection(state, o, c0, c1, 18.f, physics::connection_type::normal, 1.f, -1.f);
        physics::add_connection(state, o, c0, c2, 22.f, physics::connection_type::pull, 0.8f, -1.f);
        w.try_create_creature_body(c, o);
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_set_grasps(c, creature::grasp_pair{});
        w.creature_set_behavior(c, uint8_t(behavior::idle));
        w.creature_set_current_utility(c, 0.f);
        w.creature_set_destination(c, room_view::get_tile_position(at));
        tracker::clear(state, c);
        w.scavenger_data_set_seed(d, seed);
        // AbstractCreature.Personality (its own InitState(seed))
        personality p;
        {
            rw::unity_random r{seed};
            p.sympathy = r.value();
            p.energy = r.value();
            p.bravery = r.value();
            p.sympathy = push_from_half(p.sympathy, 1.5f);
            p.energy = push_from_half(p.energy, 1.5f);
            p.bravery = push_from_half(p.bravery, 1.5f);
            float const n0 = r.value();
            float const n1 = mathf::pow(r.value(), 0.25f);
            p.nervous = mathf::lerp(n0, mathf::lerp(p.energy, 1.f - p.bravery, 0.5f), n1);
            float const a0 = r.value();
            float const a1 = mathf::pow(r.value(), 0.25f);
            p.aggression = mathf::lerp(a0, (p.energy + p.bravery) / 2.f * (1.f - p.sympathy), a1);
            float const d0 = r.value();
            float const d1 = mathf::pow(r.value(), 0.25f);
            p.dominance = mathf::lerp(d0, (p.energy + p.bravery + p.aggression) / 3.f, d1);
            p.nervous = push_from_half(p.nervous, 2.5f);
            p.aggression = push_from_half(p.aggression, 2.5f);
        }
        w.scavenger_data_set_personality(d, p);
        // Scavenger.SetUpCombatSkills (InitState(seed); DLCShared's
        // dominance scaling of a plain scavenger included)
        combat_skills sk;
        {
            rw::unity_random r{seed};
            auto const roll = [&](float a) {
                float const v0 = r.value();
                float const v1 = r.value();
                return mathf::lerp(a, v0, v1);
            };
            {
                bool const pick = r.value() < 0.5f;
                float const base = roll(pick ? p.nervous : p.aggression);
                sk.dodge = push_from_half(base, 1.f + r.value());
            }
            {
                bool const pick = r.value() < 0.5f;
                float const base = roll(pick ? p.energy : p.aggression);
                sk.mid_range = push_from_half(base, 1.f + r.value());
            }
            {
                float const base = r.value();
                sk.melee = push_from_half(base, 1.f + r.value());
            }
            {
                bool const pick = r.value() < 0.5f;
                float const base = mathf::inverse_lerp(0.35f, 1.f, roll(pick ? p.bravery : p.energy));
                sk.blocking = push_from_half(base, 1.f + r.value());
            }
            {
                float const base = roll(p.energy);
                sk.reaction = push_from_half(base, 1.f + r.value());
            }
            if (v == variant::elite) {
                float const num = mathf::lerp(p.dominance, 1.f, 0.15f);
                sk.dodge = mathf::lerp(sk.dodge, 1.f, num * 0.15f);
                sk.mid_range = mathf::lerp(sk.mid_range, 1.f, num * 0.1f);
                sk.blocking = mathf::lerp(sk.blocking, 1.f, num * 0.1f);
                sk.reaction = mathf::lerp(sk.reaction, 1.f, num * 0.05f);
            } else {
                // (the Templar's too: it isn't Elite)
                float const num2 = 1.f - p.dominance;
                sk.dodge = mathf::lerp(sk.dodge, 0.f, num2 * 0.085f);
                sk.mid_range = mathf::lerp(sk.mid_range, 0.f, num2 * 0.085f);
                sk.blocking = mathf::lerp(sk.blocking, 0.f, num2 * 0.05f);
                sk.reaction = mathf::lerp(sk.reaction, 0.f, num2 * 0.15f);
            }
            if (v == variant::disciple)
                sk.melee = 1.f;
        }
        w.scavenger_data_set_skills(d, sk);
        body_state b;
        b.climb_orientation = ivec2{rng.value() < 0.5f ? -1 : 1, 0};
        b.climb_orientation.y = rng.value() < 0.5f ? -1 : 1;
        b.last_non_solid_tile = room_view::get_tile_position(at);
        b.look_point = vec2{rng.value() * view.pixel_width(), rng.value() * view.pixel_height()};
        w.scavenger_data_set_body(d, b);
        ai_state a;
        a.test_idle_pos = ivec2{rng.range(0, view.width), rng.range(0, view.height)};
        a.test_throw_pos = ivec2{rng.range(0, view.width), rng.range(0, view.height)};
        a.test_look_pos = b.look_point;
        a.already_looked_at_pos = b.look_point;
        a.idle_counter = rng.range(100, 400);
        // karmaLevels: the Disciple's 5 to 7
        if (v == variant::disciple)
            a.karma_levels = rng.range(5, 8);
        w.scavenger_data_set_ai(d, a);
        w.creature_set_random(c, rng);
        auto& x = pl(state, c);
        x.flip = 0.f;
        x.stuck_counter = 0;
        x.commit_to_move_counter = 0;
        x.mov_mode = move_mode::run;
        x.moving = 0;
        x.drop = 0;
        x.climbing_up_coming = 0;
        // ScavengerGraphics' constructor rolls (from the seed)
        scavenger_graphics::roll(state, c);
        scavenger_graphics::reset_positions(state, c);
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        creature::release_target(state, c);
        for (uint32_t i = 0; i < 2; ++i)
            creature::release_grasp(state, c, i);
        creature::erase(state, c);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto& rng = w.creature_get_random(c);
        auto const o = body(state, c);
        // Creature.Update's HealthState: dying of wounds, healing
        if (!w.creature_get_dead(c)) {
            float& health = w.creature_get_health(c);
            if (health < 0.f && rng.value() < -health && rng.value() < 1.f / float(consious(state, c) ? 80 : 800))
                creature::die(state, c);
            if (health > 0.f && health < 1.f && rng.value() < 0.01f)
                health = mathf::min(1.f, health + 1.f / mathf::lerp(550.f, 70.f, health));
        }
        if (w.creature_get_dead(c) == 0 && rng.value() < injured(state, c)) {
            float const r = rng.value();
            stun(state, c, int32_t(mathf::lerp(0.f, 14.f * r, mathf::pow(injured(state, c), 0.5f))));
        }
        // (the Watcher's part: the Templar's karmic armour)
        if (templar(state, c))
            armor_update(state, c, room);
        weighted_push(state, c, 2, 0, custom::dir_vec(pos(state, c, 1), pos(state, c, 0)), 0.3f * (1.f - really_stuck(state, c)));
        w.body_chunk_set_collide_with_terrain(chunk(state, c, 2), room.visual_contact(pos(state, c, 0), pos(state, c, 2)));
        creature::stun_countdown(state, c);
        physics::update_object(state, room, o, nullptr, nullptr);
        auto& b = bs(state, c);
        ivec2 const mt = room_view::get_tile_position(pos(state, c, 0));
        if (!room.get_tile(mt).solid())
            b.last_non_solid_tile = mt;
        else
            physics::hard_set_position(state, chunk(state, c, 0), room_view::middle_of_tile(b.last_non_solid_tile));
        if (pl(state, c).variant != variant::scavenger)
            jump_logic_update(state, c, room);
        if (templar(state, c))
            watch_throw(state, c, room);
        if (consious(state, c)) {
            act(state, c, room);
        } else {
            stop_animation(state, c);
            ai(state, c).melee_target = dcon::creature_id{};
        }
        if (b.has_swing_pos) {
            vec2& mp = pos(state, c, 0);
            vec2& mv = vel(state, c, 0);
            float const num = rw::distance(mp, b.swing_pos);
            if (num > 100.f || (num > 70.f && body_parts::rotate_around_origo(mv, -custom::aim(mp, b.swing_pos)).y < -55.f) || (!consious(state, c) && rng.value() < 0.016666668f)) {
                b.has_swing_pos = 0;
            } else if (num > b.swing_radius) {
                vec2 const a = custom::dir_vec(b.swing_pos, mp);
                mv += a * (b.swing_radius - num);
                mp += a * (b.swing_radius - num);
            }
        }
        float const squeeze = 1.f - little_stuck(state, c);
        for (uint32_t j = 0; j < 3; ++j)
            w.body_chunk_set_terrain_squeeze(chunk(state, c, j), squeeze);
        // the Templar's room objects: the ping, the shockwave
        if (templar(state, c)) {
            ping_update(state, c, room);
            shock_update(state, c, room);
        }
        // the items it holds (GraphicsModuleUpdated: the graphics module
        // takes them to the hands where a camera looks)
        if (!state.viewed(room.id))
            carry_items_unviewed(state, c);
    }

    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) {
        return true;
    }

    void collide(sys::game_state& state, dcon::creature_id c, room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j) {
        if (!consious(state, c))
            return;
        auto& a = ai(state, c);
        hook_ctx hx{state, c, room};
        tracker::see(state, c, room, t, make_hooks(hx));
        // CollideWithOtherCreature: a spear ready and the creature on the
        // side it faces: a stab
        auto const* r = tracker::find(tracker::of(state, c), t);
        auto const k = chunk_of(state, t, j);
        if (!r || !k || !is_spear(state, grasp_item(state, c, 0)))
            return;
        vec2 const cp = state.world.body_chunk_get_pos(*k), main = pos(state, c, 0);
        if ((cp.x < main.x) == (pos(state, c, 2).x < main.x) && state.world.creature_get_random(c).value() < mathf::pow(a.agitation, 0.2f) * skills(state, c).melee
            && want_to_lethally_attack(state, c, r)) {
            a.melee_target = t;
            a.melee_chunk = uint8_t(j);
        }
    }

    void violence(sys::game_state& state, dcon::creature_id c, uint32_t k, vec2 momentum, float damage, float stun_bonus, creature::damage_type type) {
        auto& w = state.world;
        auto& rng = w.creature_get_random(c);
        auto const room = creature::room_of(state, c);
        // who did it: a slugcat's weapon flying at it (the source isn't
        // passed down; the thrown item hitting is still Thrown here)
        auto const hit = chunk(state, c, std::min<uint32_t>(k, 2));
        vec2 const hp = w.body_chunk_get_pos(hit);
        // the thrown weapon that hit (its thrower): any thrower for the
        // karmic armour, a slugcat's for the like
        dcon::creature_id thrower;
        bool thrown = false;
        rooms::for_each_item(state, room, [&](dcon::item_id it) {
            if (thrown || w.item_get_mode(it) != world::weapon_mode::thrown)
                return;
            auto const by = w.item_get_thrown_by(it);
            if (!by || !w.creature_is_valid(by) || by == c)
                return;
            auto const ic = items::chunk(state, it);
            if (custom::dist_less(w.body_chunk_get_pos(ic), hp, w.body_chunk_get_rad(hit) + rw::magnitude(w.body_chunk_get_vel(ic)) + 20.f)) {
                thrown = true;
                thrower = by;
            }
        });
        if (thrown && creature::is_slugcat(w, thrower))
            slugcat_attacked(state, c, room);
        // the Templar's karmic armour (Watcher): a thrown weapon glances off
        // (karmicArmorResetTime 45), a creature's blow does nothing (15);
        // either sets the countdown going. Explosions have no source and
        // get through
        if (karmic_armor_protected(state, c) && type != creature::damage_type::explosion) {
            auto& a = ai(state, c);
            if (thrown) {
                auto const view = world::get_room_view(state, room);
                hook_ctx hx{state, c, view};
                if (creature::live(state, thrower) && creature::here(state, thrower, room))
                    tracker::see(state, c, view, thrower, make_hooks(hx));
                a.armor_reset_time = 45;
            } else {
                a.armor_reset_time = 15;
            }
            a.armor_triggered = 1;
            a.armor_timer = 0;
            // (KarmicArmor.DeflectedProjectile's sparks and Templar_Shield_Deflect: not ported)
            return;
        }
        // Scavenger.Violence: the head is the soft spot; the Elite takes
        // less stun
        bool const head = k == 2;
        if (head)
            damage *= 10.f;
        if (elite(state, c))
            stun_bonus *= 0.85f;
        float const dmg = damage * (head ? 1.3f : 1.f);
        // Creature.Violence
        w.body_chunk_get_vel(hit) += rw::clamp_magnitude(momentum / w.body_chunk_get_mass(hit), 10.f);
        if (!w.creature_get_dead(c)) {
            float& health = w.creature_get_health(c);
            float const d = dmg / base_damage_resistance(state, c);
            float st = (dmg * 30.f + stun_bonus) / base_stun_resistance;
            health -= d;
            stun(state, c, int32_t(st));
            if (rng.value() < -health || health < -1.f || (health < 0.f && rng.value() < 0.33f))
                creature::die(state, c);
            // instantDeathDamageLimit (1.5, the Elite's and Disciple's 3.5)
            if (d >= instant_death_damage_limit(state, c))
                creature::die(state, c);
            if (head && damage > 0.5f && health < 0.f)
                creature::die(state, c);
            // ShockReaction
            if (state.viewed(room))
                scavenger_graphics::shock_reaction(state, c, mathf::inverse_lerp(0.f, 1.f, dmg) + mathf::inverse_lerp(0.f, 60.f, stun_bonus) * 0.5f);
        }
        // the Elite's VultureMask comes off when it dies of a blow
        // (readyToReleaseMask); the mask item itself: see the integration
        // notes
        if (elite(state, c) && w.creature_get_dead(c) && !ai(state, c).mask_released) {
            ai(state, c).mask_released = 1;
        }
    }

    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t) {
        auto& a = ai(state, c);
        if (a.melee_target == t)
            a.melee_target = dcon::creature_id{};
        if (a.charge_target == t)
            a.charge_target = dcon::creature_id{};
        if (a.focus == t)
            a.focus = dcon::creature_id{};
        if (a.stunned_creature == t)
            a.stunned_creature = dcon::creature_id{};
        if (a.targeted == t)
            a.targeted = dcon::creature_id{};
    }

    void died(sys::game_state& state, dcon::creature_id c) {
        stop_animation(state, c);
        // (creature::die let go of the items it held)
    }

    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        // the items in its hands come along (a transaction: the rooms aren't updating)
        for (uint32_t i = 0; i < 2; ++i)
            if (auto const it = grasp_item(state, c, i)) {
                w.physical_object_set_room_from_object_location(items::body(state, it), room);
                physics::hard_set_position(state, items::chunk(state, it), at);
                items::placed(state, it);
            }
        // Scavenger.NewRoom + ScavengerAI.NewRoom
        auto& rng = w.creature_get_random(c);
        auto const view = world::get_room_view(state, room);
        auto& b = bs(state, c);
        b.past_count = 0;
        b.commited_to_move = movement_connection{};
        b.has_swing_pos = 0;
        b.has_next_swing_pos = 0;
        b.has_knuckle_pos = 0;
        b.connection_count = 0;
        b.last_non_solid_tile = room_view::get_tile_position(at);
        b.look_point = vec2{rng.value() * view.pixel_width(), rng.value() * view.pixel_height()};
        auto& x = pl(state, c);
        x.commit_to_move_counter = 0;
        x.drop = 0;
        auto& a = ai(state, c);
        a.test_idle_pos = ivec2{rng.range(0, view.width), rng.range(0, view.height)};
        a.test_throw_pos = ivec2{rng.range(0, view.width), rng.range(0, view.height)};
        a.scavenge_candidate = dcon::item_id{};
        a.focus = dcon::creature_id{};
        a.melee_target = dcon::creature_id{};
        stop_animation(state, c);
        // the jumps start over; the Templar's armour, ping and shockwave
        // stay in the old room, and its karma is rolled again
        // (Scavenger.PlaceInRoom)
        b.finder_count = 0;
        b.has_act = 0;
        b.has_jump_cell = 0;
        a.armor_alive = 0;
        a.ping_frame = -1;
        a.shock_frame = -1;
        a.watched_throw = dcon::item_id{};
        if (templar(state, c))
            a.karma_levels = rng.range(5, 8);
    }

    std::string describe(sys::game_state const& state, dcon::creature_id c) {
        auto const& w = state.world;
        static char const* const modes[] = {"run", "stand", "crawl", "climb", "swim"};
        vec2 const p = pos(state, c, 0);
        static char const* const names[] = {"scavenger", "scavenger elite", "scavenger disciple"};
        std::string s = std::string{names[uint32_t(pl(state, c).variant) % variant_count]} + " at " + std::to_string(int(p.x)) + "," + std::to_string(int(p.y)) + " behavior " + std::to_string(int(w.creature_get_behavior(c)))
            + " " + modes[uint32_t(pl(state, c).mov_mode) % 5];
        for (uint32_t i = 0; i < 2; ++i)
            if (auto const it = grasp_item(state, c, i))
                s += is_spear(state, it) ? " +spear" : " +item";
        if (ai(state, c).like < 0.f)
            s += " hostile";
        if (ai(state, c).anim == animation::throw_charge)
            s += " charging";
        if (ai(state, c).anim == animation::prepare_to_jump || ai(state, c).anim == animation::jumping)
            s += " jumping";
        if (templar(state, c))
            s += " karma " + std::to_string(ai(state, c).karma_levels) + " lust " + std::to_string(ai(state, c).blood_lust);
        return s;
    }

    bool spawn_spot(room_view const& room, ivec2 tile) {
        return !room.get_tile(tile).solid() && pathing::tile_access(room, tile) == pathing::access::floor && !room.get_tile(tile + ivec2{0, 1}).solid()
            && room.get_tile(tile).water == 0;
    }
}
