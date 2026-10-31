#pragma once

// Barnacles (the Watcher's Barnacle.cs + BarnacleAI + BarnacleGraphics): a
// shelled herd creature on six legs, two chunks (the shell, mainBodyChunk,
// and the soft underside), walking floors, walls and ceilings and swimming
// in the water. It never attacks. With its shell on it is invulnerable
// (Violence: no damage, no stun, sparks; spears bounce off) and a
// conscious creature bumping into it while it idles is shoved off and
// stunned for 30 ticks (Collide); every slugcat within 150 of it keeps
// full lungs (airInLungs = 1: it breathes for them under water). The herd
// follows a leader between narrow idle spots and the water; creatures
// lingering near it, bumps and hits raise the group's discomfort until the
// whole herd scatters with a jump (LeadingUpdate). An explosion (or a
// lizard's bite) blows the shell off (LoseShell: rocks and shell shards
// fly, it shrinks to an eighth of its mass): bare, it flees what it fears
// (a ThreatTracker), dies to a spear like anything else, and a slugcat
// can carry it in one hand and eat it (IPlayerEdible: five bites, one
// food pip; Player.Grabability OneHand, IsCreatureLegalToHoldWithoutStun).

#include <cstdint>
#include <string>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace barnacle {
    using creature::body;
    using creature::chunk;
    using creature::consious;

    // IPlayerEdible.FoodPoints
    inline constexpr int32_t food_points = 1;

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // Barnacle.Update (its AI's with it)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // whether it collides with `t` at all (not what holds it)
    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t);
    // Barnacle.Collide: its chunk `i` touched `t`'s chunk `j` (another kind)
    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j);
    // Barnacle.Collide of both with another barnacle (the dominant one
    // climbs over, no clink sound)
    void collide_kin(sys::game_state& state, dcon::creature_id c, uint32_t i, dcon::creature_id other, uint32_t j);
    // Barnacle.Violence + Creature.Violence (the shell takes it all)
    void violence(sys::game_state& state, dcon::creature_id c, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus, creature::damage_type type);
    // it lets go of `t` (its leader or follower gone, the eyes' pick)
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // Barnacle.Die, after Creature.Die: it leaves its leader
    void died(sys::game_state& state, dcon::creature_id c);
    // Barnacle.SpitOutOfShortCut: out of a pipe in `room` at `at`
    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, rw::vec2 at);
    // a line for the log
    std::string describe(sys::game_state const& state, dcon::creature_id c);
    // where the tests' spawns may put one (--fuzz, --bench): a tile it can
    // walk or swim
    bool spawn_spot(world::room_view const& room, rw::ivec2 tile);

    // Barnacle.hasShell, LoseShell (an explosion, a lizard's bite: the
    // caller is in the barnacle's room's update)
    bool has_shell(sys::game_state const& state, dcon::creature_id c) noexcept;
    void lose_shell(sys::game_state& state, dcon::creature_id c);

    //
    // The slugcat side (IPlayerEdible): a bare barnacle carried in a hand
    // and eaten. (items.cpp's grab and eat loop and shortcuts.cpp's carry
    // through pipes call these the way they call needle_worm.hpp's)
    //
    // the barnacle in `p`'s hand `hand`, in either hand
    dcon::creature_id held_in_hand(sys::game_state const& state, dcon::player_id p, uint32_t hand) noexcept;
    dcon::creature_id held_by(sys::game_state const& state, dcon::player_id p) noexcept;
    // Player.PickupCandidate over the bare ones (Grabability OneHand
    // without the shell, CantGrab with it): the best one and its score,
    // lower is better
    dcon::creature_id pickup_candidate(sys::game_state const& state, dcon::player_id p, world::room_view const& room, float& score);
    // Player.SlugcatGrab: into the hand
    void grab(sys::game_state& state, dcon::player_id p, dcon::creature_id c, uint32_t hand);
    // Creature.ReleaseGrasp from the slugcat's side
    void release(sys::game_state& state, dcon::creature_id c);
    void release_player(sys::game_state& state, dcon::player_id p);
    // the held barnacle at the hand's position
    void carry_update(sys::game_state& state, dcon::player_id p, uint32_t hand, rw::vec2 hand_pos);
    // the held barnacle travels a shortcut with `p`
    void follow_player(sys::game_state& state, dcon::player_id p, dcon::room_id room, bool spit_out);
    // Barnacle.BitByPlayer: a bite (it dies of the first); true when it is
    // all eaten (ObjectEaten: the caller destroys it)
    bool bit_by_player(sys::game_state& state, dcon::creature_id c, dcon::player_id p);
}
