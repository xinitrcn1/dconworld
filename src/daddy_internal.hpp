#pragma once

// What daddy.cpp (DaddyLongLegs, DaddyAI) and daddy_tentacle.cpp (Tentacle,
// DaddyTentacle) share: a daddy's update context with working copies of its
// tentacles' tile lists (they grow and shrink every tick, so they live in
// std::vectors while the daddy updates and go back into the vector pools
// after; the chunks are edited in place), and the Room/SharedPhysics tile
// helpers both use.

#include <cstdint>
#include <source_location>
#include <span>
#include <vector>

#include "daddy.hpp"
#include "math.hpp"
#include "world.hpp"

namespace daddy::detail {
    // one tentacle while its daddy updates
    struct tentacle_work {
        // its creature_part (the stun, the health) and data object
        dcon::creature_part_id part;
        dcon::daddy_tentacle_id id;
        tentacle t;
        // its chunks, in place in the pool (their count never changes)
        std::span<tentacle_chunk> c;
        std::vector<rw::ivec2> segments;
        std::vector<rw::ivec2> grab_path;
    };

    // CreatureTemplate numbers per variant
    struct template_params {
        float damage_resistance;
        float stun_resistance;
        float body_size;
    };
    template_params const& params(variant v) noexcept;

    struct ctx {
        sys::game_state& state;
        dcon::creature_id d;
        world::room_view const& room;
        rw::rng rng;
        body_state& z;
        ai_state& a;
        // the Tracker's current prey (what the body hunts, felt this tick,
        // ticks since and where; ai_state.others are the rest) and the
        // destination
        dcon::creature_id& seen;
        uint8_t& sees;
        int32_t& ticks_since_seen;
        rw::vec2& last_seen_pos;
        rw::ivec2& destination;
        std::vector<tentacle_work>& tents;
        // the `ref List<IntVector2> path` scratch lists
        std::vector<rw::ivec2>& scratch;
        dcon::body_chunk_id chunks[max_chunks];
        uint32_t n = 0;
        variant v = variant::daddy;
        // AImap.getTerrainProximity of the room (pathing's map)
        std::vector<uint16_t> const* prox = nullptr;

        // (keyed by the constructing call site too: several ctxs of one
        // daddy in one tick roll apart)
        ctx(sys::game_state& s, dcon::creature_id id, world::room_view const& r, std::vector<tentacle_work>& t, std::vector<rw::ivec2>& sc,
            std::source_location const l = std::source_location::current());

        rw::vec2& pos(uint32_t i) { return state.world.body_chunk_get_pos(chunks[i]); }
        rw::vec2& vel(uint32_t i) { return state.world.body_chunk_get_vel(chunks[i]); }
        float mass(uint32_t i) const { return state.world.body_chunk_get_mass(chunks[i]); }
        float rad(uint32_t i) const { return state.world.body_chunk_get_rad(chunks[i]); }
        bool size_class() const { return daddy::size_class(v); }
        bool hd() const { return hunter(v); }
        bool mother() const { return v == variant::mother; }
        bool dead() const { return state.world.creature_get_dead(d) != 0; }
        bool consious() const { return !dead() && state.world.creature_get_stun(d) <= 0; }
        // DaddyAI.behavior
        behavior current() const { return behavior(state.world.creature_get_behavior(d)); }
        void set_current(behavior b) { state.world.creature_set_behavior(d, uint8_t(b)); }
        // abstractCreature.pos: the main chunk's tile
        rw::ivec2 here() { return world::room_view::get_tile_position(pos(0)); }
        rw::vec2 middle_of_body();
        float total_mass() const;
        // the tracked slugcat (Tracker's one representation), if any
        dcon::creature_id tracked() const;
        // getTerrainProximity, -1 outside the room
        int32_t proximity(rw::ivec2 t) const {
            if (!room.inside(t.x, t.y))
                return -1;
            return (*prox)[size_t(t.y) * size_t(room.width) + size_t(t.x)];
        }
    };

    // the tentacles' working copies in and out of the data container
    void load(ctx& x);
    void store(ctx& x);

    // a creature or slugcat in the room, out of the pipes and not eaten
    bool target_here(sys::game_state const& state, dcon::creature_id t, dcon::room_id room) noexcept;

    // Room.RayTraceTilesList (fills `path` from the front like vanilla, leaving
    // what is past the returned count), Room.RayTraceTilesForTerrain
    int32_t ray_trace_tiles_list(int32_t x0, int32_t y0, int32_t x1, int32_t y1, std::vector<rw::ivec2>& path);
    bool ray_trace_tiles_for_terrain(world::room_view const& room, rw::ivec2 a, rw::ivec2 b) noexcept;
    // SharedPhysics.RayTracedTilesArray
    void ray_traced_tiles(rw::vec2 a, rw::vec2 b, std::vector<rw::ivec2>& out);

    // NoiseTracker.TheorizedSource by id (null once it is gone)
    noise_source* source(ai_state& a, uint16_t id) noexcept;
    noise_source const* source(ai_state const& a, uint16_t id) noexcept;

    //
    // daddy.cpp: what the tentacles call on their daddy and its AI
    //
    // DaddyAI.UpdateDynamicRelationship
    creature::relationship relationship_to(ctx const& x, dcon::creature_id t);
    // Tracker.SeeCreature (what it doesn't eat is ignored)
    void see(ctx& x, dcon::creature_id t);
    // the tracked prey: the current one (`seen`) and the others
    prey_rep* other(ai_state& a, dcon::creature_id t) noexcept;
    prey_rep const* other(ai_state const& a, dcon::creature_id t) noexcept;
    bool tracks(ctx const& x, dcon::creature_id t);
    uint32_t prey_count(ctx const& x);
    dcon::creature_id prey_at(ctx const& x, uint32_t i);
    void forget(ai_state& a, dcon::creature_id t) noexcept;
    // felt this tick
    bool sees(ctx const& x, dcon::creature_id t);
    // Tracker.CreatureRepresentation.BestGuessForPosition, of the current
    // prey or of `t`
    rw::ivec2 best_guess(ctx& x);
    rw::ivec2 best_guess(ctx& x, dcon::creature_id t);
    rw::vec2 hunted_pos(ctx& x);

    //
    // daddy_tentacle.cpp: Tentacle + DaddyTentacle
    //
    // Tentacle.NewRoom + DaddyTentacle.NewRoom
    void tentacle_new_room(ctx& x, tentacle_work& tw);
    // DaddyLongLegs.PlaceInRoom's part: lays the tentacle out from the body
    void tentacle_place(ctx& x, tentacle_work& tw, rw::vec2 middle);
    // DaddyTentacle.Update
    void tentacle_update(ctx& x, tentacle_work& tw);
    void switch_task(ctx& x, tentacle_work& tw, task t);
    void update_climb_grab_pos(ctx& x, tentacle_work& tw);
    float release_score(tentacle_work const& tw);
    // Tentacle.Reset
    void tentacle_reset(ctx& x, tentacle_work& tw, rw::vec2 at);
}
