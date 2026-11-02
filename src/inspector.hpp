#pragma once

// Inspector (Downpour's Inspector.cs + InspectorAI.cs): an iterator's
// floating watcher. One 2.15 px chunk (mass 2.6) flying on its own
// flyingPower (its gravity cancelled while it is conscious), following its
// path (daddy-like pathing: open air and corridors cheap, the rest dear),
// with three long heads: Tentacles of 8 TentacleChunks (300 px, ropes
// around the terrain, as tentacle_plant.cpp's port), each ending in a head
// with jaws and an eye. One eye at a time is the active one: what the
// inspector sees is what that eye faces (InspectorAI.VisualScore).
//
// The heads idle about goals spread over the room (AllHeadGoals), get
// curious about what touches it or comes close, and fight: what hurts it
// becomes its prey (Eats), a head grabs it, shakes it and flings it away;
// a head reaching for a creature takes a nearer free weapon instead and
// throws it at the prey. Hitting a head costs it health (3 to start with);
// a head at 0 hangs limp. Its body takes no damage from weapons or bites
// (one in a thousand kills it outright). The DaddyLongLegs (and other
// inspectors) it Attacks: it grows angry and red. When the rain comes it
// heads for its den; dead, it shakes apart for six ticks and is gone.
//
// See inspector_graphics.hpp for the drawing, docs/DIVERGENCES.md for what
// is left out (no zap coils, corruption or coral, no OverseerCarcass eyes,
// no Safari control).

#include <cstdint>
#include <string>

#include "creature.hpp"
#include "gamestate.hpp"
#include "inspector_types.hpp"
#include "math.hpp"
#include "world.hpp"

namespace inspector {
    using creature::body;
    using creature::chunk;

    // its data object, and a head's (creature_part `i`'s)
    inline dcon::inspector_data_id data_of(dcon::data_container const& w, dcon::creature_id c) noexcept {
        return dcon::inspector_data_id{dcon::inspector_data_id::value_base_t(w.creature_get_payload(c).inspector.data)};
    }
    inline dcon::inspector_head_id head_data(dcon::data_container const& w, dcon::creature_part_id p) noexcept {
        return dcon::inspector_head_id{dcon::inspector_head_id::value_base_t(w.creature_part_get_payload(p).inspector_head.data)};
    }
    dcon::inspector_head_id head_of(sys::game_state const& state, dcon::creature_id c, uint32_t i) noexcept;

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // Inspector.Update (its AI's with it)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // collision layer 1 (the creatures'), but not what a head holds
    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t);
    // Inspector.Collide: it sees what touches it, the active eye gets curious
    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j);
    // Inspector.Violence (a head hit, or the body: no damage but by a
    // one in a thousand chance) + Creature.Violence
    void violence(sys::game_state& state, dcon::creature_id c, creature::hit_part const& part, rw::vec2 momentum, float damage, float stun_bonus,
        creature::damage_type type);
    // its heads let go of `t` and stop looking at it
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // Creature.Die: the dying shake runs in update
    void died(sys::game_state& state, dcon::creature_id c);
    // Inspector.SpitOutOfShortCut + NewRoom: out of a pipe in `room` at `at`
    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, rw::vec2 at);
    // a line for the log
    std::string describe(sys::game_state const& state, dcon::creature_id c);
    // where the tests' spawns may put one (--fuzz, --bench): open air
    bool spawn_spot(world::room_view const& room, rw::ivec2 tile);

    // IHaveAppendages: the heads (AppendagePosition: the body, then
    // tChunks[1..7]) hit by a projectile from `from` to `to`, the closest
    // along it (`at` its fraction), as a hit_part (tentacle: the head)
    bool trace_appendages(sys::game_state const& state, dcon::room_id room, rw::vec2 from, rw::vec2 to, dcon::creature_id thrown_by, dcon::creature_id& hit,
        creature::hit_part& part, float& at);

    // What the graphics module reads
    // Inspector.bodyColor (`roll` in [0, 1]: the flicker of a stunned or
    // held one, a draw time Random.value)
    rw::vec3 body_color(sys::game_state const& state, dcon::creature_id c, float roll, float* alpha = nullptr) noexcept;
    // Inspector.TrueColor
    rw::vec3 true_color(sys::game_state const& state, dcon::creature_id c) noexcept;
    // Inspector.Rad (a head's thickness along it)
    float rad(float f) noexcept;
    // the light's glow mix (Update's num4) and the attention flicker (num)
    float glow_mix(sys::game_state const& state, dcon::creature_id c) noexcept;
    float attention_flicker(sys::game_state const& state, dcon::creature_id c) noexcept;
    // a head holds or goes for a weapon (Inspector.HeadWeaponized)
    bool head_weaponized(sys::game_state const& state, dcon::creature_id c, uint32_t i) noexcept;
}
