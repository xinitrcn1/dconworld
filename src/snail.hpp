#pragma once

// Snail (Snail.cs + SnailAI + SnailPather): two chunks, the body (mainBodyChunk)
// and the shell, sucked onto any surface (suckPoint) and crawling over
// floors, walls, ceilings, poles and under the water. Its danger is Click:
// a pop that stuns (Player.Stun, Creature.Stun, up to 60 ticks) and flings
// every body chunk in sight within 60 * size (and every item's), up to 5.5
// times as far and as hard when both are under the water, so a slugcat in
// the water may be knocked out and drown; leeches close to it die. It clicks
// when its clickCounter fills: it creeps up while it is scared (SnailAI.
// scared: Uncomfortable creatures close), on every touch of a creature
// (Collide), to 0.92 on a hit or a hard landing, by 0.8 when a slugcat
// jumps off it; and it pops at once on its next landing (or now and then
// under water) once `triggered`: hit by a weapon (Violence of 0.1 damage or
// a stun bonus over 10), landing at more than 20 of speed, or 40 ticks
// after a slugcat threw it (Player.ThrowObject: triggerTicker). A spear
// kills it before it can (instant death). After a click it is stunned 60
// ticks. SnailAI, simplified: it idles to better spots in sight
// (TileIdleScore), holds still while scared, goes home before the rain
// (DenFinder, dens.hpp).
//
// A slugcat picks it up in both hands (Player.Grabability TwoHands, legal
// to hold without a stun): the jetfish's HeavyCarry code (pickup candidate,
// the pull, following through pipes); a toss sets it off 40 ticks later.

#include <cstdint>
#include <string>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "snail_types.hpp"
#include "world.hpp"

namespace snail {
    using creature::body;
    using creature::chunk;
    using creature::consious;

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // Snail.Update (SnailAI.Update from Act, like vanilla)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // whether it collides with `t` at all (not what holds it)
    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t);
    // Snail.Collide: its chunk `i` touched `t`'s chunk `j` (a slugcat or
    // another kind)
    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j);
    // Snail.Collide with another snail, both ways (creature.cpp's collide_kin)
    void collide_kin(sys::game_state& state, dcon::creature_id c, uint32_t i, dcon::creature_id other, uint32_t j);
    // Snail.Violence (triggered) + Creature.Violence
    void violence(sys::game_state& state, dcon::creature_id c, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus, creature::damage_type type);
    // it holds nothing (grasps 0)
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // Snail.Die (Creature.Die): nothing of its own
    void died(sys::game_state& state, dcon::creature_id c);
    // Snail.SpitOutOfShortCut + SnailAI.NewRoom: out of a pipe in `room` at `at`
    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, rw::vec2 at);
    // a line for the log
    std::string describe(sys::game_state const& state, dcon::creature_id c);
    // where the tests' spawns may put one (--fuzz, --bench): a tile it can
    // crawl on (a surface or under the water)
    bool spawn_spot(world::room_view const& room, rw::ivec2 tile);

    // Snail.Click: the pop (it does nothing while triggerTicker runs)
    void click(sys::game_state& state, dcon::creature_id c, world::room_view const& room);

    // The rolls of its constructor (shell colours, mini clicks), for the graphics
    looks const& looks_of(sys::game_state const& state, dcon::creature_id c) noexcept;
    // SnailAI.CreatureUnease: how uneasy `t` makes it (0 for what it
    // ignores or a dead one)
    float creature_unease(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t);
    // Snail.outOfShell (1 while SnailAI.move)
    inline float out_of_shell(sys::game_state const& state, dcon::creature_id c) noexcept {
        return state.world.creature_get_payload(c).snail.move ? 1.f : 0.f;
    }

    // The player side: a slugcat holding a snail in both hands (Player
    // grasps[0], a HeavyCarry TwoHands creature)
    // the snail `p` holds, if any
    dcon::creature_id held_by(sys::game_state const& state, dcon::player_id p) noexcept;
    // Player.PickupCandidate over the snails (Grabability TwoHands, always):
    // the best one and its score, lower is better
    dcon::creature_id pickup_candidate(sys::game_state const& state, dcon::player_id p, world::room_view const& room, float& score);
    // Player.SlugcatGrab + Creature.Grab: held by `p` by its chunk 0
    void grab(sys::game_state& state, dcon::player_id p, dcon::creature_id c);
    // Creature.ReleaseGrasp from the player's side (laid down: no trigger)
    void release(sys::game_state& state, dcon::creature_id c);
    // Player.ThrowObject's end for a snail: let go, triggerTicker = 40
    void thrown(sys::game_state& state, dcon::creature_id c);
    // lets go of the snail `p` holds
    void release_player(sys::game_state& state, dcon::player_id p);
    // Player.GraphicsModuleUpdated's HeavyCarry pull: the held snail and the
    // slugcat kept 5 + its chunk's radius apart, by mass
    void carry_update(sys::game_state& state, dcon::player_id p);
    // the held snail travels through a shortcut with `p`: moved to `room`,
    // and (when it comes out) placed on the slugcat's main chunk
    void follow_player(sys::game_state& state, dcon::player_id p, dcon::room_id room, bool spit_out);
    // Player.JumpOnChunk on a snail's chunk: clickCounter + 0.8, and a pop
    // past 1 (true: past 1, the slugcat bounces 12/9 up and its stun is
    // capped at 20 by the caller, popped or not)
    bool jumped_on(sys::game_state& state, dcon::creature_id c);
}
