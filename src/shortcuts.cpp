#include <cmath>
#include <vector>

#include "shortcuts.hpp"
#include "centipede.hpp"
#include "cicada.hpp"
#include "items.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "player_graphics.hpp"

namespace shortcuts {
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    namespace custom = rw::custom;

    namespace {
        dcon::room_id room_of(sys::game_state const& state, dcon::player_id p) {
            return state.world.physical_object_get_room_from_object_location(player::body(state, p));
        }

        dcon::shortcut_id shortcut_starting_at(sys::game_state const& state, dcon::room_id room, ivec2 tile) {
            auto const& w = state.world;
            auto const range = w.room_range_of_shortcut_location(room);
            for (auto it = range.first; it != range.second; ++it) {
                auto const sc = w.shortcut_location_get_shortcut(*it);
                if (w.shortcut_get_start(sc) == tile)
                    return sc;
            }
            return dcon::shortcut_id{};
        }

        // Moves the player, and whatever it holds, to `room`
        void move_to_room(sys::game_state& state, dcon::player_id p, dcon::room_id room) {
            auto& w = state.world;
            w.physical_object_set_room_from_object_location(player::body(state, p), room);
            if (w.creature_get_away(player::creature_of(state, p)) != 0)
                w.creature_set_away_room(player::creature_of(state, p), room);
            for (uint32_t hand = 0; hand < 2; ++hand)
                if (auto const item = items::held(state, p, hand))
                    w.physical_object_set_room_from_object_location(w.item_get_body_from_item_body(item), room);
            cicada::follow_player(state, p, room, false);
            centipede::follow_player(state, p, room, false);
        }

        // Creature/Player.SpitOutOfShortCut
        void spit_out(sys::game_state& state, dcon::player_id p, ivec2 tile) {
            auto& w = state.world;
            auto const room = world::get_room_view(state, room_of(state, p));
            vec2 const a = rw::to_vec2(room.shortcut_entrance_hole_direction(tile));
            for (uint32_t i = 0; i < 2; ++i) {
                auto const c = player::body_chunk(state, p, i);
                physics::hard_set_position(state, c, room_view::middle_of_tile(tile) - a * ((-0.5f + float(i)) * 5.f));
                w.body_chunk_set_vel(c, a * 2.f);
            }
            w.creature_set_away(player::creature_of(state, p), 0);
            w.creature_set_away_room(player::creature_of(state, p), dcon::room_id{});
            w.creature_set_shortcut_delay(player::creature_of(state, p), 20);
            w.creature_set_entering_shortcut(player::creature_of(state, p), ivec2{-1, -1});
            player_graphics::reset(state, p);
            cicada::follow_player(state, p, room.id, true);
            centipede::follow_player(state, p, room.id, true);
        }

        // the vessel heads back to where it came from
        void bounce_back(sys::game_state& state, dcon::player_id p) {
            auto& w = state.world;
            w.creature_set_vessel_reverse(player::creature_of(state, p), w.creature_get_vessel_reverse(player::creature_of(state, p)) ? 0 : 1);
            w.creature_set_vessel_progress(player::creature_of(state, p), 0.f);
            w.creature_set_vessel_last_progress(player::creature_of(state, p), 0.f);
        }
    }

    void check_entrance(sys::game_state& state, dcon::player_id p, room_view const& room) {
        auto& w = state.world;
        if (w.creature_get_shortcut_delay(player::creature_of(state, p)) >= 1 || w.creature_get_entering_shortcut(player::creature_of(state, p)).x >= 0)
            return;
        auto const& in = w.player_get_input(p)[0];
        for (uint32_t i = 0; i < 2; ++i) {
            ivec2 const tile = room_view::get_tile_position(w.body_chunk_get_pos(player::body_chunk(state, p, i)));
            if (room.get_tile(tile).t != world::terrain::shortcut_entrance)
                continue;
            auto const type = room.get_shortcut_data(tile).type;
            if (type == world::shortcut_type::dead_end || type == world::shortcut_type::creature_hole || type == world::shortcut_type::npc_transportation)
                continue;
            ivec2 const dir = room.shortcut_entrance_hole_direction(tile);
            if (in.x == -dir.x && in.y == -dir.y) {
                w.creature_set_entering_shortcut(player::creature_of(state, p), tile);
                return;
            }
        }
    }

