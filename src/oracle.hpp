#pragma once

// The iterators (Oracle.cs): Five Pebbles in SS_AI, Looks to the Moon in
// SL_AI, and Downpour's: Spearmaster's Moon in DM_AI, Saint's Pebbles in
// CL_AI and Rivulet's rotted Pebbles in RM_AI. Only their bodies and how
// they move about their chambers on their own: no conversations, no
// projections, no mark, no neurons, nothing done to or for the slugcat
// (OracleBehavior.player is always null here).
//
// Vanilla's Oracle is a PhysicalObject, not a Creature; here it is a
// creature kind (`creature::type::oracle`) so the rooms' update, the
// collisions with the slugcats and creatures, the viewed rooms' graphics
// module and the renderer take it like the others. It has no den, no AI
// tracker, no template of its own (everyone ignores it), takes no damage
// (Violence only pushes it, like a PhysicalObject hit) and is never killed.
//
// Its body: two chunks (rad 6, mass 0.5) 9 apart, the head end first, with
// the arm (Oracle.OracleArm: a base running along the chamber's frame and
// four joints down to the lower chunk) in the data object. Per kind:
//  - SS, DM (SSOracleBehavior): float in their zero-g chamber, either
//    meditating at tile (24, 17) with their eyes shut, or idling: circling
//    the marbles (SetUpMarbles' hovering PebblesPearls) one after the
//    other; the arm's base slides along the frame towards the bottom
//    middle (BasePosScore with no slugcat);
//  - SL (SLOracleBehavior): sits at the bottom of her chamber holding her
//    knees, eyes shut (Hunter's and Sofanthiel's: no neurons, unconscious);
//    Saint's (IsMoonActive) floats instead, idling between random points,
//    meditating, and showing (nothing);
//  - CL (CLOracleBehavior), RM (SSOracleRotBehavior): sit, holding their
//    knees, crawl back to their spot when moved off it.

#include <cmath>
#include <cstdint>
#include <string>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "oracle_types.hpp"
#include "world.hpp"

namespace oracle {
    // Room.Loaded's iterator: makes the room's oracle if it is an oracle
    // chamber of the campaign (state.story_slugcat). Called once per room
    // when the world is populated
    dcon::creature_id spawn_room(sys::game_state& state, dcon::room_id room);
    // a new Oracle of `which` in `room` (its vanilla start position)
    dcon::creature_id create(sys::game_state& state, dcon::room_id room, id which);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // Oracle.Update (its behaviour's and its arm's)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // it collides with every creature (collisionLayer 1)
    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t);
    // PhysicalObject.Collide: nothing
    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j);
    // a hit only pushes the chunk (a PhysicalObject, no HealthState)
    void violence(sys::game_state& state, dcon::creature_id c, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus, creature::damage_type type);
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    void died(sys::game_state& state, dcon::creature_id c);
    // it never goes through pipes: put back where it is
    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, rw::vec2 at);
    std::string describe(sys::game_state const& state, dcon::creature_id c);
    // the room gravity the oracle wants for its chamber this tick, < 0 for
    // none (SSOracleBehavior: 1 - working, 1 when it is unconscious; Saint's
    // Moon: 0.2; RM's ZeroG effect: 1 - 0.76) - for the room gravity code
    float wanted_room_gravity(sys::game_state const& state, dcon::creature_id c);

    //
    // What the graphics read
    //
    dcon::oracle_data_id data(sys::game_state const& state, dcon::creature_id c) noexcept;
    id id_of(sys::game_state const& state, dcon::creature_id c) noexcept;
    body_state const& body_of(sys::game_state const& state, dcon::creature_id c) noexcept;
    arm_state const& arm_of(sys::game_state const& state, dcon::creature_id c) noexcept;
    // Oracle.Consious
    bool consious(sys::game_state const& state, dcon::creature_id c) noexcept;
    // OracleBehavior.OracleGetToPos, GetToDir, EyesClosed
    rw::vec2 get_to_pos(sys::game_state const& state, dcon::creature_id c) noexcept;
    rw::vec2 get_to_dir(sys::game_state const& state, dcon::creature_id c) noexcept;
    bool eyes_closed(sys::game_state const& state, dcon::creature_id c) noexcept;
    // SL/CL/RM: InSitPosition, Crawl, CrawlSpeed
    bool in_sit_position(sys::game_state const& state, dcon::creature_id c) noexcept;
    inline float crawl(body_state const& b) noexcept { return std::cos(b.crawl_counter * 3.1415927f * 2.f); }
    inline float crawl_speed(body_state const& b) noexcept { return 0.5f + 0.5f * std::sin(b.crawl_counter * 3.1415927f * 2.f); }
    // OracleArm.BaseDir, OnFramePos (BasePos), Joint.ElbowPos
    rw::vec2 base_dir(arm_state const& a, id which, float t) noexcept;
    rw::vec2 on_frame_pos(arm_state const& a, id which, float t) noexcept;
    rw::vec2 elbow_pos(arm_state const& a, uint32_t joint, float t, rw::vec2 tip) noexcept;
    // the chunks' positions (pos, last_pos)
    rw::vec2 chunk_pos(sys::game_state const& state, dcon::creature_id c, uint32_t i) noexcept;
    rw::vec2 chunk_last_pos(sys::game_state const& state, dcon::creature_id c, uint32_t i) noexcept;
}
