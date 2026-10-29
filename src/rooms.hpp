#pragma once

// The objects of a room. Every room updates every tick, in parallel, and a
// room's update only ever looks at what is in that room: these walk the
// room's bucket of sys::game_state::index while the rooms update (built at
// the start of the tick, sorted by id), and every object of the kind,
// filtered by room, the rest of the time (setup, tests, the serial phases of
// the tick). Either way in id order, like the for_each_* they replace.

#include "gamestate.hpp"

namespace rooms {
    inline dcon::room_id room_of(sys::game_state const& state, dcon::physical_object_id o) noexcept {
        return state.world.physical_object_get_room_from_object_location(o);
    }
    inline dcon::room_id room_of(sys::game_state const& state, dcon::player_id p) noexcept {
        return room_of(state, state.world.player_get_body_from_player_body(p));
    }
    inline dcon::room_id room_of(sys::game_state const& state, dcon::creature_id c) noexcept {
        return room_of(state, state.world.creature_get_body_from_creature_body(c));
    }
    inline dcon::room_id room_of(sys::game_state const& state, dcon::item_id it) noexcept {
        return room_of(state, state.world.item_get_body_from_item_body(it));
    }
    inline dcon::room_id room_of(sys::game_state const& state, dcon::lizard_spit_id s) noexcept {
        return dcon::room_id{dcon::room_id::value_base_t(state.world.lizard_spit_get_room(s))};
    }

    // the room whose update runs on this thread, while the rooms update in
    // parallel (simulation.cpp)
    inline thread_local dcon::room_id updating;
    // Whether a room's update may look at the player: only one in that room.
    // Creatures keep the slugcat they track, chase or hold by its index; one
    // that went through a pipe is now another room's, updated by another
    // thread at the same time, so it is out of reach (no longer seen)
    inline bool reachable(sys::game_state const& state, dcon::player_id p) noexcept {
        return !state.parallel || room_of(state, p) == updating;
    }

    namespace detail {
        template<typename ID, typename All, typename F>
        void walk(sys::game_state const& state, sys::room_buckets<ID> const& b, dcon::room_id room, All&& all, F&& f) {
            if (state.index.valid) {
                for (auto const id : b.in(room))
                    f(id);
                return;
            }
            all([&](ID id) {
                if (room_of(state, id) == room)
                    f(id);
            });
        }
    }

    template<typename F>
    void for_each_player(sys::game_state const& state, dcon::room_id room, F&& f) {
        detail::walk(state, state.index.players, room, [&](auto&& g) { state.world.for_each_player(g); }, f);
    }
    // the room's creatures of kind `t`
    template<typename F>
    void for_each_creature(sys::game_state const& state, dcon::room_id room, creature::type t, F&& f) {
        size_t const n = creature::type_count;
        if (state.index.valid) {
            if (!room)
                return;
            size_t const b = size_t(room.index()) * n + size_t(t);
            for (auto const id : state.index.creatures.range(b, b + 1))
                f(id);
            return;
        }
        state.world.for_each_creature([&](dcon::creature_id c) {
            if (state.world.creature_get_type(c) == t && room_of(state, c) == room)
                f(c);
        });
    }
    // every creature of the room, kind by kind (in creature::type order),
    // like one loop per kind
    template<typename F>
    void for_each_creature(sys::game_state const& state, dcon::room_id room, F&& f) {
        if (state.index.valid) {
            if (!room)
                return;
            size_t const b = size_t(room.index()) * creature::type_count;
            for (auto const id : state.index.creatures.range(b, b + creature::type_count))
                f(id);
            return;
        }
        for (auto const t : creature::all_types)
            for_each_creature(state, room, t, f);
    }
    template<typename F>
    void for_each_item(sys::game_state const& state, dcon::room_id room, F&& f) {
        detail::walk(state, state.index.items, room, [&](auto&& g) { state.world.for_each_item(g); }, f);
    }
    template<typename F>
    void for_each_lizard_spit(sys::game_state const& state, dcon::room_id room, F&& f) {
        detail::walk(state, state.index.spits, room, [&](auto&& g) { state.world.for_each_lizard_spit(g); }, f);
    }

    // builds sys::game_state::index from where everything is now
    void build_index(sys::game_state& state);
}
