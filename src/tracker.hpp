#pragma once

// The AI modules every ported AI shares, over every creature of its room (the
// slugcats included): Tracker (noticing a random one of the room each tick, keeping up
// visual contact, forgetting), RelationshipTracker (the dynamic
// relationship of each, sorted into a module), PreyTracker (the most
// attractive prey, frustration), ThreatTracker (threat of tiles, the most
// threatening creature, FleeTo) and AgressionTracker (anger at rivals).
// Kept in the creature's `tracker` property (creature_types.hpp's
// tracker_state: ten representations, like vanilla's maxTrackedCreatures).
//
// What tells the kinds apart (their VisualContact, UpdateDynamicRelationship,
// ModuleToTrackRelationship, CreatureSpotted, the pathfinder's
// reachability) comes in through `hooks`. No ghosts: the best guess for a
// creature out of sight is where it was last seen.

#include <cstdint>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace tracker {
    using creature::relationship;
    using creature::track_module;
    using creature::tracked;
    using creature::tracker_state;

    // Tracker's, PreyTracker's, ThreatTracker's and AgressionTracker's
    // constructor arguments (the kind's AI constructor)
    struct params {
        // Tracker: seeAroundCorners, framesToRememberCreatures (-1 forever),
        // ghostDismissalRange
        int32_t see_around_corners = 10;
        int32_t frames_to_remember = -1;
        int32_t ghost_dismissal_range = 10;
        // TrackerToDiscardDeadCreature always (CentipedeAI), else the
        // default (unless it eats it)
        bool discard_dead_always = false;
        // PreyTracker(maxRememberedCreatures, persistanceBias,
        // sureToGetPreyDistance, sureToLosePreyDistance); giveUpOnUnreachablePrey
        int32_t prey_max = 5;
        float persistance_bias = 1.f;
        float sure_to_get = 5.f;
        float sure_to_lose = 150.f;
        int32_t give_up_unreachable = 400;
        // ThreatTracker(maxRememberedCreatures)
        int32_t threat_max = 3;
        // AgressionTracker(angerSpeedUp, angerSpeedDown)
        float anger_up = 0.001f;
        float anger_down = 0.001f;
    };

    // What the tracking AI answers
    struct hooks {
        void* user = nullptr;
        // ArtificialIntelligence.VisualContact(BodyChunk)
        bool (*sees)(void* user, dcon::body_chunk_id chunk) = nullptr;
        // ArtificialIntelligence.VisualContact(Vector2, 0)
        bool (*sees_point)(void* user, rw::vec2 pos) = nullptr;
        // IUseARelationshipTracker.UpdateDynamicRelationship (null: the static one)
        relationship (*relate)(void* user, tracked const& rep) = nullptr;
        // IUseARelationshipTracker.ModuleToTrackRelationship
        track_module (*module)(relationship r) = nullptr;
        // ArtificialIntelligence.CreatureSpotted (firstSpot)
        void (*spotted)(void* user, tracked const& rep, bool first) = nullptr;
        // PathFinder.CoordinateReachableAndGetbackable (null: always)
        bool (*reach)(void* user, rw::ivec2 tile) = nullptr;
    };

    // ModuleToTrackRelationship of most AIs: Afraid a threat, Eats and
    // Attacks prey, AgressiveRival aggression (if it has an AgressionTracker)
    track_module default_module(relationship r) noexcept;
    track_module module_without_aggression(relationship r) noexcept;

    inline tracker_state& of(sys::game_state& state, dcon::creature_id c) { return state.world.creature_get_tracker(c); }
    inline tracker_state const& of(sys::game_state const& state, dcon::creature_id c) { return state.world.creature_get_tracker(c); }

    // a creature's representation of `t`, if it has one
    tracked* find(tracker_state& s, dcon::creature_id t) noexcept;
    tracked const* find(tracker_state const& s, dcon::creature_id t) noexcept;
    inline tracked const* find(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) noexcept { return find(of(state, c), t); }

    // BestGuessForPosition's tile
    inline rw::ivec2 best_guess(tracked const& r) noexcept { return world::room_view::get_tile_position(r.last_seen); }
    // CreatureRepresentation.EstimatedChanceOfFinding (LowestGenerationAvailable 1)
    float chance_of_finding(tracked const& r, int32_t see_around_corners) noexcept;
    // AIModule's DynamicRelationship(rep): the current one, or the static one
    // for something it doesn't track
    relationship dynamic_relationship(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) noexcept;

    // ArtificialIntelligence.Update's modules: Tracker.Update,
    // RelationshipTracker.Update, PreyTracker.Update, ThreatTracker.Update,
    // AgressionTracker.Update. `at` is the creature's tile
    // (abstractCreature.pos)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room, rw::ivec2 at, params const& p, hooks const& h);
    // Tracker.SeeCreature: visual contact with `t` now (noticed if new)
    void see(sys::game_state& state, dcon::creature_id c, world::room_view const& room, dcon::creature_id t, hooks const& h);
    // Tracker.ForgetCreature
    void forget(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // forgets everything (a new room: the AI's NewRoom)
    void clear(sys::game_state& state, dcon::creature_id c);

    // PreyTracker.MostAttractivePrey, Utility, frustration
    inline tracked const* prey(tracker_state const& s) noexcept { return s.prey ? find(s, s.prey) : nullptr; }
    float prey_utility(sys::game_state& state, dcon::creature_id c, world::room_view const& room, rw::ivec2 at, params const& p);
    // TrackedPrey.PathFinderCanGetToPrey
    bool can_get_to_prey(tracked const& r, hooks const& h);

    // ThreatTracker
    inline tracked const* threat(tracker_state const& s) noexcept { return s.threat ? find(s, s.threat) : nullptr; }
    float threat_utility(tracker_state const& s) noexcept;
    float threat_of_tile(tracker_state const& s, rw::ivec2 tile, int32_t see_around_corners) noexcept;
    float threat_of_area(tracker_state const& s, rw::ivec2 tile, int32_t see_around_corners) noexcept;
    // ThreatTracker.Panic
    inline float panic(tracker_state const& s) noexcept { return 1.f - 1.f / (s.current_threat + 1.f); }
    // ThreatTracker.FleeTo within the room (`accessible`: the creature can
    // stand there and get there)
    rw::ivec2 flee_to(sys::game_state& state, dcon::creature_id c, world::room_view const& room, rw::ivec2 at, int32_t reevaluations, int32_t max_distance,
        params const& p, hooks const& h);

    // AgressionTracker
    inline tracked const* aggression(tracker_state const& s) noexcept { return s.aggression ? find(s, s.aggression) : nullptr; }
    float aggression_utility(tracker_state const& s) noexcept;
    void increment_anger(sys::game_state& state, dcon::creature_id c, dcon::creature_id t, float add);
    // SetAnger (base < 0: kept)
    void set_anger(sys::game_state& state, dcon::creature_id c, dcon::creature_id t, float anger, float base);
}
