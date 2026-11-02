#pragma once

// Big and small moth state that isn't a plain property of the creature (the
// Watcher's BigMoth, BigMothAI, BigMothGraphics): what its data object
// (`moth_data`, dcon_generated.txt) keeps that isn't a number of the payload.
// One kind, two sizes: vanilla's SmallMoth is the BigMoth class with its own
// template (BigMoth.Small: scale 0.3, fewer mesh segments, its own AI tweaks).

#include <cstdint>

#include "dcon_generated_ids.hpp"
#include "math.hpp"

namespace moth {
    // the template: BigMoth or SmallMoth
    enum class variant : uint8_t { big, small };
    inline constexpr uint32_t variant_count = 2;

    // BigMothAI.Behavior
    enum class behavior : uint8_t {
        idle,
        flee,
        attack,
        eat,
        stalk,
        stow_food,
        set_down_baby,
        feed_baby,
        escape_rain,
        get_unstuck,
    };

    // BigMoth.MothWingPair.Mode
    enum class wing_mode : uint8_t { glide, ready_flap, flap };

    // BigMoth.MothLeg: legs[pair, index], pair 0 the back legs (on chunk 2),
    // pair 1 the front ones (on chunk 1). stepPos and idealStepPos are
    // Vector2? (has_step, has_ideal); grabbedChunk is chunk `grabbed_chunk`
    // of `grabbed` (-1 none; let go of when grasps[0] holds something else)
    struct leg {
        dcon::creature_id grabbed;
        rw::vec2 connection_pos{0.f, 0.f};
        rw::vec2 last_connection_pos{0.f, 0.f};
        rw::vec2 foot_pos{0.f, 0.f};
        rw::vec2 last_foot_pos{0.f, 0.f};
        rw::vec2 step_pos{0.f, 0.f};
        rw::vec2 ideal_step_pos{0.f, 0.f};
        float max_reach = 0.f;
        float step_radius = 0.f;
        float leg_tension = 0.f;
        float last_aware_leg_extension = 0.f;
        float last_step_dist_to_new_step = 0.f;
        float step_progress_counter = 0.f;
        int32_t stretch_counter = 0;
        uint8_t has_step = 0;
        uint8_t has_ideal = 0;
        uint8_t secured_foot = 0;
        uint8_t allow_step_to_trigger_other = 0;
        uint8_t connected_chunk = 1;
        int8_t grabbed_chunk = -1;
    };

    // BigMoth.MothWingPair
    struct wing_pair {
        wing_mode mode = wing_mode::glide;
        float flapness = 0.f;
        float flap_vel = 0.f;
    };

    // a SocialMemory.Relationship (tempLike, like) of a creature it met
    struct social {
        dcon::creature_id who;
        float temp_like = 0.f;
        float like = 0.f;
    };
    inline constexpr uint32_t social_slots = 4;

    // BigMothTrackState.thrownTimer of a creature it threw away
    struct thrown {
        dcon::creature_id who;
        int32_t timer = 0;
    };
    inline constexpr uint32_t thrown_slots = 4;
    inline constexpr uint32_t thief_slots = 4;

    // BigMoth.IndividualVariations (+ BigMothState's tag, babyness)
    struct ivars {
        rw::vec3 body_color{1.f, 1.f, 1.f};
        rw::vec3 secondary_color{1.f, 1.f, 1.f};
        float knee_colorness = 0.f;
        float chub = 0.f;
        float leg_puff = 0.f;
        float antenna_width = 2.f;
        int32_t antenna_length = 3;
        int32_t tagged_leg = -1;
        rw::vec3 leg_tag_color{0.f, 0.f, 0.f};
        // AbstractCreature.personality
        float sympathy = 0.5f;
        float bravery = 0.5f;
        float aggression = 0.5f;
        float dominance = 0.5f;
    };

