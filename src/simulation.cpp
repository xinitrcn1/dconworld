#include <vector>

#include "simulation.hpp"
#include "gates.hpp"
#include "items.hpp"
#include "player.hpp"
#include "player_graphics.hpp"
#include "shortcuts.hpp"
#include "world.hpp"

namespace simulation {
    dcon::room_id player_room(sys::game_state const& state, dcon::player_id p) noexcept {
        return state.world.physical_object_get_room_from_object_location(player::body(state, p));
    }

    void activate_room(sys::game_state& state, dcon::room_id room) {
        items::spawn_room_items(state, room);
    }

    void tick(sys::game_state& state, std::span<dcon::player_id const> players, std::span<::player::input_package const> inputs) {
        for (size_t i = 0; i < players.size(); ++i) {
            auto const p = players[i];
            if (state.world.player_get_in_shortcut(p)) {
                shortcuts::update_vessel(state, p);
                if (!state.world.player_get_in_shortcut(p))
                    activate_room(state, player_room(state, p));
                continue;
            }
            auto const room = player_room(state, p);
            activate_room(state, room);
            auto const view = world::get_room_view(state, room);
            player::update(state, p, view, i < inputs.size() ? inputs[i] : ::player::input_package{});
            player_graphics::update(state, p, view);
        }

        // rooms with someone in them are the realised ones
        std::vector<dcon::room_id> rooms;
        for (auto const p : players) {
            auto const room = player_room(state, p);
            bool seen = false;
            for (auto const r : rooms)
                seen = seen || r == room;
            if (!seen)
                rooms.push_back(room);
        }
        for (auto const room : rooms) {
            items::update_room(state, world::get_room_view(state, room));
            gates::update_room(state, room);
        }
    }
}
