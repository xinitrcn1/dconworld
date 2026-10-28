#include <cmath>
#include <type_traits>

#include "player.hpp"
#include "physics.hpp"
#include "items.hpp"
#include "shortcuts.hpp"

// Port notes
// - Code is kept structurally close to Player.cs so it can be diffed against
//   the decompiled source in og/; comments name the vanilla method.
// - Sound, graphics, items/grasps, shortcuts travel, creatures, ripple,
//   Downpour/Watcher/Remix only branches are omitted. Adrenaline is 0 (no
//   mushrooms), so Mathf.Lerp(x, y, Adrenaline) terms reduce to x.

namespace player {
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    using world::terrain;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    char const* to_string(body_mode_index m) noexcept {
        switch (m) {
        case body_mode_index::default_: return "Default";
        case body_mode_index::crawl: return "Crawl";
        case body_mode_index::stand: return "Stand";
        case body_mode_index::corridor_climb: return "CorridorClimb";
        case body_mode_index::climb_into_shortcut: return "ClimbIntoShortCut";
        case body_mode_index::wall_climb: return "WallClimb";
        case body_mode_index::climbing_on_beam: return "ClimbingOnBeam";
        case body_mode_index::swimming: return "Swimming";
        case body_mode_index::zero_g: return "ZeroG";
        case body_mode_index::stunned: return "Stunned";
        case body_mode_index::dead: return "Dead";
        }
        return "?";
    }
    char const* to_string(animation_index a) noexcept {
        static char const* const names[] = {
            "None", "CrawlTurn", "StandUp", "DownOnFours", "LedgeCrawl", "LedgeGrab", "HangFromBeam", "GetUpOnBeam",
            "StandOnBeam", "ClimbOnBeam", "GetUpToBeamTip", "HangUnderVerticalBeam", "BeamTip", "CorridorTurn",
            "SurfaceSwim", "DeepSwim", "Roll", "Flip", "RocketJump", "BellySlide", "AntlerClimb", "GrapplingSwing",
            "ZeroGSwim", "ZeroGPoleGrab", "VineGrab", "Dead",
        };
        return names[size_t(a)];
    }

    ivec2 input_package::zero_g_gamepad_int_vec() const noexcept {
        if (rw::magnitude(analogue_dir) > 0.2f)
            return ivec2{
                std::fabs(analogue_dir.x) > 0.1f ? int32_t(mathf::sign(analogue_dir.x)) : 0,
                std::fabs(analogue_dir.y) > 0.1f ? int32_t(mathf::sign(analogue_dir.y)) : 0,
            };
        return int_vec();
    }

    input_package make_input(float axis_x, float axis_y, bool jmp, bool thrw, bool pckp) noexcept {
        // RWInput.PlayerInputLogic (keyboard goes through the same axes)
        input_package in;
        in.jmp = jmp;
        in.thrw = thrw;
        in.pckp = pckp;
        in.analogue_dir = rw::clamp_magnitude(vec2{axis_x, axis_y}, 1.f);
        if (in.analogue_dir.x < -0.5f)
            in.x = -1;
        if (in.analogue_dir.x > 0.5f)
            in.x = 1;
        if (in.analogue_dir.y < -0.5f)
            in.y = -1;
        if (in.analogue_dir.y > 0.5f)
            in.y = 1;
        if (in.analogue_dir.y < -0.05f) {
            if (in.analogue_dir.x < -0.05f)
                in.down_diagonal = -1;
            else if (in.analogue_dir.x > 0.05f)
                in.down_diagonal = 1;
        }
        return in;
    }

    slugcat_stats make_stats(slugcat_name name) noexcept {
        // SlugcatStats constructor and PlayerGraphics.DefaultSlugcatColor /
        // DefaultBodyPartColorHex (eyes), not malnourished
        slugcat_stats s;
        s.name = name;
        rw::vec3 const white_eyes{1.f, 1.f, 1.f};
        switch (name) {
        case slugcat_name::white:
            break;
        case slugcat_name::yellow:
            s.body_color = rw::vec3{1.f, 1.f, 0.4509804f};
            s.body_weight_fac = 0.95f;
            s.lungs_fac = 1.2f;
            break;
        case slugcat_name::red:
            s.body_color = rw::vec3{1.f, 0.4509804f, 0.4509804f};
            s.runspeed_fac = 1.2f;
            s.body_weight_fac = 1.12f;
            s.pole_climb_speed_fac = 1.25f;
            s.corridor_climb_speed_fac = 1.2f;
            break;
        case slugcat_name::gourmand:
            s.body_color = rw::vec3{0.94118f, 0.75686f, 0.59216f};
            s.body_weight_fac = 1.35f;
            s.pole_climb_speed_fac = 0.8f;
            s.corridor_climb_speed_fac = 0.86f;
            break;
        case slugcat_name::artificer:
            s.body_color = rw::vec3{0.43922f, 0.13725f, 0.23529f};
            s.eye_color = white_eyes;
            s.runspeed_fac = 1.2f;
            s.body_weight_fac = 1.12f;
            s.pole_climb_speed_fac = 1.25f;
            s.corridor_climb_speed_fac = 1.2f;
            s.swim_boost_cost = 0.015f;
            s.swim_boost_force = 6.f;
            break;
        case slugcat_name::rivulet:
            s.body_color = rw::vec3{0.56863f, 0.8f, 0.94118f};
            s.body_weight_fac = 0.95f;
            s.lungs_fac = 0.15f;
            s.runspeed_fac = 1.75f;
            s.pole_climb_speed_fac = 1.8f;
            s.corridor_climb_speed_fac = 1.6f;
            s.swim_boost_cost = 0.025f;
            s.swim_boost_cooldown = 10;
            break;
        case slugcat_name::spear:
            s.body_color = rw::vec3{0.31f, 0.18f, 0.41f};
            s.eye_color = white_eyes;
            s.runspeed_fac = 1.2f;
            s.body_weight_fac = 0.85f;
            s.pole_climb_speed_fac = 1.25f;
            s.corridor_climb_speed_fac = 1.2f;
            break;
        case slugcat_name::saint:
            s.body_color = rw::vec3{0.66667f, 0.9451f, 0.33725f};
            break;
        case slugcat_name::inv:
            s.body_color = rw::vec3{0.09f, 0.14f, 0.31f};
            s.eye_color = white_eyes;
            break;
        case slugcat_name::watcher:
            // Custom.HSL2RGB(0.63055557, 0.54, 0.2)
            s.body_color = rw::vec3{0.092f, 0.1389f, 0.308f};
            s.eye_color = white_eyes;
            s.lungs_fac = 0.8f;
            break;
        default:
            break;
        }
        return s;
    }

    char const* to_string(slugcat_name n) noexcept {
        static char const* const names[] = {
            "Survivor", "Monk", "Hunter", "Gourmand", "Artificer", "Rivulet", "Spearmaster", "Saint", "Sofanthiel", "Watcher",
        };
        return n < slugcat_name::count ? names[size_t(n)] : "?";
    }

    namespace {
        // Every player field, see the player object in dcon_generated.txt
#define PLAYER_FIELDS \
    X(stats) \
    X(random) \
    X(input) \
    X(body_mode) \
    X(animation) \
    X(stun) \
    X(dead) \
    X(shortcut_delay) \
    X(flip_direction) \
    X(last_flip_direction) \
    X(standing) \
    X(feet_stuck_pos) \
    X(animation_frame) \
    X(dynamic_run_speed) \
    X(direction_boosts) \
    X(aerobic_level) \
    X(exhausted) \
    X(air_in_lungs) \
    X(immune_to_fall_damage) \
    X(can_jump) \
    X(want_to_jump) \
    X(can_wall_jump) \
    X(jump_boost) \
    X(jump_stun) \
    X(jump_chunk_counter) \
    X(super_launch_jump) \
    X(kill_super_launch_jump_counter) \
    X(simulate_hold_jump_button) \
    X(pole_skip_penalty) \
    X(slow_movement_stun) \
    X(landing_delay) \
    X(crawl_turn_delay) \
    X(slide_counter) \
    X(slide_direction) \
    X(init_slide_counter) \
    X(upper_body_frames_on_ground) \
    X(upper_body_frames_off_ground) \
    X(lower_body_frames_on_ground) \
    X(lower_body_frames_off_ground) \
    X(consistent_down_diagonal) \
    X(wall_slide_counter) \
    X(ledge_grab_counter) \
    X(roll_direction) \
    X(roll_counter) \
    X(stop_rolling_counter) \
    X(allow_roll) \
    X(exit_belly_slide_counter) \
    X(rocket_jump_from_belly_slide) \
    X(flip_from_slide) \
    X(whiplash_jump) \
    X(long_belly_slide) \
    X(go_into_corridor_climb) \
    X(corridor_drop) \
    X(corridor_turn_dir) \
    X(corridor_turn_counter) \
    X(can_corridor_jump) \
    X(vertical_corridor_slide_counter) \
    X(horizontal_corridor_slide_counter) \
    X(time_since_in_corridor_mode) \
    X(backwards_counter) \
    X(shoot_up_counter) \
    X(want_to_grab) \
    X(no_grab_counter) \
    X(drop_grab_tile) \
    X(force_feet_to_horizontal_beam_tile) \
    X(straight_up_on_horizontal_beam) \
    X(up_on_horizontal_beam_pos) \
    X(pullup_softlock_safety) \
    X(slide_up_pole) \
    X(dive_force) \
    X(swim_cycle) \
    X(swim_force) \
    X(water_jump_delay) \
    X(submerged) \
    X(lungs_exhausted) \
    X(drown) \
    X(entering_shortcut) \
    X(shortcut_rad_add) \
    X(in_shortcut) \
    X(vessel_reverse) \
    X(vessel_progress) \
    X(vessel_last_progress) \
    X(want_to_pick_up) \
    X(want_to_throw) \
    X(dont_grab_stuff) \
    X(touched_no_input_counter) \
    X(spear_prog) \
    X(spear_row) \
    X(spear_line) \
    X(spear_sound_ready)

        // Reference accessors into the data container for one player, so the
        // port reads like the C# ("this.canJump" -> p.can_jump())
        struct pref {
            sys::game_state& state;
            dcon::player_id id;
#define X(name) decltype(auto) name() { return state.world.player_get_##name(id); }
            PLAYER_FIELDS
#undef X
        };

        // Accessors so the port reads like the C# ("bodyChunks[0].vel" -> vel(0))
        struct ctx {
            pref p;
            sys::game_state& state;
            room_view const& room;
            dcon::physical_object_id o;
            dcon::body_chunk_id c[2];
            dcon::chunk_connection_id k; // bodyChunkConnections[0]

            ctx(sys::game_state& state, dcon::player_id id, room_view const& room) : p{state, id}, state{state}, room{room} {
                o = body(state, id);
                c[0] = physics::chunk(state, o, 0);
                c[1] = physics::chunk(state, o, 1);
                state.world.physical_object_for_each_chunk_connection_as_owner(o, [&](dcon::chunk_connection_id kk) {
                    if (!k)
                        k = kk;
                });
            }

            vec2& pos(int i) { return state.world.body_chunk_get_pos(c[i]); }
            vec2& last_pos(int i) { return state.world.body_chunk_get_last_pos(c[i]); }
            vec2& vel(int i) { return state.world.body_chunk_get_vel(c[i]); }
            float rad(int i) const { return state.world.body_chunk_get_rad(c[i]); }
            ivec2 contact(int i) const { return state.world.body_chunk_get_contact_point(c[i]); }
            ivec2 last_contact(int i) const { return state.world.body_chunk_get_last_contact_point(c[i]); }
            int32_t on_slope(int i) const { return state.world.body_chunk_get_on_slope(c[i]); }
            // terrainCurveNormal is always zero without TerrainManager rooms
            bool has_terrain_curve(int) const { return false; }
            float submersion(int i) const { return physics::submersion(state, room, c[i]); }
            float object_submersion() const { return physics::object_submersion(state, room, o); }
            bool solid(int i, int32_t x, int32_t y) const { return physics::is_tile_solid(state, room, c[i], x, y); }
            void go_through_floors(bool v) { physics::set_go_through_floors(state, o, v); }
            // base.gravity / EffectiveRoomGravity
            float gravity() const { return physics::gravity(state, o, room); }
            float eff_gravity() const { return room.gravity; }
            float& surface_friction() { return state.world.physical_object_get_surface_friction(o); }
            // room.PlaySound(id, mainBodyChunk, ...)
            void sound(sound::sound_id id, float vol = 1.f, float pitch = 1.f) { state.play_sound(id, room.id, pos(0), vol, pitch); }
            float& water_friction() { return state.world.physical_object_get_water_friction(o); }
            float& water_retardation_immunity() { return state.world.physical_object_get_water_retardation_immunity(o); }
            float& connection_distance() { return state.world.chunk_connection_get_distance(k); }
            physics::connection_type& connection_type() { return state.world.chunk_connection_get_type(k); }
            input_package& in(int k) { return p.input()[k]; }
            world::tile tile(vec2 v) const { return room.get_tile(v); }
            world::tile tile(ivec2 v) const { return room.get_tile(v); }
            static ivec2 tile_pos(vec2 v) { return room_view::get_tile_position(v); }
            static vec2 mid(vec2 v) { return room_view::middle_of_tile(v); }
            static vec2 mid(ivec2 v) { return room_view::middle_of_tile(v); }
            bool consious() { return p.stun() <= 0 && !p.dead(); }
            // Player.isRivulet (the only DLC cat with movement specific branches ported)
            bool rivulet() { return p.stats().name == slugcat_name::rivulet; }
            void aerobic_increase(float f) { p.aerobic_level() = mathf::min(1.f, p.aerobic_level() + f / 9.f); }
        };

        void movement_update(ctx& x);
        void update_body_mode(ctx& x);
        void update_animation(ctx& x);
        void jump(ctx& x);
        void wall_jump(ctx& x, int32_t direction);
        void lung_update(ctx& x);

        void stun_impl(pref& p, int32_t st) {
            // Player.Stun then Creature.Stun
            if (st > p.random().range(5, 12)) {
                p.standing() = false;
                p.feet_stuck_pos().reset();
            }
            p.stun() = mathf::max(p.stun(), st);
        }
        void die_impl(pref& p) {
            p.dead() = true;
        }

