#pragma once

// Player.SlugOnBack: a slugcat riding on another's back (Downpour's
// slugpups, Jolly Co-op's players). Here any slugcat on any other, in stacks
// of any height: each one carries the next on its back. A rider doesn't move
// on its own (it can still use its hands: Spearmaster pulls needles and
// throws them), doesn't bump into slugcats and isn't hit by weapons; it goes
// where the bottom of its stack goes, through the pipes too. Who rides whom
// (and the packs) is npc.cpp's; the player's slugcat takes a slugpup on its
// back with pick up, lets it off with down and pick up, and gets off with
// jump.

#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace piggyback {
    // the one `p` rides on, the one riding on `p` (none: none)
    dcon::player_id carrier(sys::game_state const& state, dcon::player_id p) noexcept;
    dcon::player_id rider(sys::game_state const& state, dcon::player_id p) noexcept;
    inline bool riding(sys::game_state const& state, dcon::player_id p) noexcept { return bool(carrier(state, p)); }
    // the bottom and the top of the stack `p` is in (itself alone)
    dcon::player_id bottom(sys::game_state const& state, dcon::player_id p) noexcept;
    dcon::player_id top(sys::game_state const& state, dcon::player_id p) noexcept;
    // slugcats in the stack
    uint32_t height(sys::game_state const& state, dcon::player_id p) noexcept;

    // SlugToBack: `who` (and whatever rides on it) onto the top of the stack
    // `onto` is in. Not when it can't: `who` riding already, the two in the
    // same stack, in different rooms, in a pipe, dead
    bool mount(sys::game_state& state, dcon::player_id onto, dcon::player_id who);
    // DropSlug: the one riding on `p` gets off (what rides on that one stays)
    void drop(sys::game_state& state, dcon::player_id p);
    // every link of the stack `p` is in comes apart
    void undo(sys::game_state& state, dcon::player_id p);
    // every stack with a slugcat within `rad` of `at` comes apart (the
    // Artificer's pyro jump)
    void scatter(sys::game_state& state, dcon::room_id room, rw::vec2 at, float rad);

    // The player's slugcat: pick up by a slugpup takes it on its back (a
    // slugpup played: onto the slugcat it is by), down and pick up lets the
    // one on its back off, jump gets off. Whether it did something (no item
    // pick up then)
    bool player_input(sys::game_state& state, dcon::player_id p);

    // After the room's players updated: the riders on their carriers' backs,
    // bottom up (SlugOnBack.GraphicsModuleUpdated). A rider dead or grabbed,
    // a carrier dead, or the two apart (another room, a pipe) and the link
    // comes apart
    void update_room(sys::game_state& state, world::room_view const& room);
}
