#pragma once

// Garbage worms (GarbageWorm.cs, GarbageWormAI.cs, GarbageWormState.cs):
// rooted in one of the room's garbage holes (Room.garbageHoles, the tiles
// marked 10), a long tentacle with an eyed head on its end that sways over
// the garbage. Two body chunks: 0 the head (pulled about by the AI's
// lookPoint and chargePos, linked to the tentacle's tip), 1 the root pinned
// at rootPos above the hole. It never leaves its room nor uses pipes.
//
// What it does to the slugcat (GarbageWormAI.Update): it watches the most
// interesting creature it tracks (CreatureInterest: big, close, fast moving
// ones), getting stressed by the dangerous ones (body size 1 and up) and by
// what moves fast close to it; stressed enough with that creature within
// 340 px of its root it draws back into its hole (Retract) and comes out
// again a while after things calmed down, in a free hole of the room
// (Extend + NewHole), as long as the rain is more than 60 ticks away. Out
// of its hole it snatches a spear a creature it sees holds (or the
// Hunter's spear on the back) when its head gets within 10 px of it,
// taking it from the holder's hand, and carries it until it draws back in,
// where the spear is lost (destroyed). Hit by anything but a bite (a thrown
// weapon in vanilla) it becomes angry at every slugcat of the room, an
// anger the other worms of the room pick up from it now and then; at a
// slugcat it is angry at (with no spear to take) it lashes out, grabs it by
// a chunk and drags it down at the floor (shaking it about in rooms
// without water) until it lets go (now and then, sooner for a dead one).
// Any hit stuns it and makes it let go; it can't be hurt or killed by
// damage (a CreatureState, no instantDeathDamageLimit), and can't be hit at
// all while drawn in (canBeHitByWeapons).

#include <cstdint>
#include <string>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace garbage_worm {
    using creature::body;
    using creature::chunk;
    using creature::consious;

    // a worm in `room`, in a free garbage hole of it (pos only matters for
    // a room without one)
    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // GarbageWorm.Update (its AI's with it)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // whether it collides with `t` at all (not drawn in, nor with what it
    // holds)
    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t);
    // GarbageWorm has no Collide of its own
    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j);
    // GarbageWorm.Violence (angry at the room's slugcats), Stun (it lets
    // go) + Creature.Violence
    void violence(sys::game_state& state, dcon::creature_id c, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus, creature::damage_type type);
    // it lets go of `t` (only the generic grasp: nothing of its own)
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // GarbageWorm.Die, after Creature.Die (nothing of its own)
    void died(sys::game_state& state, dcon::creature_id c);
    // GarbageWorm.NewRoom: a free hole of `room`, the tentacle stretched up
    // out of it
    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, rw::vec2 at);
    // a line for the log
    std::string describe(sys::game_state const& state, dcon::creature_id c);
    // where the tests' spawns may put one (--fuzz, --bench): a garbage hole
    bool spawn_spot(world::room_view const& room, rw::ivec2 tile);
    // canBeHitByWeapons: out of its hole at all (creature::trace skips it
    // otherwise)
    bool can_be_hit(sys::game_state const& state, dcon::creature_id c) noexcept;
    // GarbageWormAI.showAsAngry, stress, attackCounter (the graphics)
    bool show_as_angry(sys::game_state const& state, dcon::creature_id c) noexcept;
    float stress(sys::game_state const& state, dcon::creature_id c) noexcept;
    int32_t attack_counter(sys::game_state const& state, dcon::creature_id c) noexcept;
    // the number of garbage holes of a room (Room.garbageHoles.Length)
    uint32_t hole_count(world::room_view const& room) noexcept;
}