        // Player.TerrainImpact
        void terrain_impact(void* user, dcon::physical_object_id object, uint32_t chunk, ivec2 direction, float speed, bool first_contact) {
            ctx& x = *static_cast<ctx*>(user);
            pref& p = x.p;
            if (object != x.o)
                return;
            if (x.in(0).down_diagonal != 0 && p.animation() != animation_index::roll
            && (speed > 12.f || p.animation() == animation_index::flip || (p.animation() == animation_index::rocket_jump && p.rocket_jump_from_belly_slide()))
            && direction.y < 0 && p.allow_roll() > 0 && p.consistent_down_diagonal() > (speed > 24.f ? 1 : 6)) {
                if (p.animation() == animation_index::rocket_jump && p.rocket_jump_from_belly_slide()) {
                    x.vel(1).y += 3.f;
                    x.pos(1).y += 3.f;
                    x.vel(0).y -= 3.f;
                    x.pos(0).y -= 3.f;
                }
                p.animation() = animation_index::roll;
                p.roll_direction() = x.in(0).down_diagonal;
                p.roll_counter() = 0;
                x.sound(sound::sound_id::slugcat_roll_init);
                x.vel(0).x = mathf::lerp(x.vel(0).x, 9.f * float(x.in(0).x), 0.7f);
                x.vel(1).x = mathf::lerp(x.vel(1).x, 9.f * float(x.in(0).x), 0.7f);
                p.standing() = false;
            } else if (first_contact) {
                bool const gourmand = p.stats().name == slugcat_name::gourmand;
                float const death_speed = gourmand ? 80.f : 60.f;
                float const stun_speed = gourmand ? 40.f : 35.f;
                float const light_speed = gourmand ? 1.f : 3.f;
                float const medium_speed = gourmand ? 8.f : 16.f;
                using sound::sound_id;
                if (speed > death_speed && p.immune_to_fall_damage() <= 0 && direction.y < 0) {
                    x.sound(sound_id::slugcat_terrain_impact_death);
                    die_impl(p);
                } else if (speed > stun_speed && p.immune_to_fall_damage() <= 0) {
                    x.sound(sound_id::slugcat_terrain_impact_hard);
                    stun_impl(p, int32_t(custom::lerp_map(speed, stun_speed, death_speed, 40.f, 140.f, 2.5f)));
                } else if (direction.y < 0 && x.consious()) {
                    x.sound(p.standing() && chunk == 1 ? sound_id::slugcat_floor_impact_standard : sound_id::slugcat_floor_impact_stealthy,
                        mathf::inverse_lerp(8.f, 11.f, speed));
                } else if (speed < light_speed) {
                    x.sound(sound_id::slugcat_terrain_impact_light, mathf::inverse_lerp(0.f, 2.f, speed), 3.f);
                } else if (speed < medium_speed) {
                    x.sound(sound_id::slugcat_terrain_impact_medium);
                } else {
                    x.sound(sound_id::slugcat_terrain_impact_hard);
                }
            }
        }

        // every field back to its Player constructor value
        void reset_fields(pref& p) {
            auto const stats = p.stats();
            auto const random = p.random();
#define X(name) p.name() = std::remove_reference_t<decltype(p.name())>{};
            PLAYER_FIELDS
#undef X
            p.stats() = stats;
            p.random() = random;
            // id properties come back as fat ids (not references)
            p.state.world.player_set_vessel_shortcut(p.id, dcon::shortcut_id{});
            p.air_in_lungs() = 1.f;
            p.flip_direction() = 1;
            p.last_flip_direction() = 1;
        }
    }

    void stun(sys::game_state& state, dcon::player_id id, int32_t st) {
        pref p{state, id};
        stun_impl(p, st);
    }

    void die(sys::game_state& state, dcon::player_id id) {
        pref p{state, id};
        die_impl(p);
    }

    dcon::physical_object_id body(sys::game_state const& state, dcon::player_id p) noexcept {
        return state.world.player_get_body_from_player_body(p);
    }

    dcon::body_chunk_id body_chunk(sys::game_state const& state, dcon::player_id p, uint32_t i) noexcept {
        return physics::chunk(state, body(state, p), i);
    }

    dcon::player_id create(sys::game_state& state, dcon::room_id room, vec2 pos, slugcat_name name) {
        auto const id = state.world.create_player();
        pref p{state, id};
        p.stats() = make_stats(name);
        p.random() = rw::rng{0x9E3779B9u ^ (uint32_t(id.index()) * 0x85EBCA6Bu)};
        reset_fields(p);
        float const mass = 0.7f * p.stats().body_weight_fac;
        physics::object_desc d;
        d.air_friction = 0.999f;
        d.gravity = 0.9f;
        d.bounce = 0.1f;
        d.surface_friction = 0.5f;
        d.water_friction = 0.96f;
        d.buoyancy = 0.95f;
        auto const o = physics::create_object(state, room, d);
        auto const c0 = physics::add_chunk(state, o, pos + vec2{0.f, 17.f}, 9.f, mass / 2.f);
        auto const c1 = physics::add_chunk(state, o, pos, 8.f, mass / 2.f);
        physics::add_connection(state, o, c0, c1, 17.f, physics::connection_type::normal, 1.f, 0.5f);
        state.world.try_create_player_body(id, o);
        // erasable slots are reused: drop any graphics state left by a previous player
        state.world.player_set_graphics(id, ::player::graphics_state{});
        p.flip_direction() = p.random().value() < 0.5f ? -1 : 1;
        return id;
    }

    void destroy(sys::game_state& state, dcon::player_id id) {
        items::release_all(state, id);
        auto const o = body(state, id);
        state.world.delete_player(id);
        if (o)
            physics::delete_object(state, o);
    }

    void reset(sys::game_state& state, dcon::player_id id, dcon::room_id room, vec2 pos) {
        items::release_all(state, id);
        pref p{state, id};
        reset_fields(p);
        auto const o = body(state, id);
        if (room)
            state.world.physical_object_set_room_from_object_location(o, room);
        auto const c0 = physics::chunk(state, o, 0);
        auto const c1 = physics::chunk(state, o, 1);
        physics::hard_set_position(state, c0, pos + vec2{0.f, 17.f});
        physics::hard_set_position(state, c1, pos);
        for (auto const c : {c0, c1}) {
            state.world.body_chunk_set_vel(c, vec2{0.f, 0.f});
            state.world.body_chunk_set_contact_point(c, ivec2{0, 0});
            state.world.body_chunk_set_last_contact_point(c, ivec2{0, 0});
        }
        state.world.physical_object_set_surface_friction(o, 0.5f);
    }

    // Player.Update, the parts that matter to movement, in vanilla order
    void update(sys::game_state& state, dcon::player_id id, room_view const& room, input_package const& new_input) {
        ctx x(state, id, room);
        pref& p = x.p;

        // aerobic level decay (exhaustion only applies to malnourished cats)
        p.exhausted() = false;
        if (p.lungs_exhausted())
            p.aerobic_level() = 1.f;
        else
            p.aerobic_level() = mathf::max(1.f - p.air_in_lungs(), p.aerobic_level() - 1.f
                / ((x.in(0).x == 0 && x.in(0).y == 0 ? 400.f : 1100.f) * (1.f + 3.f * mathf::inverse_lerp(0.9f, 1.f, p.aerobic_level()))));

        if (p.pole_skip_penalty() > 0)
            --p.pole_skip_penalty();
        if (p.shoot_up_counter() > 0) {
            p.no_grab_counter() = mathf::max(p.no_grab_counter(), 2);
            --p.shoot_up_counter();
            if (!x.in(0).jmp || x.in(0).y < 1 || x.pos(0).y < x.last_pos(0).y)
                p.shoot_up_counter() = 0;
        }

        x.surface_friction() = x.consious() ? 0.5f : 0.3f;

        // "WALLSTUCK": the two chunks ended up on opposite sides of a wall
        if (x.contact(0).y != 0 && x.contact(0).y == -x.contact(1).y && room.get_tile((x.pos(0) + x.pos(1)) / 2.f).solid()
        && state.world.physical_object_get_grabbed_by_count(x.o) < 1) {
            vec2 const perp = custom::perpendicular_vector(x.pos(0), x.pos(1));
            if (!room.visual_contact(x.pos(0), x.pos(1))
            && !room.visual_contact(x.pos(0) + perp * x.rad(0), x.pos(1) + perp * x.rad(1))
            && !room.visual_contact(x.pos(0) - perp * x.rad(0), x.pos(1) - perp * x.rad(1)))
                physics::hard_set_position(state, x.c[1], x.pos(0) + custom::dir_vec(x.pos(0), x.pos(1)) * 2.f);
        }

        for (int i = 0; i < 4; ++i)
            if (p.direction_boosts()[i] > 0.f)
                p.direction_boosts()[i] = mathf::min(p.direction_boosts()[i] + 0.025f, 1.f);
        if (p.body_mode() == body_mode_index::corridor_climb)
            p.time_since_in_corridor_mode() = 0;
        else
            ++p.time_since_in_corridor_mode();
        if (p.allow_roll() > 0)
            --p.allow_roll();
        if (room.get_tile(x.pos(1) + vec2{0.f, -20.f}).t == terrain::air && (!x.solid(1, -1, -1) || !x.solid(1, 1, -1)))
            p.allow_roll() = 15;

        // jump buffering, still looking at last tick's input like vanilla
        bool const jump_pressed = x.in(0).jmp && !x.in(1).jmp;
        if (p.want_to_jump() > 0) {
            --p.want_to_jump();
        } else if (jump_pressed) {
            bool standing_still = p.standing() && p.animation() == animation_index::none
                && std::fabs(x.last_pos(1).y - x.pos(1).y) < 20.f && x.vel(1).y < 5.f && x.pos(0).y > x.pos(1).y + 5.f;
            if (x.rivulet())
                standing_still = std::fabs(x.last_pos(1).y - x.pos(1).y) < 50.f && x.vel(1).y < 15.f
                    && (p.animation() == animation_index::none || p.animation() == animation_index::flip
                        || p.animation() == animation_index::rocket_jump || p.animation() == animation_index::roll);
            if (standing_still && p.body_mode() == body_mode_index::default_ && x.contact(1).y == 0 && x.contact(0).y == 0
            && x.contact(1).x == 0 && x.contact(0).x == 0 && !x.solid(1, -1, 0) && !x.solid(1, 1, 0)) {
                // pole skip: jump again when the feet pass a horizontal beam
                vec2 const m = x.mid(x.pos(1));
                bool on_beam = x.tile(x.pos(1)).horizontal_beam() && x.in(0).x != 0
                    && mathf::min(std::fabs(m.y - x.pos(1).y), std::fabs(m.y - x.last_pos(1).y)) < 7.5f;
                if (x.rivulet())
                    on_beam = (x.tile(x.pos(1)).horizontal_beam() || x.tile(vec2{x.pos(1).x, x.pos(1).y - 10.f}).horizontal_beam())
                        && mathf::min(std::fabs(m.y - x.pos(1).y), std::fabs(m.y - x.last_pos(1).y)) < 22.5f;
                if (on_beam && x.in(0).y <= 0 && p.pole_skip_penalty() < 1) {
                    x.vel(0).y = x.rivulet() ? 7.f : 4.5f;
                    x.vel(1).y = x.rivulet() ? 6.f : 3.5f;
                    float const dy = m.y + 5.f - x.pos(1).y;
                    x.pos(1).y += dy;
                    x.pos(0).y += dy;
                    p.jump_boost() = x.rivulet() ? 7.f : 6.f;
                    p.pole_skip_penalty() = x.rivulet() ? 3 : 6;
                    p.want_to_jump() = 0;
                    p.can_jump() = 0;
                } else {
                    p.pole_skip_penalty() = x.rivulet() ? 6 : mathf::min(30, p.pole_skip_penalty() + 12);
                    p.want_to_jump() = 5;
                }
            } else {
                p.want_to_jump() = 5;
            }
        }

        if (p.body_mode() == body_mode_index::wall_climb)
            ++p.wall_slide_counter();
        else
            p.wall_slide_counter() = 0;
        if (p.can_wall_jump() > 0)
            --p.can_wall_jump();
        else if (p.can_wall_jump() < 0)
            ++p.can_wall_jump();
        if (p.jump_chunk_counter() > 0)
            --p.jump_chunk_counter();
        else if (p.jump_chunk_counter() < 0)
            ++p.jump_chunk_counter();
        if (p.no_grab_counter() > 0)
            --p.no_grab_counter();
        if (p.water_jump_delay() > 0)
            --p.water_jump_delay();
        if (p.force_feet_to_horizontal_beam_tile() > 0)
            --p.force_feet_to_horizontal_beam_tile();
        if (p.can_jump() > 0)
            --p.can_jump();
        if (p.slow_movement_stun() > 0)
            --p.slow_movement_stun();
        if (p.backwards_counter() > 0)
            --p.backwards_counter();
        if (p.landing_delay() > 0)
            --p.landing_delay();
        if (p.vertical_corridor_slide_counter() > 0)
            --p.vertical_corridor_slide_counter();
        if (p.horizontal_corridor_slide_counter() > 0)
            --p.horizontal_corridor_slide_counter();
        if (p.jump_stun() > 0)
            --p.jump_stun();
        else if (p.jump_stun() < 0)
            ++p.jump_stun();
        if (!p.dead())
            lung_update(x);

        // Player.checkInput
        for (int i = 9; i > 0; --i)
            p.input()[i] = p.input()[i - 1];
        p.input()[0] = (p.stun() == 0 && !p.dead()) ? new_input : input_package{};
        if (x.in(0).x == 0 && x.in(0).y == 0 && !x.in(0).jmp && !x.in(0).thrw && !x.in(0).pckp)
            ++p.touched_no_input_counter();
        else
            p.touched_no_input_counter() = 0;
        if (p.super_launch_jump() > 10 && x.in(0).jmp && x.in(1).jmp && x.in(2).jmp && x.in(0).y < 1)
            x.in(0).x = 0;
        if (p.animation() == animation_index::roll && x.in(0).x == 0 && x.in(0).down_diagonal != 0)
            x.in(0).x = x.in(0).down_diagonal;

        if (x.in(0).down_diagonal != 0 && x.in(0).down_diagonal == x.in(1).down_diagonal)
            ++p.consistent_down_diagonal();
        else
            p.consistent_down_diagonal() = 0;

        if (p.dead()) {
            p.animation() = animation_index::dead;
            p.body_mode() = body_mode_index::dead;
        } else if (p.stun() > 0) {
            p.animation() = animation_index::none;
            p.body_mode() = body_mode_index::stunned;
        }

        // wall slide friction
        if (p.body_mode() != body_mode_index::swimming) {
            float const k = mathf::clamp(1.f - x.surface_friction() * (x.pos(0).y > x.pos(1).y ? 2.f : 0.5f), 0.f, 1.f);
            for (int i = 0; i < 2; ++i)
                if (x.contact(i).x != 0 && x.in(0).x == x.contact(i).x && x.vel(i).y < 0.f && p.body_mode() != body_mode_index::corridor_climb)
                    x.vel(i).y *= k;
        }
        if (x.tile_pos(x.pos(0)).y <= x.tile_pos(x.pos(1)).y && x.solid(0, 0, 1))
            p.standing() = false;

        // Creature.Update
        if (p.stun() > 0)
            --p.stun();
        if (p.shortcut_delay() > 0)
            --p.shortcut_delay();
        // being sucked into a shortcut replaces the physics and movement
        if (shortcuts::update_entering(state, id, room))
            return;
        // PhysicalObject.Update
        physics::update_object(state, room, x.o, &terrain_impact, &x);

        x.go_through_floors(false);
        if (p.stun() < 1 && !p.dead()) {
            movement_update(x);
            items::grab_update(state, id, room);
        }
        items::carry_update(state, id);
    }
}

namespace player {
    namespace {
        // Player.SwimDir
        vec2 swim_dir(ctx& x, bool normalize) {
            vec2 v = x.in(0).analogue_dir;
            if (v.x != 0.f || v.y != 0.f)
                return normalize ? rw::normalized(v) : v;
            ivec2 const iv = x.in(0).zero_g_gamepad_int_vec();
            if (iv.x != 0 || iv.y != 0)
                return rw::normalized(rw::to_vec2(x.in(0).int_vec()));
            return vec2{0.f, 0.f};
        }

