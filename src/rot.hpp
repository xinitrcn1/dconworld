#pragma once

// The rot (DaddyCorruption): the Corruption and PassiveCorruption areas of a
// room grow bulbs on the walls next to them, some with eyes looking around
// for noises and touches. Bulbs nibble at creatures touching them and once
// enough of them have, the creature is pulled into the wall and eaten. Some
// bulbs grow a little leg (LittleLeg) that grabs prey and pulls it in, or a
// passive tendril. CorruptionTube placed objects are climbable rot vines
// stretched between two walls (ClimbableCorruptionTube, with the Rope that
// wraps them around tile corners).
//
// `vines` is ClimbableVinesSystem: the slugcat grabs, climbs and swings on
// the climbable tubes of its room through it.

#include <cstdint>
#include <optional>

#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace rot {
    // DaddyCorruption.AIMapReady on the room's first realisation: lays out the
    // bulbs (seeded like vanilla, so the same room always grows the same rot)
    // and the tubes. Rooms without a Corruption area get no rot (their
    // CorruptionTubes neither, like vanilla).
    void spawn_room(sys::game_state& state, dcon::room_id room);
    dcon::rot_system_id system_of(sys::game_state const& state, dcon::room_id room) noexcept;
    // DaddyCorruption.Update, its noises and tubes, and the creatures it eats
    void update_room(sys::game_state& state, world::room_view const& room);
    // lets go of a creature (a slugcat respawned, destroyed)
    void release(sys::game_state& state, dcon::creature_id c);

    // ClimbableVinesSystem over the climbable rot tubes of a room
    namespace vines {
        struct position {
            dcon::rot_tube_id vine;
            float float_pos = 0.f;
            // a pole plant's stalk instead (PoleMimic is an IClimbableVine;
            // `vine` empty then)
            dcon::creature_id mimic;
            // a sky whale's whisker + 1 (`mimic` is then the whale), 0 none
            uint8_t whisker = 0;
        };
        // the closest vine within `rad` of `pos` (VineOverlap)
        std::optional<position> overlap(sys::game_state const& state, dcon::room_id room, rw::vec2 pos, float rad);
        // still a climbable vine of `room`
        bool valid(sys::game_state const& state, dcon::room_id room, dcon::rot_tube_id vine) noexcept;
        // ...the tube or pole plant of a position
        bool valid(sys::game_state const& state, dcon::room_id room, position const& p) noexcept;
        float total_length(sys::game_state const& state, position const& p);
        // VineBeingClimbedOn, VineCurrentlyClimbable (a pole plant wakes up)
        void being_climbed_on(sys::game_state& state, position const& p, dcon::creature_id by);
        bool currently_climbable(sys::game_state const& state, position const& p);
        rw::vec2 on_vine_pos(sys::game_state const& state, position p);
        float vine_rad(sys::game_state const& state, position p);
        rw::vec2 vine_dir(sys::game_state const& state, position p);
        float total_length(sys::game_state const& state, dcon::rot_tube_id vine);
        float climb_on_vine_speed(sys::game_state const& state, position p, rw::vec2 goal);
        std::optional<position> vine_switch(sys::game_state const& state, dcon::room_id room, position p, rw::vec2 goal, float rad);
        void connect_chunk_to_vine(sys::game_state& state, dcon::body_chunk_id chunk, position p, float con_rad);
        void push_at_vine(sys::game_state& state, position p, rw::vec2 push);
    }

    // TubeGraphic.OneDimensionalTubePos: a point `float_pos` (0..1) along the
    // drawn tube, `alpha` interpolating between ticks
    rw::vec2 tube_graphic_pos(sys::game_state const& state, dcon::rot_tube_id tube, float float_pos, float alpha);

    // Rope.Update: moves the ends to `a`, `b`, wrapping around the solid
    // tile corners in between (the daddy long legs' tentacle chunks use it)
    void update_rope(world::rope_state& r, world::room_view const& room, rw::vec2 a, rw::vec2 b);
    // Rope.AConnect / Rope.BConnect
    inline rw::vec2 rope_a_connect(world::rope_state const& r) noexcept { return r.bend_count == 0 ? r.b : r.bends[0].pos; }
    inline rw::vec2 rope_b_connect(world::rope_state const& r) noexcept { return r.bend_count == 0 ? r.a : r.bends[r.bend_count - 1].pos; }
}
