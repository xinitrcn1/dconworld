#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

#include "rain.hpp"
#include "creature.hpp"
#include "dens.hpp"
#include "player.hpp"
#include "rooms.hpp"

namespace rain {
    using rw::ivec2;
    using rw::vec2;
    using world::danger_type;
    using world::room_view;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        // the region whose cycle a room follows
        dcon::region_id region_of(sys::game_state const& state, dcon::room_id room) noexcept {
            return room ? state.world.room_get_rain_region(room) : dcon::region_id{};
        }

        // RoomRain's dangerType as the room's RoomRain sees it, and whether
        // the room has a RoomRain at all (shelters and the other danger types
        // don't: Room.Loaded)
        bool has_room_rain(sys::game_state const& state, dcon::room_id room) noexcept {
            auto const& w = state.world;
            if (w.room_get_is_shelter(room))
                return false;
            switch (w.room_get_danger_type(room)) {
            case danger_type::rain:
            case danger_type::flood:
            case danger_type::flood_and_rain:
            case danger_type::aerie_blizzard:
                return true;
            default:
                return false;
            }
        }
        bool floods(danger_type d) noexcept { return d == danger_type::flood || d == danger_type::flood_and_rain; }

        // GlobalRain.OutsidePushAround / InsidePushAround
        float outside_push_around(cycle const& c) noexcept { return mathf::pow(mathf::inverse_lerp(0.35f, 0.7f, c.intensity), 0.8f); }
        float inside_push_around(cycle const& c) noexcept { return mathf::pow(mathf::inverse_lerp(0.63f, 0.98f, c.intensity), 3.5f); }

        // RainCycle.GetDesiredCycleLength's story mode cuts: Rivulet's
        // timeline before the energy cell is taken, Saint's blizzard
        int32_t desired_cycle_length(sys::game_state const& state, dcon::region_id region, int32_t base) {
            using ::player::slugcat_name;
            auto const name = state.to_string_view(state.world.region_get_name(region));
            int32_t n = base;
            if (state.story_slugcat == slugcat_name::rivulet)
                n = int32_t(float(n) * (name == "vs" || name == "uw" || name == "sh" || name == "sb" || name == "sl" ? 0.5f : 0.33f));
            // (Region.IsRubiconRegion: HR)
            if (state.story_slugcat == slugcat_name::saint && name != "hr")
                n = int32_t(float(n) * 0.7f);
            return n;
        }

        // GlobalRain.DeathRain's phases, in the order NextDeathRainMode
        // walks them from `first`
        int32_t path_of(death_mode first, death_mode* out) {
            int32_t n = 0;
            out[n++] = first;
            // AlternateBuildUp goes on to GradeAPlateu, the rest in order
            if (first == death_mode::alternate_build_up)
                out[n++] = death_mode::grade_a_plateu;
            for (auto m = death_mode(uint8_t(out[n - 1]) + 1); m != death_mode::mayhem; m = death_mode(uint8_t(m) + 1))
                out[n++] = m;
            out[n++] = death_mode::mayhem;
            return n;
        }

        // NextDeathRainMode's timeInThisMode roll for `m`
        float vanilla_time_in_mode(death_mode m, rw::rng& r) {
            switch (m) {
            case death_mode::calm_before_storm: return mathf::lerp(400.f, 800.f, r.value());
            case death_mode::grade_a_build_up: return 6.f;
            case death_mode::grade_a_plateu: return mathf::lerp(400.f, 600.f, r.value());
            case death_mode::grade_b_build_up:
            case death_mode::grade_b_plateu: return r.value() < 0.5f ? 100.f : mathf::lerp(50.f, 300.f, r.value());
            case death_mode::final_build_up: return r.value() < 0.5f ? mathf::lerp(300.f, 500.f, r.value()) : mathf::lerp(100.f, 800.f, r.value());
            case death_mode::alternate_build_up: return mathf::lerp(400.f, 1200.f, r.value());
            default: return 1.f;
            }
        }