        // Player.DirectIntoHoles: nudge towards one tile wide openings
        void direct_into_holes(ctx& x) {
            vec2 const main = x.pos(0);
            for (auto const d : custom::four_directions)
                if (x.tile(main + rw::to_vec2(d) * 20.f).solid())
                    return;
            auto const& in = x.in(0);
            if (!((in.x != 0 && in.y == 0) || (in.y != 0 && in.x == 0)))
                return;
            vec2 const step{20.f * float(in.x), 20.f * float(in.y)};
            vec2 const t = main + step;
            if (x.tile(t).solid())
                return;
            if (!((x.tile(t + vec2{-20.f, 0.f}).solid() && x.tile(t + vec2{20.f, 0.f}).solid())
            || (x.tile(t + vec2{0.f, -20.f}).solid() && x.tile(t + vec2{0.f, 20.f}).solid())))
                return;
            vec2 const t2 = main + step * 2.f;
            if (x.tile(t2).t == terrain::shortcut_entrance) {
                auto const type = x.room.get_shortcut_data(x.tile_pos(t2)).type;
                if (type != world::shortcut_type::normal && type != world::shortcut_type::room_exit)
                    return;
            }
            x.vel(0) += (x.mid(t) - main) / 10.f;
        }

        // Player.GrabVerticalPole
        void grab_vertical_pole(ctx& x) {
            pref& p = x.p;
            ivec2 const tp = x.tile_pos(x.pos(0));
            bool const wide = x.contact(1).y < 0 && x.in(0).x == 0;
            for (int i = 0; i < (wide ? 3 : 1); ++i) {
                int32_t const dx = i > 0 ? (i == 1 ? -1 : 1) : 0;
                ivec2 const pole = tp + ivec2{dx, 0};
                if (!x.tile(pole).vertical_beam())
                    continue;
                p.animation() = animation_index::climb_on_beam;
                if (dx == 0 && std::fabs(x.vel(0).x) > 5.f)
                    p.flip_direction() = x.vel(0).x < 0.f ? -1 : 1;
                else
                    p.flip_direction() = x.pos(0).x < x.mid(pole).x ? -1 : 1;
                x.vel(0) = vec2{0.f, 0.f};
                x.pos(0).x = x.mid(pole).x;
            }
        }

        // Player.GravitateToOpening (used while swimming)
        void gravitate_to_opening(ctx& x) {
            auto const& in = x.in(0);
            float const g = x.eff_gravity();
            auto const pull = [&](vec2 target, bool horizontal) {
                int32_t best = -1;
                float dst = std::numeric_limits<float>::max();
                for (int i = 0; i < 2; ++i) {
                    if (custom::dist_less(x.pos(i), target, dst)) {
                        best = i;
                        dst = rw::distance(x.pos(i), target);
                    }
                }
                x.vel(best) += custom::dir_vec(x.pos(best), target) * mathf::lerp(0.5f, 1.f, g);
                for (int i = 0; i < 2; ++i) {
                    if (horizontal)
                        x.vel(i).y -= (x.pos(i).y - x.mid(x.pos(i)).y) * mathf::lerp(0.01f, 0.1f, g);
                    else
                        x.vel(i).x -= (x.pos(i).x - x.mid(x.pos(i)).x) * mathf::lerp(0.01f, 0.1f, g);
                    x.vel(i) = rw::lerp(x.vel(i), rw::clamp_magnitude(x.vel(i), 2.f), 0.5f);
                }
            };
            if (in.x != 0 && in.y == 0) {
                if (!x.solid(0, in.x, 0) && x.solid(0, in.x, 1) && x.solid(0, in.x, -1) && (!x.solid(0, 0, 1) || !x.solid(0, 0, -1)))
                    pull(x.mid(x.tile_pos(x.pos(0)) + ivec2{in.x, 0}), true);
            } else if (in.y != 0 && in.x == 0 && !x.solid(0, 0, in.y) && x.solid(0, 1, in.y) && x.solid(0, -1, in.y)
            && (!x.solid(0, 1, 0) || !x.solid(0, -1, 0))) {
                pull(x.mid(x.tile_pos(x.pos(0)) + ivec2{0, in.y}), false);
            }
        }

        bool is_beam_animation(animation_index a) {
            return a == animation_index::beam_tip || a == animation_index::climb_on_beam || a == animation_index::get_up_on_beam
                || a == animation_index::get_up_to_beam_tip || a == animation_index::hang_from_beam || a == animation_index::stand_on_beam
                || a == animation_index::ledge_grab || a == animation_index::hang_under_vertical_beam;
        }

        // Player.MovementUpdate
        void movement_update(ctx& x) {
            pref& p = x.p;
            direct_into_holes(x);
            if (p.rocket_jump_from_belly_slide() && p.animation() != animation_index::rocket_jump)
                p.rocket_jump_from_belly_slide() = false;
            if (p.flip_from_slide() && p.animation() != animation_index::flip)
                p.flip_from_slide() = false;
            if (p.whiplash_jump() && p.animation() != animation_index::belly_slide)
                p.whiplash_jump() = false;

            int32_t num = x.in(0).x;
            if (p.jump_stun() != 0)
                num = p.jump_stun() / std::abs(p.jump_stun());
            p.last_flip_direction() = p.flip_direction();
            if (num != p.flip_direction() && num != 0)
                p.flip_direction() = num;

            // count tight spots: solid on both sides along each axis
            int32_t squeezed = 0;
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 2; ++j)
                    if (x.solid(j, custom::eight_directions[i].x, custom::eight_directions[i].y)
                    && x.solid(j, custom::eight_directions[i + 4].x, custom::eight_directions[i + 4].y))
                        ++squeezed;

            // feet stick to the ground while standing still
            bool const stick = x.on_slope(1) == 0 && x.in(0).x == 0 && p.standing() && p.stun() < 1 && x.contact(1).y == -1 && !x.has_terrain_curve(1);
            if (p.feet_stuck_pos().has_value() && !stick)
                p.feet_stuck_pos().reset();
            else if (!p.feet_stuck_pos().has_value() && stick)
                p.feet_stuck_pos() = vec2{x.pos(1).x, x.mid(x.tile_pos(x.pos(1))).y - 10.f + x.rad(1)};
            if (p.feet_stuck_pos().has_value()) {
                p.feet_stuck_pos()->x += (x.pos(1).x - p.feet_stuck_pos()->x) * (1.f - x.surface_friction());
                x.pos(1) = *p.feet_stuck_pos();
                if (!x.solid(1, 0, -1)) {
                    bool const right = x.solid(1, 1, -1) && !x.solid(1, 1, 0);
                    bool const left = x.solid(1, -1, -1) && !x.solid(1, -1, 0);
                    if (left && !right)
                        p.feet_stuck_pos()->x += -1.6f * x.surface_friction();
                    else if (right && !left)
                        p.feet_stuck_pos()->x += 1.6f * x.surface_friction();
                    else
                        p.feet_stuck_pos().reset();
                }
            }

            if ((squeezed > 1 && x.on_slope(0) == 0 && x.on_slope(1) == 0 && (!x.solid(0, 0, 0) || !x.solid(1, 0, 0)))
            || (x.solid(0, -1, 0) && x.solid(0, 1, 0)) || (x.solid(1, -1, 0) && x.solid(1, 1, 0))) {
                ++p.go_into_corridor_climb();
            } else {
                p.go_into_corridor_climb() = 0;
                bool const grounded = x.contact(0).y == -1 || x.contact(1).y == -1;
                p.body_mode() = body_mode_index::default_;
                if (grounded) {
                    p.can_jump() = 5;
                    if (x.pos(0).y > x.pos(1).y + 3.f && !x.solid(1, 0, 1) && p.animation() != animation_index::crawl_turn && x.contact(0).y > -1)
                        p.body_mode() = body_mode_index::stand;
                    else
                        p.body_mode() = body_mode_index::crawl;
                } else if (p.jump_boost() > 0.f && (x.in(0).jmp || p.simulate_hold_jump_button() > 0)) {
                    p.jump_boost() -= 1.5f;
                    x.vel(0).y += (p.jump_boost() + 1.f) * 0.3f;
                    x.vel(1).y += (p.jump_boost() + 1.f) * 0.3f;
                } else {
                    p.jump_boost() = 0.f;
                }
                if (x.contact(0).x != 0 && x.contact(0).x == x.in(0).x)
                    p.body_mode() = body_mode_index::wall_climb;
                if (x.in(0).x != 0 && x.pos(0).y > x.pos(1).y && p.animation() != animation_index::crawl_turn && !x.solid(0, x.in(0).x, 0)
                && x.solid(1, x.in(0).x, 0) && x.contact(1).x == x.in(0).x) {
                    p.body_mode() = body_mode_index::crawl;
                    p.animation() = animation_index::ledge_crawl;
                }
                if (x.in(0).y == 1 && x.solid(0, 0, 1) && !x.solid(1, 0, 1) && (x.solid(1, -1, 1) || x.solid(1, 1, 1))) {
                    p.animation() = animation_index::none;
                    x.vel(1).y += 2.f * x.eff_gravity();
                    x.vel(0).x -= (x.pos(0).x - x.pos(1).x) * 0.25f * x.eff_gravity();
                    x.vel(0).y -= x.eff_gravity();
                }
            }
            // line up with a one tile wide hole above
            if (x.in(0).y > 0 && x.in(0).x == 0 && p.body_mode() == body_mode_index::default_ && x.pos(0).y - x.last_pos(0).y < 2.f
            && x.contact(1).y == 0 && !x.solid(0, 0, 1) && x.solid(0, -1, 1) && x.solid(0, 1, 1) && !x.solid(1, -1, 0) && !x.solid(1, 1, 0)
            && x.tile_pos(x.pos(0)) == x.tile_pos(x.pos(1)) + ivec2{0, 1} && std::fabs(x.pos(0).x - x.mid(x.pos(0)).x) < 5.f
            && x.eff_gravity() > 0.f) {
                x.pos(0).x = x.mid(x.pos(0)).x;
                x.pos(0).y += 1.f;
                x.vel(0).y += 1.f;
                x.vel(1).y += 1.f;
                x.pos(1).y += 1.f;
            }
            if (x.in(0).y == 1 && x.in(1).y != 1) {
                if (x.on_slope(1) == 0 || !x.solid(0, 0, 1))
                    p.standing() = true;
            } else if (x.in(0).y == -1 && x.in(1).y != -1) {
                p.standing() = false;
            }
            if (p.go_into_corridor_climb() > 2 && !p.corridor_drop()) {
                p.body_mode() = body_mode_index::corridor_climb;
                p.animation() = p.corridor_turn_dir().has_value() ? animation_index::corridor_turn : animation_index::none;
            }
            if (p.corridor_drop()) {
                p.body_mode() = body_mode_index::default_;
                p.animation() = animation_index::none;
                if (x.in(0).y >= 0 || p.go_into_corridor_climb() < 2)
                    p.corridor_drop() = false;
                if (x.pos(0).y < x.pos(1).y) {
                    int32_t const n = custom::int_clamp(int32_t(x.vel(0).y * -0.3f), 1, 10);
                    for (int32_t k = 0; k < n; ++k) {
                        if (x.solid(0, 0, -k)) {
                            p.corridor_drop() = false;
                            break;
                        }
                    }
                }
            }
            // water
            if (p.body_mode() != body_mode_index::wall_climb || x.submersion(0) == 1.f) {
                bool const down = x.in(0).y < 0 || x.in(0).down_diagonal != 0;
                if ((x.submersion(0) > 0.2f || x.submersion(1) > 0.2f) && p.body_mode() != body_mode_index::corridor_climb) {
                    float const wl = x.room.get_float_water_level(x.pos(0));
                    bool const deep = x.pos(0).y < wl - 80.f;
                    bool const under = x.pos(0).y < wl - (down ? 10.f : 30.f);
                    if ((p.animation() != animation_index::surface_swim || down || deep) && under && x.submersion(1) > (down ? -1.f : 0.6f)) {
                        p.body_mode() = body_mode_index::swimming;
                        p.animation() = animation_index::deep_swim;
                    } else if ((!x.solid(1, 0, -1) || x.submersion(1) == 1.f) && !is_beam_animation(p.animation())) {
                        p.body_mode() = body_mode_index::swimming;
                        p.animation() = animation_index::surface_swim;
                    }
                }
            }

            p.dynamic_run_speed()[0] = 3.6f;
            p.dynamic_run_speed()[1] = 3.6f;
            float accel = 2.4f;
            update_animation(x);

            if (x.contact(0).x == x.in(0).x && x.in(0).x != 0 && x.pos(0).y > x.mid(x.tile_pos(x.pos(0))).y
            && (p.body_mode() == body_mode_index::default_ || p.body_mode() == body_mode_index::wall_climb)
            && !x.solid(0, -x.in(0).x, 0) && !x.solid(0, 0, -2) && !x.solid(0, x.in(0).x, 1)) {
                p.animation() = animation_index::ledge_grab;
                p.body_mode() = body_mode_index::default_;
            }
            if (p.body_mode() == body_mode_index::crawl)
                ++p.crawl_turn_delay();
            else
                p.crawl_turn_delay() = 0;
            if (p.standing() && x.solid(1, 0, 1))
                p.standing() = false;
            if (x.in(0).y > 0 && x.in(1).y == 0 && !x.tile(x.pos(1)).vertical_beam() && x.tile(x.pos(1) + vec2{0.f, -20.f}).vertical_beam()) {
                p.animation() = animation_index::beam_tip;
                x.vel(1) = vec2{0.f, 0.f};
                p.want_to_grab() = -1;
            }
            update_body_mode(x);

            int32_t const body_length = 17;
            if (p.roll_direction() != 0) {
                ++p.roll_counter();
                num = p.roll_direction();
                x.connection_distance() = 10.f;
                if (p.body_mode() != body_mode_index::default_ || p.roll_counter() > 200) {
                    p.roll_counter() = 0;
                    p.roll_direction() = 0;
                }
            } else {
                x.connection_distance() = float(body_length);
            }
            x.connection_type() = p.corridor_turn_dir().has_value() ? physics::connection_type::pull : physics::connection_type::normal;

            p.want_to_grab() = (x.in(0).y <= 0 || x.object_submersion() > 0.9f) ? 0 : 1;
            if (p.want_to_grab() > 0 && p.no_grab_counter() == 0
            && (p.body_mode() == body_mode_index::default_ || p.body_mode() == body_mode_index::wall_climb || p.body_mode() == body_mode_index::stand
                || p.body_mode() == body_mode_index::climbing_on_beam || p.body_mode() == body_mode_index::swimming)
            && (p.time_since_in_corridor_mode() >= 20 || x.pos(1).y <= x.pos(0).y || x.tile_pos(x.pos(0)).x != x.tile_pos(x.pos(1)).x)
            && p.animation() != animation_index::climb_on_beam && p.animation() != animation_index::hang_from_beam && p.animation() != animation_index::get_up_on_beam
            && p.animation() != animation_index::deep_swim && p.animation() != animation_index::hang_under_vertical_beam
            && p.animation() != animation_index::get_up_to_beam_tip && p.animation() != animation_index::vine_grab) {
                int32_t const tx = x.tile_pos(x.pos(0)).x;
                int32_t hi = x.tile_pos(x.last_pos(0)).y;
                int32_t lo = x.tile_pos(x.pos(0)).y;
                if (lo > hi)
                    std::swap(hi, lo);
                for (int32_t m = hi; m >= lo; --m) {
                    if (x.room.get_tile(tx, m).horizontal_beam()) {
                        p.animation() = animation_index::hang_from_beam;
                        x.vel(0).y = 0.f;
                        x.vel(1).y *= 0.25f;
                        x.pos(0).y = room_view::middle_of_tile(tx, m).y;
                        break;
                    }
                }
                grab_vertical_pole(x);
                if (p.animation() != animation_index::hang_from_beam && p.animation() != animation_index::climb_on_beam
                && x.tile(x.pos(0) + vec2{0.f, 20.f}).vertical_beam() && !x.tile(x.pos(0)).vertical_beam()) {
                    x.pos(0) = x.mid(x.pos(0)) + vec2{0.f, 5.f};
                    x.vel(0) = vec2{0.f, 0.f};
                    x.vel(1) = rw::clamp_magnitude(x.vel(1), 9.f);
                    p.animation() = animation_index::hang_under_vertical_beam;
                }
            }

