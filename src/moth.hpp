#pragma once

// Big and small moths (the Watcher's BigMoth.cs + BigMothAI.cs +
// BigMothPather.cs): one kind in two sizes. Vanilla's SmallMoth is the same
// BigMoth class under its own template (BigMoth.Small: scale 0.3 instead of
// 1, a few AI and movement tweaks); here the payload's variant.
//
// The body: five chunks, 0 the head (mainBodyChunk), 1 and 2 the thorax, 3
// and 4 the abdomen, held up by four legs (MothLeg: two pairs, the back ones
// on chunk 2, the front ones on chunk 1, stepping along the floor) and two
// wing pairs (MothWingPair: flapping when it wants to fly, gliding between
// flaps). It walks where there is ground under its legs and flies where
// there isn't (wantToFly). Its AI fears and flees, hunts what it eats with
// its legs (grasps[0]: the prey is held by the legs and dragged along), and
// drinks with its long tongue (drinkChunk): from a dead creature (its meat
// into foodStored), from a live one (its health), or, a big moth, feeding
// its grubs (FeedBaby: foodStored out). A big moth carries its grubs to safe
// places (SetDownBaby) and attacks what threatens them; a small moth stalks
// the slugcat it antagonizes. What it remembers of the slugcats (and other
// creatures) is a SocialMemory: hurting it or its kin makes it dislike them
// (and attack, then hunt them); a big moth throwing a creature away ignores
// it a while (thrownTimer).
//
// For the slugcat: a big moth uncomfortable with it keeps its distance; one
// that dislikes it grabs it with its legs and carries it up to drop it from
// high up (CliffScore), or off into its den. A small moth antagonizes it.

#include <cstdint>
#include <string>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "moth_types.hpp"
#include "world.hpp"

namespace moth {
    using creature::body;
    using creature::chunk;
    using creature::consious;

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos, variant v);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // BigMoth.Update (its legs', wings' and AI's with it)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // whether it collides with `t` at all (not what it holds)
    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t);
    // BigMoth.Collide: its chunk `i` touched `t`'s chunk `j`
    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j);
    // BigMoth.Violence (by chunk) + Creature.Violence, and the
    // SocialEventRecognizer's events for the moths that saw who did it
    void violence(sys::game_state& state, dcon::creature_id c, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus, creature::damage_type type);
    // it lets go of `t` (its legs' grasp, its tongue)
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // after Creature.Die (nothing of its own: it lets go)
    void died(sys::game_state& state, dcon::creature_id c);
    // Creature.SpitOutOfShortCut + BigMoth.NewRoom + BigMothAI.NewRoom: out
    // of a pipe in `room` at `at`
    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, rw::vec2 at);
    // a line for the log
    std::string describe(sys::game_state const& state, dcon::creature_id c);
    // where the tests' spawns may put one (--fuzz, --bench): open air over
    // a floor, with room to stand
    bool spawn_spot(world::room_view const& room, rw::ivec2 tile);

    // the variant of a moth
    variant variant_of(sys::game_state const& state, dcon::creature_id c) noexcept;
    // BigMoth.scale
    float scale_of(sys::game_state const& state, dcon::creature_id c) noexcept;
}
