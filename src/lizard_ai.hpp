#pragma once

// What lizard.cpp (Lizard.cs), lizard_ai.cpp (LizardAI and its modules) and
// lizard_graphics.cpp (LizardGraphics) call of each other. Not for the rest
// of the game: that goes through lizard.hpp.

#include <cstdint>

#include "gamestate.hpp"
#include "lizard.hpp"
#include "math.hpp"
#include "pathing.hpp"
#include "world.hpp"

namespace lizard {
    // LizardVoice.Emotion
    enum class emotion : uint8_t {
        spotted_prey_first_time,
        re_spotted_prey,
        general_small_noise,
        curious,
        dominance,
        submission,
        pain_impact,
        pain_idle,
        frustration,
        fear,
        boredom,
        blood_lust,
        out_of_shortcut,
    };

    breed breed_of(sys::game_state const& state, dcon::creature_id l) noexcept;
    breed_params const& params_of(sys::game_state const& state, dcon::creature_id l) noexcept;
    // abstractCreature.pos: the head's tile
    rw::ivec2 here(sys::game_state const& state, dcon::creature_id l) noexcept;

    // Lizard.EnterAnimation
    void enter_animation(sys::game_state& state, dcon::creature_id l, animation a, bool force);
    // Lizard.JawOpen's setter
    void set_jaw_open(sys::game_state& state, dcon::creature_id l, float v);
    // Lizard.JawReadyForBite
    bool jaw_ready_for_bite(sys::game_state const& state, dcon::creature_id l) noexcept;
    // Lizard.AttemptBite, at a creature or slugcat (none: Bite(null))
    void attempt_bite(sys::game_state& state, dcon::creature_id l, creature::target t);
    // Lizard.JawsSnapShut
    void jaws_snap_shut(sys::game_state& state, dcon::creature_id l, rw::vec2 at);
    // Lizard.LegsGripping
    int32_t legs_gripping(sys::game_state const& state, dcon::creature_id l) noexcept;
    // Lizard.NoGripCounter: LizardGraphics' ticks without a leg gripping, 0 off screen
    int32_t no_grip_counter(sys::game_state const& state, dcon::creature_id l) noexcept;
    // LizardVoice.MakeSound (the intensity drawn at random without one)
    void make_sound(sys::game_state& state, dcon::creature_id l, emotion e);
    void make_sound(sys::game_state& state, dcon::creature_id l, emotion e, float intensity);
    // Lizard.IsWallClimber
    bool is_wall_climber(sys::game_state const& state, dcon::creature_id l) noexcept;
    // LizardTongue.Out / Ready / StuckToSomething
    bool tongue_out(sys::game_state const& state, dcon::creature_id l) noexcept;
    bool tongue_ready(sys::game_state const& state, dcon::creature_id l) noexcept;
    bool tongue_stuck(sys::game_state const& state, dcon::creature_id l) noexcept;

    namespace ai {
        using creature::relationship;
        // LizardAI.Update (ArtificialIntelligence.Update's modules: the
        // trackers, noise, stuck, lurk, spit and yellow modules)
        void update(sys::game_state& state, dcon::creature_id l, world::room_view const& room);
        // the LizardAI constructor
        void reset(sys::game_state& state, dcon::creature_id l);
        // Tracker.SeeCreature
        void see(sys::game_state& state, dcon::creature_id l, creature::target t);
        // Lizard.Collide's AI side: seen, a rival's anger
        void collided(sys::game_state& state, dcon::creature_id l, creature::target t);
        // LizardAI.BitCreature
        void bit_creature(sys::game_state& state, dcon::creature_id l, world::room_view const& room);
        // LizardAI.UnpleasantFallRisk
        bool unpleasant_fall_risk(sys::game_state& state, dcon::creature_id l, world::room_view const& room, rw::ivec2 tile);
        // LizardAI.DoIWantToHoldThisWithMyTongue, a chunk of `t`
        bool want_to_hold_with_tongue(sys::game_state& state, dcon::creature_id l, world::room_view const& room, creature::target t, uint32_t chunk);
        // UpdateDynamicRelationship: the slugcat by LikeOfPlayer, the rest
        // StaticWorld's (the tracked ones keep it: tracker::dynamic_relationship)
        relationship relationship_to(sys::game_state const& state, dcon::creature_id l, creature::target t);
        // PreyTracker.MostAttractivePrey
        creature::target tracked(sys::game_state const& state, dcon::creature_id l) noexcept;
        // focusCreature
        creature::target focus(sys::game_state const& state, dcon::creature_id l) noexcept;
        // LizardPather's field: the one to the destination, for the breed
        pathing::field_id field(sys::game_state& state, dcon::creature_id l, world::room_view const& room);
        // PathFinder.CoordinateReachableAndGetbackable, judged by the field to
        // the destination: the tile and the lizard both lead there
        bool reachable_and_getbackable(sys::game_state& state, dcon::creature_id l, world::room_view const& room, rw::ivec2 t);
        // AItile.fallRiskTile
        rw::ivec2 fall_risk_tile(world::room_view const& room, rw::ivec2 t);
        // LizardSpitTracker.AimPos, if it can aim
        bool spit_aim_pos(sys::game_state& state, dcon::creature_id l, rw::vec2& out);
        // LizardSpitTracker.AtSpitPos
        bool at_spit_pos(sys::game_state const& state, dcon::creature_id l) noexcept;
        // LizardSpitTracker.SetAim / AimABitUp / AimABitDown
        void spit_set_aim(sys::game_state& state, dcon::creature_id l, float am);
        void spit_aim_a_bit_up(sys::game_state& state, dcon::creature_id l, float am);
        void spit_aim_a_bit_down(sys::game_state& state, dcon::creature_id l, float am);
    }
}