            bool run = p.body_mode() != body_mode_index::corridor_climb;
            if (p.animation() == animation_index::climb_on_beam || p.animation() == animation_index::hang_from_beam || p.animation() == animation_index::get_up_on_beam
            || p.animation() == animation_index::ledge_grab || p.animation() == animation_index::grappling_swing || p.animation() == animation_index::antler_climb)
                run = false;
            if (run && (p.dynamic_run_speed()[0] > 0.f || p.dynamic_run_speed()[1] > 0.f)) {
                if (p.slow_movement_stun() > 0) {
                    float const k = mathf::inverse_lerp(10.f, 0.f, float(p.slow_movement_stun()));
                    p.dynamic_run_speed()[0] *= 0.5f + 0.5f * k;
                    p.dynamic_run_speed()[1] *= 0.5f + 0.5f * k;
                    accel *= 0.4f + 0.6f * k;
                }
                if (p.body_mode() == body_mode_index::default_ && x.contact(0) == ivec2{0, 0} && x.contact(1) == ivec2{0, 0})
                    accel *= x.eff_gravity();
                for (int i = 0; i < 2; ++i) {
                    float const drs = p.dynamic_run_speed()[i];
                    if (num < 0) {
                        float a = accel * x.surface_friction();
                        if (x.vel(i).x - a < -drs)
                            a = drs + x.vel(i).x;
                        if (a > 0.f)
                            x.vel(i).x -= a;
                    } else if (num > 0) {
                        float a = accel * x.surface_friction();
                        if (x.vel(i).x + a > drs)
                            a = drs - x.vel(i).x;
                        if (a > 0.f)
                            x.vel(i).x += a;
                    }
                    if (x.contact(0).y != 0 || x.contact(1).y != 0) {
                        float target = 0.f;
                        if (x.in(0).x != 0)
                            target = mathf::clamp(x.vel(i).x, -drs, drs);
                        x.vel(i).x += (target - x.vel(i).x) * std::pow(x.surface_friction(), 1.5f);
                    }
                }
            }

            // charging a super launch jump while crouched
            int32_t launch_decay = 0;
            if (p.super_launch_jump() > 0 && p.kill_super_launch_jump_counter() < 1)
                launch_decay = 1;
            if (p.body_mode() == body_mode_index::crawl && x.contact(0).y < 0 && x.contact(1).y < 0) {
                if (x.in(0).x == 0 && x.in(0).y == 0) {
                    launch_decay = 0;
                    p.want_to_jump() = 0;
                    if (x.in(0).jmp) {
                        if (p.super_launch_jump() < 20)
                            ++p.super_launch_jump();
                        else
                            p.kill_super_launch_jump_counter() = 15;
                    }
                }
                if (!x.in(0).jmp && x.in(1).jmp)
                    p.want_to_jump() = 1;
            }
            if (p.kill_super_launch_jump_counter() > 0)
                --p.kill_super_launch_jump_counter();
            if (p.simulate_hold_jump_button() > 0)
                --p.simulate_hold_jump_button();

            if (p.can_jump() > 0 && p.want_to_jump() > 0) {
                p.can_jump() = 0;
                p.want_to_jump() = 0;
                jump(x);
            } else if (p.can_wall_jump() != 0 && p.want_to_jump() > 0 && x.in(0).x != -rw::math_sign(p.can_wall_jump())) {
                wall_jump(x, rw::math_sign(p.can_wall_jump()));
                p.want_to_jump() = 0;
            }
            // (JumpOnChunk needs other objects, Adrenaline boosts need mushrooms)
            p.super_launch_jump() -= launch_decay;
            shortcuts::check_entrance(x.state, x.p.id, x.room);
        }
    }
}

