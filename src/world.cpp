#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include "world.hpp"
#include "parser_defs_generated.hpp"
#include "reports.hpp"
#include "text.hpp"

namespace sys {
    std::unique_ptr<game_state> make_game_state() {
        return std::make_unique<game_state>();
    }
    dcon::room_id game_state::find_room(std::string_view name) const {
        auto const it = room_by_name.find(text::to_lowercase_native(name));
        return it == room_by_name.end() ? dcon::room_id{} : it->second;
    }
}

namespace world {
    //
    // room_view (Room.cs tile helpers)
    //
    slope_direction room_view::identify_slope(int32_t x, int32_t y) const noexcept {
        if (get_terrain(x, y) == terrain::slope) {
            if (get_terrain(x - 1, y) == terrain::solid) {
                if (get_terrain(x, y - 1) == terrain::solid)
                    return slope_direction::up_right;
                if (get_terrain(x, y + 1) == terrain::solid)
                    return slope_direction::down_right;
            } else if (get_terrain(x + 1, y) == terrain::solid) {
                if (get_terrain(x, y - 1) == terrain::solid)
                    return slope_direction::up_left;
                if (get_terrain(x, y + 1) == terrain::solid)
                    return slope_direction::down_left;
            }
        }
        return slope_direction::broken;
    }

    bool room_view::visual_contact(rw::vec2 a, rw::vec2 b) const noexcept {
        if (get_tile(a).solid() || get_tile(b).solid())
            return false;
        float const d = rw::distance(a, b);
        for (float t = 20.f; t < d; t += 20.f)
            if (get_tile(rw::lerp(a, b, t / d)).solid())
                return false;
        return true;
    }

    rw::ivec2 room_view::shortcut_entrance_hole_direction(rw::ivec2 pos) const noexcept {
        for (auto const d : rw::custom::four_directions)
            if (get_terrain(pos.x + d.x, pos.y + d.y) != terrain::solid)
                return d;
        return rw::ivec2{0, 0};
    }

    shortcut_data room_view::get_shortcut_data(rw::ivec2 pos) const noexcept {
        shortcut_data r{pos, pos, 0, shortcut_type::dead_end, dcon::room_exit_id{}};
        // Room.shortcutData: note the vanilla quirk where (0, 0) never matches
        if (s == nullptr || (pos.x == 0 && pos.y == 0))
            return r;
        auto const& dc = s->world;
        auto const range = dc.room_range_of_shortcut_location(id);
        for (auto it = range.first; it != range.second; ++it) {
            auto const sc = dc.shortcut_location_get_shortcut(*it);
            if (dc.shortcut_get_start(sc) == pos) {
                r.dest = dc.shortcut_get_dest(sc);
                r.length = dc.shortcut_get_length(sc);
                r.type = dc.shortcut_get_type(sc);
                r.dest_exit = dc.shortcut_get_exit_from_shortcut_destination(sc);
                break;
            }
        }
        return r;
    }

    room_view get_room_view(sys::game_state const& s, dcon::room_id rid) noexcept {
        auto const& dc = s.world;
        room_view v;
        v.s = &s;
        v.id = rid;
        v.width = dc.room_get_width(rid);
        v.height = dc.room_get_height(rid);
        auto const first = dc.room_get_first_tile(rid).id;
        v.terrains = &dc.tile_get_terrain(first);
        v.flags = &dc.tile_get_flags(first);
        v.shortcuts = &dc.tile_get_shortcut(first);
        v.waters = &dc.tile_get_water(first);
        v.default_water_level = dc.room_get_water_level(rid);
        v.water = v.default_water_level != -1;
        if (v.water)
            v.float_water_level = room_view::middle_of_tile(0, v.default_water_level).y;
        // the rain's flood (rain.cpp) over the room's own water level
        if (float const flood = dc.room_get_flood_level(rid); flood > v.float_water_level) {
            v.water = true;
            v.float_water_level = flood;
        }
        v.gravity = dc.room_get_gravity(rid);
        return v;
    }

    //
    // Room file parsing (Room.LoadFromDataString, RoomPreprocessor.VersionFix,
    // RoomPreprocessor.StringToConnMap, ShortcutMapper). Parsing only touches a
    // parsed_room so that it can run in parallel; commit_room then moves the
    // result into the data container.
    //
    namespace {
        struct parsed_node {
            uint16_t type = 0;
            uint16_t shortcut_length = 0;
            int16_t viewed_by_camera = 0;
            uint16_t entrance_width = 0;
            bool submerged = false;
        };
        struct parsed_item {
            rw::ivec2 tile;
            item_type type = item_type::rock;
        };
        struct parsed_trigger {
            bool spot = true;
            rw::vec2 pos{0.f, 0.f};
            float rad = 0.f;
            float fire_chance = 1.f;
            int16_t delay = 0;
            bool multi_use = false;
            bool stop = false;
            std::string song;
            float vol = 0.3f;
            float prio = 0.5f;
            float fade_in = 0.f;
            float fade_out = 0.f;
            int16_t rooms = -1;
        };
        struct parsed_rot {
            rot_kind kind = rot_kind::corruption;
            rw::vec2 pos{0.f, 0.f};
            rw::vec2 handle{0.f, 100.f};
            rot_custom_data custom;
        };
        struct parsed_warp {
            rw::vec2 pos{0.f, 0.f};
            std::string dest_room; // lowercase, empty: dynamic
            std::optional<rw::vec2> dest_pos;
            warp_effect effect;
            bool one_way = false;
            bool one_way_entrance = false;
            bool ripple_warp = false;
            bool limited_use = false;
            int16_t uses = 0;
            bool any_slugcat = false;
            uint32_t seed = 0;
        };
        struct parsed_warp_target {
            rw::vec2 pos{0.f, 0.f};
            float ripple_requirement = 0.f;
            bool dead_end = false;
            bool bad_warp = false;
        };
        struct parsed_decal {
            rw::vec2 pos{0.f, 0.f};
            rw::vec2 handles[3];
            float from_depth = 0.f;
            float to_depth = 1.f;
            float noise = 0.f;
            std::string image;
            // CustomDecalData.vertices: per corner opacity and smudge
            rw::vec2 uv[4] = {{0.5f, 0.f}, {0.5f, 0.f}, {0.5f, 0.f}, {0.5f, 0.f}};
        };
        struct parsed_thing {
            thing_kind kind = thing_kind::dangle_fruit;
            rw::vec2 pos{0.f, 0.f};
            rw::vec2 handle{0.f, 0.f};
            rw::vec2 handle2{0.f, 0.f};
            float extra = 0.f;
            std::string name;
        };
        struct parsed_shortcut {
            rw::ivec2 start;
            rw::ivec2 dest;
            uint16_t length = 0;
            shortcut_type type = shortcut_type::dead_end;
            int16_t dest_exit = -1; // index into exits
            std::vector<rw::ivec2> path; // entrance first
        };

        struct parsed_room {
            std::string name;
            dcon::region_id region;
            parsers::error_handler err;
            bool ok = false;

            std::string display_name;
            int32_t width = 1;
            int32_t height = 1;
            int32_t water_level = -1;
            bool water_in_front = false;
            bool border_solid = true;
            rw::vec2 light_angle{0.f, 0.f};
            uint16_t preprocessing_generation = 0;
            uint16_t connectivity_templates = 0;
            std::vector<rw::vec2> cameras;
            std::vector<parsed_item> items;
            std::vector<parsed_node> nodes;
            std::vector<node_connectivity> connectivity; // nodes * templates * nodes
            std::vector<terrain> terrains;
            std::vector<uint8_t> flags;
            std::vector<uint8_t> shortcut_marks;
            std::vector<uint8_t> waters;
            std::vector<rw::ivec2> exits;
            std::vector<parsed_shortcut> shortcuts;

            bool has_settings = false;
            parsers::room_template settings;
            std::vector<parsed_trigger> triggers;
            std::vector<parsed_rot> rot;
            std::vector<parsed_warp> warps;
            std::vector<parsed_warp_target> warp_targets;
            std::vector<parsed_decal> decals;
            std::vector<parsed_thing> things;

            room_view view() const noexcept {
                room_view v;
                v.width = width;
                v.height = height;
                v.terrains = terrains.data();
                v.flags = flags.data();
                v.shortcuts = shortcut_marks.data();
                v.waters = waters.data();
                return v;
            }
        };

        // Minimal allocation free scanner over one line of a room file
        struct scanner {
            char const* p;
            char const* end;

