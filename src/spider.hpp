#pragma once

// Coalescipedes (Spider.cs): the little black spiders that live in dens of
// 5 to 30 (world file name "Spider", `big_spider` is the BigSpider's port).
// One small chunk each (radius 2..9 and mass 0.035..0.15 by its
// iVars.size), crawling on every surface (Floor, Corridor, Climb, Wall,
// Ceiling: pathing::climber) away from the light: it idles in the dark,
// and in the light (lightExp, the room's placed LightSources) follows
// random paths (CreateRandomPath) towards the darkest end (TileScore).
//
// The room's spiders are one Flock (vanilla's flocks merge into one per
// room within a few ticks: here the flock is simply the room's live
// spiders). When they see prey (Eats, at most 3.3 in mass) their bloodLust
// rises and spreads to the spiders around them; lusting spiders link up
// into a Centipede (`chain`): a line of up to 12..21 spiders, the biggest
// in front, the others crawling after the one ahead of them (Assist,
// Tighten), the head hunting the prey. A chain that outweighs its prey
// latches on to it (NonExclusive grasps); once the spiders holding a
// creature weigh as much as it does, it dies.
//
// A chain is kept by one of its spiders (spider_types.hpp chain_state in
// its spider_data); its members point at it and are ordered by their
// chain_order. Chains never span rooms. The room's spiders are walked in id
// order (rooms' index buckets), so everything is the same on any thread
// count; a room's spiders only ever touch the room's creatures.

#include <cstdint>
#include <string>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "spider_types.hpp"
#include "world.hpp"

namespace spider {
    using creature::body;
    using creature::chunk;
    using creature::consious;

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // Spider.Update (its Flock's and Centipede's with it)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // the spiders' collision layer (0): spiders push each other apart (the
    // pairs of `c` with the room's spiders after it), nothing else
    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // Creature.Violence (no HealthState) + Spider.Stun
    void violence(sys::game_state& state, dcon::creature_id c, rw::vec2 momentum, float damage, float stun_bonus);
    // lets go of `t`, forgets it as prey
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // Spider.Die, after Creature.Die
    void died(sys::game_state& state, dcon::creature_id c);
    // Spider.SpitOutOfShortCut + NewRoom: out of a pipe in `room` at `at`
    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, rw::vec2 at);
    // a line for the log
    std::string describe(sys::game_state const& state, dcon::creature_id c);
    // where the tests' spawns may put one: a tile it can crawl on
    bool spawn_spot(world::room_view const& room, rw::ivec2 tile);

    // What the graphics read of the spider
    // Spider.graphicsAttachedToBodyChunk: the chunk it holds on to (the
    // spider ahead of it in its chain, or its prey), and that spider
    struct attachment {
        dcon::body_chunk_id chunk;
        dcon::creature_id spider;
    };
    attachment attached_chunk(sys::game_state const& state, dcon::creature_id c);
    // its chain as SpiderGraphics sees it
    struct chain_view {
        bool in_chain = false;
        bool head = false;
        uint32_t count = 0;
        bool has_prey = false;
        rw::vec2 prey_pos{0.f, 0.f};
        float hunt = 0.f;
    };
    chain_view chain_of(sys::game_state const& state, dcon::creature_id c);
    // its chain's size (0 none)
    uint32_t chain_size(sys::game_state const& state, dcon::creature_id c);
    // iVars.size
    inline float size_of(sys::game_state const& state, dcon::creature_id c) noexcept { return state.world.creature_get_payload(c).spider.size; }
}
