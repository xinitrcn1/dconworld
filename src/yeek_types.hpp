#pragma once

// Yeek state that isn't a plain property of the creature (Yeek.cs, YeekAI,
// YeekState, YeekGraphics): what its data object (`yeek_data`,
// dcon_generated.txt) keeps that isn't a number: the personality, the
// looks rolled from the ID's seed, its squad (the yeeks it is in a pack
// with), the idle position it hops about, and the YeekGraphics state with
// the two YeekTails (purely cosmetic)

#include <cstdint>

#include "dcon_generated_ids.hpp"
#include "math.hpp"
#include "pathing_types.hpp"
#include "player_types.hpp"

namespace yeek {
    // The Yeek template's TileTypeResistances: floors at 1, corridors at 2,
    // poles at 1.5 (it swims on the surface only: AirAndSurface)
    inline constexpr pathing::movement movement{{10, 20, 15, 0, 0, 0, 0}, 0};

    // YeekAI.Behavior (Hungry and ReturnFood never come up: no fruit for it)
    enum class behavior : uint8_t {
        idle,
        get_unstuck,
        escape_rain,
        fear,
        hungry,
        return_food,
    };

    // AbstractCreature.personality (rolled from the ID's seed)
    struct personality {
        float sympathy = 0.5f;
        float energy = 0.5f;
        float bravery = 0.5f;
        float nervous = 0.5f;
        float aggression = 0.5f;
        float dominance = 0.5f;
    };

    // YeekGraphics.CreateCosmeticAppearance: the colours the sprites use
    // (furColor; featherColor, tailHighlightColor and trueEyeColor end up
    // the same), plumageGraphic and the body feathers' count
    struct looks {
        rw::vec3 fur{1.f, 1.f, 1.f};
        rw::vec3 highlight{1.f, 0.5f, 0.f};
        uint8_t plumage = 0;
        uint8_t feathers = 3;
    };

    // YeekAI.yeekSquad: the yeeks it treated as Pack, the few it remembers
    inline constexpr uint32_t max_squad = 8;
    struct squad {
        dcon::creature_id who[max_squad];
        uint8_t count = 0;
    };

    // YeekAI's idling: CurrentIdlePos, idleBordomCountdown, roomInterest;
    // Yeek.lastSafeClimb; YeekAI.focusCreature; a squad mate's
    // SurprisedFearReaction about `alarm` (taken in on its next update)
    struct idle_state {
        rw::ivec2 current_idle_pos{-1, -1};
        int32_t idle_boredom = 0;
        int32_t room_interest = 4;
        rw::vec2 last_safe_climb{0.f, 0.f};
        dcon::creature_id focus;
        dcon::creature_id alarm;
    };

    // Yeek.YeekTail (10 parts each, maxDist 5), drawn only
    inline constexpr uint32_t tail_length = 10;
    // YeekGraphics.YeekFeather
    inline constexpr uint32_t max_feathers = 4;
    struct feather {
        rw::vec2 root{0.f, 0.f};
        rw::vec2 last_root{0.f, 0.f};
        rw::vec2 dir{0.f, 0.f};
        rw::vec2 last_dir{0.f, 0.f};
    };
    // YeekGraphics.YeekLeg (and its AnimState)
    enum class leg_state : uint8_t { sit, climb, tunnel, jump, dangle };
    struct leg {
        rw::vec2 foot{0.f, 0.f};
        rw::vec2 last_foot{0.f, 0.f};
        rw::vec2 root{0.f, 0.f};
        rw::vec2 last_root{0.f, 0.f};
        rw::vec2 goal{0.f, 0.f};
        float jump_timer = 0.f;
        leg_state state = leg_state::sit;
    };
    struct graphics_state {
        player::body_part tails[2][tail_length];
        feather feathers[max_feathers];
        leg legs[2];
        rw::vec2 head_draw_dir{0.f, -0.6f};
        rw::vec2 last_head_draw_dir{0.f, -0.6f};
        rw::vec2 fear_look_loc{0.f, 0.f};
        float blink_percent = 0.f;
        float eye_shudder = 0.f;
        float watch_intensity = 0.f;
        float swim_counter = 0.f;
        float flip = 1.f;
        float last_flip = 1.f;
        int32_t blink_start_counter = 0;
        int32_t more_blinks = 0;
        // EyeShaker / EyeShakerScale, rolled once a tick: the eyes' x, y
        // offsets and their scale
        float eye_shake[2][2] = {};
        float eye_shake_scale[2][2] = {{1.f, 1.f}, {1.f, 1.f}};
        // the climbing mode last tick (StartClimbing's legs Reset)
        uint8_t was_climbing = 0;
    };
}
