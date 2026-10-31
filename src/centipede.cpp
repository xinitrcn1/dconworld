#include <cmath>
#include <vector>

#include "centipede.hpp"
#include "dens.hpp"
#include "rooms.hpp"
#include "centipede_graphics.hpp"
#include "items.hpp"
#include "lizard_ai.hpp"
#include "particles.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "migrate.hpp"
#include "rooms.hpp"
#include "tracker.hpp"

namespace centipede {
    using rw::ivec2;
    using rw::vec2;
    using world::centipede_behavior;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;
    using creature::relationship;

    char const* to_string(variant v) noexcept {
        switch (v) {
        case variant::centipede: return "centipede";
        case variant::small: return "baby centipede";
        case variant::wing: return "centiwing";
        case variant::red: return "red centipede";
        case variant::aqua: return "aquapede";
        }
        return "centipede";
    }

    bool parse_variant(std::string_view name, variant& out) noexcept {
        if (name == "centipede" || name == "plain") out = variant::centipede;
        else if (name == "small" || name == "baby") out = variant::small;
        else if (name == "wing" || name == "centiwing") out = variant::wing;
        else if (name == "red") out = variant::red;
        else if (name == "aqua" || name == "aquapede" || name == "aquacenti") out = variant::aqua;
        else return false;
        return true;
    }

    namespace {
        // CreatureTemplate: Centipede (0.9; RedCentipede, SmallCentipede and
        // Centiwing 1, AquaCenti 0.9), its stun resistance (every one 0.6) and
        // damageRestistances[Electric] (102, the aquapede's too)
        float base_damage_resistance(variant v) noexcept {
            return v == variant::red || v == variant::small || v == variant::wing ? 1.f : 0.9f;
        }
        constexpr float base_stun_resistance = 0.6f;
        constexpr float electric_resistance = 102.f;
        float visual_radius(variant v) noexcept {
            switch (v) {
            case variant::red: return 1100.f;
            case variant::wing: return 300.f;
            case variant::aqua: return 1900.f;
            default: return 900.f;
            }
        }

        using creature::consious;
        using creature::room_of;

        vec2 pos(sys::game_state const& state, dcon::creature_id c, uint32_t i) {
            return state.world.body_chunk_get_pos(chunk(state, c, i));
        }
        // Custom.RNV(), rolled before what multiplies it
        vec2 rnv(rw::rng& rng) {
            return custom::deg_to_vec(rng.value() * 360.f);
        }

