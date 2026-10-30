#pragma once

// Creatures wandering to other rooms: AbstractCreatureAI's migration as the
// realized AIs use it. Vanilla's creatures roam between rooms off screen
// (the abstract layer); every room is realized here, so the abstract roll
// (MigrationBehavior: the room's attraction for the creature,
// roamBetweenRoomsChance, abstractedLaziness against the time spent in the
// room) runs from the AI like LizardAI's and DropBugAI's
// `abstractAI.AbstractBehavior(1)`. A migration plans a trip
// (RandomMoveToOtherRoom's walk over the room exits weighted by the rooms'
// size dependent attraction, or MoveToMoreAttractiveNeighborRoom): the
// creature's `trip`, the rooms ahead with the exit leading to each. The AI
// heads for the next exit's pipe (`travel_tile`, like dens::home_tile) and
// travels it (dens.hpp's vessels); arriving moves the trip on (`arrived`,
// AbstractCreatureAI.Moved).
//
// The rooms' attractions are the regions' properties.txt Room_Attr lines
// (with their Default), shelters and gates Forbidden (`load_attractions`).
// Now and then (`cross_region_chance`) a trip may lead through a region
// gate or a Watcher warp into another region (gates.hpp cycles for
// creatures, warps.hpp carries them).

#include <cstdint>
#include <optional>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace migrate {
    // reads every region's Room_Attr lines into the rooms' `attraction`
    void load_attractions(sys::game_state& state);

    // AbstractCreatureAI.WantToMigrate: a trip ahead
    inline bool want_to(sys::game_state const& state, dcon::creature_id c) noexcept {
        auto const& t = state.world.creature_get_trip(c);
        return t.next < t.hops;
    }
    // MigrationDestination's room (the creature's own without a trip)
    dcon::room_id destination(sys::game_state const& state, dcon::creature_id c) noexcept;
    // the next room of the trip (none without one)
    dcon::room_id next_room(sys::game_state const& state, dcon::creature_id c) noexcept;
    // AbstractRoom.AttractionValueForCreature, SizeDependentAttractionValueForCreature
    float attraction(sys::game_state const& state, dcon::creature_id c, dcon::room_id room) noexcept;
    float size_dependent_attraction(sys::game_state const& state, dcon::creature_id c, dcon::room_id room) noexcept;

    // AbstractCreatureAI.AbstractBehavior(1) for a realized creature: the
    // MigrationBehavior roll, which may plan a trip
    void abstract_behavior(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // the AI's chance a tick of calling AbstractBehavior(1) for the kinds
    // whose vanilla AI never does (LizardAI's rate by RoomLike)
    void idle_roll(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // RandomMoveToOtherRoom(maxRoamDistance), MoveToMoreAttractiveNeighborRoom
    void random_move(sys::game_state& state, dcon::creature_id c, world::room_view const& room, int32_t max_roam);
    void move_to_more_attractive_neighbor(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // drops the trip
    void cancel(sys::game_state& state, dcon::creature_id c) noexcept;

    // The tile to head for on the trip (the open tile before the next
    // exit's pipe entrance), marking the creature as heading into it this
    // tick; none without a trip, or when the way is lost (the trip ends)
    std::optional<rw::ivec2> travel_tile(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // out of a pipe in `room` (dens.hpp): AbstractCreatureAI.Moved
    void arrived(sys::game_state& state, dcon::creature_id c, dcon::room_id room);
    // a tick in the room (AbstractWorldEntity.timeSpentHere)
    inline void tick(sys::game_state& state, dcon::creature_id c) noexcept { ++state.world.creature_get_time_in_room(c); }
}
