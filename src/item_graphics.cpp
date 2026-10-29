#include <cmath>
#include <string_view>

#include "item_graphics.hpp"
#include "items.hpp"
#include "outposts.hpp"
#include "physics.hpp"

namespace item_graphics {
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using sprites::sprite;
    using world::item_type;
    namespace custom = rw::custom;
    namespace mathf = rw::mathf;

    namespace {
        constexpr vec3 white{1.f, 1.f, 1.f};

        vec3 mix(vec3 a, vec3 b, float t) {
            t = mathf::clamp01(t);
            return a + (b - a) * t;
        }

        // the element names the ports pick by number
        constexpr std::string_view pebbles[15] = {"Pebble0", "Pebble1", "Pebble2", "Pebble3", "Pebble4", "Pebble5", "Pebble6", "Pebble7",
            "Pebble8", "Pebble9", "Pebble10", "Pebble11", "Pebble12", "Pebble13", "Pebble14"};
        constexpr std::string_view bio_spears[3] = {"BioSpear1", "BioSpear2", "BioSpear3"};
        constexpr std::string_view skulls[4][4] = {
            {"skull1_1", "skull1_2", "skull1_3", "skull1_4"},
            {"skull2_1", "skull2_2", "skull2_3", "skull2_4"},
            {"skull3_1", "skull3_2", "skull3_3", "skull3_4"},
            {"skull4_1", "skull4_2", "skull4_3", "skull4_4"},
        };

        // the interpolated body position, shaken while it vibrates
        // (DrawSprites' UnityEngine.Random: a per item, per tick roll here)
        vec2 draw_pos(sys::game_state const& state, dcon::item_id it, float alpha) {
            auto const& w = state.world;
            auto const c = items::chunk(state, it);
            vec2 p = rw::lerp(w.body_chunk_get_last_pos(c), w.body_chunk_get_pos(c), alpha);
            if (w.item_get_vibrate(it) > 0) {
                rw::rng r{(uint32_t(w.item_get_seed(it)) * 2654435761u) ^ (state.tick * 40503u) ^ 0x9E3779B9u};
                r.next();
                p += custom::deg_to_vec(r.value() * 360.f) * 2.f * r.value();
            }
            return p;
        }

        // Vector3.Slerp(lastRotation, rotation, timeStacker)
        vec2 draw_rot(sys::game_state const& state, dcon::item_id it, float alpha) {
            return rw::slerp(state.world.item_get_last_rotation(it), state.world.item_get_rotation(it), alpha);
        }

        void tri(std::vector<sprites::draw_cmd>& out, vec2 a, vec2 b, vec2 c, vec3 col) {
            sprites::draw_cmd d;
            d.is_triangle = true;
            d.tri[0] = a;
            d.tri[1] = b;
            d.tri[2] = c;
            d.color = col;
            out.push_back(d);
        }

        // Rock.DrawSprites: the pebble (picked by the ID's seed in
        // InitiateSprites) and the triangle trail while thrown, in blackColor
        void rock(sys::game_state const& state, dcon::item_id it, float alpha, palette const& pal, std::vector<sprites::draw_cmd>& out) {
            auto const& w = state.world;
            auto const c = items::chunk(state, it);
            vec2 const p = draw_pos(state, it, alpha);
            if (w.item_get_mode(it) == world::weapon_mode::thrown) {
                vec2 const tail = rw::lerp(w.item_get_tail_pos(it), w.body_chunk_get_last_pos(c), alpha);
                vec2 const a = custom::perpendicular_vector(rw::normalized(p - tail));
                tri(out, p + a * 3.f, p - a * 3.f, tail, pal.black);
            }
            rw::unity_random r{w.item_get_seed(it)};
            sprites::add(out, sprite{pebbles[r.range(1, 15)], p, custom::vec_to_deg(draw_rot(state, it, alpha)), vec2{1.f, 1.f}, vec2{0.5f, 0.5f}, pal.black});
        }

        // Spear.DrawSprites' placement of a spear sprite (the pivot moves to
        // the tip while it flies)
        sprite spear_sprite(sys::game_state const& state, dcon::item_id it, std::string_view element, vec2 p, vec2 rot, float alpha, vec3 col) {
            auto const& w = state.world;
            float const anchor_y = mathf::lerp(w.item_get_last_pivot_at_tip(it) ? 0.85f : 0.5f, w.item_get_pivot_at_tip(it) ? 0.85f : 0.5f, alpha);
            return sprite{element, p, custom::vec_to_deg(rot), vec2{1.f, 1.f}, vec2{0.5f, anchor_y}, col};
        }

