#include <algorithm>
#include <climits>
#include <cmath>
#include <functional>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "overseer.hpp"
#include "dens.hpp"
#include "migrate.hpp"
#include "npc.hpp"
#include "overseer_graphics.hpp"
#include "particles.hpp"
#include "pathing.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "rain.hpp"
#include "rooms.hpp"

namespace overseer {
    using creature::tmpl;
    using rw::ivec2;
    using rw::vec2;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // CreatureTemplate (Overseer): baseDamageResistance, baseStunResistance,
        // instantDeathDamageLimit; OverseerAI.scaredDistance
        constexpr float base_damage_resistance = 0.9f;
        constexpr float base_stun_resistance = 0.5f;
        constexpr float instant_death_damage_limit = 1.f;
        constexpr float scared_distance = 130.f;
        // float.MaxValue, float.MinValue
        constexpr float max_value = 3.4028235e38f;
        constexpr float min_value = -3.4028235e38f;
        // RainCycle.TimeUntilRain under which it heads for the offscreen den
        constexpr int32_t rain_leave_ticks = 4800;

        creature::overseer_payload& pl(sys::game_state& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).overseer;
        }
        creature::overseer_payload const& pl(sys::game_state const& state, dcon::creature_id c) {
            return state.world.creature_get_payload(c).overseer;
        }
        dcon::overseer_data_id data(sys::game_state const& state, dcon::creature_id c) {
            return creature::overseer_data(state.world, c);
        }
        body_state& bs(sys::game_state& state, dcon::creature_id c) {
            return state.world.overseer_data_get_body(data(state, c));
        }
        body_state const& bs(sys::game_state const& state, dcon::creature_id c) {
            return state.world.overseer_data_get_body(data(state, c));
        }
        ai_state& ais(sys::game_state& state, dcon::creature_id c) {
            return state.world.overseer_data_get_ai(data(state, c));
        }
        ai_state const& ais(sys::game_state const& state, dcon::creature_id c) {
            return state.world.overseer_data_get_ai(data(state, c));
        }
        vec2 mid(ivec2 t) {
            return room_view::middle_of_tile(t);
        }
        ivec2 tile_of(vec2 p) {
            return room_view::get_tile_position(p);
        }
        // IntVector2.FloatDist
        float float_dist(ivec2 a, ivec2 b) {
            return rw::magnitude(rw::to_vec2(a - b));
        }
        bool is_overseer(sys::game_state const& state, dcon::creature_id c) {
            return creature::is(state.world, c, creature::type::overseer);
        }
        // the room it is in, or the one whose pipes or dens it is in
        dcon::room_id room_of_any(sys::game_state const& state, dcon::creature_id c) {
            auto const& w = state.world;
            if (!c || !w.creature_is_valid(c))
                return dcon::room_id{};
            // (the body's room and away_room only change in the transactions)
            auto const r = creature::room_of(state, c);
            return r ? r : w.creature_get_away_room(c);
        }
        dcon::region_id region_of(sys::game_state const& state, dcon::room_id room) {
            return room ? state.world.room_get_region_from_region_membership(room) : dcon::region_id{};
        }
        std::string_view region_name(sys::game_state const& state, dcon::room_id room) {
            auto const r = region_of(state, room);
            return r ? state.to_string_view(state.world.region_get_name(r)) : std::string_view{};
        }

        //
        // Rooms (OverseerAbstractAI.RoomAllowed and the attractions)
        //
        creature::attraction attraction_type(sys::game_state const& state, dcon::room_id room) {
            return state.world.room_get_attraction(room).v[size_t(tmpl::overseer)];
        }
        // AbstractRoom.AttractionValueForCreature
        float attraction(sys::game_state const& state, dcon::room_id room) {
            return room ? creature::attraction_value(attraction_type(state, room)) : 0.f;
        }
        // AbstractRoom.SizeDependentAttractionValueForCreature (not a flyer)
        float size_dependent_attraction(sys::game_state const& state, dcon::room_id room) {
            auto const& w = state.world;
            float const a = attraction(state, room);
            float const x = float(w.room_get_width(room)), y = float(w.room_get_height(room));
            return a * mathf::lerp(x * mathf::lerp(x, 1.f, 0.5f) * y / 50000.f, 1.f, 0.985f);
        }
        // RoomAllowed: not Forbidden (a gate is for the guide), and in the
        // overseer's own region (its World) unless it is the player guide,
        // which follows the slugcat from world to world. (No scavenger
        // outposts or traders check)
        bool room_allowed(sys::game_state const& state, dcon::creature_id c, dcon::room_id room) {
            if (!room)
                return false;
            auto const& w = state.world;
            bool const guide = pl(state, c).guide != 0;
            if (attraction_type(state, room) == creature::attraction::forbidden && !(guide && w.room_get_is_gate(room)))
                return false;
            if (w.room_get_is_shelter(room))
                return false;
            return guide || region_of(state, room) == region_of(state, creature::room_of(state, c));
        }

        // the regions without a rain timer (OverseerAbstractAI's ignoreCycle)
        bool ignores_cycle(sys::game_state const& state, dcon::creature_id c, dcon::room_id room) {
            if (state.world.creature_get_spawn_flags(c) & dens::flag_ignore_cycle)
                return true;
            auto const n = region_name(state, room);
            if (n == "SS" || n == "RM" || n == "DM")
                return true;
            auto const r = region_of(state, room);
            return r && state.world.region_get_never_rains(r);
        }
        // RainCycle.TimeUntilRain < 4800
        bool rain_near(sys::game_state const& state, dcon::room_id room) {
            auto const* cy = rain::of_room(state, room);
            return cy && rain::time_until_rain(*cy) < rain_leave_ticks;
        }

        // OverseerAbstractAI's constructor: whose overseer it is (the colour)
        uint8_t roll_owner(sys::game_state const& state, dcon::room_id room, rw::rng& rng) {
            using sn = ::player::slugcat_name;
            auto const n = region_name(state, room);
            auto const s = state.story_slugcat;
            uint8_t owner = 0;
            if (n == "LM" || n == "DM" || n == "MS")
                owner = 1;
            if ((s == sn::spear || s == sn::artificer) && (n == "HI" || n == "SB" || n == "SH") && rng.value() < 0.2f)
                owner = 1;
            if (s == sn::red && n != "UW" && rng.value() < (n == "SL" ? 0.2f : 0.0125f))
                owner = 1;
            if (s != sn::rivulet && s != sn::saint && s != sn::spear && n == "SB")
                owner = 2;
            if (n == "OE") {
                owner = 0;
                if (rng.value() < 0.1f)
                    owner = 1;
                if (rng.value() < 0.01f)
                    owner = 2;
                if (rng.value() < 0.0086f) {
                    owner = 3;
                    if (rng.value() < 0.1f)
                        owner = 5;
                    else if (rng.value() < 0.45f)
                        owner = 4;
                }
            }
            if (s != sn::spear && n != "SL" && n != "MS" && rng.value() < 0.0001f)
                owner = 3;
            return owner;
        }

        // the scratch list of the room's creatures (Room.abstractRoom.creatures:
        // the ones out in it, the offscreen den's overseers left out)
        std::vector<dcon::creature_id>& creatures_buffer() {
            static thread_local std::vector<dcon::creature_id> v;
            return v;
        }

        // What the overseer's update works with
        struct ctx {
            sys::game_state& state;
            dcon::creature_id c;
            room_view const& room;
            rw::rng& rng;
            creature::overseer_payload& p;
            body_state& b;
            ai_state& ai;
            std::vector<uint16_t> const& prox;
            std::vector<dcon::creature_id>& crits;

            ctx(sys::game_state& s, dcon::creature_id id, room_view const& r)
                : state(s), c(id), room(r), rng(s.world.creature_get_random(id)), p(pl(s, id)), b(bs(s, id)), ai(ais(s, id)),
                  prox(pathing::ai_terrain_proximity_map(s, r.id)), crits(creatures_buffer()) {}

            void fill_creatures() {
                crits.clear();
                rooms::for_each_creature(state, room.id, [&](dcon::creature_id o) {
                    if (state.world.creature_get_away(o) != 0)
                        return;
                    if (is_overseer(state, o) && pl(state, o).offscreen)
                        return;
                    crits.push_back(o);
                });
            }
            vec2& pos() { return state.world.body_chunk_get_pos(chunk(state, c, 0)); }
            vec2& vel() { return state.world.body_chunk_get_vel(chunk(state, c, 0)); }
            bool dead() const { return state.world.creature_get_dead(c) != 0; }
            // AImap.getTerrainProximity (-1 outside the room)
            int32_t proximity(ivec2 t) const {
                if (!room.inside(t.x, t.y))
                    return -1;
                return int32_t(prox[size_t(t.y) * size_t(room.width) + size_t(t.x)]);
            }
            bool solid(ivec2 t) const { return room.get_tile(t).solid(); }
        };

        //
        // The zip pathing matrix
        //
        // ResetZipPathingMatrix
        void reset_matrix(ai_state& ai, ivec2 center) {
            ai.matrix_center = center;
            ai.matrix_expanded = 1;
        }
        // the matrix's window: 30 tiles around the centre (the last row and
        // column left out, like vanilla's)
        struct window {
            int32_t x0 = 0, y0 = 0, w = 1, h = 1;
        };
        window matrix_window(room_view const& room, ivec2 center) {
            window win;
            win.x0 = std::max(0, center.x - 30);
            win.y0 = std::max(0, center.y - 30);
            int32_t const x1 = std::min(room.width - 1, center.x + 30);
            int32_t const y1 = std::min(room.height - 1, center.y + 30);
            win.w = std::max(1, x1 - win.x0);
            win.h = std::max(1, y1 - win.y0);
            return win;
        }
        // the cell's index, -1 outside the room or the window
        int64_t cell_index(room_view const& room, window const& win, ivec2 t) {
            if (!room.inside(t.x, t.y))
                return -1;
            int32_t const lx = t.x - win.x0, ly = t.y - win.y0;
            if (lx < 0 || ly < 0 || lx >= win.w || ly >= win.h)
                return -1;
            return int64_t(ly) * win.w + lx;
        }
        // GetZipPathMatrixCell(t) >= 0 && < float.MaxValue: in the window and
        // filled in already. UpdateZipMatrix fills a few cells a tick from
        // the centre out: here a ring is counted filled once as many cells
        // as the square it closes have been
        bool cell_reached(ctx const& x, ivec2 t) {
            auto const win = matrix_window(x.room, x.ai.matrix_center);
            if (cell_index(x.room, win, t) < 0)
                return false;
            int64_t const r = std::max(std::abs(t.x - x.ai.matrix_center.x), std::abs(t.y - x.ai.matrix_center.y));
            return (2 * r + 1) * (2 * r + 1) <= int64_t(x.ai.matrix_expanded);
        }
        // UpdateZipMatrix: 8 (16 sitting in the wall) * 2 + 1 cells a tick
        // (the pathfinder's resource divider never holds it back here)
        void update_zip_matrix(ctx& x) {
            int32_t const n = (x.p.mode == mode::sitting_in_wall ? 16 : 8) * 2 + 1;
            x.ai.matrix_expanded = std::min(x.ai.matrix_expanded + n, 1 << 20);
        }
        // UpdateZipMatrix's cost of entering tile `n` in direction `d`: a
        // pipe in the rock next to nothing, the rock, poles, the wall behind,
        // open air
        float zip_cost(room_view const& room, ivec2 n, ivec2 d) {
            auto const tl = room.get_tile(n);
            float num;
            if (tl.solid()) {
                if (tl.shortcut > 0) {
                    num = 0.001f;
                } else {
                    bool flag = true;
                    for (int k = 0; k < 4 && flag; ++k)
                        flag = room.get_tile(n + custom::eight_directions[k]).solid();
                    num = flag ? 1.5f : 1.f;
                }
            } else if (tl.any_beam()) {
                num = 5.f;
            } else if ((tl.flags & world::tile_wall_behind) != 0) {
                num = tl.shortcut > 0 ? 2.f : 100.f;
            } else {
                num = 1000.f;
            }
            return num * rw::magnitude(rw::to_vec2(d));
        }
        // the whole matrix (UpdateZipMatrix run to the end): from the
        // centre out, cheapest first, every cell set when first reached
        void build_matrix(ctx& x, window const& win, std::vector<float>& m) {
            m.assign(size_t(win.w) * size_t(win.h), max_value);
            auto const get = [&](ivec2 t) {
                int64_t const i = cell_index(x.room, win, t);
                return i < 0 ? -1.f : m[size_t(i)];
            };
            using item = std::pair<float, uint64_t>;
            std::priority_queue<item, std::vector<item>, std::greater<item>> open;
            auto const pack = [](ivec2 t) { return (uint64_t(uint32_t(t.x)) << 32) | uint64_t(uint32_t(t.y)); };
            auto const unpack = [](uint64_t k) { return ivec2{int32_t(uint32_t(k >> 32)), int32_t(uint32_t(k & 0xFFFFFFFFu))}; };
            ivec2 const center = x.ai.matrix_center;
            if (int64_t const i = cell_index(x.room, win, center); i >= 0)
                m[size_t(i)] = 0.f;
            open.push({get(center), pack(center)});
            while (!open.empty()) {
                ivec2 const t = unpack(open.top().second);
                open.pop();
                float const cur = get(t);
                for (auto const d : custom::eight_directions) {
                    ivec2 const n = t + d;
                    if (get(n) != max_value)
                        continue;
                    float const v = cur + zip_cost(x.room, n, d) * (1.f + x.rng.value());
                    m[size_t(cell_index(x.room, win, n))] = v;
                    open.push({v, pack(n)});
                }
            }
        }
        // Room.RayTraceTilesList
        void ray_trace_tiles(int32_t x0, int32_t y0, int32_t x1, int32_t y1, body_state& b) {
            int32_t num = std::abs(x1 - x0), num2 = std::abs(y1 - y0);
            int32_t cx = x0, cy = y0;
            int32_t i = 1 + num + num2;
            int32_t const sx = x1 > x0 ? 1 : -1, sy = y1 > y0 ? 1 : -1;
            int32_t err = num - num2;
            num *= 2;
            num2 *= 2;
            b.path_count = 0;
            while (i > 0 && b.path_count < max_zip_path) {
                b.path[b.path_count++] = ivec2{cx, cy};
                if (err > 0) {
                    cx += sx;
                    err -= num2;
                } else {
                    cy += sy;
                    err += num;
                }
                --i;
            }
            if (b.path_count == 0)
                b.path[b.path_count++] = ivec2{x0, y0};
        }

        //
        // Overseer
        //
        void switch_modes(sys::game_state& state, dcon::creature_id c, room_view const& room, mode m);

        // Overseer.FindZipPath: down the matrix from the new root to the root
        void find_zip_path(ctx& x, ivec2 new_root, ivec2 new_hover) {
            if (new_root == x.p.root_tile)
                return;
            x.b.next_hover_tile = new_hover;
            x.b.path[0] = new_root;
            x.b.path_count = 1;
            static thread_local std::vector<float> m;
            auto const win = matrix_window(x.room, x.ai.matrix_center);
            build_matrix(x, win, m);
            auto const get = [&](ivec2 t) {
                int64_t const i = cell_index(x.room, win, t);
                return i < 0 ? -1.f : m[size_t(i)];
            };
            ivec2 cur = new_root;
            for (int i = 0; i < 200; ++i) {
                float best = max_value;
                ivec2 next = cur;
                for (auto const d : custom::eight_directions) {
                    ivec2 const n = cur + d;
                    if (n.x >= 0 && n.y >= 0 && n.x < x.room.width && n.y < x.room.height && get(n) >= 0.f && get(n) < best) {
                        best = get(n);
                        next = n;
                    }
                }
                if (next == cur || x.b.path_count >= max_zip_path)
                    break;
                cur = next;
                x.b.path[x.b.path_count++] = cur;
                if (cur == x.p.root_tile)
                    return;
            }
            // ("OVERSEER FAILED PATHING")
            ray_trace_tiles(new_root.x, new_root.y, x.p.root_tile.x, x.p.root_tile.y, x.b);
        }

        // where the other room lies seen from this one (InterRoomZipPath's
        // RoomToWorldPos of a random spot of the other room): far out past
        // the exit leading there, else far out in a random direction (no
        // world map positions)
        vec2 other_room_point(ctx& x, dcon::room_id other) {
            auto const& w = x.state.world;
            vec2 const centre{x.room.pixel_width() * 0.5f, x.room.pixel_height() * 0.5f};
            std::optional<vec2> exit;
            if (other)
                w.room_for_each_exit_location(x.room.id, [&](dcon::exit_location_id el) {
                    auto const e = w.exit_location_get_exit(el);
                    if (!exit && w.room_exit_get_target_from_exit_connection(e) == other)
                        exit = mid(w.room_exit_get_tile(e));
                });
            if (exit)
                return *exit + custom::dir_vec(centre, *exit) * 2000.f;
            return centre + custom::rnv(x.rng) * 2000.f;
        }

        // Overseer.InterRoomZipPath: from the root, through the rock, towards
        // the other room (reversed when leaving: the far end first)
        void inter_room_zip_path(ctx& x, vec2 target, bool into) {
            auto& b = x.b;
            ivec2 cur = x.p.root_tile;
            b.path[0] = cur;
            b.path_count = 1;
            for (int i = 0; i < 50 && b.path_count < max_zip_path; ++i) {
                float best = max_value;
                ivec2 next = cur;
                for (auto const d : custom::eight_directions) {
                    ivec2 const n = cur + d;
                    bool seen = false;
                    for (uint32_t k = 0; k < b.path_count; ++k)
                        if (b.path[k] == n) {
                            seen = true;
                            break;
                        }
                    if (seen)
                        continue;
                    float num = x.solid(n) ? 0.f : 100000.f;
                    num += rw::distance(mid(n), target);
                    num += x.rng.value();
                    if (num < best) {
                        best = num;
                        next = n;
                    }
                }
                if (next == cur || best == max_value)
                    break;
                cur = next;
                b.path[b.path_count++] = cur;
            }
            if (!into)
                std::reverse(b.path, b.path + b.path_count);
        }

        // Overseer.HardSetTile
        void hard_set_tile(sys::game_state& state, dcon::creature_id c, ivec2 hover, ivec2 root) {
            auto& p = pl(state, c);
            auto& b = bs(state, c);
            p.hover_tile = hover;
            b.next_hover_tile = hover;
            physics::hard_set_position(state, chunk(state, c, 0), mid(hover));
            p.root_tile = root;
            b.root_pos = mid(root);
            b.last_root_pos = b.root_pos;
            b.path[0] = root;
            b.path_count = 1;
            reset_matrix(ais(state, c), root);
            // (the graphics module only where a camera looks: came_into_view
            // resets it otherwise)
            if (state.viewed(creature::room_of(state, c)))
                overseer_graphics::reset_positions(state, c);
        }

        // Overseer.PlaceInRoom: a random open tile with rock next to it (the
        // first 100 rolls), else `fallback`'s, else anywhere
        void place_randomly(sys::game_state& state, dcon::creature_id c, room_view const& room, ivec2 fallback) {
            auto& rng = state.world.creature_get_random(c);
            if (room.width > 0 && room.height > 0)
                for (int i = 0; i < 100; ++i) {
                    int32_t const tx = rng.range(0, room.width);
                    ivec2 const t{tx, rng.range(0, room.height)};
                    if (room.get_tile(t).solid())
                        continue;
                    for (int j = 0; j < 8; ++j)
                        if (room.get_tile(t + custom::eight_directions_and_zero[j]).solid()) {
                            hard_set_tile(state, c, t, t + custom::eight_directions_and_zero[j]);
                            return;
                        }
                }
            if (room.inside(fallback.x, fallback.y))
                for (int k = 0; k < 8; ++k)
                    if (room.get_tile(fallback + custom::eight_directions_and_zero[k]).solid()) {
                        hard_set_tile(state, c, fallback, fallback + custom::eight_directions_and_zero[k]);
                        return;
                    }
            int32_t const tx = rng.range(0, std::max(1, room.width));
            ivec2 const t{tx, rng.range(0, std::max(1, room.height))};
            hard_set_tile(state, c, t, t);
        }

        // made at a spot (the tools, the tests): the closest open tile with
        // rock next to it (not vanilla, whose PlaceInRoom is random)
        void place_near(sys::game_state& state, dcon::creature_id c, room_view const& room, ivec2 at) {
            int best = INT_MAX;
            ivec2 hover = at, root = at;
            for (int dy = -8; dy <= 8; ++dy)
                for (int dx = -8; dx <= 8; ++dx) {
                    ivec2 const t = at + ivec2{dx, dy};
                    if (!room.inside(t.x, t.y) || room.get_tile(t).solid() || dx * dx + dy * dy >= best)
                        continue;
                    for (int j = 1; j < 9; ++j)
                        if (room.get_tile(t + custom::eight_directions_and_zero[j]).solid()) {
                            best = dx * dx + dy * dy;
                            hover = t;
                            root = t + custom::eight_directions_and_zero[j];
                            break;
                        }
                }
            if (best == INT_MAX) {
                place_randomly(state, c, room, at);
                return;
            }
            hard_set_tile(state, c, hover, root);
        }

        void switch_modes(sys::game_state& state, dcon::creature_id c, room_view const& room, mode m) {
            auto& w = state.world;
            if (w.creature_get_dead(c))
                return;
            auto& rng = w.creature_get_random(c);
            auto& p = pl(state, c);
            auto& b = bs(state, c);
            if (p.mode == mode::conversing) {
                p.conversation_delay = int16_t(rng.range(50, 120));
                b.last_conversation_partner = b.conversation_partner;
                b.conversation_partner = dcon::creature_id{};
            }
            p.mode = m;
            if (m == mode::withdrawing)
                for (auto& z : b.zip_progs)
                    z = 0.f;
            switch (m) {
            case mode::withdrawing:
                // back into the wall: a little zip into the rock beside the root and out
                if (p.after_withdraw_mode == mode::sitting_in_wall) {
                    ivec2 list[8];
                    int32_t n = 0;
                    for (int j = 0; j < 8; ++j)
                        if (room.get_tile(p.root_tile + custom::eight_directions_and_zero[j]).solid())
                            list[n++] = p.root_tile + custom::eight_directions_and_zero[j];
                    b.path[0] = p.root_tile;
                    b.path_count = 1;
                    if (n > 0) {
                        b.path[1] = list[rng.range(0, n)];
                        b.path[2] = p.root_tile;
                        b.path_count = 3;
                    }
                }
                // (no Overseer_Withdraw sound)
                p.zip_mesh_direction = 0;
                return;
            case mode::zipping:
                p.zip_prog_speed = 3.f / float(std::max<uint16_t>(1, b.path_count));
                reset_matrix(ais(state, c), b.path[0]);
                return;
            case mode::emerging:
                // (no Overseer_Emerge sound)
                p.root_tile = b.path[0];
                p.hover_tile = b.next_hover_tile;
                b.root_pos = mid(p.root_tile);
                b.last_root_pos = b.root_pos;
                b.root_dir = vec2{0.f, 0.f};
                b.last_root_dir = vec2{0.f, 0.f};
                physics::hard_set_position(state, chunk(state, c, 0), b.root_pos);
                if (state.viewed(room.id))
                    overseer_graphics::emerge(state, c);
                return;
            case mode::sitting_in_wall:
                p.zip_prog_speed = 0.f;
                return;
            default:
                return;
            }
        }

        //
        // OverseerAbstractAI
        //
        // HowInterestingIsCreature. A creature in another room (out of
        // reach) is taken as alive; the slugcat's karma is the story's
        float how_interesting(ctx const& x, dcon::creature_id t) {
            auto const& state = x.state;
            auto const& w = state.world;
            if (!t || !w.creature_is_valid(t))
                return 0.f;
            tmpl const tt = creature::template_of(state, t);
            if (creature::small_creature(tt))
                return 0.f;
            float num;
            switch (tt) {
            case tmpl::slugcat:
                num = custom::lerp_map(float(state.karma), 0.f, 4.f, 0.15f, 2.f, 1.2f);
                if (x.p.guide)
                    num = mathf::max(1.5f, num);
                break;
            case tmpl::pink_lizard:
            case tmpl::yellow_lizard:
            case tmpl::black_lizard:
            case tmpl::blue_lizard:
            case tmpl::salamander: num = 0.5f; break;
            case tmpl::green_lizard: num = 0.2f; break;
            case tmpl::white_lizard: num = 0.6f; break;
            case tmpl::red_lizard: num = 0.65f; break;
            case tmpl::overseer: num = 1.f; break;
            case tmpl::vulture:
            case tmpl::king_vulture:
            case tmpl::lantern_mouse: num = 0.25f; break;
            case tmpl::scavenger: num = 1.f; break;
            case tmpl::garbage_worm: num = 0.65f; break;
            case tmpl::cicada_a:
            case tmpl::cicada_b: num = 0.15f; break;
            case tmpl::daddy_long_legs:
            case tmpl::brother_long_legs: num = 0.2f; break;
            default: num = 0.1f; break;
            }
            if (creature::live(state, t) && w.creature_get_dead(t))
                num /= 10.f;
            num *= attraction(state, room_of_any(state, t));
            // world.game.SeededRandom(ID.RandomSeed + testCrit.ID.RandomSeed)
            rw::unity_random r{state.world.overseer_data_get_seed(data(state, x.c)) + int32_t(t.index())};
            return num * mathf::lerp(0.5f, 1.5f, r.value());
        }

        // AbstractRoom.creatures.Count, a random one of them
        dcon::creature_id random_creature(ctx& x) {
            if (x.crits.empty())
                return dcon::creature_id{};
            return x.crits[size_t(x.rng.range(0, int32_t(x.crits.size())))];
        }

        // the room the abstract destination is in (its own without one)
        dcon::room_id destination_room(ctx const& x) {
            return x.ai.destination ? x.ai.destination : x.room.id;
        }
        void set_destination(ctx& x, dcon::room_id room) {
            x.ai.destination_offscreen = 0;
            x.ai.destination = room == x.room.id ? dcon::room_id{} : room;
        }

        // OverseerAbstractAI.Roam (not from the offscreen den): to the
        // neighbouring room that wins the rolls
        void roam(ctx& x, float chance) {
            if (x.ai.destination_offscreen || destination_room(x) != x.room.id)
                return;
            if (!(x.rng.value() < chance))
                return;
            auto const& w = x.state.world;
            float most = 0.f;
            for (auto const o : x.crits)
                most = mathf::max(most, how_interesting(x, o));
            float const here = attraction(x.state, x.room.id);
            float best = min_value;
            dcon::room_id pick;
            w.room_for_each_exit_location(x.room.id, [&](dcon::exit_location_id el) {
                auto const to = w.room_exit_get_target_from_exit_connection(w.exit_location_get_exit(el));
                if (!to || !room_allowed(x.state, x.c, to))
                    return;
                float num = x.rng.value();
                num += most * x.rng.value();
                num *= here;
                num += 20.f * here;
                for (uint32_t m = 0; m < x.ai.last_room_count; ++m)
                    if (x.ai.last_rooms[m] == to) {
                        num -= (20.f - float(m)) * x.rng.value();
                        break;
                    }
                if (num > best) {
                    best = num;
                    pick = to;
                }
            });
            if (pick)
                set_destination(x, pick);
        }

        // Roam from the offscreen den: the best of 20 random rooms of the
        // region (by size, a room with overseers in it less)
        dcon::room_id roam_from_den(sys::game_state const& state, dcon::creature_id c, dcon::region_id region, rw::rng& rng) {
            auto const& w = state.world;
            if (!region)
                return dcon::room_id{};
            auto const range = w.region_range_of_region_membership(region);
            int32_t const n = int32_t(range.second - range.first);
            if (n <= 0)
                return dcon::room_id{};
            float best = min_value;
            dcon::room_id pick;
            for (int i = 0; i < 20; ++i) {
                auto const r = w.region_membership_get_room(*(range.first + rng.range(0, n)));
                if (!r || w.room_get_is_shelter(r) || w.room_get_is_gate(r) || attraction_type(state, r) == creature::attraction::forbidden)
                    continue;
                float num = size_dependent_attraction(state, r) * mathf::lerp(0.9f, 1.1f, rng.value());
                if (attraction_type(state, r) == creature::attraction::avoid)
                    num *= 0.1f;
                rooms::for_each_creature(state, r, creature::type::overseer, [&](dcon::creature_id o) {
                    if (o != c)
                        num *= 0.5f;
                });
                if (num > best) {
                    best = num;
                    pick = r;
                }
            }
            return pick;
        }

        // RelevantPlayer: the first slugcat (not an NPC). (Co-op's first
        // alive one instead of a dead first isn't looked for: the others are
        // other rooms' while the rooms update)
        dcon::creature_id relevant_player(sys::game_state const& state) {
            auto const& w = state.world;
            dcon::creature_id first;
            w.for_each_player([&](dcon::player_id p) {
                if (!first && !npc::is_npc(state, p))
                    first = w.player_get_creature(p);
            });
            return first;
        }

        // PlayerGuideUpdate (likesPlayer 0, handHolding 0.5: no save state)
        void guide_update(ctx& x) {
            auto& ai = x.ai;
            ai.guide_counter -= 1;
            if (ai.guide_counter < 1) {
                ai.go_to_player = ai.go_to_player ? 0 : 1;
                float num = mathf::inverse_lerp(-0.5f, 1.f, 0.f);
                num = mathf::pow(num, 0.15f) * mathf::lerp(0.5f, 1.f, 0.5f);
                if (ai.go_to_player)
                    ai.guide_counter += int32_t(mathf::lerp(200.f, mathf::lerp(400.f, 1200.f, num), x.rng.value()));
                else
                    ai.guide_counter += int32_t(mathf::lerp(800.f, mathf::lerp(4400.f, 2200.f, num), x.rng.value()));
            }
        }

        void zip_out_of_room(ctx& x);

        // OverseerAbstractAI.AbstractBehavior(1) for a realized overseer
        void abstract_behavior(ctx& x) {
            auto& ai = x.ai;
            auto& rng = x.rng;
            auto const& w = x.state.world;
            if (ai.target && !w.creature_is_valid(ai.target))
                ai.target = dcon::creature_id{};
            if (ai.last_target && !w.creature_is_valid(ai.last_target))
                ai.last_target = dcon::creature_id{};
            if (x.p.guide)
                guide_update(x);
            dcon::creature_id const player = x.p.guide ? relevant_player(x.state) : dcon::creature_id{};
            if (x.p.guide && ai.go_to_player && player) {
                ai.target = player;
            } else {
                if (x.p.guide && ai.target && creature::is_slugcat(w, ai.target)) {
                    if (destination_room(x) == room_of_any(x.state, ai.target))
                        roam(x, 1.f);
                    ai.last_target = ai.target;
                    ai.target = dcon::creature_id{};
                }
                if (x.crits.size() > 1 && rng.value() < 0.2f) {
                    auto const o = random_creature(x);
                    if (o && !is_overseer(x.state, o) && (!creature::is_slugcat(w, o) || !x.p.guide) && o != ai.last_target
                    && how_interesting(x, o) > how_interesting(x, ai.target)) {
                        ai.target = o;
                        ai.target_counter = int32_t(mathf::lerp(100.f, 2000.f, rng.value()) * how_interesting(x, o));
                    }
                }
                if (ai.target || how_interesting(x, ai.target) <= 0.f) {
                    ai.target_counter -= 1;
                    if (ai.target_counter < 1) {
                        ai.last_target = ai.target;
                        ai.target = dcon::creature_id{};
                    }
                }
            }
            // lastRooms
            if (ai.last_room_count == 0 || ai.last_rooms[0] != x.room.id) {
                for (uint32_t k = remembered - 1; k > 0; --k)
                    ai.last_rooms[k] = ai.last_rooms[k - 1];
                ai.last_rooms[0] = x.room.id;
                if (ai.last_room_count < remembered)
                    ++ai.last_room_count;
            }
            if (rain_near(x.state, x.room.id) && !ignores_cycle(x.state, x.c, x.room.id)) {
                // to the offscreen den
                if (rng.value() < 0.008333334f) {
                    ai.destination = dcon::room_id{};
                    ai.destination_offscreen = 1;
                }
            } else if (auto const tr = room_of_any(x.state, ai.target); ai.target && tr && room_allowed(x.state, x.c, tr)) {
                set_destination(x, tr);
            } else {
                roam(x, 1.f / mathf::lerp(20.f, 2000.f, attraction(x.state, x.room.id)));
            }
            // somewhere else to be: out it zips
            if (ai.destination_offscreen || destination_room(x) != x.room.id) {
                float const a = ai.destination_offscreen ? creature::attraction_value(creature::attraction::neutral) : attraction(x.state, ai.destination);
                if (rng.value() < 1.f / mathf::lerp(200.f, 10.f, mathf::pow(a, 0.4f)))
                    if (x.p.mode == mode::watching || x.p.mode == mode::projecting)
                        zip_out_of_room(x);
            }
        }

        //
        // OverseerAI
        //
        // RealizedCreatureInterest
        float interest(ctx const& x, dcon::creature_id t) {
            auto const& state = x.state;
            if (!t || !creature::here(state, t, x.room.id))
                return min_value;
            float num = how_interesting(x, t) + 0.05f;
            vec2 const my = state.world.body_chunk_get_pos(chunk(state, x.c, 0));
            vec2 const dp = creature::main_pos(state, t);
            if (custom::dist_less(dp, my, 900.f) && x.room.visual_contact(my, dp)) {
                auto const k = creature::chunk_of(state, t, creature::main_chunk_of(state, t));
                num *= custom::lerp_map(rw::distance(state.world.body_chunk_get_last_pos(k), state.world.body_chunk_get_pos(k)), 2.f, 14.f, 1.f, 3.f);
            }
            return num * custom::lerp_map(rw::distance(x.b.root_pos, dp), 20.f, 600.f, 4.f, 1.f, 0.2f);
        }

        // LensUpdate: bringUpLens rises while the creature it studies stays put
        void lens_update(ctx& x, dcon::creature_id t) {
            auto& ai = x.ai;
            auto const& w = x.state.world;
            auto const k = creature::chunk_of(x.state, t, creature::main_chunk_of(x.state, t));
            vec2 const mp = w.body_chunk_get_pos(k), lp = w.body_chunk_get_last_pos(k);
            ai.target_stationary = mathf::clamp(ai.target_stationary + custom::lerp_map(rw::distance(lp, mp), 1.f, 12.f, 0.0055555557f, -0.0125f), 0.f, 1.f);
            if (!custom::dist_less(mp, ai.last_target_pos, 200.f)) {
                ai.last_target_pos = mp;
                ai.target_stationary = mathf::max(0.f, ai.target_stationary - 0.25f);
            } else {
                ai.last_target_pos = custom::move_towards(ai.last_target_pos, mp, 2.f);
                ai.target_stationary = mathf::max(0.f, ai.target_stationary - mathf::inverse_lerp(0.f, 200.f, rw::distance(ai.last_target_pos, mp)) * 0.1f);
            }
            float const num = interest(x, t);
            float num2 = (mathf::pow(ai.target_stationary, mathf::lerp(2.f, 0.1f, num))
                + mathf::inverse_lerp(40.f, mathf::lerp(400.f, 70.f, num), float(x.p.stationary_counter)) + mathf::lerp(ai.bring_up_lens, 0.5f, 0.2f)
                + mathf::lerp(num, ai.random_bring_up_lens_bonus, 0.5f)) / 4.f;
            if (x.p.mode == mode::zipping && !ai.destination_offscreen && destination_room(x) == x.room.id) {
                ai.bring_up_lens -= 0.025f;
            } else {
                if (x.p.mode == mode::sitting_in_wall)
                    num2 *= 0.49f;
                ai.bring_up_lens = mathf::clamp(ai.bring_up_lens + mathf::lerp(-1.f, 1.f, num2) / 40.f, 0.f, 1.f);
            }
            if (x.rng.value() < 0.00625f)
                ai.random_bring_up_lens_bonus = x.rng.value();
        }

        // LookAtAirPosScore (AItile.visibility, which isn't kept here, as
        // the open space around the spot: its terrain proximity)
        float look_at_air_score(ctx const& x, vec2 test) {
            if (x.room.get_tile(test).solid())
                return min_value;
            ivec2 const t = tile_of(test);
            float num = float(std::max(0, x.proximity(t))) * 40.f;
            for (uint32_t i = 0; i < x.ai.last_look_at_air_count; ++i)
                num -= mathf::inverse_lerp(300.f, 10.f, rw::distance(test, x.ai.last_look_at_air[i])) * 400.f;
            if (t.x < 0 || t.y < 0 || t.x >= x.room.width || t.y > x.room.height)
                num -= 1000.f;
            return num;
        }

        // DoIWantToTalkToThisOverSeer
        bool want_to_talk(ctx const& x, dcon::creature_id other) {
            return x.p.conversation_delay == 0 && pl(x.state, other).conversation_delay == 0 && x.b.last_conversation_partner != other;
        }

        // HoverScoreOfTile: lower is better
        float hover_score(ctx const& x, ivec2 t) {
            auto const& state = x.state;
            auto const& w = state.world;
            if (!x.room.inside(t.x, t.y) || x.solid(t))
                return max_value;
            if (x.proximity(t) > int32_t(6.f * x.p.size))
                return max_value;
            vec2 const tp = mid(t);
            float num = std::fabs(300.f - rw::distance(tp, x.ai.look_at));
            if (custom::dist_less(tp, x.ai.look_at, 1000.f) && x.room.visual_contact(tp, mid(tile_of(x.ai.look_at))))
                num -= 100.f;
            for (uint32_t i = 0; i < x.ai.avoid_count; ++i) {
                float const d = float_dist(x.ai.avoid[i], t);
                if (d < 15.f)
                    num += custom::lerp_map(d, 10.f, 15.f, 50.f, 0.f);
            }
            for (auto const o : x.crits) {
                if (!is_overseer(state, o)) {
                    tmpl const tt = creature::template_of(state, o);
                    vec2 const dp = creature::main_pos(state, o);
                    if (!creature::small_creature(tt) && !w.creature_get_dead(o) && custom::dist_less(tp, dp, scared_distance + 10.f))
                        return max_value;
                    float const size = creature::body_size(tt);
                    num += custom::lerp_map(rw::distance(tp, dp), 40.f, mathf::clamp(size * 600.f, 60.f, 800.f), size * 100.f, 0.f);
                } else if (o != x.c && !want_to_talk(x, o)) {
                    num += custom::lerp_map(float_dist(pl(state, o).hover_tile, t), 0.f, 3.f, 250.f, 0.f);
                    num += custom::lerp_map(float_dist(bs(state, o).next_hover_tile, t), 0.f, 3.f, 250.f, 0.f);
                }
            }
            if (pathing::narrow_space(x.room, t))
                num += 200.f;
            num -= float(x.proximity(t)) * 10.f;
            if (t.y <= x.room.default_water_level)
                num += 10000.f;
            return num;
        }

        // FindRootTileForHoverPos: down the terrain proximity to the rock
        // (none when another overseer's root is there)
        std::optional<ivec2> find_root_for_hover(ctx const& x, ivec2 t) {
            ivec2 cur = t;
            for (int i = 0; i < 15; ++i) {
                int32_t num = INT_MAX;
                ivec2 next = cur;
                for (auto const d : custom::eight_directions) {
                    int32_t const pr = x.proximity(cur + d);
                    if (pr < num) {
                        next = cur + d;
                        num = pr;
                    }
                }
                if (cur == next)
                    return std::nullopt;
                cur = next;
                if (x.solid(cur)) {
                    for (auto const o : x.crits)
                        if (o != x.c && is_overseer(x.state, o) && pl(x.state, o).root_tile == cur)
                            return std::nullopt;
                    return cur;
                }
            }
            return std::nullopt;
        }

        // UpdateTempHoverPosition: a better spot nearby to hover at, or
        // further off: zipping over to another root
        void update_temp_hover(ctx& x) {
            auto& ai = x.ai;
            auto& p = x.p;
            vec2 const r = custom::rnv(x.rng);
            float const v = x.rng.value();
            ivec2 const t = tile_of(mid(p.hover_tile) + r * v * 400.f);
            if (auto const root = find_root_for_hover(x, t); root && cell_reached(x, *root) && hover_score(x, t) < hover_score(x, ai.temp_hover_tile))
                ai.temp_hover_tile = t;
            float num = 200.f;
            if (p.mode == mode::sitting_in_wall)
                num = 10.f;
            if (float_dist(ai.temp_hover_tile, p.root_tile) <= 6.f * p.size && hover_score(x, ai.temp_hover_tile) < hover_score(x, p.hover_tile)) {
                p.hover_tile = ai.temp_hover_tile;
                return;
            }
            if ((p.extended > 0.5f || p.mode == mode::sitting_in_wall) && p.mode != mode::withdrawing && float_dist(ai.temp_hover_tile, p.hover_tile) > 3.f * p.size
            && hover_score(x, ai.temp_hover_tile) + num < hover_score(x, p.hover_tile)) {
                auto const root = find_root_for_hover(x, ai.temp_hover_tile);
                if (root && cell_reached(x, *root)) {
                    if (ai.avoid_count < remembered) {
                        ai.avoid[ai.avoid_count++] = p.hover_tile;
                    } else {
                        for (uint32_t k = 0; k + 1 < remembered; ++k)
                            ai.avoid[k] = ai.avoid[k + 1];
                        ai.avoid[remembered - 1] = p.hover_tile;
                    }
                    find_zip_path(x, *root, ai.temp_hover_tile);
                    if (p.mode == mode::sitting_in_wall) {
                        switch_modes(x.state, x.c, x.room, mode::zipping);
                        return;
                    }
                    p.after_withdraw_mode = mode::zipping;
                    switch_modes(x.state, x.c, x.room, mode::withdrawing);
                }
            }
        }

        // Overseer.ZipOutOfRoom: towards the destination, out through the rock
        void zip_out_of_room(ctx& x) {
            dcon::room_id const dest = x.ai.destination_offscreen ? dcon::room_id{} : x.ai.destination;
            if (!x.ai.destination_offscreen && (!dest || dest == x.room.id))
                return;
            inter_room_zip_path(x, other_room_point(x, dest), false);
            x.p.leave_room_on_zip = 1;
            x.p.after_withdraw_mode = mode::zipping;
            switch_modes(x.state, x.c, x.room, mode::withdrawing);
        }

        // Overseer.ZipIntoRoom: in through the rock from where it came from
        void zip_into_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, dcon::room_id from) {
            auto const view = world::get_room_view(state, room);
            ctx x{state, c, view};
            inter_room_zip_path(x, other_room_point(x, from), true);
            for (auto& z : x.b.zip_progs)
                z = 0.f;
            x.p.extended = 0.f;
            x.p.last_extended = 0.f;
            switch_modes(state, c, view, mode::zipping);
        }

        // ShortcutHandler.CreatureTeleportOutOfRoom and the TeleportationVessel
        // arriving (Overseer.TeleportingIntoRoom): the transaction moving it
        void teleport(sys::game_state& state, dcon::creature_id c, dcon::room_id to, dcon::room_id from) {
            auto& w = state.world;
            if (!w.creature_is_valid(c) || !is_overseer(state, c) || w.creature_get_dead(c) || !to)
                return;
            auto& ai = ais(state, c);
            auto& p = pl(state, c);
            w.physical_object_set_room_from_object_location(body(state, c), to);
            dens::changed_rooms(state, c, from, to, false);
            ai.destination = dcon::room_id{};
            ai.destination_offscreen = 0;
            p.offscreen = 0;
            p.leave_room_on_zip = 0;
            vec2 const at{float(w.room_get_width(to)) * 10.f, float(w.room_get_height(to)) * 10.f};
            // (PlaceInRoom, then ZipIntoRoom from the room it left)
            creature::placed_in_room(state, c, to, at);
        }

        // the zip is done and the room left behind
        void leave_room(ctx& x) {
            auto& p = x.p;
            auto& ai = x.ai;
            if (ai.destination_offscreen) {
                // the offscreen den: hidden in the wall until the next cycle
                p.offscreen = 1;
                p.mode = mode::sitting_in_wall;
                p.extended = 0.f;
                p.last_extended = 0.f;
                ai.destination_offscreen = 0;
                ai.destination = dcon::room_id{};
                ai.target = dcon::creature_id{};
                return;
            }
            dcon::room_id const to = ai.destination;
            if (!to || to == x.room.id) {
                switch_modes(x.state, x.c, x.room, mode::emerging);
                return;
            }
            dcon::creature_id const c = x.c;
            dcon::room_id const from = x.room.id;
            x.state.defer(from, [c, to, from](sys::game_state& st) { teleport(st, c, to, from); });
        }

        // OverseerAI.Update
        void ai_update(ctx& x) {
            auto& state = x.state;
            auto const& w = state.world;
            auto& ai = x.ai;
            auto& p = x.p;
            auto& rng = x.rng;
            vec2 const my = x.pos();
            ai.slow_look_at = rw::lerp(custom::move_towards(ai.slow_look_at, ai.look_at, 60.f), ai.look_at, 0.02f);
            if (rng.value() < 0.0125f) {
                float const a = mathf::pow(rng.value(), 3.f) * 2.f;
                ai.casual_interest_bonus = a * (rng.value() < 0.5f ? -1.f : 1.f);
            }
            if (ai.casual_interest && (!w.creature_is_valid(ai.casual_interest) || !creature::here(state, ai.casual_interest, x.room.id)))
                ai.casual_interest = dcon::creature_id{};
            dcon::creature_id const casual = ai.casual_interest;
            dcon::creature_id const target = ai.target && w.creature_is_valid(ai.target) && creature::here(state, ai.target, x.room.id) ? ai.target : dcon::creature_id{};
            bool flag = bool(casual);
            if (flag)
                flag = interest(x, casual) + ai.casual_interest_bonus > (target ? interest(x, target) : 0.f);
            if (ai.weapon_ticks > 0) {
                // the weapon it notices flying, while it flies
                ai.look_at = ai.weapon_pos;
                ai.weapon_pos += ai.weapon_vel;
                --ai.weapon_ticks;
            } else if (flag) {
                ai.look_at = creature::main_pos(state, casual);
                lens_update(x, casual);
            } else if (target) {
                ai.look_at = creature::main_pos(state, target);
                lens_update(x, target);
            } else {
                ai.target_stationary = mathf::max(0.f, ai.target_stationary - 0.008333334f);
                vec2 test;
                if (rng.value() < 0.1f) {
                    vec2 const r = custom::rnv(rng);
                    test = my + r * rng.value() * 600.f;
                } else {
                    vec2 const r = custom::rnv(rng);
                    test = ai.look_at + r * mathf::pow(rng.value(), 3.f) * 600.f;
                }
                if (look_at_air_score(x, test) > look_at_air_score(x, ai.look_at)) {
                    ai.look_at = test;
                    ai.look_at_same_air_pos_counter = rng.range(30, 130);
                } else if (--ai.look_at_same_air_pos_counter < 1) {
                    for (uint32_t k = remembered - 1; k > 0; --k)
                        ai.last_look_at_air[k] = ai.last_look_at_air[k - 1];
                    ai.last_look_at_air[0] = ai.look_at;
                    if (ai.last_look_at_air_count < remembered)
                        ++ai.last_look_at_air_count;
                    ai.look_at_same_air_pos_counter = rng.range(30, 130);
                }
            }
            if (rng.value() < 0.025f) {
                vec2 const r = custom::rnv(rng);
                ai.look_at_add = r * rng.value();
            }
            update_zip_matrix(x);
            update_temp_hover(x);
            if (p.mode == mode::watching || p.mode == mode::projecting) {
                auto const o = random_creature(x);
                if (o && !is_overseer(state, o)) {
                    tmpl const tt = creature::template_of(state, o);
                    vec2 const dp = creature::main_pos(state, o);
                    if (!creature::small_creature(tt) && !w.creature_get_dead(o) && custom::dist_less(x.b.root_pos, dp, scared_distance)) {
                        // too close: into the wall
                        ai.casual_interest = o;
                        p.after_withdraw_mode = mode::sitting_in_wall;
                        switch_modes(state, x.c, x.room, mode::withdrawing);
                    } else if (ai.target != o && (!ai.casual_interest || interest(x, o) > interest(x, ai.casual_interest) + 0.1f || ai.target == ai.casual_interest)
                    && x.room.visual_contact(my, dp)) {
                        ai.casual_interest = o;
                    }
                } else if (o && p.mode != mode::projecting && p.conversation_delay == 0) {
                    // another overseer close by (or itself, like vanilla's)
                    auto& op = pl(state, o);
                    auto& ob = bs(state, o);
                    if (custom::dist_less(x.b.root_pos, ob.root_pos, 70.f * p.size + 70.f + op.size) && op.mode == mode::watching && !ob.conversation_partner
                    && op.conversation_delay == 0 && x.b.last_conversation_partner != o) {
                        x.b.conversation_partner = o;
                        ob.conversation_partner = x.c;
                        switch_modes(state, x.c, x.room, mode::conversing);
                        switch_modes(state, o, x.room, mode::conversing);
                        p.conversation_delay = int16_t(rng.range(30, 190));
                        op.conversation_delay = int16_t(rng.range(30, 190));
                    }
                }
            } else if (p.mode == mode::sitting_in_wall) {
                bool near = false;
                for (auto const o : x.crits) {
                    if (is_overseer(state, o) || creature::small_creature(creature::template_of(state, o)) || w.creature_get_dead(o))
                        continue;
                    if (custom::dist_less(x.b.root_pos, creature::main_pos(state, o), 200.f)) {
                        near = true;
                        break;
                    }
                }
                if (!near)
                    switch_modes(state, x.c, x.room, mode::emerging);
            } else if (p.mode == mode::conversing) {
                auto const partner = x.b.conversation_partner;
                if (!partner || !w.creature_is_valid(partner) || !is_overseer(state, partner) || creature::room_of(state, partner) != x.room.id
                || pl(state, partner).mode != mode::conversing || bs(state, partner).conversation_partner != x.c)
                    switch_modes(state, x.c, x.room, mode::watching);
                else
                    ai.look_at = w.body_chunk_get_pos(chunk(state, partner, 0));
            }
            // (no tutorial; AbstractBehavior(1) twice a tick, as vanilla calls it)
            abstract_behavior(x);
            abstract_behavior(x);
        }

        // waiting the rain out in the offscreen den, hidden in its wall:
        // AbstractBehavior's Roam out of the den once the rain is far off
        void offscreen_update(ctx& x) {
            auto& p = x.p;
            auto& ai = x.ai;
            p.extended = 0.f;
            p.last_extended = 0.f;
            p.mode = mode::sitting_in_wall;
            for (int k = 0; k < 2; ++k) {
                if (rain_near(x.state, x.room.id) && !ignores_cycle(x.state, x.c, x.room.id))
                    continue;
                if (!ai.destination && x.rng.value() < 1.f / mathf::lerp(20.f, 2000.f, creature::attraction_value(creature::attraction::neutral)))
                    ai.destination = roam_from_den(x.state, x.c, region_of(x.state, x.room.id), x.rng);
                if (ai.destination && x.rng.value() < 1.f / mathf::lerp(200.f, 10.f, mathf::pow(attraction(x.state, ai.destination), 0.4f))) {
                    dcon::creature_id const c = x.c;
                    dcon::room_id const to = ai.destination;
                    // (it comes out of the den: no room it came from)
                    ai.last_room_count = 0;
                    x.state.defer(x.room.id, [c, to](sys::game_state& st) { teleport(st, c, to, dcon::room_id{}); });
                    return;
                }
            }
        }

        // Creature.Update's HealthState: hurt, it may die, else heals
        void health_update(sys::game_state& state, dcon::creature_id c, rw::rng& rng) {
            auto& w = state.world;
            if (w.creature_get_dead(c))
                return;
            float& health = w.creature_get_health(c);
            if (health < 0.f && rng.value() < -health && rng.value() < 1.f / float(creature::consious(state, c) ? 80 : 800))
                creature::die(state, c);
            if (rng.value() < 0.033333335f && (rng.value() * 0.2f > health || rng.value() < -health))
                creature::stun(state, c, rng.range(1, rng.range(1, 27 - custom::int_clamp(int32_t(20.f * health), 0, 10))));
            if (health > 0.f && health < 1.f && rng.value() < 0.01f)
                health = mathf::min(1.f, health + 1.f / mathf::lerp(550.f, 70.f, health));
        }
    }

    vec2 cosmetic_look_at(sys::game_state const& state, dcon::creature_id c) noexcept {
        auto const& ai = ais(state, c);
        vec2 const my = state.world.body_chunk_get_pos(chunk(state, c, 0));
        return ai.slow_look_at + ai.look_at_add * rw::distance(my, ai.look_at) * 0.2f;
    }

    dcon::creature_id create(sys::game_state& state, dcon::room_id room, vec2 at) {
        auto& w = state.world;
        auto const c = creature::make(state, creature::type::overseer);
        auto const d = data(state, c);
        rw::rng rng{state.random.next() | 1u};
        int32_t seed = int32_t(rng.next() & 0x7FFFFFFFu);
        if (state.spawn_seed >= 0)
            seed = state.spawn_seed;
        physics::object_desc desc;
        desc.gravity = 0.f;
        desc.air_friction = 0.8f;
        desc.bounce = 0.f;
        desc.surface_friction = 1.f;
        desc.water_friction = 1.f;
        desc.buoyancy = 0.f;
        auto const o = physics::create_object(state, room, desc);
        // one chunk (3, 0.4), through the terrain
        auto const k = physics::add_chunk(state, o, at, 3.f, 0.4f);
        physics::set_go_through_floors(state, o, true);
        w.body_chunk_set_collide_with_terrain(k, 0);
        w.try_create_creature_body(c, o);
        auto& p = pl(state, c);
        p = creature::overseer_payload{};
        p.data = uint16_t(d.index());
        {
            // the size, rolled from the ID's seed
            rw::unity_random r{seed};
            p.size = mathf::lerp(0.5f, 1.f, r.value());
        }
        p.extended = 1.f;
        p.last_extended = 1.f;
        p.mode = mode::watching;
        p.after_withdraw_mode = mode::watching;
        p.owner_iterator = roll_owner(state, room, rng);
        w.creature_set_random(c, rng);
        w.creature_set_graphics_random(c, rw::rng{(rng.state ^ 0x0E5EE7u) | 1u});
        w.creature_set_stun(c, 0);
        w.creature_set_dead(c, 0);
        w.creature_set_health(c, 1.f);
        w.creature_set_grasps(c, creature::grasp_pair{});
        w.creature_set_behavior(c, uint8_t(mode::watching));
        w.creature_set_current_utility(c, 0.f);
        w.creature_set_destination(c, tile_of(at));
        w.overseer_data_set_seed(d, seed);
        body_state b;
        b.path[0] = tile_of(at);
        b.path_count = 1;
        w.overseer_data_set_body(d, b);
        ai_state ai;
        ai.look_at = at;
        ai.slow_look_at = at;
        ai.random_bring_up_lens_bonus = w.creature_get_random(c).value();
        ai.guide_counter = w.creature_get_random(c).range(200, 500);
        w.overseer_data_set_ai(d, ai);
        w.overseer_data_set_graphics(d, graphics_state{});
        overseer_graphics::setup(state, c, seed);
        place_near(state, c, world::get_room_view(state, room), tile_of(at));
        return c;
    }

    void destroy(sys::game_state& state, dcon::creature_id c) {
        creature::release_target(state, c);
        creature::erase(state, c);
    }

    void update(sys::game_state& state, dcon::creature_id c, room_view const& room) {
        auto& w = state.world;
        ctx x{state, c, room};
        auto& p = x.p;
        auto& b = x.b;
        auto& ai = x.ai;
        auto& rng = x.rng;
        if (p.offscreen) {
            offscreen_update(x);
            return;
        }
        x.fill_creatures();
        health_update(state, c, rng);
        // Creature.Update + PhysicalObject.Update (no terrain collision)
        creature::stun_countdown(state, c);
        physics::update_object(state, room, body(state, c), nullptr, nullptr);
        b.last_root_pos = b.root_pos;
        b.last_root_dir = b.root_dir;
        p.last_extended = p.extended;
        p.last_dying = p.dying;
        vec2& pos = x.pos();
        vec2& vel = x.vel();
        if (x.dead()) {
            {
                vec2 const r = custom::rnv(rng);
                vel += r * rng.value() * 4.f * p.dying;
            }
            {
                vec2 const r = custom::rnv(rng);
                pos += r * rng.value() * 4.f * p.dying;
            }
            if (p.last_dying == 0.f) {
                // (no Overseer_Death sound; the ShockWave as a flash, no
                // OverseerCarcass left behind)
                particles::spawn(state, room.id, particles::kind::flash, pos, vec2{0.f, 0.f}, 120.f * p.size, 0.175f, overseer_graphics::main_color(state, c));
            }
            p.extended = 1.f;
            p.dying = mathf::min(1.f, p.dying + 0.16666667f);
            if (p.dying >= 1.f && !p.destroy_queued) {
                p.destroy_queued = 1;
                state.defer(room.id, [c](sys::game_state& st) {
                    if (st.world.creature_is_valid(c) && is_overseer(st, c))
                        creature::destroy(st, c);
                });
            }
        }
        if (p.conversation_delay > 0)
            --p.conversation_delay;
        ++p.stationary_counter;
        {
            vec2 const hover = mid(p.hover_tile);
            vec2 const r0{float(p.root_tile.x) * 20.f, float(p.root_tile.y) * 20.f};
            vec2 const restricted{mathf::clamp(hover.x, r0.x, r0.x + 20.f), mathf::clamp(hover.y, r0.y, r0.y + 20.f)};
            b.root_pos = rw::lerp(b.root_pos, restricted, 0.2f);
            float const num = (p.mode == mode::conversing ? 100.f : 50.f) * p.size;
            if (!custom::dist_less(hover, pos, num)) {
                vec2 const a = custom::dir_vec(hover, pos) * (num - rw::distance(hover, pos));
                vel += a * 0.15f;
                pos += a * 0.15f;
            }
            vel += custom::dir_vec(pos, hover) * mathf::inverse_lerp(0.f, num, rw::distance(hover, pos)) * 0.8f * p.size;
            if (!custom::dist_less(b.root_pos, pos, num)) {
                vec2 const a = custom::dir_vec(b.root_pos, pos) * (num - rw::distance(b.root_pos, pos));
                vel += a * 0.15f;
                pos += a * 0.15f;
            }
            vec2 a3{0.f, 0.f};
            for (auto const d : custom::eight_directions)
                if (!x.solid(p.root_tile + d))
                    a3 += rw::normalized(rw::to_vec2(d));
            vec2 const toward = custom::dir_vec(b.root_pos, (hover + pos) / 2.f);
            if (rw::magnitude(a3) > 0.f)
                b.root_dir = rw::slerp(b.root_dir, rw::normalized(toward + a3 * 5.f), 0.2f);
            else
                b.root_dir = rw::slerp(b.root_dir, toward, 0.2f);
        }
        auto const partner = b.conversation_partner;
        if (p.mode == mode::conversing && partner && w.creature_is_valid(partner) && is_overseer(state, partner) && creature::room_of(state, partner) == room.id) {
            vec2 const pp = w.body_chunk_get_pos(chunk(state, partner, 0));
            vec2 v = (pos + pp) / 2.f;
            v += custom::dir_vec(v, pos) * 10.f;
            vel *= 0.6f;
            vel += (v - pos) * 0.1f;
        } else {
            vel += custom::dir_vec(pos, ai.look_at) * custom::lerp_map(rw::distance(pos, ai.look_at), 30.f, 150.f, -0.8f, 0.8f) * p.size;
        }
        ai_update(x);
        if (p.offscreen)
            return;
        // (no holograms: never Projecting)
        if (p.mode == mode::withdrawing) {
            p.extended = mathf::max(0.f, p.extended - 0.25f);
            if (p.extended <= 0.f)
                switch_modes(state, c, room, p.after_withdraw_mode);
        } else if (p.mode == mode::zipping) {
            if (b.zip_progs[1] <= 0.5f && b.zip_progs[0] > 0.5f)
                p.zip_mesh_direction = 1;
            if (b.zip_progs[0] >= 1.f) {
                if (p.leave_room_on_zip) {
                    p.leave_room_on_zip = 0;
                    leave_room(x);
                } else {
                    switch_modes(state, c, room, mode::emerging);
                }
            }
            p.stationary_counter = 0;
        } else if (p.mode == mode::emerging) {
            p.extended = mathf::min(1.f, p.extended + 0.25f);
            vec2 const hover = mid(p.hover_tile);
            x.vel() += (hover + custom::dir_vec(hover, ai.look_at) * 30.f - x.pos()) * 0.05f;
            if (p.extended >= 1.f)
                switch_modes(state, c, room, mode::watching);
        } else if (p.mode == mode::conversing && p.conversation_delay == 0) {
            if (partner && w.creature_is_valid(partner) && is_overseer(state, partner) && creature::room_of(state, partner) == room.id)
                switch_modes(state, partner, room, mode::watching);
            switch_modes(state, c, room, mode::watching);
        }
        if (p.mode == mode::zipping || p.mode == mode::emerging) {
            for (uint32_t j = zip_prog_count - 1; j >= 1; --j)
                b.zip_progs[j] = b.zip_progs[j - 1];
            b.zip_progs[0] = mathf::min(1.f, b.zip_progs[0] + p.zip_prog_speed);
        }
        w.creature_set_behavior(c, uint8_t(p.mode));
    }

    bool collides(sys::game_state const&, dcon::creature_id, dcon::creature_id) {
        // collision layer 0, the creatures' is 1
        return false;
    }

    void collide(sys::game_state&, dcon::creature_id, room_view const&, uint32_t, dcon::creature_id, uint32_t) {}

    void violence(sys::game_state& state, dcon::creature_id c, uint32_t, vec2 momentum, float damage, float stun_bonus, creature::damage_type) {
        auto& w = state.world;
        auto& p = pl(state, c);
        if (p.offscreen)
            return;
        auto& rng = w.creature_get_random(c);
        // Creature.Violence (HealthState); no damage resistances by type
        auto const k = chunk(state, c, 0);
        w.body_chunk_get_vel(k) += rw::clamp_magnitude(momentum / w.body_chunk_get_mass(k), 10.f);
        float const dmg = damage / base_damage_resistance;
        float st = (damage * 30.f + stun_bonus) / base_stun_resistance;
        float& health = w.creature_get_health(c);
        st *= 1.5f + mathf::inverse_lerp(0.5f, 0.f, health) * rng.value();
        creature::stun(state, c, int32_t(st));
        health -= dmg;
        if (rng.value() < -health || health < -1.f || (health < 0.f && rng.value() < 0.33f))
            creature::die(state, c);
        if (dmg >= instant_death_damage_limit)
            creature::die(state, c);
        // HitByWeapon: the guide hit while coming to the slugcat stays a
        // quarter as long (taken as any hit; the thrower isn't known here)
        if (p.guide) {
            auto& ai = ais(state, c);
            if (ai.go_to_player)
                ai.guide_counter /= 4;
        }
    }

    void release(sys::game_state&, dcon::creature_id, dcon::creature_id) {
        // it never holds anything
    }

    void died(sys::game_state& state, dcon::creature_id c) {
        // (no hologram to take down; the player guide isn't respawned)
        auto& p = pl(state, c);
        p.leave_room_on_zip = 0;
    }

    void place_in_room(sys::game_state& state, dcon::creature_id c, dcon::room_id room, vec2 at) {
        auto const view = world::get_room_view(state, room);
        place_randomly(state, c, view, tile_of(at));
        auto& ai = ais(state, c);
        // OverseerAI.NewRoom
        ai.last_look_at_air_count = 0;
        ai.avoid_count = 0;
        ai.slow_look_at = ai.look_at;
        ai.casual_interest = dcon::creature_id{};
        ai.weapon_ticks = 0;
        auto& b = bs(state, c);
        b.conversation_partner = dcon::creature_id{};
        // TeleportingIntoRoom: in through the rock from the room it was in
        dcon::room_id const from = ai.last_room_count > 0 && ai.last_rooms[0] != room ? ai.last_rooms[0] : dcon::room_id{};
        pl(state, c).offscreen = 0;
        zip_into_room(state, c, room, from);
    }

    std::string describe(sys::game_state const& state, dcon::creature_id c) {
        static constexpr char const* names[] = {"watching", "withdrawing", "zipping", "emerging", "sitting in wall", "conversing", "projecting"};
        auto const& w = state.world;
        auto const& p = pl(state, c);
        auto const& ai = ais(state, c);
        vec2 const at = w.body_chunk_get_pos(chunk(state, c, 0));
        uint32_t const m = uint32_t(p.mode);
        std::string s = std::string{p.guide ? "player guide overseer" : "overseer"} + " at " + std::to_string(int(at.x)) + "," + std::to_string(int(at.y)) + " mode "
            + (m < 7 ? names[m] : "?") + " root " + std::to_string(p.root_tile.x) + "," + std::to_string(p.root_tile.y);
        if (ai.target && w.creature_is_valid(ai.target))
            s += std::string{" watching a "} + creature::to_string(w.creature_get_type(ai.target));
        if (ai.destination_offscreen)
            s += " leaving for the den";
        else if (ai.destination)
            s += " leaving for " + std::string{state.to_string_view(w.room_get_name(ai.destination))};
        if (p.offscreen)
            s += " in the offscreen den";
        return s;
    }

    bool spawn_spot(room_view const& room, ivec2 tile) {
        if (!room.inside(tile.x, tile.y) || room.get_tile(tile).solid())
            return false;
        for (auto const d : custom::eight_directions)
            if (room.get_tile(tile + d).solid())
                return true;
        return false;
    }

    bool can_be_hit(sys::game_state const& state, dcon::creature_id c) noexcept {
        auto const& p = pl(state, c);
        return !p.offscreen && p.extended > 0.2f;
    }

    bool hidden(sys::game_state const& state, dcon::creature_id c) noexcept {
        return pl(state, c).offscreen != 0;
    }

    void flying_weapon(sys::game_state& state, dcon::creature_id c, vec2 at, vec2 vel) {
        auto& w = state.world;
        auto& p = pl(state, c);
        if (p.offscreen || w.creature_get_dead(c))
            return;
        auto& ai = ais(state, c);
        auto& rng = w.creature_get_random(c);
        vec2 const my = w.body_chunk_get_pos(chunk(state, c, 0));
        auto const room = world::get_room_view(state, creature::room_of(state, c));
        if (!custom::dist_less(my, at, 600.f) || !room.visual_contact(my, at))
            return;
        // flinching into the wall from a weapon coming straight at it
        if (rng.value() < mathf::inverse_lerp(10.f, 40.f, float(p.stationary_counter)) * 0.75f && p.mode == mode::watching
        && custom::dist_less(my, at + rw::normalized(vel) * 100.f, 100.f)) {
            p.after_withdraw_mode = mode::sitting_in_wall;
            switch_modes(state, c, room, mode::withdrawing);
        }
        // lookAtFlyingWeapon: the closest (the one it looks at keeps being told)
        bool const same = ai.weapon_ticks > 0 && custom::dist_less(ai.weapon_pos, at, mathf::max(20.f, rw::magnitude(vel) * 1.5f));
        if (ai.weapon_ticks <= 0 || same || rw::distance(my, at) < rw::distance(my, ai.weapon_pos)) {
            ai.weapon_pos = at;
            ai.weapon_vel = vel;
            ai.weapon_ticks = 3;
        }
    }

    uint32_t spawn_world(sys::game_state& state) {
        using sn = ::player::slugcat_name;
        auto& w = state.world;
        uint32_t made = 0;
        // one overseer into `room`, at a random wall spot
        auto const spawn_one = [&](dcon::room_id room, int32_t seed, bool guide, uint8_t owner) {
            auto const saved = state.random;
            state.random = rw::rng{uint32_t(seed) * 0x9E3779B1u ^ 0x0E5EE7u};
            if (state.random.state == 0)
                state.random.state = 0x2545F491u;
            state.spawn_seed = seed;
            auto const view = world::get_room_view(state, room);
            vec2 const centre{view.pixel_width() * 0.5f, view.pixel_height() * 0.5f};
            auto const c = create(state, room, centre);
            state.spawn_seed = -1;
            state.random = saved;
            if (!c)
                return;
            place_randomly(state, c, view, tile_of(centre));
            if (guide) {
                auto& p = pl(state, c);
                p.guide = 1;
                p.owner_iterator = owner;
                if (state.story_slugcat != sn::gourmand && state.story_slugcat != sn::spear)
                    p.size = 0.6f;
                // (rolled again for the guide's fixed size: the mycelia, the lens)
                overseer_graphics::setup(state, c, seed);
                overseer_graphics::reset_positions(state, c);
            }
            ++made;
        };
        // GeneratePopulation: per region a chance (the cycle number taken as
        // 21: no early cycles here) of overseersMin to overseersMax - 1 of
        // them, each in the room the offscreen den's Roam picks
        w.for_each_region([&](dcon::region_id region) {
            if (w.region_get_is_pseudo(region))
                return;
            auto const name = state.to_string_view(w.region_get_name(region));
            uint32_t h = 2166136261u;
            for (char const ch : name)
                h = (h ^ uint8_t(ch)) * 16777619u;
            rw::rng rng{(h ^ (state.random.state * 0x85EBCA6Bu)) | 1u};
            if (!(name == "UW" || name == "LC" || name == "LM" || rng.value() < w.region_get_overseers_spawn_chance(region)))
                return;
            int32_t const n = rng.range(int32_t(w.region_get_overseers_min(region)), int32_t(w.region_get_overseers_max(region)));
            for (int32_t i = 0; i < n; ++i) {
                auto const room = roam_from_den(state, dcon::creature_id{}, region, rng);
                if (!room)
                    continue;
                spawn_one(room, ++state.next_issued_id, false, 0);
            }
        });
        // OverseerSpawnConditions: the player guide (EntityID(-1, 5)), one for
        // the world, in SU_C04 like the Outskirts' (Gourmand's in OE); none
        // for Hunter, Saint, the Watcher, and Artificer and Rivulet (their
        // conditions hang on the save state)
        auto const s = state.story_slugcat;
        uint8_t owner = 1;
        bool guide = false;
        dcon::room_id room;
        if (s == sn::white || s == sn::yellow || s == sn::inv || s == sn::spear) {
            guide = true;
            owner = s == sn::spear ? 3 : 1;
            room = state.find_room("SU_C04");
        } else if (s == sn::gourmand) {
            guide = true;
            owner = 2;
            w.for_each_region([&](dcon::region_id region) {
                if (!room && state.to_string_view(w.region_get_name(region)) == "OE") {
                    rw::rng rng{0x6E0E5Eu};
                    room = roam_from_den(state, dcon::creature_id{}, region, rng);
                }
            });
        }
        if (guide && room)
            spawn_one(room, 5, true, owner);
        return made;
    }
}
