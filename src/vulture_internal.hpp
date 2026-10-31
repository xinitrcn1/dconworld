#pragma once

// What vulture.cpp (Vulture, VultureAI), vulture_tentacle.cpp (Tentacle,
// VultureTentacle) and vulture_tusks.cpp (KingTusks) share: a vulture's
// update context with working copies of its tentacles' tile lists (they
// grow and shrink every tick, so they live in std::vectors while the
// vulture updates and go back into the data container after; the chunks
// are edited in place), and the SharedPhysics helpers they use.

#include <cstdint>
#include <optional>
#include <source_location>
#include <span>
#include <vector>

#include "vulture.hpp"
#include "pathing.hpp"

namespace vulture::detail {
    // Tentacle.TentacleProps (stiff and rope are off for every vulture tentacle)
    struct tentacle_props {
        float mass_deterioration_per_chunk;
        float pull_at_connection_chunk;
        float goal_attraction_speed_tip;
        float goal_attraction_speed;
        float align_to_segment_speed;
        float backtrack_speed;
        float chunk_velocity_cap;
        float tile_tentacle_update_speed;
        float tile_tentacle_snap_with_path_distance;
        int32_t segment_phase_through_terrain_frames;
        int32_t max_pull_tiles_per_tick;
        int32_t terrain_hits_before_phase;
    };
    // the neck's TentacleProps(false, false, true, 0.5f, 0f, 0.5f, 1.8f, 0.2f,
    // 1.2f, 10f, 0.25f, 3f, 15, 20, 6, 0)
    inline constexpr tentacle_props neck_props{0.5f, 0.f, 0.5f, 1.8f, 0.2f, 1.2f, 10.f, 0.25f, 3.f, 15, 6, 0};
    // VultureTentacle's TentacleProps(false, false, true, 0.5f, 0f, 0.2f,
    // 1.2f, 0.2f, 1.2f, 10f, 0.25f, 5f, 15, 60, 12, 0)
    inline constexpr tentacle_props wing_props{0.5f, 0.f, 0.2f, 1.2f, 0.2f, 1.2f, 10.f, 0.25f, 5.f, 15, 12, 0};

    // one Tentacle while its vulture updates: its scalars and chunks in
    // place, its tile lists copied out
    struct tent {
        tentacle* t = nullptr;
        std::span<tentacle_chunk> c;
        std::vector<rw::ivec2> seg;
        std::vector<rw::ivec2> gp;
        tentacle_props const* tp = &neck_props;
        tentacle_chunk& tip() { return c[c.size() - 1]; }
    };

    // one wing while its vulture updates
    struct wing_work {
        dcon::creature_part_id part;
        dcon::vulture_wing_id id;
        uint32_t number = 0;
        wing w;
        tent tn;
    };

    // CreatureTemplate numbers per variant
    struct template_params {
        float damage_resistance;
        float stun_resistance;
    };
    template_params const& params(variant v) noexcept;

    // a vulture's update context
    struct ctx {
        sys::game_state& state;
        dcon::creature_id c;
        world::room_view const& room;
        rw::rng rng;
        creature::vulture_payload& p;
        dcon::vulture_data_id d;
        body_state& b;
        ai_state& ai;
        neck_state& neck_s;
        tent neck;
        std::vector<wing_work>& wings;
        // the `ref List<IntVector2> path` scratch list
        std::vector<rw::ivec2>& scratch;
        dcon::body_chunk_id ch[chunk_count];
        variant v = variant::vulture;
        bool viewed = false;
        // the path field to the destination, once asked for
        std::optional<pathing::field_id> field;

        // (each place it's made keys its own rolls: the source location)
        ctx(sys::game_state& s, dcon::creature_id cc, world::room_view const& r, std::source_location l = std::source_location::current());
        ~ctx();
        ctx(ctx const&) = delete;
        ctx& operator=(ctx const&) = delete;

        dcon::data_container& w() { return state.world; }
        rw::vec2& pos(uint32_t i) { return state.world.body_chunk_get_pos(ch[i]); }
        rw::vec2& last_pos(uint32_t i) { return state.world.body_chunk_get_last_pos(ch[i]); }
        rw::vec2& vel(uint32_t i) { return state.world.body_chunk_get_vel(ch[i]); }
        float mass(uint32_t i) { return state.world.body_chunk_get_mass(ch[i]); }
        float rad(uint32_t i) { return state.world.body_chunk_get_rad(ch[i]); }
        rw::ivec2 contact(uint32_t i) { return state.world.body_chunk_get_contact_point(ch[i]); }
        bool king() const { return v == variant::king; }
        bool miros() const { return v == variant::miros; }
        bool dead() { return state.world.creature_get_dead(c) != 0; }
        bool awake() { return creature::consious(state, c); }
        // Vulture.MostlyConsious
        bool mostly_consious() { return state.world.creature_get_stun(c) < 40 && !dead(); }
        dcon::creature_id grasped() { return creature::grasped(state.world, c, 0); }
        rw::ivec2& destination() { return state.world.creature_get_destination(c); }
        void set_destination(rw::ivec2 t) {
            if (destination() != t) {
                destination() = t;
                field.reset();
            }
        }
        // abstractCreature.pos: the main chunk's tile
        rw::ivec2 at() { return world::room_view::get_tile_position(pos(0)); }
        uint32_t wing_count() const { return uint32_t(wings.size()); }
        // Vulture.AirBorne
        bool air_borne();
        // CheckTentacleModeOr / And
        bool any_wing(wing_mode m);
        bool all_wings(wing_mode m);
        // AImap.getTerrainProximity (-1 outside the room)
        int32_t proximity(rw::ivec2 t);
        bool accessible(rw::ivec2 t) { return room.inside(t.x, t.y) && pathing::accessible(room, t, flyer); }
        pathing::field_id path();
        // the wing `i` (by its number)
        wing_work& wing_at(uint32_t i) { return wings[i]; }
        // the vulture's other wing of the pair (VultureTentacle.OtherTentacle)
        wing_work& other_wing(wing_work const& ww);
        float wing_stun(wing_work const& ww) const { return float(state.world.creature_part_get_stun(ww.part)); }
        int32_t& wing_stun_ref(wing_work const& ww) { return state.world.creature_part_get_stun(ww.part); }
        float& wing_health(wing_work const& ww) { return state.world.creature_part_get_health(ww.part); }
    };

