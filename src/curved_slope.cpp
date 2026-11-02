#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "curved_slope.hpp"

namespace curved_slope {
    using rw::vec2;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        // Mathf.FloorToInt / CeilToInt / RoundToInt (Math.Round: half to even)
        int32_t floor_to_int(float v) noexcept { return int32_t(std::floor(v)); }
        int32_t ceil_to_int(float v) noexcept { return int32_t(std::ceil(v)); }
        int32_t round_to_int(float v) noexcept { return int32_t(std::nearbyint(v)); }

        // TerrainCurve.Normalize / CurvedSlope.Normalize
        float normalize(float f, float fallback) noexcept { return std::isnan(f) || std::isinf(f) ? fallback : f; }

        float num(std::string_view sv) { return std::strtof(std::string{sv}.c_str(), nullptr); }

        std::vector<std::string_view> split(std::string_view sv, char sep) {
            std::vector<std::string_view> r;
            for (size_t p = 0;;) {
                size_t const q = sv.find(sep, p);
                r.push_back(sv.substr(p, q == std::string_view::npos ? std::string_view::npos : q - p));
                if (q == std::string_view::npos)
                    return r;
                p = q + 1;
            }
        }

        // SplineObjectData.ParseVector
        vec2 parse_vector(std::string_view s) {
            auto const p = split(s, '^');
            return vec2{num(p[0]), p.size() > 1 ? num(p[1]) : 0.f};
        }

        // RWCustom.BezierCurve
        struct bezier {
            vec2 a, ca, b, cb;
            vec2 point(float f) const noexcept { return custom::bezier(a, ca, b, cb, f); }
        };

        // Custom.BezierYatX
        float bezier_y_at_x(bezier const& bz, float x) noexcept {
            float lo = 0.f, hi = 1.f;
            for (int i = 0; i < 16; ++i) {
                float const mid = (lo + hi) / 2.f;
                if (bz.point(mid).x < x)
                    lo = mid;
                else
                    hi = mid;
            }
            return bz.point((lo + hi) / 2.f).y;
        }

        // SplineObjectData.FromString at splineIndexInSaveString 1 (both
        // LocalTerrainData kinds) and BezierSpline.GetAllBeziers, moved to
        // the object's position
        bool parse_spline(std::vector<std::string_view> const& d, vec2 pos, std::vector<bezier>& out) {
            if (d.size() < 4)
                return false;
            vec2 const pos_b = parse_vector(d[1]);
            vec2 const handle_a = parse_vector(d[2]);
            vec2 const handle_b = parse_vector(d[3]);
            struct midpoint {
                vec2 pos;
                vec2 dir1, dir2;
            };
            std::vector<midpoint> mids;
            if (d.size() > 4)
                for (auto const s : split(d[4], '|')) {
                    if (s.find_first_not_of(" \t\r\n") == std::string_view::npos)
                        continue;
                    auto const m = split(s, '^');
                    if (m.size() < 5)
                        return false;
                    // BezierSpline.Midpoint: dir1/dir2 from the perpendicular angle
                    float const ang = num(m[2]);
                    mids.push_back({vec2{num(m[0]), num(m[1])}, custom::deg_to_vec(ang + 90.f) * num(m[3]), custom::deg_to_vec(ang - 90.f) * num(m[4])});
                }
            size_t const segments = mids.size() + 1;
            out.clear();
            for (size_t s = 0; s < segments; ++s) {
                bezier bz;
                if (s == 0) {
                    bz.a = vec2{0.f, 0.f};
                    bz.ca = handle_a;
                } else {
                    bz.a = mids[s - 1].pos;
                    bz.ca = mids[s - 1].dir2 + mids[s - 1].pos;
                }
                if (s == mids.size()) {
                    bz.b = pos_b;
                    bz.cb = handle_b;
                } else {
                    bz.b = mids[s].pos;
                    bz.cb = mids[s].dir1 + mids[s].pos;
                }
                bz.a += pos;
                bz.ca += pos;
                bz.b += pos;
                bz.cb += pos;
                out.push_back(bz);
            }
            return true;
        }

        // one object as the queries read it
        struct view {
            kind k = kind::curved;
            vec2 pos{0.f, 0.f};
            vec2 pos2{0.f, 0.f};
            float thickness = 0.f; // CurvedSlope/SuperSlope thickness, LocalTerrain's bottom y
            float start_x = 0.f;
            float end_x = 0.f;
            float const* y = nullptr; // collisionPoints' y
            int32_t segments = 0;

