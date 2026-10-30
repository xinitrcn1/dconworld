#pragma once

// Port of the slugcat movement code from Player.cs: Update (the movement
// relevant parts), MovementUpdate, UpdateBodyMode, UpdateAnimation, Jump,
// WallJump, TerrainImpact, LungUpdate and helpers, with every slugcat's stats
// and Rivulet's movement branches. No Remix (MMF) or other Downpour/Watcher
// abilities. Shortcuts and items hook in from shortcuts.* and items.*.
//
// A slugcat is a creature (creature.hpp, type slugcat): its body, stun,
// death, grasps, pipes and rain are the creature's like any kind's. What
// only the slugcat has (Player.cs' movement state, PlayerGraphics, the
// abilities, food) and its controller's input is its `player` object,
// which its creature's payload points at (like a kind's data object) and
// which points back at its creature.

#include <cstdint>

#include "gamestate.hpp"
#include "math.hpp"
#include "player_types.hpp"
#include "world.hpp"

namespace player {
    char const* to_string(body_mode_index m) noexcept;
    char const* to_string(animation_index a) noexcept;
    char const* to_string(slugcat_name n) noexcept;

    // Builds an input package the way RWInput.PlayerInputLogic does from
    // a (possibly digital) stick: directions are derived from analogueDir
    input_package make_input(float axis_x, float axis_y, bool jmp, bool thrw, bool pckp) noexcept;

    // Creates a player and its two body chunk slugcat body (Player
    // constructor) with the lower chunk at `pos` (room pixel coordinates)
    dcon::player_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos, slugcat_name name = slugcat_name::white);
    // Removes the player and its body
    void destroy(sys::game_state& state, dcon::player_id p);
    // Moves the body to `room`/`pos`, clearing velocities and movement state
    void reset(sys::game_state& state, dcon::player_id p, dcon::room_id room, rw::vec2 pos);
    // One 40Hz tick (Player.Update), `in` is this tick's controller input
    void update(sys::game_state& state, dcon::player_id p, world::room_view const& room, input_package const& in);
    void stun(sys::game_state& state, dcon::player_id p, int32_t st);
    void die(sys::game_state& state, dcon::player_id p);
    // Player.Violence (through Creature.Violence) on chunk `k`
    void violence(sys::game_state& state, dcon::player_id p, uint32_t k, rw::vec2 momentum, float damage, float stun_bonus);

    // the slugcat creature it drives
    inline dcon::creature_id creature_of(sys::game_state const& state, dcon::player_id p) noexcept { return state.world.player_get_creature(p); }
    inline bool dead(sys::game_state const& state, dcon::player_id p) noexcept { return state.world.creature_get_dead(creature_of(state, p)) != 0; }
    inline int32_t stun_of(sys::game_state const& state, dcon::player_id p) noexcept { return state.world.creature_get_stun(creature_of(state, p)); }
    // travelling a pipe (its vessel; the body stays in the room it left)
    inline bool in_shortcut(sys::game_state const& state, dcon::player_id p) noexcept { return state.world.creature_get_away(creature_of(state, p)) != 0; }

    dcon::physical_object_id body(sys::game_state const& state, dcon::player_id p) noexcept;
    // bodyChunks[i] of the player's body
    dcon::body_chunk_id body_chunk(sys::game_state const& state, dcon::player_id p, uint32_t i) noexcept;
}