namespace player {
    namespace {
        // Player.UpdateBodyMode
        void update_body_mode(ctx& x) {
            pref& p = x.p;
            p.dive_force() = mathf::max(0.f, p.dive_force() - 0.05f);
            x.water_retardation_immunity() = mathf::inverse_lerp(0.f, 0.3f, p.dive_force()) * 0.85f;
            if (p.drop_grab_tile().has_value() && p.body_mode() != body_mode_index::default_ && p.body_mode() != body_mode_index::corridor_climb)
                p.drop_grab_tile().reset();
            if (x.contact(0).y < 0) {
                ++p.upper_body_frames_on_ground();
                p.upper_body_frames_off_ground() = 0;
            } else {
                p.upper_body_frames_on_ground() = 0;
                ++p.upper_body_frames_off_ground();
            }
            if (x.contact(1).y < 0) {
                ++p.lower_body_frames_on_ground();
                p.lower_body_frames_off_ground() = 0;
            } else {
                p.lower_body_frames_on_ground() = 0;
                ++p.lower_body_frames_off_ground();
            }

            auto const& in = x.in(0);
            float const g = x.eff_gravity();
            if (p.body_mode() == body_mode_index::default_) {
                if (in.y < 0 && (p.animation() != animation_index::roll || in.x == 0)) {
                    x.go_through_floors(true);
                    // diagonal down while falling: land on a floor next to a wall
                    if (in.down_diagonal != 0 && p.consistent_down_diagonal() > 6 && x.contact(0) == ivec2{0, 0} && x.contact(1) == ivec2{0, 0}) {
                        ivec2 t = x.tile_pos(x.pos(0).y < x.pos(1).y ? x.pos(0) : x.pos(1));
                        for (int i = 0; i < 5; ++i) {
                            t = t + ivec2{0, -1};
                            if (x.tile(t).t == terrain::solid)
                                break;
                            if (x.tile(t).t == terrain::floor) {
                                if (x.tile(t + ivec2{in.x, 0}).t == terrain::solid && !x.tile(t + ivec2{in.x, 1}).solid())
                                    x.go_through_floors(false);
                                break;
                            }
                        }
                    }
                }
                if (p.standing()) {
                    x.vel(0).y += 4.f * g;
                    x.vel(1).y -= 4.f * g;
                    p.dynamic_run_speed()[0] = 4.2f * p.stats().runspeed_fac;
                    p.dynamic_run_speed()[1] = 4.f * p.stats().runspeed_fac;
                    if (in.y != 0)
                        p.dynamic_run_speed()[1] = 2.f;
                    if (in.y > 0 && !x.solid(0, 0, 1) && x.solid(0, -1, 1) && x.solid(0, 1, 1))
                        x.vel(0).x = x.vel(0).x * 0.8f - (x.pos(0).x - x.mid(x.tile_pos(x.pos(0))).x) * 0.4f;
                } else {
                    p.dynamic_run_speed()[0] = 4.f;
                    if (in.y != 0)
                        p.dynamic_run_speed()[0] = 2.5f;
                    if (p.animation() == animation_index::crawl_turn)
                        p.dynamic_run_speed()[0] *= 0.75f;
                    p.dynamic_run_speed()[1] = p.dynamic_run_speed()[0];
                }
                // diving
                if (in.y < 0 && x.contact(0) == ivec2{0, 0} && x.contact(1) == ivec2{0, 0} && x.vel(0).y < -6.f) {
                    p.dive_force() = mathf::min(1.f, p.dive_force() + 1.f / 7.f);
                    x.vel(0).y -= 1.2f * p.dive_force();
                    x.vel(1).y += 1.2f * p.dive_force();
                }
                if (p.drop_grab_tile().has_value()) {
                    ivec2 const dgt = *p.drop_grab_tile();
                    if ((x.tile_pos(x.pos(0)) == dgt || x.tile_pos(x.pos(1)) == dgt) && !in.jmp && x.consious()) {
                        x.pos(0) = x.mid(dgt);
                        if (x.tile(dgt).vertical_beam())
                            p.animation() = animation_index::climb_on_beam;
                        else if (x.tile(dgt).horizontal_beam())
                            p.animation() = animation_index::hang_from_beam;
                        if (x.pos(1).y > x.pos(0).y) {
                            float const d = x.pos(1).x < x.pos(0).x ? -2.f : 2.f;
                            x.vel(1).x += d;
                            x.pos(1).x += d;
                        }
                        p.drop_grab_tile().reset();
                        return;
                    }
                    if (x.tile_pos(x.pos(0)).y > dgt.y + 2 || x.tile_pos(x.pos(0)).y < dgt.y || x.tile_pos(x.pos(1)).y < dgt.y
                    || (x.tile_pos(x.pos(0)).x != dgt.x && x.tile_pos(x.pos(1)).x != dgt.x)) {
                        p.drop_grab_tile().reset();
                        return;
                    }
                }
            } else if (p.body_mode() == body_mode_index::crawl) {
                p.dynamic_run_speed()[0] = 2.5f;
                if (in.y != 0)
                    p.dynamic_run_speed()[0] = 1.f;
                bool const backwards = (in.x > 0) == (x.pos(0).x < x.pos(1).x);
                if (backwards && x.on_slope(0) == 0 && x.on_slope(1) == 0) {
                    p.dynamic_run_speed()[0] *= 0.75f;
                    if (p.crawl_turn_delay() > 5 && !x.solid(0, 0, 1) && !x.solid(1, 0, 1) && in.x != 0) {
                        p.crawl_turn_delay() = 0;
                        if (!x.solid(0, -1, 1) || !x.solid(0, 1, 1))
                            p.animation() = animation_index::crawl_turn;
                    }
                }
                p.dynamic_run_speed()[1] = p.dynamic_run_speed()[0];
                if (x.on_slope(0) != 0 || x.on_slope(1) != 0) {
                    if ((in.x > 0) == (x.pos(0).x < x.pos(1).x))
                        p.dynamic_run_speed()[0] *= 0.5f;
                    else
                        p.dynamic_run_speed()[0] *= 0.75f;
                    if (x.on_slope(0) != 0)
                        x.vel(0).y -= 1.5f;
                    if (x.on_slope(1) != 0)
                        x.vel(1).y -= 1.5f;
                }
                if (x.contact(0).y > -1 && in.x != 0 && x.pos(1).y < x.pos(0).y - 3.f && x.contact(1).x == in.x)
                    x.pos(1).y += 1.f;
                if (in.y < 0) {
                    x.go_through_floors(true);
                    for (int j = 0; j < 2; ++j) {
                        if (!x.solid(j, 0, -1) && (x.solid(j, -1, -1) || x.solid(j, 1, -1))) {
                            x.vel(j).x = x.vel(j).x * 0.8f - (x.pos(j).x - x.mid(x.tile_pos(x.pos(j))).x) * 0.4f;
                            x.vel(j).y -= 1.f;
                            break;
                        }
                    }
                }
                if (p.standing() && (p.lower_body_frames_on_ground() >= 3
                || (x.contact(1).y < 0 && x.tile(x.tile_pos(x.pos(1)) + ivec2{0, -1}).t != terrain::air
                    && x.tile(x.tile_pos(x.pos(0)) + ivec2{0, -1}).t != terrain::air))) {
                    p.animation() = animation_index::stand_up;
                    if (in.x == 0) {
                        if (x.contact(1).y == -1 && x.solid(1, 0, -1) && !x.solid(1, 0, 1))
                            p.feet_stuck_pos() = x.mid(x.tile_pos(x.pos(1))) + vec2{0.f, -10.f + x.rad(1)};
                        else if (x.contact(0).y == -1 && x.solid(0, 0, -1) && !x.solid(0, 0, 1))
                            p.feet_stuck_pos() = x.pos(0) + vec2{0.f, -1.f};
                    }
                }
                if (x.on_slope(1) != 0 && x.contact(1).y > -1 && in.x == 0)
                    x.vel(0).x += float(x.on_slope(1));
                if (in.x != 0 && std::fabs(x.pos(1).x - x.last_pos(1).x) > 0.5f)
                    ++p.animation_frame();
                else
                    p.animation_frame() = 0;
                if (p.animation_frame() > 10)
                    p.animation_frame() = 0; // crawling step sound
            } else if (p.body_mode() == body_mode_index::stand) {
                x.vel(0).y += 1.5f * g;
                x.vel(1).y -= 4.5f * g;
                if (!p.standing() && p.lower_body_frames_on_ground() >= 5 && p.upper_body_frames_off_ground() >= 5) {
                    bool drop = true;
                    int32_t const fd = p.flip_direction();
                    ivec2 const ft = x.tile_pos(x.pos(1));
                    if (x.tile_pos(x.pos(0)).y == ft.y + 1 && x.solid(1, fd, 0) && x.solid(0, fd, -1)) {
                        bool flat = x.tile(ft).t != terrain::slope && x.tile(ft + ivec2{0, -1}).t != terrain::slope
                            && x.tile(ft + ivec2{fd, 0}).t != terrain::slope;
                        if (!flat)
                            flat = x.solid(0, fd, 0) && x.solid(1, fd, 0);
                        if (flat) {
                            // standing against a wall: stay up, nudge away from ledges
                            drop = false;
                            if (x.tile(ft + ivec2{-fd, 0}).t == terrain::air) {
                                if (x.tile(ft + ivec2{-fd, -1}).t != terrain::air) {
                                    x.vel(1).x += -1.1f * float(fd);
                                } else if (in.x != fd) {
                                    if (x.solid(0, fd, 1)) {
                                        p.flip_direction() *= -1;
                                    } else if (x.in(0).y < 0 && x.in(1).y < 0 && x.in(2).y < 0 && x.in(3).y < 0) {
                                        x.vel(1).x += -0.4f * float(fd);
                                        x.vel(0).y -= 1.f;
                                    }
                                }
                            }
                        }
                    }
                    if (drop)
                        p.animation() = animation_index::down_on_fours;
                }
                p.dynamic_run_speed()[0] = 4.2f * p.stats().runspeed_fac;
                p.dynamic_run_speed()[1] = 4.f * p.stats().runspeed_fac;
                if (in.y != 0)
                    p.dynamic_run_speed()[1] = 2.f;
                if (x.on_slope(1) != 0) {
                    p.dynamic_run_speed()[0] *= 0.75f;
                    p.dynamic_run_speed()[1] *= 0.75f;
                    if (x.contact(0).y == 1)
                        p.standing() = false;
                    if (in.x == x.on_slope(1))
                        x.vel(1).y -= x.gravity();
                    x.pos(1).y -= 2.f;
                }
                if (in.x != 0) {
                    if (x.contact(1).x == in.x) {
                        x.vel(0).x += float(in.x);
                        x.vel(0).y += 2.f * g;
                        x.vel(1).y += 2.f * g;
                    }
                    ++p.animation_frame();
                } else {
                    p.animation_frame() = 0;
                }
                if (p.animation_frame() > 6) {
                    p.animation_frame() = 0;
                    if (p.aerobic_level() < 0.7f)
                        x.aerobic_increase(0.05f);
                }
                if (p.slide_counter() > 0) {
                    // skidding after reversing direction at speed
                    ++p.slide_counter();
                    if (p.slide_counter() > 20 || in.x != -p.slide_direction())
                        p.slide_counter() = 0;
                    float const k = -std::sin(float(p.slide_counter()) / 20.f * mathf::pi * 0.5f) + 0.5f;
                    float const sd = float(p.slide_direction());
                    x.vel(0).x += k * 3.5f * sd - sd * (k < 0.f ? 0.8f : 0.5f);
                    x.vel(1).x += k * 3.5f * sd + sd * 0.5f;
                } else if (in.x != 0) {
                    if (in.x != p.slide_direction()) {
                        if (p.init_slide_counter() > (x.rivulet() ? 5 : 10) && (x.vel(0).x > 0.f) == (p.slide_direction() > 0) && std::fabs(x.vel(0).x) > 1.f)
                            p.slide_counter() = 1;
                        else
                            p.slide_direction() = in.x;
                        p.init_slide_counter() = 0;
                        return;
                    }
                    if (p.init_slide_counter() < 30) {
                        ++p.init_slide_counter();
                        return;
                    }
                } else if (p.init_slide_counter() > 0) {
                    --p.init_slide_counter();
                    return;
                }
            } else if (p.body_mode() == body_mode_index::corridor_climb) {
                x.go_through_floors(true);
                p.roll_direction() = 0;
                if (p.corridor_turn_dir().has_value()) {
                    for (int k = 0; k < 2; ++k)
                        x.vel(k).y += x.gravity();
                    return;
                }
                // dropping down a vertical shaft onto a pole
                if (in.y < 0 && !in.jmp && !x.solid(0, 0, -1) && !x.solid(0, 0, -2) && !x.solid(0, 0, -3)
                && ((x.pos(0).y < x.pos(1).y && x.solid(0, -1, 1) && x.solid(0, 1, 1) && (!x.solid(0, -1, 0) || !x.solid(0, 1, 0)))
                    || (x.pos(0).y > x.pos(1).y && x.solid(1, -1, 1) && x.solid(1, 1, 1) && (!x.solid(1, -1, 0) || !x.solid(1, 1, 0))))) {
                    if (x.pos(0).y < x.pos(1).y) {
                        if (x.tile(x.pos(0)).any_beam())
                            p.drop_grab_tile() = x.tile_pos(x.pos(0));
                        else if (x.tile(x.pos(0) + vec2{0.f, -20.f}).any_beam())
                            p.drop_grab_tile() = x.tile_pos(x.pos(0) + vec2{0.f, -20.f});
                    } else if (x.tile(x.pos(1)).any_beam()) {
                        p.drop_grab_tile() = x.tile_pos(x.pos(1));
                    } else if (x.tile(x.pos(1) + vec2{0.f, -20.f}).any_beam()) {
                        p.drop_grab_tile() = x.tile_pos(x.pos(1) + vec2{0.f, -20.f});
                    }
                } else {
                    p.drop_grab_tile().reset();
                }
                bool const vertical = std::fabs(x.pos(0).x - x.pos(1).x) < 5.f && x.solid(0, -1, 0) && x.solid(0, 1, 0);
                bool const horizontal = std::fabs(x.pos(0).y - x.pos(1).y) < 7.5f && x.solid(0, 0, -1) && x.solid(0, 0, 1);
                bool braced = false;
                if (in.jmp && g > 0.f && !x.in(1).jmp && in.y < 0 && !x.solid(0, 0, -1) && !x.solid(1, 0, -1)) {
                    p.corridor_drop() = true;
                    p.can_corridor_jump() = 0;
                } else if (vertical) {
                    if (x.pos(0).y > x.pos(1).y) {
                        braced = x.solid(1, 0, -1);
                    } else if (in.jmp && !x.in(1).jmp && x.pos(0).y < x.pos(1).y && in.y > 0) {
                        p.corridor_turn_dir() = ivec2{0, 1};
                        p.corridor_turn_counter() = 0;
                        p.can_corridor_jump() = 0;
                    }
                } else if (horizontal) {
                    if (in.jmp && !x.in(1).jmp && in.x != 0 && (x.pos(0).x < x.pos(1).x) == (in.x > 0)) {
                        p.corridor_turn_dir() = ivec2{in.x, 0};
                        p.corridor_turn_counter() = 0;
                        p.can_corridor_jump() = 0;
                    } else {
                        braced = x.solid(1, -p.flip_direction(), 0) && !x.solid(0, p.flip_direction(), 0);
                    }
                }
                if (braced)
                    p.can_corridor_jump() = 5;
                else if (p.can_corridor_jump() > 0)
                    --p.can_corridor_jump();

                // corridor boosts (zero gravity variants not ported)
                if (in.jmp && !x.in(1).jmp && p.slow_movement_stun() < 1 && g > 0.f && p.vertical_corridor_slide_counter() < 1) {
                    if (vertical && in.y > -1 && x.pos(0).y > x.pos(1).y) {
                        x.vel(0).y += 15.f;
                        x.vel(1).y += 10.f;
                        if (p.can_corridor_jump() > 0) {
                            p.shoot_up_counter() = 30;
                            p.vertical_corridor_slide_counter() = 22;
                            p.slow_movement_stun() = 2;
                        } else {
                            p.vertical_corridor_slide_counter() = 34;
                            p.slow_movement_stun() = 18;
                        }
                        p.can_corridor_jump() = 0;
                    } else if (horizontal) {
                        p.flip_direction() = x.pos(0).x > x.pos(1).x ? 1 : -1;
                        float const fd = float(p.flip_direction());
                        if (in.x == p.flip_direction() || in.x == 0) {
                            if (p.can_corridor_jump() > 0 && in.x != 0) {
                                x.pos(0).x += 12.f * fd;
                                x.pos(1).x += 12.f * fd;
                                x.pos(0).y = x.mid(x.pos(0)).y;
                                x.pos(1).y = x.pos(0).y;
                                x.vel(0).x += 7.f * fd;
                                x.vel(1).x += 7.f * fd;
                                p.horizontal_corridor_slide_counter() = 25;
                                p.slow_movement_stun() = 5;
                            } else {
                                x.vel(0).x += 6.f * fd;
                                x.vel(1).x += 4.f * fd;
                                p.horizontal_corridor_slide_counter() = 15;
                                p.slow_movement_stun() = 15;
                            }
                        }
                    }
                }
                if (p.vertical_corridor_slide_counter() == 1 || p.horizontal_corridor_slide_counter() == 1)
                    p.slow_movement_stun() = 15;
                float const slide = mathf::inverse_lerp(0.f, 10.f, float(std::max(p.vertical_corridor_slide_counter(), p.horizontal_corridor_slide_counter())));
                float const sf = x.surface_friction();
                x.vel(0) *= 0.9f - 0.3f * sf * (1.f - slide);
                x.vel(1) *= 0.9f - 0.3f * sf * (1.f - slide);
                float const antigrav = x.gravity() * mathf::clamp(sf * 8.f, 0.2f, 1.f)
                    * mathf::lerp(1.f, 0.2f, mathf::inverse_lerp(0.f, 10.f, float(p.vertical_corridor_slide_counter())));
                x.vel(0).y += antigrav;
                x.vel(1).y += antigrav;
                float const buoyancy = x.state.world.physical_object_get_buoyancy(x.o);
                x.vel(0).y -= buoyancy * x.submersion(0);
                x.vel(1).y -= buoyancy * x.submersion(1);
                p.dynamic_run_speed()[0] = 0.f;
                p.dynamic_run_speed()[1] = 0.f;

                float speed = 2.4f * mathf::clamp(sf + 0.2f, 0.2f, 0.5f) * p.stats().corridor_climb_speed_fac;
                speed *= mathf::lerp(0.1f, 1.f, mathf::inverse_lerp(10.f, 0.f, float(p.slow_movement_stun())));
                if (in.x != 0 && in.y != 0)
                    speed *= 0.4f;
                if (in.x != 0 && (in.x > 0) == (x.pos(0).x < x.pos(1).x))
                    p.backwards_counter() += 2;
                else if (in.y > 0 && x.pos(0).y < x.pos(1).y)
                    p.backwards_counter() += 2;
                if (p.backwards_counter() > 20)
                    p.backwards_counter() = 20;
                if (p.backwards_counter() > 10)
                    speed *= 0.6f;
                for (int i = 0; i < 2; ++i) {
                    if (in.x != 0 && !x.solid(i, in.x, 0)) {
                        x.vel(i).x += speed * float(in.x);
                        float const off = x.pos(i).y - x.mid(x.tile_pos(x.pos(i))).y;
                        x.vel(i).y = x.vel(i).y * 0.8f - off * 0.2f;
                        if (in.y == 0)
                            x.vel(1 - i).y = x.vel(1 - i).y * 0.8f - off * 0.2f;
                        break;
                    }
                    if (in.y != 0 && !x.solid(i, 0, in.y)) {
                        x.vel(i).y += speed * float(in.y);
                        float const off = x.pos(i).x - x.mid(x.tile_pos(x.pos(i))).x;
                        x.vel(i).x = x.vel(i).x * 0.8f - off * 0.2f;
                        if (in.x == 0)
                            x.vel(1 - i).x = x.vel(1 - i).x * 0.8f - off * 0.2f;
                        break;
                    }
                }
                x.vel(0).x += speed * float(in.x) * 0.1f;
                x.vel(0).y += speed * float(in.y) * 0.1f;
                p.standing() = x.pos(0).y > x.pos(1).y + 5.f;
                if ((in.x != 0 || in.y != 0) && p.vertical_corridor_slide_counter() < 16)
                    ++p.animation_frame();
                else
                    p.animation_frame() = 0;
                if (p.animation_frame() > 12)
                    p.animation_frame() = 1;
                if (in.y > 0 && p.shoot_up_counter() < 1 && (!x.solid(0, -1, 0) || !x.solid(0, 1, 0)) && x.tile(x.pos(0)).vertical_beam()) {
                    p.body_mode() = body_mode_index::climbing_on_beam;
                    p.animation() = animation_index::climb_on_beam;
                    return;
                }
                if (in.x != 0 && x.tile(x.pos(0)).horizontal_beam() && x.solid(1, 0, -1) && x.solid(1, 0, 1) && !x.solid(0, in.x, -1)) {
                    p.body_mode() = body_mode_index::climbing_on_beam;
                    p.animation() = animation_index::hang_from_beam;
                    return;
                }
            } else if (p.body_mode() == body_mode_index::wall_climb) {
                if (x.pos(0).y > x.pos(1).y) {
                    if (in.x != 0 && in.x == x.contact(0).x)
                        p.can_wall_jump() = in.x * -15;
                    p.can_jump() = 1;
                }
                if (in.x != 0 && x.solid(0, in.x, 0) && x.solid(1, in.x, 0)) {
                    // sliding down a wall slowly at first
                    float const k = x.gravity() * custom::lerp_map(float(p.wall_slide_counter()), 0.f, 30.f, 0.8f, 0.f) * g;
                    x.pos(0).y += k;
                    x.pos(1).y += k;
                }
                x.vel(1).y += x.submersion(1) * g;
            } else if (p.body_mode() == body_mode_index::climbing_on_beam) {
                if ((p.animation() == animation_index::get_up_on_beam || p.animation() == animation_index::stand_on_beam) && !x.tile(x.pos(1)).horizontal_beam()) {
                    vec2 const ahead = x.pos(1) + vec2{float(p.flip_direction()) * 20.f, 0.f};
                    vec2 const behind = x.pos(1) - vec2{float(p.flip_direction()) * 20.f, 0.f};
                    if (p.force_feet_to_horizontal_beam_tile() > 0 && x.tile(ahead).horizontal_beam()) {
                        x.pos(1).x = x.mid(ahead).x - float(p.flip_direction()) * 8.f;
                        x.vel(1).x = 0.f;
                        p.force_feet_to_horizontal_beam_tile() = 20;
                    } else if (p.force_feet_to_horizontal_beam_tile() > 0 && x.tile(behind).horizontal_beam()) {
                        x.pos(1).x = x.mid(behind).x + float(p.flip_direction()) * 8.f;
                        x.vel(1).x = 0.f;
                        p.force_feet_to_horizontal_beam_tile() = 20;
                    } else if (!x.tile(x.pos(1) + vec2{0.f, 20.f}).horizontal_beam() || p.animation() != animation_index::get_up_on_beam) {
                        p.animation() = animation_index::none;
                    }
                }
                if (p.animation() != animation_index::beam_tip && p.animation() != animation_index::climb_on_beam && p.animation() != animation_index::get_up_on_beam
                && p.animation() != animation_index::hang_from_beam && p.animation() != animation_index::stand_on_beam
                && p.animation() != animation_index::get_up_to_beam_tip && p.animation() != animation_index::hang_under_vertical_beam)
                    p.body_mode() = body_mode_index::default_;
            } else if (p.body_mode() == body_mode_index::swimming) {
                gravitate_to_opening(x);
                x.go_through_floors(true);
            }
        }
    }
}

namespace player {
    namespace {
        // UnityEngine.Vector3.Slerp restricted to the XY plane
        vec2 slerp(vec2 a, vec2 b, float t) {
            t = mathf::clamp01(t);
            float const ma = rw::magnitude(a);
            float const mb = rw::magnitude(b);
            if (ma < 1e-6f || mb < 1e-6f)
                return rw::lerp(a, b, t);
            vec2 const na = a / ma;
            vec2 const nb = b / mb;
            float const theta = std::acos(mathf::clamp(rw::dot(na, nb), -1.f, 1.f));
            if (theta < 1e-4f)
                return rw::lerp(a, b, t);
            float const s = std::sin(theta);
            vec2 const dir = (na * std::sin((1.f - t) * theta) + nb * std::sin(t * theta)) / s;
            return dir * (ma + (mb - ma) * t);
        }

