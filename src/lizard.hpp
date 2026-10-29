#pragma once

// Lizards (Lizard.cs, LizardAI, LizardBreeds), every breed: Pink, Green,
// Blue, Yellow, White, Red, Black, Salamander, Cyan and the Downpour and
// Watcher ones (Caramel/SpitLizard, Eel, Zoop, Train, Blizzard, Basilisk,
// Indigo, Peach). A three chunk body carried by four legs gripping the
// terrain (LizardGraphics' limbs decide how fast it can run), a head that
// bites and grabs, a tail. It hunts the slugcat, bites it (a killing bite
// at the breed's chance), shakes it and carries it off to its den (a room
// exit here), where it is eaten; it idles about the room otherwise.
//
// The breed abilities: the tongue (LizardTongue: Blue, White, Salamander,
// Cyan, Zoop, Indigo, Peach, Train), the lunge, Cyan's jumps
// (LizardJumpModule), Red's and Caramel's spit (LizardSpit), White's
// camouflage, Black's blindness and hearing, the swimmers (Salamander, Eel,
// Peach) and the Yellow pack's flashing communication.
//
// The AI (lizard_ai.cpp) is LizardAI's utility comparison over a simplified
// tracker of the slugcat, and its paths come from pathing.hpp's shared, on
// demand distance fields for the breed's TileTypeResistances.

#include <cstdint>
#include <string_view>

#include "gamestate.hpp"
#include "lizard_types.hpp"
#include "math.hpp"
#include "world.hpp"

namespace lizard {
    // LizardBreeds.BreedTemplate
    breed_params const& params(breed b) noexcept;
    char const* to_string(breed b) noexcept;
    // a breed by name ("green", "salamander", "spit" or "caramel", ...)
    bool parse_breed(std::string_view name, breed& out) noexcept;

    // Lizard constructor + LizardGraphics constructor, standing at `pos`
    dcon::lizard_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos, breed b);
    void destroy(sys::game_state& state, dcon::lizard_id l);
    // Lizard.Update (+ LizardGraphics.Update) for every lizard in the room,
    // and the room's LizardSpit
    void update_room(sys::game_state& state, world::room_view const& room);
    // Room object collisions between lizards and slugcats (pushing apart,
    // Lizard.Collide's bites) and between lizards
    void collide(sys::game_state& state, world::room_view const& room);
    // Weapon.Update's projectile trace against lizards: the lizard the
    // segment hits (radius `rad`), and which chunk
    bool trace(sys::game_state const& state, dcon::room_id room, rw::vec2 from, rw::vec2 to, float rad, dcon::lizard_id& hit, uint32_t& chunk);
    // Lizard.Violence: `dir` is the hit's direction (the head shield and the
    // mouth), `rock` a Rock turning it around, `explosion` the damage type
    void violence(sys::game_state& state, dcon::lizard_id l, uint32_t chunk, rw::vec2 momentum, float damage, float stun_bonus, bool rock = false, bool explosion = false);

    dcon::physical_object_id body(sys::game_state const& state, dcon::lizard_id l) noexcept;
    // bodyChunks[i]: 0 the head (mainBodyChunk), 1 the middle, 2 the hips
    dcon::body_chunk_id chunk(sys::game_state const& state, dcon::lizard_id l, uint32_t i) noexcept;
    bool consious(sys::game_state const& state, dcon::lizard_id l) noexcept;
    // the slugcat in its jaws (grasps[0]), if any
    dcon::player_id grasped(sys::game_state const& state, dcon::lizard_id l) noexcept;
    // releases `p` from every lizard's jaws and tongue
    void release_player(sys::game_state& state, dcon::player_id p);
    // Lizard.JawOpen
    float jaw_open(sys::game_state const& state, dcon::lizard_id l) noexcept;
}
