#include <cmath>

#include "items.hpp"
#include "outposts.hpp"
#include "cicada.hpp"
#include "creature.hpp"
#include "explosions.hpp"
#include "particles.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "player_graphics.hpp"
#include "rooms.hpp"

namespace items {
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    using world::weapon_mode;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // Weapon.exitThrownModeSpeed
        constexpr float exit_thrown_mode_speed = 30.f;

        // Weapons fly and hurt; the rest (food, pearls) is only tossed
        bool is_weapon(world::item_type t) {
            using world::item_type;
            return t == item_type::rock || t == item_type::spear || t == item_type::explosive_spear || t == item_type::electric_spear
                || t == item_type::bomb || t == item_type::flare_bomb || t == item_type::lilly_puck;
        }

        // what goes off after the update loop (items can't be destroyed while
        // it walks them), per thread: a room's update runs on one thread
        struct pending_blast {
            dcon::item_id item;
            vec2 at;
        };
        std::vector<pending_blast>& blasts() {
            static thread_local std::vector<pending_blast> v;
            return v;
        }
        void detonate(sys::game_state& state, dcon::item_id it, vec2 at) {
            for (auto const& b : blasts())
                if (b.item == it)
                    return;
            blasts().push_back({it, at});
        }

        // Custom.AimFromOneVectorToAnother(0, v)
        float aim(vec2 v) {
            return std::atan2(v.x, v.y) / 6.2831855f * 360.f;
        }

        dcon::room_id room_of(sys::game_state const& state, dcon::physical_object_id o) {
            return state.world.physical_object_get_room_from_object_location(o);
        }
        // the item's room's UnityEngine.Random
        rw::rng& item_rng(sys::game_state& state, dcon::item_id it) {
            return state.room_random(room_of(state, body(state, it)));
        }

        float room_gravity(sys::game_state const& state, dcon::room_id room) {
            return room ? state.world.room_get_gravity(room) : 1.f;
        }

        // SlugcatStats.throwingSkill
        int32_t throwing_skill(::player::slugcat_name n) {
            switch (n) {
            case ::player::slugcat_name::yellow:
            case ::player::slugcat_name::saint:
                return 0;
            case ::player::slugcat_name::red:
            case ::player::slugcat_name::gourmand:
            case ::player::slugcat_name::artificer:
            case ::player::slugcat_name::spear:
                return 2;
            default:
                return 1;
            }
        }

        bool is_spear(sys::game_state const& state, dcon::item_id it) {
            auto const t = state.world.item_get_type(it);
            return t == world::item_type::spear || t == world::item_type::explosive_spear || t == world::item_type::electric_spear;
        }

        // Weapon.SetRandomSpin / Spear.SetRandomSpin
        void set_random_spin(sys::game_state& state, dcon::item_id it) {
            float const g = room_gravity(state, room_of(state, body(state, it)));
            if (is_spear(state, it)) {
                float const sign = item_rng(state, it).value() < 0.5f ? -1.f : 1.f;
                state.world.item_set_rotation_speed(it, sign * mathf::lerp(50.f, 150.f, item_rng(state, it).value()) * mathf::lerp(0.05f, 1.f, g));
                state.world.item_set_spinning(it, 1);
                return;
            }
            state.world.item_set_rotation_speed(it, mathf::lerp(-100.f, 100.f, item_rng(state, it).value()) * mathf::lerp(0.05f, 1.f, g));
        }

        // a tile's flags in the data container (null out of bounds)
        uint8_t* tile_flags(sys::game_state& state, dcon::room_id room, ivec2 t) {
            auto& w = state.world;
            int32_t const width = w.room_get_width(room);
            if (t.x < 0 || t.y < 0 || t.x >= width || t.y >= w.room_get_height(room))
                return nullptr;
            dcon::tile_id const id{dcon::tile_id::value_base_t(w.room_get_first_tile(room).id.index() + t.y * width + t.x)};
            return &w.tile_get_flags(id);
        }

        // the tiles a stuck spear lends its beam to: its own and the free
        // neighbours along it
        template<typename F>
        void for_each_pole_tile(sys::game_state& state, dcon::item_id it, F&& f) {
            auto& w = state.world;
            auto const room = room_of(state, body(state, it));
            auto const view = world::get_room_view(state, room);
            bool const vertical = w.item_get_stuck_cycles(it) < 0;
            ivec2 const center = room_view::get_tile_position(w.item_get_stuck_pos(it));
            for (int32_t k = -1; k < 2; ++k) {
                ivec2 const t = center + (vertical ? ivec2{0, k} : ivec2{k, 0});
                if (k != 0 && view.get_tile(t).solid())
                    continue;
                if (uint8_t* flags = tile_flags(state, room, t))
                    f(*flags, uint8_t(1u << (k + 1)), vertical ? uint8_t(world::tile_vertical_beam) : uint8_t(world::tile_horizontal_beam));
            }
        }

        // Spear.resetHorizontalBeamState
        void reset_beam_state(sys::game_state& state, dcon::item_id it) {
            auto& w = state.world;
            if (w.item_get_has_stuck(it)) {
                ++w.room_get_geometry_version(room_of(state, body(state, it)));
                uint8_t const was = w.item_get_was_beam(it);
                for_each_pole_tile(state, it, [&](uint8_t& flags, uint8_t bit, uint8_t beam) {
                    flags = uint8_t((flags & ~beam) | ((was & bit) ? beam : 0));
                });
            }
            w.item_set_has_beam_state(it, 0);
            w.item_set_stuck_cycles(it, 0);
        }

        // Weapon.ChangeMode (+ Spear.ChangeMode)
        void change_mode(sys::game_state& state, dcon::item_id it, weapon_mode m) {
            auto& w = state.world;
            if (w.item_get_mode(it) == m)
                return;
            if (is_spear(state, it)) {
                if (m == weapon_mode::stuck_in_wall) {
                    if (w.item_get_stuck_cycles(it) == 0)
                        w.item_set_stuck_cycles(it, int8_t(item_rng(state, it).range(3, 7) * (w.item_get_throw_dir(it).y != 0 ? -1 : 1)));
                    bool const vertical = w.item_get_stuck_cycles(it) < 0;
                    auto const view = world::get_room_view(state, room_of(state, body(state, it)));
                    vec2 const stuck = w.item_get_stuck_pos(it);
                    for (int32_t i = -1; i < 2; i += 2) {
                        vec2 const probe = stuck + (vertical ? vec2{0.f, 20.f * float(i)} : vec2{20.f * float(i), 0.f});
                        if (!view.get_tile(probe).solid()) {
                            // points into the wall, half a tile out of it
                            vec2 const rot = vertical ? vec2{0.f, float(-i)} : vec2{float(-i), 0.f};
                            w.item_set_rotation(it, rot);
                            w.item_set_add_poles(it, 1);
                            w.item_set_stuck_pos(it, room_view::middle_of_tile(stuck) - rot * 5.f);
                            break;
                        }
                    }
                    w.item_set_rotation_speed(it, 0.f);
                }
                if (m != weapon_mode::free)
                    w.item_set_spinning(it, 0);
                if (m != weapon_mode::stuck_in_wall)
                    w.item_set_has_stuck(it, 0);
            }
            if (m != weapon_mode::thrown)
                w.item_set_throw_mode_frames(it, -1);
            auto const c = chunk(state, it);
            w.body_chunk_set_collide_with_terrain(c, m == weapon_mode::free || m == weapon_mode::thrown);
            w.body_chunk_set_go_through_floors(c, true);
            // (DataPearl is a PlayerCarryableItem: no Weapon spin)
            if (m == weapon_mode::free && w.item_get_type(it) != world::item_type::data_pearl)
                set_random_spin(state, it);
            w.item_set_mode(it, m);
        }

        // Weapon.HitWall
        void hit_wall(sys::game_state& state, dcon::item_id it) {
            auto const c = chunk(state, it);
            state.play_sound(is_spear(state, it) ? sound::sound_id::spear_bounce_off_wall : sound::sound_id::rock_hit_wall,
                room_of(state, body(state, it)), state.world.body_chunk_get_pos(c));
            set_random_spin(state, it);
            change_mode(state, it, weapon_mode::free);
            state.world.item_set_forbidden_to_player(it, 10);
        }

        // Creature.Grasp: a hand of a player and what it holds
        struct grasp {
            dcon::item_id item;
            uint32_t hand = 0;
            explicit operator bool() const noexcept { return bool(item); }
        };

        void set_hand(sys::game_state& state, dcon::player_id p, uint32_t hand, dcon::item_id it) {
            if (hand == 0)
                state.world.player_set_grasp0(p, it);
            else
                state.world.player_set_grasp1(p, it);
        }

        grasp grasp_in_hand(sys::game_state const& state, dcon::player_id p, uint32_t hand) {
            auto const it = hand == 0 ? state.world.player_get_grasp0(p) : state.world.player_get_grasp1(p);
            return grasp{it, hand};
        }

        // Creature.ReleaseGrasp
        void release_grasp(sys::game_state& state, dcon::player_id p, grasp g) {
            auto& w = state.world;
            auto const o = body(state, g.item);
            uint8_t& n = w.physical_object_get_grabbed_by_count(o);
            if (n > 0)
                --n;
            set_hand(state, p, g.hand, dcon::item_id{});
            w.item_set_grabbed_by(g.item, dcon::player_id{});
        }

        // Player.ThrowDirection
        int32_t throw_direction(sys::game_state const& state, dcon::player_id p) {
            auto const& w = state.world;
            vec2 const p0 = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
            vec2 const p1 = w.body_chunk_get_pos(player::body_chunk(state, p, 1));
            if (w.player_get_body_mode(p) != ::player::body_mode_index::default_ && std::fabs(p0.x - p1.x) >= 15.f)
                return int32_t(mathf::sign(p0.x - p1.x));
            int32_t const x = w.player_get_input(p)[0].x;
            return x == 0 ? w.player_get_flip_direction(p) : x;
        }

        // Weapon.Thrown (+ Rock.Thrown)
        void thrown(sys::game_state& state, dcon::item_id it, dcon::player_id by, vec2 thrown_pos, ivec2 throw_dir, float frc) {
            auto& w = state.world;
            auto const c = chunk(state, it);
            vec2 const main_vel = w.body_chunk_get_vel(player::body_chunk(state, by, 0));
            w.item_set_thrown_by(it, by);
            w.item_set_thrown_pos(it, thrown_pos);
            w.item_set_throw_dir(it, throw_dir);
            w.item_set_change_dir_counter(it, 3);
            // BodyChunk.MoveFromOutsideMyUpdate
            w.body_chunk_set_last_pos(c, w.body_chunk_get_pos(c));
            w.body_chunk_set_pos(c, thrown_pos);
            vec2& vel = w.body_chunk_get_vel(c);
            if (!is_weapon(w.item_get_type(it))) {
                // PlayerCarryableItem: tossed, not thrown
                vel = main_vel * 0.5f + vec2{float(throw_dir.x) * 6.f, 4.f + float(throw_dir.y) * 6.f};
                change_mode(state, it, weapon_mode::free);
                w.item_set_forbidden_to_player(it, 10);
                state.play_sound(sound::sound_id::slugcat_throw_rock, room_of(state, body(state, it)), thrown_pos, 0.5f);
                return;
            }
            if (throw_dir.x != 0) {
                vel.y = main_vel.y * 0.5f;
                vel.x = main_vel.x * 0.2f;
                vel.x += float(throw_dir.x) * 40.f * frc;
                vel.y += is_spear(state, it) ? 1.5f : 3.f;
            } else {
                if (throw_dir.y == 0) {
                    change_mode(state, it, weapon_mode::free);
                    return;
                }
                vel.x = main_vel.x * 0.5f;
                vel.y = float(throw_dir.y) * 40.f * frc;
            }
            w.item_set_override_exit_thrown_speed(it, frc >= 1.f ? 0.f : mathf::min(exit_thrown_mode_speed, frc * 20.f));
            change_mode(state, it, weapon_mode::thrown);
            w.item_set_rotation(it, rw::to_vec2(throw_dir));
            w.item_set_rotation_speed(it, 0.f);
            w.item_set_always_stick(it, 0);
            state.play_sound(is_spear(state, it) ? sound::sound_id::slugcat_throw_spear : sound::sound_id::slugcat_throw_rock,
                room_of(state, body(state, it)), thrown_pos);
        }

