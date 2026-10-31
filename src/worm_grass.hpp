#pragma once

// Worm grass (WormGrass): patches of worms on the floor tiles the room's
// geometry marks. The worms wiggle up from the ground, get excited by the
// creatures over their patch, reach for them and stick to what they touch;
// a creature in the thick of a patch is pulled down, and once buried it dies
// and is eaten (a slugcat stays, dead and gone, like the rot's prey).
//
// Not a creature in vanilla either (an UpdatableAndDeletable of the room):
// its patches (worm_grass_patch) and worms (worm_grass_worm) are data
// objects of their room, like the rot.

#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace worm_grass {
    // Room.Loaded's first visit: the patches and their worms
    void spawn_room(sys::game_state& state, dcon::room_id room);
    // WormGrass.Update: the worms reach for the creatures over them and
    // pull them in
    void update_room(sys::game_state& state, world::room_view const& room);
    // lets go of a creature (a slugcat respawned, destroyed)
    void release(sys::game_state& state, dcon::creature_id c);
    // WormGrass.Explosion: the worms near it are thrown off what they hold
    // (from the exploding room's update)
    void explosion(sys::game_state& state, dcon::room_id room, rw::vec2 at, float rad, float force);
    // the worms' DrawSprites
    void build(sys::game_state const& state, dcon::room_id room, float alpha, sprites::room_palette const& pal, std::vector<sprites::draw_cmd>& out,
        std::vector<sprites::strip_vertex>& strip);
}