            bool done() const noexcept { return p >= end; }
            bool eat(char c) noexcept {
                if (p < end && *p == c) {
                    ++p;
                    return true;
                }
                return false;
            }
            void skip_to(char c) noexcept {
                while (p < end && *p != c)
                    ++p;
            }
            void skip_spaces() noexcept {
                while (p < end && (*p == ' ' || *p == '\t'))
                    ++p;
            }
            bool read_int(int32_t& out) noexcept {
                skip_spaces();
                bool neg = false;
                if (p < end && (*p == '-' || *p == '+')) {
                    neg = *p == '-';
                    ++p;
                }
                if (p >= end || *p < '0' || *p > '9')
                    return false;
                int64_t v = 0;
                while (p < end && *p >= '0' && *p <= '9')
                    v = v * 10 + (*p++ - '0');
                out = int32_t(neg ? -v : v);
                return true;
            }
            bool read_float(float& out) noexcept {
                skip_spaces();
                char buf[64];
                size_t n = 0;
                while (p < end && n + 1 < sizeof(buf) && (std::strchr("+-.eE", *p) != nullptr || (*p >= '0' && *p <= '9')))
                    buf[n++] = *p++;
                buf[n] = '\0';
                if (n == 0)
                    return false;
                char* e = nullptr;
                out = std::strtof(buf, &e);
                return e != buf;
            }
        };

        // File.ReadAllLines semantics: \n, \r\n and \r all end a line and a
        // trailing terminator does not produce an empty last line
        void split_lines(std::string_view contents, std::vector<std::string_view>& lines) {
            size_t start = 0;
            size_t i = 0;
            while (i < contents.size()) {
                char const c = contents[i];
                if (c == '\n' || c == '\r') {
                    lines.push_back(contents.substr(start, i - start));
                    if (c == '\r' && i + 1 < contents.size() && contents[i + 1] == '\n')
                        ++i;
                    start = i + 1;
                }
                ++i;
            }
            if (start < contents.size())
                lines.push_back(contents.substr(start));
        }

        // ShortcutHandler.NextShortcutPosition
        rw::ivec2 next_shortcut_position(room_view const& room, rw::ivec2 pos, rw::ivec2 last_pos) noexcept {
            rw::ivec2 const d = pos - last_pos;
            if ((d.x != 0 || d.y != 0) && room.get_tile(pos + d).shortcut != 0 && room.inside(pos.x + d.x, pos.y + d.y)) {
                pos = pos + d;
            } else {
                for (auto const fd : rw::custom::four_directions) {
                    if ((fd.x != -d.x || fd.y != -d.y) && room.get_tile(pos + fd).shortcut != 0 && room.inside(pos.x + fd.x, pos.y + fd.y)) {
                        pos = pos + fd;
                        break;
                    }
                }
                if (pos == last_pos)
                    pos = pos - d;
            }
            return pos;
        }

        // ShortcutMapper.MapShortcut
        parsed_shortcut map_shortcut(room_view const& room, int32_t x, int32_t y, std::vector<rw::ivec2> const& nodes_index) noexcept {
            auto const node_of = [&](rw::ivec2 p) -> int16_t {
                auto const it = std::find(nodes_index.begin(), nodes_index.end(), p);
                return it == nodes_index.end() ? int16_t(-1) : int16_t(it - nodes_index.begin());
            };
            rw::ivec2 const start{x, y};
            rw::ivec2 cur = start;
            rw::ivec2 last = cur;
            uint32_t n = 0;
            shortcut_type type = shortcut_type::dead_end;
            int16_t dest = -1;
            std::vector<rw::ivec2> path{start};
            for (;;) {
                ++n;
                rw::ivec2 const prev = cur;
                cur = next_shortcut_position(room, cur, last);
                if (cur == prev)
                    break;
                path.push_back(cur);
                last = prev;
                auto const t = room.get_tile(cur);
                if (t.t == terrain::shortcut_entrance) {
                    type = shortcut_type::normal;
                    break;
                } else if (t.shortcut == 2) {
                    type = shortcut_type::room_exit;
                    dest = node_of(cur);
                    break;
                } else if (t.shortcut == 3) {
                    type = shortcut_type::creature_hole;
                    dest = node_of(cur);
                    break;
                } else if (t.shortcut == 4) {
                    type = shortcut_type::npc_transportation;
                    break;
                } else if (t.shortcut == 5) {
                    type = shortcut_type::region_transportation;
                    dest = node_of(cur);
                    break;
                } else if (n > 1000) {
                    break;
                }
            }
            return parsed_shortcut{start, cur, uint16_t(std::min<uint32_t>(n, 0xFFFF)), type, dest, std::move(path)};
        }

        void parse_cameras(parsed_room& r, std::string_view line) {
            scanner sc{line.data(), line.data() + line.size()};
            while (!sc.done()) {
                float x = 0.f, y = 0.f;
                if (!sc.read_float(x)) {
                    r.err.add_error("bad camera position");
                    break;
                }
                if (sc.eat(','))
                    sc.read_float(y);
                // Room.LoadFromDataString flips camera Y into room space
                r.cameras.push_back(rw::vec2{x, -(800.f - float(r.height) * 20.f + y)});
                sc.skip_to('|');
                sc.eat('|');
            }
            if (r.cameras.empty()) {
                r.err.add_warning("no cameras defined");
                r.cameras.push_back(rw::vec2{0.f, 0.f});
            }
        }

        void parse_items(parsed_room& r, std::string_view line) {
            // "<type>,<x>,<y>|" entries, the trailing (unterminated) element is ignored
            scanner sc{line.data(), line.data() + line.size()};
            while (!sc.done()) {
                char const* entry_end = static_cast<char const*>(std::memchr(sc.p, '|', size_t(sc.end - sc.p)));
                if (entry_end == nullptr)
                    break;
                scanner e{sc.p, entry_end};
                int32_t type = 0, x = 0, y = 0;
                if (e.read_int(type) && e.eat(',') && e.read_int(x) && e.eat(',') && e.read_int(y))
                    r.items.push_back(parsed_item{rw::ivec2{x - 1, r.height - y}, type == 1 ? item_type::spear : item_type::rock});
                else
                    r.err.add_error("bad placed item entry");
                sc.p = entry_end + 1;
            }
        }

        void parse_connectivity(parsed_room& r, std::string_view line) {
            scanner sc{line.data(), line.data() + line.size()};
            int32_t gen = 0, node_count = 0, templates = 0;
            sc.read_int(gen);
            r.preprocessing_generation = uint16_t(gen < 0 ? 0 : gen);
            // Like WorldLoader, only trust maps from the current preprocessing
            // generation; older ones (different layout) must be regenerated
            if (gen < preprocessing_generation)
                return;
            if (!(sc.eat('|') && sc.read_int(node_count) && sc.eat('|') && sc.read_int(templates) && sc.eat('|'))
            || node_count < 0 || templates < 0) {
                r.err.add_error("bad connectivity map header");
                return;
            }
            r.connectivity_templates = uint16_t(templates);
            size_t const pairs = size_t(templates) * size_t(node_count);
            for (int32_t i = 0; i < node_count; ++i) {
                parsed_node n;
                int32_t type = 0, sc_len = 0, sub = 0, cam = 0, width = 0;
                if (!(sc.read_int(type) && sc.eat(',') && sc.read_int(sc_len) && sc.eat(',') && sc.read_int(sub) && sc.eat(',')
                && sc.read_int(cam) && sc.eat(',') && sc.read_int(width) && sc.eat(','))) {
                    r.err.add_error("bad connectivity node " + std::to_string(i));
                    break;
                }
                n.type = uint16_t(type);
                n.shortcut_length = uint16_t(sc_len);
                n.submerged = sub == 1;
                n.viewed_by_camera = int16_t(cam);
                n.entrance_width = uint16_t(width);
                r.nodes.push_back(n);
                for (size_t j = 0; j < pairs; ++j) {
                    int32_t a = 0, b = 0;
                    if (!(sc.read_int(a) && sc.eat(' ') && sc.read_int(b))) {
                        r.err.add_error("bad connectivity pair");
                        a = b = -1;
                    }
                    sc.eat(',');
                    r.connectivity.push_back(node_connectivity{int16_t(a), int16_t(b)});
                }
                sc.skip_to('|');
                sc.eat('|');
            }
        }