        void spear(sys::game_state const& state, dcon::item_id it, float alpha, palette const& pal, std::vector<sprites::draw_cmd>& out) {
            auto const& w = state.world;
            vec2 const p = draw_pos(state, it, alpha);
            vec2 const rot = draw_rot(state, it, alpha);
            auto const type = w.item_get_type(it);
            if (uint8_t const needle = w.item_get_needle(it)) {
                // Spearmaster's needle: white while it is connected, fading to
                // black over 400 ticks once let go
                float k = needle == 1 ? 1.f : float(w.item_get_needle_fade(it)) / 400.f;
                if (k < 0.01f)
                    k = 0.01f;
                sprites::add(out, spear_sprite(state, it, bio_spears[w.item_get_needle_type(it) % 3], p, rot, alpha, mix(white, pal.black, 1.f - k)));
                return;
            }
            if (type == item_type::explosive_spear) {
                // ExplosiveSpear: the rag's mesh, the spear, the SpearRag knot
                vec3 const red = mix(vec3{1.f, 0.05f, 0.04f}, pal.black, 0.1f + 0.8f * pal.darkness);
                if (auto const ex = w.item_get_extra_from_item_extra(it)) {
                    auto const& x = w.weapon_extra_get_data(ex);
                    int32_t const n = x.rag_count;
                    vec2 verts[items::max_rag * 4];
                    // RagAttachPos(timeStacker)
                    auto const c = items::chunk(state, it);
                    vec2 a = rw::lerp(w.body_chunk_get_last_pos(c), w.body_chunk_get_pos(c), alpha) + rot * 15.f;
                    float num = 0.f;
                    for (int32_t i = 0; i < n; ++i) {
                        auto const& r = x.rag[i].v;
                        float const f = float(i) / float(n - 1);
                        vec2 const v = rw::lerp(r[1], r[0], alpha);
                        float const num2 = (2.f + 2.f * std::sin(mathf::pow(f, 2.f) * mathf::pi)) * rw::slerp(r[4], r[3], alpha).x;
                        vec2 const dir = rw::normalized(a - v);
                        vec2 const across = custom::perpendicular_vector(dir);
                        float const d = rw::distance(a, v) / 5.f;
                        verts[i * 4] = a - dir * d - across * (num2 + num) * 0.5f;
                        verts[i * 4 + 1] = a - dir * d + across * (num2 + num) * 0.5f;
                        verts[i * 4 + 2] = v + dir * d - across * num2;
                        verts[i * 4 + 3] = v + dir * d + across * num2;
                        a = v;
                        num = num2;
                    }
                    // the JaggedSquare shader's ragged edge isn't reproduced
                    sprites::long_mesh(out, verts, size_t(n) * 4, red);
                }
                sprites::add(out, spear_sprite(state, it, "SmallSpear", p, rot, alpha, pal.black));
                sprite knot = spear_sprite(state, it, "SpearRag", p, rot, alpha, red);
                if (w.item_get_mode(it) == world::weapon_mode::free && w.body_chunk_get_contact_point(items::chunk(state, it)).y < 0)
                    knot.anchor.y += 0.2f;
                sprites::add(out, knot);
                return;
            }
            sprites::add(out, spear_sprite(state, it, "SmallSpear", p, rot, alpha, pal.black));
            if (type != item_type::electric_spear)
                return;
            // ElectricSpear: four pebbles strung behind the head (InitiateSprites
            // rolls them from the running generator, here from the ID's seed),
            // the last one added first
            auto const ex = w.item_get_extra_from_item_extra(it);
            if (!ex)
                return;
            auto const& x = w.weapon_extra_get_data(ex);
            rw::unity_random r{w.item_get_seed(it) ^ 0x5EB0};
            std::string_view names[4];
            for (int32_t i = 0; i < 4; ++i)
                names[i] = i == 3 ? (r.value() < 0.5f ? pebbles[10] : pebbles[9]) : pebbles[r.range(1, 12)];
            auto const c = items::chunk(state, it);
            vec2 const base = rw::lerp(w.body_chunk_get_last_pos(c), w.body_chunk_get_pos(c), alpha);
            float const charged = w.item_get_charge(it) != 0;
            for (int32_t i = 3; i >= 0; --i) {
                // ZapperAttachPos
                vec2 const at = base + rot * 30.f + rot * float(i) * -4.f;
                float const s = mathf::lerp(0.4f, 1.f, float(i) / 4.f);
                vec3 const col = charged ? mix(w.item_get_color(it), white, std::fabs(std::sin(x.flux_timer[i]))) : pal.black;
                sprites::add(out, sprite{names[i], at, custom::vec_to_deg(rot), vec2{s, s}, vec2{0.5f, 0.5f}, col});
            }
        }

