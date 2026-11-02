#include "room_decor.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>

#include <glm/common.hpp>

#include "curved_slope.hpp"
#include "layered_fs.hpp"
#include "particles.hpp"
#include "world.hpp"

namespace room_decor {
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        // Time.fixedDeltaTime * 2: what Floater.Update adds to `time` each
        // tick the room is viewed (Unity's default fixed step, 0.02)
        constexpr float floater_time_step = 0.04f;
        // MaskSource.magicScale: a DynamicLevelElement's mesh units in pixels
        constexpr float magic_scale = 16.f;

        // ---- FloatingDebris "Rot Islands" (FloatingDebris.RotFloater)

        struct rot_floater {
            // Floater
            vec2 orig_pos{0.f, 0.f};
            vec2 offset{0.f, 0.f};
            float offset_amount = 0.f;
            float depth_raw = 0.f; // _depth
            float depth_offset = 0.f;
            float scale = 0.f;
            float scale_offset = 0.f;
            float movement = 0.f;
            int32_t index = 0; // posInArray
            // the owner FloatingDebris'
            int32_t depth_near = 0, depth_far = 30;
            // RotFloater.GenerateRandomness
            std::vector<vec2> blob_offsets;
            std::vector<float> blob_random_depth;
            std::vector<int32_t> wire_connections; // smallWiresConnections
            // Blob.GenerateRandomness
            std::vector<vec2> blob_random;
            std::vector<float> blob_scale_variation;
            // Wire.GenerateRandomness
            std::vector<float> wire_lengths, wire_widths, wire_random;

            // Floater.depth
            float depth() const noexcept { return mathf::clamp01(depth_raw + depth_offset); }
            // Floater.getPos (what Floater.Update sets pos to; RotFloater's
            // own getPos hides it but isn't used)
            vec2 pos_at(float time) const noexcept {
                vec2 const pre = orig_pos + offset * offset_amount;
                float num = std::sin(time + offset.x * 3.1415927f * 2.f) * offset.y * 10.f;
                num += std::sin(time + orig_pos.x / 20.f) * 10.f;
                num *= 1.f - depth();
                return pre + vec2{0.f, 1.f} * num * movement;
            }
            // Floater.finalScale (defaultScale 40)
            float final_scale() const noexcept { return mathf::clamp(40.f * (scale - scale_offset), 0.f, 99999.f); }
        };

        // ---- KarmaFlowerPatch

        struct flower {
            vec2 ideal_pos{0.f, 0.f};
            vec2 root{0.f, 0.f};
            vec2 direction{0.f, 0.f};
            float radius = 4.f;
            float depth = 0.f;
            vec2 pos{0.f, 0.f}, last_pos{0.f, 0.f}, vel{0.f, 0.f};
        };
        struct flower_patch {
            vec2 pos{0.f, 0.f};
            vec2 handle{0.f, 0.f};
            float tilt = 0.5f;
        };

        struct room_state {
            bool loaded = false;
            std::vector<rot_floater> rot;
            std::vector<flower_patch> patches;
            std::vector<flower> flowers;
            bool flowers_placed = false;
            // the FlowerWind effect's three amounts
            bool has_wind = false;
            float wind[3] = {0.f, 0.f, 0.f};
            float wind_scroll = 0.f;
            // Floater.time, in ticks viewed
            uint32_t viewed_ticks = 0;
            uint32_t last_tick = 0;
            bool started = false;
        };
        // (the renderer's thread only)
        std::unordered_map<uint32_t, room_state> rooms;

        float num(std::string_view sv) { return std::strtof(std::string{sv}.c_str(), nullptr); }
        int32_t inum(std::string_view sv) { return int32_t(std::strtoll(std::string{sv}.c_str(), nullptr, 10)); }
        std::vector<std::string_view> split(std::string_view sv, std::string_view sep) {
            std::vector<std::string_view> r;
            for (size_t p = 0;;) {
                size_t const q = sv.find(sep, p);
                r.push_back(sv.substr(p, q == std::string_view::npos ? std::string_view::npos : q - p));
                if (q == std::string_view::npos)
                    return r;
                p = q + sep.size();
            }
        }
        std::vector<float> num_list(std::string_view sv) {
            std::vector<float> r;
            for (auto e : split(sv, "|"))
                r.push_back(num(e));
            return r;
        }
        // the line after `key` at the start of a line
        std::string_view settings_line(std::string_view text, std::string_view key) {
            size_t at = text.find(key);
            while (at != std::string_view::npos && at != 0 && text[at - 1] != '\n')
                at = text.find(key, at + 1);
            if (at == std::string_view::npos)
                return {};
            std::string_view line = text.substr(at + key.size());
            return line.substr(0, line.find('\n'));
        }

