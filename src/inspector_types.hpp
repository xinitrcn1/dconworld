#pragma once

// Inspector state that isn't a plain property of the creature (Downpour's
// Inspector.cs, InspectorAI.cs, InspectorGraphics.cs): the payload
// (`body_payload`, the creature_payload union's `inspector`), its data
// object (`inspector_data`, dcon_generated.txt: the AI, the head goals, the
// graphics module) and its three heads, creature_parts of kind
// inspector_head with an `inspector_head` data object each (the Tentacle's
// scalars and chunks, its tile segments and grab path in vector pools, its
// RopeGraphic). A head's health is its creature_part's (InspectorState.headHealth).

#include <cstdint>
#include <optional>

#include "dcon_generated_ids.hpp"
#include "math.hpp"
#include "world_types.hpp"

namespace inspector {
    // Inspector.headCount(); the heads' Tentacle(300) of 8 TentacleChunks;
    // InspectorHeadRopeGraphics: RopeGraphic(40)
    inline constexpr uint32_t head_count = 3;
    inline constexpr uint32_t head_chunks = 8;
    inline constexpr float head_length = 300.f;
    inline constexpr uint32_t rope_segments = 40;
    // InspectorState.Wingnumber: 3 + floor(SeededRandom * 8)
    inline constexpr uint32_t max_wings = 10;
    // InspectorGraphics.mycelia: (int)(10 + 15 * SeededRandom), each of at
    // most 20 points (Mycelium: IntClamp(length / 15, 2, 20))
    inline constexpr uint32_t max_mycelia = 25;
    inline constexpr uint32_t max_mycelium_points = 20;
    // InspectorAI.AllHeadGoals: PopulateHeadGoals stops adding at 500
    inline constexpr uint32_t max_head_goals = 560;
    // InspectorState.headHealth's start
    inline constexpr float head_health = 3.f;
    // Inspector.attentionDelayMax
    inline constexpr int32_t attention_delay_max = 100;

    // InspectorAI.Behavior
    enum class behavior : uint8_t {
        idle,
        hunt,
        escape_rain,
        inspect_area,
    };

    // the creature payload (creature_types.hpp's `inspector_payload`)
    struct body_payload {
        // Inspector.flyingPower, GoalPos
        rw::vec2 flying_power;
        rw::vec2 goal_pos;
        // anger, dying, lastDying, squeezeFac, lightpulse
        float anger;
        float dying;
        float last_dying;
        float squeeze_fac;
        float lightpulse;
        // activeEye (-1 none), squeeze, ownerIterator (-1 none),
        // InspectorState.Wingnumber
        int8_t active_eye;
        uint8_t squeeze;
        int8_t owner_iterator;
        uint8_t wing_number;
        // Destroy at the end of the dying is queued
        uint8_t destroy_queued;
        // inspector_data
        uint16_t data;
    };

    // the part payload (creature_types.hpp's `inspector_head_payload`)
    struct head_payload {
        // inspector_head
        uint16_t data;
    };

    // what a head holds or goes for (headGrabChunk, headWantToGrabChunk): a
    // creature's chunk (the slugcat's too) or a weapon's
    enum class hold_kind : uint8_t { none, creature, item };
    struct hold {
        hold_kind kind = hold_kind::none;
        uint8_t chunk = 0;
        dcon::creature_id creature;
        dcon::item_id item;
        constexpr bool valid() const noexcept { return kind != hold_kind::none; }
        bool operator==(hold const& o) const noexcept {
            return kind == o.kind && (kind == hold_kind::none || (kind == hold_kind::creature ? creature == o.creature && chunk == o.chunk : item == o.item));
        }
    };

    // Tentacle.TentacleChunk (with its Rope)
    struct head_chunk {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        float rad = 3.f;
        float t_pos = 0.f;
        rw::ivec2 contact_point{0, 0};
        rw::ivec2 last_contact_point{0, 0};
        rw::vec2 phase_from{0.f, 0.f};
        int32_t phases_to_same_location = 0;
        rw::ivec2 after_phase_stuck_pos{0, 0};
        float phase = -1.f;
        // currentSegmentTrail[0], the only entry anything reads
        rw::ivec2 trail{0, 0};
        int32_t phase_attempts = 0;
        float stretched_fac = 1.f;
        world::rope_state rope;
    };

    // a head: the Tentacle's scalars and chunks, and what Inspector and
    // InspectorAI keep per head (the inspector_head object's `t`)
    struct head {
        // Tentacle
        float update_counter = 0.f;
        int32_t pull_counter = 0;
        int32_t pulls_this_tick = 0;
        std::optional<rw::vec2> float_grab_dest;
        int32_t backtrack_from = -1;
        float retract_fac = 0.f;
        uint8_t limp = 0;
        head_chunk c[head_chunks];
        // Inspector.headGrabChunk, headWantToGrabChunk, headCuriosityFocus
        hold grab;
        hold want;
        dcon::creature_id curiosity;
        // InspectorAI.AIHeadGoals
        rw::vec2 goal{0.f, 0.f};
        // InspectorGraphics.blinks (kept with the body: Inspector.Update
        // reads them to pick its active eye)
        float blink = 0.f;
    };

    // RopeGraphic.Segment
    struct rope_segment {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        int32_t bend_index = -1;
    };

    // a head's part of the graphics module (purely cosmetic): its
    // InspectorHeadRopeGraphics, JawAngle, JawAngleWiggler, and the eye
    // sprite's colour and scaleX (DrawSprites eases them every frame; here
    // every tick)
    struct head_graphics {
        rope_segment rope[rope_segments];
        float jaw_angle = 0.f;
        float jaw_wiggler = 0.f;
        rw::vec3 eye_color{1.f, 1.f, 1.f};
        float eye_scale_x = 0.45f;
    };

    // InspectorAI (with its CreatureLooker)
    struct ai_state {
        int32_t new_idle_pos_counter = 1;
        int32_t head_idler = 0;
        float attention_grabber_timer = 0.f;
        dcon::creature_id first_time_attention_grabber;
        // CreatureLooker.lookCreature, lookFocusDelay; a CreatureSpotted's
        // ReevaluateLookObject(rep, 4) waiting for the tracker's update to end
        dcon::creature_id look_creature;
        int32_t look_focus_delay = 0;
        dcon::creature_id spotted;
    };

    // GenericBodyPart (the wings' body parts)
    struct wing_part {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
    };

    // CoralBrain.Mycelium (its points in the data object's mycelium_points
    // pool, max_mycelium_points per mycelium)
    struct mycelium {
        uint8_t count = 0;
        float con_rad = 0.f;
        float length = 0.f;
    };
    struct mycelium_point {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
    };

    // InspectorGraphics (purely cosmetic): the wings' body parts and flap
    // counters (wingflapCounters tick with DrawSprites in vanilla, here with
    // the graphics update), bodyRotation, the mycelia
    struct graphics_state {
        uint8_t built = 0;
        wing_part wings[max_wings];
        float wingflap[max_wings] = {};
        float body_rotation = 0.f;
        mycelium mycelia[max_mycelia];
        uint8_t mycelia_count = 0;
    };
}