    // the tentacles' working copies in and out of the data container
    void load(ctx& x);
    void store(ctx& x);

    //
    // SharedPhysics / Room helpers (vulture_tentacle.cpp)
    //
    struct terrain_collision {
        rw::vec2 pos;
        rw::vec2 last_pos;
        rw::vec2 vel;
        float rad;
        rw::ivec2 contact{0, 0};
    };
    // SharedPhysics.VerticalCollision, SlopesVertically, HorizontalCollision
    void vertical_collision(world::room_view const& room, terrain_collision& cd);
    void slopes_vertically(world::room_view const& room, terrain_collision& cd);
    void horizontal_collision(world::room_view const& room, terrain_collision& cd);
    // SharedPhysics.RayTraceTilesForTerrainReturnFirstSolid
    std::optional<rw::ivec2> first_solid(world::room_view const& room, rw::vec2 a, rw::vec2 b);
    // where a ray from `from` along `dir` ends: the first solid tile's side
    // it meets (Custom.RectCollision on its TileRect), else the edge of the
    // room grown by 200 (the miros laser's, the king's aiming laser)
    rw::vec2 beam_end(world::room_view const& room, rw::vec2 from, rw::vec2 dir, bool* hit_terrain = nullptr);
    // SharedPhysics.ExactTerrainRayTrace: where the segment enters a solid
    // tile (the FloatRect's left, bottom) and the way into it through the
    // side it met (its right, top: (1, 0) through the tile's left side...)
    bool exact_terrain_ray_trace(world::room_view const& room, rw::vec2 a, rw::vec2 b, rw::vec2& at, rw::vec2& into);

    //
    // Tentacle (vulture_tentacle.cpp)
    //
    // Tentacle.Update
    void tentacle_update(ctx& x, tent& tn);
    // Tentacle.NewRoom: the segments at the main chunk's tile, the chunks reset
    void tentacle_new_room(ctx& x, tent& tn);
    // Tentacle.Reset
    void tentacle_reset(ctx& x, tent& tn, rw::vec2 at);
    // Tentacle.MoveGrabDest
    void move_grab_dest(ctx& x, tent& tn, rw::vec2 to);
    // Tentacle.FloatBase
    rw::vec2 float_base(ctx& x, tent const& tn);
    // Tentacle.BasePos
    rw::ivec2 base_pos(ctx& x, tent& tn);

    //
    // VultureTentacle (vulture_tentacle.cpp)
    //
    void wing_update(ctx& x, wing_work& ww);
    void switch_mode(ctx& x, wing_work& ww, wing_mode m);
    void release_grip(ctx& x, wing_work& ww);
    float support(ctx& x, wing_work const& ww);
    float release_score(ctx& x, wing_work const& ww);
    bool wing_space(ctx& x, wing_work& ww);
    bool has_any_grip(wing_work const& ww);
    float tentacle_dir(ctx const& x, uint32_t number);
    // VultureTentacle.TentacleContour, FeatherContour (static), FeatherWidth
    float tentacle_contour(variant v, float flying_mode, float x);
    float feather_contour(float x, float k);
    float feather_width(float x);
    // VultureTentacle.Damage
    void wing_damage(ctx& x, wing_work& ww, float damage, float stun_bonus);

    //
    // KingTusks (vulture_tusks.cpp)
    //
    void tusks_new_room(ctx& x);
    void tusks_update(ctx& x);
    // KingTusks.HitBySpear: a tusk knocked loose
    bool tusks_hit_by_spear(ctx& x, rw::vec2 momentum);
    bool tusks_ready_to_shoot(ctx& x);
    bool tusks_want_to_shoot(ctx& x, bool check_visual, bool check_min_distance);
    bool any_impaled(ctx& x);
    bool impaled(ctx& x, dcon::creature_id t);
    // a tusk lets go of `t`
    void tusks_release(sys::game_state& state, dcon::creature_id c, dcon::creature_id t);
    inline constexpr float tusk_length = 30.f;
    inline constexpr float max_wire_length = 500.f;
    inline constexpr float shoot_range = 550.f;
    inline constexpr float min_shoot_range = 250.f;

    //
    // vulture.cpp: what the tentacles and tusks call on their vulture
    //
    // Vulture.AirBrake
    void air_brake(ctx& x, int32_t frames);
    // VultureAI.behavior
    behavior current(ctx& x);
    // the tracked creature's representation (focus, prey), if it still tracks it
    creature::tracked const* focus(ctx& x);
    creature::tracked const* most_attractive_prey(ctx& x);
    // VultureAI.UpdateDynamicRelationship (through the tracker)
    creature::relationship dynamic_relationship(ctx& x, dcon::creature_id t);
    // Vulture.ChargingSnap
    bool charging_snap(ctx& x);
    // the vulture grabs `t`'s chunk `k` in its beak
    void grab_in_beak(ctx& x, dcon::creature_id t, uint32_t k);
}