        // Custom.RotateAroundOrigo
        vec2 rotate_around_origo(vec2 v, float deg) {
            deg *= -0.017453292f;
            float const c = std::cos(deg), s = std::sin(deg);
            return vec2{c * v.x - s * v.y, s * v.x + c * v.y};
        }

        float safe(std::vector<float> const& v, size_t i, float fallback = 0.f) {
            return v.empty() ? fallback : v[std::min(i, v.size() - 1)];
        }

        // FloatingDebrisData.FromString, then the FloatingDebris constructor
        // and its RotFloaters (only the "Rot Islands" type is drawn here)
        void add_floating_debris(room_state& rs, vec2 pos, std::string_view data) {
            auto const d = split(data, "~");
            if (d.size() < 14 || d[13] != "Rot Islands")
                return;
            int32_t const seed = inum(d[2]);
            int32_t depth_near = inum(d[3]);
            int32_t depth_far = inum(d[4]);
            float scale_min = num(d[5]);
            float scale_max = num(d[6]);
            // (Remix's cfgReduceFloatingDebrisDensity at its default 0)
            int32_t const n = std::max(inum(d[7]), 0);
            std::vector<float> const cpx = num_list(d[8]);
            std::vector<float> const cpy = d.size() > 9 ? num_list(d[9]) : std::vector<float>{50.f};
            std::vector<float> const cp_offset = d.size() > 10 ? num_list(d[10]) : std::vector<float>{0.5f};
            std::vector<float> const cp_depth = d.size() > 11 ? num_list(d[11]) : std::vector<float>{0.f};
            std::vector<float> const cp_scale = d.size() > 12 ? num_list(d[12]) : std::vector<float>{0.f};
            float const movement = d.size() > 14 ? num(d[14]) : 0.5f;
            if (cpx.empty() || cpy.empty())
                return;
            size_t const cps = std::min(cpx.size(), cpy.size());
            auto const cp = [&](size_t i) { i = std::min(i, cps - 1); return vec2{cpx[i], cpy[i]}; };

            // UpdateSeededRandomValues
            std::vector<vec2> offsets;
            std::vector<float> depths, scales;
            {
                rw::unity_random r{seed};
                for (int32_t i = 0; i < n; ++i) {
                    float const ox = r.value();
                    float const oy = r.value();
                    offsets.push_back(vec2{ox, oy} * 2.f - vec2{1.f, 1.f});
                    depths.push_back(mathf::lerp(float(depth_near), float(depth_far), r.value()) / 30.f);
                    scales.push_back(mathf::lerp(scale_min, scale_max, r.value()));
                }
            }
            // UpdateControlPointInfluence
            std::vector<std::pair<int32_t, float>> influence;
            if (cps < 2) {
                influence.assign(size_t(n), {0, 0.f});
            } else {
                float total = 0.f;
                for (size_t j = 0; j + 1 < cps; ++j)
                    total += rw::distance(cp(j), cp(j + 1));
                float const step = total / mathf::max(float(n - 1), 0.0001f) - 0.001f;
                float done = 0.f;
                size_t seg = 0;
                float seg_len = rw::distance(cp(0), cp(1));
                for (int32_t k = 0; k < n; ++k) {
                    float t = float(k) * step;
                    if (t > seg_len + done && seg + 2 < cps) {
                        --k;
                        ++seg;
                        done += seg_len;
                        seg_len = rw::distance(cp(seg), cp(seg + 1));
                    } else {
                        t -= done;
                        t = seg_len > 0.f ? t / seg_len : 0.f;
                        influence.push_back({int32_t(seg), t});
                    }
                }
            }
            for (int32_t i = 0; i < n; ++i) {
                auto const [seg, t] = influence[size_t(i)];
                rot_floater f;
                if (cps <= 1) {
                    f.orig_pos = pos + cp(0);
                    f.depth_offset = safe(cp_depth, 0);
                    f.scale_offset = safe(cp_scale, 0);
                    f.offset_amount = safe(cp_offset, 0) * 150.f;
                } else {
                    size_t const a = size_t(seg), b = size_t(seg) + 1;
                    f.orig_pos = pos + rw::lerp(cp(a), cp(b), t);
                    f.depth_offset = mathf::lerp(safe(cp_depth, a), safe(cp_depth, b), t);
                    f.scale_offset = mathf::lerp(safe(cp_scale, a), safe(cp_scale, b), t);
                    // FloatingDebris.radius 150
                    f.offset_amount = mathf::lerp(safe(cp_offset, a), safe(cp_offset, b), t) * 150.f;
                }
                f.offset = offsets[size_t(i)];
                f.depth_raw = depths[size_t(i)];
                f.scale = scales[size_t(i)];
                f.movement = movement;
                f.index = i;
                f.depth_near = depth_near;
                f.depth_far = depth_far;
                // RotFloater.GenerateRandomness (maxBlobs 10, maxSmalWires 20)
                int32_t const key = int32_t(f.offset.x * 1000.f);
                {
                    rw::unity_random r{key};
                    int32_t const blobs = int32_t((0.5f + r.value() * 0.5f) * 10.f);
                    for (int32_t b = 0; b < blobs; ++b) {
                        float const bx = r.value();
                        float const by = r.value();
                        f.blob_offsets.push_back(vec2{bx, by} * 2.f - vec2{1.f, 1.f});
                        f.blob_random_depth.push_back(r.value());
                    }
                    int32_t const wires = int32_t((0.5f + r.value() * 0.5f) * 20.f);
                    int32_t last = 0;
                    for (int32_t j = 0; j < wires; ++j) {
                        // (the loop keeps the previous pick until a new one comes up)
                        int32_t pick = last;
                        int32_t guard = 0;
                        while (pick == last && blobs > 2 && guard++ < 1000)
                            pick = int32_t(r.value() * float(blobs - 1));
                        f.wire_connections.push_back(last);
                        last = pick;
                    }
                }
                {
                    // Blob.GenerateRandomness
                    rw::unity_random r{key};
                    for (size_t b = 0; b < f.blob_offsets.size(); ++b) {
                        float const rx = r.value();
                        float const ry = r.value();
                        f.blob_random.push_back(vec2{rx, ry});
                        f.blob_scale_variation.push_back(0.75f + r.value() * 0.25f);
                    }
                }
                {
                    // Wire.GenerateRandomness
                    rw::unity_random r{key};
                    size_t const wires = f.wire_connections.empty() ? 0 : f.wire_connections.size() - 1;
                    for (size_t w = 0; w < wires; ++w) {
                        f.wire_lengths.push_back(1.f + r.value() * 1.f);
                        f.wire_widths.push_back(r.value());
                        f.wire_random.push_back(r.value());
                    }
                }
                rs.rot.push_back(std::move(f));
            }
        }

