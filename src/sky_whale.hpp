#pragma once

// Sky whale (the Watcher's SkyWhale.cs + SkyWhaleAI + SkyWhalePather): a
// huge harmless flyer of the open skies, six chunks (mainBodyChunk 1)
// swimming through the air head first with no gravity. What it means for
// the slugcat:
// - it is in collision layer 1 like the slugcat: its chunks (32 to 63 px
//   wide, 16.7 mass each against the slugcat's 0.7) shove the slugcat
//   aside, and one can ride on its back, pushed up off it every tick; its
//   Collide never hurts (CreatureParams.stunOnCollideVelocity is 0);
// - its six whiskers (SkyWhaleWhisker: a rope of segments hanging off the
//   snout, not the AltVariant's) are climbable vines (IClimbableVine through
//   rot::vines, like the pole plant's stalk): the slugcat grabs them and
//   rides along, climbing up and down;
// - it never attacks (its template "Eats" the slugcat but it has no
//   grasps, no prey tracker and no hunting behaviour);
// - it can be hurt: baseDamageResistance 50, baseStunResistance 8 (a spear
//   takes 1/50 of its health), and a stab lets out a jet of gas (GasJet);
//   dead or stunned it drifts (airFriction 0.98).
// SkyWhaleAI: it follows the room's SkyWhalePathfinding nodes (placed
// objects: a line from the object to its handle), each node in turn, the
// closest point of the line its goal, until it has been to all of them;
// without nodes it idles at spots IdleScore likes (close to the terrain;
// the AltVariant out under the open sky). It leaves for its den when the
// rain approaches (EscapeRain), and an idle one tires of the room. Its den
// is off screen: it leaves over the room's side and flies back in later,
// maybe into another room of its region that has nodes (the
// SkyWhaleAbstractAI's roaming, which a realized room never runs; every
// room is realized here). The world files spawn it OFFSCREEN
// (`spawn_offscreen`).

#include <cstdint>
#include <string>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "sky_whale_types.hpp"
#include "world.hpp"

namespace spawns {
    struct spawn;
    struct report;
}

namespace sky_whale {
    using creature::body;
    using creature::chunk;
    using creature::consious;

    // `alt`: the AltVariant (AbstractCreature.superSizeMe, the world file's
    // {AlternateForm}): 1.5 times the size, no whiskers
    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos, bool alt = false);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // SkyWhale.Update (its AI's with it), the whiskers and gas jets
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // whether it collides with `t` at all (not while off screen)
    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t);
    // SkyWhale.Collide: harmless (stunOnCollideVelocity 0)
    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j);
    // SkyWhale.Violence: a gas jet out of a stab wound + Creature.Violence
    void violence(sys::game_state& state, dcon::creature_id c, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus, creature::damage_type type);
    // it holds nothing
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // after Creature.Die: it drifts (nothing of its own)
    void died(sys::game_state& state, dcon::creature_id c);
    // SkyWhale.SpitOutOfShortCut: out of a pipe in `room` at `at`
    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, rw::vec2 at);
    // a line for the log
    std::string describe(sys::game_state const& state, dcon::creature_id c);
    // where the tests' spawns may put one (--fuzz, --bench): in the open, 6
    // tiles off the terrain, in a room with SkyWhalePathfinding nodes
    bool spawn_spot(world::room_view const& room, rw::ivec2 tile);

    // the world file's OFFSCREEN spawns (`0-SkyWhale-{AlternateForm}-2`): each
    // made off screen at a room of `region` with nodes, to fly in after
    // 100 to 1000 ticks (vanilla's den wait); counted in `r` like
    // spawns::spawn_in_den
    uint32_t spawn_offscreen(sys::game_state& state, dcon::region_id region, spawns::spawn const& s, spawns::report& r);
    // off screen: not out in its room (not drawn, nothing touches it)
    bool hidden(sys::game_state const& state, dcon::creature_id c) noexcept;

    // the payload's parameters and the data object's
    creature_params const& params(sys::game_state const& state, dcon::creature_id c) noexcept;
    bool alt_variant(sys::game_state const& state, dcon::creature_id c) noexcept;

    // The whiskers, IClimbableVine (rot::vines): `w` the whisker
    // (0..count-1). TotalPositions (its segments past the forbidden three),
    // Pos, Push, CurrentlyClimbable, BeingClimbedOn; Rad 0.1, Mass 0.2
    uint32_t whiskers(sys::game_state const& state, dcon::creature_id c) noexcept;
    bool vine_climbable(sys::game_state const& state, dcon::creature_id c, uint32_t w) noexcept;
    uint32_t vine_positions(sys::game_state const& state, dcon::creature_id c, uint32_t w) noexcept;
    rw::vec2 vine_pos(sys::game_state const& state, dcon::creature_id c, uint32_t w, uint32_t i) noexcept;
    void vine_push(sys::game_state& state, dcon::creature_id c, uint32_t w, uint32_t i, rw::vec2 movement);
    void being_climbed_on(sys::game_state& state, dcon::creature_id c, uint32_t w);
    inline constexpr float vine_rad = 0.1f;
    inline constexpr float vine_mass = 0.2f;
}
