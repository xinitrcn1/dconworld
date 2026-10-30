#pragma once

// Centipede (Centipede.cs, CentipedeAI): a chain of body chunks that crawls
// along any surface, either end leading, grabs prey with its ends and shocks
// it once both ends hold on. Sizes 0..1 like Centipede.GenerateSize: big ones
// outweigh the slugcat, hunt it and kill it with a shock; small ones are
// afraid of it and only stun it.
//
// Every template running Centipede.cs is a variant of it (the payload's
// `variant`), with vanilla's `if (Centiwing)`, `Small`, `Red` and
// `AquaCenti` branches: the baby centipede (five chunks, stuns what holds
// it, edible), the centiwing (flies between perches, wings), the red
// centipede (18 chunks, armoured shells, hunts relentlessly) and Downpour's
// aquapede (swims, finned, a deadly shock).
//
// The AI is CentipedeAI's utility comparison over a simplified tracker (sight
// of the slugcat, prey/threat by total mass, injury) and its paths come from
// pathing.hpp's shared, on demand distance fields, each variant with its
// template's TileTypeResistances.

#include <cstdint>
#include <string_view>

#include "creature.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "pathing_types.hpp"
#include "world.hpp"

namespace centipede {
    using creature::body;
    using creature::chunk;
    using creature::chunk_count;
    using creature::total_mass;

    char const* to_string(variant v) noexcept;
    // "centipede", "small" (baby), "wing" (centiwing), "red", "aqua" (aquapede)
    bool parse_variant(std::string_view name, variant& out) noexcept;
    inline variant variant_of(dcon::data_container const& w, dcon::creature_id c) noexcept {
        return w.creature_get_payload(c).centipede.variant;
    }

    // `size` < 0 rolls one like Centipede.GenerateSize (only a plain
    // centipede's can be chosen: the other templates set their own)
    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos, float size = -1.f, variant v = variant::centipede);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // Centipede.Update
    void update(sys::game_state& state, dcon::creature_id c, world::room_view const& room);
    // whether it collides with a creature or slugcat (not the one holding it)
    bool collides(sys::game_state const& state, dcon::creature_id c, creature::target t);
    // Centipede.Collide: its chunk `i` touched `t`'s chunk `j` (pushed apart
    // already): seen, annoyed, grabbed with an end; or another centipede's
    void collide(sys::game_state& state, dcon::creature_id c, world::room_view const& room, uint32_t i, creature::target t, uint32_t j);
    void collide_kin(sys::game_state& state, dcon::creature_id c, uint32_t i, dcon::creature_id other, uint32_t j);
    // Centipede.Violence (the shells, the red centipede's armour) + Creature.Violence
    void violence(sys::game_state& state, dcon::creature_id c, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus,
        creature::damage_type type = creature::damage_type::blunt);

    // Centipede.HeadIndex
    uint32_t head_index(sys::game_state const& state, dcon::creature_id c) noexcept;
    // the pathfinding of its template (and the centiwing's flying or not)
    pathing::movement const& movement(sys::game_state const& state, dcon::creature_id c) noexcept;
    // CentipedeState.shells: the chunk's shell is on
    bool has_shell(sys::game_state const& state, dcon::creature_id c, uint32_t chunk) noexcept;
    // lets go of `t` if it holds it, and is let go of by `t`
    void release(sys::game_state& state, dcon::creature_id c, creature::target t);

    // The player side: a baby centipede in a slugcat's hand (IPlayerEdible,
    // Grabability OneHand)
    // the centipede `p` holds in `hand`, if any
    dcon::creature_id held_in_hand(sys::game_state const& state, dcon::player_id p, uint32_t hand) noexcept;
    // Player.PickupCandidate over the baby centipedes: the best one and its
    // score, lower is better
    dcon::creature_id pickup_candidate(sys::game_state const& state, dcon::player_id p, world::room_view const& room, float& score);
    // Player.SlugcatGrab + Creature.Grab: held by `p` in `hand`
    void grab(sys::game_state& state, dcon::player_id p, dcon::creature_id c, uint32_t hand);
    // Creature.ReleaseGrasp from the player's side
    void release(sys::game_state& state, dcon::creature_id c);
    // lets go of whatever baby centipede `p` holds
    void release_player(sys::game_state& state, dcon::player_id p);
    // Player.GraphicsModuleUpdated: the held chunk in the hand
    void carry_update(sys::game_state& state, dcon::player_id p, uint32_t hand, rw::vec2 hand_pos);
    // the baby centipedes `p` holds travel through a shortcut or a warp with
    // it: moved to `room`, and (when it comes out) put in its hands
    void follow_player(sys::game_state& state, dcon::player_id p, dcon::room_id room, bool spit_out);
    // Centipede.BitByPlayer: one bite (it dies on the first); true when it
    // was the last one (eaten: let go of and gone after the tick)
    bool bit_by_player(sys::game_state& state, dcon::creature_id c, dcon::player_id p);
    // IPlayerEdible.FoodPoints
    inline constexpr int32_t food_points = 2;
}
