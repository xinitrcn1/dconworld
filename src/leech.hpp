#pragma once

// Leeches (Leech.cs): one small chunk that swims in schools (LeechSchool),
// hunts what swims near it, charges at it and latches on, dragging it down
// under the water while it bites; out of the water it slowly drowns,
// inching along the ground. Leech, SeaLeech (bigger and heavier, keeps to a
// territory column, pulls harder) and Downpour's JungleLeech (lives on land,
// wanders between floor spots and drains the slugcat's food) all run it,
// the payload's `variant` tells them apart. Leech has no AI and no
// pathfinding: it steers by its swim direction alone.
//
// A school is kept by one of its leeches (its leech_data holds the prey list
// and the next member's place); its members point at it and are ordered by
// their place, like LeechSchool.leeches. Schools never span rooms.

#include <cstdint>
#include <string_view>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace leech {
    using creature::body;
    using creature::chunk;
    using creature::consious;

    char const* to_string(variant v) noexcept;
    // "leech", "sea", "jungle"
    bool parse_variant(std::string_view name, variant& out) noexcept;

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos, variant v = variant::leech);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // Leech.Update
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // Creature.Violence (+ Leech.Stun)
    void violence(sys::game_state& state, dcon::creature_id c, rw::vec2 momentum, float damage, float stun_bonus);
    // lets go of `p` if it is latched on to it
    void release(sys::game_state& state, dcon::creature_id c, creature::target t);

    // the slugcat it is latched on to (grasps[0]), if any
    creature::target attached_to(sys::game_state const& state, dcon::creature_id c) noexcept;
    // its school's size (LeechSchool.leeches.Count)
    uint32_t school_size(sys::game_state const& state, dcon::creature_id c);
}
