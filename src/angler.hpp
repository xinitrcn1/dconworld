#pragma once

// The Watcher's angler (Angler.cs + AnglerAI.cs): a big deep water fish of
// three chunks (head, middle, tail) pushed along by its propeller tail,
// with a glowing lure on a jointed arm over its head (AnglerGraphics'
// lightPos: moved into the simulation here, since the AI sees from it).
//
// What it means for the slugcat (and the scavengers, jetfish and lizards it
// antagonizes): one that the lure's light falls on (in visual contact of
// the lure, within 170 px of it, or 80 px unseen) becomes its locked
// creature, and it stalks it, keeping about 80 px away; one that swims off
// from the lure (or hides: Watcher camouflage) for 44 ticks angers it
// (angerTimer 200 to 800: the lure turns red): it hunts, jaws wide open
// (jawState 1, 2), and the jaws slam shut on the first thing they touch
// side on (Collide): whatever small enough is in front of the head
// (JawsSlamShut: its chunks' radii under 30, a slugcat among them) is
// killed and gone. Bumping into it, or hurting it, angers it too. A stab
// into the open mouth does double damage. It flees what it fears, goes home
// before the rain (dens.hpp) and travels the rooms' pipes on trips
// (migrate.hpp). docs/DIVERGENCES.md lists what's left out (sounds, the
// StuckTracker, AImap.IsTooCloseToTerrain).

#include <cstdint>
#include <string>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace angler {
    using creature::body;
    using creature::chunk;
    using creature::consious;

    // CreatureTemplate.meatPoints (no meat eating is ported)
    inline constexpr int32_t meat_points = 4;

    // its data object, from the payload
    inline dcon::angler_data_id data_of(dcon::data_container const& w, dcon::creature_id c) noexcept {
        return dcon::angler_data_id{dcon::angler_data_id::value_base_t(w.creature_get_payload(c).angler.data)};
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // Angler.Update (its AI's with it, and the lure's)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // whether it collides with `t` at all
    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t);
    // Angler.Collide: its chunk `i` touched `t`'s chunk `j`
    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j);
    // Angler.Violence + Creature.Violence
    void violence(sys::game_state& state, dcon::creature_id c, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus, creature::damage_type type);
    // it holds nothing: only forgets `t` as its locked creature
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // Angler.Die, after Creature.Die
    void died(sys::game_state& state, dcon::creature_id c);
    // Angler.SpitOutOfShortCut: out of a pipe in `room` at `at`
    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, rw::vec2 at);
    // a line for the log
    std::string describe(sys::game_state const& state, dcon::creature_id c);
    // where the tests' spawns may put one (--fuzz, --bench): under water
    bool spawn_spot(world::room_view const& room, rw::ivec2 tile);

    // for the graphics: Angler.GlowPos between ticks (the lure)
    rw::vec2 glow_pos(sys::game_state const& state, dcon::creature_id c, float alpha) noexcept;
}
