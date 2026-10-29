#pragma once

// Creatures: what every kind shares, and the dispatch to the kinds.
//
// A creature is one `creature` object (creature_types.hpp, the definitions'
// comments in dcon_generated.txt) whose `type` says which kind's code runs
// it: centipede.*, dropbug.*, cicada.*, lizard.*, daddy.*, leech.*. Whatever doesn't
// depend on the kind is here and runs the same for all of them: the body
// and its chunks, Creature.Update's stun countdown, being conscious, the
// slugcat references, the projectile trace and Violence's dispatch, letting
// go of a slugcat, the room transfer, destroying. The kinds' own code is
// called through the few `switch (type)`s of creature.cpp.
//
// Adding a kind: a payload struct (at most 64 bytes) in creature_types.hpp
// and its entry in `type`, optionally a data object in dcon_generated.txt
// (and its accessor below), and a case in each switch of creature.cpp (and
// render.cpp's sprites).

#include <cstdint>
#include <span>
#include <string>

#include "creature_types.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace creature {
    char const* to_string(type t) noexcept;

    // the creature object and its kind's data object (none for a kind
    // without one): the body comes after, from the kind's create
    dcon::creature_id make(sys::game_state& state, type t);
    // deletes the creature, its data object, its parts and its body
    void erase(sys::game_state& state, dcon::creature_id c);

    inline type type_of(dcon::data_container const& w, dcon::creature_id c) noexcept { return w.creature_get_type(c); }
    // `c` is a live creature of kind `t`
    inline bool is(dcon::data_container const& w, dcon::creature_id c, type t) noexcept {
        return w.creature_is_valid(c) && w.creature_get_type(c) == t;
    }

    // the kinds' data objects
    inline dcon::centipede_data_id centipede_data(dcon::data_container const& w, dcon::creature_id c) noexcept {
        return dcon::centipede_data_id{dcon::centipede_data_id::value_base_t(w.creature_get_payload(c).centipede.data)};
    }
    inline dcon::dropbug_data_id dropbug_data(dcon::data_container const& w, dcon::creature_id c) noexcept {
        return dcon::dropbug_data_id{dcon::dropbug_data_id::value_base_t(w.creature_get_payload(c).dropbug.data)};
    }
    inline dcon::cicada_data_id cicada_data(dcon::data_container const& w, dcon::creature_id c) noexcept {
        return dcon::cicada_data_id{dcon::cicada_data_id::value_base_t(w.creature_get_payload(c).cicada.data)};
    }
    inline dcon::lizard_data_id lizard_data(dcon::data_container const& w, dcon::creature_id c) noexcept {
        return dcon::lizard_data_id{dcon::lizard_data_id::value_base_t(w.creature_get_payload(c).lizard.data)};
    }
    inline dcon::daddy_data_id daddy_data(dcon::data_container const& w, dcon::creature_id c) noexcept {
        return dcon::daddy_data_id{dcon::daddy_data_id::value_base_t(w.creature_get_payload(c).daddy.data)};
    }
    inline dcon::leech_data_id leech_data(dcon::data_container const& w, dcon::creature_id c) noexcept {
        return dcon::leech_data_id{dcon::leech_data_id::value_base_t(w.creature_get_payload(c).leech.data)};
    }

    // every creature of kind `t`, in id order (setup, tests, the serial phases)
    template<typename F>
    void for_each_of(dcon::data_container const& w, type t, F&& f) {
        w.for_each_creature([&](dcon::creature_id c) {
            if (w.creature_get_type(c) == t)
                f(c);
        });
    }

    // its parts (creature_part), in the order they were made
    inline std::span<dcon::part_ownership_id const> parts(dcon::data_container const& w, dcon::creature_id c) noexcept {
        auto const r = w.creature_range_of_part_ownership_as_owner(c);
        return std::span<dcon::part_ownership_id const>{r.first, r.second};
    }
    inline dcon::creature_part_id part(dcon::data_container const& w, dcon::creature_id c, uint32_t i) noexcept {
        return w.part_ownership_get_part(parts(w, c)[i]);
    }
    dcon::creature_part_id add_part(sys::game_state& state, dcon::creature_id c, part_kind k, uint8_t index);

    //
    // What every kind has
    //
    inline dcon::physical_object_id body(sys::game_state const& state, dcon::creature_id c) noexcept {
        return state.world.creature_get_body_from_creature_body(c);
    }
    uint32_t chunk_count(sys::game_state const& state, dcon::creature_id c) noexcept;
    dcon::body_chunk_id chunk(sys::game_state const& state, dcon::creature_id c, uint32_t i) noexcept;
    dcon::room_id room_of(sys::game_state const& state, dcon::creature_id c) noexcept;
    float total_mass(sys::game_state const& state, dcon::creature_id c) noexcept;
    // Creature.Consious
    inline bool consious(sys::game_state const& state, dcon::creature_id c) noexcept {
        return state.world.creature_get_stun(c) <= 0 && !state.world.creature_get_dead(c);
    }
    // Creature.Update's stun countdown
    inline void stun_countdown(sys::game_state& state, dcon::creature_id c) noexcept {
        int32_t& s = state.world.creature_get_stun(c);
        if (s > 0)
            --s;
    }
    // Creature.mainBodyChunk's index (the centipede's head end)
    uint32_t main_chunk(sys::game_state const& state, dcon::creature_id c) noexcept;

    // a slugcat a creature keeps as its index + 1: the player if it still
    // exists and the room's update may look at it (rooms::reachable)
    dcon::player_id player_of(sys::game_state const& state, uint8_t v) noexcept;
    inline uint8_t player_ref(dcon::player_id p) noexcept { return p ? uint8_t(p.index() + 1) : 0; }

    //
    // The dispatch to the kinds
    //
    // what the spawn tools and the tests ask for: the kind and its variety
    struct spawn_desc {
        type kind = type::centipede;
        // centipede: the size (< 0 rolled; a plain centipede's only); cicada:
        // 1 a CicadaA (white)
        float size = -1.f;
        // dropbug: moved to the best ceiling spot of the room
        bool relocate = false;
        // lizard breed, daddy, centipede or leech variant
        uint8_t variety = 0;
    };
    dcon::creature_id create(sys::game_state& state, dcon::room_id room, rw::vec2 pos, spawn_desc const& d);
    void destroy(sys::game_state& state, dcon::creature_id c);
    // a line for the log: kind, where, behaviour and state
    std::string describe(sys::game_state const& state, dcon::creature_id c);

    // The room's creatures, kind by kind (the kinds' Update with their
    // graphics modules where a camera looks, then their collisions with the
    // slugcats and their kin)
    void update_room(sys::game_state& state, world::room_view const& room);
    // InitiateGraphicsModule + Reset for every creature of a room a camera
    // starts looking at
    void came_into_view(sys::game_state& state, dcon::room_id room);

    // What a projectile hit: a body chunk, or an appendage (a daddy's
    // tentacle: the segment before the hit and how far along it)
    struct hit_part {
        int32_t chunk = -1;
        int32_t tentacle = -1;
        int32_t prev_segment = 0;
        float distance_to_next = 0.f;
    };
    // Creature.DamageType, as far as the ports tell them apart
    enum class damage_type : uint8_t {
        blunt,
        stab,
        explosion,
        electric,
        bite,
    };
    // SharedPhysics.TraceProjectileAgainstBodyChunks over the room's
    // creatures, kind by kind: the first kind with a hit, its creature the
    // segment hits first
    bool trace(sys::game_state const& state, dcon::room_id room, rw::vec2 from, rw::vec2 to, float rad, dcon::creature_id& hit, hit_part& part);
    // Creature.Violence and the kinds' own; `rock`: the source is a Rock
    // (it turns a lizard around)
    void violence(sys::game_state& state, dcon::creature_id c, hit_part const& part, rw::vec2 momentum, float damage, float stun_bonus,
        damage_type type = damage_type::blunt, bool rock = false);
    // an explosion's blast: Violence on the first chunk of every creature
    // within `rad` of `at`, falling off with distance
    void blast(sys::game_state& state, dcon::room_id room, rw::vec2 at, float rad, float force, float damage, float stun);
    // FlareBomb's flash: a stun for every creature that sees it
    void flash(sys::game_state& state, world::room_view const& room, rw::vec2 at, float rad);
    // every creature lets go of the slugcat (respawned, destroyed, eaten,
    // warped away): the ones in its room
    void release_player(sys::game_state& state, dcon::player_id p);
    // `c` lets go of the slugcat (the kind's grasps, clinging, tongue, eating)
    void release_player(sys::game_state& state, dcon::creature_id c, dcon::player_id p);
    // Weapon.INotifyOfFlyingWeapons: a thrown weapon at `pos` moving at
    // `vel`, thrown from `thrown_pos`
    void flying_weapon(sys::game_state& state, dcon::room_id room, rw::vec2 pos, rw::vec2 vel, rw::vec2 thrown_pos);
}
