#pragma once

// What a creature living off screen (offscreen.hpp) keeps: the creature's
// `offscreen` property (dcon_generated.txt). All zero for every other
// creature (the ids' zero is "none"): zero is a valid empty state.

#include <cstdint>

#include "dcon_generated_ids.hpp"
#include "math.hpp"

namespace offscreen {
    enum state_flag : uint8_t {
        // the abstract AI sent it back to the offscreen den (the vulture's
        // check room is done, the scavenger's Migrate rolled its den)
        flag_going_home = 1 << 0,
        // a miros bird that has waited for the rest of its flock for long
        flag_impatient = 1 << 1,
        // `exit_tile` is set
        flag_exit_tile = 1 << 2,
    };

    struct state {
        // off screen: the ticks until it may move again (MirosBirdAbstractAI.wait,
        // ScavengerAbstractAI.freeze, the travel along the highway, a meal);
        // a miros bird ready for a raid counts how long it has waited up
        int32_t timer = 0;
        // in a room: the scavenger's dontMigrate, the vulture's timeInRoom
        // (its check room's 620)
        int32_t room_timer = 0;
        // AbstractCreature.personality.dominance (rolled from the seed)
        float dominance = 0.f;
        // the room it comes into next (the vulture's next check room, the
        // raid's room, the squad's destination); a scavenger keeps it in a
        // room until it gets there
        dcon::room_id target;
        // the squad's or the raid's leader (none: on its own)
        dcon::creature_id leader;
        // the raid: the side exit it comes in by and the one it leaves by
        // (borders.hpp's numbers in `target` + 1, 0 none)
        uint8_t entry = 0;
        uint8_t leave = 0;
        uint8_t flags = 0;
        // MirosBirdAbstractAI.Raid's "all of them": the region's miros birds
        // living off screen when the world was made
        uint8_t flock = 0;
        // in a room: the border tile it leaves by (flag_exit_tile)
        rw::ivec2 exit_tile{0, 0};
    };
}