            // collisionPoints[i] (x = Lerp(startX, endX, i / (segments - 1)))
            vec2 point(int32_t i) const noexcept {
                return vec2{mathf::lerp(start_x, end_x, float(i) / (float(segments) - 1.f)), y[i]};
            }
            // CurvedSlope's fixed 5, TerrainCurve's own segmentWidth
            float width() const noexcept { return k == kind::local ? (end_x - start_x) / float(segments - 1) : segment_width; }
        };

        view view_of(dcon::data_container const& w, dcon::terrain_slope_id id) noexcept {
            view v;
            v.k = kind(w.terrain_slope_get_kind(id));
            v.pos = w.terrain_slope_get_pos(id);
            v.pos2 = w.terrain_slope_get_pos2(id);
            v.thickness = w.terrain_slope_get_thickness(id);
            v.start_x = w.terrain_slope_get_start_x(id);
            v.end_x = w.terrain_slope_get_end_x(id);
            auto const pts = w.terrain_slope_get_points(id);
            v.y = pts.begin();
            v.segments = int32_t(pts.size());
            return v;
        }

        template<class F>
        void for_each(sys::game_state const& state, dcon::room_id room, F&& f) {
            auto const& w = state.world;
            auto const range = w.room_range_of_slope_location(room);
            for (auto it = range.first; it != range.second; ++it)
                if (f(view_of(w, w.slope_location_get_slope(*it))))
                    return;
        }

        bool burrow_allowed(view const& v) noexcept { return v.k == kind::local; }

        // CurvedSlope.SnapToTerrain
        vec2 snap_curved(view const& v, vec2 center, float radius, vec2& normal) noexcept {
            int32_t const from = floor_to_int((center.x - v.start_x - radius) / segment_width);
            int32_t const to = ceil_to_int((center.x - v.start_x + radius) / segment_width);
            normal = vec2{0.f, 0.f};
            int32_t const mid = round_to_int((center.x - v.start_x) / segment_width);
            bool const below = v.y[std::min(std::max(mid, 0), v.segments - 1)] - v.thickness / 2.f > center.y;
            bool found = false;
            float best = 0.f;
            for (int32_t i = std::max(from, 0); i <= std::min(v.segments - 2, to); ++i) {
                vec2 a = v.point(i);
                vec2 dir = rw::normalized(v.point(i + 1) - a);
                if (below) {
                    a.y -= v.thickness;
                    dir = -dir;
                }
                float const cx = mathf::clamp(center.x + radius * dir.y, a.x, a.x + segment_width);
                float const y = (cx - a.x) * dir.y / dir.x + a.y + std::sqrt(radius * radius - std::pow(center.x - cx, 2.f));
                // (a NaN candidate never wins: as vanilla's IsNaN(num) || ...)
                if (std::isnan(y))
                    continue;
                if (!found || (!below && y > best) || (below && y < best)) {
                    found = true;
                    best = y;
                    normal = custom::perpendicular_vector(dir);
                }
            }
            if (found)
                center.y = best;
            return center;
        }

        // TerrainCurve.SnapToTerrain (LocalTerrain)
        vec2 snap_local(view const& v, vec2 center, float radius, vec2& normal) noexcept {
            float const sw = v.width();
            float const bottom = v.thickness;
            normal = vec2{0.f, 0.f};
            if (center.y >= bottom + radius || v.start_x - radius >= center.x || center.x >= v.end_x + radius) {
                int32_t const from = floor_to_int((center.x - v.start_x - radius) / sw);
                int32_t const to = ceil_to_int((center.x - v.start_x + radius) / sw);
                bool found = false;
                float best = 0.f;
                for (int32_t i = std::max(from, 0); i <= std::min(v.segments - 2, to); ++i) {
                    vec2 const a = v.point(i);
                    vec2 const dir = rw::normalized(v.point(i + 1) - a);
                    float const cx = mathf::clamp(center.x + radius * dir.y, a.x, a.x + sw);
                    float const y = (cx - a.x) * dir.y / dir.x + a.y + std::sqrt(radius * radius - std::pow(center.x - cx, 2.f));
                    if (std::isnan(y))
                        continue;
                    if (!found || y > best) {
                        found = true;
                        best = y;
                        normal = custom::perpendicular_vector(dir);
                    }
                }
                if (found)
                    center.y = best;
                return center;
            }
            // under the bank's bottom
            if (center.y + radius >= bottom) {
                normal = vec2{0.f, -1.f};
                return vec2{center.x, bottom - radius};
            }
            return center;
        }