        // Player.ThrowObject, for weapons
        void throw_object(sys::game_state& state, dcon::player_id p, grasp g, room_view const& room) {
            auto& w = state.world;
            auto const it = g.item;
            auto const c0 = player::body_chunk(state, p, 0);
            auto const c1 = player::body_chunk(state, p, 1);
            auto const& in = w.player_get_input(p)[0];
            // AerobicIncrease(0.75f)
            w.player_set_aerobic_level(p, mathf::min(1.f, w.player_get_aerobic_level(p) + 0.75f / 9.f));

            ivec2 dir{throw_direction(state, p), 0};
            if (w.player_get_animation(p) == ::player::animation_index::flip && in.y < 0 && in.x == 0)
                dir = ivec2{0, -1};
            vec2 pos = w.body_chunk_get_pos(c0) + rw::to_vec2(dir) * 10.f + vec2{0.f, 4.f};
            if (room.get_tile(pos).solid())
                pos = w.body_chunk_get_pos(c0);
            thrown(state, it, p, pos, dir, 1.f);

            auto const ic = chunk(state, it);
            int32_t const skill = throwing_skill(w.player_get_stats(p).name);
            // Player.ThrownSpear: clumsy throwers' spears fall out of the air sooner
            if (is_spear(state, it) && skill == 0) {
                w.item_set_throw_mode_frames(it, 18);
                w.body_chunk_get_vel(ic).x *= 0.77f;
            }
            int32_t const roll_dir = w.player_get_roll_direction(p);
            int32_t const roll_counter = w.player_get_roll_counter(p);
            if (w.player_get_animation(p) == ::player::animation_index::belly_slide && roll_counter > 8 && roll_counter < 15) {
                if (dir.x == roll_dir && skill > 0) {
                    w.body_chunk_get_vel(ic).x += float(dir.x) * 15.f;
                    // rocks and spears are heavy weapons
                    w.item_set_floor_bounce_frames(it, 30);
                    if (is_spear(state, it))
                        w.item_set_always_stick(it, 1);
                    w.body_chunk_set_go_through_floors(ic, false);
                    w.body_chunk_get_vel(ic).y -= 5.f;
                    w.item_set_change_dir_counter(it, 0);
                } else if (dir.x == -roll_dir && !w.player_get_long_belly_slide(p)) {
                    w.body_chunk_get_vel(ic).y += is_spear(state, it) ? 3.f : 5.f;
                    w.item_set_change_dir_counter(it, 0);
                    w.player_set_roll_counter(p, 8);
                    w.body_chunk_get_pos(c0).x += float(roll_dir) * 6.f;
                    w.body_chunk_get_pos(c1).x += float(roll_dir) * 6.f;
                    w.body_chunk_get_pos(c1).y += 17.f;
                    w.body_chunk_get_vel(c0).x += float(roll_dir) * 16.f;
                    w.body_chunk_get_vel(c1).x += float(roll_dir) * 16.f;
                    w.player_set_exit_belly_slide_counter(p, 0);
                    w.player_set_long_belly_slide(p, 1);
                }
            }
            w.body_chunk_get_vel(c0) += rw::to_vec2(dir) * 8.f;
            w.body_chunk_get_vel(c1) -= rw::to_vec2(dir) * 4.f;

            player_graphics::throw_object(state, p, g.hand, it);
            w.player_set_dont_grab_stuff(p, 15);
            // PlayerCarryableItem.Forbid
            w.item_set_forbidden_to_player(it, 40);
            release_grasp(state, p, g);
        }

        // Player.ThrowObject for a carried cicada: Player.TossObject, the
        // creature lobbed in the throw direction, then let go
        void toss_cicada(sys::game_state& state, dcon::player_id p, dcon::creature_id cc) {
            auto& w = state.world;
            auto const& in = w.player_get_input(p)[0];
            auto const o = cicada::body(state, cc);
            auto const room = room_of(state, o);
            // AerobicIncrease(0.75f)
            w.player_set_aerobic_level(p, mathf::min(1.f, w.player_get_aerobic_level(p) + 0.75f / 9.f));
            state.play_sound(sound::sound_id::slugcat_throw_creature, room, w.body_chunk_get_pos(cicada::chunk(state, cc, 0)));
            float mass = 0.f, own = 0.f;
            for (uint32_t i = 0; i < 2; ++i) {
                mass += w.body_chunk_get_mass(cicada::chunk(state, cc, i));
                own += w.body_chunk_get_mass(player::body_chunk(state, p, i));
            }
            float ang = 45.f, force = 4.f;
            if (in.x != 0 && in.y == 0) {
                ang = custom::lerp_map(mass, 0.2f, 0.3f, 60.f, 50.f);
                force = custom::lerp_map(mass, 0.2f, 0.3f, 12.5f, 8.f, 2.f);
            } else if (in.x != 0 && in.y == 1) {
                ang = 25.f;
                force = 9.f;
            } else if (in.x == 0 && in.y == 1) {
                ang = 5.f;
                force = 8.f;
            }
            if (w.player_get_animation(p) == ::player::animation_index::flip && in.y < 0 && in.x == 0) {
                ang = 180.f;
                force = 8.f;
                physics::set_go_through_floors(state, o, true);
            }
            int32_t const dir = throw_direction(state, p);
            vec2 const p0 = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
            vec2 const p1 = w.body_chunk_get_pos(player::body_chunk(state, p, 1));
            float const edge = dir < 0 ? mathf::min(p0.x, p1.x) : mathf::max(p0.x, p1.x);
            vec2 const main_vel = w.body_chunk_get_vel(player::body_chunk(state, p, 0));
            for (uint32_t i = 0; i < 2; ++i) {
                auto const c = cicada::chunk(state, cc, i);
                vec2& cp = w.body_chunk_get_pos(c);
                vec2& cv = w.body_chunk_get_vel(c);
                // out in front of the slugcat
                if (mass < own * 2.f && dir != 0) {
                    if (dir < 0) {
                        cp.x = mathf::min(cp.x, edge - 8.f);
                        cv.x = mathf::min(cv.x, 0.f);
                    } else {
                        cp.x = mathf::max(cp.x, edge + 8.f);
                        cv.x = mathf::max(cv.x, 0.f);
                    }
                }
                if (cv.y < 0.f)
                    cv.y = 0.f;
                cv = rw::lerp(cv * 0.35f, main_vel, custom::lerp_map(mass, 0.2f, 0.5f, 0.6f, 0.3f));
                cv += custom::deg_to_vec(ang * float(dir)) * mathf::clamp(force / (mathf::lerp(mass, 0.4f, 0.2f) * 2.f), 4.f, 14.f);
            }
            w.player_set_dont_grab_stuff(p, 15);
            cicada::release(state, cc);
        }

        // Player.CanIPickThisUp, weapons: spears stuck in walls stay there,
        // except for Artificer (no Remix "dislodge spears" option)
        bool can_pick_up(sys::game_state const& state, dcon::player_id p, dcon::item_id it) {
            auto const m = state.world.item_get_mode(it);
            if (m == weapon_mode::stuck_in_wall)
                return state.world.player_get_stats(p).name == ::player::slugcat_name::artificer;
            return m == weapon_mode::free;
        }

        // Player.PickupCandidate over the items: the best one and its score (`best`)
        dcon::item_id pickup_candidate(sys::game_state const& state, dcon::player_id p, room_view const& room, float favor_spears, float& best) {
            auto const& w = state.world;
            vec2 const p0 = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
            int32_t const flip = w.player_get_flip_direction(p);
            dcon::item_id result;
            best = std::numeric_limits<float>::max();
            rooms::for_each_item(state, room.id, [&](dcon::item_id it) {
                if (w.item_get_forbidden_to_player(it) >= 1)
                    return;
                auto const c = chunk(state, it);
                vec2 const ip = w.body_chunk_get_pos(c);
                float const rad = w.body_chunk_get_rad(c);
                if (!custom::dist_less(p0, ip, rad + 40.f))
                    return;
                if (!custom::dist_less(p0, ip, rad + 20.f) && !room.visual_contact(p0, ip))
                    return;
                if (!can_pick_up(state, p, it))
                    return;
                float d = rw::distance(p0, ip);
                if (w.item_get_type(it) == world::item_type::spear)
                    d -= favor_spears;
                if ((ip.x < p0.x) == (flip < 0))
                    d -= 10.f;
                if (d < best) {
                    result = it;
                    best = d;
                }
            });
            return result;
        }

        // Player.SlugcatGrab / Creature.Grab + Weapon.Grabbed
        void grab(sys::game_state& state, dcon::player_id p, dcon::item_id it, uint32_t hand) {
            auto& w = state.world;
            if (w.item_get_grabbed_by(it) || grasp_in_hand(state, p, hand))
                return;
            set_hand(state, p, hand, it);
            w.item_set_grabbed_by(it, p);
            w.item_set_grabbed_hand(it, uint8_t(hand));
            ++w.physical_object_get_grabbed_by_count(body(state, it));
            // Spear.PickedUp: pulled out of a wall
            if (is_spear(state, it) && w.item_get_has_beam_state(it)) {
                reset_beam_state(state, it);
                w.item_set_vibrate(it, 20);
            }
            change_mode(state, it, weapon_mode::carried);
            w.item_set_thrown_by(it, dcon::player_id{});
            // picked off its stalk (DangleFruit, Mushroom, SlimeMold)
            w.item_set_stalk(it, std::nullopt);
            state.play_sound(is_spear(state, it) ? sound::sound_id::slugcat_pick_up_spear : sound::sound_id::slugcat_pick_up_rock,
                room_of(state, body(state, it)), w.body_chunk_get_pos(chunk(state, it)));
        }

        // ExplosiveSpear.RagAttachPos(1)
        vec2 rag_attach_pos(sys::game_state const& state, dcon::item_id it) {
            return state.world.body_chunk_get_pos(chunk(state, it)) + state.world.item_get_rotation(it) * 15.f;
        }

