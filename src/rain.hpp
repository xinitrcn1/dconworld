#pragma once

// The rain: RainCycle, GlobalRain with its DeathRain, and RoomRain.
//
// Every region has its own rain cycle (the region's `rain`, rain_types.hpp)
// running vanilla's story mode logic: a cycle of 400 to 800 seconds rolled
// like World's constructor, the rain approaching over its last minute
// (RainApproaching, GlobalRain.Intensity's build up), then the death rain
// with its phases. Regions linked by a gate (the merged world connections)
// share one timer: the regions of a connected group follow the first of
// them in region order (`rain_leader`), copying its cycle every tick.
// Regions where it never rains (hardly any room with a DangerType that
// rains or floods: Five Pebbles, the Watcher's dry regions) aren't synced and
// don't link the regions around them.
//
// The death rain is short: one to two minutes from the moment it hits (rolled
// every time, `deadly_rain_min`..`deadly_rain_max`), its phases rolled like
// vanilla's DeathRain and fit into the first part of it
// (`death_rain_window`), then the region rolls its next cycle and keeps
// going: no restart, no sleep screen, no karma. The flood it raised drains
// away over `flood_drain_ticks`.
//
// RoomRain runs in every room with its update (in parallel): the room's
// intensity following its region's, the deadly rain pushing, stunning and
// killing what stands outside of shelter (rainReach: under the open sky)
// in Rain and FloodAndRain rooms, the flood rising in Flood and FloodAndRain
// rooms, and the rain, rumble and flood loops of the viewed rooms.

#include <atomic>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "gamestate.hpp"
#include "rain_types.hpp"
#include "world.hpp"

namespace rain {
    // the deadly rain lasts one to two minutes (rolled when it hits), then
    // the cycle starts over
    inline constexpr int32_t deadly_rain_min = 40 * 60;
    inline constexpr int32_t deadly_rain_max = 40 * 120;
    // the death rain's phases before Mayhem share at most this much of it
    // (CalmBeforeStorm at most `calm_share` of it), Mayhem has the rest
    inline constexpr float death_rain_window = 0.75f;
    inline constexpr float calm_share = 0.15f;
    // the flood goes back down over this long after the rain
    inline constexpr int32_t flood_drain_ticks = 400;
    // a region where fewer of its rooms than this have a RoomRain that
    // rains or floods never rains
    inline constexpr float dry_region_share = 0.2f;
    // RainWorld's cycleTimeMin/cycleTimeMax, in seconds
    inline constexpr float cycle_time_min = 400.f;
    inline constexpr float cycle_time_max = 800.f;

    // World loading: which region's cycle every room follows, the regions
    // where it never rains, the groups linked by gates and their leaders,
    // and every leader's first cycle. Set the campaign (story_slugcat) and
    // the seed first
    void setup(sys::game_state& state);
    // rooms added after setup (tests): their region's cycle
    void add_rooms(sys::game_state& state, std::span<dcon::room_id const> rooms);
    // the groups, their leaders and the regions where it never rains (the log)
    std::string describe_groups(sys::game_state const& state);
    // the pairs of regions a gate room connects (both rain)
    std::vector<std::pair<dcon::region_id, dcon::region_id>> gate_links(sys::game_state const& state);
    // groups the regions by `links` and picks each group's leader (setup
    // does this with gate_links; tests pass their own)
    void group(sys::game_state& state, std::vector<std::pair<dcon::region_id, dcon::region_id>> const& links);
    // a fresh cycle for the region (World's constructor: the cycle length,
    // sunDownStartTime), rolled with its cycle's own generator
    void new_cycle(sys::game_state& state, dcon::region_id region);

    // once a tick, before the rooms: every leader's RainCycle and
    // GlobalRain, the others copying their leader's
    void update_world(sys::game_state& state);
    // RoomRain.Update (in the room's update)
    void update_room(sys::game_state& state, world::room_view const& room);

    // the cycle the room follows (none for a room of no region)
    cycle const* of_room(sys::game_state const& state, dcon::room_id room) noexcept;
    inline int32_t time_until_rain(cycle const& c) noexcept { return c.cycle_length - c.timer; }
    // RainCycle.RainApproaching (story mode): 1 far from the rain, 0 when it hits
    float rain_approaching(cycle const& c) noexcept;
    // the death rain is falling
    inline bool deadly(cycle const& c) noexcept { return c.death_rain_has_hit != 0; }
    // RainTracker.Utility (story mode) for a creature in `room`: how badly it
    // wants to get to its den; 0 where it never rains
    float tracker_utility(sys::game_state const& state, dcon::room_id room) noexcept;
    // AbstractCreature.WantToStayInDenUntilEndOfCycle's rain term: less than a
    // minute left before the rain (or it is falling)
    bool stay_in_den(sys::game_state const& state, dcon::room_id room) noexcept;
    // the room gets the deadly rain at all (RoomRain pushes in it)
    bool rains_in(sys::game_state const& state, dcon::room_id room) noexcept;
    // RoomRain.rainReach: per column, the highest solid tile (-1 none); a
    // tile above it is under the open sky
    std::vector<int16_t> const& rain_reach(sys::game_state& state, world::room_view const& room);

    // what the rain killed since start (reports, the fuzz)
    struct counters {
        std::atomic<uint32_t> players_killed{0};
        std::atomic<uint32_t> creatures_killed{0};
        // death rains that came and went (once per group)
        std::atomic<uint32_t> cycles_finished{0};
    };
    counters& stats() noexcept;

    // the debug key: the region's group gets its rain in `ticks`
    void rain_in(sys::game_state& state, dcon::region_id region, int32_t ticks);
}
