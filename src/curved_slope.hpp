#pragma once

// The smooth geometry placed over a room's tiles: TerrainManager's terrain
// objects other than the room's own sand (terrain.hpp).
//
//  - CurvedSlope (Watcher): a band `thickness` thick under a Bezier spline,
//    sampled every 5 px (CurvedSlope.collisionPoints). Its art is in the
//    level image; the object itself only collides.
//  - SuperSlope: a straight band from pos to pos + handle, `bottom` thick.
//  - LocalTerrain: a TerrainCurve of its own (a sand bank under a spline,
//    down to pos.y - bottom). Burrowers may go through it. (Vanilla draws it
//    with TerrainCurve's sprites; not drawn here.)
//
// They're loaded once with the room (dcon terrain_slope objects, located in
// their room by slope_location) and never change: every query here only
// reads them, so any room's update may call these in parallel.
//
// The body chunk side (BodyChunk.CheckVerticalCollision's TerrainManager
// part, terrainCurveNormal) is split into small functions physics.cpp calls
// at the matching points; see each one.

#include <cstdint>
#include <optional>
#include <string_view>

#include "gamestate.hpp"
#include "math.hpp"
#include "world.hpp"

namespace curved_slope {
    enum class kind : uint8_t { curved = 0, super = 1, local = 2 };

    // TerrainCurve.maxSlideNormalY: steeper than 45 degrees, things slide
    inline constexpr float max_slide_normal_y = 0.70710677f;
    // CurvedSlope.segmentWidth / TerrainCurve.targetSegmentWidth
    inline constexpr float segment_width = 5.f;

    // the PlacedObjects type names this handles ("CurvedSlope", "SuperSlope",
    // "LocalTerrain")
    std::optional<kind> kind_of(std::string_view type) noexcept;

    // Builds one placed object from its settings entry and puts it in `room`
    // (Room.LoadTerrainCurve): `data` is the entry's fourth "><" part (the
    // PlacedObject.Data string), `terrain_depth` the room's
    // RoomSettings.TerrainDepth (LocalTerrain's collision line depends on
    // it). Returns false if the entry can't be read.
    bool add(dcon::data_container& w, dcon::room_id room, kind k, rw::vec2 pos, std::string_view data, float terrain_depth = 0.f);

    // whether the room has any of these (room.terrain != null, apart from
    // the sand)
    bool any(sys::game_state const& state, dcon::room_id room) noexcept;

    // TerrainManager.SnapToTerrain(center, radius, out normal, burrow): the
    // smallest vertical correction any of the room's objects asks for.
    // Nothing asking: y is float's lowest (as vanilla's float.MinValue) and
    // normal zero.
    rw::vec2 snap_to_terrain(sys::game_state const& state, dcon::room_id room, rw::vec2 center, float radius, rw::vec2& normal, bool burrow = false) noexcept;
    // TerrainManager.TrySnapToTerrain: true if the snapped position is out
    // along the normal (the circle was inside an object)
    bool try_snap(sys::game_state const& state, dcon::room_id room, rw::vec2 center, float radius, rw::vec2& snap, rw::vec2& normal, bool burrow = false) noexcept;
    // TerrainManager.Contains
    bool contains(sys::game_state const& state, dcon::room_id room, rw::vec2 pos) noexcept;
    // TerrainManager.ObstructsTile
    bool obstructs_tile(sys::game_state const& state, dcon::room_id room, int32_t x, int32_t y) noexcept;

    // TerrainManager.TileAccessibility as AImapper.FindAccessibilityOfCurrentTile
    // uses it: none (the tile's own classification stands), solid (also
    // vanilla's Sand), or curved_floor (vanilla's CurvedFloor, just above
    // Floor: pathing's access::floor)
    enum class tile_override : uint8_t { none, solid, curved_floor };
    tile_override tile_access(sys::game_state const& state, dcon::room_id room, int32_t x, int32_t y, bool tile_is_solid) noexcept;
    // AImapper.FindPassagesOfCurrentTile: a tile an object obstructs is a
    // floor (floorAltitude counts from it) when the one above it isn't;
    // nullopt where no object obstructs the tile (the tile's own rule)
    std::optional<bool> floor_tile(sys::game_state const& state, dcon::room_id room, int32_t x, int32_t y) noexcept;

    // PhysicalObject.IsTileSolid's TerrainManager part, after the tile
    // checks said no: the circle at chunk_pos + (rx, ry) * 20 would be
    // pushed further than its radius
    bool is_tile_solid(sys::game_state const& state, dcon::room_id room, rw::vec2 chunk_pos, float rad, int32_t rx, int32_t ry) noexcept;

    // ---- BodyChunk ----

    // BodyChunk.Update's move: on a curve last tick (terrainCurveNormal not
    // zero) the chunk moves only |normal.y| of its x speed
    inline rw::vec2 move(rw::vec2 pos, rw::vec2 vel, rw::vec2 terrain_curve_normal) noexcept {
        if (terrain_curve_normal != rw::vec2{0.f, 0.f})
            return rw::vec2{pos.x + std::fabs(terrain_curve_normal.y) * vel.x, pos.y + vel.y};
        return pos + vel;
    }

    // The start of BodyChunk.CheckVerticalCollision: snaps `pos` (radius
    // TerrainRad) out of the objects; returns whether it did (vanilla's
    // `flag`), `normal` the surface's. The tile checks after it clear the
    // flag when they land the chunk on a floor or hit a ceiling.
    bool snap_chunk(sys::game_state const& state, dcon::room_id room, rw::vec2& pos, float terrain_rad, rw::vec2& normal, bool burrow = false) noexcept;

    // The end of CheckVerticalCollision when the flag is still set, before
    // anything else: the speed of the impact to report as
    // TerrainImpact(index, (0, -1), speed, lastContactPoint.y > -1), or 0
    // for none (vanilla reports it when above the object's impactTreshhold)
    inline float impact_speed(rw::vec2 normal, rw::vec2 vel) noexcept { return -vel.y * normal.y; }

    // ...and then the response: the chunk slides off slopes steeper than 45
    // degrees (contact.y 0) and stands on the rest (contact.y -1).
    // `gravity` is the object's (BodyChunk's owner.gravity), the others
    // the PhysicalObject's.
    void respond(rw::vec2 normal, rw::vec2& vel, rw::ivec2& contact, float bounce, float surface_friction, float gravity) noexcept;

    // SharedPhysics.VerticalCollision's version (TerrainCollisionData:
    // thrown weapons, tentacles): snap with try_snap at the start, the tile
    // checks clear the flag, then this
    void respond_shared(rw::vec2 normal, rw::vec2& vel, rw::ivec2& contact) noexcept;
}
