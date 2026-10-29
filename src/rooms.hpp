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
    inline dcon::room_id room_of(sys::game_state const& state, dcon::centipede_id c) noexcept {
        return room_of(state, state.world.centipede_get_body_from_centipede_body(c));
    }
    inline dcon::room_id room_of(sys::game_state const& state, dcon::dropbug_id d) noexcept {
        return room_of(state, state.world.dropbug_get_body_from_dropbug_body(d));
    }
    inline dcon::room_id room_of(sys::game_state const& state, dcon::cicada_id c) noexcept {
        return room_of(state, state.world.cicada_get_body_from_cicada_body(c));
    }
    inline dcon::room_id room_of(sys::game_state const& state, dcon::lizard_id l) noexcept {
        return room_of(state, state.world.lizard_get_body_from_lizard_body(l));
    }
    inline dcon::room_id room_of(sys::game_state const& state, dcon::daddy_id d) noexcept {
        return room_of(state, state.world.daddy_get_body_from_daddy_body(d));
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
    template<typename F>
    void for_each_centipede(sys::game_state const& state, dcon::room_id room, F&& f) {
        detail::walk(state, state.index.centipedes, room, [&](auto&& g) { state.world.for_each_centipede(g); }, f);
    }
    template<typename F>
    void for_each_dropbug(sys::game_state const& state, dcon::room_id room, F&& f) {
        detail::walk(state, state.index.dropbugs, room, [&](auto&& g) { state.world.for_each_dropbug(g); }, f);
    }
    template<typename F>
    void for_each_cicada(sys::game_state const& state, dcon::room_id room, F&& f) {
        detail::walk(state, state.index.cicadas, room, [&](auto&& g) { state.world.for_each_cicada(g); }, f);
    }
    template<typename F>
    void for_each_lizard(sys::game_state const& state, dcon::room_id room, F&& f) {
        detail::walk(state, state.index.lizards, room, [&](auto&& g) { state.world.for_each_lizard(g); }, f);
    }
    template<typename F>
    void for_each_daddy(sys::game_state const& state, dcon::room_id room, F&& f) {
        detail::walk(state, state.index.daddies, room, [&](auto&& g) { state.world.for_each_daddy(g); }, f);
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