    // BigMoth's own fields (everything the simulation reads), the moth's
    // legs and wing pairs, and the tongue's reach (TongueGraphics.outness,
    // foldness, tongueOverCounter: vanilla's graphics module decides when the
    // tongue gets there; here the simulation does, see moth.cpp)
    struct body_state {
        leg legs[2][2];
        wing_pair wings[2];
        rw::vec2 move_direction{0.f, 0.f};
        rw::vec2 feet_avg_pos{0.f, 0.f};
        rw::vec2 look_pos{0.f, 0.f};
        rw::vec2 random_look_pos{0.f, 0.f};
        float fly_upness = 0.f;
        float shake = 0.f;
        float run_speed = 0.f;
        float ground_angle = 0.f;
        float stance = 0.6f;
        float flipness = 0.f;
        int32_t legs_on_ground = 0;
        int32_t hold_through_wall_counter = 0;
        int32_t random_look_counter = 0;
        int32_t want_to_fly_counter = 0;
        int32_t stuck_counter = 0;
        int8_t flip_side = 1;
        uint8_t random_look_at_creature = 0;
        uint8_t want_to_fly = 0;
        uint8_t narrow_upcoming = 0;
        // drinkChunk (a chunk of `drink`), drinkingChunk, tongueHold,
        // eatCounter; the meat it drank of a dead one so far (vanilla's
        // meatLeft for the kinds without one here)
        dcon::creature_id drink;
        uint8_t drink_chunk = 0;
        uint8_t drinking = 0;
        float tongue_hold = 0.f;
        int32_t eat_counter = 0;
        int32_t meat_taken = 0;
        float tongue_outness = 0.f;
        float tongue_foldness = 1.f;
        int32_t tongue_over_counter = 0;
        // BigMothGraphics.blinkCounter (its chatter rolls shake the body:
        // the simulation's here, the eyes read it)
        int32_t blink_counter = 0;
        // BigMothState.foodStored
        int32_t food_stored = 0;
    };

    // StuckTracker with trackPastPositions: the last 80 tiles (newest
    // first), GetUnstuckPosCalculator
    struct stuck_tracker {
        static constexpr int32_t tracked = 80;
        rw::ivec2 past[tracked];
        int32_t count = 0;
        int32_t head = 0;
        int32_t counter = 0;
        rw::ivec2 unstuck_goal{0, 0};
    };

    // BigMothAI's own fields
    struct ai_state {
        dcon::creature_id focus;
        dcon::creature_id last_focus;
        // CreatureLooker (lookCreature, lookFocusDelay)
        dcon::creature_id look_creature;
        int32_t look_focus_delay = 0;
        // ChildTrackerModule.pickedChild
        dcon::creature_id picked_child;
        rw::ivec2 temp_idle_pos{-1, -1};
        rw::ivec2 temp_cliff_pos{-1, -1};
        rw::ivec2 temp_safe_baby_pos{-1, -1};
        rw::vec2 drink_spot{0.f, 0.f};
        int32_t give_up_on_cliff_timer = 0;
        int32_t grab_cool_down = 0;
        int32_t idle_pos_counter = 0;
        int32_t time_to_tire = 0;
        int32_t time_in_room = 0;
        uint8_t trying_to_leave_room = 0;
        // UtilityComparer's smoothed threat (FloatTweenUpAndDown)
        float threat_smoothed = 0.f;
        float fear = 0.f;
        stuck_tracker stuck;
        social memory[social_slots];
        thrown thrown_away[thrown_slots];
        dcon::creature_id known_thieves[thief_slots];
    };

    // BigMothGraphics (purely cosmetic): the head, the tongue's segments,
    // antennas, the legs' graphic segments, the wings
    inline constexpr uint32_t max_tongue_segments = 12;
    inline constexpr uint32_t max_antenna_segments = 7;
    inline constexpr uint32_t max_wing_segments = 8;
    struct wing_graphic {
        rw::vec2 segments[max_wing_segments][3];
        rw::vec2 tip_pos{0.f, 0.f};
        float spread_outness = 0.f;
        uint8_t in_front = 0;
    };
    struct graphics_state {
        rw::vec2 head_pos{0.f, 0.f};
        rw::vec2 last_head_pos{0.f, 0.f};
        rw::vec2 head_dir{0.f, -1.f};
        rw::vec2 last_head_dir{0.f, -1.f};
        float face_flip = 0.f;
        float last_face_flip = 0.f;
        float eye_openness = 1.f;
        float last_eye_openness = 1.f;
        float wing_stashness = 0.5f;
        // TongueGraphics
        rw::vec2 tongue[max_tongue_segments][3];
        float glug_size = 0.f;
        int32_t suck_counter = 0;
        // AntennaGraphics
        rw::vec2 antennas[2][max_antenna_segments][3];
        // LegGraphics (segments[4, 2]) and whether it is in front
        rw::vec2 legs[2][2][4][2];
        uint8_t leg_in_front[2][2] = {};
        // WingGraphics: wings[pair * 2 + index]
        wing_graphic wings[4];
        uint8_t shadow_mode = 0;
    };
}
