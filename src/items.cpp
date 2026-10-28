#include <cmath>

#include "items.hpp"
#include "physics.hpp"
#include "player.hpp"

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

        // Weapon.SetRandomSpin
        void set_random_spin(sys::game_state& state, dcon::item_id it) {
            float const g = room_gravity(state, room_of(state, body(state, it)));
            state.world.item_set_rotation_speed(it, mathf::lerp(-100.f, 100.f, state.random.value()) * mathf::lerp(0.05f, 1.f, g));
        }

        // Weapon.ChangeMode
        void change_mode(sys::game_state& state, dcon::item_id it, weapon_mode m) {
            auto& w = state.world;
            if (w.item_get_mode(it) == m)
                return;
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
            if (throw_dir.x != 0) {
                vel.y = main_vel.y * 0.5f;
                vel.x = main_vel.x * 0.2f;
                vel.x += float(throw_dir.x) * 40.f * frc;
                vel.y += 3.f;
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
            int32_t const roll_dir = w.player_get_roll_direction(p);
            int32_t const roll_counter = w.player_get_roll_counter(p);
            if (w.player_get_animation(p) == ::player::animation_index::belly_slide && roll_counter > 8 && roll_counter < 15) {
                if (dir.x == roll_dir && throwing_skill(w.player_get_stats(p).name) > 0) {
                    w.body_chunk_get_vel(ic).x += float(dir.x) * 15.f;
                    // Rock is a HeavyWeapon
                    w.item_set_floor_bounce_frames(it, 30);
                    w.body_chunk_set_go_through_floors(ic, false);
                    w.body_chunk_get_vel(ic).y -= 5.f;
                    w.item_set_change_dir_counter(it, 0);
                } else if (dir.x == -roll_dir && !w.player_get_long_belly_slide(p)) {
                    w.body_chunk_get_vel(ic).y += 5.f;
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

            w.player_set_dont_grab_stuff(p, 15);
            // PlayerCarryableItem.Forbid
            w.item_set_forbidden_to_player(it, 40);
            release_grasp(state, g);
        }

        // Player.CanIPickThisUp, rocks
        bool can_pick_up(sys::game_state const& state, dcon::item_id it) {
            auto const m = state.world.item_get_mode(it);
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
                if (!can_pick_up(state, it))
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
            change_mode(state, it, weapon_mode::carried);
            w.item_set_thrown_by(it, dcon::player_id{});
        }

        // Weapon.Update + PlayerCarryableItem.Update + PhysicalObject.Update + Rock.Update
        void update_item(sys::game_state& state, dcon::item_id it, room_view const& room) {
            auto& w = state.world;
            auto const o = body(state, it);
            auto const c = chunk(state, it);
            float const exit_speed = w.item_get_override_exit_thrown_speed(it) > 0.f ? w.item_get_override_exit_thrown_speed(it) : exit_thrown_mode_speed;
            ivec2 const contact = w.body_chunk_get_contact_point(c);

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
                } else if (contact == throw_dir) {
                    hit_wall(state, it);
                    floor_bounce = 0;
                }
                break;
            }
            case weapon_mode::stuck_in_wall:
                break;
            }

            // PlayerCarryableItem.Update
            int32_t& forbidden = w.item_get_forbidden_to_player(it);
            if (forbidden > 0)
                --forbidden;

            if (w.item_get_mode(it) != weapon_mode::carried)
                physics::update_object(state, room, o, nullptr, nullptr);

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

    dcon::item_id create_rock(sys::game_state& state, dcon::room_id room, vec2 pos) {
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
        w.item_set_type(it, world::item_type::rock);
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
        w.try_create_item_body(it, o);
        // Weapon.PlaceInRoom
        physics::hard_set_position(state, physics::chunk(state, o, 0), room_view::middle_of_tile(pos));
        set_random_spin(state, it);
        return it;
    }

    void destroy(sys::game_state& state, dcon::item_id item) {
        auto& w = state.world;
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

        // room file items: rocks 75%, spears 60% (spears not ported, but rolled)
        w.room_for_each_item_location(room, [&](dcon::item_location_id il) {
            auto const pi = w.item_location_get_item(il);
            bool const rock = w.placed_item_get_type(pi) == world::item_type::rock;
            bool const spawn = state.random.value() < (rock ? 0.75f : 0.6f);
            if (spawn && rock)
                create_rock(state, room, room_view::middle_of_tile(w.placed_item_get_tile(pi)));
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
            if (state.random.value() < spear_chance)
                continue; // a spear
            create_rock(state, room, room_view::middle_of_tile(t));
        }
    }

    void update_room(sys::game_state& state, room_view const& room) {
        auto& w = state.world;
        w.for_each_item([&](dcon::item_id it) {
            if (room_of(state, body(state, it)) == room.id)
                update_item(state, it, room);
        });
    }

    vec2 hand_position(sys::game_state const& state, dcon::player_id p, uint32_t hand) {
        auto const& w = state.world;
        vec2 const p0 = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
        vec2 const p1 = w.body_chunk_get_pos(player::body_chunk(state, p, 1));
        float const flip = float(w.player_get_flip_direction(p));
        // hands hang from the upper body: the first one in front, the second behind
        vec2 const shoulder = rw::lerp(p0, p1, 0.3f);
        vec2 const down = custom::dir_vec(p0, p1);
        vec2 const side{flip * (hand == 0 ? 9.f : -6.f), 0.f};
        return shoulder + side + down * 4.f;
    }

    void carry_update(sys::game_state& state, dcon::player_id p) {
        auto& w = state.world;
        vec2 const main_vel = w.body_chunk_get_vel(player::body_chunk(state, p, 0));
        w.player_for_each_grasp_as_grabber(p, [&](dcon::grasp_id g) {
            auto const it = w.grasp_get_grabbed(g);
            auto const c = chunk(state, it);
            w.body_chunk_set_last_pos(c, w.body_chunk_get_pos(c));
            w.body_chunk_set_pos(c, hand_position(state, p, w.grasp_get_hand(g)));
            w.body_chunk_set_vel(c, main_vel);
            w.item_set_rotation_speed(it, 0.f);
        });
    }

    void release_all(sys::game_state& state, dcon::player_id p) {
        for (uint32_t hand = 0; hand < 2; ++hand)
            if (auto const g = grasp_in_hand(state, p, hand))
                release_grasp(state, g);
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
            for (uint32_t hand = 0; hand < 2; ++hand)
                if (auto const g = grasp_in_hand(state, p, hand)) {
                    want_pick = 0;
                    // Player.ReleaseObject
                    w.item_set_forbidden_to_player(w.grasp_get_grabbed(g), 40);
                    release_grasp(state, g);
                    return;
                }
        } else if (candidate) {
            for (uint32_t hand = 0; hand < 2; ++hand)
                if (!grasp_in_hand(state, p, hand)) {
                    grab(state, p, candidate, hand);
                    break;
                }
            want_pick = 0;
        }
    }
}
