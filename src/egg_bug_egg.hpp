#pragma once

// EggBugEgg and Downpour's FireEgg: the eggs an egg bug (fire bug) drops.
// Both are one handed, edible items (IPlayerEdible: two bites, one food
// pip); the egg bug's dangle a little tail of goo and slowly swell up, the
// fire bug's is a grenade: tossed by a slugcat (FireEgg.Tossed) its fuse
// starts, it sticks to the first creature or wall it meets and after 180
// ticks goes off in a blast that kills the slugcats around (Explosion of
// radius 225, 50 damage). items.cpp keeps them like its other items and
// calls in here for what is their own; their own state is the
// egg_bug_egg_data object (egg_bug_types.hpp's egg_state).

#include <cstdint>
#include <vector>

#include "gamestate.hpp"
#include "math.hpp"
#include "player_types.hpp"
#include "sprites.hpp"
#include "world.hpp"

namespace egg_bug_egg {
    // the item types here (world::item_type egg_bug_egg, fire_egg)
    bool is_egg(world::item_type t) noexcept;

    // the EggBugEgg / FireEgg constructor's body, for an item just made by
    // items.cpp (its data object made and linked)
    void init(sys::game_state& state, dcon::item_id it);
    // PlaceInRoom / NewRoom: a random rotation, the tail reset
    void place(sys::game_state& state, dcon::item_id it);
    // gone (items::destroy): its data object
    void destroyed(sys::game_state& state, dcon::item_id it);
    // EggBug.DropEggs' AbstractBugEgg: the bug's hue, the velocity it
    // leaves with, setRotation, swell 0, rotVel, liquid 1
    void dropped(sys::game_state& state, dcon::item_id it, float hue, rw::vec2 vel, rw::vec2 rotation, float rot_vel);
    // EggBugEgg/FireEgg.Update, after the item's physics (PhysicalObject.Update):
    // swelling, the rotation, the tail; a FireEgg's fuse, sticking to what
    // it meets. True when a FireEgg goes off (items.cpp then calls explode
    // and destroys it)
    bool update(sys::game_state& state, dcon::item_id it, world::room_view const& room);
    // FireEgg.Tossed: thrown by `by`, the fuse starts
    void tossed(sys::game_state& state, dcon::item_id it, dcon::creature_id by);
    // FireEgg.Explode at `at`
    void explode(sys::game_state& state, world::room_view const& room, dcon::item_id it, rw::vec2 at);

    // IPlayerEdible.BitByPlayer: a bite by `p` (the egg pulled to its
    // mouth); true when it is eaten up (bites left 0: items.cpp feeds the
    // slugcat, lets go and destroys it)
    bool bite(sys::game_state& state, dcon::item_id it, dcon::player_id p);
    // IPlayerEdible.BitesLeft
    int32_t bites_left(sys::game_state const& state, dcon::item_id it) noexcept;
    // SlugcatStats.NourishmentOfObjectEaten: quarter pips for `name`
    // (FoodPoints 1: four quarters; Hunter and Artificer get a single
    // quarter of a FireEgg, Spearmaster nothing)
    int32_t nourishment(world::item_type t, player::slugcat_name name) noexcept;

    // EggBugEgg/FireEgg.DrawSprites (ApplyPalette: EggBugGraphics.EggColors
    // of the palette's black and fog)
    void build(sys::game_state const& state, dcon::item_id it, float alpha, rw::vec3 black, rw::vec3 fog, float darkness, std::vector<sprites::draw_cmd>& out);
}