        // SuperSlope.SnapToTerrain
        vec2 snap_super(view const& v, vec2 center, float radius, vec2& normal) noexcept {
            normal = -custom::perpendicular_vector(v.pos, v.pos2);
            if (v.pos2.x < v.pos.x)
                normal = -normal;
            if (center.x - radius < mathf::min(v.pos.x, v.pos2.x) || center.x + radius > mathf::max(v.pos.x, v.pos2.x))
                return center;
            float const t = mathf::inverse_lerp(v.pos.x, v.pos2.x, center.x);
            vec2 const on = rw::lerp(v.pos, v.pos2, t);
            if (center.y < on.y - v.thickness / 2.f) {
                normal = -normal;
                return rw::lerp(v.pos - vec2{0.f, v.thickness}, v.pos2 - vec2{0.f, v.thickness}, t) - vec2{0.f, radius};
            }
            return on + vec2{0.f, radius};
        }

        vec2 snap_one(view const& v, vec2 center, float radius, vec2& normal) noexcept {
            switch (v.k) {
            case kind::curved: return snap_curved(v, center, radius, normal);
            case kind::local: return snap_local(v, center, radius, normal);
            case kind::super: return snap_super(v, center, radius, normal);
            }
            return center;
        }

        // CurvedSlope.ObstructsTile
        bool obstructs_curved(view const& v, int32_t x, int32_t y) noexcept {
            if (float(x + 2) * 20.f < v.start_x || v.end_x < float(x - 1) * 20.f)
                return false;
            int32_t const from = std::clamp(floor_to_int((float(x) * 20.f - v.start_x) / segment_width), 0, v.segments - 1);
            int32_t const to = std::clamp(ceil_to_int((float(x + 1) * 20.f - v.start_x) / segment_width), 0, v.segments - 1);
            float const yc = float(y) * 20.f + 10.f;
            for (int32_t i = from; i <= to; ++i)
                if (v.y[i] > yc && yc > v.y[i] - v.thickness)
                    return true;
            return false;
        }

        // TerrainCurve.ObstructsTile
        bool obstructs_local(view const& v, int32_t x, int32_t y) noexcept {
            if (float(x + 1) * 20.f < v.start_x || v.end_x < float(x) * 20.f)
                return false;
            float const sw = v.width();
            int32_t const from = std::clamp(floor_to_int((float(x) * 20.f - v.start_x) / sw), 0, v.segments - 1);
            int32_t const to = std::clamp(ceil_to_int((float(x + 1) * 20.f - v.start_x) / sw), 0, v.segments - 1);
            float const yc = float(y) * 20.f + 10.f;
            for (int32_t i = from; i <= to; ++i)
                if (v.y[i] > yc && yc > v.thickness)
                    return true;
            return false;
        }

        // SuperSlope.ObstructsTile
        bool obstructs_super(view const& v, int32_t x, int32_t y) noexcept {
            vec2 const p{float(x) * 20.f, float(y) * 20.f};
            if (p.x + 20.f < mathf::min(v.pos.x, v.pos2.x) || mathf::max(v.pos.x, v.pos2.x) < p.x)
                return false;
            vec2 const a = rw::lerp(v.pos, v.pos2, mathf::inverse_lerp(v.pos.x, v.pos2.x, p.x));
            vec2 const b = rw::lerp(v.pos, v.pos2, mathf::inverse_lerp(v.pos.x, v.pos2.x, p.x + 20.f));
            float const lo = mathf::min(a.y, b.y) - v.thickness + 8.f;
            float const hi = mathf::max(a.y, b.y) - 8.f;
            return (p.y + 20.f > hi && p.y < hi) || (p.y > lo && p.y + 20.f < hi) || (p.y < lo && p.y + 20.f > lo);
        }