        // GlobalRain.InitDeathRain: the DeathRain's first mode, and how much
        // faster than vanilla its phases have to run to fit the window.
        // Vanilla's phases last 1300 to 3800 ticks before Mayhem, which then
        // never ends; here they get `death_rain_window` ticks between them
        // (CalmBeforeStorm at most `calm_ticks`, it isn't deadly) in their
        // vanilla proportions, and Mayhem the rest of `deadly_rain_ticks`.
        // Every phase rolls its vanilla length now, in path order, and runs
        // `time_scale` times faster; the flood rises that much faster too.
        void init_death_rain(cycle& c) {
            // (without Downpour the 30% that would be Pulses is AlternateBuildUp)
            death_mode const first = c.random.value() < 0.7f ? death_mode::alternate_build_up : death_mode::calm_before_storm;
            death_mode path[10];
            int32_t const n = path_of(first, path);
            float calm = 0.f, rest = 0.f;
            float lengths[10] = {};
            for (int32_t i = 0; i + 1 < n; ++i) {
                lengths[i] = vanilla_time_in_mode(path[i], c.random);
                (path[i] == death_mode::calm_before_storm ? calm : rest) += lengths[i];
            }
            float const window_calm = calm > 0.f ? float(calm_ticks) : 0.f;
            c.time_scale = rest / mathf::max(1.f, float(death_rain_window) - window_calm);
            for (auto& t : c.phase_ticks)
                t = 1.f;
            for (int32_t i = 0; i + 1 < n; ++i)
                c.phase_ticks[uint8_t(path[i])] = path[i] == death_mode::calm_before_storm ? window_calm : mathf::max(1.f, lengths[i] / c.time_scale);
            c.calm_before_storm_sunlight = c.random.value() < 0.5f ? 0.f : c.random.value();
            c.mode = first;
            c.progression = 0.f;
            c.time_in_this_mode = c.phase_ticks[uint8_t(first)];
            c.death_rain_ticks = 0;
        }

        // NextDeathRainMode (story mode, no Downpour)
        void next_death_rain_mode(cycle& c) {
            if (c.mode == death_mode::mayhem)
                return;
            if (c.mode == death_mode::alternate_build_up)
                c.mode = death_mode::grade_a_plateu;
            else
                c.mode = death_mode(uint8_t(c.mode) + 1);
            c.progression = 0.f;
            if (c.mode == death_mode::grade_a_build_up)
                c.shader_light = -1.f;
            c.time_in_this_mode = c.phase_ticks[uint8_t(c.mode)];
        }

        // DeathRain.DeathRainUpdate
        void death_rain_update(cycle& c) {
            c.progression += 1.f / c.time_in_this_mode;
            bool next = false;
            if (c.progression > 1.f) {
                c.progression = 1.f;
                next = true;
            }
            if (c.mode == death_mode::calm_before_storm)
                c.rumble_sound = mathf::max(c.rumble_sound - 0.025f, 0.f);
            else
                c.rumble_sound = mathf::lerp(c.rumble_sound, 1.f - mathf::inverse_lerp(0.f, 0.6f, rain_approaching(c)), 0.2f);
            switch (c.mode) {
            case death_mode::calm_before_storm:
                c.intensity = mathf::pow(mathf::inverse_lerp(0.15f, 0.f, c.progression), 1.5f) * 0.24f;
                c.shader_light = -1.f + 0.3f * std::sin(mathf::inverse_lerp(0.03f, 0.8f, c.progression) * 3.1415927f) * c.calm_before_storm_sunlight;
                c.bullet_rain_density = mathf::pow(mathf::inverse_lerp(0.3f, 1.f, c.progression), 8.f);
                break;
            case death_mode::grade_a_build_up:
                c.intensity = c.progression * 0.6f;
                c.micro_screen_shake = c.progression * 1.5f;
                c.bullet_rain_density = 1.f - c.progression;
                break;
            case death_mode::grade_b_build_up:
                c.intensity = mathf::lerp(0.6f, 0.71f, c.progression);
                c.micro_screen_shake = mathf::lerp(1.5f, 2.1f, c.progression);
                c.screen_shake = c.progression * 1.2f;
                break;
            case death_mode::final_build_up:
                c.intensity = mathf::lerp(0.71f, 1.f, c.progression);
                c.micro_screen_shake = mathf::lerp(2.1f, 4.f, mathf::pow(c.progression, 1.2f));
                c.screen_shake = mathf::lerp(1.2f, 3.f, c.progression);
                break;
            case death_mode::alternate_build_up:
                c.intensity = mathf::lerp(0.24f, 0.6f, c.progression);
                c.micro_screen_shake = 1.f + c.progression * 0.5f;
                break;
            default:
                break;
            }
            if (next)
                next_death_rain_mode(c);
        }