        void load(sys::game_state const& s, dcon::room_id room, room_state& rs) {
            rs.loaded = true;
            auto const& w = s.world;
            std::string const name{s.to_string_view(w.room_get_name(room))};
            auto const region = w.room_get_region_from_region_membership(room);
            std::string const region_name{s.to_string_view(w.region_get_name(region))};
            auto const world_dir = s.fs.get_root().get_single_subdir("world");
            auto const dir = w.region_get_is_pseudo(region) ? world_dir.get_single_subdir(region_name) : world_dir.get_single_subdir(region_name + "-rooms");
            auto const f = dir.get_single_file(name + "_settings.txt");
            auto const of = f.open_file(s.fs);
            if (!of)
                return;
            std::string_view const text = of->to_string_view();
            // RoomSettings.LoadPlacedObjects (filters aren't applied)
            std::string_view const line = settings_line(text, "PlacedObjects: ");
            for (auto entry : split(line, ", ")) {
                while (!entry.empty() && (entry.front() == ' ' || entry.front() == '\r'))
                    entry.remove_prefix(1);
                while (!entry.empty() && (entry.back() == ' ' || entry.back() == '\r' || entry.back() == ','))
                    entry.remove_suffix(1);
                auto const parts = split(entry, "><");
                if (parts.size() < 4)
                    continue;
                vec2 const pos{num(parts[1]), num(parts[2])};
                if (parts[0] == "FloatingDebris") {
                    add_floating_debris(rs, pos, parts[3]);
                } else if (parts[0] == "KarmaFlowerPatch") {
                    // KarmaFlowerPatchData.FromString
                    auto const d = split(parts[3], "~");
                    if (d.size() < 3)
                        continue;
                    rs.patches.push_back(flower_patch{pos, vec2{num(d[0]), num(d[1])}, num(d[2])});
                }
            }
            // the FlowerWind effect: "FlowerWind-<a0>,<a1>,<a2>-<panel>"
            std::string_view const effects = settings_line(text, "Effects: ");
            for (auto e : split(effects, ", ")) {
                size_t const dash = e.find('-');
                if (dash == std::string_view::npos || e.substr(0, dash) != "FlowerWind")
                    continue;
                std::string_view amounts = e.substr(dash + 1);
                amounts = amounts.substr(0, amounts.find('-'));
                auto const a = split(amounts, ",");
                rs.has_wind = true;
                for (size_t k = 0; k < 3 && k < a.size(); ++k)
                    rs.wind[k] = num(a[k]);
            }
        }