        // DataPearl.ApplyPalette's main and highlight colours
        void pearl_colors(sys::game_state const& state, dcon::item_id it, palette const& pal, vec3& color, bool& has_high, vec3& high) {
            auto const& w = state.world;
            auto const& pl = w.item_get_pearl(it);
            color = w.item_get_color(it);
            has_high = pl.has_highlight;
            high = pl.highlight;
            if (pal.saint && pl.type != items::pearl_type::cl && pl.type != items::pearl_type::lf_west) {
                color = mix(color, vec3{0.7f, 0.7f, 0.7f}, 0.76f);
                has_high = true;
                high = white;
            }
        }

        float pearl_glimmer(sys::game_state const& state, dcon::item_id it, float alpha) {
            auto const& pl = state.world.item_get_pearl(it);
            float g = mathf::lerp(pl.last_glimmer, pl.glimmer, alpha);
            if (pl.hidden)
                g *= 1.2f;
            return g;
        }

        // DataPearl.DrawSprites: the pearl and its highlight star
        void pearl(sys::game_state const& state, world::room_view const& room, dcon::item_id it, float alpha, palette const& pal, std::vector<sprites::draw_cmd>& out) {
            auto const& w = state.world;
            auto const c = items::chunk(state, it);
            vec2 p = rw::lerp(w.body_chunk_get_last_pos(c), w.body_chunk_get_pos(c), alpha);
            if (w.item_get_pearl(it).hidden)
                p.y -= 5.f;
            float const g = pearl_glimmer(state, it, alpha);
            vec3 color, high;
            bool has_high = false;
            pearl_colors(state, it, pal, color, has_high, high);
            vec3 c0 = mix(color * mathf::lerp(1.f, 0.2f, pal.darkness), white, g);
            vec3 c1;
            if (has_high) {
                vec3 const lit = mix(high, white, g);
                c1 = mix(high * mathf::lerp(1.f, 0.5f, pal.darkness), lit, g);
            } else {
                c1 = mix(color * mathf::lerp(1.3f, 0.5f, pal.darkness), white, mathf::lerp(0.5f + 0.5f * g, 0.2f + 0.8f * g, pal.darkness));
            }
            if (g > 0.9f && physics::submersion(state, room, c) >= 1.f)
                c0 = c1 = vec3{0.f, 0.003921569f, 0.f};
            sprites::add(out, sprite{"JetFishEyeA", p, 0.f, vec2{1.f, 1.f}, vec2{0.5f, 0.5f}, c0});
            sprites::add(out, sprite{"tinyStar", p + vec2{-0.5f, 1.5f}, 0.f, vec2{1.f, 1.f}, vec2{0.5f, 0.5f}, c1});
        }

        // the OutPostAntler shader's paint on the antlers' detail layer, per
        // vertex: _NoiseTex replaced by hashed value noise
        float paint_alpha(float u, float v, uint32_t seed) {
            float y = (v + 1.f) * 1.5f;
            y -= std::floor(y);
            float const fy = y * 16.f;
            int32_t const iy = int32_t(std::floor(fy));
            auto const hash = [&](int32_t k) {
                uint32_t h = uint32_t(k) * 374761393u + uint32_t(u > 0.5f ? 1 : 0) * 668265263u + seed * 2246822519u;
                h = (h ^ (h >> 13)) * 1274126177u;
                return float((h ^ (h >> 16)) & 0xFFFF) / 65535.f;
            };
            float h = mathf::lerp(hash(iy), hash(iy + 1), fy - float(iy));
            h = h < 0.5f ? h * h : std::sqrt(h);
            h -= std::sin(y * 7.f * 3.14f) * 0.7f;
            h = mathf::lerp(h, v * 2.f, mathf::min(std::fabs(0.5f - v) * 3.f, 1.f));
            if (h < 0.8f)
                return 0.f;
            return h < 0.9f ? 0.5f : 1.f;
        }
    }

