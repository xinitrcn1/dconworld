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

#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace dropbug {
    // DropBug constructor + PlaceInRoom: sits in the ceiling if `pos` is a
    // valid ceiling spot. `relocate` moves it to the best ceiling spot of the
    // room like a story session does, else (the spawn tools) to the ceiling
    // spot straight above `pos` if there is one
    dcon::dropbug_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos, bool relocate = false);
    void destroy(sys::game_state& state, dcon::dropbug_id d);
    // DropBug.Update (+ DropBugGraphics.Update) for every dropbug in the room
    void update_room(sys::game_state& state, world::room_view const& room);
    // Room object collisions between dropbugs and slugcats (pushing apart,
    // DropBug.Collide biting and grabbing) and between dropbugs
    void collide(sys::game_state& state, world::room_view const& room);
    // Weapon.Update's projectile trace against dropbugs: the dropbug the
    // segment hits (radius `rad`), and which chunk
    bool trace(sys::game_state const& state, dcon::room_id room, rw::vec2 from, rw::vec2 to, float rad, dcon::dropbug_id& hit, uint32_t& chunk);
    // DropBug.Violence (+ Creature.Violence)
    void violence(sys::game_state& state, dcon::dropbug_id d, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus);
    // DropBug.FlyingWeapon (Weapon.INotifyOfFlyingWeapons): a thrown weapon
    // at `pos` moving at `vel`, thrown from `thrown_pos`
    void flying_weapon(sys::game_state& state, dcon::room_id room, rw::vec2 pos, rw::vec2 vel, rw::vec2 thrown_pos);

    dcon::physical_object_id body(sys::game_state const& state, dcon::dropbug_id d) noexcept;
    // bodyChunks[i]: 0 the head (mainBodyChunk), 1 the middle, 2 the tail
    dcon::body_chunk_id chunk(sys::game_state const& state, dcon::dropbug_id d, uint32_t i) noexcept;
    bool consious(sys::game_state const& state, dcon::dropbug_id d) noexcept;
    // DropBug.Footing
    bool footing(sys::game_state const& state, dcon::dropbug_id d) noexcept;
    // CeilingSitModule.SittingInCeiling
    bool sitting_in_ceiling(sys::game_state const& state, dcon::dropbug_id d) noexcept;
    // the player it holds (grasps[0]), if any
    dcon::player_id grasped(sys::game_state const& state, dcon::dropbug_id d) noexcept;
    // releases `p` if a dropbug holds it
    void release_player(sys::game_state& state, dcon::player_id p);
}