        // KarmaFlowerPatch.CanSpawnFlowers / HasAnySolid / GetFloorPos
        bool has_any_solid(sys::game_state const& s, world::room_view const& v, int32_t x, int32_t y) {
            return v.get_tile(x, y).solid() || curved_slope::obstructs_tile(s, v.id, x, y);
        }
        bool can_spawn_flowers(sys::game_state const& s, world::room_view const& v, int32_t x, int32_t y) {
            if (v.get_tile(x, y).t == world::terrain::slope) {
                auto const a = v.identify_slope(x, y);
                return a == world::slope_direction::up_left || a == world::slope_direction::up_right;
            }
            return !has_any_solid(s, v, x, y) && has_any_solid(s, v, x, y - 1);
        }
        vec2 floor_pos(world::room_view const& v, int32_t x, int32_t y, float t) {
            t *= 20.f;
            vec2 const a{float(x) * 20.f, float(y) * 20.f};
            if (v.get_tile(x, y).t != world::terrain::slope)
                return a + vec2{t, 0.f};
            if (v.identify_slope(x, y) == world::slope_direction::up_left)
                return a + vec2{t, t};
            return a + vec2{t, 1.f - t};
        }

        // KarmaFlowerPatch.PlaceFlowers (no LightSources)
        void place_flowers(sys::game_state const& s, dcon::room_id room, room_state& rs) {
            rs.flowers_placed = true;
            if (rs.patches.empty())
                return;
            auto const v = world::get_room_view(s, room);
            if (v.tiles == nullptr)
                return;
            int32_t left = std::numeric_limits<int32_t>::max(), bottom = std::numeric_limits<int32_t>::max();
            int32_t right = std::numeric_limits<int32_t>::min(), top = std::numeric_limits<int32_t>::min();
            uint32_t seed = 0;
            for (auto const& p : rs.patches) {
                float const rad = rw::magnitude(p.handle);
                left = std::min(left, int32_t(std::floor((p.pos.x - rad) / 20.f)));
                bottom = std::min(bottom, int32_t(std::floor((p.pos.y - rad) / 20.f)));
                right = std::max(right, int32_t(std::floor((p.pos.x + rad) / 20.f)));
                top = std::max(top, int32_t(std::floor((p.pos.y + rad) / 20.f)));
                // (int arithmetic, wrapping)
                seed ^= uint32_t(int32_t(p.pos.x)) + uint32_t(v.width * 20) * uint32_t(int32_t(p.pos.y));
            }
            rw::unity_random r{int32_t(seed)};
            for (int32_t j = bottom; j <= top; ++j) {
                for (int32_t k = left; k <= right; ++k) {
                    if (!can_spawn_flowers(s, v, k, j))
                        continue;
                    vec2 const fp = floor_pos(v, k, j, 0.5f);
                    float num3 = 0.f, num4 = 0.f;
                    vec2 dir{0.f, 0.f};
                    for (auto const& p : rs.patches) {
                        float const rad = rw::magnitude(p.handle);
                        float const num7 = mathf::inverse_lerp(rad, 0.f, rw::distance(fp, p.pos));
                        num3 += num7 * (1.f - num3);
                        num4 += num7;
                        // KarmaFlowerPatchData.Direction
                        dir += num7 * (rw::normalized(p.handle) * p.tilt);
                    }
                    if (num4 > 0.f)
                        dir /= num4;
                    for (int l = 0; l < 3; ++l) {
                        if (r.value() < num3) {
                            float const p2 = std::pow(r.value(), 2.f);
                            float const num8 = mathf::lerp(num3, p2, r.value());
                            vec2 const fp2 = floor_pos(v, k, j, r.value());
                            float const variation = custom::clamped_random_variation(0.75f, 0.25f, 2.f, r);
                            float const tilt = r.range(-10.f, 10.f);
                            vec2 const pos = fp2 + rotate_around_origo(vec2{0.f, 30.f * num8 * variation}, tilt);
                            float const turn = r.range(-25.f, 25.f);
                            flower f;
                            f.ideal_pos = pos;
                            f.root = fp2;
                            f.direction = rotate_around_origo(dir, turn);
                            f.radius = mathf::lerp(4.f, 8.f, num8);
                            f.depth = r.value();
                            f.pos = f.last_pos = pos;
                            rs.flowers.push_back(f);
                        }
                    }
                }
            }
            std::stable_sort(rs.flowers.begin(), rs.flowers.end(), [](flower const& a, flower const& b) { return a.depth < b.depth; });
        }