    bool build(sys::game_state const& state, world::room_view const& room, dcon::item_id it, float alpha, palette const& pal,
        std::vector<sprites::draw_cmd>& out) {
        switch (state.world.item_get_type(it)) {
        case item_type::rock:
            rock(state, it, alpha, pal, out);
            return true;
        case item_type::spear:
        case item_type::explosive_spear:
        case item_type::electric_spear:
            spear(state, it, alpha, pal, out);
            return true;
        case item_type::data_pearl:
            pearl(state, room, it, alpha, pal, out);
            return true;
        default:
            return false;
        }
    }

    bool pearl_light(sys::game_state const& state, dcon::item_id it, float alpha, palette const& pal, light& out) {
        auto const& w = state.world;
        if (w.item_get_type(it) != item_type::data_pearl)
            return false;
        float const g = pearl_glimmer(state, it, alpha);
        if (g <= 0.f)
            return false;
        vec3 color, high;
        bool has_high = false;
        pearl_colors(state, it, pal, color, has_high, high);
        auto const c = items::chunk(state, it);
        out.pos = rw::lerp(w.body_chunk_get_last_pos(c), w.body_chunk_get_pos(c), alpha);
        if (w.item_get_pearl(it).hidden)
            out.pos.y -= 5.f;
        // Futile_White is 16 pixels across: scale 20 * glimmer (x 1.35 but for
        // the misc pearls) / 16
        auto const type = w.item_get_pearl(it).type;
        bool const misc = type == items::pearl_type::misc || type == items::pearl_type::misc2;
        out.rad = 10.f * g * (misc ? 1.f : 1.35f);
        out.color = has_high ? mix(high, white, g) : white;
        out.alpha = g * 0.5f;
        return true;
    }