        bool parse_tiles(parsed_room& r, std::string_view line) {
            int32_t const w = r.width;
            int32_t const h = r.height;
            size_t const total = size_t(w) * size_t(h);
            r.terrains.assign(total, terrain::air);
            r.flags.assign(total, 0);
            r.shortcut_marks.assign(total, 0);
            r.waters.assign(total, 0);
            // every tile starts as air with the default water level applied
            for (int32_t y = 0; y < h; ++y) {
                uint8_t const wv = uint8_t((y <= r.water_level ? 1 : 0) + (y == r.water_level ? 1 : 0));
                std::memset(r.waters.data() + size_t(y) * size_t(w), wv, size_t(w));
            }

            // Column major, starting at the top left; every entry is terminated
            // by '|' (vanilla ignores whatever follows the last one)
            int32_t x = 0, y = h - 1;
            size_t parsed = 0;
            char const* p = line.data();
            char const* const end = line.data() + line.size();
            while (p < end) {
                char const* entry_end = static_cast<char const*>(std::memchr(p, '|', size_t(end - p)));
                if (entry_end == nullptr)
                    break;
                if (x >= w) {
                    r.err.add_error("too many geometry tiles");
                    break;
                }
                size_t const i = size_t(y) * size_t(w) + size_t(x);
                scanner e{p, entry_end};
                int32_t value = 0;
                if (!e.read_int(value) || value < 0 || value > 4) {
                    r.err.add_error("bad terrain value at tile " + std::to_string(x) + "," + std::to_string(y));
                    value = 0;
                }
                r.terrains[i] = terrain(value);
                uint8_t flags = 0;
                uint8_t shortcut = 0;
                while (e.eat(',')) {
                    if (!e.read_int(value)) {
                        r.err.add_error("bad tile attribute");
                        break;
                    }
                    switch (value) {
                    case 1: flags |= tile_vertical_beam; break;
                    case 2: flags |= tile_horizontal_beam; break;
                    case 3: if (shortcut < 1) shortcut = 1; break;   // shortcut path
                    case 4: shortcut = 2; break;                     // room exit
                    case 5: shortcut = 3; break;                     // creature den
                    case 6: flags |= tile_wall_behind; break;
                    case 7: flags |= tile_hive; break;               // bat hive
                    case 8: flags |= tile_waterfall; break;
                    case 9: shortcut = 4; break;                     // whack-a-mole hole
                    case 10: flags |= tile_garbage_hole; break;
                    case 11: flags |= tile_worm_grass; break;
                    case 12: shortcut = 5; break;                    // NPC/region transport
                    default: break;                                  // ignored by vanilla too
                    }
                }
                r.flags[i] = flags;
                r.shortcut_marks[i] = shortcut;
                ++parsed;
                if (--y < 0) {
                    ++x;
                    y = h - 1;
                }
                p = entry_end + 1;
            }
            if (parsed != total) {
                r.err.add_error("inconsistent geometry count " + std::to_string(parsed) + " / " + std::to_string(total));
                return false;
            }
            return true;
        }

        void map_shortcuts(parsed_room& r) {
            room_view const room = r.view();
            // Room.exitAndDenIndex: room exits, then dens, then transports
            for (uint8_t const want : {uint8_t(2), uint8_t(3), uint8_t(5)})
                for (int32_t y = room.height - 1; y >= 0; --y)
                    for (int32_t x = 0; x < room.width; ++x)
                        if (room.get_tile(x, y).shortcut == want)
                            r.exits.push_back(rw::ivec2{x, y});
            for (int32_t y = room.height - 1; y >= 0; --y)
                for (int32_t x = 0; x < room.width; ++x)
                    if (room.get_terrain(x, y) == terrain::shortcut_entrance)
                        r.shortcuts.push_back(map_shortcut(room, x, y, r.exits));
        }

        bool parse_geometry(parsed_room& r, std::string_view contents) {
            auto& err = r.err;
            err.line = 0;
            std::vector<std::string_view> lines;
            lines.reserve(16);
            split_lines(contents, lines);

            // RoomPreprocessor.VersionFix: very old 8 line files lack lines 5..8
            if (lines.size() == 8) {
                std::vector<std::string_view> fixed(12);
                for (size_t i = 0; i < 5; ++i)
                    fixed[i] = lines[i];
                for (size_t i = 5; i < 8; ++i)
                    fixed[i + 4] = lines[i];
                lines = std::move(fixed);
            }
            if (lines.size() < 12) {
                err.add_error("room file has " + std::to_string(lines.size()) + " lines, expected at least 12");
                return false;
            }
            r.display_name = std::string{lines[0]};

            // line 2: "<w>*<h>|<water level>|<water in front>"
            {
                err.line = 2;
                scanner sc{lines[1].data(), lines[1].data() + lines[1].size()};
                int32_t w = 0, h = 0, water = -1, in_front = 0;
                if (!(sc.read_int(w) && sc.eat('*') && sc.read_int(h)) || w <= 0 || h <= 0) {
                    err.add_error("invalid room dimensions");
                    return false;
                }
                // VersionFix appends "|-1|0" when the water fields are missing
                if (std::count(lines[1].begin(), lines[1].end(), '|') >= 2) {
                    sc.skip_to('|');
                    sc.eat('|');
                    if (!sc.read_int(water))
                        water = -1;
                    sc.skip_to('|');
                    sc.eat('|');
                    if (!sc.read_int(in_front))
                        in_front = 0;
                }
                r.width = w;
                r.height = h;
                r.water_level = water < 0 ? -1 : water;
                r.water_in_front = water >= 0 && in_front == 1;
            }
            // line 3: "<light angle x>*<light angle y>|..."
            {
                err.line = 3;
                scanner sc{lines[2].data(), lines[2].data() + lines[2].size()};
                if (!(sc.read_float(r.light_angle.x) && sc.eat('*') && sc.read_float(r.light_angle.y)))
                    err.add_warning("bad light angle");
            }
            err.line = 4;
            parse_cameras(r, lines[3]);
            // line 5: "Border: Solid" / "Border: Passable"
            r.border_solid = lines[4].find("Passable") == std::string_view::npos;
            err.line = 6;
            parse_items(r, lines[5]);
            err.line = 10;
            parse_connectivity(r, lines[9]);
            // line 11 holds the base64 creature AI heat maps; with files mmap'd on
            // demand (RomFS) these are decoded when the AI preprocessor needs them.
            err.line = 12;
            if (!parse_tiles(r, lines[11]))
                return false;
            map_shortcuts(r);
            return true;
        }

        danger_type to_danger_type(std::string_view sv) {
            return sv == "Rain" ? danger_type::rain
                : sv == "Flood" ? danger_type::flood
                : sv == "FloodAndRain" ? danger_type::flood_and_rain
                : sv == "None" ? danger_type::none
                : sv == "AerieBlizzard" ? danger_type::aerie_blizzard
                : sv == "Blizzard" ? danger_type::blizzard
                : sv == "Sandstorm" ? danger_type::sandstorm
                : sv == "Thunder" ? danger_type::thunder
                : danger_type::unknown;
        }