        // Mathf.PerlinNoise: a 2D gradient noise in [0, 1] (hashed gradients,
        // not Unity's permutation table)
        float perlin(float x, float y) {
            auto const hash = [](int32_t ix, int32_t iy) {
                uint32_t h = uint32_t(ix) * 0x8da6b343u ^ uint32_t(iy) * 0xd8163841u;
                h ^= h >> 15;
                h *= 0x2c1b3c6du;
                h ^= h >> 12;
                return h;
            };
            auto const grad = [&](int32_t ix, int32_t iy, float dx, float dy) {
                switch (hash(ix, iy) & 7u) {
                case 0: return dx + dy;
                case 1: return -dx + dy;
                case 2: return dx - dy;
                case 3: return -dx - dy;
                case 4: return dx;
                case 5: return -dx;
                case 6: return dy;
                default: return -dy;
                }
            };
            auto const fade = [](float t) { return t * t * t * (t * (t * 6.f - 15.f) + 10.f); };
            float const fx = std::floor(x), fy = std::floor(y);
            int32_t const ix = int32_t(fx), iy = int32_t(fy);
            float const dx = x - fx, dy = y - fy;
            float const u = fade(dx), w = fade(dy);
            float const a = grad(ix, iy, dx, dy) + (grad(ix + 1, iy, dx - 1.f, dy) - grad(ix, iy, dx, dy)) * u;
            float const b = grad(ix, iy + 1, dx, dy - 1.f) + (grad(ix + 1, iy + 1, dx - 1.f, dy - 1.f) - grad(ix, iy + 1, dx, dy - 1.f)) * u;
            return mathf::clamp01((a + (b - a) * w) * 0.5f + 0.5f);
        }

        // KarmaFlowerPatch.Update (no pushes from the creatures' body chunks)
        void update_flowers(room_state& rs) {
            float n0 = 0.f, n1 = 0.f, n2 = 0.f;
            if (rs.has_wind) {
                n0 = (rs.wind[0] * 2.f - 1.f) * 2.f;
                n1 = (rs.wind[1] * 2.f - 1.f) * 2.f;
                n2 = rs.wind[2] * 5.f * (n0 + n1 > 0.f ? -1.f : 1.f);
            }
            rs.wind_scroll += n2 / 40.f;
            for (auto& f : rs.flowers) {
                f.last_pos = f.pos;
                f.pos += f.vel;
                f.vel *= 0.8f;
                f.vel.x += mathf::lerp(n0, n1, perlin(f.root.x / 100.f + rs.wind_scroll, rs.wind_scroll / 10.f));
                f.vel += (f.ideal_pos - f.pos) * 0.1f;
                float const d = rw::distance(f.pos, f.root);
                float const ideal = rw::distance(f.ideal_pos, f.root);
                if (d > ideal) {
                    vec2 const b = (d - ideal) * rw::normalized(f.root - f.pos);
                    f.pos += b;
                    f.vel += b;
                }
            }
        }

        room_state* find(dcon::room_id room) {
            auto const it = rooms.find(uint32_t(room.index()));
            return it == rooms.end() ? nullptr : &it->second;
        }

        // Color -> Color32 (Unity rounds)
        uint32_t color32(float r, float g, float b, float a) {
            auto const c = [](float v) { return uint32_t(std::lround(mathf::clamp01(v) * 255.f)); };
            return c(r) | (c(g) << 8) | (c(b) << 16) | (c(a) << 24);
        }

        // a wire's cubic (FloatingDebris.RotFloater.Wire.Spline)
        struct cubic {
            vec2 a, b, c, d;
            vec2 at(float t) const { return a + b * t + c * (t * t) + d * (t * t * t); }
        };
        cubic make_cubic(vec2 a, vec2 b, vec2 c, vec2 d) {
            return cubic{a, 3.f * (b - a), 3.f * (c - 2.f * b + a), d - 3.f * c + 3.f * b - a};
        }

