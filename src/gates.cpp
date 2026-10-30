#include <optional>

#include "gates.hpp"
#include "creature.hpp"
#include "player.hpp"
#include "rooms.hpp"

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
        // ticks something has to stay over the gate's middle before it
        // cycles (vanilla's startCounter, without its stand-still wait)
        constexpr int32_t trigger_ticks = 60;
        // not vanilla: a failsafe for creatures that stay in the airlock
        // (a lizard sitting in it, an NPC that can't find its way out): 5 s
        // with its sides shut and it goes back to open, the middle door
        // shutting on whatever is still in the way
        constexpr int32_t held_ticks = 5 * 40;

        // RegionGate.ChangeDoorStatus: rewrites the door's tiles
        void change_door_status(sys::game_state& state, dcon::room_id room, int32_t door, bool open) {
            auto& w = state.world;
            int32_t const width = w.room_get_width(room);
            int32_t const height = w.room_get_height(room);
            int32_t const x0 = door_first_x + door * door_spacing;
            auto const first = w.room_get_first_tile(room).id.index();
            ++w.room_get_geometry_version(room);
            for (int32_t i = 0; i < 2; ++i)
                for (int32_t j = door_bottom_y; j <= door_top_y; ++j) {
                    if (x0 + i >= width || j >= height)
                        continue;
                    dcon::tile_id const t{dcon::tile_id::value_base_t(first + j * width + x0 + i)};
                    w.tile_set_terrain(t, open ? world::terrain::air : world::terrain::solid);
                }
        }

        // abstract position of a creature (the tile of its main chunk), if it
        // is out in `room` and alive (a slugcat, or any creature passing
        // through: the gate works for whatever is in it)
        bool creature_tile(sys::game_state const& state, dcon::creature_id c, dcon::room_id room, ivec2& out) {
            if (!creature::here(state, c, room) || state.world.creature_get_dead(c))
                return false;
            out = world::room_view::get_tile_position(creature::main_pos(state, c));
            return true;
        }

        // RegionGate.DetectZone
        int32_t detect_zone(sys::game_state const& state, dcon::creature_id c, dcon::room_id room) {
            ivec2 t;
            if (!creature_tile(state, c, room, t))
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

        // RegionGate.PlayersInZone over the room's creatures, the slugcats
        // included, counting only those over the gate's middle (zones 1 and
        // 2, judged by x alone so flying and moving creatures count): the
        // zone they all are in, -1 none or both sides
        int32_t players_in_zone(sys::game_state const& state, dcon::room_id room) {
            int32_t num = -1;
            bool mixed = false;
            rooms::for_each_creature(state, room, [&](dcon::creature_id c) {
                if (mixed)
                    return;
                int32_t const z = detect_zone(state, c, room);
                if (z != 1 && z != 2)
                    return;
                if (num >= 0 && z != num) {
                    num = -1;
                    mixed = true;
                    return;
                }
                num = z;
            });
            return num;
        }

        bool all_players_through(sys::game_state const& state, dcon::room_id room, bool let_through_dir) {
            int32_t const half = state.world.room_get_width(room) / 2;
            bool through = true;
            rooms::for_each_creature(state, room, [&](dcon::creature_id c) {
                ivec2 t;
                if (!creature_tile(state, c, room, t))
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

    std::optional<ivec2> wait_tile(sys::game_state const& state, dcon::room_id room, vec2 from, ivec2 to) noexcept {
        auto const g = gate_of(state, room);
        if (!g)
            return std::nullopt;
        // the first closed door between it and where it goes: wait in front
        // of it (in the middle zones the gate cycles)
        int32_t const x = world::room_view::get_tile_position(from).x;
        vec3 const closed = state.world.gate_get_door_closed(g);
        bool const right = to.x > x;
        for (int32_t i = 0; i < 3; ++i) {
            int32_t const k = right ? i : 2 - i;
            int32_t const dx = door_first_x + k * door_spacing;
            bool const between = right ? (dx + 1 > x && dx < to.x) : (dx < x && dx + 1 > to.x);
            if (between && closed[k] > 0.f)
                return ivec2{right ? dx - 2 : dx + 3, door_bottom_y};
        }
        return std::nullopt;
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
                if (!w.gate_get_dont_open(g)) {
                    if (++start_counter > trigger_ticks) {
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
        case gate_mode::middle_open: {
            int32_t& held = w.gate_get_held_counter(g);
            if (all_players_through(state, room, w.gate_get_let_through_dir(g)) || ++held > held_ticks) {
                held = 0;
                goal[1] = 1.f;
                w.gate_set_mode(g, gate_mode::closing_middle);
            }
            break;
        }
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

        // RegionGateGraphics.backgroundWorkingLoop: the machinery humming in
        // the middle of the room while the gate works (the steam, waterfall,
        // clamp and screw loops come with RegionGateGraphics; FakeGate rooms
        // aren't told apart)
        auto const mode = w.gate_get_mode(g);
        bool const idle = mode == gate_mode::middle_closed || mode == gate_mode::closed || mode == gate_mode::waiting || mode == gate_mode::middle_open;
        float& vol = w.gate_get_working_loop_volume(g);
        state.play_loop(sound::loop_key(sound::loop_owner::gate, uint32_t(g.index())),
            w.gate_get_electric(g) ? sound::sound_id::gate_electric_background_loop : sound::sound_id::gate_water_working_background_loop,
            room, center, vol);
        vol = mathf::lerp(vol, idle ? 0.f : 1.f, 0.05f);
    }
}
