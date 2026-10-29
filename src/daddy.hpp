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
#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace daddy {
    char const* to_string(variant v) noexcept;
    // "daddy", "brother", "mother" (or "terror"), "hunter"
    bool parse_variant(char const* name, variant& out) noexcept;

    // DaddyLongLegs constructor + PlaceInRoom (and the DaddyGraphics layout)
    dcon::daddy_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos, variant v);
    void destroy(sys::game_state& state, dcon::daddy_id d);
    // DaddyLongLegs.Update (+ DaddyGraphics.Update) for every daddy in the room
    void update_room(sys::game_state& state, world::room_view const& room);
    // Room object collisions between daddies and slugcats (pushing apart,
    // DaddyLongLegs.Collide starting to eat a slugcat a tentacle holds)
    void collide(sys::game_state& state, world::room_view const& room);

    // What a projectile hit: a body chunk, or a tentacle (an Appendage.Pos:
    // the segment before the hit and how far along it)
    struct hit_part {
        int32_t chunk = -1;
        int32_t tentacle = -1;
        int32_t prev_segment = 0;
        float distance_to_next = 0.f;
    };
    // Weapon.Update's projectile trace (SharedPhysics.TraceProjectileAgainstBodyChunks
    // with its appendages): the daddy the segment hits first
    bool trace(sys::game_state const& state, dcon::room_id room, rw::vec2 from, rw::vec2 to, float rad, dcon::daddy_id& hit, hit_part& part);
    // DaddyLongLegs.Violence + Creature.Violence; `explosion` is the
    // Explosion damage type it barely resists
    void violence(sys::game_state& state, dcon::daddy_id d, hit_part const& part, rw::vec2 momentum, float damage, float stun_bonus, bool explosion = false);

    dcon::physical_object_id body(sys::game_state const& state, dcon::daddy_id d) noexcept;
    uint32_t chunk_count(sys::game_state const& state, dcon::daddy_id d) noexcept;
    dcon::body_chunk_id chunk(sys::game_state const& state, dcon::daddy_id d, uint32_t i) noexcept;
    dcon::daddy_tentacle_id tentacle_of(sys::game_state const& state, dcon::daddy_id d, uint32_t i) noexcept;
    dcon::daddy_tube_id tube_of(sys::game_state const& state, dcon::daddy_id d, uint32_t i) noexcept;
    uint32_t tube_count(sys::game_state const& state, dcon::daddy_id d) noexcept;
    bool consious(sys::game_state const& state, dcon::daddy_id d) noexcept;
    // DaddyLongLegs.MiddleOfBody
    rw::vec2 middle_of_body(sys::game_state const& state, dcon::daddy_id d) noexcept;
    // the slugcat `p` held by one of its tentacles / being eaten
    bool holds(sys::game_state const& state, dcon::daddy_id d, dcon::player_id p) noexcept;
    bool eating(sys::game_state const& state, dcon::daddy_id d, dcon::player_id p) noexcept;
    // tentacles gripping the terrain (atGrabDest)
    uint32_t legs_gripping(sys::game_state const& state, dcon::daddy_id d) noexcept;

    // lets go of the player (respawned, destroyed, eaten by something else)
    void release_player(sys::game_state& state, dcon::player_id p);
}
