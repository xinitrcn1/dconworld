#pragma once

// Port of the HUD's KarmaMeter (HUD/KarmaMeter.cs) and FoodMeter
// (HUD/FoodMeter.cs with its MeterCircle, QuarterPipShower and the
// HUDCircles they use) for the player's own HUD. The renderer keeps one
// meter_state, steps it once per game tick (HudPart.Update) and builds
// the sprites every frame (HudPart.Draw) in Futile screen space: pixels
// from the bottom left corner, y up, in vanilla's container order (the
// FoodMeter's sprites, then the KarmaMeter's).
//
// Shaders: FlatLight sprites (the dark fades behind the meters, the karma
// glow) are drawn as discs of flat triangles with FlatLight's falloff;
// VectorCircleFadable (a HUDCircle not at its snap radius) as a flat ring
// or disc. Left out: the reinforced karma ring's animations, the red
// blinking at gates, the scavenger flash, force sleep, the pups' meters,
// the sleep/death screens and the fade circles.

#include <cstdint>
#include <string_view>
#include <vector>

#include "math.hpp"
#include "sprites.hpp"

namespace hud {
    constexpr int max_food_circles = 16;

    // what the meters show, read from the game every tick
    struct input {
        int32_t karma = 0;
        int32_t karma_cap = 4;
        bool reinforced = false;  // showAsReinforced (smallKarmaRingReinforced)
        bool ripple = false;      // the Watcher with rippleLevel >= 1: its ripple symbol
        float ripple_level = 1.f;
        int32_t food = 0;         // whole pips
        int32_t quarter_food = 0; // quarterFoodPoints
        int32_t max_food = 7;
        int32_t food_to_hibernate = 4;
        bool reveal = false;      // HUD.owner.RevealMap / showKarmaFoodRain: the meters shown
        bool in_shelter = false;  // in a shelter: the missing pips flash
    };

    // HUDCircle: rad, fade and their last values
    struct circle {
        float rad = 0.f, last_rad = 0.f;
        float fade = 1.f, last_fade = 1.f;
        int32_t color = 0; // Custom.FadableVectorCircleColors index
        bool visible = true;
    };

    // FoodMeter.MeterCircle: [0] FoodCircleA (the ring), [1] FoodCircleB (the pip)
    struct meter_circle {
        circle circles[2];
        float rads[2][2] = {};
        float slow_x_add = 0.f, last_slow_x_add = 0.f;
        bool plopped = false;
        bool food_plopped = false;
        bool eaten = false;
        int32_t food_plop_delay = 0;
        int32_t eat_counter = 0;
    };

    struct food_meter {
        rw::vec2 pos{50.f, 25.f}, last_pos{50.f, 25.f};
        float fade = 0.f, last_fade = 0.f;
        int32_t max_food = 0;
        int32_t survival_limit = 0;
        float show_surv_lim = 0.f, last_show_surv_lim = 0.f;
        int32_t last_count = 0;
        int32_t visible_counter = 0;
        int32_t show_count = 0;
        int32_t show_count_delay = 0;
        int32_t init_plop_circle = 0;
        int32_t init_plop_delay = 0;
        int32_t eat_circle_delay = 0;
        int32_t time_counter = 0;
        float down_in_corner = 0.f;
        meter_circle circles[max_food_circles];
        // QuarterPipShower
        int32_t display_quarter_food = 0;
        int32_t quarter_pip_delay = 0;
        float quarter_pip_sin = 0.f, last_quarter_pip_sin = 0.f;
        float light_up = 0.f, last_light_up = 0.f;
    };

    struct karma_meter {
        rw::vec2 pos{55.01f, 45.01f}, last_pos{55.01f, 45.01f};
        float fade = 0.f, last_fade = 0.f;
        float rad = 22.5f, last_rad = 22.5f;
        float glowy_fac = 0.f, last_glowy_fac = 0.f;
        float reinforcement_cycle = 0.f, last_reinforcement_cycle = 0.f;
        int32_t force_visible_counter = 0;
        int32_t timer = 0;
    };

    struct meter_state {
        bool started = false;
        uint32_t last_tick = 0;
        food_meter food;
        karma_meter karma;
        // the RainMeter's fade (the food meter steps out of its way)
        float rain_fade = 0.f;
    };

    // HUD.Update for every game tick since the last call (a fresh state, a
    // different meter or a tick going backwards starts the meters over)
    void advance(meter_state& m, input const& in, uint32_t tick);
    // one tick of the meters
    void update(meter_state& m, input const& in);

    // the element of the karma meter's symbol (KarmaMeter.KarmaSymbolSprite
    // (small), or RippleSymbolSprite for the Watcher)
    std::string_view symbol_element(input const& in) noexcept;

    // what the atlas has: the symbol and the food circles' elements (else
    // the circles are drawn as their vector circles, and the caller draws
    // its own stand in for the symbol)
    struct atlas_has {
        bool symbol = false;
        bool food_circles = false;
        bool quarter_pips = false;
        bool reinforced_ring = false;
    };

    // HudPart.Draw: the meters' sprites at `time_stacker` between the last
    // two ticks, appended to `out` (Futile screen space)
    void build(meter_state const& m, input const& in, float time_stacker, atlas_has const& has, std::vector<sprites::draw_cmd>& out);

    // the karma meter's draw position, scale (rad / 22.5) and alpha, for a
    // stand in symbol
    struct symbol_place {
        rw::vec2 pos;
        float scale = 1.f;
        float alpha = 0.f;
    };
    symbol_place karma_symbol_place(meter_state const& m, float time_stacker) noexcept;
}
