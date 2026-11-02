#pragma once

// DartMaggot: what a spitter spider spits (BigSpider.Spit). A little
// maggot on a needle shot at 70 pixels a tick: it sticks in the first
// creature it hits (a stab of 0.07 damage) or in a wall it meets straight
// on, bounces off one it meets at a slant. Stuck in a creature, its venom
// works after UntilSleepDelay (340 ticks): it swells, the creature
// twitches into short stuns, and then it sleeps (a stun of 40 * (2 + 3 *
// darts in it, up to 4)); the spitter that shot it knows (its prey is
// "tagged": SpiderSpitModule.CreatureHitByDart). It lives a while
// (1100 to 2100 ticks), wriggling, then rots and fades away. A thread of
// slime (Umbilical) trails back to the spitter for a moment (cosmetic).
//
// A PhysicalObject, not something a slugcat can carry: items.cpp keeps
// it like its other items (an item of type dart_maggot) without the
// weapon modes, and calls `update` for its whole Update (the physics
// included); its own state is the dart_maggot_data object
// (big_spider_types.hpp's maggot_state).
//
// Kept to its room: stuck in a creature that leaves the room (or isn't
// out in it any more), it drops out and rots (vanilla's
// DartMaggotStick carries it along); weapons don't knock it out of the air
// (HitByWeapon).

#include <cstdint>
#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace dart_maggot {
    // world::item_type::dart_maggot
    bool is_maggot(world::item_type t) noexcept;
    // the DartMaggot constructor's body, for an item just made by
    // items.cpp (its data object made and linked)
    void init(sys::game_state& state, dcon::item_id it);
    // PlaceInRoom / NewRoom: the body reset
    void place(sys::game_state& state, dcon::item_id it);
    // gone (items::destroy): its data object
    void destroyed(sys::game_state& state, dcon::item_id it);
    // DartMaggot.Shoot: from `pos` along `dir` (normalised), shot by `by`
    // (a spitter: the Umbilical to it)
    void shoot(sys::game_state& state, dcon::item_id it, rw::vec2 pos, rw::vec2 dir, dcon::creature_id by);
    // DartMaggot.Update (+ PhysicalObject.Update when not shot) and the
    // Umbilical's: true when it is to be destroyed (out of the room, rotted
    // away; items.cpp destroys it after the rooms' update)
    bool update(sys::game_state& state, dcon::item_id it, world::room_view const& room);
    // DartMaggot.DrawSprites and the Umbilical's (ApplyPalette from the
    // palette's black and fog, `darkness` the room's)
    void build(sys::game_state const& state, dcon::item_id it, float alpha, rw::vec3 black, rw::vec3 fog, float darkness, std::vector<sprites::draw_cmd>& out);
}