        dcon::room_id commit_room(sys::game_state& s, parsed_room& r) {
            auto& dc = s.world;
            auto const rid = dc.create_room();
            // its share of the state outside the data container (caches, buffers)
            s.rooms.resize(size_t(dc.room_size()));
            dc.room_set_name(rid, s.add_text(r.name));
            dc.room_set_display_name(rid, s.add_text(r.display_name));
            dc.room_set_width(rid, r.width);
            dc.room_set_height(rid, r.height);
            dc.room_set_water_level(rid, r.water_level);
            dc.room_set_water_in_front(rid, r.water_in_front);
            dc.room_set_border_solid(rid, r.border_solid);
            dc.room_set_light_angle(rid, r.light_angle);
            dc.room_set_preprocessing_generation(rid, r.preprocessing_generation);
            dc.room_set_connectivity_templates(rid, r.connectivity_templates);
            dc.room_set_gravity(rid, s.default_room_gravity);
            if (r.region)
                dc.try_create_region_membership(rid, r.region);

            // tiles: one contiguous block, filled straight into the property arrays
            uint32_t const first = dc.tile_size();
            uint32_t const count = uint32_t(r.terrains.size());
            dc.tile_resize(first + count);
            dcon::tile_id const first_id{dcon::tile_id::value_base_t(first)};
            std::memcpy(&dc.tile_get_terrain(first_id), r.terrains.data(), count * sizeof(terrain));
            std::memcpy(&dc.tile_get_flags(first_id), r.flags.data(), count);
            std::memcpy(&dc.tile_get_shortcut(first_id), r.shortcut_marks.data(), count);
            std::memcpy(&dc.tile_get_water(first_id), r.waters.data(), count);
            dc.room_set_first_tile(rid, first_id);

            for (auto const c : r.cameras) {
                auto const cid = dc.create_room_camera();
                dc.room_camera_set_position(cid, c);
                dc.try_create_camera_location(cid, rid);
            }
            for (auto const& t : r.triggers) {
                auto const tid = dc.create_music_trigger();
                dc.music_trigger_set_spot(tid, t.spot);
                dc.music_trigger_set_pos(tid, t.pos);
                dc.music_trigger_set_rad(tid, t.rad);
                dc.music_trigger_set_fire_chance(tid, t.fire_chance);
                dc.music_trigger_set_delay(tid, t.delay);
                dc.music_trigger_set_multi_use(tid, t.multi_use);
                dc.music_trigger_set_stop(tid, t.stop);
                dc.music_trigger_set_song(tid, s.add_text(t.song));
                dc.music_trigger_set_vol(tid, t.vol);
                dc.music_trigger_set_prio(tid, t.prio);
                dc.music_trigger_set_fade_in(tid, t.fade_in);
                dc.music_trigger_set_fade_out(tid, t.fade_out);
                dc.music_trigger_set_rooms(tid, t.rooms);
                dc.music_trigger_set_counter(tid, -1);
                dc.try_create_music_trigger_location(tid, rid);
            }
            for (auto const& a : r.settings.ambient_sounds) {
                auto const aid = dc.create_ambient_sound();
                dc.ambient_sound_set_type(aid, a.type);
                dc.ambient_sound_set_sample(aid, s.add_text(a.sample));
                dc.ambient_sound_set_volume(aid, a.volume);
                dc.ambient_sound_set_pitch(aid, a.pitch);
                dc.ambient_sound_set_pos(aid, a.pos);
                dc.ambient_sound_set_direction(aid, a.direction);
                dc.ambient_sound_set_rad(aid, a.rad);
                dc.ambient_sound_set_taper(aid, a.taper);
                dc.ambient_sound_set_doppler(aid, a.doppler);
                dc.try_create_ambient_sound_location(aid, rid);
            }
            for (auto const& o : r.rot) {
                auto const oid = dc.create_rot_placed();
                dc.rot_placed_set_kind(oid, o.kind);
                dc.rot_placed_set_pos(oid, o.pos);
                dc.rot_placed_set_handle(oid, o.handle);
                dc.rot_placed_set_custom(oid, o.custom);
                dc.try_create_rot_placed_location(oid, rid);
            }
            for (auto const& o : r.warps) {
                auto const wid = dc.create_warp_placed();
                dc.warp_placed_set_pos(wid, o.pos);
                if (!o.dest_room.empty())
                    dc.warp_placed_set_dest_room(wid, s.add_text(o.dest_room));
                dc.warp_placed_set_dest_pos(wid, o.dest_pos);
                dc.warp_placed_set_effect(wid, o.effect);
                dc.warp_placed_set_one_way(wid, o.one_way);
                dc.warp_placed_set_one_way_entrance(wid, o.one_way_entrance);
                dc.warp_placed_set_ripple_warp(wid, o.ripple_warp);
                dc.warp_placed_set_limited_use(wid, o.limited_use);
                dc.warp_placed_set_uses(wid, o.uses);
                dc.warp_placed_set_any_slugcat(wid, o.any_slugcat);
                dc.warp_placed_set_seed(wid, o.seed);
                dc.try_create_warp_placed_location(wid, rid);
            }
            for (auto const& o : r.things) {
                auto const tid = dc.create_placed_thing();
                dc.placed_thing_set_kind(tid, o.kind);
                dc.placed_thing_set_pos(tid, o.pos);
                dc.placed_thing_set_handle(tid, o.handle);
                dc.placed_thing_set_handle2(tid, o.handle2);
                dc.placed_thing_set_extra(tid, o.extra);
                if (!o.name.empty())
                    dc.placed_thing_set_name(tid, s.add_text(o.name));
                dc.try_create_thing_location(tid, rid);
            }
            for (auto const& o : r.decals) {
                auto const did = dc.create_decal_placed();
                dc.decal_placed_set_pos(did, o.pos);
                dc.decal_placed_set_handle_a(did, o.handles[0]);
                dc.decal_placed_set_handle_b(did, o.handles[1]);
                dc.decal_placed_set_handle_c(did, o.handles[2]);
                dc.decal_placed_set_from_depth(did, o.from_depth);
                dc.decal_placed_set_to_depth(did, o.to_depth);
                dc.decal_placed_set_noise(did, o.noise);
                dc.decal_placed_set_image(did, s.add_text(o.image));
                dc.decal_placed_set_uv_a(did, o.uv[0]);
                dc.decal_placed_set_uv_b(did, o.uv[1]);
                dc.decal_placed_set_uv_c(did, o.uv[2]);
                dc.decal_placed_set_uv_d(did, o.uv[3]);
                dc.try_create_decal_location(did, rid);
            }
            for (auto const& o : r.warp_targets) {
                auto const tid = dc.create_warp_target();
                dc.warp_target_set_pos(tid, o.pos);
                dc.warp_target_set_ripple_requirement(tid, o.ripple_requirement);
                dc.warp_target_set_dead_end(tid, o.dead_end);
                dc.warp_target_set_bad_warp(tid, o.bad_warp);
                dc.try_create_warp_target_location(tid, rid);
            }
            for (auto const& it : r.items) {
                auto const iid = dc.create_placed_item();
                dc.placed_item_set_tile(iid, it.tile);
                dc.placed_item_set_type(iid, it.type);
                dc.try_create_item_location(iid, rid);
            }
            std::vector<dcon::room_exit_id> exits;
            exits.reserve(r.exits.size());
            for (size_t i = 0; i < r.exits.size(); ++i) {
                auto const eid = dc.create_room_exit();
                dc.room_exit_set_tile(eid, r.exits[i]);
                dc.room_exit_set_index(eid, uint8_t(i));
                dc.try_create_exit_location(eid, rid);
                exits.push_back(eid);
            }
            for (auto const& sc : r.shortcuts) {
                auto const sid = dc.create_shortcut();
                dc.shortcut_set_start(sid, sc.start);
                dc.shortcut_set_dest(sid, sc.dest);
                dc.shortcut_set_length(sid, sc.length);
                dc.shortcut_set_type(sid, sc.type);
                auto path = dc.shortcut_get_path(sid);
                for (auto const t : sc.path)
                    path.push_back(t);
                dc.try_create_shortcut_location(sid, rid);
                if (sc.dest_exit >= 0 && size_t(sc.dest_exit) < exits.size())
                    dc.try_create_shortcut_destination(sid, exits[size_t(sc.dest_exit)]);
            }
            size_t const per_node = size_t(r.connectivity_templates) * r.nodes.size();
            for (size_t i = 0; i < r.nodes.size(); ++i) {
                auto const nid = dc.create_room_node();
                auto const& n = r.nodes[i];
                dc.room_node_set_type(nid, n.type);
                dc.room_node_set_shortcut_length(nid, n.shortcut_length);
                dc.room_node_set_viewed_by_camera(nid, n.viewed_by_camera);
                dc.room_node_set_entrance_width(nid, n.entrance_width);
                dc.room_node_set_submerged(nid, n.submerged);
                dc.room_node_set_index(nid, uint8_t(i));
                auto conn = dc.room_node_get_connectivity(nid);
                for (size_t j = 0; j < per_node && i * per_node + j < r.connectivity.size(); ++j)
                    conn.push_back(r.connectivity[i * per_node + j]);
                dc.try_create_node_location(nid, rid);
            }

            // RoomSettings, -1 meaning "not set"; unset values come from
            // DefaultRoomSettings.ancestor
            auto const& t = r.settings;
            // vanilla falls back on the region palette through the settings templates
            dc.room_set_palette(rid, t.palette ? s.palette_for(*t.palette) : dcon::palette_id{});
            if (!dc.room_get_palette(rid) && r.region)
                dc.room_set_palette(rid, dc.region_get_palette(r.region));
            if (!dc.room_get_palette(rid))
                dc.room_set_palette(rid, s.palette_for(0));
            dc.room_set_effect_color_a(rid, t.effect_color_a.value_or(-1));
            dc.room_set_effect_color_b(rid, t.effect_color_b.value_or(-1));
            dc.room_set_fade_palette(rid, t.fade_palette_index ? s.palette_for(*t.fade_palette_index) : dcon::palette_id{});
            dc.room_set_fade_palette_amount(rid, t.fade_palette_amount.value_or(0.f));
            dc.room_set_danger_type(rid, to_danger_type(t.danger_type.value_or("Rain")));
            dc.room_set_grime(rid, t.grime.value_or(0.5f));
            dc.room_set_clouds(rid, t.clouds.value_or(0.f));
            dc.room_set_ceiling_drips(rid, t.ceiling_drips.value_or(0.5f));
            dc.room_set_wave_speed(rid, t.wave_speed.value_or(0.75f));
            dc.room_set_wave_length(rid, t.wave_length.value_or(0.5f));
            dc.room_set_wave_amplitude(rid, t.wave_amplitude.value_or(0.f));
            dc.room_set_second_wave_length(rid, t.second_wave_length.value_or(0.16666667f));
            dc.room_set_second_wave_amplitude(rid, t.second_wave_amplitude.value_or(0.f));
            dc.room_set_water_reflection_alpha(rid, t.water_reflection_alpha.value_or(1.f));
            dc.room_set_bkg_drone_volume(rid, t.bkg_drone_volume.value_or(0.3f));
            dc.room_set_bkg_drone_no_threat_volume(rid, t.bkg_drone_no_threat_volume.value_or(1.f));
            dc.room_set_random_item_density(rid, t.random_item_density.value_or(0.5f));
            dc.room_set_random_item_spear_chance(rid, t.random_item_spear_chance.value_or(0.2f));
            dc.room_set_rumble_intensity(rid, t.rumble_intensity.value_or(1.f));
            dc.room_set_rain_intensity(rid, t.rain_intensity.value_or(1.f));
            dc.room_set_room_specific_script_active(rid, t.room_specific_script_active.value_or(false));
            dc.room_set_wet_terrain(rid, t.wet_terrain.value_or(false));

            s.room_by_name.emplace(r.name, rid);
            return rid;
        }

