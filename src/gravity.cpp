#include "gravity.hpp"

#include <cmath>

#include "math.hpp"
#include "player_types.hpp"
#include "creature.hpp"
#include "oracle.hpp"

namespace gravity {
    using namespace rw;

    namespace {
        // the ticks a flicker cycle setup value counts in (vanilla's * 40)
        constexpr int32_t ticks_per_cycle_second = 40;

        // BrokenAntiGravity's constructor: on or off at random. Downpour's
        // regions where the moon's heart decides it (SL, MS) stay off,
        // switched off, unless it's back in (Saint's timeline; the
        // campaigns where the slugcat puts it back aren't tracked).
        // (Rivulet's RM after the energy cell is taken and Spearmaster's
        // DM/LM counting only through the cycle aren't ported.)
        void make_cycle(sys::game_state& state, dcon::region_id region, broken_cycle& c) {
            c = broken_cycle{};
            c.active = true;
            auto const name = state.to_string_view(state.world.region_get_name(region));
            if ((name == "ms" || name == "sl") && state.story_slugcat != ::player::slugcat_name::saint) {
                c.on = false;
                c.frozen = true;
                c.progress = 1.f;
            } else {
                auto r = rw::random(state.seed, state.tick, uint32_t(region.value));
                c.on = r.value() < 0.5f;
            }
            c.from = c.on ? 1.f : 0.f;
            c.to = c.from;
            c.lights = c.to;
            c.lights_get_to = c.to;
        }

        // BrokenAntiGravity.Update (the camera's switch sound and screen
        // shake are the drawing's: switch_shake)
        void update_cycle(sys::game_state& state, dcon::region_id region, broken_cycle& c) {
            auto r = rw::random(state.seed, state.tick, uint32_t(region.value));
            if (c.frozen)
                c.counter = 100;
            else
                --c.counter;
            if (c.counter < 1) {
                c.on = !c.on;
                c.counter = r.range(flicker_cycle_min * ticks_per_cycle_second, flicker_cycle_max * ticks_per_cycle_second);
                c.from = c.to;
                c.to = c.on ? 1.f : 0.f;
                c.progress = 0.f;
            }
            if (c.progress < 1.f) {
                c.progress = mathf::min(1.f, c.progress + 0.008333334f);
                if (r.value() < 0.125f) {
                    float const v = r.value();
                    c.lights_get_to = mathf::lerp(c.from, c.to,
                        mathf::pow(v * mathf::pow(mathf::inverse_lerp(0.f, 0.5f, c.progress), 0.5f), custom::lerp_map(c.progress, 0.f, 0.6f, 1.f, 0.f)));
                }
            } else {
                c.lights_get_to = c.to;
            }
            c.lights = custom::lerp_and_tick(c.lights, c.lights_get_to, 0.15f, 0.00083333335f);
        }
    }

    float current_anti_gravity(broken_cycle const& c) noexcept {
        return mathf::lerp(c.from, c.to, custom::s_curve(mathf::inverse_lerp(0.35f, 1.f, c.progress), 0.5f));
    }

    void update_world(sys::game_state& state) {
        auto& w = state.world;
        // RainCycle.Update's brokenAntiGrav.Update: the regions' cycles
        w.for_each_region([&](dcon::region_id r) {
            auto& c = w.region_get_broken_anti_gravity(r);
            if (c.active)
                update_cycle(state, r, c);
        });
        // AntiGravity.Update in every room with a ZeroG or BrokenZeroG
        // effect (Room.Loaded adds one AntiGravity for either)
        w.for_each_room([&](dcon::room_id room) {
            float const zero_g = w.room_get_zero_g(room);
            float const broken = w.room_get_broken_zero_g(room);
            if (zero_g <= 0.f && broken <= 0.f)
                return;
            float num = zero_g;
            if (num < 1.f)
                num = mathf::lerp(0.f, 0.85f, num);
            if (auto const region = w.room_get_region_from_region_membership(room)) {
                auto& c = w.region_get_broken_anti_gravity(region);
                // AntiGravity's constructor makes the world's cycle
                if (broken > 0.f && !c.active)
                    make_cycle(state, region, c);
                if (c.active)
                    num = mathf::max(num, broken * current_anti_gravity(c));
            }
            w.room_set_gravity(room, 1.f - num);
        });
        // the iterators' chambers (Oracle's constructor turns AntiGravity
        // off; SSOracleBehavior: 1 - working, SLOracleBehavior: 0.2 for an
        // active Moon): an oracle's opinion wins over the room's ZeroG /
        // BrokenZeroG effects in its own chamber
        creature::for_each_of(w, creature::type::oracle, [&](dcon::creature_id c) {
            float const g = oracle::wanted_room_gravity(state, c);
            if (g >= 0.f)
                w.room_set_gravity(creature::room_of(state, c), g);
        });
    }

    broken_cycle const* cycle_of(sys::game_state const& state, dcon::room_id room) noexcept {
        auto const& w = state.world;
        if (!room || w.room_get_broken_zero_g(room) <= 0.f)
            return nullptr;
        auto const region = w.room_get_region_from_region_membership(room);
        if (!region)
            return nullptr;
        auto const& c = w.region_get_broken_anti_gravity(region);
        return c.active ? &c : nullptr;
    }

    float electric_power(sys::game_state const& state, dcon::room_id room) noexcept {
        auto const* c = cycle_of(state, room);
        return c ? c->lights : 1.f;
    }

    float dark_palette(sys::game_state const& state, dcon::room_id room) noexcept {
        auto const* c = cycle_of(state, room);
        return c ? 1.f - c->lights : 0.f;
    }

    bool lights_fully_on(sys::game_state const& state, dcon::room_id room) noexcept {
        auto const* c = cycle_of(state, room);
        return !c || c->lights >= 1.f;
    }

    float switch_shake(sys::game_state const& state, dcon::room_id room) noexcept {
        auto const* c = cycle_of(state, room);
        if (!c || c->frozen || c->progress <= 0.f || c->progress >= 1.f)
            return 0.f;
        return state.world.room_get_broken_zero_g(room) * 0.5f * std::sin(c->progress * mathf::pi);
    }
}