        // ExplosiveSpear.ResetRag
        void reset_rag(sys::game_state& state, dcon::item_id it, spear_extra& x) {
            vec2 const at = rag_attach_pos(state, it);
            for (int32_t i = 0; i < x.rag_count; ++i) {
                x.rag[i].v[0] = at;
                x.rag[i].v[1] = at;
                x.rag[i].v[2] = vec2{0.f, 0.f};
            }
        }

        // SharedPhysics.TerrainCollisionData for a point (rad 1, floors
        // stop it from above): HorizontalCollision, VerticalCollision,
        // SlopesVertically
        struct point_collision {
            vec2 pos;
            vec2 last_pos;
            vec2 vel;
            ivec2 contact{0, 0};
        };
        void point_terrain_collision(room_view const& room, point_collision& cd) {
            constexpr float rad = 1.f;
            // HorizontalCollision
            {
                ivec2 const tp = room_view::get_tile_position(cd.last_pos);
                bool const last_solid = room.get_tile(cd.last_pos).solid();
                int32_t const y = room_view::get_tile_position(vec2{0.f, cd.pos.y + rad - 1.f}).y;
                int32_t const y2 = room_view::get_tile_position(vec2{0.f, cd.pos.y - rad + 1.f}).y;
                bool done = false;
                if (cd.vel.x > 0.f) {
                    int32_t const x = room_view::get_tile_position(vec2{cd.pos.x + rad + 0.01f, 0.f}).x;
                    int32_t const x2 = room_view::get_tile_position(vec2{cd.last_pos.x + rad, 0.f}).x;
                    for (int32_t i = x2; i <= x && !done; ++i)
                        for (int32_t j = y2; j <= y && !done; ++j)
                            if (room.get_tile(i, j).solid() && !room.get_tile(i - 1, j).solid() && (tp.x < i || last_solid)) {
                                cd.pos.x = float(i) * 20.f - rad;
                                cd.vel.x = 0.f;
                                cd.contact.x = 1;
                                done = true;
                            }
                } else if (cd.vel.x < 0.f) {
                    int32_t const x = room_view::get_tile_position(vec2{cd.pos.x - rad - 0.01f, 0.f}).x;
                    int32_t const x2 = room_view::get_tile_position(vec2{cd.last_pos.x - rad, 0.f}).x;
                    for (int32_t i = x2; i >= x && !done; --i)
                        for (int32_t j = y2; j <= y && !done; ++j)
                            if (room.get_tile(i, j).solid() && !room.get_tile(i + 1, j).solid() && (tp.x > i || last_solid)) {
                                cd.pos.x = (float(i) + 1.f) * 20.f + rad;
                                cd.vel.x = 0.f;
                                cd.contact.x = -1;
                                done = true;
                            }
                }
            }
            // VerticalCollision (SolidFloor: floors hold what comes from above)
            {
                ivec2 const tp = room_view::get_tile_position(cd.last_pos);
                bool const last_solid = room.get_tile(cd.last_pos).solid();
                int32_t const x = room_view::get_tile_position(vec2{cd.pos.x - rad + 1.f, 0.f}).x;
                int32_t const x2 = room_view::get_tile_position(vec2{cd.pos.x + rad - 1.f, 0.f}).x;
                auto const solid_floor = [&](int32_t tx, int32_t ty) {
                    auto const t = room.get_tile(tx, ty);
                    return t.solid() || (t.t == world::terrain::floor && tp.y > ty);
                };
                bool done = false;
                if (cd.vel.y > 0.f) {
                    int32_t const yy = room_view::get_tile_position(vec2{0.f, cd.pos.y + rad + 0.01f}).y;
                    int32_t const y2 = room_view::get_tile_position(vec2{0.f, cd.last_pos.y + rad}).y;
                    for (int32_t i = y2; i <= yy && !done; ++i)
                        for (int32_t j = x; j <= x2 && !done; ++j)
                            if (room.get_tile(j, i).solid() && !room.get_tile(j, i - 1).solid() && (tp.y < i || last_solid)) {
                                cd.pos.y = float(i) * 20.f - rad;
                                cd.vel.y = 0.f;
                                cd.contact.y = 1;
                                done = true;
                            }
                } else if (cd.vel.y < 0.f) {
                    int32_t const yy = room_view::get_tile_position(vec2{0.f, cd.pos.y - rad - 0.01f}).y;
                    int32_t const y2 = room_view::get_tile_position(vec2{0.f, cd.last_pos.y - rad}).y;
                    for (int32_t i = y2; i >= yy && !done; --i)
                        for (int32_t j = x; j <= x2 && !done; ++j)
                            if (solid_floor(j, i) && !solid_floor(j, i + 1) && (tp.y > i || last_solid)) {
                                cd.pos.y = (float(i) + 1.f) * 20.f + rad;
                                cd.vel.y = 0.f;
                                cd.contact.y = -1;
                                done = true;
                            }
                }
            }
            // SlopesVertically
            {
                using world::slope_direction;
                ivec2 const tp = room_view::get_tile_position(cd.pos);
                ivec2 b{0, 0};
                slope_direction a = room.identify_slope(cd.pos);
                vec2 const mid = room_view::middle_of_tile(cd.pos);
                if (room.get_tile(cd.pos).t != world::terrain::slope) {
                    if (room.identify_slope(tp.x - 1, tp.y) != slope_direction::broken && cd.pos.x - rad <= mid.x - 10.f) {
                        a = room.identify_slope(tp.x - 1, tp.y);
                        b.x = -1;
                    } else if (room.identify_slope(tp.x + 1, tp.y) != slope_direction::broken && cd.pos.x + rad >= mid.x + 10.f) {
                        a = room.identify_slope(tp.x + 1, tp.y);
                        b.x = 1;
                    } else if (cd.pos.y - rad < mid.y - 10.f) {
                        if (room.identify_slope(tp.x, tp.y - 1) != slope_direction::broken) {
                            a = room.identify_slope(tp.x, tp.y - 1);
                            b.y = -1;
                        }
                    } else if (cd.pos.y + rad > mid.y + 10.f && room.identify_slope(tp.x, tp.y + 1) != slope_direction::broken) {
                        a = room.identify_slope(tp.x, tp.y + 1);
                        b.y = 1;
                    }
                }
                if (a != slope_direction::broken) {
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
                    if (num2 == -1 && cd.pos.y <= num + rad + rad)
                        cd.pos.y = num + rad + rad;
                    else if (num2 == 1 && cd.pos.y >= num - rad - rad)
                        cd.pos.y = num - rad - rad;
                }
            }
        }

        // ExplosiveSpear.Update's rag: a short cloth strip tied behind the
        // head, swinging with the spear
        void rag_update(sys::game_state& state, dcon::item_id it, spear_extra& x, room_view const& room) {
            auto& rng = state.world.item_get_random(it);
            int32_t const n = x.rag_count;
            if (n < 2)
                return;
            vec2 const rot = state.world.item_get_rotation(it);
            // carried through a pipe or teleported: NewRoom's ResetRag
            if (!custom::dist_less(x.rag[0].v[0], rag_attach_pos(state, it), 200.f))
                reset_rag(state, it, x);
            constexpr float con_rad = 7.f;
            for (int32_t i = 0; i < n; ++i) {
                auto& r = x.rag[i].v;
                float const t = float(i) / float(n - 1);
                r[1] = r[0];
                r[0] += r[2];
                r[2] -= rot * mathf::inverse_lerp(1.f, 0.f, float(i)) * 0.8f;
                r[4] = r[3];
                r[3] = rw::normalized(r[3] + r[5] * custom::lerp_map(rw::distance(r[0], r[1]), 1.f, 18.f, 0.05f, 0.3f));
                r[5] = rw::normalized(r[5] + custom::rnv(rng) * rng.value() * mathf::pow(mathf::inverse_lerp(1.f, 18.f, rw::distance(r[0], r[1])), 0.3f));
                if (room.point_submerged(r[0])) {
                    r[2] *= custom::lerp_map(rw::magnitude(r[2]), 1.f, 10.f, 1.f, 0.5f, mathf::lerp(1.4f, 0.4f, t));
                    r[2].y += 0.05f;
                    r[2] += custom::rnv(rng) * 0.1f;
                } else {
                    r[2] *= custom::lerp_map(rw::distance(r[0], r[1]), 1.f, 6.f, 0.999f, 0.7f, mathf::lerp(1.5f, 0.5f, t));
                    r[2].y -= room.gravity * custom::lerp_map(rw::distance(r[0], r[1]), 1.f, 6.f, 0.6f, 0.f);
                    if (i % 3 == 2 || i == n - 1) {
                        point_collision cd{r[0], r[1], r[2]};
                        point_terrain_collision(room, cd);
                        r[0] = cd.pos;
                        r[2] = cd.vel;
                        if (cd.contact.x != 0)
                            r[2].y *= 0.6f;
                        if (cd.contact.y != 0)
                            r[2].x *= 0.6f;
                    }
                }
            }
            for (int32_t j = 0; j < n; ++j) {
                auto& r = x.rag[j].v;
                if (j == 0) {
                    r[0] = rag_attach_pos(state, it);
                    r[2] = vec2{0.f, 0.f};
                    continue;
                }
                auto& p = x.rag[j - 1].v;
                vec2 dir = rw::normalized(r[0] - p[0]);
                float const d = rw::distance(r[0], p[0]);
                float const k = d > con_rad ? 0.5f : 0.25f;
                r[0] += dir * (con_rad - d) * k;
                r[2] += dir * (con_rad - d) * k;
                p[0] -= dir * (con_rad - d) * k;
                p[2] -= dir * (con_rad - d) * k;
                if (j > 1) {
                    auto& pp = x.rag[j - 2].v;
                    dir = rw::normalized(r[0] - pp[0]);
                    r[2] += dir * 0.2f;
                    pp[2] -= dir * 0.2f;
                }
                if (j < n - 1) {
                    auto const& nx = x.rag[j + 1].v;
                    r[3] = rw::slerp(r[3], (p[3] * 2.f + nx[3]) / 3.f, 0.1f);
                    r[5] = rw::slerp(r[5], (p[5] * 2.f + nx[5]) / 3.f, custom::lerp_map(rw::distance(r[1], r[0]), 1.f, 8.f, 0.05f, 0.5f));
                }
            }
        }

        // ElectricSpear.Update: the zappers' flicker (ResetFluxSpeed)
        void flux_update(rw::rng& rng, spear_extra& x) {
            for (int32_t i = 0; i < 4; ++i) {
                x.flux_timer[i] += x.flux_speed[i];
                if (x.flux_timer[i] > 6.2831855f) {
                    x.flux_speed[i] = rng.value() * 0.2f + 0.025f;
                    while (x.flux_timer[i] > 6.2831855f)
                        x.flux_timer[i] -= 6.2831855f;
                }
            }
        }

