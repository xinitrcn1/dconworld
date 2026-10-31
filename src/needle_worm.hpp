#pragma once

// Noodleflies (NeedleWorm.cs: BigNeedleWorm, the adult, and SmallNeedleWorm,
// its young; BigNeedleWormAI, SmallNeedleWormAI). A long flying worm: a few
// body chunks pushed apart by every pair, then a tail of loose segments, all
// held together by ropes wrapping around the terrain and stiffened along
// their length. It flies by its wings' thrust along its look direction and
// a pull towards the next steps of its path, crawls through narrow spaces.
//
// The adult wanders the open air of the room, keeps away from what comes
// too close, and fights: other adults (Attacks 0.9 in the static
// relationships), and whatever hurt it or its young, or carries one of its
// young. It gets into position (attackFromPos), winds up its charge
// (attackReady, chargingAttack) and swishes forward along its fang, which
// sticks in a wall it hits head on (or bounces off it) or impales the
// creature it hits (a slugcat dies of it). It dodges thrown weapons and a
// rival's swish.
//
// The young flits about its mother (the closest adult of its room), hangs
// on her tail now and then, flees what comes close, and dies screaming
// when something catches it: the mother then comes after the closest
// creature. A slugcat can carry and eat one (food_points, five bites).

#include <cstdint>
#include <string>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace needle_worm {
    using creature::body;
    using creature::chunk;
    using creature::consious;
    char const* to_string(variant v) noexcept;

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos, variant v);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // BigNeedleWorm / SmallNeedleWorm (NeedleWorm.cs).Update (its AI's with it)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // whether it collides with `t` at all
    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t);
    // BigNeedleWorm / SmallNeedleWorm (NeedleWorm.cs).Collide: its chunk `i` touched `t`'s chunk `j`
    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j);
    // BigNeedleWorm / SmallNeedleWorm (NeedleWorm.cs).Violence + Creature.Violence
    void violence(sys::game_state& state, dcon::creature_id c, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus, creature::damage_type type);
    // it lets go of `t`
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // BigNeedleWorm / SmallNeedleWorm (NeedleWorm.cs).Die, after Creature.Die
    void died(sys::game_state& state, dcon::creature_id c);
    // out of a pipe in `room` at `at`
    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, rw::vec2 at);
    // a line for the log
    std::string describe(sys::game_state const& state, dcon::creature_id c);
    // where the tests' spawns may put one (--fuzz, --bench)
    bool spawn_spot(world::room_view const& room, rw::ivec2 tile);
    // BigNeedleWorm.FlyingWeapon (Weapon.INotifyOfFlyingWeapons): a thrown
    // weapon at `pos` moving at `vel` (it dodges)
    void flying_weapon(sys::game_state& state, dcon::creature_id c, rw::vec2 pos, rw::vec2 vel);

    //
    // The body as the graphics module reads it (NeedleWorm's segment helpers)
    //
    inline bool is_small(sys::game_state const& state, dcon::creature_id c) noexcept {
        return state.world.creature_get_payload(c).needle_worm.variant == variant::small;
    }
    inline uint32_t chunk_count(bool small) noexcept { return small ? 3u : 5u; }
    inline uint32_t tail_count(bool small) noexcept { return small ? 4u : 10u; }
    // TotalSegments
    inline uint32_t total_segments(bool small) noexcept { return chunk_count(small) + tail_count(small); }
    // GetSegmentPos(seg, timeStacker), GetSegmentDir, GetSegmentRadForCollision
    rw::vec2 segment_pos(sys::game_state const& state, dcon::creature_id c, uint32_t seg, float alpha) noexcept;
    rw::vec2 segment_dir(sys::game_state const& state, dcon::creature_id c, uint32_t seg, float alpha) noexcept;
    float segment_rad_for_collision(sys::game_state const& state, dcon::creature_id c, uint32_t seg) noexcept;
    // OnBodyPos, OnBodyDir, OnBodyRad
    rw::vec2 on_body_pos(sys::game_state const& state, dcon::creature_id c, float f, float alpha) noexcept;
    rw::vec2 on_body_dir(sys::game_state const& state, dcon::creature_id c, float f, float alpha) noexcept;
    float on_body_rad(sys::game_state const& state, dcon::creature_id c, float f) noexcept;
    // SmallNeedleWorm.HangingOnMother: the mother whose tail it hangs on
    // (its segment in `seg`), none
    dcon::creature_id hanging_on_mother(sys::game_state const& state, dcon::creature_id c, uint32_t* seg = nullptr) noexcept;

    //
    // The slugcat side (IPlayerEdible): a young carried in a hand and eaten.
    // (items.cpp's grab and eat loop and shortcuts.cpp's carry through pipes
    // call these the way they call centipede.hpp's baby centipede ones)
    //
    // the young in `p`'s hand `hand`, in either hand
    dcon::creature_id held_in_hand(sys::game_state const& state, dcon::player_id p, uint32_t hand) noexcept;
    dcon::creature_id held_by(sys::game_state const& state, dcon::player_id p) noexcept;
    // Player.PickupCandidate over the young (Grabability OneHand): the best
    // one and its score, lower is better
    dcon::creature_id pickup_candidate(sys::game_state const& state, dcon::player_id p, world::room_view const& room, float& score);
    // Player.SlugcatGrab: into the hand
    void grab(sys::game_state& state, dcon::player_id p, dcon::creature_id c, uint32_t hand);
    // Creature.ReleaseGrasp from the slugcat's side
    void release(sys::game_state& state, dcon::creature_id c);
    void release_player(sys::game_state& state, dcon::player_id p);
    // the young held at the hand's position
    void carry_update(sys::game_state& state, dcon::player_id p, uint32_t hand, rw::vec2 hand_pos);
    // the held young travels a shortcut with `p`
    void follow_player(sys::game_state& state, dcon::player_id p, dcon::room_id room, bool spit_out);
    // SmallNeedleWorm.BitByPlayer: a bite (it screams and dies); true when
    // it is all eaten (ObjectEaten: the caller destroys it)
    bool bit_by_player(sys::game_state& state, dcon::creature_id c, dcon::player_id p);
}
