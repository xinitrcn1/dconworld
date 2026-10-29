#pragma once

// Daddy long legs (DaddyLongLegs.cs, DaddyAI, DaddyTentacle, Tentacle) in
// the four templates that run it: the Daddy, the Brother, Downpour's Mother
// (TerrorLongLegs) and the Hunter long legs (HunterDaddy). A cluster of 4
// to 15 body chunks held up by 4 to 12 tentacles: each tentacle keeps a
// path of room tiles from the body (Tentacle's segments, pulled straight,
// grown, shortened and searched around the terrain with its own tile path
// finder), its chunks follow the tiles on ropes that wrap the tile corners,
// stick to the terrain they touch and give the body its support. The body
// moves along a distance field from pathing.hpp (the template's
// TileTypeResistances: every open tile) towards its destination, as far as
// the gripping tentacles carry it; tentacles not needed to hold it reach
// for grips ahead, examine noises and hunt.
//
// The daddy is blind: what its tentacles touch is what it knows of (the
// Tracker sees the slugcat on touch), and it hears (NoiseTracker). A
// hunting tentacle that brushes the slugcat with its tip grabs it and
// drags it to the body, where touching the body starts eating it.

#include <cstdint>

#include "daddy_types.hpp"
#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace daddy {
    using creature::body;
    using creature::chunk;
    using creature::chunk_count;
    using creature::consious;
    using creature::hit_part;

    char const* to_string(variant v) noexcept;
    // "daddy", "brother", "mother" (or "terror"), "hunter"
    bool parse_variant(char const* name, variant& out) noexcept;

    // DaddyLongLegs constructor + PlaceInRoom (and the DaddyGraphics layout)
    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos, variant v);
    void destroy(sys::game_state& state, dcon::creature_id d);
    // DaddyLongLegs.Update
    void update(sys::game_state& state, dcon::creature_id d, world::room_view const& room);
    // Room object collisions: whether it collides with the slugcat (not one
    // in a pipe or being eaten), DaddyLongLegs.Collide starting to eat a
    // slugcat a tentacle holds (its chunk `i` touched the slugcat's `j`)
    bool collides(sys::game_state const& state, dcon::creature_id d, dcon::player_id p, dcon::room_id room);
    void collide_player(sys::game_state& state, dcon::creature_id d, uint32_t i, dcon::player_id p, uint32_t j);

    // SharedPhysics.TraceProjectileAgainstBodyChunks' appendages: the
    // tentacle of the room's daddies the segment crosses first
    bool trace_tentacles(sys::game_state const& state, dcon::room_id room, rw::vec2 from, rw::vec2 to, dcon::creature_id& hit, hit_part& part);
    // DaddyLongLegs.Violence + Creature.Violence; `explosion` is the
    // Explosion damage type it barely resists
    void violence(sys::game_state& state, dcon::creature_id d, hit_part const& part, rw::vec2 momentum, float damage, float stun_bonus, bool explosion = false);

    // its parts (creature_part): the tentacles, then the tube graphics, and
    // their data objects
    inline dcon::daddy_tentacle_id tentacle_data(dcon::data_container const& w, dcon::creature_part_id t) noexcept {
        return dcon::daddy_tentacle_id{dcon::daddy_tentacle_id::value_base_t(w.creature_part_get_payload(t).daddy_tentacle.data)};
    }
    inline dcon::daddy_tube_id tube_data(dcon::data_container const& w, dcon::creature_part_id t) noexcept {
        return dcon::daddy_tube_id{dcon::daddy_tube_id::value_base_t(w.creature_part_get_payload(t).daddy_tube.data)};
    }
    inline uint32_t tentacle_count(sys::game_state const& state, dcon::creature_id d) noexcept {
        return state.world.creature_get_payload(d).daddy.tentacle_count;
    }
    inline dcon::creature_part_id tentacle_part(sys::game_state const& state, dcon::creature_id d, uint32_t i) noexcept {
        return creature::part(state.world, d, i);
    }
    inline dcon::daddy_tentacle_id tentacle_of(sys::game_state const& state, dcon::creature_id d, uint32_t i) noexcept {
        return tentacle_data(state.world, tentacle_part(state, d, i));
    }
    inline dcon::creature_part_id tube_of(sys::game_state const& state, dcon::creature_id d, uint32_t i) noexcept {
        return creature::part(state.world, d, tentacle_count(state, d) + i);
    }
    uint32_t tube_count(sys::game_state const& state, dcon::creature_id d) noexcept;
    // DaddyLongLegs.MiddleOfBody
    rw::vec2 middle_of_body(sys::game_state const& state, dcon::creature_id d) noexcept;
    // the slugcat `p` held by one of its tentacles / being eaten
    bool holds(sys::game_state const& state, dcon::creature_id d, dcon::player_id p) noexcept;
    bool eating(sys::game_state const& state, dcon::creature_id d, dcon::player_id p) noexcept;
    // tentacles gripping the terrain (atGrabDest)
    uint32_t legs_gripping(sys::game_state const& state, dcon::creature_id d) noexcept;

    // lets go of the player (respawned, destroyed, eaten by something else)
    void release_player(sys::game_state& state, dcon::creature_id d, dcon::player_id p);
}