        // DataPearl.Update: hidden in debris until moved, and the glimmer
        void pearl_update(sys::game_state& state, dcon::item_id it) {
            auto& w = state.world;
            auto& pl = w.item_get_pearl(it);
            auto& rng = w.item_get_random(it);
            if (pl.hidden && !custom::dist_less(w.body_chunk_get_pos(chunk(state, it)), pl.hidden_pos, 20.f))
                pl.hidden = false;
            pl.last_glimmer = pl.glimmer;
            pl.glimmer = std::sin(pl.glimmer_prog * mathf::pi) * rng.value();
            if (pl.glimmer_prog < 1.f) {
                pl.glimmer_prog = mathf::min(1.f, pl.glimmer_prog + pl.glimmer_speed);
            } else if (pl.glimmer_wait > 0) {
                --pl.glimmer_wait;
            } else {
                pl.glimmer_wait = rng.range(20, 40);
                pl.glimmer_prog = 0.f;
                pl.glimmer_speed = 1.f / mathf::lerp(5.f, 15.f, rng.value());
            }
        }

        // Spear.Update, after the Weapon/PhysicalObject update
        void spear_update(sys::game_state& state, dcon::item_id it, room_view const& room) {
            auto& w = state.world;
            auto const c = chunk(state, it);
            vec2& pos = w.body_chunk_get_pos(c);
            vec2& vel = w.body_chunk_get_vel(c);
            // a needle lying or stuck somewhere lets go of Spearmaster and fades
            if (w.item_get_needle(it) == 2 && w.item_get_needle_fade(it) > 0)
                --w.item_get_needle_fade(it);
            if (w.item_get_needle(it) == 1 && (w.item_get_mode(it) == weapon_mode::free || w.item_get_mode(it) == weapon_mode::stuck_in_wall))
                w.item_set_needle(it, 2);
            w.item_set_last_pivot_at_tip(it, w.item_get_pivot_at_tip(it));
            w.item_set_pivot_at_tip(it, w.item_get_mode(it) == weapon_mode::thrown);
            if (w.item_get_add_poles(it)) {
                uint8_t was = 0;
                for_each_pole_tile(state, it, [&](uint8_t& flags, uint8_t bit, uint8_t beam) {
                    if (flags & beam)
                        was |= bit;
                    flags |= beam;
                });
                w.item_set_was_beam(it, was);
                w.item_set_add_poles(it, 0);
                ++w.room_get_geometry_version(room.id);
                w.item_set_has_beam_state(it, 1);
            }
            switch (w.item_get_mode(it)) {
            case weapon_mode::free:
                if (w.item_get_spinning(it)) {
                    int32_t& still = w.item_get_still_counter(it);
                    if (custom::dist_less(pos, w.body_chunk_get_last_pos(c), 4.f * room.gravity))
                        ++still;
                    else
                        still = 0;
                    if (w.body_chunk_get_contact_point(c).y < 0 || still > 20) {
                        // comes to rest lying on the ground
                        w.item_set_spinning(it, 0);
                        w.item_set_rotation_speed(it, 0.f);
                        w.item_set_rotation(it, custom::deg_to_vec(mathf::lerp(-50.f, 50.f, item_rng(state, it).value()) + 180.f));
                        vel *= 0.f;
                        state.play_sound(sound::sound_id::spear_stick_in_ground, room.id, pos);
                    }
                } else if (!custom::dist_less(w.body_chunk_get_last_pos(c), pos, 6.f)) {
                    set_random_spin(state, it);
                }
                break;
            case weapon_mode::thrown: {
                vel.y += 0.45f; // spears fly flatter than rocks
                ivec2 const dir = w.item_get_throw_dir(it);
                vec2 const thrown_pos = w.item_get_thrown_pos(it);
                // sticks when it hits a wall it was aimed at: straight on, close
                // enough, and a third of the time unless thrown from up close
                if (custom::dist_less(thrown_pos, pos, 560.f) && w.body_chunk_get_contact_point(c) == dir
                && room.get_tile(pos).t == world::terrain::air && room.get_tile(pos + rw::to_vec2(dir) * 20.f).t == world::terrain::solid
                && (item_rng(state, it).value() < 0.33f || custom::dist_less(thrown_pos, pos, 140.f) || w.item_get_always_stick(it))) {
                    ivec2 const tile = room_view::get_tile_position(pos);
                    bool ok = tile.x > 0 && tile.y > 0 && tile.x < room.width - 1 && tile.y < room.height - 1;
                    // one spear per tile
                    rooms::for_each_item(state, room.id, [&](dcon::item_id other) {
                        if (ok && other != it && w.item_get_mode(other) == weapon_mode::stuck_in_wall
                        && room_view::get_tile_position(w.body_chunk_get_pos(chunk(state, other))) == tile)
                            ok = false;
                    });
                    if (ok) {
                        w.item_set_has_stuck(it, 1);
                        w.item_set_stuck_pos(it, room_view::middle_of_tile(pos));
                        w.item_set_vibrate(it, 10);
                        change_mode(state, it, weapon_mode::stuck_in_wall);
                        w.item_set_has_stuck(it, 1);
                        state.play_sound(sound::sound_id::spear_stick_in_wall, room.id, pos);
                        w.body_chunk_set_collide_with_terrain(c, false);
                    }
                }
                break;
            }
            case weapon_mode::stuck_in_wall:
                pos = w.item_get_stuck_pos(it);
                vel *= 0.f;
                break;
            default:
                break;
            }
        }

