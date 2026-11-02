#include <optional>

#include "borders.hpp"

namespace borders {
    using rw::ivec2;
    using world::room_view;

    bool valid_tile(room_view const& room, ivec2 pos, kind type) noexcept {
        // Room.HasAnySolid (no TerrainManager obstructions here)
        auto const t = room.get_tile(pos);
        if (t.solid())
            return false;
        switch (type) {
        case kind::side:
            if (pos.x > 0 && pos.x < room.width - 1)
                return false;
            if (t.water != 0)
                return false;
            break;
        case kind::sky:
            if (pos.y < room.height - 1)
                return false;
            break;
        case kind::sea:
            if (t.water == 0)
                return false;
            break;
        }
        return true;
    }

    std::vector<exit> map(sys::game_state const& state, room_view const& room) {
        std::vector<exit> list;
        if (!room.id || room.width <= 0 || room.height <= 0 || state.world.room_get_is_shelter(room.id) || state.world.room_get_is_gate(room.id))
            return list;
        std::vector<ivec2> run;
        // (one count over the three walks, like vanilla's)
        int32_t i = 0;
        for (int32_t j = 0; j < 3; ++j) {
            kind const type = j == 0 ? kind::side : j == 1 ? kind::sky : kind::sea;
            if (j == 2 && !room.water)
                break;
            // clockwise from the bottom left corner, up the left side
            ivec2 pos{0, 0};
            ivec2 dir{0, 1};
            run.clear();
            bool flag = false;
            std::optional<ivec2> start;
            while (i < 100000) {
                ++i;
                pos = pos + dir;
                bool flag2 = false;
                if (valid_tile(room, pos, type)) {
                    if (flag) {
                        if (!start) {
                            start = pos;
                            flag2 = true;
                        } else if (pos == *start) {
                            break;
                        }
                        run.clear();
                    }
                    if (start)
                        run.push_back(pos);
                } else if (!flag && start && !run.empty()) {
                    list.push_back(exit{type, run});
                }
                if (dir.y == 1 && pos.y == room.height - 1) {
                    dir = ivec2{1, 0};
                } else if (dir.x == 1 && pos.x == room.width - 1) {
                    dir = ivec2{0, -1};
                } else if (dir.y == -1 && pos.y == 0) {
                    dir = ivec2{-1, 0};
                } else if (dir.x == -1 && pos.x == 0) {
                    dir = ivec2{0, 1};
                    if (!start) {
                        start = pos;
                    } else if (pos == *start && !flag2) {
                        if (!run.empty())
                            list.push_back(exit{type, run});
                        break;
                    }
                }
                flag = !valid_tile(room, pos, type);
            }
        }
        return list;
    }

    bool any(sys::game_state const& state, room_view const& room, kind type, int32_t min_width) {
        for (auto const& e : map(state, room))
            if (e.type == type && e.width() >= min_width)
                return true;
        return false;
    }
}
