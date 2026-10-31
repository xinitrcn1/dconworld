#pragma once

// Lantern mice (LanternMouse.cs + MouseAI + MouseState's battery). A small
// two chunk ground creature (the head, chunk 0, and the rear, chunk 1) that
// runs on floors, through corridors and up poles (StandardPather over the
// path fields), and spends its time hanging from a rope tied to a ceiling or
// a horizontal pole by its tail (AttatchRope, Hang, UpdateRope wrapping the
// rope around tile corners), dozing off (fallAsleep) and charging its glow.
//
// MouseAI, ported: it is afraid of most things (ThreatTracker: the slugcat
// at 0.3, lizards, spiders, dropwigs...; more so of what is above it and
// close to its dangle spot), flees (FleeTo, running at full speed, jittering,
// squeaking), lets go of its rope when panicking, goes home before the rain
// (RainTracker, dens.hpp), and when idle looks for a good dangle spot
// (DangleTile, DanglePosScore: away from more dominant mice), walks to the
// tile beside where the rope goes and ties itself up, or wanders (random
// reachable tiles, following a more dominant mouse it bumped into, now and
// then a neighbouring room: migrate.hpp). It never hunts nor fights back.
//
// For the slugcat: it is a TwoHands creature (Player.Grabability:
// LanternMouse, HeavyCarry; vanilla, not one handed) picked up like a jetfish
// or a squidcada, legal to hold without stunning it; it squeaks and
// struggles in the hands (Carried) while the slugcat stands still on
// walkable ground; thrown hard it is stunned for 20 ticks; it can't be
// eaten by the slugcat (not IPlayerEdible; the carnivores' meat eating, 2
// meat points, isn't ported). Its glow (MouseGraphics' LightSource) is the
// light the slugcat carries: lantern_mouse_graphics::light. A dead mouse's
// battery runs down and its light fades.

#include <cstdint>
#include <string>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace lantern_mouse {
    using creature::body;
    using creature::chunk;
    using creature::consious;

    // MouseState's battery when new (it runs down only once dead)
    inline constexpr int32_t full_battery = 4000;
    // CreatureTemplate.meatPoints (for a meat eating port)
    inline constexpr int32_t meat_points = 2;

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // LanternMouse.Update (its AI's with it)
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // whether it collides with `t` at all (not what holds it)
    bool collides(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t);
    // LanternMouse.Collide: its chunk `i` touched `t`'s chunk `j` (it wakes up)
    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, dcon::creature_id t, uint32_t j);
    // two mice touched (LanternMouse.Collide's mouse part + MouseAI.CollideWithMouse)
    void collide_kin(sys::game_state& state, dcon::creature_id c, uint32_t i, dcon::creature_id other, uint32_t j);
    // LanternMouse.Violence + Creature.Violence: it squeaks
    void violence(sys::game_state& state, dcon::creature_id c, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus, creature::damage_type type);
    // it lets go of `t` (it holds nothing: nothing to do)
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // LanternMouse.Die (Creature.Die): the rope stays (a dead mouse keeps
    // hanging from it), the battery starts running down
    void died(sys::game_state& state, dcon::creature_id c);
    // LanternMouse.SpitOutOfShortCut: out of a pipe in `room` at `at`
    // (MouseAI.NewRoom: the dangle checks start over)
    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, rw::vec2 at);
    // a line for the log
    std::string describe(sys::game_state const& state, dcon::creature_id c);
    // where the tests' spawns may put one (--fuzz, --bench): a tile it can
    // walk on
    bool spawn_spot(world::room_view const& room, rw::ivec2 tile);

    // LanternMouse.Sleeping, the rope (ropeAttatchedPos != null)
    bool sleeping(sys::game_state const& state, dcon::creature_id c) noexcept;
    bool hanging(sys::game_state const& state, dcon::creature_id c) noexcept;
    // MouseGraphics.Battery: SCurve(battery / 4000, 0.2)
    float battery(sys::game_state const& state, dcon::creature_id c) noexcept;
    // its hue (iVars.color), for the graphics and ShortCutColor
    float hue(sys::game_state const& state, dcon::creature_id c) noexcept;
    // MouseAI.fear
    float fear(sys::game_state const& state, dcon::creature_id c) noexcept;

    // The player side: a slugcat holding a mouse in both hands (Player
    // grasps[0], a HeavyCarry TwoHands creature, like a jetfish). items.cpp's
    // grab loop, player_graphics.cpp's hands and shortcuts.cpp's pipes call
    // these the way they call jetfish.hpp's
    // the mouse `p` holds, if any
    dcon::creature_id held_by(sys::game_state const& state, dcon::player_id p) noexcept;
    // Player.PickupCandidate over the mice (Grabability TwoHands always):
    // the best one and its score, lower is better
    dcon::creature_id pickup_candidate(sys::game_state const& state, dcon::player_id p, world::room_view const& room, float& score);
    // Player.SlugcatGrab + Creature.Grab + LanternMouse.Grabbed: held by
    // `p` by its chunk 0, and it squeaks (off its rope first: the port's)
    void grab(sys::game_state& state, dcon::player_id p, dcon::creature_id c);
    // Creature.ReleaseGrasp from the player's side
    void release(sys::game_state& state, dcon::creature_id c);
    // lets go of the mouse `p` holds
    void release_player(sys::game_state& state, dcon::player_id p);
    // Player.GraphicsModuleUpdated's HeavyCarry pull: the held mouse and the
    // slugcat kept 5 + its chunk's radius apart, by mass
    void carry_update(sys::game_state& state, dcon::player_id p);
    // the held mouse travels through a shortcut with `p`: moved to `room`,
    // and (when it comes out) placed on its main chunk
    void follow_player(sys::game_state& state, dcon::player_id p, dcon::room_id room, bool spit_out);
    // Player.ThrowObject's `grabbed is LanternMouse` stun: thrown with a
    // force over 4 it is stunned for 20 ticks
    void thrown(sys::game_state& state, dcon::creature_id c, float force);
    // Player.MovementUpdate's HeavyCarry slow down of the run speed (not a
    // cicada's): 1 + max(0, TotalMass - 0.2), 1.2 for a mouse
    float carry_slowdown(sys::game_state const& state, dcon::creature_id c) noexcept;
}
