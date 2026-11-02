#pragma once

// Batflies (Fly.cs + FlyAI.cs, with FliesRoomAI.cs and FliesWorldAI.cs):
// the little one chunk fliers that swarm the swarm rooms, burrow into the
// hive grass when frightened (or when the rain comes) and hang from poles
// and from each other in chains. Many and small: one chunk (rad 6, mass
// 0.05), the payload (batfly_types.hpp's body_state) and a data object
// (`batfly_data`) for the AI's goals and threats and the graphics module.
//
// Flight (Fly.Act): BatFlight towards the AI's localGoal (flapping, wall
// avoidance), SwarmFlight (hovering in the swarm, the flock's sine), Panic
// (straight away from a threat), Burrow (sinking into the hive grass),
// Hang (from a pole or another fly), WaterBehavior (struggling to the
// surface; drowns after a while under). The AI (FlyAI.Update) picks one of
// Idle, Swarm, Flee, Drop, Burrow, Chain, FleeFromRain, its goal moving
// along a distance field (pathing.hpp) to the room's nodes (its exits and
// hives) like vanilla's Dijkstra maps; flies keep away from what they fear
// (TrackedThreat: anything the relationship table says they're Afraid of,
// noticed one random creature a tick, passed on from fly to fly) and copy
// the flock of a more dominant fly they see (FlockBehavior).
//
// The hives (Room.hives: the hive grass tiles, grouped like AddHiveTile)
// are `bat_hive` objects made on a room's first visit (`spawn_room`). A
// fly in the hive is out of the room (`away` 2, like a den, its `in_hive`
// set) and the room's pass (FliesRoomAI.Update, `update_room`) lets it out
// at the best hive (2.5% a tick, not while the rain approaches). The world
// pass (FliesWorldAI.Update, `update_world`, serial) moves flies between
// rooms towards the swarm rooms (Spread, Gather, Migrate by the swarm
// rooms' behaviours) and keeps the swarm rooms stocked.
//
// For the slugcat: caught on touch (Player.Update's auto grab, `PlayerAuto
// Grabable`) or picked up into a free hand (OneHand), it struggles, dies
// now and then in the hand, and is eaten in three bites for one pip
// (Fly.FoodPoints 1; `nourishment` for SlugcatStats.NourishmentOfObjectEaten).

#include <cstdint>
#include <string>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "player_types.hpp"
#include "world.hpp"

namespace batfly {
    using creature::body;
    using creature::chunk;
    using creature::consious;

    // IPlayerEdible.FoodPoints
    inline constexpr int32_t food_points = 1;
    // IPlayerEdible.BitesLeft at the start
    inline constexpr uint8_t start_bites = 3;

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // Fly.Update (its AI's with it)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // whether it collides with `t`: never (Fly is in collision layer 0)
    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t);
    // Fly.Collide: nothing of its own
    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j);
    // Creature.Violence (no HealthState: stunned, dead from a hit over
    // instantDeathDamageLimit)
    void violence(sys::game_state& state, dcon::creature_id c, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus, creature::damage_type type);
    // lets go of `t` if its grasp holds it (a chain)
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // Fly.Die, after Creature.Die: the surface friction drops
    void died(sys::game_state& state, dcon::creature_id c);
    // Fly.SpitOutOfShortCut + NewRoom (out of a pipe at `at`), or
    // PlaceInRoom + NewRoom (out of the hive grass at `at`)
    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, rw::vec2 at);
    // a line for the log
    std::string describe(sys::game_state const& state, dcon::creature_id c);
    // where the tests' spawns may put one (--fuzz, --bench): open air
    bool spawn_spot(world::room_view const& room, rw::ivec2 tile);

    //
    // The rooms and the world (FliesRoomAI, FliesWorldAI)
    //
    // Room.hives: the room's hive grass tiles, grouped (a room's first
    // visit; once)
    void spawn_room(sys::game_state& state, dcon::room_id room);
    // FliesWorldAI's constructor: every swarm room gets its region's
    // batsPerActiveSwarmRoom in its hives (out in the room without one);
    // builds the room routes towards the swarm rooms (serial). Returns the
    // flies made
    uint32_t spawn_world(sys::game_state& state);
    // FliesRoomAI.Update: the room's flies in the hives come out (the
    // room's update, in parallel)
    void update_room(sys::game_state& state, world::room_view const& room);
    // FliesWorldAI.Update: a fly of a random room of each region sent
    // towards the swarm rooms, the swarm rooms' behaviours rolled, the
    // swarm rooms restocked (serial, before or after the rooms update)
    void update_world(sys::game_state& state);
    // FliesWorldAI.ActiveSwarmRoom (no RegionState: every swarm room is)
    bool active_swarm_room(sys::game_state const& state, dcon::room_id room) noexcept;

    //
    // The slugcat side (IPlayerEdible, Player.Grabability OneHand).
    // items.cpp's grab and eat loop, player_graphics' hands and
    // shortcuts.cpp's pipes call these the way they call barnacle.hpp's
    //
    // the fly in `p`'s hand `hand`, in either hand
    dcon::creature_id held_in_hand(sys::game_state const& state, dcon::player_id p, uint32_t hand) noexcept;
    dcon::creature_id held_by(sys::game_state const& state, dcon::player_id p) noexcept;
    // Player.Update's batfly loop: the first fly of the room close enough
    // to be caught on touch (PlayerAutoGrabable, within 20 px, 10 in a
    // narrow space, halved outside an active swarm room, held by nothing).
    // The caller checks the rest (a free hand, dontGrabStuff, the stomach
    // not full, not the Spearmaster: AllowGrabbingBatflys)
    dcon::creature_id auto_grab_candidate(sys::game_state const& state, dcon::player_id p, world::room_view const& room);
    // Player.PickupCandidate over the flies (Grabability OneHand, CantGrab
    // while its shortcutDelay runs): the best one and its score, lower is
    // better
    dcon::creature_id pickup_candidate(sys::game_state const& state, dcon::player_id p, world::room_view const& room, float& score);
    // Player.Collide's catch (Fly_Caught, Fly_Caught_Dead) + SlugcatGrab:
    // into the hand
    void grab(sys::game_state& state, dcon::player_id p, dcon::creature_id c, uint32_t hand);
    // Creature.ReleaseGrasp from the slugcat's side
    void release(sys::game_state& state, dcon::creature_id c);
    void release_player(sys::game_state& state, dcon::player_id p);
    // the held fly at the hand's position
    void carry_update(sys::game_state& state, dcon::player_id p, uint32_t hand, rw::vec2 hand_pos);
    // the held fly travels a shortcut with `p`
    void follow_player(sys::game_state& state, dcon::player_id p, dcon::room_id room, bool spit_out);
    // Fly.BitByPlayer: a bite (it dies of the first, Slugcat_Bite_Fly /
    // Slugcat_Final_Bite_Fly); true when it is all eaten: the caller lets
    // go of it and feeds the slugcat (Player.ObjectEaten); the fly destroys
    // itself three ticks later (Fly.eaten)
    bool bit_by_player(sys::game_state& state, dcon::creature_id c, dcon::player_id p);
    // SlugcatStats.NourishmentOfObjectEaten for a fly, in quarter pips:
    // 4 (one pip), 1 for the Hunter and the Artificer, 0 for the
    // Spearmaster, -1 for the Saint (who can't stomach it: stunned 60 ticks)
    int32_t nourishment(::player::slugcat_name name) noexcept;
}
