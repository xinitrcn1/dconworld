#pragma once

// Plain types stored in the data container (see dcon_generated.txt)

#include <cstdint>

namespace gravity {
    // AntiGravity.BrokenAntiGravity: a region's flickering anti gravity, the
    // on/off cycle the rooms with a BrokenZeroG effect follow (vanilla keeps
    // one on the world's RainCycle; here every region has its own, made the
    // first tick a room of the region has the effect)
    struct broken_cycle {
        // ticks to the next switch
        int32_t counter = 0;
        // anti gravity (1: zero gravity) switched from and to, and how far
        // the switch has come (0..1, 1/120 a tick)
        float from = 0.f;
        float to = 0.f;
        float progress = 0.f;
        // the lights flickering along (Room.ElectricPower)
        float lights = 0.f;
        float lights_get_to = 0.f;
        // made (some room of the region has the effect)
        bool active = false;
        bool on = false;
        // never switches (MSC: SL and MS while the moon's heart is out)
        bool frozen = false;
    };
}