        // Player.UpdateAnimation
        void update_animation(ctx& x) {
            pref& p = x.p;
            auto& in = x.in(0);
            auto const& in1 = x.in(1);
            float const g = x.eff_gravity();
            if (p.long_belly_slide() && p.animation() != animation_index::belly_slide)
                p.long_belly_slide() = false;
            if (p.stop_rolling_counter() > 0 && p.animation() != animation_index::roll)
                p.stop_rolling_counter() = 0;
            if (p.slide_up_pole() > 0 && p.animation() != animation_index::climb_on_beam)
                p.slide_up_pole() = 0;

            switch (p.animation()) {
            case animation_index::none:
                break;
            case animation_index::crawl_turn:
                p.body_mode() = body_mode_index::default_;
                x.vel(0).x += float(p.flip_direction());
                x.vel(1).x -= 2.f * float(p.flip_direction());
                if ((in.x > 0) != (x.pos(0).x < x.pos(1).x)) {
                    x.vel(0).y -= 3.f;
                    if (x.pos(0).y < x.pos(1).y + 2.f) {
                        p.animation() = animation_index::none;
                        x.vel(0).y -= 1.f;
                    }
                } else {
                    x.vel(0).y += 2.f;
                }
                if (in.x == 0 || x.solid(1, 0, 1))
                    p.animation() = animation_index::none;
                break;
            case animation_index::stand_up:
                if (!p.standing()) {
                    p.animation() = animation_index::down_on_fours;
                    break;
                }
                x.vel(0).x *= 0.7f;
                if (x.solid(0, 0, 1) || (x.on_slope(1) != 0 && in.x != 0)) {
                    p.animation() = animation_index::none;
                    break;
                }
                p.body_mode() = body_mode_index::stand;
                if (x.pos(0).y > x.pos(1).y + 3.f)
                    p.animation() = animation_index::none;
                break;
            case animation_index::down_on_fours:
                if (p.standing()) {
                    p.animation() = animation_index::stand_up;
                    break;
                }
                x.vel(0).y -= 2.f;
                x.vel(0).x += float(p.flip_direction());
                x.vel(1).x -= float(p.flip_direction());
                if (x.pos(0).y < x.pos(1).y || x.contact(0).y == -1)
                    p.animation() = animation_index::none;
                break;
            case animation_index::ledge_crawl:
                x.vel(0).x += float(p.flip_direction()) * 2.f;
                p.body_mode() = body_mode_index::crawl;
                // (vanilla compares chunk 0's tile row with itself here)
                if (!x.solid(0, p.flip_direction(), 0) && ((x.solid(0, 0, -1) && x.solid(1, 0, -1))
                || (x.contact(0).x == p.flip_direction() && in.x != 0) || (x.contact(0).y > -1 && x.contact(1).y > -1)))
                    p.animation() = animation_index::none;
                break;
            case animation_index::ledge_grab:
                p.body_mode() = body_mode_index::default_;
                if (x.solid(0, p.flip_direction(), 0) && !x.solid(0, p.flip_direction(), 1)) {
                    x.vel(0) *= 0.5f;
                    x.pos(0) = (x.pos(0) + (x.mid(x.tile_pos(x.pos(0))) + vec2{float(p.flip_direction()) * float(p.ledge_grab_counter()), 8.f + float(p.ledge_grab_counter())})) / 2.f;
                    x.last_pos(0) = x.pos(0);
                    x.vel(1).x += float(p.flip_direction());
                    p.can_jump() = 1;
                    if (in.x == p.flip_direction() || in.y == 1) {
                        ++p.ledge_grab_counter();
                        x.vel(1) += vec2{-0.5f * float(p.flip_direction()), -0.5f};
                    } else if (p.ledge_grab_counter() > 0) {
                        --p.ledge_grab_counter();
                    }
                    if (in.y == -1 && in1.y != -1) {
                        x.pos(0).y -= 10.f;
                        x.in(1).y = -1;
                        p.animation() = animation_index::none;
                        p.ledge_grab_counter() = 0;
                    } else if (in.x == -p.flip_direction() && in1.x == 0) {
                        x.vel(0).y += 10.f;
                        p.animation() = animation_index::none;
                        p.ledge_grab_counter() = 0;
                    }
                    p.standing() = true;
                    break;
                }
                p.animation() = animation_index::none;
                p.ledge_grab_counter() = 0;
                break;
            case animation_index::hang_from_beam: {
                p.body_mode() = body_mode_index::climbing_on_beam;
                p.standing() = true;
                x.vel(0).y = 0.f;
                x.vel(0).x *= 0.2f;
                x.pos(0).y = x.mid(x.pos(0)).y;
                if (in.x != 0 && x.contact(0).x != in.x) {
                    if (x.contact(1).x != in.x)
                        x.vel(0).x += float(in.x) * 1.2f * p.stats().pole_climb_speed_fac * custom::lerp_map(float(p.slow_movement_stun()), 0.f, 10.f, 1.f, 0.5f);
                    if (++p.animation_frame() > 20) {
                        p.animation_frame() = 1;
                        x.aerobic_increase(0.05f);
                    }
                    x.vel(1).x += float(p.flip_direction()) * (0.5f + 0.5f * std::sin(float(p.animation_frame()) / 20.f * mathf::pi * 2.f)) * -0.5f;
                } else if (p.animation_frame() < 10) {
                    ++p.animation_frame();
                } else if (p.animation_frame() > 10) {
                    --p.animation_frame();
                }
                bool pull_up = false;
                if (in.y < 0 && in1.y == 0) {
                    p.animation() = animation_index::none;
                } else if (in.y > 0 && in1.y == 0) {
                    if (x.tile(x.pos(0)).vertical_beam()) {
                        p.animation() = animation_index::climb_on_beam;
                        p.flip_direction() = x.pos(0).x < x.mid(x.pos(0)).x ? -1 : 1;
                    } else {
                        pull_up = true;
                    }
                } else if (in.jmp && !in1.jmp) {
                    pull_up = true;
                }
                if (pull_up) {
                    p.animation() = animation_index::get_up_on_beam;
                    p.pullup_softlock_safety() = 0;
                    p.straight_up_on_horizontal_beam() = false;
                    auto const side = [&] { return x.tile(x.pos(0) + vec2{float(p.flip_direction()) * 20.f, 0.f}); };
                    if (side().t == terrain::solid || !side().horizontal_beam())
                        p.flip_direction() = -p.flip_direction();
                    if (side().t == terrain::solid || !side().horizontal_beam()) {
                        p.flip_direction() = -p.flip_direction();
                        p.straight_up_on_horizontal_beam() = true;
                    }
                    if (!p.straight_up_on_horizontal_beam() && x.tile(x.pos(0) + vec2{float(p.flip_direction()) * 20.f, 20.f}).solid())
                        p.straight_up_on_horizontal_beam() = true;
                    p.up_on_horizontal_beam_pos() = vec2{x.pos(0).x, x.mid(x.pos(0)).y + 20.f};
                }
                if (!x.tile(x.pos(0)).horizontal_beam())
                    p.animation() = animation_index::none;
                break;
            }
            case animation_index::get_up_on_beam:
                if (++p.pullup_softlock_safety() > 200) {
                    p.pullup_softlock_safety() = 0;
                    p.animation() = animation_index::none;
                    break;
                }
                p.body_mode() = body_mode_index::climbing_on_beam;
                x.vel(0) = vec2{0.f, 0.f};
                p.force_feet_to_horizontal_beam_tile() = 20;
                if (p.straight_up_on_horizontal_beam()) {
                    if (in.y < 0 || x.contact(0).y > 0)
                        p.straight_up_on_horizontal_beam() = false;
                    if (x.tile(p.up_on_horizontal_beam_pos()).solid()) {
                        for (int j = 1; j >= -1; j -= 2) {
                            if (!x.tile(p.up_on_horizontal_beam_pos() + vec2{float(p.flip_direction() * j) * 20.f, 0.f}).solid()) {
                                p.up_on_horizontal_beam_pos().x += float(p.flip_direction() * j) * 20.f;
                                break;
                            }
                        }
                    }
                    x.vel(0) += custom::dir_vec(x.pos(0), p.up_on_horizontal_beam_pos()) * 1.8f;
                    x.vel(1) += custom::dir_vec(x.pos(1), p.up_on_horizontal_beam_pos() + vec2{0.f, -20.f}) * 1.8f;
                    if (x.tile(x.pos(1)).horizontal_beam() && x.pos(1).y > p.up_on_horizontal_beam_pos().y - 25.f) {
                        p.no_grab_counter() = 15;
                        p.animation() = animation_index::stand_on_beam;
                        x.pos(1).y = x.mid(x.pos(1)).y + 5.f;
                        x.vel(1).y = 0.f;
                        break;
                    }
                    if ((!x.tile(x.pos(0)).horizontal_beam() && !x.tile(x.pos(1)).horizontal_beam())
                    || !custom::dist_less(x.pos(0), p.up_on_horizontal_beam_pos(), 25.f))
                        p.animation() = animation_index::none;
                } else {
                    x.pos(0).y = x.mid(x.pos(0)).y;
                    x.vel(1).y += 2.f;
                    x.vel(1).x += float(p.flip_direction()) * 0.5f;
                    float const side = mathf::sign(x.pos(1).x - x.pos(0).x);
                    if (x.pos(1).y > x.pos(0).y - 15.f && !x.tile(x.pos(0) + vec2{side * 35.f, 0.f}).horizontal_beam()
                    && x.tile(x.pos(0) + vec2{side * -15.f, 0.f}).horizontal_beam()) {
                        x.vel(0).x -= side * 1.5f;
                        x.vel(1).x -= side * 0.5f;
                    }
                    if (x.contact(1).y > 0) {
                        if (!x.tile(x.pos(0) + vec2{0.f, 20.f}).solid())
                            p.straight_up_on_horizontal_beam() = true;
                        else
                            p.animation() = animation_index::hang_from_beam;
                    }
                    if (x.pos(1).y > x.pos(0).y) {
                        p.no_grab_counter() = 15;
                        p.animation() = animation_index::stand_on_beam;
                        x.pos(1).y = x.mid(x.pos(0)).y + 5.f;
                        x.vel(1).y = 0.f;
                    }
                    if (!x.tile(x.pos(0)).horizontal_beam()) {
                        p.pullup_softlock_safety() = 0;
                        p.animation() = animation_index::none;
                    }
                }
                break;
            case animation_index::stand_on_beam:
                p.body_mode() = body_mode_index::climbing_on_beam;
                p.standing() = true;
                p.can_jump() = 5;
                x.vel(1).x *= 0.5f;
                if (x.contact(0).y < 1 || !x.solid(1, 0, 1)) {
                    x.vel(1).y = 0.f;
                    x.pos(1).y = x.mid(x.pos(1)).y + 5.f;
                    x.vel(0).y += 2.f;
                    p.dynamic_run_speed()[0] = 2.1f * p.stats().runspeed_fac;
                    p.dynamic_run_speed()[1] = 2.1f * p.stats().runspeed_fac;
                    if (in.y < 0 && in1.y == 0)
                        p.animation() = animation_index::none;
                } else {
                    p.animation() = animation_index::none;
                }
                if (in.x != 0)
                    ++p.animation_frame();
                else
                    p.animation_frame() = 0;
                if (p.animation_frame() > 6)
                    p.animation_frame() = 0;
                if (in.y == 1 && in1.y == 0 && x.tile(x.tile_pos(x.pos(0)) + ivec2{0, 1}).horizontal_beam()) {
                    x.pos(0).y += 8.f;
                    x.pos(1).y += 8.f;
                    p.animation() = animation_index::hang_from_beam;
                }
                break;
            case animation_index::climb_on_beam: {
                p.body_mode() = body_mode_index::climbing_on_beam;
                p.standing() = true;
                p.can_jump() = 1;
                for (int k = 0; k < 2; ++k)
                    if (x.contact(k).x != 0)
                        p.flip_direction() = -x.contact(k).x;
                x.vel(0).x = 0.f;
                bool hug_side = true;
                if (!x.solid(0, 0, 1) && in.y > 0 && (x.contact(0).y < 0 || x.solid(0, p.flip_direction(), 1)))
                    hug_side = false;
                if (hug_side && x.solid(0, p.flip_direction(), 0))
                    p.flip_direction() = -p.flip_direction();
                // note: chunk 1 uses the tile middle of chunk 0's *updated* position
                float const side = hug_side ? float(p.flip_direction()) * 5.f : 0.f;
                x.pos(0).x = (x.pos(0).x + x.mid(x.pos(0)).x + side) / 2.f;
                x.pos(1).x = (x.pos(1).x * 7.f + x.mid(x.pos(0)).x + side) / 8.f;
                x.vel(0).y *= 0.5f;
                if (in.y > 0) {
                    if (++p.animation_frame() > 20) {
                        p.animation_frame() = 0;
                        x.aerobic_increase(0.1f);
                    }
                    x.vel(0).y += p.stats().pole_climb_speed_fac * custom::lerp_map(float(p.slow_movement_stun()), 0.f, 10.f, 1.f, 0.2f);
                } else if (in.y < 0) {
                    x.vel(0).y -= 2.2f * (0.2f + 0.8f * g);
                }
                x.vel(0).y += 1.f + x.gravity();
                x.vel(1).y -= 1.f - x.gravity();
                if (p.slide_up_pole() > 0) {
                    --p.slide_up_pole();
                    if (p.slide_up_pole() > 8)
                        p.animation_frame() = 12;
                    if (p.slide_up_pole() == 0)
                        p.slow_movement_stun() = std::max(p.slow_movement_stun(), 16);
                    if (p.slide_up_pole() > 14) {
                        x.pos(0).y += 2.f;
                        x.pos(1).y += 2.f;
                    }
                    x.vel(0).y += custom::lerp_map(float(p.slide_up_pole()), 17.f, 0.f, 3.f, -1.2f, 0.45f);
                    x.vel(1).y += custom::lerp_map(float(p.slide_up_pole()), 17.f, 0.f, 1.5f, -1.4f, 0.45f);
                }
                x.go_through_floors(in.x == 0 && in.down_diagonal == 0);
                if (in.x != 0 && in1.x != in.x && in.x == p.flip_direction() && in.x == p.last_flip_direction()) {
                    if (x.tile(x.pos(0)).horizontal_beam() && !x.solid(0, 0, -1))
                        p.animation() = animation_index::hang_from_beam;
                    else if (x.tile(x.pos(1)).horizontal_beam())
                        p.animation() = animation_index::stand_on_beam;
                }
                // hop to an adjacent pole
                if (in.x == p.flip_direction() && in1.x == 0 && p.flip_direction() == p.last_flip_direction()
                && x.tile(x.tile_pos(x.pos(0)) + ivec2{p.flip_direction(), 0}).vertical_beam()) {
                    x.pos(0).x = x.mid(x.tile_pos(x.pos(0)) + ivec2{p.flip_direction(), 0}).x - float(p.flip_direction()) * 5.f;
                    p.flip_direction() = -p.flip_direction();
                    p.jump_stun() = 11 * p.flip_direction();
                }
                if (x.contact(1).y < 0 && in.y < 0)
                    p.animation() = animation_index::stand_up;
                if (!x.tile(x.pos(0)).vertical_beam()) {
                    p.animation() = animation_index::none;
                    if (x.tile(x.tile_pos(x.pos(0)) + ivec2{0, -1}).vertical_beam())
                        p.animation() = animation_index::get_up_to_beam_tip;
                    else if (x.tile(x.tile_pos(x.pos(0)) + ivec2{0, 1}).vertical_beam())
                        p.animation() = animation_index::hang_under_vertical_beam;
                }
                break;
            }
            case animation_index::get_up_to_beam_tip: {
                p.body_mode() = body_mode_index::climbing_on_beam;
                p.standing() = true;
                p.can_jump() = 5;
                x.vel(0).y += x.gravity();
                x.vel(1).y += x.gravity();
                vec2 tip{0.f, 0.f};
                for (int l = 0; l < 2; ++l) {
                    if (!x.tile(x.pos(l)).vertical_beam() && x.tile(x.pos(l) + vec2{0.f, -20.f}).vertical_beam()) {
                        tip = x.mid(x.pos(l));
                        break;
                    }
                }
                if (tip.x == 0.f && tip.y == 0.f) {
                    p.animation() = animation_index::none;
                    break;
                }
                x.pos(0).x = (x.pos(0).x * 14.f + tip.x) / 15.f;
                x.pos(1).x = (x.pos(1).x * 4.f + tip.x) / 5.f;
                x.vel(0).y += 0.1f;
                x.pos(1).y = (x.pos(1).y * 4.f + tip.y) / 5.f;
                if (custom::dist_less(x.pos(1), tip, 6.f))
                    p.animation() = animation_index::beam_tip;
                break;
            }
            case animation_index::beam_tip:
                p.body_mode() = body_mode_index::climbing_on_beam;
                p.standing() = true;
                p.can_jump() = 5;
                x.vel(1) *= 0.5f;
                x.pos(1) = (x.pos(1) + x.mid(x.pos(1))) / 2.f;
                x.vel(0).y += 1.5f;
                x.vel(0).y += float(in.y) * 0.1f;
                x.vel(0).x += float(in.x) * 0.1f;
                if (in.y > 0 && in1.y == 0) {
                    x.vel(1).y -= 1.f;
                    p.can_jump() = 0;
                    p.animation() = animation_index::none;
                }
                if ((in.y < 0 && in1.y == 0) || x.pos(0).y < x.pos(1).y || !x.tile(x.pos(1) + vec2{0.f, -20.f}).vertical_beam())
                    p.animation() = animation_index::none;
                break;
            case animation_index::hang_under_vertical_beam:
                p.body_mode() = body_mode_index::climbing_on_beam;
                p.standing() = false;
                if ((in.y < 0 && in1.y == 0) || rw::magnitude(x.vel(1)) > 10.f || rw::magnitude(x.vel(0)) > 10.f
                || !x.tile(x.pos(0) + vec2{0.f, 20.f}).vertical_beam()) {
                    p.animation() = animation_index::none;
                    p.standing() = true;
                } else {
                    x.pos(0).x = mathf::lerp(x.pos(0).x, x.mid(x.pos(0)).x, 0.5f);
                    x.pos(0).y = mathf::max(x.pos(0).y, x.mid(x.pos(0)).y + 5.f + x.vel(0).y);
                    x.vel(0).x *= 0.f;
                    x.vel(0).y *= 0.5f;
                    x.vel(1).x += float(in.x);
                    if (in.y > 0)
                        x.vel(0).y += 2.5f;
                    if (x.tile(x.pos(0)).vertical_beam())
                        p.animation() = animation_index::climb_on_beam;
                }
                if (in.jmp && !in1.jmp) {
                    p.animation() = animation_index::none;
                    if (in.x == 0) {
                        x.pos(0).y += 16.f;
                        x.vel(0).y = 10.f;
                        p.standing() = true;
                    } else {
                        x.vel(1).y += 4.f;
                        x.vel(1).x += 2.f * float(in.x);
                        x.vel(0).y += 6.f;
                        x.vel(0).x += 3.f * float(in.x);
                    }
                }
                break;
            case animation_index::deep_swim: {
                p.dynamic_run_speed()[0] = 0.f;
                p.dynamic_run_speed()[1] = 0.f;
                p.can_jump() = 0;
                p.standing() = false;
                x.go_through_floors(true);
                vec2 const axis = rw::normalized(x.pos(0) - x.pos(1));
                float const aligned = (std::fabs(rw::dot(rw::normalized(x.vel(0)), axis)) + std::fabs(rw::dot(rw::normalized(x.vel(1)), axis))) / 2.f;
                // swim boost
                bool const can_boost = x.rivulet() ? p.submerged() != 0 : p.air_in_lungs() > p.stats().swim_boost_min_air;
                if (in.jmp && !in1.jmp && can_boost) {
                    if (p.water_jump_delay() == 0) {
                        p.swim_cycle() = 2.7f;
                        vec2 const boost_dir = custom::dir_vec(x.pos(1), x.pos(0));
                        if (x.rivulet())
                            x.vel(0) += boost_dir * (boost_dir.y > 0.5f ? 300.f : 50.f);
                        else
                            x.vel(0) += boost_dir * p.stats().swim_boost_force;
                        p.air_in_lungs() -= p.stats().swim_boost_cost;
                    } else {
                        p.swim_cycle() = 0.f;
                    }
                    p.water_jump_delay() = p.stats().swim_boost_cooldown;
                }
                p.swim_cycle() += 0.01f;
                ivec2 const iv = in.zero_g_gamepad_int_vec();
                if (iv.x != 0 || iv.y != 0) {
                    float const bend = rw::angle(x.last_pos(0) - x.last_pos(1), x.pos(0) - x.pos(1));
                    float force = 0.2f + mathf::inverse_lerp(0.f, 12.f, bend) * 0.8f;
                    if (p.slow_movement_stun() > 0)
                        force *= 0.5f;
                    if (force > p.swim_force())
                        p.swim_force() = mathf::lerp(p.swim_force(), force, 0.7f);
                    else
                        p.swim_force() = mathf::lerp(p.swim_force(), force, 0.05f);
                    p.swim_cycle() += mathf::lerp(p.swim_force(), 1.f, 0.5f) / 10.f;
                    if (p.air_in_lungs() < 0.5f && p.air_in_lungs() > 1.f / 6.f)
                        p.swim_cycle() += 0.05f;
                    if (x.contact(0) != ivec2{0, 0})
                        p.swim_force() *= 0.5f;
                    vec2 const stroke = custom::dir_vec(x.pos(1), x.pos(0)) * 0.7f * mathf::lerp(p.swim_force() * p.stats().swim_force_fac, 1.f, 0.5f) * x.submersion(0);
                    if (p.swim_cycle() > 4.f)
                        p.swim_cycle() = 0.f;
                    else if (p.swim_cycle() > 3.f)
                        x.vel(0) += stroke;
                    vec2 dir = swim_dir(x, true);
                    if (p.air_in_lungs() < p.stats().drown_threshold)
                        dir = slerp(dir, vec2{0.f, 1.f}, mathf::inverse_lerp(p.stats().drown_threshold, 0.f, p.air_in_lungs()));
                    float const sub0 = x.submersion(0);
                    x.vel(0) += dir * (0.5f * p.swim_force() * mathf::lerp(aligned, 1.f, 0.5f) * sub0 * p.stats().swim_force_fac);
                    x.vel(1) -= dir * (0.1f * sub0);
                    x.vel(0) += custom::dir_vec(x.pos(1), x.pos(0)) * (0.4f * p.swim_force() * aligned * sub0 * p.stats().swim_force_fac);
                    if (rw::magnitude(x.vel(0)) < 3.f) {
                        float const slow = mathf::inverse_lerp(3.f, 1.5f, rw::magnitude(x.vel(0)));
                        x.vel(0) += dir * (0.2f * slow);
                        x.vel(1) -= dir * (0.1f * slow);
                    }
                }
                x.water_friction() = x.rivulet() && p.water_jump_delay() >= 5 ? 0.99f : mathf::lerp(0.92f, 0.96f, aligned);
                if (p.body_mode() != body_mode_index::swimming)
                    p.animation() = animation_index::none;
                break;
            }
            case animation_index::surface_swim:
                p.can_jump() = 0;
                p.swim_cycle() += 0.025f;
                x.water_friction() = x.rivulet() && p.water_jump_delay() >= 5 ? 0.999f : 0.96f;
                p.swim_force() *= 0.5f;
                if (in.y > -1 && x.vel(0).y > -5.f && x.vel(0).y < 3.f && p.water_jump_delay() == 0 && !in.jmp) {
                    // bob at the surface
                    x.vel(0).y *= 0.8f;
                    x.vel(1).y *= 0.8f;
                    x.vel(0).y += mathf::clamp((x.pos(0).y - (x.room.get_float_water_level(x.pos(0)) + 15.f)) * -0.1f, -0.5f, 1.5f);
                    x.vel(1).y -= 0.5f;
                } else if (in.y == -1) {
                    x.vel(0).y -= 0.2f;
                    x.vel(1).y += 0.1f;
                } else if (in.y == 1) {
                    x.vel(0).y += 0.5f;
                }
                p.dynamic_run_speed()[0] = x.rivulet() ? 5.f : 2.7f;
                p.dynamic_run_speed()[1] = 0.f;
                if (in.x != 0) {
                    x.vel(1).x -= float(in.x) * 0.2f * (x.rivulet() ? 1.5f : 1.f);
                    p.swim_cycle() += 1.f / 30.f;
                }
                if (in.jmp && !in1.jmp) {
                    if (p.water_jump_delay() == 0) {
                        if (x.vel(0).y < 2.f && x.vel(1).y < 2.f) {
                            x.vel(0).y += x.rivulet() ? 18.f : 6.f;
                            x.vel(1).y += x.rivulet() ? 18.f : 6.f;
                        } else {
                            float const k = x.rivulet() ? 3.f : 1.f;
                            x.vel(0) += custom::dir_vec(x.pos(1), x.pos(0)) * 4.f * k;
                            x.vel(1).y += (x.vel(1).y < 4.f ? 3.f : 1.5f) * k;
                        }
                        p.water_jump_delay() = x.rivulet() ? 6 : 17;
                    } else {
                        if (p.water_jump_delay() < 10 && !x.rivulet())
                            p.water_jump_delay() = 10;
                        x.vel(0) += vec2{0.f, x.rivulet() ? 12.f : 4.f};
                        x.vel(1).x *= 0.75f;
                    }
                }
                if (p.body_mode() != body_mode_index::swimming)
                    p.animation() = animation_index::none;
                break;
            case animation_index::roll: {
                p.body_mode() = body_mode_index::default_;
                vec2 const a = custom::perpendicular_vector(x.pos(1), x.pos(0));
                float const rd = float(p.roll_direction());
                x.vel(0) *= 0.9f;
                x.vel(1) *= 0.9f;
                x.vel(0) += a * 2.f * rd;
                x.vel(1) -= a * 2.f * rd;
                x.aerobic_increase(0.01f);
                bool blocked = x.contact(0).x == p.roll_direction() || x.contact(1).x == p.roll_direction();
                if (x.on_slope(1) == -p.roll_direction() || x.on_slope(0) == -p.roll_direction()) {
                    x.pos(0) += a * rd;
                    x.pos(1) -= a * rd;
                }
                if (!x.solid(0, 0, -1) && !x.solid(1, 0, -1) && x.contact(0).y >= 0 && x.contact(1).y >= 0) {
                    if (x.solid(0, 0, -2) || x.solid(1, 0, -2)) {
                        x.vel(0) *= 0.7f;
                        x.vel(1) *= 0.7f;
                        x.pos(0).y -= 2.5f;
                        x.pos(1).y -= 2.5f;
                    } else {
                        blocked = true;
                    }
                } else {
                    x.vel(0).x += 1.1f * rd;
                    x.vel(1).x += 1.1f * rd;
                    p.can_jump() = std::max(p.can_jump(), 5);
                    // roll up single tile steps
                    for (int m = 0; m < 2; ++m) {
                        if (x.solid(m, p.roll_direction(), 0) && !x.solid(m, p.roll_direction(), 1) && !x.solid(0, 0, 1) && !x.solid(1, 0, 1)) {
                            x.vel(0) *= 0.7f;
                            x.vel(1) *= 0.7f;
                            x.pos(0).y += 5.f;
                            x.pos(1).y += 5.f;
                            x.vel(0).y += x.gravity();
                            x.vel(1).y += x.gravity();
                            blocked = false;
                            break;
                        }
                    }
                }
                if (blocked)
                    ++p.stop_rolling_counter();
                else
                    p.stop_rolling_counter() = 0;
                if ((((p.roll_counter() > 15 && in.y > -1 && in.down_diagonal == 0) || p.roll_counter() > 30 || in.x == -p.roll_direction())
                    && x.pos(0).y > x.pos(1).y) || p.roll_counter() > 60 || p.stop_rolling_counter() > 6) {
                    p.roll_direction() = 0;
                    p.animation() = animation_index::none;
                    p.standing() = in.y > -1;
                }
                break;
            }
            case animation_index::rocket_jump: {
                p.body_mode() = body_mode_index::default_;
                p.standing() = false;
                x.vel(1) *= 0.99f;
                vec2 const n = rw::normalized(x.vel(0));
                x.vel(0) += n;
                x.vel(1) -= n;
                x.vel(0).y += 0.1f;
                x.vel(1).y += 0.1f;
                if (x.contact(1) != ivec2{0, 0})
                    p.animation() = animation_index::none;
                break;
            }
            case animation_index::flip: {
                p.body_mode() = body_mode_index::default_;
                vec2 const a = custom::perpendicular_vector(x.pos(1), x.pos(0));
                float const k = float(p.slide_direction()) * 0.38f * (p.flip_from_slide() ? 2.5f : 1.f);
                x.vel(0) -= a * k;
                x.vel(1) += a * k;
                p.standing() = false;
                for (int n = 0; n < 2; ++n) {
                    if (x.contact(n) != ivec2{0, 0}) {
                        p.animation() = animation_index::none;
                        p.standing() = x.pos(0).y > x.pos(1).y;
                        break;
                    }
                }
                break;
            }
            case animation_index::belly_slide: {
                p.body_mode() = body_mode_index::default_;
                float const rd = float(p.roll_direction());
                if (p.roll_counter() < 6 && !x.rivulet()) {
                    x.vel(1).y += 2.7f;
                    x.vel(1).x -= 9.1f * rd;
                } else if (x.solid(1, 0, -1) || x.solid(1, 0, -2)) {
                    x.vel(1).y -= 0.5f;
                }
                float const long_push = x.rivulet() ? 20.f : 14.f;
                float const push = x.rivulet() ? 25.f : 18.1f;
                x.vel(0).x += (p.long_belly_slide() ? long_push : push) * rd * std::sin(float(p.roll_counter()) / (p.long_belly_slide() ? 39.f : 15.f) * mathf::pi);
                if (x.solid(0, 0, -1) || x.solid(0, 0, -2))
                    x.vel(0).y -= 2.3f;
                for (int n = 0; n < 2; ++n)
                    if (x.contact(n).y == 0)
                        x.vel(n).x *= x.surface_friction();
                // slide into a one tile hole in the floor
                if (in.y < 0 && in.down_diagonal == 0 && in.x == 0 && p.roll_counter() > 8 && x.tile_pos(x.pos(0)).y == x.tile_pos(x.pos(1)).y) {
                    ivec2 const tp = x.tile_pos(x.pos(0));
                    if (!x.tile(tp + ivec2{0, -1}).solid() && x.tile(tp + ivec2{-1, -1}).solid() && x.tile(tp + ivec2{1, -1}).solid()) {
                        x.pos(0) = x.mid(x.pos(0)) + vec2{0.f, -20.f};
                        x.vel(0) = vec2{0.f, -11.f};
                        x.pos(1) = rw::lerp(x.pos(1), x.pos(0) + vec2{0.f, x.connection_distance()}, 0.5f);
                        x.vel(1) = vec2{0.f, -11.f};
                        p.animation() = animation_index::none;
                        p.standing() = false;
                        x.go_through_floors(true);
                        p.roll_direction() = 0;
                        break;
                    }
                }
                if (in.x != p.roll_direction() && in.down_diagonal != p.roll_direction())
                    ++p.exit_belly_slide_counter();
                else
                    p.exit_belly_slide_counter() = 0;
                if (p.long_belly_slide())
                    p.whiplash_jump() = false;
                else if (p.roll_counter() > 5 && in.x == -p.roll_direction())
                    p.whiplash_jump() = true;
                int32_t const jump_window = x.rivulet() ? 6 : 12;
                int32_t const long_jump_window = x.rivulet() ? 20 : 34;
                if ((p.roll_counter() > 8 && p.exit_belly_slide_counter() > (p.long_belly_slide() ? 16 : 6))
                || p.roll_counter() > (p.long_belly_slide() ? 39 : 15)
                || (!p.long_belly_slide() && p.roll_counter() > 6 && !x.solid(0, 0, -1) && !x.solid(1, 0, -1))
                || (in.jmp && !in1.jmp && p.roll_counter() > 0 && p.roll_counter() < (p.long_belly_slide() ? long_jump_window : jump_window))) {
                    x.vel(0).y = 0.f;
                    x.vel(1).y = 0.f;
                    p.roll_direction() = 0;
                    p.animation() = animation_index::none;
                    if (p.long_belly_slide()) {
                        p.standing() = true;
                        x.vel(0).y = 6.f;
                        x.vel(1).y = 4.f;
                    } else {
                        p.standing() = in.y == 1 && !x.solid(0, 0, 1);
                        for (int n = 0; n < 2; ++n)
                            if (std::fabs(x.vel(n).x) > 8.f)
                                x.vel(n) *= 0.5f;
                        p.slow_movement_stun() = p.standing() ? 20 : 40;
                    }
                    p.long_belly_slide() = false;
                    break;
                }
                p.standing() = false;
                break;
            }
            case animation_index::corridor_turn:
                if (p.corridor_turn_dir().has_value() && p.body_mode() == body_mode_index::corridor_climb && p.corridor_turn_counter() < 40) {
                    p.slow_movement_stun() = std::max(10, p.slow_movement_stun());
                    x.vel(0) *= 0.5f;
                    x.vel(1) *= 0.5f;
                    if (p.corridor_turn_counter() < 30) {
                        x.vel(0) += custom::deg_to_vec(p.random().value() * 360.f);
                    } else {
                        x.vel(0) += rw::to_vec2(*p.corridor_turn_dir()) * 0.5f;
                        x.vel(1) -= rw::to_vec2(*p.corridor_turn_dir()) * 0.5f;
                    }
                    ++p.corridor_turn_counter();
                    break;
                }
                if (p.corridor_turn_dir().has_value()) {
                    x.vel(0) += rw::to_vec2(*p.corridor_turn_dir()) * 6.f;
                    x.vel(1) += rw::to_vec2(*p.corridor_turn_dir()) * 5.f;
                }
                p.corridor_turn_dir().reset();
                p.animation() = animation_index::none;
                break;
            case animation_index::dead:
                p.body_mode() = body_mode_index::dead;
                break;
            default:
                // antlers, tube worms, zero gravity and vines are not ported
                break;
            }
        }

