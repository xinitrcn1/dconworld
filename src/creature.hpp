#pragma once

// Creatures: what every kind shares, and the dispatch to the kinds.
//
// A creature is one `creature` object (creature_types.hpp, the definitions'
// comments in dcon_generated.txt) whose `type` says which kind's code runs
// it: player.* (the slugcat), centipede.*, dropbug.*, cicada.*, lizard.*,
// daddy.*, leech.*, tube_worm.*, needle_worm.*, pole_mimic.*,
// tentacle_plant.*, jetfish.*, scavenger.*, deer.*, miros.*, barnacle.*,
// egg_bug.*, garbage_worm.*, lantern_mouse.*, loach.*, moth_grub.*,
// overseer.*, snail.*, sky_whale.*, yeek.*, vulture.*. Whatever doesn't depend on the kind is here and runs the
// same for all of them: the body and its chunks, Creature.Update's stun
// countdown, being conscious, grasps, the other creatures it refers to (by
// id: a tracked prey, a grasp, a bite), the projectile trace and Violence's
// dispatch, letting go, the room transfer, destroying. The kinds' own code
// is called through the few `switch (type)`s of creature.cpp.
//
// Adding a kind: a payload struct (at most 64 bytes) in creature_types.hpp
// and its entry in `type`, optionally a data object in dcon_generated.txt
// (and its accessor below), and a case in each switch of creature.cpp (and
// render.cpp's sprites).

#include <atomic>
#include <cstdint>
#include <span>
#include <string>