        void merge_errors(parsers::error_handler& into, parsers::error_handler const& from) {
            into.accumulated_errors += from.accumulated_errors;
            into.accumulated_warnings += from.accumulated_warnings;
            into.error_count += from.error_count;
            into.warning_count += from.warning_count;
        }
    }

    dcon::room_id add_room(sys::game_state& s, std::string_view name, dcon::region_id region, std::string_view contents, parsers::error_handler& err) {
        parsed_room r;
        r.name = std::string{name};
        r.region = region;
        r.err.filename = err.filename;
        r.ok = parse_geometry(r, contents);
        merge_errors(err, r.err);
        return r.ok ? commit_room(s, r) : dcon::room_id{};
    }

    //
    // Scenario loading
    //
    namespace {
        using template_map = std::unordered_map<std::string, parsers::room_template>;

        void register_regions(sys::game_state& s, layered_fs::directory const& world_dir, parsers::error_handler& err) {
            auto const f = world_dir.get_single_file("regions.txt");
            auto const of = f.open_file(s.fs);
            if (!of) {
                err.add_standalone_error("no world/regions.txt found");
                err.fatal = true;
                return;
            }
            std::vector<std::string_view> lines;
            split_lines(of->to_string_view(), lines);
            auto const add = [&](std::string const& name, bool pseudo) {
                if (s.region_by_name.contains(name))
                    return;
                auto const rid = s.world.create_region();
                s.world.region_set_name(rid, s.add_text(name));
                s.world.region_set_display_name(rid, s.add_text(name));
                s.world.region_set_is_pseudo(rid, pseudo);
                s.region_by_name.emplace(name, rid);
            };
            for (auto line : lines) {
                while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
                    line.remove_suffix(1);
                if (!line.empty())
                    add(text::to_lowercase_native(line), false);
            }
            // mods add regions through modify/world/regions.txt ("[ADD]WARA"),
            // every root's own copy counts (the modify merge system isn't
            // ported beyond this)
            for (auto it = s.fs.roots.rbegin(); it != s.fs.roots.rend(); ++it) {
                // modify/ folders aren't indexed (they are patches, not files to layer)
                std::ifstream in(it->root + "/modify/world/regions.txt");
                if (!in)
                    continue;
                std::string line;
                while (std::getline(in, line)) {
                    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
                        line.pop_back();
                    constexpr std::string_view tag = "[ADD]";
                    if (line.size() > tag.size() && std::string_view{line}.substr(0, tag.size()) == tag)
                        add(text::to_lowercase_native(line.substr(tag.size())), false);
                }
            }
            // gates and shared shelters live in their own folders, expose them
            // as pseudo regions so they can be browsed like any other room
            add("gates", true);
            add("gate shelters", true);
        }

        // The "AmbientSounds:" line of a settings file (RoomSettings.
        // LoadAmbientSounds): ", " separated sounds of "<TYPE><sample>..."
        // split on "><" (OMNI, DIR, SPOT; AmbientSound.FromString)
        void parse_ambient_sounds(std::string_view text, std::vector<ambient_sound_desc>& out) {
            constexpr std::string_view key = "AmbientSounds: ";
            size_t at = text.find(key);
            while (at != std::string_view::npos && at != 0 && text[at - 1] != '\n')
                at = text.find(key, at + 1);
            if (at == std::string_view::npos)
                return;
            std::string_view line = text.substr(at + key.size());
            line = line.substr(0, line.find('\n'));
            auto const split = [](std::string_view sv, std::string_view sep) {
                std::vector<std::string_view> r;
                for (size_t p = 0;;) {
                    size_t const q = sv.find(sep, p);
                    r.push_back(sv.substr(p, q == std::string_view::npos ? std::string_view::npos : q - p));
                    if (q == std::string_view::npos)
                        return r;
                    p = q + sep.size();
                }
            };
            auto const num = [](std::string_view sv) { return std::strtof(std::string{sv}.c_str(), nullptr); };
            for (auto entry : split(line, ",")) {
                while (!entry.empty() && (entry.front() == ' ' || entry.front() == '\r'))
                    entry.remove_prefix(1);
                while (!entry.empty() && (entry.back() == ' ' || entry.back() == '\r'))
                    entry.remove_suffix(1);
                auto const f = split(entry, "><");
                ambient_sound_desc a;
                if (f[0] == "OMNI" && f.size() >= 6)
                    a.type = ambient_type::omni;
                else if (f[0] == "DIR" && f.size() >= 7)
                    a.type = ambient_type::directional;
                else if (f[0] == "SPOT" && f.size() >= 10)
                    a.type = ambient_type::spot;
                else
                    continue;
                a.sample = std::string{f[1]};
                a.volume = num(f[4]);
                a.pitch = num(f[5]);
                if (a.type == ambient_type::directional) {
                    a.direction = rw::custom::deg_to_vec(num(f[6]));
                    if (f.size() > 7)
                        a.doppler = num(f[7]);
                } else if (a.type == ambient_type::spot) {
                    a.pos = rw::vec2{num(f[6]), num(f[7])};
                    a.rad = rw::magnitude(rw::vec2{num(f[8]), num(f[9])});
                    if (f.size() > 11) {
                        a.doppler = num(f[10]);
                        a.taper = num(f[11]);
                    }
                }
                out.push_back(std::move(a));
            }
        }

        parsers::room_template parse_template(sys::game_state const& s, std::string_view sv, parsers::error_handler& err) {
            parsers::token_generator gen(sv);
            parsers::room_context ctx{s};
            auto t = parsers::parse_room_template(gen, ctx, err);
            parse_ambient_sounds(sv, t.ambient_sounds);
            return t;
        }

