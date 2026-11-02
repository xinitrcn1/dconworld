#pragma once

// Big spider (BigSpider.cs + BigSpiderAI), the spitter spider (the same
// classes with the SpitterSpider template) and Downpour's mother spider
// (MotherSpider): the payload's variant. Two chunks on eight legs, running
// along floors, poles, walls and ceilings.
//
// The big spider is an ambush hunter that has to get used to its prey
// first (SpiderTrackState.accustomed: the longer it watches it, the
// bolder it gets; until then it is afraid of it): then it charges a
// pounce (InitiateJump: the mandibles open, a warning rustle) and leaps at
// it, biting what it lands on (Collide: mandiblesCharged and canBite) and
// grabbing it in its jaws to drag it home (ReturnPrey, CarryObject).
// Grabbed itself, it bites back after a moment. Bumping into something
// big while it can't bite, it hops away. Its dead kin it tries to revive
// (ReviveBuddy: it holds the body and breathes into it until it gets up;
// the revived one dies again once its borrowed time runs out).
//
// The spitter keeps its distance: it finds a spot with a view of its prey
// (SpiderSpitModule.SpitPosScore), sits there and spits dart maggots
// (DartMaggot, dart_maggot.hpp: a sleeping venom that knocks the prey
// out after a while), four in a row before it has to regrow them; once
// the prey sleeps it comes for it. Its legs hold on to what touches it
// (grabChunks) and, held long enough, the prey ends up in its jaws.
//
// The mother spider (Downpour) is the big spider's bigger, slower
// sister: when it dies it bursts in a puff of spores and twenty five
// young spiders (coalescipedes, the `spider` kind) swarm out, out for
// blood.
//
// The AI is BigSpiderAI's utility comparison (threat, prey, rain) over
// tracker.hpp's modules, its paths pathing.hpp's distance fields.
// Nothing of the graphics module drives the simulation (vanilla's legs
// pull the body along their grips where a camera looks: here it moves
// like a spider no camera sees), and nothing depends on a camera: the
// light shyness (LightThreat) and the spitter's same screen rule aren't
// ported, the mother spawns its young wherever it dies.

#include <cstdint>
#include <string>

#include "big_spider_types.hpp"
#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace big_spider {
    using creature::body;
    using creature::chunk;
    using creature::consious;

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos, variant v);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // BigSpider.Update (its AI's with it)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // whether it collides with `t` at all (not what it holds)
    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t);
    // BigSpider.Collide with another kind (`t`'s chunk `j` met its chunk `i`)
    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j);
    // BigSpider.Collide with another big spider (any variant): kin
    // (CollideWithKin), reviving a dead one, climbing over
    void collide_kin(sys::game_state& state, dcon::creature_id c, uint32_t i, dcon::creature_id other, uint32_t j);
    // BigSpider.Violence (+ Creature.Violence)
    void violence(sys::game_state& state, dcon::creature_id c, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus, creature::damage_type type);
    // it lets go of `t` (jaws and legs)
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // BigSpider.Die, after Creature.Die: the mother's young (BabyPuff)
    void died(sys::game_state& state, dcon::creature_id c);
    // BigSpider.SpitOutOfShortCut: out of a pipe in `room` at `at`
    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, rw::vec2 at);
    // BigSpider.FlyingWeapon (Weapon.INotifyOfFlyingWeapons): a big
    // spider leaps out of the way of a weapon flying at it
    void flying_weapon(sys::game_state& state, dcon::creature_id c, rw::vec2 pos, rw::vec2 vel, rw::vec2 thrown_pos);
    // SpiderSpitModule.CreatureHitByDart: one of its darts stuck in `t`
    // (dart_maggot.cpp, only for a spitter in the dart's room)
    void creature_hit_by_dart(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // one of the mother's young (BabyPuff's Spider with bloodLust 1) at
    // `at` in `room`: outside a room's update (a deferred transaction)
    void spawn_young(sys::game_state& state, dcon::room_id room, rw::vec2 at);
    // a line for the log
    std::string describe(sys::game_state const& state, dcon::creature_id c);
    // where the tests' spawns may put one: standing on the ground
    bool spawn_spot(world::room_view const& room, rw::ivec2 tile);

    inline variant variant_of(sys::game_state const& state, dcon::creature_id c) noexcept {
        return state.world.creature_get_payload(c).big_spider.variant;
    }
    inline bool spitter(sys::game_state const& state, dcon::creature_id c) noexcept { return variant_of(state, c) == variant::spitter; }
    inline bool mother(sys::game_state const& state, dcon::creature_id c) noexcept { return variant_of(state, c) == variant::mother; }
    char const* to_string(variant v) noexcept;
    // BigSpider.Footing
    bool footing(sys::game_state const& state, dcon::creature_id c) noexcept;
    // what it holds in its jaws (grasps[0]), if any
    dcon::creature_id grasped(sys::game_state const& state, dcon::creature_id c) noexcept;
    // BigSpider.yellowCol (the variant's colour, rolled from the ID)
    rw::vec3 yellow_col(sys::game_state const& state, dcon::creature_id c) noexcept;
}
