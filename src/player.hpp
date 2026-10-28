#pragma once

// Port of the slugcat movement code from Player.cs: Update (the movement
// relevant parts), MovementUpdate, UpdateBodyMode, UpdateAnimation, Jump,
// WallJump, TerrainImpact, LungUpdate and helpers. Base game behaviour only:
// no Remix (MMF), Downpour or Watcher slugcat mechanics, no items/grasps.
//
// Players are `player` objects in the data container (one per co-op slot),
// each controlling a physical_object body through the player_body relation.

#include <cstdint>

#include "gamestate.hpp"
#include "math.hpp"
#include "player_types.hpp"
#include "world.hpp"

namespace player {
    char const* to_string(body_mode_index m) noexcept;
    char const* to_string(animation_index a) noexcept;

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

    dcon::physical_object_id body(sys::game_state const& state, dcon::player_id p) noexcept;
    // bodyChunks[i] of the player's body
    dcon::body_chunk_id body_chunk(sys::game_state const& state, dcon::player_id p, uint32_t i) noexcept;
}