        void register_region_settings(sys::game_state& s, layered_fs::directory const& world_dir, dcon::region_id region,
            parsers::error_handler& err, template_map& templates, std::vector<parsers::room_template>& defaults) {
            auto& dc = s.world;
            std::string const name{s.to_string_view(dc.region_get_name(region))};
            auto const dir = world_dir.get_single_subdir(name);
            if (auto const of = dir.get_single_file("displayname.txt").open_file(s.fs); of) {
                auto sv = of->to_string_view();
                while (!sv.empty() && (sv.back() == '\n' || sv.back() == '\r' || sv.back() == ' '))
                    sv.remove_suffix(1);
                dc.region_set_display_name(region, s.add_text(sv));
            }
            auto const f = dir.get_single_file("properties.txt");
            auto const of = f.open_file(s.fs);
            if (!of)
                return; // pseudo regions have none
            err.filename = f.path;
            parsers::token_generator gen(of->to_string_view());
            parsers::region_context ctx{s, uint32_t(region.index())};
            auto const props = parsers::parse_region_properties(gen, ctx, err);
            dc.region_set_palette(region, s.palette_for(props.palette));
            dc.region_set_bat_deplete_cycles_min(region, uint16_t(props.bat_deplete_cycles_min));
            dc.region_set_bat_deplete_cycles_max(region, uint16_t(props.bat_deplete_cycles_max));
            dc.region_set_bat_deplete_cycles_max_if_less_than_five_left(region, uint16_t(props.bat_deplete_cycles_max_if_less_than_five_left));
            dc.region_set_bat_deplete_cycles_max_if_less_than_two_left(region, uint16_t(props.bat_deplete_cycles_max_if_less_than_two_left));
            dc.region_set_bats_per_active_swarm_room(region, uint16_t(props.bats_per_active_swarm_room));
            dc.region_set_overseers_min(region, uint16_t(props.overseers_min));
            dc.region_set_overseers_max(region, uint16_t(props.overseers_max));
            dc.region_set_ghost_rel_karma_req(region, uint16_t(props.ghost_rel_karma_req));
            dc.region_set_overseers_spawn_chance(region, props.overseers_spawn_chance);
            dc.region_set_player_guide_overseer_spawn_chance(region, props.player_guide_overseer_spawn_chance);
            dc.region_set_corruption_effect_color(region, rw::vec3{props.corruption_effect_color.r, props.corruption_effect_color.g, props.corruption_effect_color.b});
            dc.region_set_corruption_eye_color(region, rw::vec3{props.corruption_eye_color.r, props.corruption_eye_color.g, props.corruption_eye_color.b});
            dc.region_set_albinos(region, props.albinos);
            for (auto const& tname : props.room_setting_templates) {
                auto const lname = text::to_lowercase_native(tname);
                auto const tf = dir.get_single_file(name + "_settingstemplate_" + lname + ".txt");
                if (auto const tof = tf.open_file(s.fs); tof) {
                    err.filename = tf.path;
                    auto t = parse_template(s, tof->to_string_view(), err);
                    // RoomSettings: the first listed template is the region default
                    if (&tname == &props.room_setting_templates.front())
                        defaults[size_t(region.index())] = t;
                    templates.insert_or_assign(name + "_settingstemplate_" + lname, std::move(t));
                } else {
                    err.add_standalone_warning("missing settings template \"" + tf.path + "\"");
                }
            }
        }

        // The "Triggers:" line of a room settings file (EventTrigger.FromString):
        // ", " separated triggers of "<type><tA>key<tB>value<tA>..." where the
        // event value is "<EventType><eA>key<eB>value<eA>...". Only music events
        // are kept.
        void parse_triggers(std::string_view text, std::vector<parsed_trigger>& out) {
            constexpr std::string_view key = "Triggers: ";
            size_t at = text.find(key);
            while (at != std::string_view::npos && at != 0 && text[at - 1] != '\n')
                at = text.find(key, at + 1);
            if (at == std::string_view::npos)
                return;
            std::string_view line = text.substr(at + key.size());
            line = line.substr(0, line.find('\n'));
            auto const split = [](std::string_view sv, std::string_view sep) {
                std::vector<std::string_view> r;
                for (size_t p = 0;;) {
                    size_t const q = sv.find(sep, p);
                    r.push_back(sv.substr(p, q == std::string_view::npos ? std::string_view::npos : q - p));
                    if (q == std::string_view::npos)
                        return r;
                    p = q + sep.size();
                }
            };
            auto const num = [](std::string_view sv) { return std::strtof(std::string{sv}.c_str(), nullptr); };
            for (auto entry : split(line, ",")) {
                while (!entry.empty() && (entry.front() == ' ' || entry.front() == '\r'))
                    entry.remove_prefix(1);
                while (!entry.empty() && (entry.back() == ' ' || entry.back() == '\r'))
                    entry.remove_suffix(1);
                auto const parts = split(entry, "<tA>");
                if (parts.empty() || (parts[0] != "Spot" && parts[0] != "RegionBump" && parts[0] != "PreRegionBump"))
                    continue;
                parsed_trigger t;
                t.spot = parts[0] == "Spot";
                bool music = false;
                rw::vec2 handle{0.f, 0.f};
                for (size_t i = 1; i < parts.size(); ++i) {
                    auto const kv = split(parts[i], "<tB>");
                    if (kv.size() < 2)
                        continue;
                    if (kv[0] == "event") {
                        auto const ev = split(kv[1], "<eA>");
                        if (ev[0] != "MusicEvent" && ev[0] != "StopMusicEvent")
                            break;
                        music = true;
                        t.stop = ev[0] == "StopMusicEvent";
                        for (size_t j = 1; j < ev.size(); ++j) {
                            auto const ekv = split(ev[j], "<eB>");
                            if (ekv.size() < 2)
                                continue;
                            if (ekv[0] == "songName") t.song = std::string{ekv[1]};
                            else if (ekv[0] == "vol") t.vol = num(ekv[1]);
                            else if (ekv[0] == "prio") t.prio = num(ekv[1]);
                            else if (ekv[0] == "fadeIn") t.fade_in = num(ekv[1]);
                            else if (ekv[0] == "fadeOut") t.fade_out = num(ekv[1]);
                            else if (ekv[0] == "rooms") t.rooms = int16_t(num(ekv[1]));
                        }
                    } else if (kv[0] == "multiUse") {
                        t.multi_use = num(kv[1]) != 0.f;
                    } else if (kv[0] == "delay") {
                        t.delay = int16_t(num(kv[1]));
                    } else if (kv[0] == "fireChance") {
                        t.fire_chance = num(kv[1]);
                    } else if (kv[0] == "pos" && kv.size() >= 3) {
                        t.pos = rw::vec2{num(kv[1]), num(kv[2])};
                    } else if (kv[0] == "handlePos" && kv.size() >= 3) {
                        handle = rw::vec2{num(kv[1]), num(kv[2])};
                    }
                }
                // SpotTrigger.rad is the length of the radius handle
                t.rad = rw::magnitude(handle);
                if (music)
                    out.push_back(std::move(t));
            }
        }

