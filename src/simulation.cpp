#include <vector>

#include "simulation.hpp"
#include "centipede.hpp"
#include "gates.hpp"
#include "items.hpp"
#include "player.hpp"
#include "player_graphics.hpp"
#include "rot.hpp"
#include "warps.hpp"
#include "shortcuts.hpp"
#include "world.hpp"

namespace simulation {
    using rw::vec2;

    dcon::room_id player_room(sys::game_state const& state, dcon::player_id p) noexcept {
        return state.world.physical_object_get_room_from_object_location(player::body(state, p));
    }

    void activate_room(sys::game_state& state, dcon::room_id room) {
        items::spawn_room_items(state, room);
        rot::spawn_room(state, room);
        warps::spawn_room(state, room);
    }

    namespace {
        // ActiveTriggerChecker for the music triggers of the player's room
        void update_music_triggers(sys::game_state& state, dcon::player_id p, dcon::room_id room) {
            auto& w = state.world;
            bool const entered = w.player_get_trigger_room(p) != room;
            if (entered) {
                // a realised room gets fresh trigger checkers
                w.player_set_trigger_room(p, room);
                w.room_for_each_music_trigger_location(room, [&](dcon::music_trigger_location_id l) {
                    auto const t = w.music_trigger_location_get_trigger(l);
                    w.music_trigger_set_fired(t, false);
                    w.music_trigger_set_counter(t, -1);
                });
            }
            vec2 const main = w.body_chunk_get_pos(player::body_chunk(state, p, 0));
            w.room_for_each_music_trigger_location(room, [&](dcon::music_trigger_location_id l) {
                auto const t = w.music_trigger_location_get_trigger(l);
                if (w.music_trigger_get_fired(t) && !w.music_trigger_get_multi_use(t))
                    return;
                bool fire = false;
                int16_t& counter = w.music_trigger_get_counter(t);
                if (counter >= 0) {
                    if (++counter >= w.music_trigger_get_delay(t)) {
                        fire = true;
                        counter = -1;
                    }
                } else {
                    bool const positive = w.music_trigger_get_spot(t)
                        ? rw::custom::dist_less(main, w.music_trigger_get_pos(t), w.music_trigger_get_rad(t))
                        : entered;
                    if (positive) {
                        if (w.music_trigger_get_delay(t) < 1)
                            fire = true;
                        else
                            counter = 0;
                    }
                }
                if (!fire)
                    return;
                w.music_trigger_set_fired(t, true);
                float const chance = w.music_trigger_get_fire_chance(t);
                if (chance < 1.f && state.random.value() >= chance)
                    return;
                sound::music_request r;
                r.stop = w.music_trigger_get_stop(t);
                r.song = std::string{state.to_string_view(w.music_trigger_get_song(t))};
                r.vol = w.music_trigger_get_vol(t);
                r.prio = w.music_trigger_get_prio(t);
                r.fade_in = w.music_trigger_get_fade_in(t);
                r.fade_out = w.music_trigger_get_fade_out(t);
                r.rooms = w.music_trigger_get_rooms(t);
                if (state.music.size() < 16)
                    state.music.push_back(std::move(r));
            });
        }
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
            update_music_triggers(state, p, room);
            auto const view = world::get_room_view(state, room);
            player::update(state, p, view, i < inputs.size() ? inputs[i] : ::player::input_package{});
            player_graphics::update(state, p, view);
            // Player.GraphicsModuleUpdated: held items follow the hands
            items::carry_update(state, p);
            items::back_spear_update(state, p);
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
            auto const view = world::get_room_view(state, room);
            centipede::update_room(state, view);
            centipede::collide(state, view);
            items::update_room(state, view);
            rot::update_room(state, view);
            warps::update_room(state, view);
            gates::update_room(state, room);
        }
        warps::update_sequence(state);
        warps::update_ripple_rings(state);
        // noises are heard the tick they happen
        state.noises.clear();
        ++state.tick;
    }
}
