#include "gates.hpp"
#include "player.hpp"

namespace gates {
    using rw::ivec2;
    using rw::vec2;
    using rw::vec3;
    using world::gate_mode;
    namespace mathf = rw::mathf;

    namespace {
        // Door.closeSpeed / openSpeed
        constexpr float close_speed = 0.0055555557f;
        constexpr float open_speed = 0.0045454544f;

        // RegionGate.ChangeDoorStatus: rewrites the door's tiles
        void change_door_status(sys::game_state& state, dcon::room_id room, int32_t door, bool open) {
            auto& w = state.world;
            int32_t const width = w.room_get_width(room);
            int32_t const height = w.room_get_height(room);
            int32_t const x0 = door_first_x + door * door_spacing;
            auto const first = w.room_get_first_tile(room).id.index();
            for (int32_t i = 0; i < 2; ++i)
                for (int32_t j = door_bottom_y; j <= door_top_y; ++j) {
                    if (x0 + i >= width || j >= height)
                        continue;
                    dcon::tile_id const t{dcon::tile_id::value_base_t(first + j * width + x0 + i)};
                    w.tile_set_terrain(t, open ? world::terrain::air : world::terrain::solid);
                }
        }

        // abstract position of a player (tile of the main chunk), if in `room`
        bool player_tile(sys::game_state const& state, dcon::player_id p, dcon::room_id room, ivec2& out) {
            auto const& w = state.world;
            if (w.player_get_in_shortcut(p) || w.physical_object_get_room_from_object_location(player::body(state, p)) != room)
                return false;
            out = world::room_view::get_tile_position(w.body_chunk_get_pos(player::body_chunk(state, p, 0)));
            return true;
        }

        // RegionGate.DetectZone
        int32_t detect_zone(sys::game_state const& state, dcon::player_id p, dcon::room_id room) {
            ivec2 t;
            if (!player_tile(state, p, room, t))
                return -1;
            int32_t const half = state.world.room_get_width(room) / 2;
            if (t.x < half - 8)
                return 0;
            if (t.x < half)
                return 1;
            if (t.x < half + 8)
                return 2;
            return 3;
        }

        // RegionGate.PlayersInZone
        int32_t players_in_zone(sys::game_state const& state, dcon::room_id room) {
            int32_t num = -1;
            bool any = false;
            bool mixed = false;
            state.world.for_each_player([&](dcon::player_id p) {
                if (mixed)
                    return;
                int32_t const z = detect_zone(state, p, room);
                if (any && z != num) {
                    num = -1;
                    mixed = true;
                    return;
                }
                any = true;
                num = z;
            });
            return num;
        }

        bool players_standing_still(sys::game_state const& state) {
            bool still = true;
            state.world.for_each_player([&](dcon::player_id p) {
                if (state.world.player_get_touched_no_input_counter(p) < 20)
                    still = false;
            });
            return still;
        }

        bool all_players_through(sys::game_state const& state, dcon::room_id room, bool let_through_dir) {
            int32_t const half = state.world.room_get_width(room) / 2;
            bool through = true;
            state.world.for_each_player([&](dcon::player_id p) {
                ivec2 t;
                if (!player_tile(state, p, room, t))
                    return;
                if ((!let_through_dir || t.x < half + 3) && (let_through_dir || t.x > half - 4))
                    through = false;
            });
            return through;
        }

        bool all_doors_in_position(vec3 const& closed, vec3 const& goal) {
            return closed[0] == goal[0] && closed[1] == goal[1] && closed[2] == goal[2];
        }
    }

    dcon::gate_id gate_of(sys::game_state const& state, dcon::room_id room) noexcept {
        return state.world.room_get_gate_from_gate_location(room);
    }

    void update_room(sys::game_state& state, dcon::room_id room) {
        auto& w = state.world;
        auto const g = gate_of(state, room);
        if (!g)
            return;
        vec3& goal = w.gate_get_door_goal(g);
        vec3& closed = w.gate_get_door_closed(g);
        int32_t& start_counter = w.gate_get_start_counter(g);
        switch (w.gate_get_mode(g)) {
        case gate_mode::middle_closed: {
            int32_t const num = players_in_zone(state, room);
            if (num > 0 && num < 3) {
                bool const dir = num == 1;
                w.gate_set_let_through_dir(g, dir);
                if (!w.gate_get_dont_open(g) && players_standing_still(state)) {
                    if (++start_counter > 60) {
                        w.gate_set_mode(g, gate_mode::closing_air_lock);
                        goal[dir ? 0 : 2] = 1.f;
                        start_counter = 0;
                    }
                } else {
                    start_counter = 0;
                }
            } else {
                w.gate_set_dont_open(g, 0);
                start_counter = 0;
            }
            break;
        }
        case gate_mode::closing_air_lock:
            if (all_doors_in_position(closed, goal)) {
                w.gate_set_washing_counter(g, 0);
                w.gate_set_mode(g, gate_mode::waiting);
            }
            break;
        case gate_mode::waiting:
            // the next region is always loaded: no waitingForWorldLoader
            if (++w.gate_get_washing_counter(g) > 400) {
                w.gate_set_mode(g, gate_mode::opening_middle);
                goal[1] = 0.f;
            }
            break;
        case gate_mode::opening_middle:
            if (all_doors_in_position(closed, goal))
                w.gate_set_mode(g, gate_mode::middle_open);
            break;
        case gate_mode::middle_open:
            if (all_players_through(state, room, w.gate_get_let_through_dir(g))) {
                goal[1] = 1.f;
                w.gate_set_mode(g, gate_mode::closing_middle);
            }
            break;
        case gate_mode::closing_middle:
            if (all_doors_in_position(closed, goal)) {
                w.gate_set_mode(g, gate_mode::opening_side);
                goal[0] = 0.f;
                goal[2] = 0.f;
            }
            break;
        case gate_mode::opening_side:
            if (all_doors_in_position(closed, goal)) {
                w.gate_set_mode(g, gate_mode::closed);
                w.gate_set_dont_open(g, 1);
            }
            break;
        case gate_mode::closed:
            // vanilla's gate stays Closed until the room is realised again,
            // which resets it to MiddleClosed. Same door layout, and dontOpen
            // keeps it shut until every player left the middle zones
            w.gate_set_mode(g, gate_mode::middle_closed);
            break;
        case gate_mode::broken:
            break;
        }

        // Door.Update
        uint8_t& last_closed = w.gate_get_last_closed(g);
        vec2 const center{float(w.room_get_width(room)) * 10.f, float(w.room_get_height(room)) * 10.f};
        for (int32_t i = 0; i < 3; ++i) {
            // RegionGateGraphics: the door machinery starts and slams shut
            if (closed[i] < goal[i] && closed[i] == 0.f)
                state.play_sound(sound::sound_id::gate_poles_and_rails_in, room, center);
            if (closed[i] < goal[i] && closed[i] + close_speed >= 1.f)
                state.play_sound(sound::sound_id::gate_rails_collide, room, center);
            if (closed[i] > goal[i])
                closed[i] = mathf::max(0.f, closed[i] - open_speed);
            else if (closed[i] < goal[i])
                closed[i] = mathf::min(1.f, closed[i] + close_speed);
            bool const is_closed = closed[i] > 0.f;
            bool const was_closed = (last_closed >> i) & 1u;
            if (is_closed != was_closed)
                change_door_status(state, room, i, !is_closed);
            last_closed = uint8_t((last_closed & ~(1u << i)) | (uint32_t(is_closed) << i));
        }
    }
}
