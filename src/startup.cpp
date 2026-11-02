#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>

#include <SDL3/SDL.h>

#include "startup.hpp"
#include "npc.hpp"
#include "parsers.hpp"
#include "player.hpp"
#include "reports.hpp"
#include "world.hpp"

namespace startup {
    std::string find_streaming_assets() {
        if (char const* env = std::getenv("RW_ROOT"); env != nullptr && *env != '\0')
            return env;
        std::string const home = std::getenv("HOME") != nullptr ? std::getenv("HOME") : "";
        for (auto const& base : {home + "/.local/share/Steam/steamapps/common/Rain World",
                 home + "/.steam/steam/steamapps/common/Rain World",
                 home + "/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/Rain World"}) {
            auto const sa = base + "/RainWorld_Data/StreamingAssets";
            if (std::filesystem::exists(sa + "/world/regions.txt"))
                return sa;
        }
        return {};
    }

    // a comma separated list
    std::vector<std::string> split(std::string const& s) {
        std::vector<std::string> out;
        size_t pos = 0;
        while (pos <= s.size()) {
            size_t const end = std::min(s.find(',', pos), s.size());
            if (end > pos)
                out.push_back(s.substr(pos, end - pos));
            pos = end + 1;
        }
        return out;
    }

    // the NPC slugcats' tracing and debugging options (npc::debug_options);
    // false: not one of them
    bool npc_debug_option(std::string const& a, std::function<std::string()> const& next) {
        auto& d = npc::debug();
        auto const ints = [](std::string const& s) {
            std::vector<int32_t> v;
            for (auto const& x : split(s))
                v.push_back(std::atoi(x.c_str()));
            return v;
        };
        if (a == "--npc-trace") {
            auto const parts = split(next());
            d.trace_room = parts.empty() ? "*" : parts[0];
            if (parts.size() > 1)
                d.trace_every = std::max(1, std::atoi(parts[1].c_str()));
        } else if (a == "--npc-route") {
            d.route = split(next());
        } else if (a == "--npc-maps") {
            d.maps = true;
        } else if (a == "--npc-room") {
            d.room = next();
        } else if (a == "--npc-goal") {
            d.goal = std::atoi(next().c_str());
        } else if (a == "--npc-box") {
            auto const v = ints(next());
            d.box = v.size() == 4;
            if (d.box) {
                d.box_lo = rw::ivec2{v[0], v[1]};
                d.box_hi = rw::ivec2{v[2], v[3]};
            }
        } else if (a == "--npc-edges") {
            auto const parts = split(next());
            if (parts.size() == 5) {
                d.edges_room = parts[0];
                d.edges_lo = rw::ivec2{std::atoi(parts[1].c_str()), std::atoi(parts[2].c_str())};
                d.edges_hi = rw::ivec2{std::atoi(parts[3].c_str()), std::atoi(parts[4].c_str())};
            }
        } else if (a == "--npc-dead-ends") {
            d.dead_ends = true;
        } else if (a == "--npc-unreachable") {
            d.unreachable = true;
        } else if (a == "--npc-list") {
            d.list = true;
        } else if (a == "--npc-guess") {
            d.guess = true;
        } else if (a == "--npc-measure") {
            d.measure = true;
        } else if (a == "--npc-exp") {
            d.experiment = next();
        } else {
            return false;
        }
        return true;
    }

    player::slugcat_name parse_slugcat(std::string n) {
        for (auto& c : n)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        if (n == "white") return player::slugcat_name::white;
        if (n == "yellow") return player::slugcat_name::yellow;
        if (n == "red") return player::slugcat_name::red;
        if (n == "inv") return player::slugcat_name::inv;
        for (uint32_t i = 0; i < uint32_t(player::slugcat_name::count); ++i) {
            std::string name = player::to_string(player::slugcat_name(i));
            for (auto& c : name)
                c = char(std::tolower(static_cast<unsigned char>(c)));
            if (name == n)
                return player::slugcat_name(i);
        }
        return player::slugcat_name::white;
    }

    std::unique_ptr<sys::game_state> load_world(std::vector<std::string> const& roots, bool clock_seed, bool log) {
        auto ptr = sys::make_game_state();
        auto& g = *ptr;
        // the world's seed, once, before anything rolls
        if (clock_seed)
            g.seed ^= uint32_t(SDL_GetTicksNS() * 2654435761ull) | 1u;
        // our own assets (HUD font) come before so the game data can be overriden
        if (char const* base = SDL_GetBasePath(); base != nullptr)
            g.fs.add_root(std::string{base} + "assets");
        for (auto const& r : roots) {
            if (log)
                reports::add_log("root: " + r);
            g.fs.add_root(r);
        }
        g.fs.generate_unpopulated_tree();
        parsers::error_handler err;
        world::load(g, err);
        if (log && !err.accumulated_errors.empty())
            reports::add_log("errors:\n" + err.accumulated_errors);
        if (log && !err.accumulated_warnings.empty())
            reports::add_log("warnings:\n" + err.accumulated_warnings);
        if (err.fatal || g.world.room_size() == 0)
            reports::error_fatal("no rooms loaded");
        return ptr;
    }
}
