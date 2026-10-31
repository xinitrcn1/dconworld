#pragma once

// Vultures (Vulture.cs, VultureAI, VultureTentacle on Tentacle, KingTusks)
// in their three templates, one kind with a variant: Vulture, KingVulture
// and the Downpour MirosVulture.
//
// The body: five chunks, 0 the front (mainBodyChunk), 1 the back, 2 and 3
// the bases of the wings, 4 the head at the end of the neck (a Tentacle of
// four chunks). Its wings (two, four for the miros vulture) are
// VultureTentacles, creature_parts with a `vulture_wing` data object each:
// climbing, a wing reaches for a grip on the terrain or a pole and carries
// the body; flying, the wings flap and lift it. Four thrusters (jets of
// smoke) push the body where it is going when it moves against its momentum
// (VultureThruster.Utility), and on take off.
//
// What it does to the slugcat: a vulture (and the king) hunts it from the
// air, charges its tusks and snaps with its head (Snap): the head that
// touches the slugcat grabs it (a bite, 0.4 damage) and carries it off
// (ReturnPrey); a vulture that dislikes it (its mask knocked off) only bites
// (OnlyHurtDontGrab). The king also shoots its harpoons (KingTusks): one
// that hits the slugcat impales it (1.5 damage: it dies) and the wire pulls
// it to the head, which grabs it. The miros vulture snaps its jaws shut on
// what is in front of them (a bite of 1.2: it kills) and, when hurt, fires
// its laser: a beam from the head that, after 190 ticks, explodes where it
// meets the terrain. The vulture and the king wear a mask that turns
// spears and rocks coming at it head on (SpearStick: a tenth of the damage)
// and comes off under a hit of more than 0.9; no VultureMask item is
// dropped (see DIVERGENCES). The jets push the vulture itself; their smoke
// (vanilla's NewVultureSmoke) pushes nothing and is drawn as dust puffs. Carried prey taken off over the top of
// the room, or into a den, is eaten.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "vulture_types.hpp"
#include "world.hpp"

namespace vulture {
    using creature::body;
    using creature::chunk;
    using creature::consious;

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos, variant v);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // Vulture.Update (its AI's, tentacles', thrusters' and tusks' with it)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // whether it collides with `t` at all (not what it holds)
    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t);
    // Vulture.Collide: its chunk `i` touched `t`'s chunk `j` (the snapping
    // head grabs or bites)
    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j);
    // Vulture.Violence + Creature.Violence, on a chunk or on an appendage
    // (the neck: part.tentacle 0, a wing: its number + 1)
    void violence(sys::game_state& state, dcon::creature_id c, creature::hit_part const& part, rw::vec2 momentum, float damage, float stun_bonus,
        creature::damage_type type);
    // it lets go of `t` (the beak, the tusks impaling it)
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // Vulture.Die, after Creature.Die
    void died(sys::game_state& state, dcon::creature_id c);
    // Vulture.SpitOutOfShortCut + NewRoom: out of a pipe in `room` at `at`
    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, rw::vec2 at);
    // a line for the log
    std::string describe(sys::game_state const& state, dcon::creature_id c);
    // where the tests' spawns may put one (--fuzz, --bench): open air with
    // room for the wings
    bool spawn_spot(world::room_view const& room, rw::ivec2 tile);

    // the room's vultures' appendages a projectile from `from` to `to`
    // crosses (IHaveAppendages: the neck, then the wings), the first one:
    // the creature, the appendage (part.tentacle), the segment, and how
    // far along the projectile's way (`at`, 0..1)
    bool trace_appendages(sys::game_state const& state, dcon::room_id room, rw::vec2 from, rw::vec2 to, dcon::creature_id thrown_by, dcon::creature_id& hit,
        creature::hit_part& part, float& at);

    // the variant of a vulture
    variant variant_of(sys::game_state const& state, dcon::creature_id c) noexcept;
    // VultureState.mask
    bool has_mask(sys::game_state const& state, dcon::creature_id c) noexcept;

    // The world files' OFFSCREEN vultures (no offscreen den here): a den
    // of a room of `region` with open sky over it and room in front of the
    // den's hole, rolled from `seed` (none: no such room); the spawn goes
    // there like a den spawn
    std::optional<std::pair<dcon::room_id, uint8_t>> offscreen_den(sys::game_state const& state, dcon::region_id region, int32_t seed);
}