        // Player.WallJump
        void wall_jump(ctx& x, int32_t direction) {
            pref& p = x.p;
            float k = 1.f;
            if (p.exhausted())
                k *= 1.f - 0.5f * p.aerobic_level();
            bool const ledge = x.in(0).x != 0 && x.contact(0).x == x.in(0).x && x.solid(0, x.in(0).x, 0) && !x.solid(0, x.in(0).x, 1);
            if (x.solid(1, 0, -1) || x.solid(0, 0, -1) || x.submersion(1) > 0.1f || ledge) {
                // close to the ground: plain upward jump
                if (x.contact(1).y > -1 && x.contact(0).y > -1 && x.object_submersion() == 0.f)
                    k *= 0.7f;
                x.vel(0).y = (x.rivulet() ? 9.f : 8.f) * k;
                x.vel(1).y = (x.rivulet() ? 8.f : 7.f) * k;
                x.sound(sound::sound_id::slugcat_normal_jump);
                x.pos(0).y += 10.f * mathf::min(1.f, k);
                x.pos(1).y += 10.f * mathf::min(1.f, k);
                p.jump_boost() = 0.f;
            } else {
                if (x.rivulet()) {
                    x.vel(0).y = 10.f * k;
                    x.vel(1).y = 9.f * k;
                    x.vel(0).x = 9.f * k * float(direction);
                    x.vel(1).x = 7.f * k * float(direction);
                } else {
                    x.vel(0).y = 8.f * k;
                    x.vel(1).y = 7.f * k;
                    x.vel(0).x = 6.f * k * float(direction);
                    x.vel(1).x = 5.f * k * float(direction);
                }
                p.standing() = true;
                p.jump_boost() = x.rivulet() ? 4.f : 0.f;
                p.jump_stun() = 8 * direction;
                x.sound(sound::sound_id::slugcat_wall_jump);
            }
            p.can_wall_jump() = 0;
        }