        // Weapon.Update + PlayerCarryableItem.Update + PhysicalObject.Update + Rock.Update
        void update_item(sys::game_state& state, dcon::item_id it, room_view const& room) {
            auto& w = state.world;
            auto const o = body(state, it);
            auto const c = chunk(state, it);
            float const exit_speed = w.item_get_override_exit_thrown_speed(it) > 0.f ? w.item_get_override_exit_thrown_speed(it) : exit_thrown_mode_speed;
            ivec2 const contact = w.body_chunk_get_contact_point(c);
            if (w.item_get_vibrate(it) > 0)
                --w.item_get_vibrate(it);

            int32_t& floor_bounce = w.item_get_floor_bounce_frames(it);
            if (floor_bounce > 0 && w.item_get_throw_dir(it).y == 0) {
                --floor_bounce;
                vec2& vel = w.body_chunk_get_vel(c);
                if (contact.y != 0 && std::fabs(vel.x) > 5.f) {
                    vel.y = (std::fabs(vel.y) + 1.f) * float(-contact.y) + (contact.y < 0 ? 4.5f : 0.f);
                    vel.x = mathf::max(exit_speed + 1.f, std::fabs(vel.x)) * mathf::sign(vel.x);
                    floor_bounce /= 2;
                }
            }
            w.item_set_last_rotation(it, w.item_get_rotation(it));
            w.item_set_rotation(it, custom::deg_to_vec(aim(w.item_get_rotation(it)) + w.item_get_rotation_speed(it)));
            w.physical_object_set_water_retardation_immunity(o, w.item_get_mode(it) == weapon_mode::thrown ? 0.9f : 0.f);
            w.item_set_tail_pos(it, w.body_chunk_get_last_pos(c));

            switch (w.item_get_mode(it)) {
            case weapon_mode::carried:
                if (w.physical_object_get_grabbed_by_count(o) < 1)
                    change_mode(state, it, weapon_mode::free);
                break;
            case weapon_mode::free:
                if (physics::submersion(state, room, c) > 0.f && std::fabs(w.item_get_rotation_speed(it)) > 5.f)
                    w.item_get_rotation_speed(it) *= 0.7f;
                break;
            case weapon_mode::thrown: {
                auto const by = w.item_get_thrown_by(it);
                ivec2& throw_dir = w.item_get_throw_dir(it);
                vec2& vel = w.body_chunk_get_vel(c);
                if (w.item_get_change_dir_counter(it) > 0 && by && w.player_is_valid(by) && rooms::reachable(state, by) && throw_direction(state, by) == -throw_dir.x) {
                    w.item_set_change_dir_counter(it, 0);
                    throw_dir = ivec2{throw_direction(state, by), 0};
                    vel.x = std::fabs(vel.x) * float(throw_dir.x);
                    w.item_set_rotation(it, custom::deg_to_vec(vel.x < 0.f ? 270.f : 90.f));
                }
                --w.item_get_change_dir_counter(it);
                int32_t& frames = w.item_get_throw_mode_frames(it);
                if (frames > 0)
                    --frames;
                if (rw::magnitude(vel) < exit_speed || frames == 0) {
                    set_random_spin(state, it);
                    change_mode(state, it, weapon_mode::free);
                    w.item_set_forbidden_to_player(it, 10);
                }
                vec2 const pos = w.body_chunk_get_pos(c);
                if (floor_bounce > 0 && throw_dir.x != 0 && (contact.x != 0 || contact.y != 0) && room.get_tile(pos).t == world::terrain::slope) {
                    // vertical bounce off a slope
                    vel.x = 0.5f * float(throw_dir.x);
                    auto const slope = room.identify_slope(pos);
                    throw_dir = ivec2{0, (slope == world::slope_direction::up_left || slope == world::slope_direction::up_right) ? 1 : -1};
                    vel.y = float(throw_dir.y) * mathf::max(rw::magnitude(vel), exit_speed + 1.f + (throw_dir.y > 0 ? 10.f : 0.f));
                    w.body_chunk_set_pos(c, room_view::middle_of_tile(pos));
                    floor_bounce = 0;
                    change_mode(state, it, weapon_mode::thrown);
                    w.item_set_thrown_pos(it, w.body_chunk_get_pos(c));
                    w.item_set_throw_mode_frames(it, -1);
                    w.item_set_rotation(it, rw::to_vec2(throw_dir));
                    w.item_set_rotation_speed(it, 0.f);
                } else if ((w.item_get_type(it) == world::item_type::bomb || w.item_get_type(it) == world::item_type::flare_bomb) && contact != ivec2{0, 0}) {
                    // ScavengerBomb/FlareBomb: anything hard sets them off
                    detonate(state, it, pos);
                } else if (contact == throw_dir) {
                    hit_wall(state, it);
                    floor_bounce = 0;
                } else {
                    // Weapon.INotifyOfFlyingWeapons
                    creature::flying_weapon(state, room.id, pos, vel, w.item_get_thrown_pos(it));
                    // SharedPhysics.TraceProjectileAgainstBodyChunks (creatures)
                    dcon::creature_id hit;
                    creature::hit_part hit_part;
                    float const rad = w.body_chunk_get_rad(c) + (by ? 5.f : 0.f);
                    // the creature it hit: Creature.Violence(momentum, damage, stun bonus)
                    auto const hit_creature = [&](auto&& violence) {
                        auto& rng = state.room_random(room.id);
                        auto const type = w.item_get_type(it);
                        if (type == world::item_type::bomb || type == world::item_type::flare_bomb) {
                            detonate(state, it, pos);
                        } else if (type == world::item_type::lilly_puck) {
                            // LillyPuck: cuts, then glances off
                            violence(vel * w.body_chunk_get_mass(c), 0.7f, 10.f);
                            change_mode(state, it, weapon_mode::free);
                            vel = vel * -0.3f;
                            state.play_sound(sound::sound_id::rock_hit_creature, room.id, pos);
                        } else if (is_spear(state, it)) {
                            // Spear.HitSomething (lodging in the creature isn't ported)
                            violence(vel * w.body_chunk_get_mass(c), 1.f, 0.f);
                            if (type == world::item_type::explosive_spear) {
                                detonate(state, it, pos);
                            } else if (type == world::item_type::electric_spear && w.item_get_charge(it) > 0) {
                                // ElectricSpear: the charge goes into what it hit
                                violence(vec2{0.f, 0.f}, 0.3f, 140.f);
                                w.item_set_charge(it, int8_t(w.item_get_charge(it) - 1));
                                state.play_sound(sound::sound_id::centipede_shock, room.id, pos, 0.7f, 1.3f);
                            }
                            change_mode(state, it, weapon_mode::free);
                            vel *= -0.1f;
                        } else {
                            // Rock.HitSomething
                            violence(vel * w.body_chunk_get_mass(c), 0.01f, 45.f);
                            change_mode(state, it, weapon_mode::free);
                            vel = vel * -0.5f + custom::deg_to_vec(rng.value() * 360.f) * mathf::lerp(0.1f, 0.4f, rng.value()) * rw::magnitude(vel);
                            state.play_sound(sound::sound_id::rock_hit_creature, room.id, pos);
                        }
                        w.item_set_forbidden_to_player(it, 40);
                        w.item_set_thrown_by(it, dcon::player_id{});
                    };
                    if (creature::trace(state, room.id, pos, pos + vel, rad, hit, hit_part)) {
                        // a body chunk or an appendage; a Rock turns a lizard
                        // around (Lizard.Violence)
                        bool const rock = w.item_get_type(it) == world::item_type::rock;
                        auto const type = is_spear(state, it) ? creature::damage_type::stab : creature::damage_type::blunt;
                        hit_creature([&](vec2 m, float dmg, float st) { creature::violence(state, hit, hit_part, m, dmg, st, type, rock); });
                    }
                }
                break;
            }
            case weapon_mode::stuck_in_wall:
            case weapon_mode::on_back:
                break;
            }

            // PlayerCarryableItem.Update
            int32_t& forbidden = w.item_get_forbidden_to_player(it);
            if (forbidden > 0)
                --forbidden;

            if (w.item_get_mode(it) != weapon_mode::carried && w.item_get_mode(it) != weapon_mode::on_back)
                physics::update_object(state, room, o, nullptr, nullptr);

            // hanging or growing on its stalk
            if (auto const anchor = w.item_get_stalk(it); anchor.has_value() && w.item_get_mode(it) == weapon_mode::free) {
                vec2& pos = w.body_chunk_get_pos(c);
                vec2& vel = w.body_chunk_get_vel(c);
                float const len = w.item_get_stalk_length(it);
                if (len <= 0.f) {
                    pos = *anchor;
                    vel = vec2{0.f, 0.f};
                } else if (!custom::dist_less(pos, *anchor, len)) {
                    vec2 const dir = custom::dir_vec(*anchor, pos);
                    pos = *anchor + dir * len;
                    vel -= dir * rw::dot(vel, dir);
                    vel *= 0.98f;
                }
            }
            // ExplosiveSpear: goes off where it sticks
            if (w.item_get_type(it) == world::item_type::explosive_spear && w.item_get_mode(it) == weapon_mode::stuck_in_wall)
                detonate(state, it, w.body_chunk_get_pos(c));
            // ElectricSpear: sparks while charged
            if (w.item_get_type(it) == world::item_type::electric_spear && w.item_get_charge(it) > 0 && state.room_random(room.id).value() < 0.02f)
                particles::spawn(state, room.id, particles::kind::spark, w.body_chunk_get_pos(c), custom::deg_to_vec(state.room_random(room.id).value() * 360.f) * 2.f, 1.f, 0.15f, rw::vec3{0.7f, 0.85f, 1.f});
            if (auto const ex = w.item_get_extra_from_item_extra(it)) {
                if (w.item_get_type(it) == world::item_type::explosive_spear)
                    rag_update(state, it, w.weapon_extra_get_data(ex), room);
                else
                    flux_update(w.item_get_random(it), w.weapon_extra_get_data(ex));
            }
            if (w.item_get_type(it) == world::item_type::data_pearl)
                pearl_update(state, it);

            // Rock/Spear.soundLoop (ChunkDynamicSoundLoop): skidding or
            // flying fast. Only Rock_Skidding_On_Ground_LOOP has a sample;
            // the through the air ones are commented out in sounds.txt
            if (w.item_get_type(it) == world::item_type::rock || is_spear(state, it)) {
                vec2 const vel = w.body_chunk_get_vel(c);
                float const speed = rw::magnitude(vel);
                auto id = sound::sound_id::count;
                if (speed > 5.f) {
                    if (w.item_get_type(it) == world::item_type::rock)
                        id = w.body_chunk_get_contact_point(c).y < 0 ? sound::sound_id::rock_skidding_on_ground_loop : sound::sound_id::rock_through_air_loop;
                    else if (w.item_get_mode(it) == weapon_mode::thrown)
                        id = sound::sound_id::spear_thrown_through_air_loop;
                    else if (w.item_get_mode(it) == weapon_mode::free)
                        id = sound::sound_id::spear_spinning_through_air_loop;
                }
                if (id != sound::sound_id::count)
                    state.play_loop(sound::loop_key(sound::loop_owner::item, uint32_t(it.index())), id, room.id,
                        w.body_chunk_get_pos(c), mathf::inverse_lerp(5.f, 15.f, speed));
            }
            if (is_spear(state, it)) {
                spear_update(state, it, room);
                return;
            }
            // Rock.Update
            if (w.body_chunk_get_contact_point(c).y != 0)
                w.item_set_rotation_speed(it, (w.item_get_rotation_speed(it) * 2.f + w.body_chunk_get_vel(c).x * 5.f) / 3.f);
        }
    }

    dcon::physical_object_id body(sys::game_state const& state, dcon::item_id item) noexcept {
        return state.world.item_get_body_from_item_body(item);
    }

    dcon::body_chunk_id chunk(sys::game_state const& state, dcon::item_id item) noexcept {
        return physics::chunk(state, body(state, item), 0);
    }

    dcon::player_id grabber(sys::game_state const& state, dcon::item_id item) noexcept {
        return state.world.item_get_grabbed_by(item);
    }

    namespace {
        // Rock/Spear constructors share the body and the weapon state
        dcon::item_id create_weapon(sys::game_state& state, dcon::room_id room, vec2 pos, world::item_type type) {
            auto& w = state.world;
            physics::object_desc d;
            d.air_friction = 0.999f;
            d.gravity = 0.9f;
            d.bounce = 0.4f;
            d.surface_friction = 0.4f;
            d.water_friction = 0.98f;
            d.buoyancy = 0.4f;
            auto const o = physics::create_object(state, room, d);
            physics::add_chunk(state, o, pos, 5.f, 0.07f);
            auto const it = w.create_item();
            w.item_set_grabbed_by(it, dcon::player_id{});
            bool const pearl = type == world::item_type::data_pearl;
            w.item_set_type(it, type);
            w.item_set_mode(it, weapon_mode::free);
            // RainWorldGame.GetNewID
            w.item_set_seed(it, ++state.next_issued_id);
            w.item_set_random(it, rw::rng{uint32_t(w.item_get_seed(it)) * 2654435761u | 1u});
            // the Weapon constructor's roll (DataPearl isn't a Weapon)
            w.item_set_rotation(it, pearl ? vec2{0.f, 1.f} : custom::deg_to_vec(state.room_random(room).value() * 360.f));
            w.item_set_last_rotation(it, w.item_get_rotation(it));
            w.item_set_rotation_speed(it, 0.f);
            w.item_set_throw_dir(it, ivec2{0, 0});
            w.item_set_thrown_pos(it, pos);
            w.item_set_change_dir_counter(it, 0);
            w.item_set_floor_bounce_frames(it, 0);
            w.item_set_forbidden_to_player(it, 0);
            w.item_set_override_exit_thrown_speed(it, 0.f);
            w.item_set_throw_mode_frames(it, -1);
            w.item_set_thrown_by(it, dcon::player_id{});
            // erasable slots are reused: clear the spear state too
            w.item_set_vibrate(it, 0);
            w.item_set_has_stuck(it, 0);
            w.item_set_stuck_pos(it, pos);
            w.item_set_stuck_cycles(it, 0);
            w.item_set_add_poles(it, 0);
            w.item_set_has_beam_state(it, 0);
            w.item_set_was_beam(it, 0);
            w.item_set_spinning(it, 0);
            w.item_set_still_counter(it, 0);
            w.item_set_pivot_at_tip(it, 0);
            w.item_set_last_pivot_at_tip(it, 0);
            w.item_set_always_stick(it, 0);
            w.item_set_needle(it, 0);
            w.item_set_needle_fade(it, 400);
            w.item_set_tail_pos(it, pos);
            // the Spear constructor's spearmasterNeedleType
            bool const spear = type == world::item_type::spear || type == world::item_type::explosive_spear || type == world::item_type::electric_spear;
            // (from the item's own generator, not the world's: the rooms' spawns
            // don't shift each other's rolls)
            w.item_set_needle_type(it, uint8_t(spear ? w.item_get_random(it).range(0, 3) : 0));
            w.item_set_charge(it, type == world::item_type::electric_spear ? 3 : 0);
            w.item_set_stalk(it, std::nullopt);
            w.item_set_stalk_length(it, 0.f);
            w.item_set_color(it, rw::vec3{1.f, 1.f, 1.f});
            w.item_set_glow(it, type == world::item_type::slime_mold ? 1.f : 0.f);
            w.item_set_burn(it, 0);
            w.item_set_pearl(it, pearl_state{});
            if (pearl)
                set_pearl_type(state, it, "misc");
            w.try_create_item_body(it, o);
            if (type == world::item_type::explosive_spear) {
                // the ExplosiveSpear constructor, seeded by the ID: explodeAt,
                // then the rag's length
                auto const ex = w.create_weapon_extra();
                spear_extra x;
                rw::unity_random r{w.item_get_seed(it)};
                r.range(60, 100);
                int32_t const inner = r.range(4, 10);
                x.rag_count = uint8_t(custom::int_clamp(r.range(4, inner), 2, max_rag));
                w.weapon_extra_set_data(ex, x);
                w.try_create_item_extra(ex, it);
            } else if (type == world::item_type::electric_spear) {
                // the ElectricSpear constructor: electricColor (from the item's
                // own generator), the flux speeds seeded by the ID
                auto& own = w.item_get_random(it);
                float const h = mathf::lerp(0.55f, 0.7f, own.value());
                float const sa = mathf::lerp(0.8f, 1.f, own.value());
                float const l = mathf::lerp(0.3f, 0.6f, own.value());
                w.item_set_color(it, custom::hsl2rgb(h, sa, l));
                auto const ex = w.create_weapon_extra();
                spear_extra x;
                rw::unity_random r{w.item_get_seed(it)};
                for (int32_t i = 0; i < 4; ++i)
                    x.flux_speed[i] = r.value() * 0.2f + 0.025f;
                w.weapon_extra_set_data(ex, x);
                w.try_create_item_extra(ex, it);
            }
            // Weapon.PlaceInRoom (DataPearl.PlaceInRoom: no spin)
            physics::hard_set_position(state, physics::chunk(state, o, 0), room_view::middle_of_tile(pos));
            if (!pearl)
                set_random_spin(state, it);
            placed(state, it);
            return it;
        }
    }

    dcon::item_id create_spear(sys::game_state& state, dcon::room_id room, vec2 pos) {
        return create_weapon(state, room, pos, world::item_type::spear);
    }

    void spear_ends(sys::game_state const& state, dcon::item_id spear, float alpha, vec2& tail, vec2& tip) {
        auto const& w = state.world;
        auto const c = chunk(state, spear);
        vec2 const pos = rw::lerp(w.body_chunk_get_last_pos(c), w.body_chunk_get_pos(c), alpha);
        vec2 const rot = rw::normalized(rw::lerp(w.item_get_last_rotation(spear), w.item_get_rotation(spear), alpha));
        // the sprite pivots at its middle, or near the tip while flying
        float const anchor = mathf::lerp(w.item_get_last_pivot_at_tip(spear) ? 0.85f : 0.5f, w.item_get_pivot_at_tip(spear) ? 0.85f : 0.5f, alpha);
        tail = pos - rot * (anchor * spear_length);
        tip = pos + rot * ((1.f - anchor) * spear_length);
    }

    dcon::item_id create_on_tile(sys::game_state& state, dcon::room_id room, vec2 pos, world::item_type type) {
        return create_weapon(state, room, pos, type);
    }

    dcon::item_id create_rock(sys::game_state& state, dcon::room_id room, vec2 pos) {
        return create_weapon(state, room, pos, world::item_type::rock);
    }

    dcon::item_id create(sys::game_state& state, dcon::room_id room, vec2 pos, world::item_type type) {
        auto const it = create_weapon(state, room, pos, type);
        // placed exactly, not snapped to the middle of the tile
        physics::hard_set_position(state, chunk(state, it), pos);
        placed(state, it);
        return it;
    }

    void placed(sys::game_state& state, dcon::item_id it) {
        auto& w = state.world;
        // ExplosiveSpear.PlaceInRoom: ResetRag
        if (auto const ex = w.item_get_extra_from_item_extra(it); ex && w.item_get_type(it) == world::item_type::explosive_spear)
            reset_rag(state, it, w.weapon_extra_get_data(ex));
        // DataPearl.PlaceInRoom: a hidden one remembers where it lies
        auto& pl = w.item_get_pearl(it);
        pl.hidden_pos = w.body_chunk_get_pos(chunk(state, it));
    }

    void set_pearl_type(sys::game_state& state, dcon::item_id it, std::string_view name) {
        using pt = pearl_type;
        struct entry {
            std::string_view name;
            pt type;
            rw::vec3 main;
            bool highlight;
            rw::vec3 high;
        };
        // DataPearl.UniquePearlMainColor / UniquePearlHighLightColor (Downpour
        // and the Watcher on), by the lower case type name
        constexpr rw::vec3 no{0.f, 0.f, 0.f};
        auto const mix = [](rw::vec3 a, rw::vec3 b, float t) { return a + (b - a) * t; };
        rw::vec3 const g7{0.7f, 0.7f, 0.7f}, wh{1.f, 1.f, 1.f}, jam{0.38431373f, 0.18431373f, 0.9843137f}, wind{0.45f, 0.15f, 0.8f}, dark{0.1f, 0.1f, 0.1f};
        entry const table[] = {
            {"misc", pt::misc, g7, false, no},
            {"misc2", pt::misc2, {1.f, 0.6f, 0.9f}, true, wh},
            {"cc", pt::cc, {0.9f, 0.6f, 0.1f}, true, {1.f, 1.f, 0.f}},
            {"ds", pt::ds, {0.f, 0.7f, 0.1f}, false, no},
            {"gw", pt::gw, {0.f, 0.7f, 0.5f}, true, {0.5f, 1.f, 0.5f}},
            {"hi", pt::hi, {0.007843138f, 0.19607843f, 1.f}, true, {0.5f, 0.8f, 1.f}},
            {"lf_bottom", pt::lf_bottom, {1.f, 0.1f, 0.1f}, false, no},
            {"lf_west", pt::lf_west, {1.f, 0.f, 0.3f}, false, no},
            {"sb_filtration", pt::sb_filtration, {0.1f, 0.5f, 0.5f}, false, no},
            {"sh", pt::sh, {0.2f, 0.f, 0.1f}, true, {1.f, 0.2f, 0.6f}},
            {"si_top", pt::si_top, {0.01f, 0.01f, 0.01f}, true, {0.1f, 0.4f, 0.6f}},
            {"si_west", pt::si_west, {0.01f, 0.01f, 0.01f}, true, {0.1f, 0.6f, 0.4f}},
            {"sl_bridge", pt::sl_bridge, {0.4f, 0.1f, 0.9f}, true, {1.f, 0.4f, 1.f}},
            {"sl_moon", pt::sl_moon, {0.9f, 0.95f, 0.2f}, false, no},
            {"sb_ravine", pt::sb_ravine, {0.01f, 0.01f, 0.01f}, true, {0.6f, 0.1f, 0.4f}},
            {"su", pt::su, {0.5f, 0.6f, 0.9f}, false, no},
            {"uw", pt::uw, {0.4f, 0.6f, 0.4f}, true, {1.f, 0.7f, 1.f}},
            {"sl_chimney", pt::sl_chimney, {1.f, 0.f, 0.55f}, true, {0.8f, 0.3f, 1.f}},
            {"red_stomach", pt::red_stomach, {0.6f, 1.f, 0.9f}, true, wh},
            {"si_chat3", pt::si_chat3, {0.01f, 0.01f, 0.01f}, true, {0.4f, 0.1f, 0.6f}},
            {"si_chat4", pt::si_chat4, {0.01f, 0.01f, 0.01f}, true, {0.4f, 0.6f, 0.1f}},
            {"si_chat5", pt::si_chat5, {0.01f, 0.01f, 0.01f}, true, {0.6f, 0.1f, 0.4f}},
            {"spearmasterpearl", pt::spearmasterpearl, {0.04f, 0.01f, 0.04f}, true, {0.95f, 0.f, 0.f}},
            {"su_filt", pt::su_filt, {1.f, 0.75f, 0.9f}, false, no},
            {"dm", pt::dm, {0.95686275f, 0.92156863f, 0.20784314f}, false, no},
            {"lc", pt::lc, custom::hsl2rgb(0.34f, 1.f, 0.2f), false, no},
            {"lc_second", pt::lc_second, {0.6f, 0.f, 0.f}, true, {0.8f, 0.8f, 0.f}},
            {"oe", pt::oe, {0.54901963f, 0.36862746f, 0.8f}, false, no},
            {"ms", pt::ms, {0.8156863f, 0.89411765f, 0.27058825f}, false, no},
            {"rm", pt::rm, {0.38431373f, 0.18431373f, 0.9843137f}, true, {1.f, 0.f, 0.f}},
            {"rivulet_stomach", pt::rivulet_stomach, {0.5882353f, 0.87058824f, 0.627451f}, false, no},
            {"cl", pt::cl, {0.48431373f, 0.28431374f, 1.f}, true, {1.f, 0.f, 0.f}},
            {"vs", pt::vs, {0.53f, 0.05f, 0.92f}, true, {1.f, 0.f, 1.f}},
            {"broadcastmisc", pt::broadcast_misc, {0.9f, 0.7f, 0.8f}, true, {0.4f, 0.9f, 0.4f}},
            {"waua", pt::waua, {0.95f, 0.2f, 0.1f}, true, {1.f, 0.4f, 0.3f}},
            {"wora", pt::wora, mix({0.5f, 0.9f, 0.1f}, g7, 0.3f), true, mix({0.8f, 1.f, 0.5f}, wh, 0.3f)},
            {"drone", pt::drone, {0.9f, 0.7f, 0.5f}, true, {1.f, 0.9f, 0.8f}},
            {"abstract", pt::abstract, {0.f, 0.45f, 0.45f}, true, {0.7f, 0.58f, 0.58f}},
            {"text_audio_talkshow", pt::text_audio_talkshow, {0.8f, 0.08f, 0.65f}, true, dark},
            {"text_stardust", pt::text_stardust, {0.35f, 0.9f, 0.65f}, true, dark},
            {"text_notionofself", pt::text_notionofself, {0.4f, 0.75f, 0.4f}, true, dark},
            {"text_kitesday", pt::text_kitesday, {0.95f, 0.65f, 0.02f}, true, dark},
            {"text_secret", pt::text_secret, {0.5f, 0.2f, 0.6f}, true, dark},
            {"text_contempt", pt::text_contempt, mix({0.8f, 0.15f, 0.3f}, g7, 0.6f), true, dark},
            {"audio_jam1", pt::audio_jam1, mix(jam, g7, 0.13f), true, mix({0.45f, 1.f, 0.f}, wh, 0.13f)},
            {"audio_jam2", pt::audio_jam2, mix(jam, g7, 0.2f), true, mix({1.f, 0.8f, 0.f}, wh, 0.2f)},
            {"audio_jam3", pt::audio_jam3, mix(jam, g7, 0.08f), true, mix({0.f, 1.f, 0.55f}, wh, 0.08f)},
            {"audio_jam4", pt::audio_jam4, mix(jam, g7, 0.05f), true, mix({1.f, 0.f, 1.f}, wh, 0.05f)},
            {"audio_groove", pt::audio_groove, mix(jam, g7, 0.17f), true, mix({1.f, 0.4f, 0.5f}, wh, 0.17f)},
            {"audio_voicewind1", pt::audio_voicewind1, mix(wind, g7, 0.05f), true, mix({0.35f, 0.f, 0.2f}, wh, 0.05f)},
            {"audio_voicewind2", pt::audio_voicewind2, mix(wind, g7, 0.1f), true, mix({0.35f, 0.2f, 0.f}, wh, 0.1f)},
        };
        // anything else (unknown types too) is a plain grey pearl
        entry e = table[0];
        for (auto const& t : table)
            if (t.name == name)
                e = t;
        auto& w = state.world;
        w.item_set_color(it, e.main);
        auto& pl = w.item_get_pearl(it);
        pl.type = e.type;
        pl.has_highlight = e.highlight;
        pl.highlight = e.high;
    }

    void destroy(sys::game_state& state, dcon::item_id item) {
        auto& w = state.world;
        if (auto const ex = w.item_get_extra_from_item_extra(item))
            w.delete_weapon_extra(ex);
        if (w.item_get_has_beam_state(item))
            reset_beam_state(state, item);
        if (auto const by = w.item_get_grabbed_by(item))
            release_grasp(state, by, grasp{item, w.item_get_grabbed_hand(item)});
        auto const o = body(state, item);
        w.delete_item(item);
        if (o)
            physics::delete_object(state, o);
    }

    void spawn_room_items(sys::game_state& state, dcon::room_id room) {
        auto& w = state.world;
        if (w.room_get_items_spawned(room))
            return;
        w.room_set_items_spawned(room, true);
        auto const view = world::get_room_view(state, room);

        // room file items: rocks 75%, spears 60%
        w.room_for_each_item_location(room, [&](dcon::item_location_id il) {
            auto const pi = w.item_location_get_item(il);
            bool const rock = w.placed_item_get_type(pi) == world::item_type::rock;
            if (state.room_random(room).value() < (rock ? 0.75f : 0.6f))
                create_weapon(state, room, room_view::middle_of_tile(w.placed_item_get_tile(pi)), w.placed_item_get_type(pi));
        });

        // Room.Loaded's first pass over the placed objects: scavenger
        // outposts and treasuries with their loot, multiplayer items
        outposts::spawn_room(state, room);

        // the placed food, weapons and pearls of the room settings
        w.room_for_each_thing_location(room, [&](dcon::thing_location_id l) {
            auto const t = w.thing_location_get_thing(l);
            vec2 const pos = w.placed_thing_get_pos(t);
            using world::thing_kind;
            using world::item_type;
            switch (w.placed_thing_get_kind(t)) {
            case thing_kind::dangle_fruit:
            case thing_kind::rotten_fruit: {
                auto const it = create(state, room, pos, w.placed_thing_get_kind(t) == thing_kind::dangle_fruit ? item_type::dangle_fruit : item_type::rotten_fruit);
                // DangleFruit hangs from the ceiling above it
                for (float up = 20.f; up < 400.f; up += 10.f) {
                    if (view.get_tile(pos + vec2{0.f, up}).solid()) {
                        vec2 const anchor{pos.x, room_view::middle_of_tile(pos + vec2{0.f, up}).y - 10.f};
                        w.item_set_stalk(it, anchor);
                        w.item_set_stalk_length(it, mathf::max(8.f, anchor.y - pos.y));
                        break;
                    }
                }
                break;
            }
            case thing_kind::mushroom:
            case thing_kind::slime_mold: {
                auto const it = create(state, room, pos, w.placed_thing_get_kind(t) == thing_kind::mushroom ? item_type::mushroom : item_type::slime_mold);
                w.item_set_stalk(it, pos);
                w.item_set_stalk_length(it, 0.f);
                break;
            }
            case thing_kind::lilly_puck:
                create(state, room, pos, item_type::lilly_puck);
                break;
            case thing_kind::flare_bomb:
                create(state, room, pos, item_type::flare_bomb);
                break;
            case thing_kind::data_pearl:
            case thing_kind::unique_pearl: {
                // DataPearl.PlaceInRoom: at the placed object's own position
                auto const it = create(state, room, pos, item_type::data_pearl);
                set_pearl_type(state, it, state.to_string_view(w.placed_thing_get_name(t)));
                w.item_set_glow(it, w.placed_thing_get_kind(t) == thing_kind::unique_pearl ? 1.f : 0.f);
                w.item_get_pearl(it).hidden = w.placed_thing_get_extra(t) > 0.f;
                break;
            }
            case thing_kind::reliable_spear:
                // Room.Loaded: an AbstractSpear on the object's tile, always there
                create_weapon(state, room, room_view::middle_of_tile(pos), item_type::spear);
                break;
            default:
                break;
            }
        });

        if (w.room_get_is_shelter(room) || w.room_get_is_gate(room))
            return;
        float const density = w.room_get_random_item_density(room);
        float const spear_chance = w.room_get_random_item_spear_chance(room);
        for (int32_t n = int32_t(float(view.width) * float(view.height) * mathf::pow(density, 2.f) / 5.f); n >= 0; --n) {
            // Room.RandomTile
            ivec2 const t{state.room_random(room).range(0, view.width), state.room_random(room).range(0, view.height)};
            if (view.get_tile(t).solid())
                continue;
            bool ok = true;
            for (int32_t dx = -1; dx < 2; ++dx)
                if (!view.get_tile(t + ivec2{dx, -1}).solid()) {
                    ok = false;
                    break;
                }
            if (!ok)
                continue;
            // a few of the random spears are explosive or electric (Downpour's rolls simplified)
            if (state.room_random(room).value() < spear_chance) {
                float const roll = state.room_random(room).value();
                create_weapon(state, room, room_view::middle_of_tile(t),
                    roll < 0.06f ? world::item_type::explosive_spear : roll < 0.1f ? world::item_type::electric_spear : world::item_type::spear);
            }
            else
                create_rock(state, room, room_view::middle_of_tile(t));
        }
    }

    void update_room(sys::game_state& state, room_view const& room) {
        auto& w = state.world;
        rooms::for_each_item(state, room.id, [&](dcon::item_id it) { update_item(state, it, room); });
        // bombs and explosive spears that went off this tick
        auto pending = std::move(blasts());
        blasts().clear();
        for (auto const& b : pending) {
            if (!w.item_is_valid(b.item))
                continue;
            if (w.item_get_type(b.item) == world::item_type::flare_bomb)
                explosions::flash(state, room, b.at, 400.f);
            else
                explosions::explode(state, room, b.at, explosions::desc{110.f, 7.f, 1.6f, 80.f, 45.f}, body(state, b.item));
            // gone at the end of the tick (Destroy: slatedForDeletion)
            state.defer(room.id, [it = b.item](sys::game_state& s) {
                if (s.world.item_is_valid(it))
                    destroy(s, it);
            });
        }
    }

    dcon::item_id held(sys::game_state const& state, dcon::player_id p, uint32_t hand) {
        return grasp_in_hand(state, p, hand).item;
    }

    void carry_update(sys::game_state& state, dcon::player_id p) {
        auto& w = state.world;
        vec2 const main_pos = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
        vec2 const main_vel = w.body_chunk_get_vel(player::body_chunk(state, p, 0));
        float const flip = float(w.player_get_flip_direction(p));
        for (uint32_t hand = 0; hand < 2; ++hand) {
            auto const it = grasp_in_hand(state, p, hand).item;
            if (!it)
                continue;
            auto const c = chunk(state, it);
            w.body_chunk_set_last_pos(c, w.body_chunk_get_pos(c));
            w.body_chunk_set_pos(c, player_graphics::hand_position(state, p, hand));
            w.body_chunk_set_vel(c, main_vel);
            w.item_set_rotation_speed(it, 0.f);
            // Player.GetHeldItemDirection: spears are held level, pointing ahead
            vec2 dir = custom::perpendicular_vector(custom::dir_vec(main_pos, w.body_chunk_get_pos(c)) * (hand == 0 ? -1.f : 1.f));
            if (is_spear(state, it))
                dir = custom::deg_to_vec(80.f * flip);
            w.item_set_rotation(it, dir);
        }
        // a carried cicada (HeavyCarry)
        cicada::carry_update(state, p);
    }

    void release_all(sys::game_state& state, dcon::player_id p) {
        for (uint32_t hand = 0; hand < 2; ++hand)
            if (auto const g = grasp_in_hand(state, p, hand))
                release_grasp(state, p, g);
        cicada::release_player(state, p);
    }


    namespace {
        bool hunter(sys::game_state const& state, dcon::player_id p) {
            return state.world.player_get_stats(p).name == ::player::slugcat_name::red;
        }

        dcon::item_id back_spear(sys::game_state const& state, dcon::player_id p) {
            auto const s = state.world.player_get_back_spear(p);
            if (!s || !state.world.item_is_valid(s) || state.world.item_get_mode(s) != weapon_mode::on_back)
                return dcon::item_id{};
            return s;
        }

        // Custom.LerpAndTick
        float lerp_and_tick(float from, float to, float l, float tick) {
            if (from < to)
                return mathf::min(mathf::lerp(from, to, l) + tick, to);
            return mathf::max(mathf::lerp(from, to, l) - tick, to);
        }

        // SpearOnBack.SpearToBack
        void spear_to_back(sys::game_state& state, dcon::player_id p, dcon::item_id spear) {
            auto& w = state.world;
            if (back_spear(state, p))
                return;
            if (auto const by = w.item_get_grabbed_by(spear))
                release_grasp(state, by, grasp{spear, w.item_get_grabbed_hand(spear)});
            w.player_set_back_spear(p, spear);
            change_mode(state, spear, weapon_mode::on_back);
            w.player_set_back_locked(p, 1);
            state.play_sound(sound::sound_id::slugcat_stash_spear_on_back, room_of(state, body(state, spear)),
                w.body_chunk_get_pos(player::body_chunk(state, p, 0)));
        }

        // SpearOnBack.SpearToHand
        void spear_to_hand(sys::game_state& state, dcon::player_id p) {
            auto& w = state.world;
            auto const spear = back_spear(state, p);
            if (!spear)
                return;
            for (uint32_t hand = 0; hand < 2; ++hand) {
                if (grasp_in_hand(state, p, hand))
                    continue;
                physics::hard_set_position(state, chunk(state, spear), player_graphics::hand_position(state, p, hand));
                w.player_set_back_spear(p, dcon::item_id{});
                grab(state, p, spear, hand);
                w.player_set_back_locked(p, 1);
                return;
            }
        }

        // SpearOnBack.DropSpear
        void drop_back_spear(sys::game_state& state, dcon::player_id p) {
            auto& w = state.world;
            auto const spear = back_spear(state, p);
            w.player_set_back_spear(p, dcon::item_id{});
            if (!spear)
                return;
            w.body_chunk_set_vel(chunk(state, spear), w.body_chunk_get_vel(player::body_chunk(state, p, 0))
                + custom::deg_to_vec(state.room_random(rooms::room_of(state, p)).value() * 360.f) * 3.f * state.room_random(rooms::room_of(state, p)).value());
            change_mode(state, spear, weapon_mode::free);
        }

        // Player.GrabUpdate's back spear part + SpearOnBack.Update (Hunter)
        void back_spear_logic(sys::game_state& state, dcon::player_id p) {
            auto& w = state.world;
            auto const& in = w.player_get_input(p)[0];
            bool const locked = w.player_get_back_locked(p);
            bool const has = bool(back_spear(state, p));
            bool const hands_full = grasp_in_hand(state, p, 0) && grasp_in_hand(state, p, 1);
            dcon::item_id held_spear;
            for (uint32_t hand = 0; hand < 2; ++hand)
                if (auto const it = held(state, p, hand); it && is_spear(state, it))
                    held_spear = it;
            bool const can_put = !locked && !has && held_spear;
            bool const can_retrieve = !locked && has && !hands_full;
            if (in.pckp && (can_put || can_retrieve))
                w.player_set_back_increment(p, 1);
            int32_t& counter = w.player_get_back_counter(p);
            if (w.player_get_back_increment(p)) {
                ++counter;
                if (has && counter > 20) {
                    spear_to_hand(state, p);
                    counter = 0;
                } else if (!has && counter > 20 && held_spear) {
                    w.body_chunk_get_pos(player::body_chunk(state, p, 0)) += custom::dir_vec(w.body_chunk_get_pos(chunk(state, held_spear)),
                        w.body_chunk_get_pos(player::body_chunk(state, p, 0))) * 2.f;
                    spear_to_back(state, p, held_spear);
                    counter = 0;
                }
            } else {
                counter = 0;
            }
            if (!in.pckp)
                w.player_set_back_locked(p, 0);
            w.player_set_back_increment(p, 0);
        }

        // Spearmaster holding pick up with a free hand pulls a needle out of
        // its tail (Player.GrabUpdate, TailSpeckles)
        void spearmaster_update(sys::game_state& state, dcon::player_id p, room_view const& room) {
            auto& w = state.world;
            if (w.player_get_stats(p).name != ::player::slugcat_name::spear)
                return;
            auto const& in = w.player_get_input(p)[0];
            float& prog = w.player_get_spear_prog(p);
            if (!in.pckp) {
                if (prog > 0.f) {
                    prog = mathf::lerp(prog, 0.f, 0.05f);
                    if (prog < 0.025f)
                        prog = 0.f;
                }
                return;
            }
            bool const free_hand = !grasp_in_hand(state, p, 0) || !grasp_in_hand(state, p, 1);
            if (!free_hand || in.y != 0)
                return;
            auto const c0 = player::body_chunk(state, p, 0);
            auto const c1 = player::body_chunk(state, p, 1);
            if (prog == 0.f) {
                // TailSpeckles.newSpearSlot
                w.player_set_spear_line(p, uint8_t(state.room_random(room.id).range(0, 2)));
                w.player_set_spear_row(p, uint8_t(state.room_random(room.id).range(0, 4)));
            }
            if (prog < 0.1f) {
                prog = mathf::lerp(prog, 0.11f, 0.1f);
            } else {
                if (!w.player_get_spear_sound_ready(p)) {
                    w.player_set_spear_sound_ready(p, 1);
                    state.play_sound(sound::sound_id::sm_spear_pull, room.id, w.body_chunk_get_pos(c0), 1.f, 1.f + state.room_random(room.id).value() * 0.5f);
                }
                prog = mathf::lerp(prog, 1.f, 0.05f);
            }
            if (prog > 0.6f) {
                auto& g = w.player_get_graphics(p);
                g.head.vel += custom::deg_to_vec(state.room_random(room.id).value() * 360.f) * ((prog - 0.6f) / 0.4f) * 2.f;
            }
            if (prog > 0.95f)
                prog = 1.f;
            if (prog < 1.f)
                return;
            state.play_sound(sound::sound_id::sm_spear_grab, room.id, w.body_chunk_get_pos(c0), 1.f, 0.5f + state.room_random(room.id).value() * 1.5f);
            w.player_set_spear_sound_ready(p, 0);
            prog = 0.f;
            vec2 const p0 = w.body_chunk_get_pos(c0);
            vec2 const p1 = w.body_chunk_get_pos(c1);
            vec2 at = p0;
            vec2 dir = custom::dir_vec(p1, p0);
            if (std::fabs(p0.y - p1.y) > std::fabs(p0.x - p1.x) && p0.y > p1.y) {
                at += custom::dir_vec(p1, p0) * 5.f;
                dir *= -1.f;
                dir.x += 0.4f * float(w.player_get_flip_direction(p));
                dir = rw::normalized(dir);
            }
            vec2 const push = dir * 2.f + custom::deg_to_vec(state.room_random(room.id).value() * 360.f) * state.room_random(room.id).value();
            w.player_set_want_to_throw(p, 0);
            // a new object: made after the rooms' update, straight into the free hand
            state.defer(room.id, [p, at, push, rid = room.id](sys::game_state& s) {
                auto& sw = s.world;
                if (!sw.player_is_valid(p) || rooms::room_of(s, p) != rid)
                    return;
                auto const spear = create_spear(s, rid, at);
                auto const sc = chunk(s, spear);
                physics::hard_set_position(s, sc, at);
                sw.body_chunk_set_vel(sc, rw::clamp_magnitude(push / sw.body_chunk_get_mass(sc), 6.f));
                sw.item_set_needle(spear, 1);
                for (uint32_t hand = 0; hand < 2; ++hand)
                    if (!grasp_in_hand(s, p, hand)) {
                        grab(s, p, spear, hand);
                        break;
                    }
            });
        }
    }

    void grab_update(sys::game_state& state, dcon::player_id p, room_view const& room) {
        auto& w = state.world;
        auto const& input = w.player_get_input(p);
        // Player.checkInput: PickupPressed
        if (input[0].pckp && !input[1].pckp)
            w.player_set_want_to_pick_up(p, 5);
        int32_t& dont_grab = w.player_get_dont_grab_stuff(p);
        if (dont_grab > 0)
            --dont_grab;

        spearmaster_update(state, p, room);
        if (hunter(state, p))
            back_spear_logic(state, p);
        float item_score = 0.f, cicada_score = 0.f;
        auto candidate = dont_grab < 1 ? pickup_candidate(state, p, room, 20.f, item_score) : dcon::item_id{};
        // the creatures PickupCandidate looks at too: cicadas
        auto cicada_candidate = dont_grab < 1 ? cicada::pickup_candidate(state, p, room, cicada_score) : dcon::creature_id{};
        if (candidate && cicada_candidate) {
            if (cicada_score < item_score)
                candidate = dcon::item_id{};
            else
                cicada_candidate = dcon::creature_id{};
        }
        // a carried cicada takes both hands (grasps[0], HeavyCarry)
        auto const carried = cicada::held_by(state, p);
        int32_t& want_pick = w.player_get_want_to_pick_up(p);
        int32_t& want_throw = w.player_get_want_to_throw(p);
        if (want_pick > 0)
            --want_pick;
        if (want_throw > 0)
            --want_throw;
        if (input[0].thrw && !input[1].thrw)
            want_throw = 5;
        if (want_throw > 0 && carried) {
            toss_cicada(state, p, carried);
            want_throw = 0;
        }
        if (want_throw > 0) {
            for (uint32_t hand = 0; hand < 2; ++hand)
                if (auto const g = grasp_in_hand(state, p, hand)) {
                    throw_object(state, p, g, room);
                    want_throw = 0;
                    break;
                }
        }
        if (want_pick <= 0)
            return;

        // holding down while pressing pick up drops the held item
        bool drop = true;
        if (w.player_get_animation(p) == ::player::animation_index::deep_swim) {
            if (!grasp_in_hand(state, p, 0) && !grasp_in_hand(state, p, 1)) {
                drop = false;
            } else {
                for (int i = 0; i < 10; ++i)
                    if (input[i].y > -1 || input[i].x != 0) {
                        drop = false;
                        break;
                    }
            }
        } else {
            for (int i = 0; i < 5; ++i)
                if (input[i].y > -1) {
                    drop = false;
                    break;
                }
        }
        // HeavyCarry: pick up alone lets go of it (no Remix)
        if (carried) {
            want_pick = 0;
            // Player.ReleaseObject
            state.play_sound(sound::sound_id::slugcat_lay_down_creature, room.id, w.body_chunk_get_pos(cicada::chunk(state, carried, 0)));
            cicada::release(state, carried);
            return;
        }
        if (drop) {
            if (!grasp_in_hand(state, p, 0) && !grasp_in_hand(state, p, 1) && back_spear(state, p)
            && w.body_chunk_get_contact_point(player::body_chunk(state, p, 0)).y < 0) {
                drop_back_spear(state, p);
                return;
            }
            for (uint32_t hand = 0; hand < 2; ++hand)
                if (auto const g = grasp_in_hand(state, p, hand)) {
                    want_pick = 0;
                    // Player.ReleaseObject
                    state.play_sound(sound::sound_id::slugcat_lay_down_object, room.id, w.body_chunk_get_pos(chunk(state, g.item)));
                    w.item_set_forbidden_to_player(g.item, 40);
                    release_grasp(state, p, g);
                    return;
                }
        } else if (cicada_candidate) {
            // TwoHands: whatever the hands hold is let go of first
            release_all(state, p);
            state.play_sound(sound::sound_id::slugcat_pick_up_creature, room.id, w.body_chunk_get_pos(cicada::chunk(state, cicada_candidate, 0)));
            cicada::grab(state, p, cicada_candidate);
            want_pick = 0;
        } else if (candidate) {
            // a spear goes straight to the back when the hands are full
            if (hunter(state, p) && is_spear(state, candidate) && !w.player_get_back_locked(p) && !back_spear(state, p)
            && grasp_in_hand(state, p, 0) && grasp_in_hand(state, p, 1)) {
                spear_to_back(state, p, candidate);
                want_pick = 0;
                return;
            }
            for (uint32_t hand = 0; hand < 2; ++hand)
                if (!grasp_in_hand(state, p, hand)) {
                    grab(state, p, candidate, hand);
                    break;
                }
            want_pick = 0;
        }
    }

    void back_spear_update(sys::game_state& state, dcon::player_id p) {
        auto& w = state.world;
        auto const spear = back_spear(state, p);
        if (!spear)
            return;
        auto const& g = w.player_get_graphics(p);
        auto const& in = w.player_get_input(p)[0];
        vec2 const c0 = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
        vec2 const c1 = w.body_chunk_get_pos(player::body_chunk(state, p, 1));
        vec2 const v = g.initialized ? rw::lerp(g.draw_pos[0], g.head.pos, 0.2f) : c0;
        vec2 const v2 = g.initialized ? g.draw_pos[1] : c1;
        vec2 const v3 = custom::dir_vec(v2, v);
        float& flip = w.player_get_back_flip(p);
        vec2 rot;
        bool const awake = w.player_get_stun(p) <= 0 && !w.player_get_dead(p);
        using ::player::body_mode_index;
        if (awake) {
            auto const mode = w.player_get_body_mode(p);
            if (mode == body_mode_index::default_ && w.player_get_animation(p) == ::player::animation_index::none && w.player_get_standing(p) && c1.y < c0.y - 6.f)
                flip = lerp_and_tick(flip, float(in.x) * 0.3f, 0.05f, 0.02f);
            else if (mode == body_mode_index::stand && in.x != 0)
                flip = lerp_and_tick(flip, float(in.x), 0.02f, 0.1f);
            else
                flip = lerp_and_tick(flip, float(w.player_get_flip_direction(p)) * std::fabs(v3.x), 0.15f, 1.f / 6.f);
            int32_t const counter = w.player_get_back_counter(p);
            if (counter > 12 && !w.player_get_back_locked(p) && in.x != 0 && w.player_get_standing(p)) {
                // swung around over the shoulder into the free hand
                float side = 0.f;
                for (uint32_t hand = 0; hand < 2; ++hand)
                    if (!grasp_in_hand(state, p, hand)) {
                        side = hand == 0 ? -1.f : 1.f;
                        break;
                    }
                float const a = std::atan2(v.x - v2.x, v.y - v2.y) * 57.29578f;
                rot = custom::deg_to_vec(a + custom::lerp_map(float(counter), 12.f, 20.f, 0.f, 360.f * side));
            } else {
                rot = rw::normalized(v3 - custom::perpendicular_vector(v3) * 0.9f * (1.f - std::fabs(flip)));
            }
        } else {
            flip = lerp_and_tick(flip, 0.f, 0.15f, 1.f / 7.f);
            rot = v3 - custom::perpendicular_vector(v3) * 0.9f;
        }
        auto const c = chunk(state, spear);
        w.body_chunk_set_last_pos(c, w.body_chunk_get_pos(c));
        w.body_chunk_set_pos(c, rw::lerp(v2, v, 0.6f) - custom::perpendicular_vector(v2, v) * 7.5f * flip);
        w.body_chunk_set_vel(c, w.body_chunk_get_vel(player::body_chunk(state, p, 0)));
        w.item_set_rotation(spear, rot);
        w.item_set_rotation_speed(spear, 0.f);
    }
}