    void outpost(sys::game_state const& state, dcon::outpost_id o, palette const& pal, std::vector<sprites::draw_cmd>& out) {
        auto const& w = state.world;
        vec2 const pos = w.outpost_get_pos(o);
        vec2 const handle = w.outpost_get_handle(o);
        vec2 const antler_pos = w.outpost_get_antler_pos(o);
        float const flip = w.outpost_get_antler_flip(o);
        auto const points = w.outpost_get_antlers(o);
        int32_t const parts = w.outpost_get_antler_parts(o);
        // ApplyPalette
        vec3 const black = pal.black;
        vec3 const bone = mix(black, vec3{0.9f, 0.9f, 0.8f}, mathf::lerp(0.9f, 0.2f, pal.darkness));
        vec3 const red = mix(black, mix(bone, vec3{1.f, 0.f, 0.f}, 0.9f), mathf::lerp(0.5f, 0.15f, pal.darkness));

        // DeerGraphics.Antlers.DrawSprites for one layer (num) of meshes,
        // plain or painted
        float const rotation = custom::aim(pos, antler_pos);
        static std::vector<vec2> verts;
        auto const layer = [&](int32_t num, bool paint) {
            for (int32_t i = 0; i < 2; ++i) {
                if ((flip < 0.f ? i : 1 - i) != num)
                    continue;
                float const side = i == 0 ? -1.f : 1.f;
                for (int32_t j = 0; j < parts; ++j) {
                    verts.clear();
                    vec2 v0{0.f, 0.f};
                    bool first = true;
                    float num3 = 0.2f;
                    for (uint32_t k = 0; k < points.size(); ++k) {
                        auto const& ap = points.at(k);
                        if (ap.part != j)
                            continue;
                        vec2 const v2 = outposts::transform_to_head_rotat(outposts::transformed_pos(ap, side), antler_pos, rotation, side, flip);
                        if (first) {
                            v0 = v2;
                            first = false;
                        }
                        vec2 const n = rw::normalized(v2 - v0);
                        vec2 const a = custom::perpendicular_vector(n);
                        float const d = rw::distance(v2, v0) / 5.f;
                        verts.push_back(v0 - a * (num3 + ap.rad) * 0.5f * outposts::antler_thickness + n * d);
                        verts.push_back(v0 + a * (num3 + ap.rad) * 0.5f * outposts::antler_thickness + n * d);
                        verts.push_back(v2 - a * ap.rad * outposts::antler_thickness - n * d);
                        verts.push_back(v2 + a * ap.rad * outposts::antler_thickness - n * d);
                        num3 = ap.rad;
                        v0 = v2;
                    }
                    if (!paint) {
                        sprites::long_mesh(out, verts.data(), verts.size(), bone);
                        continue;
                    }
                    // TriangleMesh.MakeLongMesh's UVs: x across, y along
                    size_t const segs = verts.size() / 4;
                    if (segs < 1)
                        continue;
                    float const step = 1.f / float((segs - 1) * 2 + 1);
                    for (size_t k = 0; k + 2 < verts.size(); ++k) {
                        sprites::draw_cmd c;
                        c.is_triangle = true;
                        c.vertex_colors = true;
                        float sum = 0.f;
                        for (size_t q = 0; q < 3; ++q) {
                            size_t const vi = k + q;
                            float const al = paint_alpha(float(vi % 2), float(vi / 2) * step, uint32_t(j * 2 + i) + uint32_t(o.index()) * 977u);
                            c.tri[q] = verts[vi];
                            c.vcol[q] = vec4{red.x, red.y, red.z, al};
                            sum += al;
                        }
                        if (sum > 0.f)
                            out.push_back(c);
                    }
                }
            }
        };
        layer(0, false);
        layer(0, true);
        // the pole: Futile_White half a tile wide, from the base through the
        // handle and as far back (the JaggedSquare shader's ragged edge isn't
        // reproduced)
        {
            vec2 const along = rw::normalized(handle) * rw::magnitude(handle);
            vec2 const side = custom::perpendicular_vector(handle) * 4.f;
            tri(out, pos - along - side, pos - along + side, pos + along + side, black);
            tri(out, pos - along - side, pos + along + side, pos + along - side, black);
        }
        layer(1, false);
        layer(1, true);
        // the skulls on the pole's top
        int32_t const num = custom::int_clamp(int32_t(std::nearbyint(std::fabs(flip) * 4.f)) + 1, 1, 4);
        bool const paint = w.outpost_get_paint_job(o) != 0;
        vec3 const skull_colors[3] = {mix(mix(bone, vec3{0.6f, 0.5f, 0.1f}, 0.3f), black, mathf::lerp(0.6f, 1.f, pal.darkness)), bone, red};
        for (int32_t i = 0; i < 3; ++i) {
            int32_t const k = ((paint && i == 2) ? 2 : 1) + i;
            sprites::add(out, sprite{skulls[num - 1][k - 1], pos + handle,
                custom::vec_to_deg(rw::normalized(handle)) + float(num) * 2.5f * mathf::sign(flip), vec2{-mathf::sign(flip), 1.f}, vec2{0.5f, 0.85f},
                skull_colors[i]});
        }
    }

    void pearl_strings(sys::game_state const& state, dcon::room_id room, float alpha, palette const& pal, std::vector<sprites::draw_cmd>& out) {
        auto const& w = state.world;
        w.room_for_each_outpost_location(room, [&](dcon::outpost_location_id ol) {
            auto const o = w.outpost_location_get_outpost(ol);
            w.outpost_for_each_string_outpost(o, [&](dcon::string_outpost_id so) {
                auto const s = w.string_outpost_get_string(so);
                auto const pearls = w.pearl_string_get_pearls(s);
                vec2 v = outposts::attached_pos(state, s);
                bool visible = true;
                for (uint32_t i = 0; i < pearls.size(); ++i) {
                    auto const it = outposts::pearl_item(state, pearls.at(i));
                    if (!pearls.at(i).active || !it || w.physical_object_get_room_from_object_location(items::body(state, it)) != room) {
                        visible = false;
                        continue;
                    }
                    auto const c = items::chunk(state, it);
                    vec2 const p = rw::lerp(w.body_chunk_get_last_pos(c), w.body_chunk_get_pos(c), alpha);
                    if (visible)
                        sprites::add(out, sprite{"pixel", p, custom::aim(p, v), vec2{1.f, rw::distance(p, v)}, vec2{0.5f, 0.f}, pal.black});
                    visible = true;
                    v = p;
                }
            });
        });
    }
}
