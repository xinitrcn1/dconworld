#pragma once

// The world files' creatures (WorldLoader's CREATURES section and
// GeneratePopulation for a fresh save).
//
// Every region's `world_<region>.txt` (plus the CREATURES lines of every
// root's `modify/world/<region>/world_<region>.txt` [MERGE] blocks, how the
// Watcher adds its own to the vanilla regions) lists den spawns
// (`ROOM : den-Type[-{data}][-count], ...`) and lineages (`LINEAGE : ROOM :
// den : Type[-{data}]-chance, ...`), each line possibly for some campaigns
// only (`(White,Yellow)ROOM...`, `(X-Saint)` for all but those). A fresh
// save's lineage spawns its first step (NONE: nothing). OFFSCREEN spawns go
// to the world's offscreen den (scavengers, vultures...: none of them is
// ported). At world load every spawn of a ported kind is made in its den,
// all regions and rooms at once, with an EntityID-like number for its seed
// ({Seed:N} replaces it); creatures leave their dens as their
// remainInDenCounter runs out.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "creature.hpp"
#include "gamestate.hpp"
#include "player_types.hpp"

namespace spawns {
    // one den spawn or lineage, as the world file has it
    struct spawn {
        std::string room;     // as written (OFFSCREEN for the offscreen den)
        int32_t den = 0;      // the room's exitAndDenIndex node
        std::string type;     // as written; a lineage's current step
        int32_t count = 1;
        std::string data;     // the spawn data, "{...}" or empty
        bool lineage = false;
        bool night = false;   // a lineage's fifth field
    };
    // the CREATURES lines of a world file kept for the campaign (`story`),
    // a lineage resolved to its first step (left out when that is NONE)
    std::vector<spawn> parse(std::string_view text, ::player::slugcat_name story);

    // the ported kind of a creature type name (WorldLoader.CreatureTypeFromString)
    std::optional<creature::spawn_desc> kind_of(std::string_view type) noexcept;
    // AbstractCreature.setCustomFlags: the flags of a spawn data string
    // (dens.hpp's spawn_flag), its {Seed:N} (-1 none) and a centipede's
    // size (an unrecognised number, < 0 none)
    struct spawn_data {
        uint8_t flags = 0;
        int32_t seed = -1;
        float size = -1.f;
    };
    spawn_data parse_data(std::string_view data) noexcept;

    // what world load made
    struct report {
        std::map<std::string, uint32_t> made;     // by kind and variant
        std::map<std::string, uint32_t> skipped;  // not ported, by type as written
        uint32_t creatures = 0;
        uint32_t lineages = 0;      // lineages whose first step was a ported kind
        uint32_t lineage_none = 0;  // ... or NONE
        uint32_t night = 0, pre_cycle = 0, ignore_cycle = 0;
        uint32_t no_room = 0, no_den = 0;
        // which (the first few)
        std::vector<std::string> missing;
    };
    // every region's spawns of ported kinds, made in their dens
    report spawn_world(sys::game_state& state);
    // one spawn, `n` creatures (the tests' synthetic world files)
    uint32_t spawn_in_den(sys::game_state& state, dcon::room_id room, spawn const& s, report& r);
    std::string to_string(report const& r);
}