        creature::centipede_payload& pl(sys::game_state& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).centipede;
        }
        creature::centipede_payload const& pl(sys::game_state const& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).centipede;
        }
        dcon::centipede_data_id data(sys::game_state const& state, dcon::creature_id c) {
            return creature::centipede_data(state.world, c);
        }
        bool is(sys::game_state const& state, dcon::creature_id c, variant v) {
            return pl(state, c).variant == v;
        }

        dcon::creature_id grasped(sys::game_state const& state, dcon::creature_id c, int g) {
            return creature::grasped(state.world, c, uint32_t(g));
        }
        // the slugcat holding the baby in its hand
        dcon::player_id holder(sys::game_state const& state, dcon::creature_id c) {
            return creature::player_of(state, creature::holder(state, c));
        }

        // Centipede.AquacentiSwim
        bool aqua_swimming(sys::game_state const& state, dcon::creature_id c, room_view const& room) {
            return is(state, c, variant::aqua) && physics::object_submersion(state, room, body(state, c)) > 0.5f && !pl(state, c).flying;
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

        // Centipede.RatherClimbThanFly
        bool rather_climb_than_fly(sys::game_state& state, room_view const& room, ivec2 t) {
            auto const tile = room.get_tile(t);
            return tile.any_beam() || pathing::ai_terrain_proximity(state, room.id, t) < 2;
        }
        // Centipede.ClimbableTile
        bool climbable_tile(sys::game_state& state, dcon::creature_id c, room_view const& room, ivec2 t) {
            if (is(state, c, variant::wing) && !pl(state, c).flying)
                return rather_climb_than_fly(state, room, t);
            auto const tile = room.get_tile(t);
            return (tile.flags & world::tile_wall_behind) != 0 || tile.any_beam() || pathing::ai_terrain_proximity(state, room.id, t) < 2;
        }
        // Centipede.AccessibleTile: the AImap's accessibility for its
        // template, or what it can climb along the water's surface
        bool accessible(sys::game_state& state, dcon::creature_id c, room_view const& room, ivec2 t) {
            if (is(state, c, variant::wing) && !pl(state, c).flying)
                return rather_climb_than_fly(state, room, t);
            if (t.y != room.default_water_level)
                return pathing::accessible(room, t, movement(state, c));
            return climbable_tile(state, c, room, t);
        }
        bool accessible(sys::game_state& state, dcon::creature_id c, room_view const& room, vec2 p) {
            return accessible(state, c, room, room_view::get_tile_position(p));
        }

        void release(sys::game_state& state, dcon::creature_id c, int g) {
            creature::release_grasp(state, c, uint32_t(g));
        }

        void lose_all_grasps(sys::game_state& state, dcon::creature_id c) {
            release(state, c, 0);
            release(state, c, 1);
        }

        // Creature.Stun + Centipede.Stun: a centiwing always lets go, and
        // stays stunned twice as long
        void stun(sys::game_state& state, dcon::creature_id c, int32_t st) {
            auto& w = state.world;
            bool const wing = is(state, c, variant::wing);
            // (the stun's length in the key: two stuns of a tick roll apart)
            if (rw::random(state.seed, state.tick, rw::ids(uint32_t(c.value), uint32_t(st))).value() < 0.5f || wing)
                lose_all_grasps(state, c);
            if (wing)
                st *= 2;
            w.creature_set_stun(c, std::max(w.creature_get_stun(c), st));
        }

        void die(sys::game_state& state, dcon::creature_id c) {
            state.world.creature_set_dead(c, 1);
            lose_all_grasps(state, c);
        }

        // CentipedeAI.OverChasm: how much of a pit lies under and beside `t`
        float over_chasm(room_view const& room, ivec2 t) {
            float num = lizard::ai::fall_risk_tile(room, t).y < 0 ? 1.f : 0.f;
            for (int i = -1; i < 2; i += 2)
                for (int k = 1; k < 7 && !room.get_tile(t + ivec2{k * i, 0}).solid(); ++k)
                    if (lizard::ai::fall_risk_tile(room, t + ivec2{k * i, 0}).y < 0)
                        num += 1.f / float(k);
            return mathf::inverse_lerp(0.f, 5.9f, num);
        }

        // the tracker's settings (CentipedeAI's constructor)
        tracker::params const& tracker_params(variant v) {
            static tracker::params const normal = [] {
                tracker::params p;
                p.see_around_corners = 10;
                p.frames_to_remember = -1;
                p.ghost_dismissal_range = 20;
                p.discard_dead_always = true;
                p.prey_max = 5;
                p.persistance_bias = 1.f;
                p.sure_to_get = 5.f;
                p.sure_to_lose = 150.f;
                p.threat_max = 3;
                return p;
            }();
            static tracker::params const red = [] {
                tracker::params p = normal;
                p.sure_to_get = 100.f;
                return p;
            }();
            return v == variant::red ? red : normal;
        }

        // CentipedeAI.UpdateDynamicRelationship: afraid of what outweighs it,
        // else prey; the red centipede wants the heavy ones most, the
        // centiwing is wary of what it has just noticed and keeps away from
        // what is over a chasm
        relationship relationship_to(sys::game_state const& state, dcon::creature_id c, creature::tracked const& r) {
            auto const st = creature::static_relationship(state, c, r.who);
            if (st.type == creature::rel::ignores || !creature::live(state, r.who))
                return st;
            if (creature::dead(state, r.who))
                return relationship{};
            float const mine = total_mass(state, c);
            float const theirs = creature::mass_of(state, r.who);
            if (st.type != creature::rel::eats || theirs >= mine)
                return relationship{creature::rel::afraid, 0.2f + 0.8f * mathf::inverse_lerp(mine, mine * 1.5f, theirs)};
            switch (pl(state, c).variant) {
            case variant::wing: {
                float num = mathf::pow(mathf::inverse_lerp(0.f, mine, theirs), 0.75f);
                num *= 1.f - over_chasm(world::get_room_view(state, room_of(state, c)), tracker::best_guess(r));
                if (r.age < 300)
                    return relationship{creature::rel::afraid, num * mathf::inverse_lerp(300.f, 0.f, float(r.age))};
                return relationship{creature::rel::eats, num * mathf::inverse_lerp(300.f, 800.f, float(r.age))};
            }
            case variant::red:
                return relationship{creature::rel::eats, mathf::pow(mathf::inverse_lerp(0.f, mine, theirs), custom::lerp_map(theirs, 0.2f, 0.7f, 3.f, 0.1f)) * st.intensity};
            default:
                return relationship{creature::rel::eats, mathf::inverse_lerp(0.f, mine, theirs) * st.intensity};
            }
        }

        // CentipedeAI.ModuleToTrackRelationship: Antagonizes is prey too
        creature::track_module module_of(relationship r) noexcept {
            if (r.type == creature::rel::antagonizes)
                return creature::track_module::prey;
            return tracker::module_without_aggression(r);
        }

        // CentipedeAI.DoIWantToShockCreature
        bool want_to_shock(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) {
            auto const& w = state.world;
            auto const behavior = world::centipede_behavior(w.creature_get_behavior(c));
            if (pl(state, c).annoying_collisions < 150 && (behavior == centipede_behavior::flee || behavior == centipede_behavior::escape_rain) && w.creature_get_current_utility(c) > 0.1f)
                return false;
            if (!creature::live(state, t) || creature::dead(state, t))
                return false;
            auto const* r = tracker::find(state, c, t);
            if (!is(state, c, variant::small) && pl(state, c).annoying_collisions > 150
            && (!r || float(r->state) > mathf::lerp(210.f, 30.f, pl(state, c).size) * w.creature_get_health(c)))
                return true;
            if (r)
                return r->rel.type == creature::rel::eats;
            return creature::static_relationship(state, c, t).type == creature::rel::eats;
        }

        // CentipedeAI.AnnoyingCollision (CentipedeTrackState.annoyingCollisions
        // is the representation's `state`)
        void annoying_collision(sys::game_state& state, dcon::creature_id c, dcon::creature_id t) {
            if (creature::dead(state, t) || is(state, c, variant::small))
                return;
            pl(state, c).annoying_collisions += 10;
            if (pl(state, c).annoying_collisions < 150)
                return;
            if (auto* r = tracker::find(tracker::of(state, c), t); r && r->state < 255)
                ++r->state;
        }

        // Centipede.Shock
        void shock(sys::game_state& state, dcon::creature_id c, dcon::creature_id p) {
            auto& w = state.world;
            auto rng = rw::random(state.seed, state.tick, rw::ids(uint32_t(c.value), uint32_t(p.value)));
            auto const room = room_of(state, c);
            uint32_t const n = chunk_count(state, c);
            auto const me = creature::template_of(state, c);
            state.play_sound(sound::sound_id::centipede_shock, room, pos(state, c, n / 2));
            // (the sparks are the shock sound's, particles.cpp)
            w.centipede_data_set_light_flash(data(state, c), 1.f);
            auto const jolt = [&](dcon::body_chunk_id k) {
                vec2 const a = rnv(rng);
                w.body_chunk_get_vel(k) += a * 6.f * rng.value();
                vec2 const b = rnv(rng);
                w.body_chunk_get_pos(k) += b * 6.f * rng.value();
            };
            for (uint32_t i = 0; i < n; ++i)
                jolt(chunk(state, c, i));
            uint32_t const m = creature::chunk_count_of(state, p);
            for (uint32_t i = 0; i < m; ++i)
                jolt(creature::chunk_of(state, p, i));
            float const theirs = creature::mass_of(state, p);
            float const mine = total_mass(state, c);
            // Creature.LoseAllGrasps of what was shocked
            auto const lose_all = [&] { creature::lose_all_grasps(state, p); };
            switch (pl(state, c).variant) {
            case variant::aqua: {
                // Violence(Electric, 2, 200): a slugcat's instantDeathDamageLimit
                // is 1; and the UnderwaterShock through the water around it
                creature::violence_on(state, p, creature::main_chunk_of(state, p), vec2{0.f, 0.f}, 2.f, 200.f, creature::damage_type::electric, me);
                lose_all();
                auto const view = world::get_room_view(state, room);
                bool submerged = false;
                for (uint32_t i = 0; i < m && !submerged; ++i)
                    submerged = physics::submersion(state, view, creature::chunk_of(state, p, i)) > 0.f;
                if (submerged) {
                    w.centipede_data_set_underwater_shock(data(state, c), 0);
                    w.centipede_data_set_underwater_shock_pos(data(state, c), pos(state, c, head_index(state, c)));
                }
                return;
            }
            case variant::small:
                creature::stun(state, p, 120);
                lose_all();
                return;
            default:
                break;
            }
            if (theirs < mine) {
                creature::kill(state, p, me);
                lose_all();
            } else {
                creature::stun(state, p, int32_t(custom::lerp_map(theirs, 0.f, mine * 2.f, 300.f, 30.f)));
                lose_all();
                stun(state, c, 6);
                pl(state, c).shock_give_up_counter = std::max(pl(state, c).shock_give_up_counter, 30);
                pl(state, c).annoying_collisions = std::min(pl(state, c).annoying_collisions / 2, 150);
            }
        }

        // UnderwaterShock (the aquapede's, 14 frames of radius 80 and 1
        // damage): every creature and slugcat in the water around it,
        // the aquapede aside
        void underwater_shock(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto const d = data(state, c);
            int32_t const frame = w.centipede_data_get_underwater_shock(d);
            if (frame < 0)
                return;
            constexpr int32_t life = 14;
            constexpr float rad = 80.f, damage = 1.f;
            vec2 const at = w.centipede_data_get_underwater_shock_pos(d);
            float const r = rad * (0.25f + 0.75f * std::sin(mathf::inverse_lerp(0.f, float(life), float(frame)) * mathf::pi));
            auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
            auto const me = creature::template_of(state, c);
            auto const hit = [&](dcon::creature_id t) {
                if (!creature::here(state, t, room.id))
                    return;
                uint32_t const n = creature::chunk_count_of(state, t);
                bool any = false;
                for (uint32_t i = 0; i < n; ++i)
                    any = any || physics::submersion(state, room, creature::chunk_of(state, t, i)) > 0.f;
                if (!any)
                    return;
                float num = 0.f;
                for (uint32_t i = 0; i < n; ++i) {
                    auto const k = creature::chunk_of(state, t, i);
                    vec2 const kp = w.body_chunk_get_pos(k);
                    if (custom::dist_less(at, kp, r))
                        num = mathf::max(num, custom::lerp_map(rw::distance(at, kp), r / 2.f, r, 1.f, 0.f, 0.5f) * physics::submersion(state, room, k));
                }
                if (w.room_get_is_shelter(room.id) || num <= 0.f)
                    return;
                for (uint32_t i = 0; i < n; ++i) {
                    auto const k = creature::chunk_of(state, t, i);
                    w.body_chunk_get_vel(k) += rnv(rng) * num * mathf::min(5.f, w.body_chunk_get_rad(k));
                }
                creature::violence_on(state, t, uint32_t(rng.range(0, int32_t(n))), vec2{0.f, 0.f}, damage * num, damage * num * 240.f + 30.f,
                    creature::damage_type::electric, me);
            };
            rooms::for_each_creature(state, room.id, [&](dcon::creature_id o) {
                if (o != c)
                    hit(o);
            });
            w.centipede_data_set_underwater_shock(d, frame + 1 > life ? -1 : frame + 1);
        }

        void grab(sys::game_state& state, dcon::creature_id c, int g, dcon::creature_id p, uint32_t k) {
            creature::grab(state, c, uint32_t(g), p, k);
            uint32_t const end = g == 0 ? 0 : chunk_count(state, c) - 1;
            state.play_sound(sound::sound_id::centipede_attach, room_of(state, c), pos(state, c, end));
        }

        // Centipede.UpdateGrasp
        void update_grasp(sys::game_state& state, dcon::creature_id c, int g) {
            auto& w = state.world;
            auto const p = grasped(state, c, g);
            if (!p)
                return;
            auto const room = room_of(state, c);
            if (!creature::here(state, p, room) || creature::grasp_chunk(w, c, uint32_t(g)) >= creature::chunk_count_of(state, p)) {
                release(state, c, g);
                return;
            }
            if (rw::random(state.seed, state.tick, rw::ids(uint32_t(c.value), uint32_t(g))).value() < 0.025f && (!want_to_shock(state, c, p) || !consious(state, c) || grasped(state, c, 1 - g))) {
                release(state, c, g);
                return;
            }
            uint32_t const n = chunk_count(state, c);
            auto const end = chunk(state, c, g == 0 ? 0 : n - 1);
            auto const held = creature::chunk_of(state, p, creature::grasp_chunk(w, c, uint32_t(g)));
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
            uint32_t const m = creature::chunk_count_of(state, p);
            for (uint32_t i = 0; i < m; ++i) {
                auto const k = creature::chunk_of(state, p, i);
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
                float& charge = pl(state, c).shock_charge;
                charge += 1.f / mathf::lerp(100.f, 5.f, pl(state, c).size);
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

        // ArtificialIntelligence.VisualScore + CentipedeAI.VisualScore,
        // roughly: distance, facing, and the poor sight of a crawling
        // centipede (not a centiwing's or a red one's)
        bool sees(sys::game_state const& state, dcon::creature_id c, room_view const& room, vec2 target) {
            uint32_t const n = chunk_count(state, c);
            variant const v = pl(state, c).variant;
            bool const front = (state.tick & 1u) != 0; // CentipedeAI alternates ends
            vec2 const p1 = front ? pos(state, c, 1) : pos(state, c, n - 2);
            vec2 const p2 = front ? pos(state, c, 0) : pos(state, c, n - 1);
            float const radius = visual_radius(v);
            float const d = rw::distance(p2, target);
            if (d > radius || !room.visual_contact(p2, target))
                return false;
            bool const keen = v == variant::wing || v == variant::red;
            float score = 1.f - d / radius;
            score -= mathf::inverse_lerp(1.f, keen ? 0.3f : 0.7f, rw::dot(rw::normalized(p1 - p2), rw::normalized(p2 - target)));
            if (pl(state, c).moving && !keen)
                score -= 0.75f;
            return score > 0.05f;
        }

        // the trackers' questions, answered by the centipede
        struct hook_ctx {
            sys::game_state& state;
            dcon::creature_id c;
            room_view const& room;
        };
        bool hook_sees(void* u, dcon::body_chunk_id k) {
            auto& x = *static_cast<hook_ctx*>(u);
            return sees(x.state, x.c, x.room, x.state.world.body_chunk_get_pos(k));
        }
        bool hook_sees_point(void* u, vec2 p) {
            auto& x = *static_cast<hook_ctx*>(u);
            return sees(x.state, x.c, x.room, p);
        }
        relationship hook_relate(void* u, creature::tracked const& r) {
            auto& x = *static_cast<hook_ctx*>(u);
            return relationship_to(x.state, x.c, r);
        }
        // PathFinder.CoordinateReachableAndGetbackable, by the field to its
        // destination: the tile leads there and so does the centipede
        bool hook_reach(void* u, ivec2 t) {
            auto& x = *static_cast<hook_ctx*>(u);
            if (!x.room.inside(t.x, t.y) || !pathing::accessible(x.room, t, movement(x.state, x.c)))
                return false;
            auto const f = pathing::field_to(x.state, x.room.id, x.state.world.creature_get_destination(x.c), movement(x.state, x.c));
            return pathing::distance(x.state, f, t) != pathing::unreachable;
        }
        tracker::hooks make_hooks(hook_ctx& x) {
            tracker::hooks h;
            h.user = &x;
            h.sees = hook_sees;
            h.sees_point = hook_sees_point;
            h.relate = hook_relate;
            h.module = module_of;
            h.reach = hook_reach;
            return h;
        }

        // CentipedeAI.IdleScore (no attractors, the other centipedes' spots
        // aren't taken into account)
        float idle_score(sys::game_state& state, dcon::creature_id c, room_view const& room, ivec2 t) {
            if (!room.inside(t.x, t.y) || !pathing::accessible(room, t, movement(state, c)))
                return -1e9f;
            // (vanilla's AquaCenti test reads the tile coordinates as a
            // position, so nearly every tile counts as submerged)
            if (is(state, c, variant::aqua) && !room.point_submerged(rw::to_vec2(t)))
                return -1e9f;
            float score = 1000.f;
            if (!is(state, c, variant::wing))
                score /= mathf::max(1.f, float(pathing::ai_terrain_proximity(state, room.id, t)) - 1.f);
            ivec2 const f = state.world.centipede_data_get_forbidden_idle_pos(data(state, c));
            score -= custom::lerp_map(rw::magnitude(rw::to_vec2(t - f)), 0.f, 10.f, 1000.f, 0.f);
            if (lizard::ai::fall_risk_tile(room, t).y < 0)
                score -= custom::lerp_map(float(t.y), 10.f, 30.f, 1000.f, 0.f);
            return score;
        }

        // CentipedeAI.CheckRandomIdlePos
        void check_random_idle_pos(sys::game_state& state, dcon::creature_id c, room_view const& room, rw::rng& rng) {
            int32_t const x = rng.range(0, room.width);
            ivec2 const test{x, rng.range(0, room.height)};
            if (idle_score(state, c, room, test) > idle_score(state, c, room, pl(state, c).temp_idle_pos)) {
                pl(state, c).temp_idle_pos = test;
                state.world.centipede_data_set_idle_counter(data(state, c), 0);
            }
        }

        // CentipedeAI.Update
        void ai_update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
            auto const d = data(state, c);
            variant const v = pl(state, c).variant;
            if (pl(state, c).annoying_collisions > 0)
                --pl(state, c).annoying_collisions;
            ivec2 const here = room_view::get_tile_position(pos(state, c, head_index(state, c)));

            // the trackers (Tracker, PreyTracker, ThreatTracker, RelationshipTracker)
            hook_ctx hx{state, c, room};
            tracker::update(state, c, room, here, tracker_params(v), make_hooks(hx));
            auto const& ts = tracker::of(state, c);
            auto const* prey = tracker::prey(ts);
            auto const* threat = tracker::threat(ts);
            // CentipedeAI.Update: the prey's weight follows how long ago it
            // was seen (the red centipede's stays), the threat's too
            auto const since = [](creature::tracked const& r) { return float(std::max(0, r.ticks_since_seen - 10)); };
            float prey_weight = v == variant::wing ? 0.12f : 0.9f;
            if (prey && v != variant::red)
                prey_weight = mathf::inverse_lerp(50.f, 10.f, since(*prey));
            float const threat_weight = threat ? mathf::inverse_lerp(500.f, 100.f, since(*threat)) : 1.f;
            // UtilityComparer: threat (1), prey, rain (1), injury (0.7)
            float const prey_utility = tracker::prey_utility(state, c, room, here, tracker_params(v)) * prey_weight;
            float const threat_utility = tracker::threat_utility(ts) * threat_weight;
            float const health = w.creature_get_health(c);
            float const injury_utility = health < 0.6f ? (0.6f - health) / 0.6f * 0.7f : 0.f;
            float utility = threat_utility;
            centipede_behavior behavior = centipede_behavior::flee;
            if (float const rain = dens::rain_utility(state, c, room.id); rain > utility) {
                utility = rain;
                behavior = centipede_behavior::escape_rain;
            }
            if (prey_utility > utility) {
                utility = prey_utility;
                behavior = centipede_behavior::hunt;
            }
            if (injury_utility > utility) {
                utility = injury_utility;
                behavior = centipede_behavior::injured;
            }
            if (utility < 0.1f || (behavior == centipede_behavior::hunt && !prey))
                behavior = centipede_behavior::idle;
            w.creature_set_current_utility(c, utility);
            w.creature_set_behavior(c, uint8_t(behavior));
            // (no realized migration in CentipedeAI: the abstract roll, now and then)
            if (behavior == centipede_behavior::idle)
                migrate::idle_roll(state, c, room);

            float excitement_goal = 0.f;
            ivec2& dest = w.creature_get_destination(c);
            switch (behavior) {
            case centipede_behavior::idle:
                if (auto const t = migrate::travel_tile(state, c, room)) {
                    dest = *t;
                    break;
                }
                [[fallthrough]];
            case centipede_behavior::injured: {
                excitement_goal = behavior == centipede_behavior::injured ? 1.f : 0.f;
                if (behavior == centipede_behavior::injured) {
                    if (auto const t = dens::home_tile(state, c, room)) {
                        dest = *t;
                        break;
                    }
                }
                int32_t const dx = rng.range(-10, 11);
                ivec2 test = here + ivec2{dx, rng.range(-10, 11)};
                // a centiwing looks anywhere in the room far more often
                if (rng.value() < 1.f / (v == variant::wing ? 3.f : 100.f)) {
                    int32_t const x = rng.range(0, room.width);
                    test = ivec2{x, rng.range(0, room.height)};
                }
                ivec2& idle = pl(state, c).temp_idle_pos;
                int32_t& counter = w.centipede_data_get_idle_counter(d);
                if (idle_score(state, c, room, test) > idle_score(state, c, room, idle)) {
                    idle = test;
                    counter = 0;
                } else {
                    ++counter;
                    float const to_idle = rw::magnitude(rw::to_vec2(here - idle));
                    if ((v == variant::wing || v == variant::aqua) && to_idle < 15.f)
                        counter += 2;
                    bool const in_air = v == variant::wing && pathing::tile_access(room, idle) > pathing::access::climb;
                    if (counter > (in_air ? 400 : 1400) || (pl(state, c).flying && !rather_climb_than_fly(state, room, idle) && to_idle < 5.f)
                    || w.centipede_data_get_outside_level(d)) {
                        counter = 0;
                        w.centipede_data_set_forbidden_idle_pos(data(state, c), idle);
                    }
                }
                if (idle != dest && idle_score(state, c, room, idle) > idle_score(state, c, room, dest) + 100.f)
                    dest = idle;
                break;
            }
            case centipede_behavior::flee:
                excitement_goal = 1.f;
                dest = tracker::flee_to(state, c, room, here, 1, 30, tracker_params(v), make_hooks(hx));
                break;
            case centipede_behavior::hunt:
                excitement_goal = prey->rel.intensity;
                dest = tracker::best_guess(*prey);
                break;
            case centipede_behavior::escape_rain:
                // the den (DenFinder, dens.hpp)
                excitement_goal = 0.5f;
                if (auto const t = dens::home_tile(state, c, room))
                    dest = *t;
                break;
            }
            float& excitement = pl(state, c).excitement;
            excitement = mathf::lerp(excitement, excitement_goal, 0.1f);
            // a centiwing never rests, nor does a red centipede on the hunt
            float& run = pl(state, c).run;
            if (v == variant::wing || (v == variant::red && behavior == centipede_behavior::hunt)) {
                run = 500.f;
                return;
            }
            // bursts of running and resting
            run -= 1.f;
            if (run < mathf::lerp(-50.f, -5.f, excitement))
                run = mathf::lerp(30.f, 50.f, excitement);
        }

        // Centipede.Crawl
        void crawl(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
            uint32_t const n = chunk_count(state, c);
            float const size = pl(state, c).size;
            variant const v = pl(state, c).variant;
            float const health = mathf::clamp01(w.creature_get_health(c));
            float const g = physics::gravity(state, body(state, c), room);
            bool const moving = pl(state, c).moving;
            int const fwd = pl(state, c).body_direction ? -1 : 1;
            auto const hold = [&]() { return g * mathf::pow(mathf::max(0.f, mathf::lerp(health, 1.f, rng.value())), 0.25f); };
            uint32_t grip = 0;
            for (uint32_t i = 0; i < n; ++i) {
                auto const k = chunk(state, c, i);
                vec2& vel = w.body_chunk_get_vel(k);
                vec2 const p = w.body_chunk_get_pos(k);
                if (!accessible(state, c, room, p))
                    continue;
                ++grip;
                vel *= 0.7f;
                vel.y += hold();
                if (i > 0 && !accessible(state, c, room, pos(state, c, i - 1))) {
                    vel *= 0.3f;
                    vel.y += hold();
                }
                if (i < n - 1 && !accessible(state, c, room, pos(state, c, i + 1))) {
                    vel *= 0.3f;
                    vel.y += hold();
                }
                if (i == 0 || i == n - 1)
                    continue;
                if (moving) {
                    vec2 const ahead = pos(state, c, uint32_t(int(i) + fwd));
                    if (accessible(state, c, room, ahead))
                        vel += custom::dir_vec(p, ahead) * 1.5f * mathf::lerp(0.5f, 1.5f, size * health) * (v == variant::red ? 1.25f : 1.f);
                    vel -= custom::dir_vec(p, pos(state, c, uint32_t(int(i) - fwd))) * 0.8f * mathf::lerp(0.7f, 1.3f, size * health);
                } else {
                    // settle onto the surface it sits on
                    vec2 const along = (rw::normalized(p - pos(state, c, i - 1)) + rw::normalized(pos(state, c, i + 1) - p)) / 2.f;
                    float const k_fac = mathf::lerp(0.01f, 0.6f, mathf::pow(size * health, 1.2f));
                    float const rad = w.body_chunk_get_rad(k);
                    if (std::fabs(along.x) > 0.5f)
                        vel.y -= (p.y - (room_view::middle_of_tile(p).y + float(vertical_sit_surface(room, p)) * (10.f - rad))) * k_fac;
                    if (std::fabs(along.y) > 0.5f)
                        vel.x -= (p.x - (room_view::middle_of_tile(p).x + float(horizontal_sit_surface(room, p)) * (10.f - rad))) * k_fac;
                }
            }
            auto const head = chunk(state, c, head_index(state, c));
            vec2 const target = pl(state, c).move_to_pos;
            if (grip > 0 && !custom::dist_less(w.body_chunk_get_pos(head), target, 10.f)) {
                if (v == variant::small)
                    w.body_chunk_get_vel(head) += custom::dir_vec(w.body_chunk_get_pos(head), target) * custom::lerp_map(float(grip), 0.f, float(n), 3.f, 1.f);
                else
                    w.body_chunk_get_vel(head) += custom::dir_vec(w.body_chunk_get_pos(head), target) * custom::lerp_map(float(grip), 0.f, float(n), 6.f, 3.f)
                        * mathf::lerp(0.7f, 1.3f, size * w.creature_get_health(c)) * (v == variant::wing ? 0.7f : 1.f);
            }
            // a centiwing with nothing to hold on to takes off
            if (v == variant::wing && grip == 0) {
                w.centipede_data_get_fly_mode_counter(data(state, c)) += 10;
                w.centipede_data_set_want_to_fly(data(state, c), 1);
            }
            if (v == variant::aqua)
                w.body_chunk_get_vel(head) *= 0.2f;
        }

        // Centipede.Fly: the centiwing's flight and the aquapede's swimming,
        // the body waving as it goes
        void fly(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto const d = data(state, c);
            uint32_t const n = chunk_count(state, c);
            float const size = pl(state, c).size;
            bool const aqua = is(state, c, variant::aqua);
            auto const head = chunk(state, c, head_index(state, c));
            vec2 const head_pos = w.body_chunk_get_pos(head);
            // (vanilla measures to the idle tile's coordinates as a position)
            vec2 const idle = rw::to_vec2(pl(state, c).temp_idle_pos);
            float& wave = w.centipede_data_get_body_wave(d);
            if (aqua)
                wave += mathf::clamp(rw::distance(head_pos, idle) / 80.f, 0.1f, 1.f);
            else
                wave += 1.f;
            float const g = w.physical_object_get_g(body(state, c));
            float const started = w.centipede_data_get_wings_started_up(d);
            bool const dir = pl(state, c).body_direction;
            bool const idling = aqua && world::centipede_behavior(w.creature_get_behavior(c)) == centipede_behavior::idle;
            for (uint32_t i = 0; i < n; ++i) {
                float t = float(i) / float(n - 1);
                if (!dir)
                    t = 1.f - t;
                float const s = std::sin((wave - t * mathf::lerp(12.f, 28.f, size)) * mathf::pi * 0.11f);
                auto const k = chunk(state, c, i);
                vec2& vel = w.body_chunk_get_vel(k);
                vel *= 0.9f;
                vel.y += g * started;
                if (i == 0 || i == n - 1)
                    continue;
                vec2 const along = custom::dir_vec(w.body_chunk_get_pos(k), pos(state, c, uint32_t(int(i) + (dir ? -1 : 1))));
                vec2 const perp = custom::perpendicular_vector(along);
                vel += along * 0.5f * mathf::lerp(0.5f, 1.5f, size);
                if (idling) {
                    vel *= mathf::clamp(rw::distance(head_pos, idle) / 40.f, 0.02f, 1.f);
                    if (rw::distance(head_pos, idle) < 20.f)
                        vel *= 0.28f;
                }
                w.body_chunk_get_pos(k) += perp * 2.5f * s;
            }
            vec2 const to = pl(state, c).move_to_pos;
            bool const swim = aqua_swimming(state, c, room);
            vec2& hv = w.body_chunk_get_vel(head);
            if (pathing::ai_terrain_proximity(state, room.id, room_view::get_tile_position(to)) > 2) {
                if (swim)
                    hv += custom::dir_vec(head_pos, to + custom::deg_to_vec(wave * 5.f) * 10.f) * 5.f * mathf::lerp(0.7f, 1.3f, size);
                else
                    hv += custom::dir_vec(head_pos, to + custom::deg_to_vec(wave * 10.f) * 60.f) * 4.f * mathf::lerp(0.7f, 1.3f, size);
            } else if (swim) {
                hv += custom::dir_vec(head_pos, to) * 2.f * mathf::lerp(0.2f, 0.8f, size);
            } else {
                hv += custom::dir_vec(head_pos, to) * 4.f * mathf::lerp(0.7f, 1.3f, size);
            }
        }

        void act(sys::game_state& state, dcon::creature_id c, room_view const& room);

        // Centipede.Swim: a centipede that isn't an aquapede, in the water,
        // follows its path along the surface
        void swim(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
            uint32_t const n = chunk_count(state, c);
            auto const head = chunk(state, c, head_index(state, c));
            ivec2 from = room_view::get_tile_position(w.body_chunk_get_pos(head));
            from.y = std::max(from.y, room.default_water_level);
            auto const field = pathing::field_to(state, room.id, w.creature_get_destination(c), movement(state, c));
            auto const step = pathing::next_step(state, field, from);
            if (!step)
                return;
            if (step->y > from.y) {
                if (accessible(state, c, room, w.body_chunk_get_pos(head)))
                    act(state, c, room);
                w.body_chunk_get_vel(head).y += 1.f;
                return;
            }
            vec2 const dir = custom::dir_vec(rw::to_vec2(from), rw::to_vec2(*step));
            w.body_chunk_get_vel(head) += dir * 1.2f;
            if (accessible(state, c, room, w.body_chunk_get_pos(head))) {
                for (uint32_t i = 0; i < n; ++i) {
                    auto const k = chunk(state, c, i);
                    vec2 const r = rnv(rng);
                    vec2& vel = w.body_chunk_get_vel(k);
                    vel += dir * 0.05f + r * rng.value() * 4.f;
                    vel.y += mathf::clamp(room.float_water_level - w.body_chunk_get_pos(k).y, -5.f, 5.f) * 0.05f;
                }
            }
            if (pl(state, c).moving) {
                float const size = pl(state, c).size;
                int const fwd = pl(state, c).body_direction ? -1 : 1;
                for (uint32_t i = 1; i + 1 < n; ++i)
                    w.body_chunk_get_vel(chunk(state, c, i)) += custom::dir_vec(pos(state, c, i), pos(state, c, uint32_t(int(i) + fwd))) * 0.2f * mathf::lerp(0.5f, 1.5f, size);
            }
        }

        // Centipede.Act
        void act(sys::game_state& state, dcon::creature_id c, room_view const& room) {
            auto& w = state.world;
            auto const d = data(state, c);
            uint32_t const n = chunk_count(state, c);
            float const size = pl(state, c).size;
            variant const v = pl(state, c).variant;
            bool const swimming = aqua_swimming(state, c, room);
            if (swimming)
                w.centipede_data_set_fly_mode_counter(d, 100);
            if (v == variant::wing) {
                // taking off once it has wanted to for a while, landing again
                int32_t& counter = w.centipede_data_get_fly_mode_counter(d);
                if (w.centipede_data_get_want_to_fly(d)) {
                    if (counter == 100)
                        pl(state, c).flying = 1;
                    counter = std::min(100, counter + 1);
                } else {
                    if (counter < 90)
                        pl(state, c).flying = 0;
                    counter = std::max(0, counter - 1);
                }
                float const goal = mathf::inverse_lerp(80.f, 100.f, float(counter));
                float& started = w.centipede_data_get_wings_started_up(d);
                if (started < goal)
                    started = mathf::min(1.f, started + 0.025f);
                else
                    started = mathf::max(0.f, started - 0.025f);
                started = mathf::lerp(started, goal, 0.05f);
                // fell out of the room: fly back in
                ivec2 const at = room_view::get_tile_position(pos(state, c, n / 2));
                if (at.y < -10) {
                    counter = 100;
                    w.centipede_data_set_want_to_fly(d, 1);
                    pl(state, c).flying = 1;
                    started = 1.f;
                    for (uint32_t i = 0; i < n; ++i)
                        w.body_chunk_get_vel(chunk(state, c, i)).y += 3.f;
                    if (at.x < -10) {
                        for (uint32_t i = 0; i < n; ++i)
                            w.body_chunk_get_vel(chunk(state, c, i)).x += 2.f;
                    } else if (at.x > room.width + 10) {
                        for (uint32_t i = 0; i < n; ++i)
                            w.body_chunk_get_vel(chunk(state, c, i)).x -= 2.f;
                    }
                    pl(state, c).move_to_pos = vec2{room.pixel_width() / 2.f, room.pixel_height() / 2.f};
                    w.centipede_data_set_outside_level(d, 1);
                } else {
                    w.centipede_data_set_outside_level(d, 0);
                }
            }

            // turn around when the tail is the end closer to the goal
            int32_t& block = pl(state, c).direction_change_block;
            if (block > 0) {
                block -= pl(state, c).moving ? 1 : 0;
            } else if (!pl(state, c).flying) {
                auto const field = pathing::field_to(state, room.id, w.creature_get_destination(c), movement(state, c));
                bool const dir = pl(state, c).body_direction;
                ivec2 const head_t = room_view::get_tile_position(pos(state, c, dir ? 0 : n - 1));
                ivec2 const tail_t = room_view::get_tile_position(pos(state, c, dir ? n - 1 : 0));
                if (pathing::distance(state, field, tail_t) < pathing::distance(state, field, head_t)) {
                    bool const open = pathing::terrain_proximity(room, head_t) > 1 || pathing::terrain_proximity(room, tail_t) > 1;
                    int32_t const wait = v == variant::wing ? (open ? 40 : 10) : (open ? 10 : 2);
                    if (++pl(state, c).change_dir_counter > wait) {
                        pl(state, c).body_direction = dir ? 0 : 1;
                        block = 40;
                        pl(state, c).change_dir_counter = 0;
                    }
                } else {
                    pl(state, c).change_dir_counter = 0;
                }
            }

            ai_update(state, c, room);
            auto const head = chunk(state, c, head_index(state, c));
            ivec2 const head_t = room_view::get_tile_position(w.body_chunk_get_pos(head));
            ivec2 const goal = w.creature_get_destination(c);
            bool moving = pl(state, c).run > 0.f && std::abs(head_t.x - goal.x) + std::abs(head_t.y - goal.y) > 2;
            if (pl(state, c).flying || swimming)
                moving = true;
            if (moving) {
                auto const f = pathing::field_to(state, room.id, goal, movement(state, c));
                if (pathing::distance(state, f, head_t) == pathing::unreachable && world::centipede_behavior(w.creature_get_behavior(c)) == centipede_behavior::idle) {
                    // nowhere to go from here: try somewhere else
                    w.centipede_data_set_forbidden_idle_pos(data(state, c), goal);
                    pl(state, c).temp_idle_pos = head_t;
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
                    pl(state, c).move_to_pos = to;
                    if (v == variant::wing || v == variant::aqua) {
                        bool const flying = pl(state, c).flying;
                        if (!rather_climb_than_fly(state, room, *step) && !flying)
                            moving = false;
                        if (flying && room.visual_contact(w.body_chunk_get_pos(head), room_view::middle_of_tile(goal))) {
                            pl(state, c).move_to_pos = room_view::middle_of_tile(goal);
                        } else {
                            // flying, it heads for the furthest tile along
                            // the path it can see
                            ivec2 at = *step;
                            for (int k = 0; k < 10; ++k) {
                                auto const next = pathing::next_step(state, f, at);
                                if (!next || *next == goal)
                                    break;
                                at = *next;
                                if (flying && room.visual_contact(w.body_chunk_get_pos(head), room_view::middle_of_tile(at)))
                                    pl(state, c).move_to_pos = room_view::middle_of_tile(at);
                            }
                            if (!flying && !rather_climb_than_fly(state, room, *step))
                                w.centipede_data_set_want_to_fly(d, 1);
                            else if (flying && rather_climb_than_fly(state, room, *step))
                                w.centipede_data_set_want_to_fly(d, 0);
                        }
                    }
                    pl(state, c).no_follow_con_counter = 0;
                } else if (++pl(state, c).no_follow_con_counter > 40) {
                    uint32_t const k = uint32_t(rw::random(state.seed, state.tick, uint32_t(c.value)).range(0, int32_t(n)));
                    if (accessible(state, c, room, pos(state, c, k)))
                        pl(state, c).move_to_pos = pos(state, c, k);
                }
            }
            pl(state, c).moving = moving;
            // landing on a perch it reached
            if (pl(state, c).flying && rather_climb_than_fly(state, room, goal) && rw::magnitude(rw::to_vec2(head_t - goal)) < 3.f)
                w.centipede_data_set_want_to_fly(d, 0);
            // close enough to the prey it is hunting: reach for it (the path ends
            // two tiles short of the prey's tile)
            auto const* hunted = tracker::prey(tracker::of(state, c));
            if (world::centipede_behavior(w.creature_get_behavior(c)) == centipede_behavior::hunt && hunted && hunted->ticks_since_seen < 10) {
                vec2 const prey = hunted->last_seen;
                if (custom::dist_less(w.body_chunk_get_pos(head), prey, 70.f) && room.visual_contact(w.body_chunk_get_pos(head), prey))
                    pl(state, c).move_to_pos = prey;
            }

            auto const o = body(state, c);
            if (!grasped(state, c, 0) && !grasped(state, c, 1)) {
                if (pl(state, c).flying || swimming) {
                    if (v == variant::aqua)
                        w.physical_object_set_buoyancy(o, 0.78f);
                    fly(state, c, room);
                } else {
                    crawl(state, c, room);
                }
                pl(state, c).double_grab_charge = mathf::max(0.f, pl(state, c).double_grab_charge - 0.025f);
                pl(state, c).shock_give_up_counter = std::max(0, pl(state, c).shock_give_up_counter - 2);
                return;
            }
            // holding on: an aquapede sinks with what it holds
            if (v == variant::aqua)
                w.physical_object_set_buoyancy(o, 0.15f);
            // holding on with one end: bring the other one around (along its
            // own body where it can't see the prey's chunk)
            for (int g = 0; g < 2; ++g) {
                auto const p = grasped(state, c, g);
                if (!p || grasped(state, c, 1 - g) || !want_to_shock(state, c, p))
                    continue;
                if (!creature::live(state, p))
                    continue;
                vec2 const held = w.body_chunk_get_pos(creature::chunk_of(state, p, creature::grasp_chunk(w, c, uint32_t(g))));
                vec2 const hp = w.body_chunk_get_pos(head);
                pl(state, c).move_to_pos = held;
                if (!room.visual_contact(hp, held)) {
                    if (head_index(state, c) == 0) {
                        for (uint32_t k = n; k-- > 0;)
                            if (room.visual_contact(pos(state, c, k), hp)) {
                                pl(state, c).move_to_pos = pos(state, c, k);
                                break;
                            }
                    } else {
                        for (uint32_t k = 0; k < n; ++k)
                            if (room.visual_contact(pos(state, c, k), hp)) {
                                pl(state, c).move_to_pos = pos(state, c, k);
                                break;
                            }
                    }
                }
            }
            float& charge = pl(state, c).double_grab_charge;
            w.body_chunk_get_vel(head) += custom::dir_vec(w.body_chunk_get_pos(head), pl(state, c).move_to_pos) * mathf::pow(charge, 2.f) * 6.f * mathf::lerp(0.7f, 1.3f, size);
            charge = mathf::min(1.f, charge + 0.0125f);
            if (charge > 0.9f) {
                int32_t& give_up = pl(state, c).shock_give_up_counter;
                give_up = std::min(110, give_up + 1);
                if (give_up >= 110) {
                    stun(state, c, 12);
                    give_up = 30;
                    lose_all_grasps(state, c);
                }
            }
        }

        // Centipede.Violence's CentipedeShells: cosmetic, only where a camera
        // looks (graphicsModule != null), rolled with the room's cosmetic random
        void drop_shells(sys::game_state& state, dcon::creature_id c, uint32_t k, vec2 momentum) {
            auto& w = state.world;
            auto const room = room_of(state, c);
            if (!state.viewed(room))
                return;
            auto rng = rw::random(state.seed, state.tick, rw::ids(uint32_t(room.value), rw::ids(uint32_t(c.value), k)));
            auto const hit = chunk(state, c, k);
            float const rad = w.body_chunk_get_rad(hit);
            int const count = is(state, c, variant::red) ? 3 : 1;
            for (int j = 0; j < count; ++j) {
                float const f = mathf::lerp(0.7f, 1.6f, rng.value());
                vec2 const r = rnv(rng);
                vec2 const vel = momentum * f + r * rng.value() * (j == 0 ? 3.f : 6.f);
                particles::centipede_shell(state, room, w.body_chunk_get_pos(hit), vel, w.centipede_data_get_hue(data(state, c)),
                    w.centipede_data_get_graphics(data(state, c)).saturation, rad * 1.8f * 0.071428575f * 1.2f, rad * 1.3f * 0.09090909f * 1.2f);
            }
        }
    }

    pathing::movement const& movement(sys::game_state const& state, dcon::creature_id c) noexcept {
        switch (pl(state, c).variant) {
        case variant::wing: return pl(state, c).flying ? pathing::centiwing_fly : pathing::centiwing_crawl;
        case variant::aqua: return pathing::aquacenti;
        default: return pathing::climber;
        }
    }

    bool has_shell(sys::game_state const& state, dcon::creature_id c, uint32_t k) noexcept {
        return k < 32 && (state.world.centipede_data_get_shells(data(state, c)) >> k & 1u) != 0;
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        auto const o = body(state, c);
        auto const d = data(state, c);
        uint32_t const n = chunk_count(state, c);
        float const size = pl(state, c).size;
        variant const v = pl(state, c).variant;
        auto rng = rw::random(state.seed, state.tick, uint32_t(c.value));
        if (v == variant::aqua)
            underwater_shock(state, c, room);
        // the aquapede's Glower: lit while it is calm
        if (v == variant::aqua) {
            float& glow = w.centipede_data_get_glow(d);
            if (w.creature_get_stun(c) <= 0 && pl(state, c).shock_charge < 0.2f && consious(state, c))
                glow = mathf::min(0.5f, glow + 0.2f);
            else
                glow = mathf::max(0.f, glow - 0.05f);
        }
        // a baby centipede in a slugcat's hand charges up and shocks it
        if (v == variant::small && !w.creature_get_dead(c)) {
            if (auto const p = holder(state, c)) {
                float& charge = pl(state, c).shock_charge;
                charge += 0.016666668f;
                for (uint32_t i = 0; i < n; ++i) {
                    vec2 const r = rnv(rng);
                    w.body_chunk_get_vel(chunk(state, c, i)) += r * rng.value() * 2.f;
                }
                if (charge >= 1.f) {
                    shock(state, c, player::creature_of(state, p));
                    stun(state, c, 14);
                    for (uint32_t i = 0; i < n; ++i) {
                        vec2 const r = rnv(rng);
                        w.body_chunk_get_vel(chunk(state, c, i)) += r * rng.value() * 7.f;
                    }
                }
            }
        }
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
        float const stiff = mathf::lerp(1.f, 6.f, pl(state, c).shock_charge) * mathf::lerp(1.f, 2.f, size);
        for (uint32_t i = 0; i + 2 < n; ++i) {
            vec2 const a = pos(state, c, i), b = pos(state, c, i + 2);
            w.body_chunk_get_vel(chunk(state, c, i)) += custom::dir_vec(b, a) * stiff;
            w.body_chunk_get_vel(chunk(state, c, i + 2)) += custom::dir_vec(a, b) * stiff;
        }
        for (int g = 0; g < 2; ++g)
            update_grasp(state, c, g);

        float const health = w.creature_get_health(c);
        if (w.creature_get_dead(c) && !holder(state, c)) {
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
                    if (rng.value() > health * 2.f) {
                        vec2 const r = rnv(rng);
                        float const f = mathf::pow(rng.value(), custom::lerp_map(health, 0.75f, 0.f, 3.f, 0.1f, 2.f));
                        w.body_chunk_get_vel(chunk(state, c, i)) += r * f * 4.f * mathf::inverse_lerp(0.75f, 0.f, health);
                    }
        }
        if (consious(state, c)) {
            if (aqua_swimming(state, c, room))
                act(state, c, room);
            else if (physics::object_submersion(state, room, o) > 0.5f && !pl(state, c).flying)
                swim(state, c, room);
            else
                act(state, c, room);
        }
        pl(state, c).shock_charge = mathf::max(0.f, pl(state, c).shock_charge - 0.008333334f);
        w.centipede_data_set_light_flash(d, mathf::max(0.f, w.centipede_data_get_light_flash(d) - 0.1f));
        if (pl(state, c).moving)
            w.centipede_data_get_walk_cycle(d) += 0.25f;
    }

    // Centipede.HeadIndex: the free end when holding on with one, else the leading one
    uint32_t head_index(sys::game_state const& state, dcon::creature_id c) noexcept {
        uint32_t const last = chunk_count(state, c) - 1;
        auto const& g = state.world.creature_get_grasps(c).obj;
        if (g[0] && !g[1])
            return last;
        if (!g[0] && g[1])
            return 0;
        return pl(state, c).body_direction ? 0 : last;
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 at, float size, variant v) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::centipede);
        auto const d = data(state, c);
        auto rng = rw::random(state.seed, 0, uint32_t(c.value));
        // AbstractCreature.ID.RandomSeed: Centipede.GenerateSize and the
        // graphics' layout roll from it
        int32_t seed = int32_t(rng.next() & 0x7FFFFFFFu);
        if (state.spawn_seed >= 0)
            seed = state.spawn_seed;
        {
            rw::unity_random r{seed};
            float const roll = mathf::lerp(0.f, 1.f, mathf::pow(r.value(), 1.5f));
            switch (v) {
            case variant::centipede: size = size < 0.f ? roll : mathf::clamp01(size); break;
            case variant::small: size = 0.f; break;
            case variant::wing: size = mathf::lerp(0.5f, 0.65f, r.value()); break;
            case variant::aqua: size = 0.9f + (1.8f - 0.9f) * r.value(); break;
            case variant::red: size = 1.f; break;
            }
        }
        physics::object_desc desc;
        desc.air_friction = 0.999f;
        desc.gravity = 0.9f;
        desc.bounce = 0.1f;
        desc.surface_friction = 0.4f;
        desc.water_friction = 0.96f;
        desc.buoyancy = v == variant::aqua ? 0.78f : 1.05f;
        auto const o = physics::create_object(state, room, desc);
        int const n = v == variant::red ? 18 : v == variant::small ? 5 : int(mathf::lerp(7.f, 17.f, size));
        auto const view = world::get_room_view(state, room);
        auto const& move = v == variant::wing ? pathing::centiwing_fly : v == variant::aqua ? pathing::aquacenti : pathing::climber;
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
                    int const score = dx * dx + dy * dy + (pathing::accessible(view, t, move) ? 0 : 50);
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
            float rad = mathf::lerp(mathf::lerp(2.f, 3.5f, size), mathf::lerp(4.f, 6.5f, size),
                mathf::pow(mathf::clamp01(std::sin(mathf::pi * t)), mathf::lerp(0.7f, 0.3f, size)));
            if (v == variant::red)
                rad += 1.5f;
            if (v == variant::wing)
                rad = mathf::lerp(rad, mathf::lerp(2.f, 3.5f, size), 0.4f);
            if (v == variant::small)
                rad = mathf::lerp(1.5f, 3.f, mathf::pow(mathf::clamp01(std::sin(mathf::pi * t)), 0.5f));
            float mass = mathf::lerp(0.042857144f, 0.32352942f, mathf::pow(size, 1.4f));
            // a red centipede's armour weighs it down
            if (v == variant::red)
                mass += 0.02f + 0.08f * mathf::clamp01(std::sin(mathf::inverse_lerp(0.f, float(n - 1), float(i)) * mathf::pi));
            // laid out sideways from the spawn point, folding back at walls
            if (i > 0) {
                vec2 const next = p + vec2{8.f, 0.f};
                if (!view.get_tile(next).solid())
                    p = next;
            }
            chunks.push_back(physics::add_chunk(state, o, p, rad, mass));
        }
        // CentipedeState.shells: all but the odd one on, the small one has none
        uint32_t shells = 0;
        if (v != variant::small)
            for (int i = 0; i < n; ++i)
                if (rng.value() < 0.985f)
                    shells |= 1u << i;
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j) {
                float const mi = w.body_chunk_get_mass(chunks[size_t(i)]), mj = w.body_chunk_get_mass(chunks[size_t(j)]);
                // the aquapede's body gives (elasticity 0.3)
                physics::add_connection(state, o, chunks[size_t(i)], chunks[size_t(j)],
                    w.body_chunk_get_rad(chunks[size_t(i)]) + w.body_chunk_get_rad(chunks[size_t(j)]),
                    physics::connection_type::push, v == variant::aqua ? 0.3f : 1.f, mj / (mi + mj));
            }
        w.try_create_creature_body(c, o);
        auto& x = pl(state, c);
        x.size = size;
        x.variant = v;
        x.flying = v == variant::wing ? 1 : 0;
        w.centipede_data_set_seed(d, seed);
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        x.body_direction = rng.value() < 0.5f;
        x.moving = 0;
        x.move_to_pos = at;
        x.change_dir_counter = 0;
        x.direction_change_block = 0;
        x.no_follow_con_counter = 0;
        w.creature_set_grasps(c, creature::grasp_pair{});
        x.shock_charge = 0.f;
        x.double_grab_charge = 0.f;
        x.shock_give_up_counter = 0;
        w.creature_set_behavior(c, uint8_t(centipede_behavior::idle));
        x.run = 0.f;
        x.excitement = 0.f;
        w.creature_set_current_utility(c, 0.f);
        ivec2 const tile = room_view::get_tile_position(at);
        w.creature_set_destination(c, tile);
        x.temp_idle_pos = tile;
        x.annoying_collisions = 0;
        tracker::clear(state, c);
        w.centipede_data_set_idle_counter(d, 0);
        w.centipede_data_set_seen_age(d, 0);
        w.centipede_data_set_light_flash(d, 0.f);
        w.centipede_data_set_walk_cycle(d, 0.f);
        // (a centiwing starts out flying, and lands on its first Act unless
        // it finds nothing to hold on to)
        w.centipede_data_set_want_to_fly(d, 0);
        w.centipede_data_set_fly_mode_counter(d, 0);
        w.centipede_data_set_wings_started_up(d, 0.f);
        w.centipede_data_set_outside_level(d, 0);
        w.centipede_data_set_body_wave(d, 0.f);
        w.centipede_data_set_shells(d, shells);
        w.centipede_data_set_shell_just_fell_off(d, -1);
        w.centipede_data_set_bites(d, 5);
        w.centipede_data_set_forbidden_idle_pos(d, ivec2{-1000, -1000});
        w.centipede_data_set_glow(d, 0.f);
        w.centipede_data_set_underwater_shock(d, -1);
        centipede_graphics::reset(state, c);
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        lose_all_grasps(state, c);
        release(state, c);
        creature::erase(state, c);
    }

    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) {
        // the slugcat holding the baby keeps it in its hand
        return !creature::holds(state.world, t, c);
    }

    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::creature_id p, uint32_t j) {
        auto& w = state.world;
        if (!consious(state, c))
            return;
        // Centipede.Collide: noticed, annoyed, and grabbed with an end
        uint32_t const n = chunk_count(state, c);
        hook_ctx hx{state, c, room};
        tracker::see(state, c, room, p, make_hooks(hx));
        if (creature::type_of(w, p) != creature::type::centipede)
            annoying_collision(state, c, p);
        if (!want_to_shock(state, c, p))
            return;
        for (int g = 0; g < 2; ++g) {
            if (i != (g == 0 ? 0 : n - 1) || grasped(state, c, g))
                continue;
            // not what holds it; a centiwing doesn't grab over a pit (and
            // only half the time anyway)
            bool ok = !creature::holds(w, p, c);
            if (is(state, c, variant::wing)) {
                auto const room = world::get_room_view(state, room_of(state, c));
                if (lizard::ai::fall_risk_tile(room, room_view::get_tile_position(pos(state, c, i))).y < 0
                || rw::random(state.seed, state.tick, rw::ids(rw::ids(uint32_t(c.value), uint32_t(p.value)), rw::ids(i, j * 2u + uint32_t(g)))).value() < 0.5f)
                    ok = false;
            }
            if (pl(state, c).shock_give_up_counter > 0)
                ok = false;
            if (ok)
                grab(state, c, g, p, j);
        }
    }

    void collide_kin(sys::game_state& state, dcon::creature_id c, uint32_t ci, dcon::creature_id other, uint32_t oi) {
        // Centipede.Collide with a bigger centipede: it looks for another
        // idle spot (both ways: the pair's collision is reported once)
        auto const room = world::get_room_view(state, room_of(state, c));
        // the pair and its chunks in the key: each collision of a tick rolls apart
        auto rng = rw::random(state.seed, state.tick, rw::ids(rw::ids(uint32_t(c.value), uint32_t(other.value)), rw::ids(ci, oi)));
        if (consious(state, c) && pl(state, other).size > pl(state, c).size)
            check_random_idle_pos(state, c, room, rng);
        if (consious(state, other) && pl(state, c).size > pl(state, other).size)
            check_random_idle_pos(state, other, room, rng);
    }

    void violence(sys::game_state& state, dcon::creature_id c, uint32_t k, vec2 momentum, float damage, float stun_bonus, creature::damage_type type) {
        auto& w = state.world;
        // the hit chunk in the key: two hits of a tick roll apart
        auto rng = rw::random(state.seed, state.tick, rw::ids(uint32_t(c.value), k));
        auto const d = data(state, c);
        uint32_t const n = chunk_count(state, c);
        variant const v = pl(state, c).variant;
        float const size = pl(state, c).size;
        float const health = w.creature_get_health(c);
        bool const directed = momentum != vec2{0.f, 0.f};
        int8_t& fell_off = w.centipede_data_get_shell_just_fell_off(d);
        fell_off = -1;
        // Centipede.Violence: a hit shell may break off; without its shell
        // the chunk is hurt more
        if (v != variant::small && k < n) {
            if (has_shell(state, c, k)) {
                if (directed && (v != variant::wing || rng.value() < 0.85f)) {
                    bool off = true;
                    if (type == creature::damage_type::electric) {
                        off = false;
                    } else if (v == variant::red && damage >= 0.95f) {
                        off = true;
                    } else if (rng.value() < mathf::lerp(0.9f, 0.1f, mathf::pow(size, 0.4f))) {
                        off = false;
                    } else if (damage < mathf::lerp(0.4f, 1.1f, rng.value())) {
                        off = false;
                    } else if (rng.value() < 0.85f) {
                        uint32_t on = 0;
                        for (uint32_t i = 0; i < n; ++i)
                            on += has_shell(state, c, i) ? 1 : 0;
                        off = float(on) / float(n) > (w.creature_get_dead(c) ? 0.33f : 0.55f);
                    }
                    if (off) {
                        fell_off = int8_t(k);
                        w.centipede_data_get_shells(d) &= ~(1u << k);
                        drop_shells(state, c, k, momentum);
                        if (v == variant::red)
                            state.play_sound(sound::sound_id::red_centipede_shield_falloff, room_of(state, c), pos(state, c, k));
                    }
                }
            } else {
                damage *= 1.f + 0.45f * mathf::pow(size, 0.65f);
                if (v == variant::red)
                    stun_bonus *= 1.5f;
            }
        }
        if (v == variant::red) {
            // the armour: what hits a shell (or the one just knocked off)
            // glances off in sparks
            if (k < n && (has_shell(state, c, k) || int32_t(k) == fell_off)) {
                if (type == creature::damage_type::bite || type == creature::damage_type::stab)
                    type = creature::damage_type::blunt;
                int32_t const sparks = int32_t(mathf::min(mathf::max((damage * 15.f + stun_bonus) / base_stun_resistance / 2.f,
                    float(int32_t(damage / base_damage_resistance(v) * 15.f))), 25.f));
                if (directed && sparks > 5) {
                    auto const room = room_of(state, c);
                    if (state.viewed(room)) {
                        auto cr = rw::random(state.seed, state.tick, rw::ids(uint32_t(room.value), rw::ids(uint32_t(c.value), k)));
                        vec2 const at = pos(state, c, k);
                        float const speed = rw::magnitude(momentum) * 10.f;
                        for (int32_t s = 0; s < sparks; ++s) {
                            vec2 const off = custom::deg_to_vec(cr.value() * 360.f) * 5.f * cr.value();
                            vec2 const dir = custom::deg_to_vec(cr.value() * 360.f) * mathf::lerp(0.2f, 0.4f, cr.value()) * speed;
                            particles::spawn(state, room, particles::kind::spark, at + off, rw::normalized(momentum) * -1.f + dir, 1.5f, 0.3f, rw::vec3{1.f, 1.f, 1.f});
                        }
                        particles::spawn(state, room, particles::kind::flash, at, vec2{0.f, 0.f}, 10.f, 0.25f, rw::vec3{1.f, 1.f, 1.f}, 1.f);
                    }
                    state.play_sound(sound::sound_id::lizard_head_shield_deflect, room_of(state, c), pos(state, c, k));
                }
                damage *= 0.05f;
                stun_bonus *= 0.2f;
            } else if (type == creature::damage_type::explosion) {
                damage *= 1.1f;
            }
            // (the spears already stuck in the chunk aren't ported)
            if (k == 0 || k == n - 1)
                damage *= 0.5f;
            stun_bonus *= 1.f - health * 0.5f;
        } else if (v == variant::aqua) {
            stun_bonus *= 2.f;
        } else {
            stun_bonus *= 1.f - health;
        }
        float scale = size;
        if (v == variant::aqua)
            scale /= 2.f;
        damage *= mathf::lerp(1.3f, 0.075f, mathf::pow(scale, 0.5f));
        // Creature.Violence
        auto const hit = chunk(state, c, std::min(k, n - 1));
        w.body_chunk_get_vel(hit) += rw::clamp_magnitude(momentum / w.body_chunk_get_mass(hit), 10.f);
        float dmg = damage / base_damage_resistance(v);
        float st = (damage * 30.f + stun_bonus) / base_stun_resistance;
        st *= 1.5f + mathf::inverse_lerp(0.5f, 0.f, health) * rng.value();
        if (type == creature::damage_type::electric) {
            dmg /= electric_resistance;
            st /= electric_resistance;
        }
        stun(state, c, int32_t(st));
        w.creature_set_health(c, health - dmg);
        // only the small one dies of a single blow (instantDeathDamageLimit 1)
        if (v == variant::small && dmg >= 1.f)
            die(state, c);
    }

    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id p) {
        for (int g = 0; g < 2; ++g)
            if (grasped(state, c, g) == p)
                release(state, c, g);
    }

    dcon::creature_id held_in_hand(sys::game_state const& state, dcon::player_id p, uint32_t hand) noexcept {
        if (!p)
            return dcon::creature_id{};
        auto const c = creature::grasped(state.world, player::creature_of(state, p), hand);
        return c && creature::type_of(state.world, c) == creature::type::centipede ? c : dcon::creature_id{};
    }

    dcon::creature_id pickup_candidate(sys::game_state const& state, dcon::player_id p, room_view const& room, float& score) {
        auto const& w = state.world;
        vec2 const p0 = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
        int32_t const flip = w.player_get_flip_direction(p);
        dcon::creature_id result;
        score = 3.4e38f;
        rooms::for_each_creature(state, room.id, creature::type::centipede, [&](dcon::creature_id c) {
            // Player.Grabability: OneHand for a small one, the others can't
            // be picked up alive (and dead ones are dragged: not ported)
            if (!is(state, c, variant::small) || creature::held(state, c))
                return;
            uint32_t const n = chunk_count(state, c);
            for (uint32_t i = 0; i < n; ++i) {
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

    void grab(sys::game_state& state, dcon::player_id p, dcon::creature_id c, uint32_t hand) {
        // SlugcatGrab: an IPlayerEdible by its chunk 0, in the slugcat's grasp
        auto const pc = player::creature_of(state, p);
        creature::grab(state, pc, hand, c, 0);
        // it can't hold the slugcat holding it
        for (int g = 0; g < 2; ++g)
            if (grasped(state, c, g) == pc)
                release(state, c, g);
    }

    void release(sys::game_state& state, dcon::creature_id c) {
        uint32_t g = 0;
        if (auto const h = creature::holder(state, c, &g))
            creature::release_grasp(state, h, g);
    }

    void release_player(sys::game_state& state, dcon::player_id p) {
        for (uint32_t hand = 0; hand < 2; ++hand)
            if (auto const c = held_in_hand(state, p, hand))
                creature::release_grasp(state, player::creature_of(state, p), hand);
    }

    void carry_update(sys::game_state& state, dcon::player_id p, uint32_t hand, vec2 hand_pos) {
        auto& w = state.world;
        auto const c = held_in_hand(state, p, hand);
        if (!c)
            return;
        auto const k = chunk(state, c, 0);
        w.body_chunk_set_last_pos(k, w.body_chunk_get_pos(k));
        w.body_chunk_set_pos(k, hand_pos);
        w.body_chunk_set_vel(k, w.body_chunk_get_vel(player::body_chunk(state, p, 0)));
    }

    void follow_player(sys::game_state& state, dcon::player_id p, dcon::room_id room, bool spit_out) {
        auto& w = state.world;
        for (uint32_t hand = 0; hand < 2; ++hand) {
            auto const c = held_in_hand(state, p, hand);
            if (!c)
                continue;
            w.physical_object_set_room_from_object_location(body(state, c), room);
            if (!spit_out)
                continue;
            vec2 const at = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
            uint32_t const n = chunk_count(state, c);
            for (uint32_t i = 0; i < n; ++i) {
                physics::hard_set_position(state, chunk(state, c, i), at);
                w.body_chunk_set_vel(chunk(state, c, i), w.body_chunk_get_vel(player::body_chunk(state, p, 0)));
            }
            centipede_graphics::reset_positions(state, c);
        }
    }

    bool bit_by_player(sys::game_state& state, dcon::creature_id c, dcon::player_id p) {
        auto& w = state.world;
        auto const d = data(state, c);
        uint8_t& bites = w.centipede_data_get_bites(d);
        if (bites > 0)
            --bites;
        die(state, c);
        vec2 const mouth = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
        state.play_sound(bites == 0 ? sound::sound_id::slugcat_eat_centipede : sound::sound_id::slugcat_bite_centipede, room_of(state, c),
            pos(state, c, chunk_count(state, c) / 2));
        // firstChunk.MoveFromOutsideMyUpdate to the grabber's mouth
        auto const k = chunk(state, c, 0);
        w.body_chunk_set_pos(k, mouth);
        w.body_chunk_set_last_pos(k, mouth);
        return bites < 1;
    }
}