        // The "PlacedObjects:" line of a room settings file
        // (RoomSettings.LoadPlacedObjects): ", " separated objects of
        // "<type><x><y><data>", the data '~' separated. Only the rot objects
        // (ResizableObjectData "<handle x>~<handle y>", CustomRotData appends
        // its fields) and the Watcher's warps are kept; slugcat filters
        // aren't applied.
        void parse_placed_objects(std::string_view text, parsed_room& r) {
            auto& out = r.rot;
            constexpr std::string_view key = "PlacedObjects: ";
            size_t at = text.find(key);
            while (at != std::string_view::npos && at != 0 && text[at - 1] != '\n')
                at = text.find(key, at + 1);
            if (at == std::string_view::npos)
                return;
            std::string_view line = text.substr(at + key.size());
            line = line.substr(0, line.find('\n'));
            auto const split = [](std::string_view sv, std::string_view sep) {
                std::vector<std::string_view> r;
                for (size_t p = 0;;) {
                    size_t const q = sv.find(sep, p);
                    r.push_back(sv.substr(p, q == std::string_view::npos ? std::string_view::npos : q - p));
                    if (q == std::string_view::npos)
                        return r;
                    p = q + sep.size();
                }
            };
            auto const num = [](std::string_view sv) { return std::strtof(std::string{sv}.c_str(), nullptr); };
            for (auto entry : split(line, ", ")) {
                while (!entry.empty() && (entry.front() == ' ' || entry.front() == '\r'))
                    entry.remove_prefix(1);
                while (!entry.empty() && (entry.back() == ' ' || entry.back() == '\r' || entry.back() == ','))
                    entry.remove_suffix(1);
                auto const parts = split(entry, "><");
                if (parts.size() < 4)
                    continue;
                if (parts[0] == "WarpPoint") {
                    // WarpPointData.FromString
                    auto const d = split(parts[3], "~");
                    if (d.size() < 10)
                        continue;
                    parsed_warp w;
                    w.pos = rw::vec2{num(parts[1]), num(parts[2])};
                    if (d[5] != "NULL" && !d[5].empty())
                        w.dest_room = text::to_lowercase_native(d[5]);
                    if (d[6] != "NULL" && d[7] != "NULL")
                        w.dest_pos = rw::vec2{num(d[6]), num(d[7])};
                    // the uuid seeds the dynamic target picks
                    for (char const c : d[8])
                        w.seed = w.seed * 31u + uint8_t(c);
                    w.any_slugcat = d[9] == "AnySlugcat";
                    auto const knob = [&](size_t i, uint8_t& v) {
                        if (d.size() > i)
                            v = uint8_t(std::clamp(int(num(d[i])), 0, 15));
                    };
                    knob(10, w.effect.vignette);
                    knob(11, w.effect.darkness);
                    knob(12, w.effect.spiral_gap);
                    knob(13, w.effect.swirl);
                    knob(14, w.effect.lensing);
                    knob(15, w.effect.noise);
                    knob(16, w.effect.twist);
                    knob(17, w.effect.agitation);
                    knob(18, w.effect.spaghettification);
                    if (d.size() > 19)
                        w.effect.trigger_duration = uint16_t(std::clamp(int(num(d[19])), 0, 10000));
                    if (d.size() > 20)
                        w.effect.active_duration = uint16_t(std::clamp(int(num(d[20])), 0, 10000));
                    auto const flag = [&](size_t i) { return d.size() > i && d[i] == "true"; };
                    w.one_way = flag(24);
                    w.ripple_warp = flag(25);
                    w.one_way_entrance = flag(26);
                    bool const identified = flag(27);
                    w.limited_use = flag(28);
                    if (d.size() > 29)
                        w.uses = int16_t(num(d[29]));
                    // WarpPoint(): an unidentified one way warp is the entrance
                    if (w.one_way && !identified)
                        w.one_way_entrance = true;
                    r.warps.push_back(std::move(w));
                    continue;
                }
                {
                    // food, weapons, pearls, plants, mud and terrain handles
                    static constexpr std::pair<std::string_view, thing_kind> kinds[] = {
                        {"DangleFruit", thing_kind::dangle_fruit}, {"RottenDangleFruit", thing_kind::rotten_fruit},
                        {"Mushroom", thing_kind::mushroom}, {"SlimeMold", thing_kind::slime_mold},
                        {"SeedCob", thing_kind::seed_cob}, {"DeadSeedCob", thing_kind::dead_seed_cob},
                        {"RottenSeedCob", thing_kind::rotten_seed_cob}, {"LillyPuck", thing_kind::lilly_puck},
                        {"DataPearl", thing_kind::data_pearl}, {"UniqueDataPearl", thing_kind::unique_pearl},
                        {"FlareBomb", thing_kind::flare_bomb}, {"MudPit", thing_kind::mud_pit},
                        {"TerrainHandle", thing_kind::terrain_handle},
                        {"ScavengerOutpost", thing_kind::scav_outpost}, {"ScavengerTreasury", thing_kind::scav_treasury},
                        {"ReliableSpear", thing_kind::reliable_spear}, {"MultiplayerItem", thing_kind::multiplayer_item},
                    };
                    auto const it = std::find_if(std::begin(kinds), std::end(kinds), [&](auto const& k) { return k.first == parts[0]; });
                    if (it != std::end(kinds)) {
                        parsed_thing t;
                        t.kind = it->second;
                        t.pos = rw::vec2{num(parts[1]), num(parts[2])};
                        auto const d = parts.size() > 3 ? split(parts[3], "~") : std::vector<std::string_view>{};
                        auto const f = [&](size_t i) { return i < d.size() && !d[i].empty() ? num(d[i]) : 0.f; };
                        t.handle = rw::vec2{f(0), f(1)};
                        if (t.kind == thing_kind::terrain_handle) {
                            t.handle2 = rw::vec2{f(2), f(3)};
                            t.extra = f(4);
                        } else if (t.kind == thing_kind::mud_pit) {
                            t.extra = f(2);
                        } else if ((t.kind == thing_kind::data_pearl || t.kind == thing_kind::unique_pearl) && d.size() > 4) {
                            // DataPearlData: the pearl's type, then whether it lies hidden
                            t.name = text::to_lowercase_native(d[4]);
                            t.extra = f(5) > 0.f ? 1.f : 0.f;
                        } else if (t.kind == thing_kind::scav_outpost) {
                            // ScavengerOutpostData: handle, panel, direction, skullSeed, pearlsSeed
                            t.handle2 = rw::vec2{f(5), f(6)};
                            t.extra = f(4);
                        } else if (t.kind == thing_kind::multiplayer_item) {
                            // MultiplayerItemData: type, panel, chance
                            t.handle = rw::vec2{0.f, 0.f};
                            t.name = d.empty() ? std::string{} : text::to_lowercase_native(d[0]);
                            t.extra = d.size() > 3 ? f(3) : 1.f;
                        }
                        r.things.push_back(std::move(t));
                        continue;
                    }
                }
                if (parts[0] == "CustomDecal") {
                    // QuadObjectData handles, then CustomDecalData.FromString
                    auto const d = split(parts[3], "~");
                    if (d.size() < 12)
                        continue;
                    parsed_decal dc;
                    dc.pos = rw::vec2{num(parts[1]), num(parts[2])};
                    for (int k = 0; k < 3; ++k)
                        dc.handles[k] = rw::vec2{num(d[size_t(k * 2)]), num(d[size_t(k * 2 + 1)])};
                    dc.from_depth = num(d[8]);
                    dc.to_depth = num(d[9]);
                    dc.noise = num(d[10]);
                    dc.image = text::to_lowercase_native(d[11]);
                    if (d.size() >= 20)
                        for (int k = 0; k < 4; ++k)
                            dc.uv[k] = rw::vec2{num(d[size_t(12 + k * 2)]), num(d[size_t(13 + k * 2)])};
                    r.decals.push_back(std::move(dc));
                    continue;
                }
                if (parts[0] == "DynamicWarpTarget") {
                    auto const d = split(parts[3], "~");
                    parsed_warp_target t;
                    t.pos = rw::vec2{num(parts[1]), num(parts[2])};
                    if (d.size() > 2)
                        t.ripple_requirement = num(d[2]);
                    t.dead_end = d.size() > 3 && d[3] == "true";
                    t.bad_warp = d.size() > 4 && d[4] == "true";
                    r.warp_targets.push_back(t);
                    continue;
                }
                parsed_rot o;
                if (parts[0] == "Corruption") o.kind = rot_kind::corruption;
                else if (parts[0] == "PassiveCorruption") o.kind = rot_kind::passive_corruption;
                else if (parts[0] == "CorruptionTube") o.kind = rot_kind::tube;
                else if (parts[0] == "CorruptionDarkness") o.kind = rot_kind::darkness;
                else continue;
                o.pos = rw::vec2{num(parts[1]), num(parts[2])};
                auto const d = split(parts[3], "~");
                if (d.size() >= 2)
                    o.handle = rw::vec2{num(d[0]), num(d[1])};
                // CustomRotData.FromString (panelPos at 2 and 3)
                if (o.kind == rot_kind::passive_corruption && d.size() >= 19) {
                    auto& c = o.custom;
                    c.min_size = num(d[4]);
                    c.max_size = num(d[5]);
                    c.eye_chance = num(d[6]);
                    c.min_eye_size = num(d[7]);
                    c.max_eye_size = num(d[8]);
                    c.min_color = num(d[9]);
                    c.max_color = num(d[10]);
                    c.slowdown = num(d[11]);
                    c.leg_chance = num(d[12]);
                    c.color = d[13] == "EffectColorA" ? rot_color::effect_color_a : d[13] == "EffectColorB" ? rot_color::effect_color_b : rot_color::default_;
                    c.cross_eyes = d[14] == "Cross";
                    c.hostile = d[15] == "Hostile";
                    c.density = num(d[16]);
                    c.darkness_chance = num(d[17]);
                    c.darkness_scale = num(d[18]);
                }
                out.push_back(o);
            }
        }

        // Settings of one room: template (explicit or region default) then the room file
        void resolve_settings(sys::game_state const& s, parsed_room& r, layered_fs::directory const& dir, std::string const& region_name,
            template_map const& templates, parsers::room_template const& region_default) {
            auto const f = dir.get_single_file(r.name + "_settings.txt");
            auto const of = f.open_file(s.fs);
            if (!of) {
                r.settings.apply(region_default);
                return;
            }
            r.has_settings = true;
            r.err.filename = f.path;
            auto const t = parse_template(s, of->to_string_view(), r.err);
            parse_triggers(of->to_string_view(), r.triggers);
            parse_placed_objects(of->to_string_view(), r);
            auto const lname = text::to_lowercase_native(t.template_);
            if (lname == "none") {
                // explicitly no template
            } else if (!lname.empty()) {
                // "SU_SettingsTemplate_Inside": vanilla takes the third '_' piece
                std::string_view piece{lname};
                for (int i = 0; i < 2 && piece.find('_') != std::string_view::npos; ++i)
                    piece.remove_prefix(piece.find('_') + 1);
                piece = piece.substr(0, piece.find('_'));
                auto const key = region_name + "_settingstemplate_" + std::string{piece};
                if (auto const it = templates.find(key); it != templates.end())
                    r.settings.apply(it->second);
                else
                    r.err.add_warning("can't find settings template \"" + t.template_ + "\"");
            } else {
                r.settings.apply(region_default);
            }
            r.settings.apply(t); // room specific values override the template
        }