    bool update_entering(sys::game_state& state, dcon::player_id p, room_view const& room) {
        auto& w = state.world;
        ivec2 const entering = w.creature_get_entering_shortcut(player::creature_of(state, p));
        if (entering.x < 0)
            return false;
        float& rad_add = w.creature_get_shortcut_rad_add(player::creature_of(state, p));
        rad_add += 0.2f;
        vec2 const target = room_view::middle_of_tile(entering) + rw::to_vec2(room.shortcut_entrance_hole_direction(entering)) * -5.f;
        int outside = 0;
        for (uint32_t i = 0; i < 2; ++i) {
            auto const c = player::body_chunk(state, p, i);
            vec2& pos = w.body_chunk_get_pos(c);
            w.body_chunk_get_vel(c) *= 0.06f;
            if (!custom::dist_less(pos, target, std::max(10.f, rad_add))) {
                w.body_chunk_set_last_pos(c, pos);
                pos += custom::dir_vec(pos, target) * 4.5f;
                ++outside;
            } else {
                pos = target;
                w.body_chunk_set_last_pos(c, target);
            }
        }
        if (outside == 0) {
            // SuckedIntoShortCut: becomes a ShortCutVessel at the entrance
            auto const sc = shortcut_starting_at(state, room.id, entering);
            w.creature_set_entering_shortcut(player::creature_of(state, p), ivec2{-1, -1});
            rad_add = 0.f;
            if (!sc)
                return true;
            w.creature_set_away(player::creature_of(state, p), 1);
            w.creature_set_away_room(player::creature_of(state, p), room.id);
            w.creature_set_vessel_shortcut(player::creature_of(state, p), sc);
            w.creature_set_vessel_reverse(player::creature_of(state, p), 0);
            w.creature_set_vessel_progress(player::creature_of(state, p), 0.f);
            w.creature_set_vessel_last_progress(player::creature_of(state, p), 0.f);
        }
        return true;
    }

    void update_vessel(sys::game_state& state, dcon::player_id p) {
        auto& w = state.world;
        auto const sc = w.creature_get_vessel_shortcut(player::creature_of(state, p));
        auto const path = w.shortcut_get_path(sc);
        float const len = float(path.size());
        if (len < 2.f) {
            spit_out(state, p, w.shortcut_get_start(sc));
            return;
        }
        float& progress = w.creature_get_vessel_progress(player::creature_of(state, p));
        w.creature_set_vessel_last_progress(player::creature_of(state, p), progress);
        progress += vessel_speed;
        // ShortcutHandler ticks once per tile
        if (std::floor(progress) != std::floor(progress - vessel_speed))
            state.play_sound(sound::sound_id::player_tick_along_in_shortcut, dcon::room_id{}, vec2{0.f, 0.f});
        if (progress < len - 1.f)
            return;
        progress = len - 1.f;

        if (w.creature_get_vessel_reverse(player::creature_of(state, p))) {
            // travelling back towards the entrance
            spit_out(state, p, path[0]);
            return;
        }
        auto const type = w.shortcut_get_type(sc);
        if (type == world::shortcut_type::normal) {
            spit_out(state, p, path[uint32_t(len) - 1]);
            return;
        }
        if (type != world::shortcut_type::room_exit) {
            bounce_back(state, p);
            return;
        }
        // room exit: AbstractRoom.connections, then the destination's own exit
        // leading back here (AbstractRoom.ExitIndex) and the pipe ending there
        auto const from_room = room_of(state, p);
        auto const exit = w.shortcut_get_exit_from_shortcut_destination(sc);
        auto const target = exit ? w.room_exit_get_target_from_exit_connection(exit) : dcon::room_id{};
        if (!target) {
            bounce_back(state, p);
            return;
        }
        dcon::room_exit_id back_exit;
        w.room_for_each_exit_location(target, [&](dcon::exit_location_id el) {
            auto const e = w.exit_location_get_exit(el);
            if (!back_exit && w.room_exit_get_target_from_exit_connection(e) == from_room)
                back_exit = e;
        });
        dcon::shortcut_id arrival;
        if (back_exit) {
            w.room_for_each_shortcut_location(target, [&](dcon::shortcut_location_id sl) {
                auto const s2 = w.shortcut_location_get_shortcut(sl);
                if (!arrival && w.shortcut_get_exit_from_shortcut_destination(s2) == back_exit)
                    arrival = s2;
            });
        }
        if (!arrival) {
            bounce_back(state, p);
            return;
        }
        move_to_room(state, p, target);
        w.creature_set_vessel_shortcut(player::creature_of(state, p), arrival);
        w.creature_set_vessel_reverse(player::creature_of(state, p), 1);
        w.creature_set_vessel_progress(player::creature_of(state, p), 0.f);
        w.creature_set_vessel_last_progress(player::creature_of(state, p), 0.f);
    }

    vec2 vessel_position(sys::game_state const& state, dcon::player_id p, float alpha) {
        auto const& w = state.world;
        auto const sc = w.creature_get_vessel_shortcut(player::creature_of(state, p));
        auto const path = w.shortcut_get_path(sc);
        uint32_t const n = path.size();
        if (n == 0)
            return w.body_chunk_get_pos(player::body_chunk(state, p, 0));
        float t = rw::mathf::lerp(w.creature_get_vessel_last_progress(player::creature_of(state, p)), w.creature_get_vessel_progress(player::creature_of(state, p)), alpha);
        if (w.creature_get_vessel_reverse(player::creature_of(state, p)))
            t = float(n - 1) - t;
        t = rw::mathf::clamp(t, 0.f, float(n - 1));
        uint32_t const i = std::min(uint32_t(t), n - 1);
        uint32_t const j = std::min(i + 1, n - 1);
        return rw::lerp(room_view::middle_of_tile(path[i]), room_view::middle_of_tile(path[j]), t - float(i));
    }
}
