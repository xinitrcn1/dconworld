#pragma once

// Overseer (Overseer.cs + OverseerAI + what a realized overseer needs of
// OverseerAbstractAI): Five Pebbles' (or Moon's) little eyes. An overseer is
// a single 3 px chunk floating on a stalk out of a wall: its root sits in a
// solid tile (`root_tile`), its eye hovers over an open tile next to the
// terrain (`hover_tile`) and looks at things (`look_at`). It changes walls by
// withdrawing into the root, zipping through the terrain along a zip path
// (the zip pathing matrix: solid tiles cheap, open air dear) and emerging
// at the new root. It never collides with anything (collision layer 0, no
// terrain collision) and only a weapon can hit it while it is out of the
// wall (extended > 0.2): a spear kills it, it flickers out over six ticks
// and is gone.
//
// OverseerAI: it looks at its target creature (the most interesting one of
// the room: the slugcat by its karma, scavengers, other overseers, the
// lizards...) or one of casual interest, at flying weapons, else at random
// spots in the air; it picks a hover tile about 300 px from what it looks
// at and away from creatures, hides in the wall when a big creature comes
// within 130 px or a weapon flies at it, and comes out once nothing is
// within 200 px. Two overseers close to each other "converse" a moment.
//
// OverseerAbstractAI (without the abstract layer): its target creature is
// followed between rooms (it teleports: zips out of the room towards the
// other room and zips into the target's, wherever it is in the region);
// without one it roams to the neighbouring rooms. Before the rain it goes
// to the offscreen den (here: hidden in its wall until the next cycle,
// then to a room of its region). The player guide (one per world for the
// campaigns that have one, yellow) comes to the first slugcat now and then
// across regions and goes away again (PlayerGuideUpdate, simplified).
// Holograms, the tutorial and the communication module aren't ported.

#include <cstdint>
#include <string>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace overseer {
    using creature::body;
    using creature::chunk;

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // Overseer.Update (its AI's, and AbstractBehavior's, with it)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // collision layer 0: it touches no creature (false)
    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t);
    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j);
    // Overseer.Violence (Creature.Violence, HealthState) + HitByWeapon
    void violence(sys::game_state& state, dcon::creature_id c, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus, creature::damage_type type);
    // it holds nothing
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // Overseer.Die, after Creature.Die (the dying flicker runs in update)
    void died(sys::game_state& state, dcon::creature_id c);
    // Overseer.PlaceInRoom: a random open tile beside a wall of `room`
    // (`at`'s tile when none is found), out of the wall at once
    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, rw::vec2 at);
    // a line for the log
    std::string describe(sys::game_state const& state, dcon::creature_id c);
    // where the tests' spawns may put one (--fuzz, --bench): an open tile
    // beside a solid one
    bool spawn_spot(world::room_view const& room, rw::ivec2 tile);

    // Overseer.canBeHitByWeapons (extended > 0.2, not in the offscreen den):
    // creature::trace skips it otherwise
    bool can_be_hit(sys::game_state const& state, dcon::creature_id c) noexcept;
    // not drawn: waiting out the rain in the offscreen den
    bool hidden(sys::game_state const& state, dcon::creature_id c) noexcept;
    // Weapon.INotifyOfFlyingWeapons (OverseerAI.FlyingWeapon): a thrown
    // weapon at `pos` moving at `vel`
    void flying_weapon(sys::game_state& state, dcon::creature_id c, rw::vec2 pos, rw::vec2 vel);

    // GraphicsModule's view of it: OverseerAI.CosmeticLookAt
    rw::vec2 cosmetic_look_at(sys::game_state const& state, dcon::creature_id c) noexcept;

    // WorldLoader.GeneratePopulation's overseers and OverseerSpawnConditions'
    // player guide, for every region at once (no abstract layer: each one
    // put straight into a room the way the offscreen den's Roam picks it)
    uint32_t spawn_world(sys::game_state& state);
}