        // world_<region>.txt ROOMS section: "ROOM : CONN, CONN, DISCONNECTED : ATTR".
        // Connection i is where room exit i (exitAndDenIndex[i]) leads; gate
        // rooms appear in both regions with the far side DISCONNECTED, so the
        // files are merged.
        void load_world_files(sys::game_state& s, layered_fs::directory const& world_dir, parsers::error_handler& err) {
            auto& dc = s.world;
            auto const trim = [](std::string_view v) {
                while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
                    v.remove_prefix(1);
                while (!v.empty() && (v.back() == ' ' || v.back() == '\t'))
                    v.remove_suffix(1);
                return v;
            };
            uint32_t links = 0;
            dc.for_each_region([&](dcon::region_id region) {
                if (dc.region_get_is_pseudo(region))
                    return;
                std::string const name{s.to_string_view(dc.region_get_name(region))};
                auto const f = world_dir.get_single_subdir(name).get_single_file("world_" + name + ".txt");
                auto const of = f.open_file(s.fs);
                if (!of) {
                    err.add_standalone_warning("no world file for region \"" + name + "\"");
                    return;
                }
                std::vector<std::string_view> lines;
                split_lines(of->to_string_view(), lines);
                bool in_rooms = false;
                for (auto line : lines) {
                    line = trim(line);
                    if (line == "ROOMS") {
                        in_rooms = true;
                        continue;
                    }
                    if (line == "END ROOMS")
                        break;
                    if (!in_rooms || line.empty() || line.starts_with("//"))
                        continue;
                    // split on " : "
                    std::vector<std::string_view> parts;
                    size_t pos = 0;
                    while (true) {
                        size_t const sep = line.find(':', pos);
                        parts.push_back(trim(line.substr(pos, sep == std::string_view::npos ? std::string_view::npos : sep - pos)));
                        if (sep == std::string_view::npos)
                            break;
                        pos = sep + 1;
                    }
                    auto const rid = s.find_room(parts[0]);
                    if (!rid || parts.size() < 2)
                        continue;
                    for (size_t i = 2; i < parts.size(); ++i) {
                        if (parts[i] == "SHELTER")
                            dc.room_set_is_shelter(rid, true);
                        else if (parts[i] == "GATE")
                            dc.room_set_is_gate(rid, true);
                    }
                    // the room's exits, in exitAndDenIndex order
                    std::vector<dcon::room_exit_id> exits;
                    dc.room_for_each_exit_location(rid, [&](dcon::exit_location_id el) {
                        exits.push_back(dc.exit_location_get_exit(el));
                    });
                    std::sort(exits.begin(), exits.end(), [&](auto a, auto b) { return dc.room_exit_get_index(a) < dc.room_exit_get_index(b); });
                    size_t i = 0, c = 0;
                    std::string_view conns = parts[1];
                    while (c <= conns.size()) {
                        size_t const comma = std::min(conns.find(',', c), conns.size());
                        auto const target_name = trim(conns.substr(c, comma - c));
                        c = comma + 1;
                        if (target_name.empty())
                            continue;
                        if (i < exits.size() && target_name != "DISCONNECTED") {
                            auto const target = s.find_room(target_name);
                            if (target && !dc.room_exit_get_target_from_exit_connection(exits[i])) {
                                dc.room_exit_set_target_from_exit_connection(exits[i], target);
                                ++links;
                            }
                        }
                        ++i;
                    }
                }
            });
            // a RegionGate for every gate room: an ElectricGate if listed in
            // gates/egates.txt, else a WaterGate
            std::vector<std::string> egates;
            if (auto const ef = world_dir.get_single_subdir("gates").get_single_file("egates.txt").open_file(s.fs); ef) {
                std::vector<std::string_view> lines;
                split_lines(ef->to_string_view(), lines);
                for (auto line : lines)
                    if (!(line = trim(line)).empty())
                        egates.push_back(text::to_lowercase_native(line));
            }
            dc.for_each_room([&](dcon::room_id rid) {
                if (!dc.room_get_is_gate(rid))
                    return;
                auto const g = dc.create_gate();
                auto const lname = text::to_lowercase_native(s.to_string_view(dc.room_get_name(rid)));
                dc.gate_set_electric(g, std::find(egates.begin(), egates.end(), lname) != egates.end());
                dc.gate_set_mode(g, gate_mode::middle_closed);
                dc.gate_set_door_closed(g, rw::vec3{0.f, 1.f, 0.f});
                dc.gate_set_door_goal(g, rw::vec3{0.f, 1.f, 0.f});
                dc.try_create_gate_location(g, rid);
            });
            reports::add_log("world files: " + std::to_string(links) + " room connections, " + std::to_string(dc.gate_size()) + " gates");
        }

        struct room_job {
            layered_fs::file file;
            layered_fs::directory dir;
            dcon::region_id region;
            std::string name;
        };
    }

    void load(sys::game_state& s, parsers::error_handler& err) {
        using clock = std::chrono::steady_clock;
        auto const t0 = clock::now();
        auto& dc = s.world;
        auto const world_dir = s.fs.get_root().get_single_subdir("world");
        register_regions(s, world_dir, err);
        if (err.fatal)
            return;

        template_map templates;
        std::vector<parsers::room_template> defaults(dc.region_size());
        dc.for_each_region([&](dcon::region_id r) { register_region_settings(s, world_dir, r, err, templates, defaults); });

        // collect the room files (first occurrence of a name wins, like the
        // shared shelters which appear in several folders)
        std::vector<room_job> jobs;
        std::unordered_map<std::string, bool> seen;
        dc.for_each_region([&](dcon::region_id r) {
            std::string const name{s.to_string_view(dc.region_get_name(r))};
            bool const pseudo = dc.region_get_is_pseudo(r);
            auto const dir = pseudo ? world_dir.get_single_subdir(name) : world_dir.get_single_subdir(name + "-rooms");
            for (auto const& f : dir.get_files(s.fs)) {
                auto const base = f.get_basename();
                if (f.get_extension() != "txt" || base.find("_settings") != std::string::npos)
                    continue;
                if (name == "gates" && !base.starts_with("gate_"))
                    continue;
                if (base.starts_with("world_"))
                    continue; // region world files sometimes sit next to the rooms
                auto room_name = f.get_filename();
                if (!seen.emplace(room_name, true).second)
                    continue;
                jobs.push_back(room_job{f, dir, r, std::move(room_name)});
            }
        });

        // parse everything in parallel...
        std::vector<parsed_room> parsed(jobs.size());
        tbb::parallel_for(tbb::blocked_range<size_t>(0, jobs.size()), [&](tbb::blocked_range<size_t> const& range) {
            for (size_t i = range.begin(); i != range.end(); ++i) {
                auto const& job = jobs[i];
                auto& r = parsed[i];
                r.name = job.name;
                r.region = job.region;
                r.err.filename = job.file.path;
                auto const of = job.file.open_file(s.fs);
                if (!of) {
                    r.err.add_standalone_error("can't open room file \"" + job.file.path + "\"");
                    continue;
                }
                r.ok = parse_geometry(r, of->to_string_view());
                if (r.ok) {
                    std::string const region_name{s.to_string_view(dc.region_get_name(job.region))};
                    resolve_settings(s, r, job.dir, region_name, templates, defaults[size_t(job.region.index())]);
                }
            }
        });
        auto const t1 = clock::now();

        // ...then commit to the data container serially, in a stable order
        for (auto& r : parsed) {
            merge_errors(err, r.err);
            if (r.ok)
                commit_room(s, r);
        }
        load_world_files(s, world_dir, err);
        dc.for_each_region([&](dcon::region_id r) {
            if (!dc.region_get_is_pseudo(r) && dc.region_get_region_membership(r).begin() == dc.region_get_region_membership(r).end())
                err.add_standalone_warning("no rooms found for region \"" + std::string{s.to_string_view(dc.region_get_name(r))} + "\"");
        });

        auto const ms = [](auto d) { return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(d).count()); };
        reports::add_log("loaded " + std::to_string(dc.region_size()) + " regions, " + std::to_string(dc.room_size()) + " rooms, "
            + std::to_string(dc.tile_size()) + " tiles, " + std::to_string(dc.shortcut_size()) + " shortcuts, "
            + std::to_string(dc.room_node_size()) + " nodes in " + ms(clock::now() - t0) + "ms (parallel parse " + ms(t1 - t0) + "ms)");
    }
}