        bool obstructs_one(view const& v, int32_t x, int32_t y) noexcept {
            switch (v.k) {
            case kind::curved: return obstructs_curved(v, x, y);
            case kind::local: return obstructs_local(v, x, y);
            case kind::super: return obstructs_super(v, x, y);
            }
            return false;
        }
    }

    std::optional<kind> kind_of(std::string_view type) noexcept {
        if (type == "CurvedSlope")
            return kind::curved;
        if (type == "SuperSlope")
            return kind::super;
        if (type == "LocalTerrain")
            return kind::local;
        return std::nullopt;
    }

    bool add(dcon::data_container& w, dcon::room_id room, kind k, vec2 pos, std::string_view data, float terrain_depth) {
        auto const d = split(data, '~');
        if (d.empty() || d[0].empty())
            return false;
        float start_x = 0.f, end_x = 0.f, thickness = 0.f;
        vec2 pos2 = pos;
        std::vector<float> ys;
        if (k == kind::super) {
            // SuperSlopeData: handlePos, then bottom (20 if absent)
            if (d.size() < 2)
                return false;
            pos2 = pos + vec2{num(d[0]), num(d[1])};
            thickness = d.size() > 2 && !d[2].empty() ? num(d[2]) : 20.f;
            start_x = mathf::min(pos.x, pos2.x);
            end_x = mathf::max(pos.x, pos2.x);
        } else {
            // LocalTerrainData: bottom, then the spline
            float const bottom = num(d[0]);
            std::vector<bezier> bz;
            if (!parse_spline(d, pos, bz) || bz.empty())
                return false;
            start_x = bz.front().a.x;
            end_x = bz.back().b.x;
            // CurvedSlope.UpdateSize / TerrainCurve.UpdateSize
            int32_t const segments = k == kind::curved ? int32_t((end_x - start_x) / segment_width) : int32_t((end_x - start_x) / segment_width) + 1;
            if (!(segments >= 2) || segments > 100000)
                return false;
            // TerrainCurve.UpdateCollision's depth between front and back
            float const depth_t = (6.f - terrain_depth) / (35.f - terrain_depth);
            ys.resize(size_t(segments));
            size_t b = 0;
            for (int32_t j = 0; j < segments; ++j) {
                float const x = mathf::lerp(start_x, end_x, float(j) / (float(segments) - 1.f));
                while (b < bz.size() - 1 && bz[b].b.x < x)
                    ++b;
                float const front = normalize(bezier_y_at_x(bz[b], x), -10000.f);
                if (k == kind::curved) {
                    ys[size_t(j)] = front;
                } else {
                    // LocalTerrainCurve.RefreshCurve: the back line has its ends 50 up
                    bezier back = bz[b];
                    back.a.y += 50.f;
                    back.b.y += 50.f;
                    float const by = normalize(bezier_y_at_x(back, x), -10000.f);
                    ys[size_t(j)] = rw::lerp(vec2{x, front}, vec2{x, by}, depth_t).y;
                }
            }
            thickness = k == kind::curved ? bottom : pos.y - bottom;
        }
        auto const id = w.create_terrain_slope();
        w.terrain_slope_set_kind(id, uint8_t(k));
        w.terrain_slope_set_pos(id, pos);
        w.terrain_slope_set_pos2(id, pos2);
        w.terrain_slope_set_thickness(id, thickness);
        w.terrain_slope_set_start_x(id, start_x);
        w.terrain_slope_set_end_x(id, end_x);
        auto pts = w.terrain_slope_get_points(id);
        for (float const y : ys)
            pts.push_back(y);
        w.try_create_slope_location(id, room);
        return true;
    }

    bool any(sys::game_state const& state, dcon::room_id room) noexcept {
        auto const range = state.world.room_range_of_slope_location(room);
        return range.first != range.second;
    }

    vec2 snap_to_terrain(sys::game_state const& state, dcon::room_id room, vec2 center, float radius, vec2& normal, bool burrow) noexcept {
        normal = vec2{0.f, 0.f};
        float y = std::numeric_limits<float>::lowest();
        float closest = std::numeric_limits<float>::max();
        for_each(state, room, [&](view const& v) {
            if (burrow && burrow_allowed(v))
                return false;
            vec2 n{0.f, 0.f};
            vec2 const s = snap_one(v, center, radius, n);
            if (std::fabs(s.y - center.y) < closest && s != center) {
                y = s.y;
                normal = n;
                closest = std::fabs(s.y - center.y);
            }
            return false;
        });
        center.y = y;
        return center;
    }

    bool try_snap(sys::game_state const& state, dcon::room_id room, vec2 center, float radius, vec2& snap, vec2& normal, bool burrow) noexcept {
        snap = snap_to_terrain(state, room, center, radius, normal, burrow);
        // Custom.RotateAroundVector(snapPos, center, VecToDeg(normal)).y > center.y
        float const ang = custom::vec_to_deg(normal) * -0.017453292f;
        vec2 const d = snap - center;
        return std::sin(ang) * d.x + std::cos(ang) * d.y + center.y > center.y;
    }

    bool contains(sys::game_state const& state, dcon::room_id room, vec2 pos) noexcept {
        vec2 s, n;
        return try_snap(state, room, pos, 0.f, s, n);
    }

    bool obstructs_tile(sys::game_state const& state, dcon::room_id room, int32_t x, int32_t y) noexcept {
        bool hit = false;
        for_each(state, room, [&](view const& v) { return hit = obstructs_one(v, x, y); });
        return hit;
    }

    tile_override tile_access(sys::game_state const& state, dcon::room_id room, int32_t x, int32_t y, bool tile_is_solid) noexcept {
        if (!any(state, room))
            return tile_override::none;
        // TerrainManager.TileAccessibility: the first object that has a say
        tile_override acc = tile_override::none;
        for_each(state, room, [&](view const& v) {
            if (obstructs_one(v, x, y))
                acc = tile_override::solid;
            else if (obstructs_one(v, x, y - 1))
                acc = tile_override::curved_floor;
            return acc != tile_override::none;
        });
        // AImapper.FindAccessibilityOfCurrentTile
        if (obstructs_tile(state, room, x, y))
            return acc;
        if (!tile_is_solid && obstructs_tile(state, room, x, y - 1))
            return acc;
        return tile_override::none;
    }

    std::optional<bool> floor_tile(sys::game_state const& state, dcon::room_id room, int32_t x, int32_t y) noexcept {
        if (!obstructs_tile(state, room, x, y))
            return std::nullopt;
        return !obstructs_tile(state, room, x, y + 1);
    }

    bool is_tile_solid(sys::game_state const& state, dcon::room_id room, vec2 chunk_pos, float rad, int32_t rx, int32_t ry) noexcept {
        if (!any(state, room))
            return false;
        vec2 const v = chunk_pos + vec2{float(rx), float(ry)} * 20.f;
        vec2 a, n;
        return try_snap(state, room, v, rad, a, n) && rw::distance(a, v) > rad;
    }

    bool snap_chunk(sys::game_state const& state, dcon::room_id room, vec2& pos, float terrain_rad, vec2& normal, bool burrow) noexcept {
        normal = vec2{0.f, 0.f};
        if (!any(state, room))
            return false;
        vec2 s;
        if (!try_snap(state, room, pos, terrain_rad, s, normal, burrow)) {
            normal = vec2{0.f, 0.f};
            return false;
        }
        pos = s;
        return true;
    }

    void respond(vec2 normal, vec2& vel, rw::ivec2& contact, float bounce, float surface_friction, float gravity) noexcept {
        if (normal.y < max_slide_normal_y) {
            // too steep to stand on: off along it
            contact.y = 0;
            vel -= normal * mathf::min(0.f, rw::dot(vel, normal) * (1.f + bounce * 0.2f));
            vec2 const along{-normal.y, normal.x};
            vel -= rw::dot(vel, along) * mathf::clamp01(1.f - surface_friction * 2.f) * along;
            return;
        }
        contact.y = -1;
        float const mag = rw::magnitude(vel);
        float const slope = vel.x * -normal.x / normal.y;
        vel.y -= slope;
        vel.y = std::fabs(vel.y) * bounce;
        if (vel.y < gravity || vel.y < 1.f + 9.f * (1.f - bounce))
            vel.y = 0.f;
        vel.y += slope;
        vel.x *= mathf::clamp(surface_friction * 2.f, 0.f, 1.f);
        vel = rw::clamp_magnitude(vel, mag);
    }

    void respond_shared(vec2 normal, vec2& vel, rw::ivec2& contact) noexcept {
        if (normal.y < max_slide_normal_y) {
            contact.y = 0;
            vel -= normal * mathf::min(0.f, rw::dot(vel, normal));
            return;
        }
        contact.y = -1;
        float const mag = rw::magnitude(vel);
        vel.y = vel.x * -normal.x / normal.y;
        vel = rw::clamp_magnitude(vel, mag);
    }
}
