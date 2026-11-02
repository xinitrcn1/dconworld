#pragma once

// The world's offscreen den (World.offScreenDen, GenerateOffScreenDen) and
// the creatures living there: the world files' OFFSCREEN vultures (the king
// and the miros vulture too), miros birds and scavengers (the Elite and the
// Disciple too). Vanilla's abstract AIs move them along the highways
// between the offscreen den and the region's rooms; here they are a fourth
// `away` state (dens::where::offscreen): the body in no room, the creature
// in the away bucket of its region's anchor room (the first room of the
// region, its `home_room`; its `home_den` is `den_node`), so that a
// region's offscreen creatures update one after the other in one lane
// (dens::update_room), in id order: the raid and the squad, which look at
// each other, stay deterministic. Coming into a room and leaving it are
// transactions (game_state::defer).
//
// - Vultures (VultureAbstractAI): the check room, a room of the region
//   with a SkyExit wider than 4 tiles, rolled by its size dependent
//   attraction squared (AddRandomCheckRoom; a neighbour of the room it left
//   now and then, like AddRoomClusterToCheckList). It flies in from 99 tiles
//   above a random tile of the exit (ShortcutHandler.FlyingCreatureArrived
//   InRealizedRoom), idles there 620 ticks and flies off over the top
//   (Vulture.AccessSkyGate), as it does with prey (eaten off screen), when
//   disencouraged or fleeing the rain.
// - Miros birds (MirosBirdAbstractAI): the allowed SideExits are those at
//   least 5 tiles wide of the rooms where the miros bird's attraction is Stay
//   (MMF's dehardcoded rooms; vanilla's list without one). The most dominant
//   ready bird raids when three are ready or all of them are (Raid): a
//   random allowed exit to come in by, another wide side exit of that room
//   to leave by, up to 2 to 4 ready followers coming in after 0 to 49
//   ticks. It walks in at the exit, crosses the room and walks off at the
//   other side; the rain and its prey send it out too.
// - Scavengers (ScavengerAbstractAI): back in the den they rest 300 to 500
//   ticks; the most dominant ready one assembles a squad when two others
//   are ready (TryAssembleSquad): a destination room rolled by its size
//   dependent attraction (RandomDestinationRoom), 2 to 6 of them (4 in SH,
//   CC, LC; 2 in SU, UW, SI), the members following 10 ticks apart. They
//   come out of a region transportation pipe of the room closest to the
//   destination and walk there; in rooms they Migrate (dontMigrate 400 to
//   4800) to another room or, one time in five (always on their own), back
//   to the den by the closest region transportation pipe.
//
// All of them stay in while the rain is near (or they are hurt), like a den.
// The travel time between the den and a room is a stand-in (no world map
// distances): 100 to 500 tiles at the template's offScreenSpeed.

#include <atomic>
#include <cstdint>
#include <optional>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "offscreen_types.hpp"
#include "world.hpp"

namespace spawns {
    struct spawn;
    struct report;
}

namespace offscreen {
    // the home_den of a creature living off screen
    inline constexpr uint8_t den_node = 0xFE;

    // lives off screen (wherever it is now)
    bool homed(sys::game_state const& state, dcon::creature_id c) noexcept;
    // the scavenger's abstract AI sent it home (its Migrate)
    bool heading_home(sys::game_state const& state, dcon::creature_id c) noexcept;

    // whether the world file's OFFSCREEN spawns of this kind live off screen
    bool lives_offscreen(creature::type kind) noexcept;
    // the world file's OFFSCREEN spawn `s` of `region`: made off screen
    uint32_t spawn(sys::game_state& state, dcon::region_id region, spawns::spawn const& s, spawns::report& r);

    // one tick of an offscreen creature (dens::update_room of its anchor room)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& anchor);

    // dens::home_tile for a creature living off screen: the tile it heads
    // for to leave the room (a side border tile, a region transportation
    // pipe's entrance, the room exit's on the way to a room with one); none
    // for a vulture in a room with sky exits (it flies up) or no way out
    std::optional<rw::ivec2> way_out(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // dens::after_update: a creature heading home to a border tile (its
    // travel_tile) is there, at the edge of the room: it leaves
    bool near_border(sys::game_state const& state, dcon::creature_id c, world::room_view const& room);
    // a creature leaving `from` for the offscreen den (serial: a deferred
    // transaction), after `meal` ticks of eating there
    void arrive(sys::game_state& state, dcon::creature_id c, dcon::room_id from, int32_t meal);

    // the kinds' AIs:
    // VultureAbstractAI.AbstractBehavior from an idle vulture: true when its
    // check room is done (it flies off)
    bool vulture_idle(sys::game_state& state, dcon::creature_id c);
    // a vulture high over the top of its room flies off (`meal`: ticks of eating)
    void take_flight(sys::game_state& state, dcon::creature_id c, world::room_view const& room, int32_t meal);
    // ScavengerAbstractAI in a room: the way to the squad's destination,
    // dontMigrate and Migrate (`roaming`: bravery * energy)
    void scavenger_tick(sys::game_state& state, dcon::creature_id c, world::room_view const& room, float roaming);

    // what happened since start (reports, the fuzz; never read by the simulation)
    struct counters {
        std::atomic<uint32_t> spawned{0};
        std::atomic<uint32_t> entered{0};
        std::atomic<uint32_t> left{0};
        std::atomic<uint32_t> raids{0};
        std::atomic<uint32_t> squads{0};
    };
    counters& stats() noexcept;
}
