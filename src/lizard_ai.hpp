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

    dcon::room_id room_of(sys::game_state const& state, dcon::lizard_id l) noexcept;
    dcon::player_id player_of(sys::game_state const& state, uint8_t v) noexcept;
    inline uint8_t player_ref(dcon::player_id p) noexcept { return p ? uint8_t(p.index() + 1) : 0; }
    breed breed_of(sys::game_state const& state, dcon::lizard_id l) noexcept;
    breed_params const& params_of(sys::game_state const& state, dcon::lizard_id l) noexcept;
    // abstractCreature.pos: the head's tile
    rw::ivec2 here(sys::game_state const& state, dcon::lizard_id l) noexcept;
    // whether the slugcat is in the room (not in a shortcut)
    bool player_here(sys::game_state const& state, dcon::player_id p, dcon::room_id room) noexcept;

    // Lizard.EnterAnimation
    void enter_animation(sys::game_state& state, dcon::lizard_id l, animation a, bool force);
    // Lizard.JawOpen's setter
    void set_jaw_open(sys::game_state& state, dcon::lizard_id l, float v);
    // Lizard.JawReadyForBite
    bool jaw_ready_for_bite(sys::game_state const& state, dcon::lizard_id l) noexcept;
    // Lizard.AttemptBite, at the slugcat (none: Bite(null))
    void attempt_bite(sys::game_state& state, dcon::lizard_id l, dcon::player_id p);
    // Lizard.JawsSnapShut
    void jaws_snap_shut(sys::game_state& state, dcon::lizard_id l, rw::vec2 at);
    // Lizard.LegsGripping
    int32_t legs_gripping(sys::game_state const& state, dcon::lizard_id l) noexcept;
    // LizardVoice.MakeSound (the intensity drawn at random without one)
    void make_sound(sys::game_state& state, dcon::lizard_id l, emotion e);
    void make_sound(sys::game_state& state, dcon::lizard_id l, emotion e, float intensity);
    // Lizard.IsWallClimber
    bool is_wall_climber(sys::game_state const& state, dcon::lizard_id l) noexcept;
    // LizardTongue.Out / Ready / StuckToSomething
    bool tongue_out(sys::game_state const& state, dcon::lizard_id l) noexcept;
    bool tongue_ready(sys::game_state const& state, dcon::lizard_id l) noexcept;
    bool tongue_stuck(sys::game_state const& state, dcon::lizard_id l) noexcept;

    namespace ai {
        // LizardAI.Update (ArtificialIntelligence.Update's modules: the
        // tracker, the prey, noise, stuck, lurk, spit and yellow modules)
        void update(sys::game_state& state, dcon::lizard_id l, world::room_view const& room);
        // the LizardAI constructor
        void reset(sys::game_state& state, dcon::lizard_id l);
        // Tracker.SeeCreature for a slugcat
        void see_player(sys::game_state& state, dcon::lizard_id l, dcon::player_id p);
        // LizardAI.BitCreature
        void bit_creature(sys::game_state& state, dcon::lizard_id l, world::room_view const& room);
        // LizardAI.UnpleasantFallRisk
        bool unpleasant_fall_risk(sys::game_state& state, dcon::lizard_id l, world::room_view const& room, rw::ivec2 tile);
        // LizardAI.DoIWantToHoldThisWithMyTongue, a slugcat's chunk
        bool want_to_hold_with_tongue(sys::game_state& state, dcon::lizard_id l, world::room_view const& room, dcon::player_id p, uint32_t chunk);
        // the dynamic relationship towards a slugcat (UpdateDynamicRelationship):
        // Eats at `intensity`, or Ignores
        struct relationship {
            bool eats = false;
            float intensity = 0.f;
            bool go_for_kill() const noexcept { return eats; }
        };
        relationship player_relationship(sys::game_state const& state, dcon::lizard_id l, dcon::player_id p);
        // the player it tracks (index + 1 in ai_state::rep), when it is valid
        dcon::player_id tracked(sys::game_state const& state, dcon::lizard_id l) noexcept;
        // focusCreature, when it is the tracked slugcat
        dcon::player_id focus(sys::game_state const& state, dcon::lizard_id l) noexcept;
        // LizardPather's field: the one to the destination, for the breed
        pathing::field_id field(sys::game_state& state, dcon::lizard_id l, world::room_view const& room);
        // PathFinder.CoordinateReachableAndGetbackable, judged by the field to
        // the destination: the tile and the lizard both lead there
        bool reachable_and_getbackable(sys::game_state& state, dcon::lizard_id l, world::room_view const& room, rw::ivec2 t);
        // AItile.fallRiskTile
        rw::ivec2 fall_risk_tile(world::room_view const& room, rw::ivec2 t);
        // LizardSpitTracker.AimPos, if it can aim
        bool spit_aim_pos(sys::game_state& state, dcon::lizard_id l, rw::vec2& out);
        // LizardSpitTracker.AtSpitPos
        bool at_spit_pos(sys::game_state const& state, dcon::lizard_id l) noexcept;
        // LizardSpitTracker.SetAim / AimABitUp / AimABitDown
        void spit_set_aim(sys::game_state& state, dcon::lizard_id l, float am);
        void spit_aim_a_bit_up(sys::game_state& state, dcon::lizard_id l, float am);
        void spit_aim_a_bit_down(sys::game_state& state, dcon::lizard_id l, float am);
    }
}