#include "creature_types.hpp"
#include "gamestate.hpp"
#include "math.hpp"
#include "rooms.hpp"
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
    inline dcon::tube_worm_data_id tube_worm_data(dcon::data_container const& w, dcon::creature_id c) noexcept {
        return dcon::tube_worm_data_id{dcon::tube_worm_data_id::value_base_t(w.creature_get_payload(c).tube_worm.data)};
    }
    inline dcon::needle_worm_data_id needle_worm_data(dcon::data_container const& w, dcon::creature_id c) noexcept {
        return dcon::needle_worm_data_id{dcon::needle_worm_data_id::value_base_t(w.creature_get_payload(c).needle_worm.data)};
    }
    inline dcon::pole_mimic_data_id pole_mimic_data(dcon::data_container const& w, dcon::creature_id c) noexcept {
        return dcon::pole_mimic_data_id{dcon::pole_mimic_data_id::value_base_t(w.creature_get_payload(c).pole_mimic.data)};
    }
    inline dcon::tentacle_plant_data_id tentacle_plant_data(dcon::data_container const& w, dcon::creature_id c) noexcept {
        return dcon::tentacle_plant_data_id{dcon::tentacle_plant_data_id::value_base_t(w.creature_get_payload(c).tentacle_plant.data)};
    }
    inline dcon::jetfish_data_id jetfish_data(dcon::data_container const& w, dcon::creature_id c) noexcept {
        return dcon::jetfish_data_id{dcon::jetfish_data_id::value_base_t(w.creature_get_payload(c).jetfish.data)};
    }
    inline dcon::scavenger_data_id scavenger_data(dcon::data_container const& w, dcon::creature_id c) noexcept {
        return dcon::scavenger_data_id{dcon::scavenger_data_id::value_base_t(w.creature_get_payload(c).scavenger.data)};
    }
    inline dcon::deer_data_id deer_data(dcon::data_container const& w, dcon::creature_id c) noexcept {
        return dcon::deer_data_id{dcon::deer_data_id::value_base_t(w.creature_get_payload(c).deer.data)};
    }
    inline dcon::miros_data_id miros_data(dcon::data_container const& w, dcon::creature_id c) noexcept {
        return dcon::miros_data_id{dcon::miros_data_id::value_base_t(w.creature_get_payload(c).miros.data)};
    }
    inline dcon::barnacle_data_id barnacle_data(dcon::data_container const& w, dcon::creature_id c) noexcept {
        return dcon::barnacle_data_id{dcon::barnacle_data_id::value_base_t(w.creature_get_payload(c).barnacle.data)};
    }
    inline dcon::egg_bug_data_id egg_bug_data(dcon::data_container const& w, dcon::creature_id c) noexcept {
        return dcon::egg_bug_data_id{dcon::egg_bug_data_id::value_base_t(w.creature_get_payload(c).egg_bug.data)};
    }
    inline dcon::garbage_worm_data_id garbage_worm_data(dcon::data_container const& w, dcon::creature_id c) noexcept {
        return dcon::garbage_worm_data_id{dcon::garbage_worm_data_id::value_base_t(w.creature_get_payload(c).garbage_worm.data)};
    }
    inline dcon::lantern_mouse_data_id lantern_mouse_data(dcon::data_container const& w, dcon::creature_id c) noexcept {
        return dcon::lantern_mouse_data_id{dcon::lantern_mouse_data_id::value_base_t(w.creature_get_payload(c).lantern_mouse.data)};
    }
    inline dcon::loach_data_id loach_data(dcon::data_container const& w, dcon::creature_id c) noexcept {
        return dcon::loach_data_id{dcon::loach_data_id::value_base_t(w.creature_get_payload(c).loach.data)};
    }
    inline dcon::moth_grub_data_id moth_grub_data(dcon::data_container const& w, dcon::creature_id c) noexcept {
        return dcon::moth_grub_data_id{dcon::moth_grub_data_id::value_base_t(w.creature_get_payload(c).moth_grub.data)};
    }
    inline dcon::overseer_data_id overseer_data(dcon::data_container const& w, dcon::creature_id c) noexcept {
        return dcon::overseer_data_id{dcon::overseer_data_id::value_base_t(w.creature_get_payload(c).overseer.data)};
    }
    inline dcon::snail_data_id snail_data(dcon::data_container const& w, dcon::creature_id c) noexcept {
        return dcon::snail_data_id{dcon::snail_data_id::value_base_t(w.creature_get_payload(c).snail.data)};
    }
    inline dcon::sky_whale_data_id sky_whale_data(dcon::data_container const& w, dcon::creature_id c) noexcept {
        return dcon::sky_whale_data_id{dcon::sky_whale_data_id::value_base_t(w.creature_get_payload(c).sky_whale.data)};
    }
    inline dcon::yeek_data_id yeek_data(dcon::data_container const& w, dcon::creature_id c) noexcept {
        return dcon::yeek_data_id{dcon::yeek_data_id::value_base_t(w.creature_get_payload(c).yeek.data)};
    }
    inline dcon::vulture_data_id vulture_data(dcon::data_container const& w, dcon::creature_id c) noexcept {
        return dcon::vulture_data_id{dcon::vulture_data_id::value_base_t(w.creature_get_payload(c).vulture.data)};
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
    // Creature.Consious (false for one out of reach)
    inline bool consious(sys::game_state const& state, dcon::creature_id c) noexcept {
        return rooms::reachable(state, c) && state.world.creature_get_stun(c) <= 0 && !state.world.creature_get_dead(c);
    }
    // Creature.Update's stun countdown
    inline void stun_countdown(sys::game_state& state, dcon::creature_id c) noexcept {
        int32_t& s = state.world.creature_get_stun(c);
        if (s > 0)
            --s;
    }
    // Creature.mainBodyChunk's index (the centipede's head end)
    uint32_t main_chunk(sys::game_state const& state, dcon::creature_id c) noexcept;

    //
    // Other creatures: what one creature looks at of another (the slugcats
    // included: a slugcat is a creature of type slugcat)
    //
    // whether `c` exists and the room's update may look at it
    // (rooms::reachable: in the room updating on this thread, or in its
    // pipes and dens); what follows answers for live ones only
    inline bool live(sys::game_state const& state, dcon::creature_id c) noexcept {
        return c && state.world.creature_is_valid(c) && rooms::reachable(state, c);
    }
    // the slugcat's Player.cs state (its `player`), none for any other kind
    // or a creature out of reach
    dcon::player_id player_of(sys::game_state const& state, dcon::creature_id c) noexcept;
    inline bool is_slugcat(dcon::data_container const& w, dcon::creature_id c) noexcept { return w.creature_get_type(c) == type::slugcat; }
    // the creature whose body `o` is (none for an item's)
    inline dcon::creature_id of_body(dcon::data_container const& w, dcon::physical_object_id o) noexcept {
        return o ? w.physical_object_get_creature_from_creature_body(o) : dcon::creature_id{};
    }
    // its body, chunks, mass (none for one out of reach)
    dcon::physical_object_id body_of(sys::game_state const& state, dcon::creature_id c) noexcept;
    uint32_t chunk_count_of(sys::game_state const& state, dcon::creature_id c) noexcept;
    dcon::body_chunk_id chunk_of(sys::game_state const& state, dcon::creature_id c, uint32_t i) noexcept;
    // Creature.mainBodyChunk (a slugcat's chunk 0)
    inline uint32_t main_chunk_of(sys::game_state const& state, dcon::creature_id c) noexcept { return live(state, c) ? main_chunk(state, c) : 0; }
    rw::vec2 main_pos(sys::game_state const& state, dcon::creature_id c) noexcept;
    float mass_of(sys::game_state const& state, dcon::creature_id c) noexcept;
    // a slugcat eaten (by a den, the rot, a long legs): dead and gone
    // (a creature eaten is destroyed)
    bool eaten(sys::game_state const& state, dcon::creature_id c) noexcept;
    // dead, or gone out of reach
    inline bool dead(sys::game_state const& state, dcon::creature_id c) noexcept { return !live(state, c) || state.world.creature_get_dead(c) != 0; }
    // out in `room` (not in a pipe, a den or another room, nor an eaten
    // slugcat): what can be seen, touched, bitten
    bool here(sys::game_state const& state, dcon::creature_id c, dcon::room_id room) noexcept;

    //
    // Relationships (relationships.cpp): CreatureTemplate.relationships
    //
    tmpl template_of(sys::game_state const& state, dcon::creature_id c) noexcept;
    char const* to_string(tmpl t) noexcept;
    // CreatureTemplate.CreatureRelationship: `a`'s towards `b`
    relationship static_relationship(tmpl a, tmpl b) noexcept;
    inline relationship static_relationship(sys::game_state const& state, dcon::creature_id c, dcon::creature_id t) noexcept {
        return static_relationship(template_of(state, c), template_of(state, t));
    }
    // CreatureTemplate.bodySize, smallCreature
    float body_size(tmpl t) noexcept;
    bool small_creature(tmpl t) noexcept;
    // AbstractRoom.CreatureAttractionToFloat
    float attraction_value(attraction a) noexcept;

    //
    // Grasps (Creature.grasps): what `c` holds, by grasp: a creature, or (a
    // slugcat's hands) an item
    //
    inline dcon::physical_object_id grasped_object(dcon::data_container const& w, dcon::creature_id c, uint32_t i = 0) noexcept {
        return w.creature_get_grasps(c).obj[i];
    }
    inline dcon::creature_id grasped(dcon::data_container const& w, dcon::creature_id c, uint32_t i = 0) noexcept {
        return of_body(w, grasped_object(w, c, i));
    }
    inline dcon::item_id grasped_item(dcon::data_container const& w, dcon::creature_id c, uint32_t i = 0) noexcept {
        auto const o = grasped_object(w, c, i);
        return o ? w.physical_object_get_item_from_item_body(o) : dcon::item_id{};
    }
    inline uint8_t grasp_chunk(dcon::data_container const& w, dcon::creature_id c, uint32_t i = 0) noexcept { return w.creature_get_grasps(c).chunk[i]; }
    // the grasp holding `t` (-1 none)
    inline int32_t grasp_on(dcon::data_container const& w, dcon::creature_id c, dcon::creature_id t) noexcept {
        auto const o = w.creature_get_body_from_creature_body(t);
        auto const& g = w.creature_get_grasps(c);
        return !o ? -1 : g.obj[0] == o ? 0 : g.obj[1] == o ? 1 : -1;
    }
    inline bool holds(dcon::data_container const& w, dcon::creature_id c, dcon::creature_id t) noexcept { return grasp_on(w, c, t) >= 0; }
    // Creature.Grab: `c`'s grasp `i` on `t`'s chunk (grabbedBy counted on the body)
    void grab(sys::game_state& state, dcon::creature_id c, uint32_t i, dcon::creature_id t, uint32_t chunk);
    // ...on an item (a slugcat's hand)
    void grab_item(sys::game_state& state, dcon::creature_id c, uint32_t i, dcon::item_id it);
    // Creature.ReleaseGrasp
    void release_grasp(sys::game_state& state, dcon::creature_id c, uint32_t i);
    // something holds `t` (grabbedBy.Count > 0)
    inline bool held(sys::game_state const& state, dcon::creature_id t) noexcept {
        auto const o = state.world.creature_get_body_from_creature_body(t);
        return o && state.world.physical_object_get_grabbed_by_count(o) > 0;
    }
    // every creature of `room` holding `t`, with the grasp
    template<typename F>
    void for_each_holder(sys::game_state const& state, dcon::room_id room, dcon::creature_id t, F&& f) {
        auto const o = state.world.creature_get_body_from_creature_body(t);
        if (!o || state.world.physical_object_get_grabbed_by_count(o) == 0)
            return;
        rooms::for_each_creature(state, room, [&](dcon::creature_id h) {
            auto const& g = state.world.creature_get_grasps(h);
            for (uint32_t i = 0; i < 2; ++i)
                if (g.obj[i] == o)
                    f(h, i);
        });
    }
    // the first creature of its room holding `t` (grabbedBy[0]), and by which grasp
    dcon::creature_id holder(sys::game_state const& state, dcon::creature_id t, uint32_t* grasp = nullptr) noexcept;

    //
    // Fights: one creature hurting another (a slugcat included)
    //
    // Creature.Violence on `t`'s chunk `k` (Player.Violence for a slugcat: a
    // stun, death from a full point of damage, letting go of what it holds
    // when stunned hard), by a creature of template `source`
    void violence_on(sys::game_state& state, dcon::creature_id t, uint32_t k, rw::vec2 momentum, float damage, float stun_bonus, damage_type type, tmpl source);
    // Creature.Die of `t`, by `source`
    void kill(sys::game_state& state, dcon::creature_id t, tmpl source);
    // a creature died of something that wasn't a creature (fuzz statistics)
    inline constexpr uint8_t killed_by_other = 0xFF;
    // the kills since start, by killer and victim (the fuzz's; never read
    // by the simulation)
    struct kill_counts {
        std::atomic<uint32_t> n[tmpl_count][tmpl_count] = {};
    };
    kill_counts& kills() noexcept;

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
        // lizard breed, daddy, centipede, leech or noodlefly variant
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
    // starts looking at, or for one
    void came_into_view(sys::game_state& state, dcon::room_id room);
    void reset_graphics(sys::game_state& state, dcon::creature_id c);
    // out of a pipe in `room` at `at` (dens.hpp): what keeps tiles of the
    // room it was in starts over, the graphics module too
    void placed_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, rw::vec2 at);
    // Creature.Stun and Creature.Die as the rain deals them (the kinds' own
    // reactions aside): the stun, death and letting go
    void stun(sys::game_state& state, dcon::creature_id c, int32_t st);
    void die(sys::game_state& state, dcon::creature_id c);
    // Creature.LoseAllGrasps: a slugcat drops what it holds, a creature
    // lets go of everything of its room (below)
    void lose_all_grasps(sys::game_state& state, dcon::creature_id c);
    // Creature.LoseAllGrasps and the rest the kind holds on to (tongue,
    // tentacles, clinging): it lets go of everything of the room
    void release_all(sys::game_state& state, dcon::creature_id c, dcon::room_id room);

    // What a projectile hit: a body chunk, or an appendage (a daddy's
    // tentacle: the segment before the hit and how far along it)
    struct hit_part {
        int32_t chunk = -1;
        int32_t tentacle = -1;
        int32_t prev_segment = 0;
        float distance_to_next = 0.f;
    };
    // SharedPhysics.TraceProjectileAgainstBodyChunks over the room's
    // creatures: the chunk (or a daddy's tentacle) the segment hits first
    // (not `thrown_by`, nor a slugcat when a slugcat threw it)
    bool trace(sys::game_state const& state, dcon::room_id room, rw::vec2 from, rw::vec2 to, float rad, dcon::creature_id thrown_by, dcon::creature_id& hit,
        hit_part& part);
    // Creature.Violence and the kinds' own; `rock`: the source is a Rock
    // (it turns a lizard around)
    void violence(sys::game_state& state, dcon::creature_id c, hit_part const& part, rw::vec2 momentum, float damage, float stun_bonus,
        damage_type type = damage_type::blunt, bool rock = false);
    // an explosion's blast: Violence on the first chunk of every creature
    // within `rad` of `at`, falling off with distance; a slugcat (not
    // `source`'s) within `kill_rad` dies, further out it is stunned (none
    // with no kill radius)
    void blast(sys::game_state& state, dcon::room_id room, rw::vec2 at, float rad, float force, float damage, float stun, float kill_rad = 0.f,
        dcon::physical_object_id source = {});
    // FlareBomb's flash: a stun for every creature that sees it
    void flash(sys::game_state& state, world::room_view const& room, rw::vec2 at, float rad);
    // every creature of `room` lets go of `t` (respawned, destroyed, eaten,
    // warped away, carried into a den)
    void release_target(sys::game_state& state, dcon::creature_id t, dcon::room_id room);
    // ...of `t`, wherever it is
    void release_target(sys::game_state& state, dcon::creature_id t);
    // `c` lets go of `t` (the kind's grasps, clinging, tongue, eating)
    void release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    // Weapon.INotifyOfFlyingWeapons: a thrown weapon at `pos` moving at
    // `vel`, thrown from `thrown_pos`
    void flying_weapon(sys::game_state& state, dcon::room_id room, rw::vec2 pos, rw::vec2 vel, rw::vec2 thrown_pos);
}
