#pragma once

// Dropwig (DropBug.cs, DropBugAI): a three chunk insect that sits hidden in
// the ceiling, hanging on its legs, and drops onto prey passing under it,
// biting or grabbing it; otherwise it walks floors, walls and ceilings and
// pounces from the ground.
//
// The AI is DropBugAI's utility comparison (prey, injury, being stuck and its
// CeilingSitModule picking a ceiling spot) over a simplified tracker (sight of
// the slugcat, contact) and its paths come from pathing.hpp's shared, on
// demand distance fields. Vanilla's AImap fields the ceiling spots are
// scored with (floor altitude, fall risk tile, narrow spaces, exit
// distances) are worked out from the tiles when asked for.

#include <cstdint>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace dropbug {
    using creature::body;
    using creature::chunk;
    using creature::consious;

    // DropBug constructor + PlaceInRoom: sits in the ceiling if `pos` is a
    // valid ceiling spot. `relocate` moves it to the best ceiling spot of the
    // room like a story session does, else (the spawn tools) to the ceiling
    // spot straight above `pos` if there is one
    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos, bool relocate = false);
    void destroy(sys::game_state& state, dcon::creature_id d);
    // DropBug.Update
    void update(sys::game_state& state, dcon::creature_id d, world::room_view const& room);
    // Room object collisions: whether it collides with a creature or
    // slugcat (not with what it holds), DropBug.Collide biting and grabbing
    // (its chunk `i` touched the other's `j`), climbing over another
    // dropbug and following the dominant ones (CollideWithKin)
    bool collides(sys::game_state const& state, dcon::creature_id d, dcon::creature_id t);
    void collide(sys::game_state& state, dcon::creature_id d, world::room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j);
    void collide_kin(sys::game_state& state, dcon::creature_id d, uint32_t i, dcon::creature_id other, uint32_t j);
    void touched_kin(sys::game_state& state, dcon::creature_id d, dcon::creature_id other);
    // DropBug.Violence (+ Creature.Violence)
    void violence(sys::game_state& state, dcon::creature_id d, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus);
    // DropBug.FlyingWeapon (Weapon.INotifyOfFlyingWeapons): a thrown weapon
    // at `pos` moving at `vel`, thrown from `thrown_pos`
    void flying_weapon(sys::game_state& state, dcon::creature_id d, rw::vec2 pos, rw::vec2 vel, rw::vec2 thrown_pos);

    // DropBug.Footing
    bool footing(sys::game_state const& state, dcon::creature_id d) noexcept;
    // CeilingSitModule.SittingInCeiling
    bool sitting_in_ceiling(sys::game_state const& state, dcon::creature_id d) noexcept;
    // what it holds (grasps[0]), if any
    dcon::creature_id grasped(sys::game_state const& state, dcon::creature_id d) noexcept;
    // lets go of `t` if it holds it
    void release(sys::game_state& state, dcon::creature_id d, dcon::creature_id t);
}
