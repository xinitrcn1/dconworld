#pragma once

// Coalescipede state (Spider.cs, SpiderGraphics.cs): the creature's payload
// (`spider::payload`, the creature_payload union's `spider`) and what its
// data object (`spider_data`, dcon_generated.txt) keeps that doesn't fit in
// it: the random path it crawls along, the chain it keeps (Spider.Centipede,
// when it is the chain's keeper) and the graphics module.
//
// Kept apart from the big spiders (BigSpider/SpitterSpider: `big_spider`).

#include <cstdint>

#include "dcon_generated_ids.hpp"
#include "math.hpp"

namespace spider {
    // payload.chain / banned_chain: no chain
    inline constexpr uint16_t no_chain = 0xFFFF;
    // the most spiders one chain can hold (Centipede.maxSize is 12 to 21,
    // a merge stays below it)
    inline constexpr uint32_t max_chain = 24;

    // payload.flags
    enum flag : uint8_t {
        flag_idle = 1 << 0,
        flag_moving = 1 << 1,
        flag_in_accessible = 1 << 2,   // inAccessibleTerrain
        flag_move_away = 1 << 3,       // moveAwayFromPos != null
        flag_in_right_place = 1 << 4,  // its CentipedePart.inRightPlace
        flag_gfx_attached = 1 << 5,    // graphicsAttachedToBodyChunk != null
    };

    // The MovementConnection types its crawling makes (followingConnection,
    // the path's steps): the four Standard steps, OpenDiagonal and
    // DropToFloor (no pipes: see spider.hpp)
    enum class move : uint8_t { none, standard, diagonal, drop };

    // The creature's payload (at most 64 bytes)
    struct payload {
        // Spider.dragPos, moveAwayFromPos (flag_move_away)
        rw::vec2 drag_pos;
        rw::vec2 move_away_from;
        float blood_lust;
        // lightExp, lightToMove
        float light_exp;
        float light_to_move;
        float death_spasms;
        // Spider.legsPosition (the chain sets it: -1 head .. 1 tail)
        float legs_position;
        // iVars.size (= iVars.dominance)
        float size;
        // followingConnection's DestTile (follow_type none: no connection)
        int16_t follow_x;
        int16_t follow_y;
        // seenNoPreyCounter (saturating)
        uint16_t seen_no_prey;
        // its chain (Spider.centipede): the keeper's creature index
        // (no_chain none), and bannedCentipede
        uint16_t chain;
        uint16_t banned_chain;
        // Creature.leechedOut of its last meal, as far as it is concerned
        dcon::creature_id leeched_out;
        // spider_data
        uint16_t data;
        // idleCounter, outsideAccessibleCounter, noCentipedeCounter (saturating)
        uint8_t idle_counter;
        uint8_t outside_accessible_counter;
        uint8_t no_centipede_counter;
        // its CentipedePart: index in the body, separatedCounter (255: cut off)
        uint8_t chain_order;
        uint8_t separated_counter;
        uint8_t flags;
        // denMovement (-1 stay, 0, 1 home)
        int8_t den_movement;
        move follow_type;
    };

    // A path of connections (Spider.path, pathCount): its start tile and
    // up to `max_path` steps, 4 bits each (0..7 the eight directions of
    // rw::custom::eight_directions, 8 a drop to the floor below)
    inline constexpr uint32_t max_path = 15;
    inline constexpr uint8_t step_drop = 8;
    struct path_state {
        int16_t x = 0;
        int16_t y = 0;
        uint8_t count = 0;
        uint64_t steps = 0;
    };

    // What its crawling keeps besides the payload: the path, the last
    // connection it followed (lastFollowingConnection) and the pipe it
    // last came out of (lastShortCut: its entrance tile, -1 none)
    struct crawl_state {
        path_state path;
        rw::ivec2 last_follow{-1, -1};
        move last_follow_type = move::none;
        rw::ivec2 last_shortcut{-1, -1};
    };

    // Spider.Centipede (SpiderMass): kept by one of its spiders (its
    // `chain`), whose spider_data holds it. The members are the room's
    // spiders pointing at the keeper, in chain_order (body[0] the head)
    struct chain_state {
        dcon::creature_id prey;
        rw::vec2 prey_pos{0.f, 0.f};
        float total_mass = 0.f;
        float light_adaption = 0.f;
        float hunt = 0.f;
        int32_t walk_cycle = 0;
        int32_t counter = 0;
        int32_t prey_visual_counter = 0;
        // ShouldIUpdate: the tick it last updated (+1, 0 never)
        uint32_t last_tick = 0;
        uint8_t max_size = 12;
        uint8_t next_order = 0;
    };

    // a Limb of SpiderGraphics (rad 1, surface friction 0.5, air friction
    // 0.98, hunt speed 15, quickness 0.95; never pushed out of the terrain)
    struct limb {
        rw::vec2 pos{0.f, 0.f};
        rw::vec2 last_pos{0.f, 0.f};
        rw::vec2 vel{0.f, 0.f};
        rw::vec2 absolute_hunt_pos{0.f, 0.f};
        float hunt_speed = 15.f;
        float quickness = 0.95f;
        // Limb.Mode: 0 HuntAbsolutePosition, 1 Dangle
        uint8_t dangle = 1;
    };

    // SpiderGraphics (purely cosmetic): limbs[4, 2], limbGoalDistances,
    // deathLegPositions, bodyDir, the walk cycle, the spider whose front
    // legs hold on to it (behindMeSpider)
    struct graphics_state {
        limb limbs[4][2];
        float limb_goal_distances[4][2] = {};
        rw::vec2 death_leg_positions[4][2] = {};
        rw::vec2 body_dir{0.f, 1.f};
        rw::vec2 last_body_dir{0.f, 1.f};
        float walk_cycle = 0.f;
        float limb_length = 10.f;
        dcon::creature_id behind_me;
        uint8_t legs_position = 0;
        uint8_t last_legs_position = 0;
    };
}