        // Player.Jump
        void jump(ctx& x) {
            pref& p = x.p;
            auto const& in = x.in(0);
            p.feet_stuck_pos().reset();
            float const k = 1.f;
            x.aerobic_increase(1.f);
            if (p.body_mode() == body_mode_index::wall_climb) {
                int32_t direction;
                if (p.can_wall_jump() != 0)
                    direction = rw::math_sign(p.can_wall_jump());
                else if (x.contact(0).x != 0)
                    direction = -x.contact(0).x;
                else
                    direction = -p.flip_direction();
                wall_jump(x, direction);
                return;
            }
            if (p.body_mode() == body_mode_index::corridor_climb) {
                x.vel(0).y = 6.f * k;
                x.vel(1).y = 5.f * k;
                p.standing() = true;
                p.jump_boost() = x.rivulet() ? 14.f : 8.f;
                return;
            }
            if (p.animation() == animation_index::ledge_grab) {
                if (in.x != 0)
                    wall_jump(x, -in.x);
                return;
            }
            if (p.animation() == animation_index::climb_on_beam) {
                p.jump_boost() = 0.f;
                if (in.x != 0) {
                    p.animation() = animation_index::none;
                    x.sound(sound::sound_id::slugcat_from_vertical_pole_jump);
                    float const fd = float(p.flip_direction());
                    if (x.rivulet()) {
                        x.vel(0).y = 9.f * k;
                        x.vel(1).y = 8.f * k;
                        x.vel(0).x = 9.f * fd * k;
                        x.vel(1).x = 7.f * fd * k;
                    } else {
                        x.vel(0).y = 8.f * k;
                        x.vel(1).y = 7.f * k;
                        x.vel(0).x = 6.f * fd * k;
                        x.vel(1).x = 5.f * fd * k;
                    }
                    return;
                }
                if (in.y <= 0) {
                    p.animation() = animation_index::none;
                    x.sound(sound::sound_id::slugcat_from_vertical_pole_jump, 0.3f);
                    x.vel(0).y = 2.f * k;
                    if (in.y > -1)
                        x.vel(0).x = 2.f * float(p.flip_direction()) * k;
                    return;
                }
                // shimmy up the pole
                if (p.slow_movement_stun() < 1 && p.slide_up_pole() < 1) {
                    for (int i = 0; i < 2; ++i) {
                        x.pos(i).y += 4.5f;
                        x.vel(i).y += 2.f;
                    }
                    p.slide_up_pole() = 17;
                }
                return;
            }
            if (p.animation() == animation_index::roll) {
                x.vel(1) *= 0.f;
                x.pos(1) += vec2{5.f * float(p.roll_direction()), 5.f};
                x.pos(0) = x.pos(1) + vec2{5.f * float(p.roll_direction()), 5.f};
                float const t = mathf::inverse_lerp(0.f, 25.f, float(p.roll_counter()));
                vec2 const v = custom::deg_to_vec(float(p.roll_direction()) * mathf::lerp(60.f, 35.f, t)) * mathf::lerp(9.5f, 13.1f, t) * k;
                x.vel(0) = v;
                x.vel(1) = v;
                x.vel(0).x *= x.rivulet() ? 1.5f : 1.f;
                x.vel(1).x *= x.rivulet() ? 1.5f : 1.f;
                p.animation() = animation_index::rocket_jump;
                p.roll_direction() = 0;
                x.sound(sound::sound_id::slugcat_rocket_jump);
                return;
            }
            if (p.animation() == animation_index::belly_slide) {
                float const speed = x.rivulet() ? 18.f : 9.f;
                if (!p.whiplash_jump() && in.x != -p.roll_direction()) {
                    float const up = x.rivulet() ? 10.f : 8.5f;
                    x.pos(1) += vec2{5.f * float(p.roll_direction()), 5.f};
                    x.pos(0) = x.pos(1) + vec2{5.f * float(p.roll_direction()), 5.f};
                    vec2 const v = vec2{float(p.roll_direction()) * speed, up} * k * (p.long_belly_slide() ? 1.2f : 1.f);
                    x.vel(1) = v;
                    x.vel(0) = v;
                    p.animation() = animation_index::rocket_jump;
                    p.rocket_jump_from_belly_slide() = true;
                    p.roll_direction() = 0;
                    x.sound(sound::sound_id::slugcat_rocket_jump);
                    return;
                }
                // whiplash: backflip out of the slide
                p.animation() = animation_index::flip;
                p.standing() = true;
                int32_t room_behind = 1;
                for (int32_t i = 1; i < 4; ++i) {
                    vec2 const probe = x.pos(0) + vec2{float(i * -p.roll_direction()) * 15.f, 0.f};
                    if (x.tile(probe).solid() || x.tile(probe + vec2{0.f, 20.f}).solid())
                        break;
                    room_behind = i;
                }
                float const rd = float(p.roll_direction());
                x.pos(0) += vec2{rd * -(float(room_behind) * 15.f + 8.f), 14.f};
                x.pos(1) += vec2{rd * -(float(room_behind) * 15.f + 2.f), 0.f};
                x.vel(0) = vec2{rd * (x.rivulet() ? -11.f : -7.f), x.rivulet() ? 12.f : 10.f};
                x.vel(1) = vec2{rd * (x.rivulet() ? -11.f : -7.f), x.rivulet() ? 13.f : 11.f};
                p.roll_direction() = -p.roll_direction();
                p.flip_from_slide() = true;
                p.whiplash_jump() = false;
                p.jump_boost() = 0.f;
                x.sound(sound::sound_id::slugcat_flip_jump);
                return;
            }
            if (p.animation() == animation_index::zero_g_swim || p.animation() == animation_index::zero_g_pole_grab || p.animation() == animation_index::antler_climb)
                return; // not ported

            int32_t dir = in.x;
            bool slide = false;
            if (p.animation() == animation_index::down_on_fours && x.contact(1).y < 0 && in.down_diagonal == p.flip_direction()) {
                p.animation() = animation_index::belly_slide;
                p.roll_direction() = p.flip_direction();
                p.roll_counter() = 0;
                p.standing() = false;
                slide = true;
                x.sound(sound::sound_id::slugcat_belly_slide_init);
            }
            if (slide)
                return;
            p.animation() = animation_index::none;
            if (p.standing()) {
                if (p.slide_counter() > 0 && p.slide_counter() < 10) {
                    // flip out of a skid
                    x.vel(0).y = (x.rivulet() ? 12.f : 9.f) * k;
                    x.vel(1).y = (x.rivulet() ? 10.f : 7.f) * k;
                    x.vel(0).x *= 0.5f;
                    x.vel(1).x *= 0.5f;
                    x.vel(0).x -= float(p.slide_direction()) * 4.f * k;
                    p.jump_boost() = x.rivulet() ? 9.f : 5.f;
                    p.animation() = animation_index::flip;
                    p.slide_counter() = 0;
                    x.sound(sound::sound_id::slugcat_flip_jump);
                } else {
                    x.vel(0).y = (x.rivulet() ? 6.f : 4.f) * k;
                    x.vel(1).y = (x.rivulet() ? 5.f : 3.f) * k;
                    p.jump_boost() = 8.f;
                    x.sound(p.body_mode() == body_mode_index::climbing_on_beam ? sound::sound_id::slugcat_from_horizontal_pole_jump
                        : sound::sound_id::slugcat_normal_jump);
                }
            } else {
                float horizontal = 1.5f;
                if (p.super_launch_jump() >= 20) {
                    p.super_launch_jump() = 0;
                    horizontal = x.rivulet() ? 12.f : 9.f;
                    dir = x.pos(0).x > x.pos(1).x ? 1 : -1;
                    p.simulate_hold_jump_button() = 6;
                }
                x.pos(0).y += 6.f;
                if (x.contact(0).y == -1) {
                    x.vel(0).y += 3.f * k;
                    if (dir == 0)
                        x.vel(0).y += 3.f * k;
                }
                x.vel(1).y += 4.f * k;
                p.jump_boost() = 6.f;
                x.sound(horizontal >= 9.f ? sound::sound_id::slugcat_super_jump : sound::sound_id::slugcat_crouch_jump);
                if (dir != 0 && (x.pos(0).x > x.pos(1).x) == (dir > 0)) {
                    x.vel(0).x += float(dir) * horizontal * k;
                    x.vel(1).x += float(dir) * horizontal * k;
                }
            }
            if (x.on_slope(1) != 0) {
                if (dir == -x.on_slope(1)) {
                    x.vel(1).x += float(x.on_slope(1)) * 8.f * k;
                    return;
                }
                x.vel(0).x += float(x.on_slope(1)) * 1.8f * k;
                x.vel(1).x += float(x.on_slope(1)) * 1.2f * k;
            }
        }

        // Player.LungUpdate (base game lung settings, no Artificer explosions)
        void lung_update(ctx& x) {
            pref& p = x.p;
            if (x.submersion(0) > 0.9f) {
                if (!p.submerged()) {
                    p.swim_force() = mathf::inverse_lerp(0.f, 8.f, std::fabs(x.vel(0).x));
                    p.swim_cycle() = 0.f;
                }
                bool const holding_up = x.in(0).y == 1 && x.in(0).x == 0 && p.air_in_lungs() < p.stats().drown_threshold;
                p.air_in_lungs() -= 1.f / (40.f * (p.lungs_exhausted() ? 4.5f : 9.f) * (holding_up ? 1.5f : 1.f)) * p.stats().lungs_fac;
                bool near_surface = x.room.get_float_water_level(x.pos(0)) - x.pos(0).y < 200.f;
                bool lenient = p.air_in_lungs() <= 0.f && x.in(0).y == 1 && near_surface;
                if (lenient) {
                    ivec2 const tp = x.tile_pos(x.pos(0));
                    for (int32_t l = tp.y; l <= x.room.default_water_level; ++l) {
                        if (x.room.get_tile(tp.x, l).solid()) {
                            lenient = false;
                            break;
                        }
                    }
                }
                if (p.air_in_lungs() <= (lenient ? -0.3f : 0.f) && x.submersion(0) == 1.f && x.submersion(1) > 0.5f) {
                    p.air_in_lungs() = 0.f;
                    stun_impl(p, 10);
                    p.drown() += 1.f / 120.f;
                    if (p.drown() >= 1.f)
                        die_impl(p);
                } else if (p.air_in_lungs() < p.stats().drown_threshold) {
                    if (p.slow_movement_stun() < 1)
                        p.slow_movement_stun() = 1;
                    if (p.random().value() < 0.5f)
                        x.vel(0) += custom::deg_to_vec(p.random().value() * 360.f) * p.random().value();
                    if (x.in(0).y < 1)
                        x.vel(1) *= mathf::lerp(1.f, 0.9f, mathf::inverse_lerp(0.f, p.stats().drown_threshold, p.air_in_lungs()));
                }
                p.submerged() = true;
            } else {
                if (p.submerged() && p.air_in_lungs() < p.stats().drown_threshold)
                    p.lungs_exhausted() = true;
                if (!p.lungs_exhausted() && p.air_in_lungs() > 0.9f)
                    p.air_in_lungs() = 1.f;
                if (p.air_in_lungs() <= 0.f)
                    p.air_in_lungs() = 0.f;
                p.air_in_lungs() += 1.f / float(p.lungs_exhausted() ? 240 : 60);
                if (p.air_in_lungs() >= 1.f) {
                    p.air_in_lungs() = 1.f;
                    p.lungs_exhausted() = false;
                    p.drown() = 0.f;
                }
                p.submerged() = false;
                if (p.lungs_exhausted() && p.animation() != animation_index::surface_swim)
                    p.swim_cycle() += 0.1f;
            }
            if (p.lungs_exhausted()) {
                if (p.slow_movement_stun() < 5)
                    p.slow_movement_stun() = 5;
                if (p.drown() > 0.f && p.slow_movement_stun() < 10)
                    p.slow_movement_stun() = 10;
            }
        }
    }
}