        // RotFloater's Blob and Wire meshes at `time`, in room pixels: wires
        // (uv.y shifted up by 2, dynamic_level.frag tells them apart) then the
        // blobs from the farthest
        void floater_mesh(rot_floater const& f, float time, std::vector<gpu::vertex>& out) {
            size_t const blobs = f.blob_offsets.size();
            if (blobs == 0)
                return;
            vec2 const origin = f.pos_at(time);
            float const fs = f.final_scale();
            // Blob.UpdateBlobPositions
            std::vector<float> depth(blobs);
            std::vector<vec2> at(blobs);
            for (size_t i = 0; i < blobs; ++i) {
                float const n = (std::sin(time + f.blob_random[i].x * 11.f) * 0.5f + 0.5f) * 30.f * (f.blob_random[i].y * 0.5f + 0.5f) * f.movement;
                depth[i] = mathf::clamp01(1.f - mathf::lerp(float(f.depth_near), float(f.depth_far), f.blob_random_depth[i]) / 30.f + f.depth_offset);
                at[i] = f.blob_offsets[i] * (n + fs * (0.75f + depth[i] * 0.25f)) / 16.f;
            }
            auto const place = [&](vec2 local) { return origin + local * magic_scale; };

            // Wire.UpdateMesh (wireSegments 13, wireWidth 1, wireLength 15, wireC 0.333)
            constexpr int segs = 13;
            constexpr float wire_length = 15.f;
            constexpr float wire_c = 0.333f;
            size_t const wires = f.wire_lengths.size();
            vec2 pts[segs * 2];
            uint32_t cols[segs];
            for (size_t i = 0; i < wires && i + 1 < f.wire_connections.size(); ++i) {
                size_t const a = size_t(std::clamp(f.wire_connections[i], 0, int32_t(blobs) - 1));
                size_t const b = size_t(std::clamp(f.wire_connections[i + 1], 0, int32_t(blobs) - 1));
                float const da = depth[a], db = depth[b];
                vec2 const va = at[a], vb = at[b];
                vec2 const v3 = vb - va;
                float const dir = mathf::sign((f.blob_offsets[b] - f.blob_offsets[a]).x);
                float const g = (dir > 0.f ? da - db : db - da) * 0.5f + 0.5f;
                float const len = f.wire_lengths[i];
                float const sag = mathf::max(0.001f, wire_length * len * ((da + db) * 0.125f + 0.75f) - rw::magnitude(v3));
                float const dd = 1.f - mathf::max(v3.x, 0.f) / wire_length * len;
                vec2 const v4 = vec2{1.f, 1.f} - vec2{std::fabs(mathf::min(v3.y, 0.f)), mathf::max(v3.y, 0.f)} / wire_length * len * 0.75f * dd;
                vec2 const cb = va + (vb - va) * wire_c - vec2{sag * 0.1f * dir, sag * 0.4f} * v4.y;
                vec2 const cc = vb + (va - vb) * wire_c - vec2{sag * -0.1f * dir, sag * 0.4f} * v4.x;
                cubic const sp = make_cubic(va, cb, cc, vb);
                for (int j = 0; j < segs; ++j) {
                    float t = float(j) / float(segs - 1);
                    if (dir > 0.f)
                        t = 1.f - t;
                    cols[j] = color32(mathf::lerp(da, db, t) - 0.08f, g, f.wire_widths[i], f.wire_random[i]);
                    pts[j * 2] = sp.at(t);
                }
                for (int k = 0; k < segs; ++k) {
                    int const prev = std::max(k, 1) * 2 - 2;
                    int const next = std::min(k, segs - 2) * 2 + 2;
                    float const t = float(k) / float(segs - 1);
                    vec2 side = rw::normalized(pts[prev] - pts[next]);
                    side = vec2{-side.y, side.x};
                    side = side * 1.f * (1.f + f.wire_widths[i] * 0.3f) * (0.75f + mathf::lerp(da, db, t) * 0.25f);
                    pts[k * 2 + 1] = pts[k * 2] - side;
                }
                auto const vert = [&](int idx) {
                    int const k = idx / 2;
                    vec2 const p = place(pts[idx]);
                    // (uv: along, across; v shifted up by 2: a wire)
                    return gpu::vertex{p.x, p.y, cols[k], float(k) / float(segs - 1), float(idx % 2) + 2.f};
                };
                for (int j = 0; j + 1 < segs; ++j) {
                    int const n5 = j * 2;
                    out.push_back(vert(n5));
                    out.push_back(vert(n5 + 2));
                    out.push_back(vert(n5 + 1));
                    out.push_back(vert(n5 + 2));
                    out.push_back(vert(n5 + 3));
                    out.push_back(vert(n5 + 1));
                }
            }

            // Blob.UpdateMesh (blobScale 3), the farthest first (vanilla's
            // depth test; there is no depth buffer here)
            std::vector<size_t> order(blobs);
            for (size_t i = 0; i < blobs; ++i)
                order[i] = i;
            std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) { return depth[x] < depth[y]; });
            for (size_t i : order) {
                float const d = 3.f * f.blob_scale_variation[i] * (0.5f + depth[i] * 0.5f);
                vec2 const v0 = at[i] - vec2{d, d};
                vec2 const v3 = at[i] + vec2{d, d};
                vec2 const v1 = v0 + vec2{0.f, d * 2.f};
                vec2 const v2 = v0 + vec2{d * 2.f, 0.f};
                uint32_t const col = color32(depth[i], f.blob_random[i].x, f.blob_random[i].y, 0.f);
                auto const vert = [&](vec2 local, float u, float v) {
                    vec2 const p = place(local);
                    return gpu::vertex{p.x, p.y, col, u, v};
                };
                gpu::vertex const q0 = vert(v0, 0.f, 0.f), q1 = vert(v1, 0.f, 1.f), q2 = vert(v2, 1.f, 0.f), q3 = vert(v3, 1.f, 1.f);
                out.insert(out.end(), {q0, q2, q1, q2, q3, q1});
            }
        }

        // decal.frag's uniform layout plus the effect colours
        struct dynamic_level_uniforms {
            glm::mat4 proj;
            glm::vec2 screen;
            glm::vec2 view_pos;
            glm::vec2 cam_origin;
            glm::vec2 light_dir;
            glm::ivec2 effect_colors;
            int32_t index;
            int32_t camera_index;
            float zoom;
            float night_time;
            float rain;
            float light;
            float clouds_speed;
            int32_t later_count;
            float pad0, pad1;
            glm::vec4 later[16];
        };
        static_assert(offsetof(dynamic_level_uniforms, effect_colors) == 96 && offsetof(dynamic_level_uniforms, later_count) == 132
            && offsetof(dynamic_level_uniforms, later) == 144 && sizeof(dynamic_level_uniforms) == 400);
    }

    void step(sys::game_state const& s, dcon::room_id room) {
        if (!room)
            return;
        auto& rs = rooms[uint32_t(room.index())];
        if (!rs.loaded)
            load(s, room, rs);
        if (!rs.flowers_placed)
            place_flowers(s, room, rs);
        if (!rs.started) {
            rs.started = true;
            rs.last_tick = s.tick;
            return;
        }
        // the ticks since the last frame (a room seen again after a while
        // goes on where it was, like vanilla's while it wasn't viewed)
        uint32_t ticks = s.tick - rs.last_tick;
        rs.last_tick = s.tick;
        if (ticks > 10)
            ticks = 1;
        rs.viewed_ticks += ticks;
        for (uint32_t i = 0; i < ticks; ++i)
            update_flowers(rs);
    }

    void draw_rot_islands(gpu::context& g, sys::game_state const& s, dcon::room_id room, float alpha, level_params const& f) {
        room_state const* rs = find(room);
        if (rs == nullptr || rs->rot.empty() || f.cams == nullptr || f.cams->empty() || f.cameras == 0)
            return;
        float const time = (float(rs->viewed_ticks) + alpha) * floater_time_step;
        // the floaters from the farthest
        std::vector<std::pair<float, size_t>> order;
        for (size_t i = 0; i < rs->rot.size(); ++i)
            order.push_back({rs->rot[i].depth(), i});
        std::stable_sort(order.begin(), order.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
        static std::vector<gpu::vertex> verts;
        verts.clear();
        for (auto const& o : order)
            floater_mesh(rs->rot[o.second], time, verts);
        if (verts.empty())
            return;
        vec2 lo{verts[0].x, verts[0].y}, hi = lo;
        for (auto const& v : verts) {
            lo = glm::min(lo, vec2{v.x, v.y});
            hi = glm::max(hi, vec2{v.x, v.y});
        }
        gpu::upload_vertices(g, verts.data(), verts.size());
        dynamic_level_uniforms u{};
        u.proj = f.proj;
        u.screen = f.screen;
        u.view_pos = f.view_pos;
        u.light_dir = f.light_dir;
        u.effect_colors = f.effect_colors;
        u.index = f.palette;
        u.zoom = f.zoom;
        u.night_time = f.night_time;
        u.rain = f.rain;
        u.light = f.light;
        u.clouds_speed = 1.f;
        auto const& cams = *f.cams;
        for (size_t c = 0; c < cams.size(); ++c) {
            vec2 const o = cams[c];
            if (hi.x < o.x || lo.x > o.x + 1400.f || hi.y < o.y || lo.y > o.y + 800.f)
                continue;
            u.camera_index = int32_t(c);
            u.cam_origin = o;
            // the camera images drawn after this one (the level shows theirs)
            int32_t n = 0;
            for (size_t k = c + 1; k < cams.size() && n < 32; ++k, ++n) {
                glm::vec4& v = u.later[n / 2];
                if (n % 2 == 0) {
                    v.x = cams[k].x;
                    v.y = cams[k].y;
                } else {
                    v.z = cams[k].x;
                    v.w = cams[k].y;
                }
            }
            u.later_count = n;
            gpu::draw(g, gpu::pipeline::dynamic_level, &u, sizeof(u), {f.cameras, f.palettes, f.effects}, 0, verts.size());
        }
    }

    void flower_cmds(sys::game_state const& s, dcon::room_id room, float alpha, vec2 depth_point, vec3 black, vec3 fog, vec2 ring_size,
        std::vector<sprites::draw_cmd>& out, std::vector<sprites::strip_vertex>& strip) {
        room_state const* rs = find(room);
        if (rs == nullptr || rs->flowers.empty())
            return;
        // KarmaFlowerPatch.ApplyPalette
        vec3 const stalk = black + (fog - black) * 0.3f;
        vec3 const petal{0.529f, 0.365f, 0.184f}; // RainWorld.GoldRGB
        // RoomCamera.ApplyDepth
        auto const apply_depth = [&](vec2 v, float d) {
            d *= -0.025f;
            v -= depth_point;
            d = (10.f - d) * 0.1f;
            return v / d + depth_point;
        };
        for (size_t i = 0; i < rs->flowers.size(); ++i) {
            flower const& fl = rs->flowers[i];
            // (shrinkFactor 0)
            float const r3 = fl.radius;
            vec2 a = fl.root;
            vec2 b = rw::lerp(fl.last_pos, fl.pos, alpha);
            vec2 c = b + rw::normalized(b - a) * r3 * 0.75f;
            a = apply_depth(a, fl.depth);
            b = apply_depth(b, fl.depth);
            c = apply_depth(c, fl.depth);
            // the stem: MakeLongMeshAtlased(3, false, true), 12 vertices in pairs
            vec2 sv[12];
            vec4 sc[12];
            vec2 const side = custom::perpendicular_vector(a, b) * 0.75f;
            for (int l = 0; l < 12; l += 2) {
                float const t = float(l) / 10.f;
                vec2 p = rw::lerp(a, b, t);
                if (l == 0)
                    p.y -= 5.f;
                vec3 const col = stalk + (petal - stalk) * t;
                sc[l] = sc[l + 1] = vec4{col.x, col.y, col.z, 1.f};
                sv[l] = p + side;
                sv[l + 1] = p - side;
            }
            sprites::long_mesh(out, strip, sv, sc, 12);
            // the petals around the flower's facing
            float const dz = std::sqrt(mathf::max(0.f, 1.f - rw::sqr_magnitude(fl.direction)));
            vec3 const v4{fl.direction.x, fl.direction.y, dz};
            // Vector3.Cross((0, 0, 1), v4).normalized
            vec2 v5{-v4.y, v4.x};
            v5 = rw::normalized(v5);
            // Vector3.Cross(v4, v5) as a Vector2
            vec2 const v6{-v4.z * v5.y, v4.z * v5.x};
            for (int m = 0; m < 4; ++m) {
                float const fa = float(m) * 3.1415927f / 2.f + float(i);
                vec2 const v7 = c + (v6 * std::sin(fa) + v5 * std::cos(fa)) * r3;
                sprites::sprite sp;
                sp.element = "KarmaPetal";
                sp.pos = b;
                sp.rotation = custom::aim(b, v7);
                sp.scale = vec2{0.375f, rw::magnitude(v7 - b) / 20.f};
                sp.anchor = vec2{0.5f, 0.f};
                sp.color = petal;
                sprites::add(out, sp);
            }
            // the ring (FSprite.width / height)
            sprites::sprite ring;
            ring.element = "FoodCircleA";
            ring.pos = c;
            ring.rotation = custom::vec_to_deg(v6);
            ring.scale = vec2{r3 * 2.f / mathf::max(ring_size.x, 1.f), r3 * rw::magnitude(v6) * 2.f / mathf::max(ring_size.y, 1.f)};
            ring.color = petal;
            sprites::add(out, ring);
        }
    }

    void bubble_cmds(sys::game_state const& s, dcon::room_id room, float alpha, vec3 color, std::vector<sprites::draw_cmd>& out) {
        for (auto const& p : s.particles) {
            if (p.room != room || particles::kind(p.kind) != particles::kind::bottom_bubble)
                continue;
            // Bubble.DrawSprites
            float const sc = mathf::lerp(p.saturation, p.hue, alpha) * p.size * 0.45f;
            if (sc <= 0.f)
                continue;
            sprites::sprite sp;
            sp.element = "LizardBubble5";
            sp.pos = rw::lerp(p.last_pos, p.pos, alpha);
            sp.scale = vec2{sc, sc};
            sp.color = color;
            sprites::add(out, sp);
        }
    }
}
