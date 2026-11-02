#pragma once

// what starting the game and the test tools (tests/tools.cpp) share: finding
// the game's data, loading the world, the command line's common options
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "gamestate.hpp"
#include "player_types.hpp"

namespace startup {
    // the StreamingAssets folder: $RW_ROOT, else a Steam install's (empty:
    // none found)
    std::string find_streaming_assets();
    // a comma separated list
    std::vector<std::string> split(std::string const& s);
    // the NPC slugcats' tracing and debugging options (npc::debug_options,
    // the --npc-* flags); `next` takes the option's argument. False: not one
    // of them
    bool npc_debug_option(std::string const& a, std::function<std::string()> const& next);
    // --slugcat's names (white, yellow, red, inv or any slugcat's); unknown: white
    player::slugcat_name parse_slugcat(std::string n);
    // a fresh world from `roots` (first has priority; our own assets before
    // them). `clock_seed`: its seed mixed with the clock (a played world);
    // `log`: report the roots and the parse errors. Fatal without rooms
    std::unique_ptr<sys::game_state> load_world(std::vector<std::string> const& roots, bool clock_seed, bool log);
}
