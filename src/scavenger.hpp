#pragma once

// Scavengers (Scavenger.cs, a simpler ScavengerAI): three chunks, the chest
// (mainBodyChunk 0), the hips (1) and the head (2). It walks on its
// knuckles, crawls through corridors, climbs and swings along poles, drops
// down and swims, carries weapons in its two hands (vanilla's four grasps:
// grasps[0] the throwing hand and [1] the other one) and throws spears and
// rocks at what it fights.
//
// The AI is a cut down ScavengerAI: it idles about its room or wanders off
// to others (migrate.hpp), picks up the weapons it sees, flees what scares
// it (throwing at it on the way), and of the slugcat keeps a simple like:
// hostile once a slugcat hurt it or a scavenger of its room, neutral
// before. A hostile slugcat is threatened first (the throw charge without a
// throw), then attacked with thrown weapons. No trading, squads, tolls or
// kings. Its paths come from pathing.hpp's distance fields.

#include <cstdint>
#include <optional>
#include <string>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace scavenger {
    using creature::body;
    using creature::chunk;
    using creature::consious;

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // Scavenger.Update (its AI's with it)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // whether it collides with `t` at all
    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t);
    // Scavenger.Collide: its chunk `i` touched `t`'s chunk `j`
    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j);
    // Scavenger.Violence + Creature.Violence
    void violence(sys::game_state& state, dcon::creature_id c, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus, creature::damage_type type);
    // it lets go of `t`
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // Scavenger.Die, after Creature.Die
    void died(sys::game_state& state, dcon::creature_id c);
    // out of a pipe in `room` at `at`
    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, rw::vec2 at);
    // a line for the log
    std::string describe(sys::game_state const& state, dcon::creature_id c);
    // where the tests' spawns may put one (--fuzz, --bench): a floor tile
    bool spawn_spot(world::room_view const& room, rw::ivec2 tile);

    // What the graphics module reads of it
    // Scavenger.HeadLookDir / JoinedLookPoint (EyesLookPoint)
    rw::vec2 look_point(sys::game_state const& state, dcon::creature_id c);
    rw::vec2 head_look_dir(sys::game_state const& state, dcon::creature_id c);
    // ScavengerAI.ActNervous
    float act_nervous(sys::game_state const& state, dcon::creature_id c);
    // the item in grasp `i` (none)
    dcon::item_id held_item(sys::game_state const& state, dcon::creature_id c, uint32_t i);
    // SharedPhysics.ExactTerrainRayTracePos
    std::optional<rw::vec2> exact_terrain_ray_trace_pos(world::room_view const& room, rw::vec2 a, rw::vec2 b);
}
