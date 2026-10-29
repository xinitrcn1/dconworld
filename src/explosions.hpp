#pragma once

// Explosion (the ScavengerBomb's, ExplosiveSpear's and Artificer's): pushes
// every body chunk in the radius away, hurts and stuns centipedes, kills a
// slugcat caught near its middle, and shows as a flash, sparks, smoke and,
// under water, bubbles and a wave on the surface. FlareBomb's flash blinds
// (stuns) creatures in sight instead.

#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace explosions {
    struct desc {
        float rad = 100.f;
        float force = 6.f;
        float damage = 1.f;  // to centipedes at the middle
        float stun = 60.f;   // ticks at the middle
        float kill_rad = 0.f; // slugcats closer than this die
    };
    void explode(sys::game_state& state, world::room_view const& room, rw::vec2 at, desc const& d, dcon::physical_object_id source = {});
    void flash(sys::game_state& state, world::room_view const& room, rw::vec2 at, float rad);
}
