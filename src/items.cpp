#include <cmath>

#include "items.hpp"
#include "centipede.hpp"
#include "explosions.hpp"
#include "particles.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "player_graphics.hpp"

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

        // what goes off after the update loop (items can't be destroyed while it walks them)
        struct pending_blast {
            dcon::item_id item;
            vec2 at;
        };
        std::vector<pending_blast>& blasts() {
            static std::vector<pending_blast> v;
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
                float const sign = state.random.value() < 0.5f ? -1.f : 1.f;
                state.world.item_set_rotation_speed(it, sign * mathf::lerp(50.f, 150.f, state.random.value()) * mathf::lerp(0.05f, 1.f, g));
                state.world.item_set_spinning(it, 1);
                return;
            }
            state.world.item_set_rotation_speed(it, mathf::lerp(-100.f, 100.f, state.random.value()) * mathf::lerp(0.05f, 1.f, g));
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
                        w.item_set_stuck_cycles(it, int8_t(state.random.range(3, 7) * (w.item_get_throw_dir(it).y != 0 ? -1 : 1)));
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
            if (m == weapon_mode::free)
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

        dcon::grasp_id grasp_in_hand(sys::game_state const& state, dcon::player_id p, uint32_t hand) {
            dcon::grasp_id found;
            state.world.player_for_each_grasp_as_grabber(p, [&](dcon::grasp_id g) {
                if (state.world.grasp_get_hand(g) == hand)
                    found = g;
            });
            return found;
        }

        // Creature.ReleaseGrasp
        void release_grasp(sys::game_state& state, dcon::grasp_id g) {
            auto& w = state.world;
            auto const o = body(state, w.grasp_get_grabbed(g));
            uint8_t& n = w.physical_object_get_grabbed_by_count(o);
            if (n > 0)
                --n;
            w.delete_grasp(g);
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
        void throw_object(sys::game_state& state, dcon::player_id p, dcon::grasp_id g, room_view const& room) {
            auto& w = state.world;
            auto const it = w.grasp_get_grabbed(g);
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

            player_graphics::throw_object(state, p, w.grasp_get_hand(g), it);
            w.player_set_dont_grab_stuff(p, 15);
            // PlayerCarryableItem.Forbid
            w.item_set_forbidden_to_player(it, 40);
            release_grasp(state, g);
        }

        // Player.CanIPickThisUp, weapons: spears stuck in walls stay there,
        // except for Artificer (no Remix "dislodge spears" option)
        bool can_pick_up(sys::game_state const& state, dcon::player_id p, dcon::item_id it) {
            auto const m = state.world.item_get_mode(it);
            if (m == weapon_mode::stuck_in_wall)
                return state.world.player_get_stats(p).name == ::player::slugcat_name::artificer;
            return m == weapon_mode::free;
        }

        // Player.PickupCandidate
        dcon::item_id pickup_candidate(sys::game_state const& state, dcon::player_id p, room_view const& room, float favor_spears) {
            auto const& w = state.world;
            vec2 const p0 = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
            int32_t const flip = w.player_get_flip_direction(p);
            dcon::item_id result;
            float best = std::numeric_limits<float>::max();
            w.for_each_item([&](dcon::item_id it) {
                auto const o = body(state, it);
                if (room_of(state, o) != room.id || w.item_get_forbidden_to_player(it) >= 1)
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
            auto const g = w.try_create_grasp(p, it);
            if (!g)
                return;
            w.grasp_set_hand(g, uint8_t(hand));
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

        // Spear.Update, after the Weapon/PhysicalObject update
        void spear_update(sys::game_state& state, dcon::item_id it, room_view const& room) {
            auto& w = state.world;
            auto const c = chunk(state, it);
            vec2& pos = w.body_chunk_get_pos(c);
            vec2& vel = w.body_chunk_get_vel(c);
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
                        w.item_set_rotation(it, custom::deg_to_vec(mathf::lerp(-50.f, 50.f, state.random.value()) + 180.f));
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
                && (state.random.value() < 0.33f || custom::dist_less(thrown_pos, pos, 140.f) || w.item_get_always_stick(it))) {
                    ivec2 const tile = room_view::get_tile_position(pos);
                    bool ok = tile.x > 0 && tile.y > 0 && tile.x < room.width - 1 && tile.y < room.height - 1;
                    // one spear per tile
                    w.for_each_item([&](dcon::item_id other) {
                        if (ok && other != it && w.item_get_mode(other) == weapon_mode::stuck_in_wall
                        && room_of(state, body(state, other)) == room.id
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
                if (w.item_get_change_dir_counter(it) > 0 && by && w.player_is_valid(by) && throw_direction(state, by) == -throw_dir.x) {
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
                    // SharedPhysics.TraceProjectileAgainstBodyChunks (creatures)
                    dcon::centipede_id hit;
                    uint32_t hit_chunk = 0;
                    float const rad = w.body_chunk_get_rad(c) + (by ? 5.f : 0.f);
                    if (centipede::trace(state, room.id, pos, pos + vel, rad, hit, hit_chunk)) {
                        auto& rng = state.random;
                        auto const type = w.item_get_type(it);
                        if (type == world::item_type::bomb || type == world::item_type::flare_bomb) {
                            detonate(state, it, pos);
                        } else if (type == world::item_type::lilly_puck) {
                            // LillyPuck: cuts, then glances off
                            centipede::violence(state, hit, hit_chunk, vel * w.body_chunk_get_mass(c), 0.7f, 10.f);
                            change_mode(state, it, weapon_mode::free);
                            vel = vel * -0.3f;
                            state.play_sound(sound::sound_id::rock_hit_creature, room.id, pos);
                        } else if (is_spear(state, it)) {
                            // Spear.HitSomething (lodging in the creature isn't ported)
                            centipede::violence(state, hit, hit_chunk, vel * w.body_chunk_get_mass(c), 1.f, 0.f);
                            if (type == world::item_type::explosive_spear) {
                                detonate(state, it, pos);
                            } else if (type == world::item_type::electric_spear && w.item_get_charge(it) > 0) {
                                // ElectricSpear: the charge goes into what it hit
                                centipede::violence(state, hit, hit_chunk, vec2{0.f, 0.f}, 0.3f, 140.f);
                                w.item_set_charge(it, int8_t(w.item_get_charge(it) - 1));
                                state.play_sound(sound::sound_id::centipede_shock, room.id, pos, 0.7f, 1.3f);
                            }
                            change_mode(state, it, weapon_mode::free);
                            vel *= -0.1f;
                        } else {
                            // Rock.HitSomething
                            centipede::violence(state, hit, hit_chunk, vel * w.body_chunk_get_mass(c), 0.01f, 45.f);
                            change_mode(state, it, weapon_mode::free);
                            vel = vel * -0.5f + custom::deg_to_vec(rng.value() * 360.f) * mathf::lerp(0.1f, 0.4f, rng.value()) * rw::magnitude(vel);
                            state.play_sound(sound::sound_id::rock_hit_creature, room.id, pos);
                        }
                        w.item_set_forbidden_to_player(it, 40);
                        w.item_set_thrown_by(it, dcon::player_id{});
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
            if (w.item_get_type(it) == world::item_type::electric_spear && w.item_get_charge(it) > 0 && state.random.value() < 0.02f)
                particles::spawn(state, room.id, particles::kind::spark, w.body_chunk_get_pos(c), custom::deg_to_vec(state.random.value() * 360.f) * 2.f, 1.f, 0.15f, rw::vec3{0.7f, 0.85f, 1.f});

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
        auto const g = state.world.item_get_grasp_as_grabbed(item);
        return g ? state.world.grasp_get_grabber(g) : dcon::player_id{};
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
            w.item_set_type(it, type);
            w.item_set_mode(it, weapon_mode::free);
            w.item_set_rotation(it, custom::deg_to_vec(state.random.value() * 360.f));
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
            w.item_set_charge(it, type == world::item_type::electric_spear ? 3 : 0);
            w.item_set_stalk(it, std::nullopt);
            w.item_set_stalk_length(it, 0.f);
            w.item_set_color(it, rw::vec3{1.f, 1.f, 1.f});
            w.item_set_glow(it, type == world::item_type::slime_mold ? 1.f : 0.f);
            w.item_set_burn(it, 0);
            w.try_create_item_body(it, o);
            // Weapon.PlaceInRoom
            physics::hard_set_position(state, physics::chunk(state, o, 0), room_view::middle_of_tile(pos));
            set_random_spin(state, it);
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

    dcon::item_id create_rock(sys::game_state& state, dcon::room_id room, vec2 pos) {
        return create_weapon(state, room, pos, world::item_type::rock);
    }

    dcon::item_id create(sys::game_state& state, dcon::room_id room, vec2 pos, world::item_type type) {
        auto const it = create_weapon(state, room, pos, type);
        // placed exactly, not snapped to the middle of the tile
        physics::hard_set_position(state, chunk(state, it), pos);
        return it;
    }

    rw::vec3 pearl_color(std::string_view name) {
        // our own colours per pearl type: an even spread of hues from the
        // name, the plain "misc" pearls a pale grey
        if (name.empty() || name == "misc")
            return rw::vec3{0.8f, 0.8f, 0.82f};
        uint32_t h = 2166136261u;
        for (char const c : name)
            h = (h ^ uint8_t(c)) * 16777619u;
        float const hue = float(h % 360u) / 360.f;
        auto const f = [&](float n) {
            float const k = std::fmod(n + hue * 6.f, 6.f);
            return 1.f - mathf::max(0.f, mathf::min(mathf::min(k, 4.f - k), 1.f)) * 0.55f;
        };
        return rw::vec3{f(5.f), f(3.f), f(1.f)};
    }

    void destroy(sys::game_state& state, dcon::item_id item) {
        auto& w = state.world;
        if (w.item_get_has_beam_state(item))
            reset_beam_state(state, item);
        if (auto const g = w.item_get_grasp_as_grabbed(item))
            release_grasp(state, g);
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
            if (state.random.value() < (rock ? 0.75f : 0.6f))
                create_weapon(state, room, room_view::middle_of_tile(w.placed_item_get_tile(pi)), w.placed_item_get_type(pi));
        });

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
                auto const it = create(state, room, pos, item_type::data_pearl);
                w.item_set_color(it, pearl_color(state.to_string_view(w.placed_thing_get_name(t))));
                w.item_set_glow(it, w.placed_thing_get_kind(t) == thing_kind::unique_pearl ? 1.f : 0.f);
                break;
            }
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
            ivec2 const t{state.random.range(0, view.width), state.random.range(0, view.height)};
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
            if (state.random.value() < spear_chance) {
                float const roll = state.random.value();
                create_weapon(state, room, room_view::middle_of_tile(t),
                    roll < 0.06f ? world::item_type::explosive_spear : roll < 0.1f ? world::item_type::electric_spear : world::item_type::spear);
            }
            else
                create_rock(state, room, room_view::middle_of_tile(t));
        }
    }

    void update_room(sys::game_state& state, room_view const& room) {
        auto& w = state.world;
        w.for_each_item([&](dcon::item_id it) {
            if (room_of(state, body(state, it)) == room.id)
                update_item(state, it, room);
        });
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
            destroy(state, b.item);
        }
    }

    dcon::item_id held(sys::game_state const& state, dcon::player_id p, uint32_t hand) {
        auto const g = grasp_in_hand(state, p, hand);
        return g ? state.world.grasp_get_grabbed(g) : dcon::item_id{};
    }

    void carry_update(sys::game_state& state, dcon::player_id p) {
        auto& w = state.world;
        vec2 const main_pos = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
        vec2 const main_vel = w.body_chunk_get_vel(player::body_chunk(state, p, 0));
        float const flip = float(w.player_get_flip_direction(p));
        w.player_for_each_grasp_as_grabber(p, [&](dcon::grasp_id g) {
            auto const it = w.grasp_get_grabbed(g);
            auto const c = chunk(state, it);
            uint8_t const hand = w.grasp_get_hand(g);
            w.body_chunk_set_last_pos(c, w.body_chunk_get_pos(c));
            w.body_chunk_set_pos(c, player_graphics::hand_position(state, p, hand));
            w.body_chunk_set_vel(c, main_vel);
            w.item_set_rotation_speed(it, 0.f);
            // Player.GetHeldItemDirection: spears are held level, pointing ahead
            vec2 dir = custom::perpendicular_vector(custom::dir_vec(main_pos, w.body_chunk_get_pos(c)) * (hand == 0 ? -1.f : 1.f));
            if (is_spear(state, it))
                dir = custom::deg_to_vec(80.f * flip);
            w.item_set_rotation(it, dir);
        });
    }

    void release_all(sys::game_state& state, dcon::player_id p) {
        for (uint32_t hand = 0; hand < 2; ++hand)
            if (auto const g = grasp_in_hand(state, p, hand))
                release_grasp(state, g);
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
            if (auto const g = w.item_get_grasp_as_grabbed(spear))
                release_grasp(state, g);
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
                + custom::deg_to_vec(state.random.value() * 360.f) * 3.f * state.random.value());
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
                w.player_set_spear_line(p, uint8_t(state.random.range(0, 2)));
                w.player_set_spear_row(p, uint8_t(state.random.range(0, 4)));
            }
            if (prog < 0.1f) {
                prog = mathf::lerp(prog, 0.11f, 0.1f);
            } else {
                if (!w.player_get_spear_sound_ready(p)) {
                    w.player_set_spear_sound_ready(p, 1);
                    state.play_sound(sound::sound_id::sm_spear_pull, room.id, w.body_chunk_get_pos(c0), 1.f, 1.f + state.random.value() * 0.5f);
                }
                prog = mathf::lerp(prog, 1.f, 0.05f);
            }
            if (prog > 0.6f) {
                auto& g = w.player_get_graphics(p);
                g.head.vel += custom::deg_to_vec(state.random.value() * 360.f) * ((prog - 0.6f) / 0.4f) * 2.f;
            }
            if (prog > 0.95f)
                prog = 1.f;
            if (prog < 1.f)
                return;
            state.play_sound(sound::sound_id::sm_spear_grab, room.id, w.body_chunk_get_pos(c0), 1.f, 0.5f + state.random.value() * 1.5f);
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
            auto const spear = create_spear(state, room.id, at);
            auto const sc = chunk(state, spear);
            physics::hard_set_position(state, sc, at);
            w.body_chunk_set_vel(sc, rw::clamp_magnitude((dir * 2.f + custom::deg_to_vec(state.random.value() * 360.f) * state.random.value()) / w.body_chunk_get_mass(sc), 6.f));
            w.item_set_needle(spear, 1);
            for (uint32_t hand = 0; hand < 2; ++hand)
                if (!grasp_in_hand(state, p, hand)) {
                    grab(state, p, spear, hand);
                    break;
                }
            w.player_set_want_to_throw(p, 0);
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
        auto const candidate = dont_grab < 1 ? pickup_candidate(state, p, room, 20.f) : dcon::item_id{};
        int32_t& want_pick = w.player_get_want_to_pick_up(p);
        int32_t& want_throw = w.player_get_want_to_throw(p);
        if (want_pick > 0)
            --want_pick;
        if (want_throw > 0)
            --want_throw;
        if (input[0].thrw && !input[1].thrw)
            want_throw = 5;
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
                    state.play_sound(sound::sound_id::slugcat_lay_down_object, room.id, w.body_chunk_get_pos(chunk(state, w.grasp_get_grabbed(g))));
                    w.item_set_forbidden_to_player(w.grasp_get_grabbed(g), 40);
                    release_grasp(state, g);
                    return;
                }
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
