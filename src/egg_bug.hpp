#pragma once

// Egg bug (EggBug.cs + EggBugAI) and Downpour's fire bug (the same class
// with the FireBug template: the payload's variant). Two chunks on four
// legs, scurrying about floors and poles (the fire bug walls and ceilings
// too), its eggs hanging off its back.
//
// The egg bug is prey: afraid of nearly everything (the slugcat included),
// it flees (ThreatTracker.FleeTo), leaps away from what comes close, from
// loud noises and from weapons flying at it (TryJump), and when it dies its
// six eggs (EggBugEgg: an edible item, one food pip each) spill out.
//
// The fire bug is the opposite: its eggs (FireEgg) hang on it until a
// blunt or stabbing hit (or any damage over 0.05) makes it drop them all in
// a blast (an Explosion of radius 200 that throws and stuns the slugcat);
// while it has eggs, hits don't hurt it at all. Without eggs it hunts what
// it eats (the slugcat first of all, PreyTracker): it grabs it in its jaws
// (LethalBehavior) and stabs it with the spines its eggs left (CarryObject:
// two light stabs, then half of them deadly), until it is dead and a while
// longer. Its eggs are thrown like grenades: a FireEgg tossed by a slugcat
// sticks to what it hits and goes off after 180 ticks in a killing blast.
// Dying, it drops six spears.
//
// The AI is EggBugAI's utility comparison (threat, rain, the fire bug's
// prey) over tracker.hpp's modules, its paths pathing.hpp's distance
// fields (no shortcuts inside a room: dens.hpp takes it home). Nothing of
// the graphics module drives the simulation (vanilla's legs pull the body
// along their grips where a camera looks: here it moves like an egg bug no
// camera sees).

#include <cstdint>
#include <string>

#include "creature.hpp"
#include "egg_bug_types.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace egg_bug {
    using creature::body;
    using creature::chunk;
    using creature::consious;

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos, variant v);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // EggBug.Update (its AI's with it)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // whether it collides with `t` at all (not what it holds)
    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t);
    // EggBug.Collide with another kind (nothing of its own)
    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j);
    // EggBug.Collide with one of its kind (`other` is an egg bug too):
    // the upper one climbs over; one of the same variant
    // (EggBugAI.CollideWithKin) follows the dominant one's way
    void collide_kin(sys::game_state& state, dcon::creature_id c, uint32_t i, dcon::creature_id other, uint32_t j);
    void touched_kin(sys::game_state& state, dcon::creature_id c, dcon::creature_id other);
    // EggBug.Violence (+ Creature.Violence): the eggs squirt, the fire bug
    // drops its eggs in a blast and shrugs the hit off while it has them
    void violence(sys::game_state& state, dcon::creature_id c, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus, creature::damage_type type);
    // it lets go of `t`
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // EggBug.Die, after Creature.Die: the eggs (and the fire bug's spears)
    // drop
    void died(sys::game_state& state, dcon::creature_id c);
    // EggBug.SpitOutOfShortCut: out of a pipe in `room` at `at`
    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, rw::vec2 at);
    // EggBug.FlyingWeapon (Weapon.INotifyOfFlyingWeapons, no Remix: only a
    // weapon flying rightwards): it leaps away, or squirts
    void flying_weapon(sys::game_state& state, dcon::creature_id c, rw::vec2 pos, rw::vec2 vel);
    // a line for the log
    std::string describe(sys::game_state const& state, dcon::creature_id c);
    // where the tests' spawns may put one (--fuzz, --bench): standing on
    // the ground
    bool spawn_spot(world::room_view const& room, rw::ivec2 tile);

    // EggBug.FireBug
    inline bool fire(sys::game_state const& state, dcon::creature_id c) noexcept {
        return state.world.creature_get_payload(c).egg_bug.variant == variant::fire_bug;
    }
    // EggBug.Footing
    bool footing(sys::game_state const& state, dcon::creature_id c) noexcept;
    // the template's movement (pathing.hpp)
    pathing::movement const& movement(sys::game_state const& state, dcon::creature_id c) noexcept;
    // what it holds in its jaws (grasps[0]: a fire bug's prey), if any
    dcon::creature_id grasped(sys::game_state const& state, dcon::creature_id c) noexcept;

    // Explosion, the way vanilla's runs over its lifeTime frames (all of
    // them in this tick): every chunk in reach pushed away, every creature
    // hurt and stunned by the closest of its chunks (a slugcat dies of a
    // full point of damage in one frame), the source's body skipped. What
    // the fire bug's egg drop and the FireEgg go off with
    struct explosion {
        int32_t life_time = 5;
        float rad = 200.f;
        float force = 10.f;
        float damage = 0.25f;
        float stun = 60.f;
        float min_stun = 0.f;
        // the light's colour (cosmetic)
        rw::vec3 color{1.f, 1.f, 1.f};
        // killTagHolder and killTagHolderDmgFactor: the one it is blamed
        // on is hurt the less
        dcon::creature_id kill_tag;
        float kill_tag_factor = 1.f;
    };
    void explode(sys::game_state& state, world::room_view const& room, rw::vec2 at, explosion const& e, dcon::physical_object_id source);
}
