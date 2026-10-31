#pragma once

// Plain types stored in the data container (see dcon_generated.txt)

#include <cstdint>
#include <optional>

#include "math.hpp"

namespace player {
    // Player.BodyModeIndex
    enum class body_mode_index : uint8_t {
        default_,
        crawl,
        stand,
        corridor_climb,
        climb_into_shortcut,
        wall_climb,
        climbing_on_beam,
        swimming,
        zero_g,
        stunned,
        dead,
    };

    // Player.AnimationIndex
    enum class animation_index : uint8_t {
        none,
        crawl_turn,
        stand_up,
        down_on_fours,
        ledge_crawl,
        ledge_grab,
        hang_from_beam,
        get_up_on_beam,
        stand_on_beam,
        climb_on_beam,
        get_up_to_beam_tip,
        hang_under_vertical_beam,
        beam_tip,
        corridor_turn,
        surface_swim,
        deep_swim,
        roll,
        flip,
        rocket_jump,
        belly_slide,
        antler_climb,
        grappling_swing,
        zero_g_swim,
        zero_g_pole_grab,
        vine_grab,
        dead,
    };

    // Player.InputPackage
    struct input_package {
        int32_t x = 0;
        int32_t y = 0;
        bool jmp = false;
        bool thrw = false;
        bool pckp = false;
        bool mp = false;
        bool spec = false;
        bool gamepad = false;
        rw::vec2 analogue_dir{0.f, 0.f};
        int32_t down_diagonal = 0;

        rw::ivec2 int_vec() const noexcept { return rw::ivec2{x, y}; }
        rw::ivec2 zero_g_gamepad_int_vec() const noexcept;
    };

    // SlugcatStats.Name: base game, Downpour (MoreSlugcats) and Watcher
    enum class slugcat_name : uint8_t {
        white,      // Survivor
        yellow,     // Monk
        red,        // Hunter
        gourmand,
        artificer,
        rivulet,
        spear,      // Spearmaster
        saint,
        inv,        // Sofanthiel
        watcher,
        // Downpour's slugpup (NPCs; --slugcat slugpup for the player)
        slugpup,
        count
    };

    // SlugcatStats
    struct slugcat_stats {
        slugcat_name name = slugcat_name::white;
        float runspeed_fac = 1.f;
        float body_weight_fac = 1.f;
        float pole_climb_speed_fac = 1.f;
        float corridor_climb_speed_fac = 1.f;
        float swim_force_fac = 1.f;
        float swim_boost_force = 3.f;
        float swim_boost_cost = 0.2f;
        int32_t swim_boost_cooldown = 20;
        float swim_boost_min_air = 0.5f;
        float drown_threshold = 1.f / 3.f;
        float lungs_fac = 1.f;
        // PlayerGraphics.DefaultSlugcatColor / DefaultBodyPartColorHex eyes
        rw::vec3 body_color{1.f, 1.f, 1.f};
        rw::vec3 eye_color{0.0627451f, 0.0627451f, 0.0627451f};
    };
    slugcat_stats make_stats(slugcat_name name) noexcept;

    // BodyPart (GenericBodyPart / TailSegment) of the PlayerGraphics module
    struct body_part {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        float rad = 1.f;
        float stretched = 1.f; // TailSegment.stretched
    };

    // Limb.Mode
    enum class limb_mode : uint8_t {
        hunt_relative_position,
        hunt_absolute_position,
        retracted,
        dangle,
    };

    // SlugcatHand (Limb)
    struct hand_state {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        rw::vec2 relative_hunt_pos{0.f, 0.f};
        rw::vec2 absolute_hunt_pos{0.f, 0.f};
        float hunt_speed = 7.f;
        float quickness = 0.5f;
        int32_t retract_counter = 0;
        limb_mode mode = limb_mode::hunt_relative_position;
        uint8_t reached_snap_position = 0;
        uint8_t reaching_for_object = 0;
    };

    // PlayerGraphics state that drives the sprites (purely cosmetic)
    struct graphics_state {
        rw::vec2 draw_pos[2];      // drawPositions[i, 0]
        rw::vec2 last_draw_pos[2]; // drawPositions[i, 1]
        body_part head;
        body_part legs;
        rw::vec2 legs_direction{0.f, -1.f};
        body_part tail[4];
        // AxolotlGills scales (Rivulet)
        body_part gills[6];
        hand_state hands[2];
        float spear_dir = 0.f;
        float airborne_counter = 0.f;
        // PlayerGraphics.ThrowObject: the hand follows the thrown item
        int32_t throw_counter = 0;
        uint32_t thrown_item = 0; // dcon::item_id index + 1, 0 for none
        uint8_t throw_hand = 0;
        // Watcher after the Void Weaver: longer tail, fluff, halo, gold eyes
        uint8_t weaver = 0;
        // drawn as a pup (PlayerGraphics.RenderAsPup): a shorter body and tail
        uint8_t pup = 0;
        float halo_flux = 1.f;
        uint32_t ticks = 0;
        rw::vec2 look_direction{0.f, 0.f};
        rw::vec2 last_look_direction{0.f, 0.f};
        int32_t blink = 0;
        // cosmetic randomness, kept apart from the gameplay rng
        rw::rng random{0x2545F491u};
        uint8_t initialized = 0;
    };

    // Player.input: the last 10 ticks of input, [0] is the current one
    struct input_history {
        input_package in[10];
        input_package& operator[](int i) noexcept { return in[i]; }
        input_package const& operator[](int i) const noexcept { return in[i]; }
    };
}