        // RainCycle.Update + GlobalRain.Update for one cycle (story mode,
        // no precycles); returns true when the rain ended and a new cycle
        // started
        bool update_cycle(sys::game_state& state, dcon::region_id region, cycle& c) {
            // GlobalRain.Update: the rain's direction wanders
            if (c.random.value() < 0.025f)
                c.rain_direction_get_to = mathf::lerp(-1.f, 1.f, c.random.value());
            c.last_rain_direction = c.rain_direction;
            c.rain_direction = mathf::lerp(c.rain_direction, c.rain_direction_get_to, 0.01f);
            c.rain_direction = c.rain_direction < c.rain_direction_get_to ? mathf::min(c.rain_direction + 0.0125f, c.rain_direction_get_to)
                : mathf::max(c.rain_direction - 0.0125f, c.rain_direction_get_to);
            if (c.mode != death_mode::none) {
                death_rain_update(c);
                // the flood (story mode: once the build up is past grade A)
                bool const flag = c.mode != death_mode::calm_before_storm && c.mode != death_mode::grade_a_build_up;
                if (flag)
                    c.flood_speed = mathf::min(0.8f, c.flood_speed + 0.0025f * c.time_scale);
                c.flood += c.flood_speed * c.time_scale;
            } else if (c.drain_ticks > 0) {
                // after the rain: everything settles, the flood drains away
                c.intensity *= 0.97f;
                c.rumble_sound *= 0.97f;
                c.screen_shake *= 0.95f;
                c.micro_screen_shake *= 0.95f;
                c.bullet_rain_density = 0.f;
                c.flood -= c.flood / float(c.drain_ticks);
                if (--c.drain_ticks == 0) {
                    c.flood = 0.f;
                    c.flood_speed = 0.f;
                    c.shader_light = -1.f;
                }
            } else {
                c.intensity = mathf::inverse_lerp(600.f, 200.f, float(time_until_rain(c))) * 0.24f;
                c.bullet_rain_density = 0.f;
            }

            // RainCycle.Update
            ++c.timer;
            if (c.timer >= c.sun_down_start_time)
                ++c.day_night_counter;
            if (!c.death_rain_has_hit && c.timer >= c.cycle_length && state.world.region_get_never_rains(region)) {
                // (where it never rains nothing hits: the next cycle)
                new_cycle(state, region);
                return true;
            }
            if (!c.death_rain_has_hit && c.timer >= c.cycle_length) {
                // RainHit
                init_death_rain(c);
                c.death_rain_has_hit = 1;
            } else if (c.death_rain_has_hit && ++c.death_rain_ticks >= deadly_rain_ticks) {
                // the short rain is over: the next cycle, right away
                c.mode = death_mode::none;
                c.death_rain_has_hit = 0;
                c.death_rain_ticks = 0;
                c.drain_ticks = flood_drain_ticks;
                ++c.cycles;
                ++stats().cycles_finished;
                new_cycle(state, region);
                return true;
            }
            return false;
        }
    }

    namespace {
        // the room's region; a gate or a shared shelter (pseudo regions)
        // follows a region it connects to, one where it rains if any
        void assign_room(sys::game_state& state, dcon::room_id room) {
            auto& w = state.world;
            auto r = w.room_get_region_from_region_membership(room);
            if (r && w.region_get_is_pseudo(r)) {
                dcon::region_id best;
                w.room_for_each_exit_location(room, [&](dcon::exit_location_id el) {
                    auto const target = w.room_exit_get_target_from_exit_connection(w.exit_location_get_exit(el));
                    auto const tr = target ? w.room_get_region_from_region_membership(target) : dcon::region_id{};
                    if (!tr || w.region_get_is_pseudo(tr))
                        return;
                    if (!best || (w.region_get_never_rains(best) && !w.region_get_never_rains(tr)))
                        best = tr;
                });
                r = best ? best : r;
            }
            w.room_set_rain_region(room, r);
            w.room_set_rain_now(room, 0.f);
            w.room_set_flood_level(room, std::numeric_limits<float>::lowest());
        }
    }

    void add_rooms(sys::game_state& state, std::span<dcon::room_id const> rooms) {
        for (auto const r : rooms)
            assign_room(state, r);
    }

    counters& stats() noexcept {
        static counters c;
        return c;
    }

    float rain_approaching(cycle const& c) noexcept {
        return mathf::inverse_lerp(0.f, 2400.f, float(time_until_rain(c)));
    }

    cycle const* of_room(sys::game_state const& state, dcon::room_id room) noexcept {
        auto const r = region_of(state, room);
        return r ? &state.world.region_get_rain(r) : nullptr;
    }

    float tracker_utility(sys::game_state const& state, dcon::room_id room) noexcept {
        auto const r = region_of(state, room);
        if (!r || state.world.region_get_never_rains(r))
            return 0.f;
        cycle const& c = state.world.region_get_rain(r);
        return mathf::inverse_lerp(1.f, 0.f, custom::s_curve(mathf::inverse_lerp(800.f, 4000.f, float(time_until_rain(c))), 0.1f));
    }

    bool stay_in_den(sys::game_state const& state, dcon::room_id room) noexcept {
        auto const r = region_of(state, room);
        if (!r || state.world.region_get_never_rains(r))
            return false;
        cycle const& c = state.world.region_get_rain(r);
        return time_until_rain(c) < 60 * 40 || c.death_rain_has_hit;
    }

    bool rains_in(sys::game_state const& state, dcon::room_id room) noexcept {
        auto const r = region_of(state, room);
        return r && !state.world.region_get_never_rains(r) && has_room_rain(state, room);
    }

    std::vector<int16_t> const& rain_reach(sys::game_state& state, room_view const& room) {
        auto& rt = state.runtime(room.id);
        if (rt.rain_reach.size() != size_t(room.width)) {
            // RoomRain.GenerateShelterTex: the first solid tile down each column
            rt.rain_reach.assign(size_t(room.width), int16_t(-1));
            for (int32_t x = 0; x < room.width; ++x)
                for (int32_t y = room.height - 1; y >= 0; --y)
                    if (room.get_tile(x, y).solid()) {
                        rt.rain_reach[size_t(x)] = int16_t(y);
                        break;
                    }
        }
        return rt.rain_reach;
    }

    std::vector<std::pair<dcon::region_id, dcon::region_id>> gate_links(sys::game_state const& state) {
        auto const& w = state.world;
        std::vector<std::pair<dcon::region_id, dcon::region_id>> links;
        w.for_each_room([&](dcon::room_id room) {
            if (!w.room_get_is_gate(room))
                return;
            // the regions on either side: where the gate's exits lead
            std::vector<dcon::region_id> sides;
            w.room_for_each_exit_location(room, [&](dcon::exit_location_id el) {
                auto const target = w.room_exit_get_target_from_exit_connection(w.exit_location_get_exit(el));
                auto const r = target ? w.room_get_region_from_region_membership(target) : dcon::region_id{};
                if (r && !w.region_get_is_pseudo(r) && std::find(sides.begin(), sides.end(), r) == sides.end())
                    sides.push_back(r);
            });
            for (size_t i = 0; i < sides.size(); ++i)
                for (size_t j = i + 1; j < sides.size(); ++j)
                    if (!w.region_get_never_rains(sides[i]) && !w.region_get_never_rains(sides[j]))
                        links.emplace_back(sides[i], sides[j]);
        });
        return links;
    }

    void group(sys::game_state& state, std::vector<std::pair<dcon::region_id, dcon::region_id>> const& links) {
        auto& w = state.world;
        // union-find, the smallest region index as every set's root: the
        // first region of the group in region order leads it
        std::vector<uint32_t> parent(size_t(w.region_size()));
        std::iota(parent.begin(), parent.end(), 0u);
        auto const find = [&](uint32_t a) {
            while (parent[a] != a)
                a = parent[a] = parent[parent[a]];
            return a;
        };
        for (auto const& [a, b] : links) {
            if (w.region_get_never_rains(a) || w.region_get_never_rains(b))
                continue;
            uint32_t const ra = find(uint32_t(a.index())), rb = find(uint32_t(b.index()));
            parent[std::max(ra, rb)] = std::min(ra, rb);
        }
        w.for_each_region([&](dcon::region_id r) {
            w.region_set_rain_leader(r, dcon::region_id{dcon::region_id::value_base_t(find(uint32_t(r.index())))});
        });
    }

    void new_cycle(sys::game_state& state, dcon::region_id region) {
        using ::player::slugcat_name;
        auto& c = state.world.region_get_rain(region);
        // World's constructor: Monk, Rivulet, Gourmand and Saint get longer cycles
        auto const s = state.story_slugcat;
        float const v = c.random.value();
        float const t = (s == slugcat_name::yellow || s == slugcat_name::rivulet || s == slugcat_name::gourmand || s == slugcat_name::saint)
            ? 0.35f + 0.65f * mathf::pow(v, 1.2f) : v;
        float const minutes = mathf::lerp(cycle_time_min, cycle_time_max, t) / 60.f;
        c.timer = 0;
        c.base_cycle_length = int32_t(minutes * 40.f * 60.f);
        c.sun_down_start_time = int32_t(mathf::lerp(float(c.base_cycle_length), cycle_time_max * 60.f, mathf::lerp(0.02f, 0.045f, c.random.value())));
        c.cycle_length = desired_cycle_length(state, region, c.base_cycle_length);
        c.day_night_counter = 0;
        c.death_rain_has_hit = 0;
        c.death_rain_ticks = 0;
        c.mode = death_mode::none;
    }

    void setup(sys::game_state& state) {
        auto& w = state.world;
        // regions where it never rains: hardly any room has a RoomRain that
        // rains or floods (Five Pebbles: 9 rooms out of 74 lack a DangerType
        // None, the Watcher's dry regions likewise)
        std::vector<uint32_t> raining(size_t(w.region_size()), 0), rooms(size_t(w.region_size()), 0);
        w.for_each_room([&](dcon::room_id room) {
            auto const r = w.room_get_region_from_region_membership(room);
            if (!r)
                return;
            ++rooms[size_t(r.index())];
            if (has_room_rain(state, room))
                ++raining[size_t(r.index())];
        });
        w.for_each_region([&](dcon::region_id r) {
            w.region_set_never_rains(r, !w.region_get_is_pseudo(r) && rooms[size_t(r.index())] > 0
                && float(raining[size_t(r.index())]) < dry_region_share * float(rooms[size_t(r.index())]));
        });
        w.for_each_room([&](dcon::room_id room) { assign_room(state, room); });
        group(state, gate_links(state));
        // every region its own generator from the world seed and its name,
        // and its first cycle
        w.for_each_region([&](dcon::region_id r) {
            uint32_t h = 2166136261u;
            for (char const ch : state.to_string_view(w.region_get_name(r)))
                h = (h ^ uint8_t(ch)) * 16777619u;
            h ^= state.random.state * 0x2C1B3C6Du;
            cycle c;
            c.random = rw::rng{h != 0 ? h : 0x3A1F00D5u};
            w.region_set_rain(r, c);
            new_cycle(state, r);
        });
        // the others run their leader's cycle
        w.for_each_region([&](dcon::region_id r) {
            auto const leader = w.region_get_rain_leader(r);
            if (leader && leader != r)
                w.region_set_rain(r, w.region_get_rain(leader));
        });
    }

    std::string describe_groups(sys::game_state const& state) {
        auto const& w = state.world;
        std::string groups, dry;
        uint32_t n = 0;
        std::vector<uint32_t> raining(size_t(w.region_size()), 0), rooms(size_t(w.region_size()), 0);
        w.for_each_room([&](dcon::room_id room) {
            if (auto const r = w.room_get_region_from_region_membership(room)) {
                ++rooms[size_t(r.index())];
                raining[size_t(r.index())] += has_room_rain(state, room) ? 1 : 0;
            }
        });
        w.for_each_region([&](dcon::region_id leader) {
            if (w.region_get_is_pseudo(leader) || w.region_get_rain_leader(leader) != leader)
                return;
            if (w.region_get_never_rains(leader)) {
                dry += std::string{dry.empty() ? "" : ", "} + std::string{state.to_string_view(w.region_get_name(leader))} + " ("
                    + std::to_string(raining[size_t(leader.index())]) + "/" + std::to_string(rooms[size_t(leader.index())]) + " rooms with rain)";
                return;
            }
            std::string members;
            w.for_each_region([&](dcon::region_id r) {
                if (r != leader && w.region_get_rain_leader(r) == leader && !w.region_get_is_pseudo(r))
                    members += " " + std::string{state.to_string_view(w.region_get_name(r))};
            });
            ++n;
            groups += "\n  " + std::string{state.to_string_view(w.region_get_name(leader))} + " (" + std::to_string(w.region_get_rain(leader).cycle_length / 40)
                + " s):" + members;
        });
        return "rain: " + std::to_string(n) + " groups of regions linked by gates (leader, its first cycle: the others)" + groups + "\n  never rains: " + (dry.empty() ? "-" : dry);
    }

    void rain_in(sys::game_state& state, dcon::region_id region, int32_t ticks) {
        auto& w = state.world;
        if (!region)
            return;
        auto const leader = w.region_get_rain_leader(region) ? w.region_get_rain_leader(region) : region;
        auto& c = w.region_get_rain(leader);
        if (c.death_rain_has_hit)
            return;
        c.timer = std::max(0, c.cycle_length - ticks);
    }

    void update_world(sys::game_state& state) {
        auto& w = state.world;
        w.for_each_region([&](dcon::region_id r) {
            auto const leader = w.region_get_rain_leader(r);
            if (leader && leader != r)
                return;
            if (update_cycle(state, r, w.region_get_rain(r)))
                dens::cycle_restarted(state, r);
        });
        w.for_each_region([&](dcon::region_id r) {
            auto const leader = w.region_get_rain_leader(r);
            if (leader && leader != r)
                w.region_set_rain(r, w.region_get_rain(leader));
        });
    }

    void update_room(sys::game_state& state, room_view const& room) {
        auto& w = state.world;
        auto const region = region_of(state, room.id);
        if (!region || !has_room_rain(state, room.id) || w.region_get_never_rains(region))
            return;
        cycle const& c = w.region_get_rain(region);
        danger_type const danger = w.room_get_danger_type(room.id);
        float const room_rain = w.room_get_rain_intensity(room.id);
        // RoomRain.intensity
        float intensity = w.room_get_rain_now(room.id);
        if (danger == danger_type::rain || danger == danger_type::flood_and_rain || danger == danger_type::aerie_blizzard)
            intensity = mathf::lerp(intensity, c.intensity, 0.2f);
        intensity = danger == danger_type::aerie_blizzard ? mathf::min(intensity, 1.f) : mathf::min(intensity, room_rain);
        w.room_set_rain_now(room.id, intensity);

        // ThrowAroundObjects
        float const inside = danger == danger_type::aerie_blizzard ? inside_push_around(c) : danger == danger_type::rain ? inside_push_around(c) * room_rain : 0.f;
        float const outside = danger == danger_type::aerie_blizzard || danger == danger_type::rain || danger == danger_type::flood_and_rain
            ? outside_push_around(c) * room_rain : 0.f;
        bool const push = outside_push_around(c) > 0.f || inside_push_around(c) > 0.f;
        if (push && (danger == danger_type::aerie_blizzard || room_rain >= 0.02f) && room_rain != 0.f) {
            auto const& reach = rain_reach(state, room);
            auto& rng = state.room_random(room.id);
            auto const exposed = [&](ivec2 t) { return reach[size_t(custom::int_clamp(t.x, 0, room.width - 1))] < t.y; };
            // one chunk: its push, and what the rain does to its creature
            // (`main`: Creature.mainBodyChunk; `count`: its bodyChunks)
            auto const throw_chunk = [&](dcon::body_chunk_id k, uint32_t count, bool main, auto&& stun, auto&& rain_death, auto&& die) {
                vec2 const p = w.body_chunk_get_pos(k);
                float const rad = w.body_chunk_get_rad(k);
                float const ox = mathf::lerp(-rad, rad, rng.value());
                ivec2 const t = room_view::get_tile_position(p + vec2{ox, mathf::lerp(-rad, rad, rng.value())});
                float num = inside;
                bool out = false;
                if (exposed(t)) {
                    out = true;
                    num = mathf::max(outside, inside);
                }
                if (room.water)
                    num *= mathf::inverse_lerp(room.float_water_level - 100.f, room.float_water_level, p.y);
                if (!(num > 0.f))
                    return;
                vec2& vel = w.body_chunk_get_vel(k);
                float const mass = w.body_chunk_get_mass(k);
                if (w.body_chunk_get_contact_point(k).y < 0) {
                    int32_t side = 0;
                    if (!exposed(t + ivec2{-1, 0}) && !room.get_tile(t + ivec2{-1, 0}).solid())
                        --side;
                    if (!exposed(t + ivec2{1, 0}) && !room.get_tile(t + ivec2{1, 0}).solid())
                        ++side;
                    vel += custom::deg_to_vec(mathf::lerp(-30.f, 30.f, rng.value()) + float(side * 16)) * rng.value() * (out ? 9.f : 4.f) * num / mass;
                } else {
                    vel.y -= mathf::pow(rng.value(), 5.f) * 16.5f * num / mass;
                }
                if (count > 0) {
                    if (mathf::pow(rng.value(), 1.2f) * 2.f * float(count) < num)
                        stun(rng.range(1, 1 + int32_t(9.f * num)));
                    float& death = rain_death();
                    if (main)
                        death += num / 20.f;
                    if (num > 0.5f && death > 1.f && rng.value() < 0.025f)
                        die();
                }
                vel += custom::deg_to_vec(mathf::lerp(90.f, 270.f, rng.value())) * rng.value() * 5.f * inside;
            };
            rooms::for_each_creature(state, room.id, [&](dcon::creature_id cr) {
                auto const p = creature::player_of(state, cr);
                // (a slugcat in a pipe or eaten is out of the rain)
                if (p && (w.creature_get_away(cr) != 0 || w.player_get_eaten(p)))
                    return;
                uint32_t const n = creature::chunk_count(state, cr);
                uint32_t const m = creature::main_chunk(state, cr);
                for (uint32_t i = 0; i < n; ++i)
                    throw_chunk(creature::chunk(state, cr, i), n, i == m, [&](int32_t st) { creature::stun(state, cr, st); },
                        [&]() -> float& { return w.creature_get_rain_death(cr); }, [&]() {
                            if (!w.creature_get_dead(cr))
                                ++(p ? stats().players_killed : stats().creatures_killed);
                            creature::die(state, cr);
                        });
            });
            rooms::for_each_item(state, room.id, [&](dcon::item_id it) {
                // (what a slugcat holds moves with its hands)
                if (w.item_get_grabbed_by(it))
                    return;
                float dummy = 0.f;
                w.physical_object_for_each_chunk_ownership_as_owner(w.item_get_body_from_item_body(it), [&](dcon::chunk_ownership_id co) {
                    throw_chunk(w.chunk_ownership_get_chunk(co), 0, false, [](int32_t) {}, [&]() -> float& { return dummy; }, [] {});
                });
            });
        }
        // Creature.Update's rainDeath recovery and Player.LungUpdate's cap
        rooms::for_each_creature(state, room.id, [&](dcon::creature_id cr) {
            float& d = w.creature_get_rain_death(cr);
            d = mathf::clamp(d - 0.0125f, 0.f, 1.f);
            if (auto const p = creature::player_of(state, cr))
                w.player_get_air_in_lungs(p) = mathf::min(w.player_get_air_in_lungs(p), 1.f - d);
        });

        // the flood: the water rises over the room's own level (a room
        // without water gets some from below, Room.AddWater's -200)
        if (floods(danger)) {
            float const original = room.default_water_level >= 0 ? room_view::middle_of_tile(0, room.default_water_level).y
                : room_view::middle_of_tile(0, -10).y;
            float& level = w.room_get_flood_level(room.id);
            if (c.flood > 0.f) {
                float const target = mathf::min(original + c.flood, room.pixel_height() + 500.f);
                level = level == std::numeric_limits<float>::lowest() ? (room.default_water_level >= 0 ? original : -200.f) : level;
                level = mathf::lerp(level, target, 0.2f);
            } else if (level != std::numeric_limits<float>::lowest()) {
                level = room.default_water_level >= 0 && level <= original + 0.5f ? std::numeric_limits<float>::lowest()
                    : room.default_water_level < 0 && level < -150.f ? std::numeric_limits<float>::lowest() : mathf::lerp(level, original, 0.2f);
            }
        }

        // the loops (DisembodiedDynamicSoundLoops), from the viewed rooms
        if (!state.viewed(room.id))
            return;
        using sound::sound_id;
        bool const flood_only = danger == danger_type::flood;
        auto const loop = [&](uint32_t slot, sound_id id, float vol) {
            sound::loop_request r;
            r.owner = sound::loop_key(sound::loop_owner::rain, uint32_t(room.id.index()), slot);
            r.id = id;
            r.room = uint32_t(room.id.index());
            r.shape = sound::loop_shape::disembodied;
            r.vol = vol;
            state.play_loop(r);
        };
        float const death_vol = mathf::pow(mathf::inverse_lerp(0.35f, 0.75f, intensity), 0.8f);
        if (!flood_only) {
            loop(0, sound_id::normal_rain_loop,
                intensity > 0.f ? 0.1f + 0.9f * mathf::pow(mathf::clamp01(std::sin(mathf::inverse_lerp(0.001f, 0.7f, intensity) * 3.1415927f)), 1.5f) : 0.f);
            loop(1, sound_id::heavy_rain_loop, mathf::pow(mathf::inverse_lerp(0.12f, 0.5f, intensity), 0.85f) * mathf::pow(1.f - death_vol, 0.3f));
        }
        loop(2, flood_only ? sound_id::death_rain_heard_from_underground_loop : sound_id::death_rain_loop, death_vol);
        float const rumble = c.rumble_sound * w.room_get_rumble_intensity(room.id);
        loop(3, sound_id::death_rain_rumble_loop, rumble);
        loop(4, flood_only ? sound_id::death_rain_approaching_heard_from_underground_loop : sound_id::death_rain_approaching_loop,
            mathf::inverse_lerp(1400.f, 0.f, float(time_until_rain(c))) * room_rain);
        if (danger != danger_type::rain && danger != danger_type::aerie_blizzard)
            loop(5, sound_id::flash_flood_loop, mathf::inverse_lerp(0.01f, 0.5f, c.flood_speed * (c.mode != death_mode::none ? 1.f : 0.f)));
        loop(6, sound_id::screen_shake_loop, mathf::min(1.f, c.screen_shake) * (1.f - rumble));
    }
}
